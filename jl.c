#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
	T_BEGINOBJECT,
	T_ENDOBJECT,
	T_PAIRSEP,
	T_MEMBERSEP,
	T_BEGINARRAY,
	T_ENDARRAY,
	T_STRING,
	T_NUMBER,
	T_BOOL,
	T_NULL,
	T_EOF,
} TokenType;

typedef struct {
	TokenType type;
	char *text;
} Token;

typedef struct {
	size_t len, cap;
	char *str;
} Buf;

typedef struct {
	size_t nrows, ncols;
	size_t rowcap;
	Buf **rows;
	Buf *newrow;
} Table;

typedef struct {
	enum { OP_OBJECT, OP_ARRAY, OP_COLLECT } type;
	Table *table;
} Op;

typedef struct {
	Op op;
	Op *next;
	bool isroot;
} ArrayOp;

typedef struct Prop Prop;
struct Prop {
	char *name;
	Op *op;
	Prop *next;
};

typedef struct {
	Op op;
	Prop *prop;
	bool isroot;
} ObjectOp;

typedef struct {
	Op op;
	size_t column;
} CollectOp;

typedef struct {
	char *pos;
} Parser;

static ArrayOp *new_array_op(void);
static ObjectOp *new_object_op(void);
static CollectOp *new_collect_op(Table *t);

static Op *parse_pattern(char *pat);
static ArrayOp *parse_array(Parser *p);
static ObjectOp *parse_object(Parser *p);
static Prop *parse_property(Parser *p, ObjectOp *obj);
static Prop *add_property(ObjectOp *op, char *name);

static Table *new_table(void);
static void add_value(Table *t, size_t column, char *val);
static void add_row(Table *t);

static bool find_root(Op *head);

static void flush_tables(void);
static void emit_row(size_t *rowindex);

static Token *next_token(void);
static Token *peek_token(void);

static void read_token(void);
static void read_literal(Token *t, char *v, size_t offset);
static void after_quote(void);
static void after_slash(void);
static void after_minus(void);
static void after_0(void);
static void after_1to9(void);
static void after_frac(void);
static void after_exp(void);
static int append_digits(void);
static int read_char(void);
static void unread_char(int c);

static void append_char(Buf *b, char c);
static void ensure_bufcap(Buf *b, size_t c);

static void run_op(Op *op);
static void run_array_op(ArrayOp *op);
static void run_object_op(ObjectOp *op);
static void run_collect_op(CollectOp *op);

static void accept(TokenType type);
static void skip_value(void);
static void skip_array(void);
static void skip_object(void);
static bool is_literal(TokenType type);

static void die(const char *fmt, ...);
static void *xcalloc(size_t nmemb, size_t size);
static void *xrealloc(void *ptr, size_t size);

struct {
	FILE *file;

	struct {
		char data[BUFSIZ];
		size_t i, len;
	} buf;
	int unread;

	Buf text;
	Token token, *peek;
} lexer;

static struct {
	Table **t;
	size_t len, cap;
} tables;

const char usage[] = "usage: jl [-f FIELDSEP] PATTERN [FILE...]\n";
const char *fieldsep = "\t";

int main(int argc, char *argv[])
{
	if (argc < 2)
		die(usage);

	int argi = 1;

	if (!strcmp(argv[argi], "-f")) {
		if (argi + 2 >= argc)
			die(usage);

		fieldsep = argv[argi + 1];
		argi += 2;
	}

	Op *head = parse_pattern(argv[argi++]);

	if (head == NULL)
		die("invalid pattern\n");

	if (!find_root(head))
		abort();

	Token *t;

	if (argi == argc) {
		lexer.file = stdin;
		do {
			run_op(head);
			t = peek_token();
		} while (t->type != T_EOF);
	}
	else {
		for (; argi < argc; argi++) {
			lexer.file = fopen(argv[argi], "r");
			do {
				run_op(head);
				t = peek_token();
			} while (t->type != T_EOF);
			fclose(lexer.file);
		}
	}
}

Op *parse_pattern(char *pat)
{
	Parser p = { .pos = pat };
	Op *op = NULL;

	if (*p.pos == '[')
		op = (Op*)parse_array(&p);
	else if (*p.pos == '{')
		op = (Op*)parse_object(&p);

	// initialize tables
	if (op) {
		for (size_t i = 0; i < tables.len; i++) {
			Table *t = tables.t[i];
			t->newrow = xcalloc(t->ncols, sizeof(*t->newrow));
		}
	}

	return op;
}

