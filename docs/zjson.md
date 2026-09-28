# zjson: a strict JSON parser

`sw/common/zjson.c`. JSON as zfed's payloads use it
([fed.md](fed.md#payloads)): one value, UTF-8, integers only, no
duplicate keys -- and anything outside that **refused, never
repaired**. Two parsers that read one signed object differently is how
signature checks get bypassed, so this one reads exactly one way.
Portable C99; no malloc.

```c
zjson_tok_t tok[64];
int n = zjson_parse(js, len, tok, 64, &where);       // tokens, or -error
int v = zjson_get(js, tok, 0, "name");               // a key's value
zjson_str(js, &tok[v], buf, sizeof(buf));            // unescaped
tok[i].num                                           // a number's value
zjson_next(tok, i)                                   // past a whole value
zjson_utf8(s, len)                                   // valid UTF-8, no NUL
```

It fills the caller's array of tokens in document order: a container's
token counts its values (an object's, its pairs), a string's covers the
bytes inside its quotes, still escaped. Each token is 32 bytes, so size
the array for the payloads you expect: 64 tokens is 2 KB.

## What it accepts

- one value, whitespace around it, UTF-8, no byte-order mark;
- strings of valid UTF-8 (shortest forms, no encoded surrogates, at
  most U+10FFFF); the standard escapes; `\u` escapes must be whole
  characters -- a surrogate pair, never half of one -- and never
  `\u0000` (a NUL cuts C strings short); control characters escaped;
- numbers `-?(0|[1-9][0-9]*)` within +-2^53: no fraction, no exponent,
  no -0;
- `true`, `false`, `null`;
- no two keys of an object equal **once unescaped** (`"a"` and
  `"\u0061"` collide); at most 256 keys in one object;
- nesting at most 16 deep.

## Testing

`make -C sw/common/tests -f Makefile.zjson` (AddressSanitizer and
UBSan): 5,047 inputs from `gen_zjson_cases.py` -- hand-written nasty
ones, 3,000 random documents and 2,000 random byte-level mutations --
each with the verdict of **Python's own `json` module** plus the subset's
rules. Every verdict agrees, and every one of the 1,672 accepted inputs
re-serializes from its tokens to exactly what `json.dumps` wrote: the
same values, not only the same yes or no. Allowing duplicate keys,
-0, `\u0000`, a lone low surrogate, deeper nesting, or overlong UTF-8
each fails it.
