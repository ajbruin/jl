# jl

jl is a filter that converts JSON to lines of text. These can easily be
processed using other command-line tools.


## Features

* Simple filter syntax
* Works with streams and large files
* No dependencies


## Examples

The filter is defined using a syntax that looks like the JSON it matches. The
example below extracts the "foo" and "bar" fields from JSON objects in an
array.

For each object, jl emits a line with the values of "foo" and "bar". By default
values are separated by a tab character.


```json
[
    { "foo": "yes", "bar": 1 },
    { "foo": "no", "bar": 0 }
]
```

```
$ jl '[{foo,bar' data.json
yes 1
no  0
```


Values are repeated for multiple matches:

```json
{
    "user": "henry",
    "pets": [
        { "type": "dog", "name": "fred" },
        { "type": "cat", "name": "igor" }
    ]
}
```

```
$ jl '{user,pets[{name' data.json
henry   fred
henry   igor
```

## Rationale

The de facto tool for handling JSON on the command-line is
[jq](https://stedolan.github.io/jq/), which is very powerful and flexible.
However, for occasional users its feature set can be daunting and the syntax
hard to remember. Where jq is complex like sed, jl tries to be simple like grep.
The goal of jl is to quickly get the data into the format that the standard Unix
tools are built to handle.


## Build

To build jl you need a C99 compiler.

```
$ make
$ make install
```