ArrayOp *parse_array(Parser *p)
{
	if (*p->pos++ != '[')
		return NULL;

	ArrayOp *arr = new_array_op();

	if (*p->pos == '*') {
		Table *t = new_table();
		arr->op.table = t;
		arr->next = (Op*)new_collect_op(t);
		p->pos++;
	}
	else if (*p->pos == '[') {
		arr->next = (Op*)parse_array(p);
	}
	else if (*p->pos == '{') {
		arr->next = (Op*)parse_object(p);
	}

	if (!arr->next)
		return NULL;

	switch (*p->pos) {
	case '\0':
		break;
	case ']':
		p->pos++;
		break;
	default:
		return NULL;
	}

	return arr;
}

ObjectOp *parse_object(Parser *p)
{
	if (*p->pos != '{')
		return NULL;

	ObjectOp *obj = new_object_op();

	char c;
	do {
		p->pos++;

		Prop *prop = parse_property(p, obj);
		char *end = p->pos;

		// read the inner property
		switch (*p->pos) {
		case ',':
		case '}':
		case '\0':
			if (!prop)
				return NULL;

			if (!obj->op.table)
				obj->op.table = new_table();

			prop->op = (Op*)new_collect_op(obj->op.table);
			break;
		case '{':
			prop->op = (Op*)parse_object(p);
			break;
		case '[':
			prop->op = (Op*)parse_array(p);
			break;
		default:
			return NULL;
		}

		if (!prop->op)
			return NULL;

		// 0-terminate the property name
		c = *p->pos;
		*end = '\0';
	} while (c == ',');

	if (c == '}')
		p->pos++;
	else if (c != '\0')
		return NULL;

	// reject objects without properties
	if (!obj->prop)
		return NULL;

	return obj;
}

Prop *parse_property(Parser *p, ObjectOp *obj)
{
	char *start = p->pos;

	if (*start == '"') {
		// find the end of the string
		start++;
		p->pos++;

		bool esc = false;
		while (*p->pos != '\0') {
			if (*p->pos == '"') {
				if (!esc) {
					*p->pos = '\0';
					p->pos++;
					break;
				}
				esc = false;
			}
			else if (*p->pos == '\\') {
				esc = !esc;
			}
			else {
				esc = false;
			}

			p->pos++;
		}
	}
	else {
		// find the end of the property name
		p->pos += strcspn(p->pos, ",[]{}");

		if (p->pos == start)
			return NULL;
	}

	return add_property(obj, start);
}

bool find_root(Op *head)
{
	ArrayOp *aop;
	ObjectOp *oop;

	Op *op = head;
	while (op) {
		switch (op->type) {
		case OP_ARRAY:
			aop = (ArrayOp*)op;
			if (aop->next->type == OP_COLLECT) {
				aop->isroot = true;
				return true;
			}
			op = aop->next;
			break;
		case OP_OBJECT:
			oop = (ObjectOp*)op;
			Prop *p = oop->prop;
			if (p->next || p->op->type == OP_COLLECT) {
				oop->isroot = true;
				return true;
			}
			op = p->op;
			break;
		case OP_COLLECT:
			return false;
		default:
			abort();
		}
	}
	return false;
}

ArrayOp *new_array_op()
{
	ArrayOp *op = xcalloc(1, sizeof(*op));
	op->op.type = OP_ARRAY;
	return op;
}

ObjectOp *new_object_op()
{
	ObjectOp *op = xcalloc(1, sizeof(*op));
	op->op.type = OP_OBJECT;
	return op;
}

CollectOp *new_collect_op(Table *t)
{
	CollectOp *op = xcalloc(1, sizeof(*op));
	op->op.type = OP_COLLECT;
	op->op.table = t;
	op->column = t->ncols++;
	return op;
}

Prop *add_property(ObjectOp *op, char *name)
{
	Prop *p = xcalloc(1, sizeof(*p));
	p->name = name;
	p->next = op->prop;
	op->prop = p;
	return p;
}

Table *new_table()
{
	if (tables.cap == tables.len) {
		tables.cap = tables.cap == 0 ? 4 : tables.cap * 2;
		tables.t = xrealloc(tables.t, tables.cap * sizeof(*tables.t));
	}

	Table *t = xcalloc(1, sizeof(*t));
	tables.t[tables.len++] = t;
	return t;
}

