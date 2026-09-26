// The `string` module (import 'string'). It is linked only when a program
// imports it. Signatures must agree with stringCallType in src/checker.nio and
// genStringCall in src/codegen.nio. string.length and string.append have no
// entry point here: codegen reads the Str header and uses rt_str_concat.
//
// Strings are immutable byte strings. Each function returns a new string, or
// the input when nothing changes. All functions work on bytes: case mapping is
// ASCII only, and indexes are byte offsets. A function that allocates roots its
// Str parameters first, because C parameters are not roots and an allocation
// can collect.
//
// toInt, toUint and toFloat are fallible. They use the `void **err`
// out-parameter convention (lib/fs.c), because text that is not a number is an
// expected input. The other functions panic on bad arguments, because those
// arguments come from the caller's own arithmetic.

#include "runtime.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static TypeDesc td_string_array = {TD_ARRAY, &rt_td_string, 0, NULL, NULL, 0};

// byte[]: the elements are scalars, so tracing keeps the element block alive.
static TypeDesc td_byte_array = {TD_ARRAY, &rt_td_uint8, 0, NULL, NULL, 0};

// Returns the byte offset of the first occurrence of sub at or after from, or
// -1. An empty sub matches at from. memchr on the first byte of sub finds the
// candidate positions, and every platform vectorizes memchr.
static int64_t find_from(Str *s, Str *sub, int64_t from) {
    if (sub->len == 0) return from <= s->len ? from : -1;
    if (from < 0) from = 0;

    // The last offset where a match can start. It is negative when sub is
    // longer than the remaining text, and then the loop does not run.
    int64_t last = s->len - sub->len;
    const char *base = s->data;

    while (from <= last) {
        // Only [from, last] can start a match, so memchr reads only that range.
        const char *hit = memchr(base + from, (unsigned char)sub->data[0],
                                 (size_t)(last - from + 1));
        if (hit == NULL) return -1;
        int64_t i = hit - base;
        if (sub->len == 1 ||
            memcmp(base + i + 1, sub->data + 1, (size_t)(sub->len - 1)) == 0) {
            return i;
        }
        from = i + 1;
    }
    return -1;
}

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

int64_t rt_string_find(Str *s, Str *sub) { return find_from(s, sub, 0); }

int64_t rt_string_contains(Str *s, Str *sub) { return find_from(s, sub, 0) >= 0; }

