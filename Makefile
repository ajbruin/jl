VERSION = 0.1

PREFIX = /usr/local
MANPREFIX = $(PREFIX)/man

CFLAGS = -std=c99 -Wall -Wextra -pedantic -Os

DISTFILES = jl.c jl.1 Makefile LICENSE.md README.md

jl:

clean:
	rm -f jl

install: jl
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f jl $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	cp -f jl.1 $(DESTDIR)$(MANPREFIX)/man1

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/jl
	rm -f $(DESTDIR)$(MANPREFIX)/man1/jl.1

dist:
	mkdir -p jl-$(VERSION)
	cp $(DISTFILES) jl-$(VERSION)
	tar -cf jl-$(VERSION).tar jl-$(VERSION)
	gzip jl-$(VERSION).tar
	rm -rf jl-$(VERSION)

.PHONY: clean install uninstall dist