void add_value(Table *t, size_t column, char *val)
{
	Buf *b = &t->newrow[column];

	// reset the buffer
	if (b->len > 0) {
		b->len = 0;
		b->str[0] = '\0';
	}

	size_t len = strlen(val);
	if (len > 0) {
		ensure_bufcap(b, len + 1);
		memcpy(b->str, val, len);
		b->len = len;
		b->str[b->len] = '\0';
	}
}

void add_row(Table *t)
{
	// check if the new row contains values
	bool hasval = false;
	for (size_t i = 0; i < t->ncols; i++) {
		if (t->newrow[i].len > 0) {
			hasval = true;
			break;
		}
	}

	if  (hasval) {
		if (t->nrows == t->rowcap) {
			t->rowcap = t->rowcap == 0 ? 4 : t->rowcap * 2;
			t->rows = xrealloc(t->rows, t->rowcap * sizeof(*t->rows));
		}
		t->rows[t->nrows++] = t->newrow;
		t->newrow = xcalloc(t->ncols, sizeof(*t->newrow));
	}
}

void flush_tables()
{
	// determine the number of rows
	bool hasrows = false;
	size_t nrows = 1;

	for (size_t i = 0; i < tables.len; i++) {
		size_t n = tables.t[i]->nrows;
		if (n > 0) {
			hasrows = true;
			nrows *= n;
		}
	}

	if (!hasrows)
		return;

	// emit rows
	size_t rowindex[tables.len];
	memset(rowindex, 0, sizeof(rowindex));

	for (size_t i = 0; i < nrows; i++) {
		for (size_t j = 0; j < tables.len; j++) {
			Table *tab = tables.t[j];
			if (tab->nrows > 0)
				rowindex[j] = i % tab->nrows;
		}

		emit_row(rowindex);
	}

	// reset tables
	for (size_t i = 0; i < tables.len; i++)
		tables.t[i]->nrows = 0;
}

void emit_row(size_t *rowindex)
{
	for (size_t i = 0; i < tables.len; i++) {
		Table *t = tables.t[i];

		Buf *row = NULL;
		if (t->nrows > 0)
			row = t->rows[rowindex[i]];

		for (size_t j = 0; j < t->ncols; j++) {
			if (i > 0 || j > 0)
				fputs(fieldsep, stdout);

			if (row && row[j].str)
				fputs(row[j].str, stdout);
		}
	}

	putc('\n', stdout);
}

Token *next_token()
{
	if (lexer.peek) {
		Token *t = lexer.peek;
		lexer.peek = NULL;
		return t;
	}

	lexer.token.text = NULL;

	if (lexer.text.len > 0) {
		lexer.text.str[0] = '\0';
		lexer.text.len = 0;
	}

	read_token();

	if (!lexer.token.text)
		lexer.token.text = lexer.text.str ? lexer.text.str : "";

	return &lexer.token;
}

Token *peek_token()
{
	if (!lexer.peek)
		lexer.peek = next_token();

	return lexer.peek;
}

void read_token()
{
	// Skip whitespace
	static char ws[] = { ' ', '\t', '\n', '\r' };
	int c;
	do {
		c = read_char();
	} while (memchr(ws, c, sizeof(ws)));

	Token *t = &lexer.token;
	Buf *b = &lexer.text;

	switch (c) {
	case '\0':
		t->type = T_EOF;
		break;
	case '{':
		t->type = T_BEGINOBJECT;
		t->text = "{";
		break;
	case '}':
		t->type = T_ENDOBJECT;
		t->text = "}";
		break;
	case ':':
		t->type = T_PAIRSEP;
		t->text = ":";
		break;
	case ',':
		t->type = T_MEMBERSEP;
		t->text = ",";
		break;
	case '[':
		t->type = T_BEGINARRAY;
		t->text = "[";
		break;
	case ']':
		t->type = T_ENDARRAY;
		t->text = "]";
		break;
	case 't':
		t->type = T_BOOL;
		read_literal(t, "true", 1);
		break;
	case 'f':
		t->type = T_BOOL;
		read_literal(t, "false", 1);
		break;
	case 'n':
		t->type = T_NULL;
		read_literal(t, "null", 1);
		break;
	case '"':
		t->type = T_STRING;
		after_quote();
		break;
	case '-':
		t->type = T_NUMBER;
		append_char(b, c);
		after_minus();
		break;
	case '0':
		t->type = T_NUMBER;
		append_char(b, c);
		after_0();
		break;
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		t->type = T_NUMBER;
		append_char(b, c);
		after_1to9();
		break;
	default:
		die("unexpected character: %c\n", c);
	}
}