// ---- constant-time equality ----
//
// `==` compiles to rt_str_eq, which stops at the first different byte. Its
// time shows where two values differ, so an attacker with an oracle can recover
// a MAC or a token byte by byte. These functions take a time that depends only
// on the length. They xor each pair of bytes, OR the results together, and test
// the accumulator once at the end. The loop has no branch and no early exit, so
// the optimizer cannot replace it with memcmp.
//
// The length is not hidden. A caller whose length is secret must pad the
// values, and that is a protocol decision.
static int64_t ct_equal(const unsigned char *a, const unsigned char *b, int64_t n) {
    unsigned char diff = 0;
    for (int64_t i = 0; i < n; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    // (diff - 1) >> 8 is 1 when diff is 0 and 0 otherwise, with no branch:
    // 0 - 1 borrows into bit 8, and a value in 1..255 does not.
    return (int64_t)((((unsigned int)diff - 1u) >> 8) & 1u);
}

int64_t rt_string_equals_ct(Str *a, Str *b) {
    if (a->len != b->len) return 0;
    return ct_equal((const unsigned char *)a->data, (const unsigned char *)b->data, a->len);
}

// The byte[] form. A byte[] stores its elements as bytes (§5.7), so the
// comparison is the same as for a String.
int64_t rt_bytes_equals_ct(Arr *a, Arr *b) {
    if (a->len != b->len) return 0;
    return ct_equal(rt_arr_bytes(a), rt_arr_bytes(b), a->len);
}

Arr *rt_string_to_byte_array(Str *s) {
    // s must stay alive across the array allocation. The array is filled after
    // the last allocation, so it needs no root.
    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *a = rt_arr_new(s->len, 1);
    memcpy(rt_arr_bytes(a), s->data, (size_t)s->len);

    rt_gc_top = f.prev;
    return a;
}

// The inverse of toByteArray. Both directions are one memcpy, because a byte[]
// stores bytes (§5.7). Only the array needs a root.
Str *rt_string_from_byte_array(Arr *a) {
    TypeDesc *tds[1] = {&td_byte_array};
    int64_t slots[1] = {(int64_t)(intptr_t)a};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Str *s = rt_str_alloc(a->len);
    memcpy(s->data, rt_arr_bytes(a), (size_t)a->len);

    rt_gc_top = f.prev;
    return s;
}

// Copies s and maps each byte from lower to upper case (dir > 0) or from upper
// to lower case (dir < 0).
static Str *map_case(Str *s, int dir) {
    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Str *r = rt_str_alloc(s->len);
    for (int64_t i = 0; i < s->len; i++) {
        char c = s->data[i];
        if (dir > 0 && c >= 'a' && c <= 'z') c -= 'a' - 'A';
        if (dir < 0 && c >= 'A' && c <= 'Z') c += 'a' - 'A';
        r->data[i] = c;
    }

    rt_gc_top = f.prev;
    return r;
}

Str *rt_string_upper(Str *s) { return map_case(s, 1); }

Str *rt_string_lower(Str *s) { return map_case(s, -1); }

Str *rt_string_copy(Str *s) {
    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Str *r = rt_str_alloc(s->len);
    memcpy(r->data, s->data, (size_t)s->len);

    rt_gc_top = f.prev;
    return r;
}

Str *rt_string_trim(Str *s) {
    int64_t start = 0, end = s->len;
    while (start < end && is_space(s->data[start])) start++;
    while (end > start && is_space(s->data[end - 1])) end--;
    if (start == 0 && end == s->len) return s;

    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Str *r = rt_str_alloc(end - start);
    memcpy(r->data, s->data + start, (size_t)(end - start));

    rt_gc_top = f.prev;
    return r;
}

Arr *rt_string_split(Str *s, Str *sep) {
    if (sep->len == 0) rt_panic("split with an empty separator");

    // Each piece is allocated while the result array exists, so the array is
    // rooted with its real type. Unfilled slots are zero, and tracing skips
    // null pointers.
    TypeDesc *tds[3] = {&rt_td_string, &rt_td_string, &td_string_array};
    int64_t slots[3] = {(int64_t)(intptr_t)s, (int64_t)(intptr_t)sep, 0};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t n = 1;
    for (int64_t i = 0; (i = find_from(s, sep, i)) >= 0; i += sep->len) n++;

    Arr *a = rt_arr_new(n, 8);
    slots[2] = (int64_t)(intptr_t)a;
    int64_t start = 0;
    for (int64_t k = 0; k < n; k++) {
        int64_t end = find_from(s, sep, start);
        if (end < 0) end = s->len;
        Str *piece = rt_str_alloc(end - start);
        memcpy(piece->data, s->data + start, (size_t)(end - start));
        rt_arr_units(a)[k] = (int64_t)(intptr_t)piece;
        start = end + sep->len;
    }

    rt_gc_top = f.prev;
    return a;
}

// The inverse of split: joins the pieces into one string, with sep between
// each adjacent pair. The total length is computed first, so there is one
// allocation. (`s = s + piece` in a loop copies all earlier text on each
// iteration.) Nothing allocates after rt_str_alloc.
Str *rt_string_join(Arr *parts, Str *sep) {
    TypeDesc *tds[2] = {&td_string_array, &rt_td_string};
    int64_t slots[2] = {(int64_t)(intptr_t)parts, (int64_t)(intptr_t)sep};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t total = parts->len > 0 ? sep->len * (parts->len - 1) : 0;
    for (int64_t i = 0; i < parts->len; i++) {
        total += ((Str *)(intptr_t)rt_arr_units(parts)[i])->len;
    }

    Str *r = rt_str_alloc(total);
    char *out = r->data;
    for (int64_t i = 0; i < parts->len; i++) {
        if (i > 0) {
            memcpy(out, sep->data, (size_t)sep->len);
            out += sep->len;
        }
        Str *p = (Str *)(intptr_t)rt_arr_units(parts)[i];
        memcpy(out, p->data, (size_t)p->len);
        out += p->len;
    }

    rt_gc_top = f.prev;
    return r;
}

// Replaces the first occurrence of old with new, or all occurrences
// (non-overlapping, left to right) when all is set. When old is empty or does
// not occur, returns s.
static Str *replace(Str *s, Str *old, Str *new, int all) {
    if (old->len == 0 || find_from(s, old, 0) < 0) return s;

    TypeDesc *tds[3] = {&rt_td_string, &rt_td_string, &rt_td_string};
    int64_t slots[3] = {(int64_t)(intptr_t)s, (int64_t)(intptr_t)old,
                        (int64_t)(intptr_t)new};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t count = 0;
    for (int64_t i = 0; (i = find_from(s, old, i)) >= 0; i += old->len) {
        count++;
        if (!all) break;
    }

    Str *r = rt_str_alloc(s->len + count * (new->len - old->len));
    char *out = r->data;
    int64_t start = 0;
    for (int64_t k = 0; k < count; k++) {
        int64_t at = find_from(s, old, start);
        memcpy(out, s->data + start, (size_t)(at - start));
        out += at - start;
        memcpy(out, new->data, (size_t)new->len);
        out += new->len;
        start = at + old->len;
    }
    memcpy(out, s->data + start, (size_t)(s->len - start));

    rt_gc_top = f.prev;
    return r;
}

Str *rt_string_replace(Str *s, Str *old, Str *new) {
    return replace(s, old, new, 0);
}

Str *rt_string_replace_all(Str *s, Str *old, Str *new) {
    return replace(s, old, new, 1);
}

// Returns the bytes s->data[start..end). A bound out of range panics, as an
// array index out of range does, because the bounds come from the caller's
// arithmetic. The largest valid end is s->len.
Str *rt_string_substring(Str *s, int64_t start, int64_t end) {
    if (start < 0 || end < start || end > s->len) {
        rt_panic("substring out of range");
    }
    if (start == 0 && end == s->len) return s;

    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Str *r = rt_str_alloc(end - start);
    memcpy(r->data, s->data + start, (size_t)(end - start));

    rt_gc_top = f.prev;
    return r;
}

// Builds the Error that a failed parse raises:
// `string.<fn> "<text>": <reason>`. The text is bounded by its length,
// because a Nio string has no terminator. The message shows at most 200 bytes
// of it.
static void *parse_error(const char *fn, Str *s, const char *reason) {
    char msg[512];
    int64_t shown = s->len > 200 ? 200 : s->len;
    snprintf(msg, sizeof msg, "string.%s \"%.*s\": %s", fn, (int)shown, s->data, reason);
    // Text that cannot be parsed is the only failure here, so the code is
    // always INVALID.
    return rt_error_new(msg, NIO_ERR_INVALID);
}

// Parses [+-]?digits as an int, or stores an Error. The value accumulates as
// unsigned against the exact bound, so INT64_MIN parses. Nothing allocates
// before the parse fails, so s is never held across an allocation.
int64_t rt_string_to_int(Str *s, void **err) {
    int64_t i = 0;
    int neg = 0;
    if (i < s->len && (s->data[i] == '+' || s->data[i] == '-')) {
        neg = s->data[i] == '-';
        i++;
    }
    if (i == s->len) {
        *err = parse_error("toInt", s, "not an integer");
        return 0;
    }
    uint64_t v = 0;
    uint64_t limit = (uint64_t)INT64_MAX + (neg ? 1 : 0);
    for (; i < s->len; i++) {
        char c = s->data[i];
        if (c < '0' || c > '9') {
            *err = parse_error("toInt", s, "not an integer");
            return 0;
        }
        uint64_t d = (uint64_t)(c - '0');
        if (v > (limit - d) / 10) {
            *err = parse_error("toInt", s, "out of range");
            return 0;
        }
        v = v * 10 + d;
    }
    return neg ? (int64_t)(0 - v) : (int64_t)v;
}

// The unsigned counterpart of toInt: parses `+`? digits as a uint, or stores
// an Error. A leading `-` followed by digits is "out of range", as in the JSON
// reader. The result is the 8-byte unit as an int64, and its bits are the
// value.
int64_t rt_string_to_uint(Str *s, void **err) {
    int64_t i = 0;
    if (i < s->len && s->data[i] == '+') i++;
    if (i < s->len && s->data[i] == '-') {
        // The rest must be digits: "-x" is "not an integer", and only a
        // well-formed negative number is "out of range".
        for (int64_t j = i + 1; j < s->len; j++) {
            if (s->data[j] < '0' || s->data[j] > '9') {
                *err = parse_error("toUint", s, "not an integer");
                return 0;
            }
        }
        *err = parse_error("toUint", s, i + 1 == s->len ? "not an integer" : "out of range");
        return 0;
    }
    if (i == s->len) {
        *err = parse_error("toUint", s, "not an integer");
        return 0;
    }
    uint64_t v = 0;
    for (; i < s->len; i++) {
        char c = s->data[i];
        if (c < '0' || c > '9') {
            *err = parse_error("toUint", s, "not an integer");
            return 0;
        }
        uint64_t d = (uint64_t)(c - '0');
        if (v > (UINT64_MAX - d) / 10) {
            *err = parse_error("toUint", s, "out of range");
            return 0;
        }
        v = v * 10 + d;
    }
    return (int64_t)v;
}

// Parses [+-]? digits [. digits] with an optional [eE][+-]digits exponent.
// This function checks the shape, so strtod (which also accepts hex floats,
// "inf" and "nan") gets only text that this grammar accepts. A value that is
// too large is out of range. A value that is too small rounds to zero.
double rt_string_to_float(Str *s, void **err) {
    int64_t i = 0;
    if (i < s->len && (s->data[i] == '+' || s->data[i] == '-')) i++;
    int64_t digits = 0;
    while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9') {
        i++;
        digits++;
    }
    if (i < s->len && s->data[i] == '.') {
        i++;
        while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9') {
            i++;
            digits++;
        }
    }
    int64_t exp_digits = -1; // -1: no exponent part
    if (digits > 0 && i < s->len && (s->data[i] == 'e' || s->data[i] == 'E')) {
        i++;
        if (i < s->len && (s->data[i] == '+' || s->data[i] == '-')) i++;
        exp_digits = 0;
        while (i < s->len && s->data[i] >= '0' && s->data[i] <= '9') {
            i++;
            exp_digits++;
        }
    }
    if (digits == 0 || exp_digits == 0 || i != s->len) {
        *err = parse_error("toFloat", s, "not a number");
        return 0;
    }

    // strtod needs a NUL terminator, which a Str does not have. The copy holds
    // no other NUL, because the grammar above accepts only sign, digit, dot and
    // exponent bytes.
    char *c = malloc((size_t)s->len + 1);
    if (!c) rt_panic("out of memory");
    memcpy(c, s->data, (size_t)s->len);
    c[s->len] = 0;
    double v = strtod(c, NULL);
    free(c);
    if (!isfinite(v)) {
        *err = parse_error("toFloat", s, "out of range");
        return 0;
    }
    return v;
}

