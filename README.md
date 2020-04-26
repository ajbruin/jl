# jl

jl is a filter that converts JSON to lines of text. These can easily be
processed using other command-line tools.

The filter is defined using a syntax that looks like the JSON it matches. The
example below extracts the "foo" and "bar" fields from JSON objects in an
array.

```
$ jl '[{foo,bar' data.json
```

For each object, jl emits a line with the values of "foo" and "bar". By default
values are separated by a tab character.