void read_literal(Token *t, char *v, size_t offset)
{
	for (char *p = v + offset; *p != '\0'; p++) {
		if (read_char() != *p)
			die("error matching literal: %s\n", v);
	}
	t->text = v;
}

void after_quote()
{
	Buf *b = &lexer.text;

	for (;;) {
		int c = read_char();

		if (c == '\0') {
			char *str = b->len > 0 ? b->str : "";
			die("non-terminated string: %s\n", str);
		}
		else if (c == '"') {
			break;
		}
		else if (c == '\\') {
			append_char(b, '\\');
			after_slash();
		}
		else if (c >= 0x00 && c <= 0x1f) {
			// the delete character 0x7f is allowed
			die("control character in string\n");
		}
		else {
			append_char(b, c);
		}
	}
}

void after_slash()
{
	int c = read_char();
	Buf *b = &lexer.text;

	static char valid[] = { '"', '\\', '/', 'b', 'f', 'n', 'r', 't' };

	if (memchr(valid, c, sizeof(valid))) {
		append_char(b, c);
	}
	else if (c == 'u') {
		append_char(b, 'u');

		for (int i = 0; i < 4; i++) {
			c = read_char();

			if (isxdigit(c))
				append_char(b, c);
			else
				die("not a hex character: %c\n", c);
		}
	}
	else {
		die("invalid escape character: %c\n", c);
	}
}

void after_minus()
{
	int c = read_char();

	switch (c) {
	case '0':
		append_char(&lexer.text, c);
		after_0();
		break;
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		append_char(&lexer.text, c);
		after_1to9();
		break;
	default:
		die("no digit following minus sign\n");
	}
}

void after_0()
{
	int c = read_char();

	if (c == '.') {
		append_char(&lexer.text, '.');
		after_frac();
	}
	else if (c == 'e' || c == 'E') {
		append_char(&lexer.text, c);
		after_exp();
	}
	else if (c != '\0') {
		unread_char(c);
	}
}

void after_1to9()
{
	for (;;) {
		int c = read_char();

		switch (c) {
		case '\0':
			return;
		case '.':
			append_char(&lexer.text, c);
			after_frac();
			return;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			append_char(&lexer.text, c);
			break;
		case 'e':
		case 'E':
			append_char(&lexer.text, c);
			after_exp();
			return;
		default:
			unread_char(c);
			return;
		}
	}
}

void after_frac()
{
	if (append_digits() < 1)
		die("no digits after fraction\n");

	int c = read_char();

	switch (c) {
	case '\0':
		break;
	case 'e':
	case 'E':
		append_char(&lexer.text, c);
		after_exp();
		break;
	default:
		unread_char(c);
	}
}

void after_exp()
{
	int c = read_char();

	switch (c) {
	case '\0':
		die("no exponent digits\n");
	case '+':
	case '-':
		append_char(&lexer.text, c);

		if (append_digits() == 0)
			die("no exponent digits\n");
		break;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
		append_char(&lexer.text, c);
		append_digits();
		break;
	default:
		die("no exponent digits\n");
	}
}

int append_digits()
{
	for (int n = 0; ; n++) {
		int c = read_char();

		switch (c) {
		case '\0':
			return n;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
			append_char(&lexer.text, c);
			break;
		default:
			unread_char(c);
			return n;
		}
	}
}

int read_char()
{
	if (lexer.unread) {
		int c = lexer.unread;
		lexer.unread = '\0';
		return c;
	}

	if (lexer.buf.i >= lexer.buf.len) {
		size_t size = sizeof(lexer.buf.data);
		lexer.buf.len = fread(lexer.buf.data, 1, size, lexer.file);

		if (lexer.buf.len < size) {
			if (ferror(lexer.file))
				die("read: %s\n", strerror(errno));

			if (lexer.buf.len == 0)
				return '\0';
		}

		lexer.buf.i = 0;
	}

	return lexer.buf.data[lexer.buf.i++];
}

void unread_char(int c)
{
	lexer.unread = c;
}