// ---- code points (§6.6) ----
//
// The decoder is rt_str_decode in runtime.c, because forEach over a String
// needs no import. The four functions below give what a walk does not.

// int[]: the elements are scalars, so tracing keeps the element block alive.
static TypeDesc td_int_array = {TD_ARRAY, &rt_td_int, 0, NULL, NULL, 0};

int64_t rt_string_rune_count(Str *s) {
    int64_t n = 0;
    int64_t off = 0;
    while (off < s->len) {
        int64_t w;
        rt_str_decode(s, off, &w);
        off += w;
        n++;
    }
    return n;
}

int64_t rt_string_rune_at(Str *s, int64_t off) {
    if (off < 0 || off >= s->len) rt_panic("runeAt offset out of range");
    int64_t w;
    return rt_str_decode(s, off, &w);
}

// A real U+FFFD is three bytes wide. A one-byte U+FFFD is the decoder's
// replacement for a byte that starts no valid sequence.
int64_t rt_string_valid_utf8(Str *s) {
    int64_t off = 0;
    while (off < s->len) {
        int64_t w;
        int64_t cp = rt_str_decode(s, off, &w);
        if (cp == 0xFFFD && w == 1) return 0;
        off += w;
    }
    return 1;
}

static int64_t code_point_or_replacement(int64_t cp) {
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
    return cp;
}

