#ifndef ZJSON_H
#define ZJSON_H
/*
 * Zeitlos
 * Copyright (c) 2026 Lone Dynamics Corporation. All rights reserved.
 *
 * A strict JSON parser: zfed's subset (docs/fed.md, "Payloads"), no
 * malloc. docs/zjson.md.
 *
 * It accepts exactly:
 *   - one JSON value, UTF-8, no byte-order mark, whitespace around it;
 *   - strings of valid UTF-8, with the standard escapes; \u escapes
 *     must be whole characters (a surrogate pair, never half of one)
 *     and never \u0000; control characters must be escaped;
 *   - numbers that are integers within +-2^53: -?(0|[1-9][0-9]*),
 *     no fraction, no exponent, no -0;
 *   - true, false, null;
 *   - no two keys in one object equal once unescaped ("a" and "\u0061"
 *     are the same key);
 *   - nesting at most ZJSON_MAX_DEPTH deep.
 * Anything else is refused, never repaired: two parsers reading one
 * signed object differently is how signature checks get bypassed.
 *
 * It fills a caller's array of tokens, in document order. A container's
 * token says how many values it holds (an object's: key-value pairs);
 * zjson_next() skips over a whole value. Strings are left escaped in
 * the input; zjson_str() unescapes into a buffer.
 */
#include <stdint.h>
#include <stdbool.h>

#define ZJSON_MAX_DEPTH 16
#define ZJSON_MAX_INT   9007199254740992LL		// 2^53

typedef enum { ZJ_OBJECT = 1, ZJ_ARRAY, ZJ_STRING, ZJ_NUMBER, ZJ_TRUE, ZJ_FALSE, ZJ_NULL } zjson_type_t;

typedef struct {
	uint8_t  type;			// zjson_type_t
	uint32_t start, end;	// the bytes: for a string, inside the quotes
	uint32_t size;			// object: pairs; array: elements
	int64_t  num;			// ZJ_NUMBER
} zjson_tok_t;

typedef enum {
	ZJ_OK = 0,
	ZJ_E_EMPTY, ZJ_E_BOM, ZJ_E_TRAILING, ZJ_E_TOKENS, ZJ_E_DEPTH,
	ZJ_E_SYNTAX, ZJ_E_UTF8, ZJ_E_CONTROL, ZJ_E_ESCAPE, ZJ_E_SURROGATE, ZJ_E_NUL,
	ZJ_E_NUMBER, ZJ_E_RANGE, ZJ_E_DUPLICATE,
} zjson_err_t;

// Parses `len` bytes. Returns the number of tokens (> 0), or -(error);
// *where (if not NULL) is the byte offset of the problem.
int zjson_parse(const char *js, uint32_t len, zjson_tok_t *tok, int ntok, uint32_t *where);
const char *zjson_strerror(int err);

// The token after the whole value at tok[i] (its end in the array).
int zjson_next(const zjson_tok_t *tok, int i);

// In the object at tok[obj], the value of `key` (index), or -1.
int zjson_get(const char *js, const zjson_tok_t *tok, int obj, const char *key);

// A string token, unescaped into out[cap] with a terminating NUL.
// False if it is not a string or does not fit.
bool zjson_str(const char *js, const zjson_tok_t *t, char *out, uint32_t cap);

// A string token equal to `s` once unescaped.
bool zjson_eq(const char *js, const zjson_tok_t *t, const char *s);

// Is s[0..len) valid UTF-8 (shortest forms, no surrogates, at most
// U+10FFFF) with no NUL? The same check zjson_parse() applies inside
// strings; zfed uses it for `format: text` and `format: json` payloads.
bool zjson_utf8(const char *s, uint32_t len);

#endif