void append_char(Buf *b, char c)
{
	ensure_bufcap(b, b->len + 2);
	b->str[b->len++] = c;
	b->str[b->len] = '\0';
}

void ensure_bufcap(Buf *b, size_t cap)
{
	if (b->cap < cap) {
		if (b->cap == 0)
			b->cap = 4;
		while (b->cap < cap)
			b->cap *= 2;
		b->str = xrealloc(b->str, b->cap);
	}
}

void run_op(Op *op)
{
	switch (op->type) {
	case OP_ARRAY:
		run_array_op((ArrayOp*)op);
		break;
	case OP_OBJECT:
		run_object_op((ObjectOp*)op);
		break;
	case OP_COLLECT:
		run_collect_op((CollectOp*)op);
		break;
	default:
		abort();
	}
}

void run_array_op(ArrayOp *op)
{
	Token *t = peek_token();

	if (t->type == T_BEGINARRAY) {
		accept(T_BEGINARRAY);

		t = peek_token();

		if (t->type == T_ENDARRAY) {
			next_token();
		}
		else {
			do {
				run_op(op->next);

				if (op->op.table)
					add_row(op->op.table);

				t = next_token();
			} while (t->type == T_MEMBERSEP);

			if (t->type != T_ENDARRAY)
				die("expected array end\n");

			if (op->op.table)
				add_row(op->op.table);

			if (op->isroot)
				flush_tables();
		}
	}
	else {
		skip_value();
	}
}

void run_object_op(ObjectOp *op)
{
	Token *t = peek_token();

	if (t->type != T_BEGINOBJECT) {
		skip_value();
		return;
	}

	accept(T_BEGINOBJECT);

	t = next_token();

	while (t->type == T_STRING) {
		Prop *p = NULL;
		for (p = op->prop; p; p = p->next) {
			if (strcmp(p->name, t->text) == 0)
				break;
		}

		accept(T_PAIRSEP);

		if (p)
			run_op(p->op);
		else
			skip_value();

		t = next_token();

		if (t->type != T_MEMBERSEP)
			break;

		t = next_token();
	}

	if (t->type != T_ENDOBJECT)
		die("expected object end\n");

	if (op->op.table)
		add_row(op->op.table);

	if (op->isroot)
		flush_tables();
}

void run_collect_op(CollectOp *op)
{
	Token *t = peek_token();

	switch (t->type) {
	case T_BEGINARRAY:
		skip_array();
		break;
	case T_BEGINOBJECT:
		skip_object();
		break;
	default:
		if (!is_literal(t->type))
			die("unexpected token type\n");

		add_value(op->op.table, op->column, t->text);
		next_token();
		break;
	}
}

void accept(TokenType type)
{
	Token *t = next_token();
	if (t->type != type)
		die("unexpected token type\n");
}

void skip_value()
{
	Token *t = peek_token();

	switch (t->type) {
	case T_BEGINARRAY:
		skip_array();
		break;
	case T_BEGINOBJECT:
		skip_object();
		break;
	default:
		if (!is_literal(t->type))
			die("unexpected token type\n");
		next_token();
		break;
	}
}

void skip_array()
{
	accept(T_BEGINARRAY);

	Token *t = peek_token();

	if (t->type == T_ENDARRAY) {
		next_token();
	}
	else {
		do {
			skip_value();
			t = next_token();
		} while (t->type == T_MEMBERSEP);

		if (t->type != T_ENDARRAY)
			die("expected array end\n");
	}
}

void skip_object()
{
	accept(T_BEGINOBJECT);
	Token *t = peek_token();

	if (t->type == T_ENDOBJECT) {
		next_token();
	}
	else {
		do {
			accept(T_STRING);
			accept(T_PAIRSEP);
			skip_value();
			t = next_token();
		} while (t->type == T_MEMBERSEP);

		if (t->type != T_ENDOBJECT)
			die("expected object end\n");
	}
}

bool is_literal(TokenType type)
{
	switch (type) {
	case T_NULL:
	case T_BOOL:
	case T_NUMBER:
	case T_STRING:
		return true;
	default:
		return false;
	}
}

void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

void *xcalloc(size_t nmemb, size_t size)
{
	void *ptr = calloc(nmemb, size);
	if (!ptr)
		abort();
	return ptr;
}

void *xrealloc(void *ptr, size_t size)
{
	ptr = realloc(ptr, size);
	if (!ptr)
		abort();
	return ptr;
}