static int64_t utf8_width(int64_t cp) {
    if (cp < 0x80) return 1;
    if (cp < 0x800) return 2;
    if (cp < 0x10000) return 3;
    return 4;
}

// Writes each code point as UTF-8. A value that is not a code point is written
// as U+FFFD, so the result is always valid UTF-8. The array is read before and
// after the allocation, so it is rooted across it.
Str *rt_string_from_runes(Arr *a) {
    TypeDesc *tds[1] = {&td_int_array};
    int64_t slots[1] = {(int64_t)(intptr_t)a};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t total = 0;
    for (int64_t i = 0; i < a->len; i++) {
        total += utf8_width(code_point_or_replacement(rt_arr_get(a, i, &rt_td_int)));
    }
    Str *s = rt_str_alloc(total);
    unsigned char *d = (unsigned char *)s->data;
    for (int64_t i = 0; i < a->len; i++) {
        int64_t cp = code_point_or_replacement(rt_arr_get(a, i, &rt_td_int));
        if (cp < 0x80) {
            *d++ = (unsigned char)cp;
        } else if (cp < 0x800) {
            *d++ = (unsigned char)(0xC0 | (cp >> 6));
            *d++ = (unsigned char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            *d++ = (unsigned char)(0xE0 | (cp >> 12));
            *d++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            *d++ = (unsigned char)(0x80 | (cp & 0x3F));
        } else {
            *d++ = (unsigned char)(0xF0 | (cp >> 18));
            *d++ = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            *d++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            *d++ = (unsigned char)(0x80 | (cp & 0x3F));
        }
    }

    rt_gc_top = f.prev;
    return s;
}
