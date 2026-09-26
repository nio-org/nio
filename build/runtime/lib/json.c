// The `json` module (import 'json'). It is linked only when a program imports
// it. Signatures must agree with jsonCallType in src/checker.nio and the json
// branch of src/codegen.nio.

#include "runtime.h"

#include <errno.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- string builder ----

// The scratch buffer comes from malloc and not rt_alloc, because it is never a
// language value.

typedef struct {
    char *buf;
    size_t len, cap;
} SB;

static void sb_init(SB *sb) {
    sb->cap = 64;
    sb->len = 0;
    sb->buf = malloc(sb->cap);
    if (!sb->buf) rt_panic("out of memory");
}

static void sb_write(SB *sb, const char *p, size_t n) {
    if (sb->len + n > sb->cap) {
        while (sb->cap < sb->len + n) sb->cap *= 2;
        char *nb = realloc(sb->buf, sb->cap);
        if (!nb) rt_panic("out of memory");
        sb->buf = nb;
    }
    memcpy(sb->buf + sb->len, p, n);
    sb->len += n;
}

static void sb_cstr(SB *sb, const char *c) { sb_write(sb, c, strlen(c)); }
static void sb_char(SB *sb, char c) { sb_write(sb, &c, 1); }

static void sb_i64(SB *sb, int64_t v) {
    char b[32];
    snprintf(b, sizeof b, "%lld", (long long)v);
    sb_cstr(sb, b);
}

static void sb_u64(SB *sb, uint64_t v) {
    char b[32];
    snprintf(b, sizeof b, "%llu", (unsigned long long)v);
    sb_cstr(sb, b);
}

// How a NUMBER node holds `num` (runtime.h).
#define JN_DOUBLE 0
#define JN_INT 1
#define JN_UINT 2

// The bound tested before a cast to uint64. It is written as a power, because
// 18446744073709551615 is not a double.
#define UINT64_LIMIT 18446744073709551616.0

static const char *uint_kind_name(int64_t kind) {
    switch (kind) {
    case TD_UINT8:
        return "uint8";
    case TD_UINT16:
        return "uint16";
    case TD_UINT32:
        return "uint32";
    default:
        return "uint64";
    }
}

// The buffer is static because jp_fail copies the message and does not return.
static const char *uint_range_message(int64_t kind) {
    static char msg[48];
    snprintf(msg, sizeof msg, "%s out of range", uint_kind_name(kind));
    return msg;
}

static uint64_t uint_kind_max(int64_t kind) {
    switch (kind) {
    case TD_UINT8:
        return 255u;
    case TD_UINT16:
        return 65535u;
    case TD_UINT32:
        return 4294967295u;
    default:
        return UINT64_MAX;
    }
}

// The shortest text that reads back as v at the precision of its own type.
// rt_fmt_double is core, so JSON and print write a float the same way.
static void sb_double(SB *sb, double v, int f32) {
    char b[64];
    rt_fmt_double(b, sizeof b, v, f32);
    sb_cstr(sb, b);
}

static void sb_json_string(SB *sb, const char *p, int64_t n) {
    sb_char(sb, '"');
    for (int64_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        switch (c) {
        case '"': sb_cstr(sb, "\\\""); break;
        case '\\': sb_cstr(sb, "\\\\"); break;
        case '\n': sb_cstr(sb, "\\n"); break;
        case '\t': sb_cstr(sb, "\\t"); break;
        case '\r': sb_cstr(sb, "\\r"); break;
        case '\b': sb_cstr(sb, "\\b"); break;
        case '\f': sb_cstr(sb, "\\f"); break;
        default:
            if (c < 0x20) {
                char e[8];
                snprintf(e, sizeof e, "\\u%04x", c);
                sb_cstr(sb, e);
            } else {
                sb_char(sb, (char)c);
            }
        }
    }
    sb_char(sb, '"');
}

// ---- serialization ----

// Whether the type has a JSON form. Only a function and a compiled pattern have
// none. This does not recurse into a record, so a recursive type terminates.
static int json_has_form(const TypeDesc *td) {
    switch (td->kind) {
    case TD_FUNC:
    case TD_REGEXP:
        return 0;
    case TD_ARRAY:
    case TD_OPTIONAL:
        return json_has_form(td->elem);
    case TD_MAP:
        // JSON object keys are strings, so only a String-keyed map has a form.
        return td->field_types[0]->kind == TD_STRING && json_has_form(td->field_types[1]);
    default:
        return 1;
    }
}

// Whether field i is the record's catch-all (§6.3). TypeDesc.rest holds
// 1 + the index, so 0 means "none".
static int json_field_is_rest(const TypeDesc *td, int64_t i) { return td->rest == i + 1; }

// Whether td declares a field under this JSON key. A declared key is kept out
// of the spliced catch-all. Otherwise the document holds it twice.
static int json_declares(const TypeDesc *td, const Str *key) {
    for (int64_t i = 0; i < td->nfields; i++) {
        if (json_field_is_rest(td, i)) continue;
        const char *n = td->field_names[i];
        size_t len = strlen(n);
        if ((int64_t)len == key->len && memcmp(n, key->data, len) == 0) return 1;
    }
    return 0;
}

static void json_node_value(SB *sb, JsonNode *n);

static void json_value(SB *sb, int64_t raw, TypeDesc *td) {
    switch (td->kind) {
    case TD_INT8:
    case TD_INT16:
    case TD_INT32:
    case TD_INT:
    case TD_DURATION: // a duration serializes as its count of milliseconds
    case TD_ENUM:     // an enum serializes as its member's numeric value
        sb_i64(sb, raw);
        break;
    case TD_UINT8:
    case TD_UINT16:
    case TD_UINT32:
    case TD_UINT:
        sb_u64(sb, (uint64_t)raw);
        break;
    case TD_FLOAT:
    case TD_FLOAT32: {
        double d;
        memcpy(&d, &raw, 8);
        if (isnan(d) || isinf(d)) rt_panic("cannot serialize a non-finite float to JSON");
        sb_double(sb, d, td->kind == TD_FLOAT32);
        break;
    }
    case TD_BOOL:
        sb_cstr(sb, raw ? "true" : "false");
        break;
    case TD_STRING: {
        Str *s = (Str *)(intptr_t)raw;
        sb_json_string(sb, s->data, s->len);
        break;
    }
    case TD_DATETIME: {
        Str *s = rt_date_to_text(raw);
        sb_char(sb, '"');
        sb_write(sb, s->data, (size_t)s->len);
        sb_char(sb, '"');
        break;
    }
    case TD_ARRAY: {
        Arr *a = (Arr *)(intptr_t)raw;
        sb_char(sb, '[');
        for (int64_t i = 0; i < a->len; i++) {
            if (i) sb_char(sb, ',');
            json_value(sb, rt_arr_get(a, i, td->elem), td->elem);
        }
        sb_char(sb, ']');
        break;
    }
    case TD_OPTIONAL: {
        void *p = (void *)(intptr_t)raw;
        if (!p) {
            sb_cstr(sb, "null");
            break;
        }
        json_value(sb, rt_box_get(p, td->elem), td->elem);
        break;
    }
    case TD_MAP: {
        // Entries in insertion order, keys as JSON strings.
        Map *m = (Map *)(intptr_t)raw;
        sb_char(sb, '{');
        for (int64_t i = 0; i < m->len; i++) {
            if (i) sb_char(sb, ',');
            Str *k = (Str *)(intptr_t)rt_map_key_at(m, i, td->field_types[0]);
            sb_json_string(sb, k->data, k->len);
            sb_char(sb, ':');
            json_value(sb, rt_map_val_at(m, i, td->field_types[1]), td->field_types[1]);
        }
        sb_char(sb, '}');
        break;
    }
    case TD_JSON:
        json_node_value(sb, (JsonNode *)(intptr_t)raw);
        break;
    case TD_UNION:
    case TD_RECORD: {
        // Serialize as the dynamic type of the value. A record carries its own
        // descriptor (REC_TD), so a value reached through a sealed base (§2.4)
        // keeps the fields that the base does not declare. The null guard is
        // for the zero value that a sealed-typed slot can hold.
        if (!raw) {
            sb_cstr(sb, "null");
            break;
        }
        if (REC_TD((char *)(intptr_t)raw)) td = REC_TD((char *)(intptr_t)raw);
        void *rec = (void *)(intptr_t)raw;
        // A union member serializes as its bare payload (§2.11).
        if (td->uni) {
            json_value(sb, rt_rec_get(rec, 0, td), td->field_types[0]);
            break;
        }
        sb_char(sb, '{');
        int first = 1;
        for (int64_t i = 0; i < td->nfields; i++) {
            if (!json_has_form(td->field_types[i])) continue;
            int64_t fraw = rt_rec_get(rec, i, td);
            // The entries of the catch-all field (§6.3) are spliced into this
            // object. They are not nested. A value in the field that is not an
            // object adds nothing.
            if (json_field_is_rest(td, i)) {
                JsonNode *rest = (JsonNode *)(intptr_t)fraw;
                if (rt_json_kind(rest) != JSON_OBJECT) continue;
                JsonVec *keys = rest->a;
                JsonVec *vals = rest->b;
                for (int64_t k = 0; keys && k < keys->len; k++) {
                    Str *key = (Str *)(intptr_t)keys->slots[k];
                    // A declared field has priority, because a key must not
                    // appear twice.
                    if (json_declares(td, key)) continue;
                    if (!first) sb_char(sb, ',');
                    first = 0;
                    sb_json_string(sb, key->data, key->len);
                    sb_char(sb, ':');
                    json_node_value(sb, (JsonNode *)(intptr_t)vals->slots[k]);
                }
                continue;
            }
            if (td->field_types[i]->kind == TD_OPTIONAL && fraw == 0) continue;
            if (!first) sb_char(sb, ',');
            first = 0;
            sb_json_string(sb, td->field_names[i], (int64_t)strlen(td->field_names[i]));
            sb_char(sb, ':');
            json_value(sb, fraw, td->field_types[i]);
        }
        sb_char(sb, '}');
        break;
    }
    }
}

Str *rt_json_totext(int64_t raw, TypeDesc *td) {
    // The value is an unrooted parameter, held across every allocation below.
    TypeDesc *tds[1] = {td};
    int64_t slots[1] = {raw};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    SB sb;
    sb_init(&sb);
    json_value(&sb, raw, td);
    Str *s = rt_str_alloc((int64_t)sb.len);
    memcpy(s->data, sb.buf, sb.len);
    free(sb.buf);

    rt_gc_top = f.prev;
    return s;
}

// ---- deserialization ----
//
// The type descriptor controls the scan, so there is no intermediate document.
// A key that the record does not declare is skipped with no allocation. When
// the text cannot supply a value, the result is an error and not a zero value.

typedef struct {
    const char *p;
    const char *end;
} JP;

// The key path an error names, built on the C stack.
typedef struct JPath {
    const struct JPath *prev;
    const char *name; // NULL for an array element
    int64_t nlen;     // name's byte length, or -1 for a NUL-terminated literal
    int64_t index;
} JPath;

// A parse failure is a value and does not stop the program. The text comes from
// outside the program, so the caller can catch it (§2.9). Reporting leaves the
// recursion through longjmp, which must not skip the GC frame stack. rt_gc_top
// is saved with the jump target and restored on arrival.
static jmp_buf jp_err_jmp;
static int jp_err_catching;
static char jp_err_msg[512];

static void jp_fail(const JPath *path, const char *what) {
    // A fixed buffer, because reporting an error must not allocate.
    char buf[256];
    size_t n = 0;
    const JPath *stack[32];
    int depth = 0;
    for (const JPath *q = path; q && depth < 32; q = q->prev) stack[depth++] = q;
    for (int i = depth - 1; i >= 0 && n + 32 < sizeof buf; i--) {
        if (stack[i]->name) {
            // A rooted Str is not NUL-terminated, so %s would read past it.
            int nl = stack[i]->nlen < 0 ? (int)strlen(stack[i]->name) : (int)stack[i]->nlen;
            n += (size_t)snprintf(buf + n, sizeof buf - n, "%s%.*s", n ? "." : "", nl,
                                  stack[i]->name);
        } else {
            n += (size_t)snprintf(buf + n, sizeof buf - n, "[%lld]", (long long)stack[i]->index);
        }
    }
    char msg[512];
    if (n > 0) {
        snprintf(msg, sizeof msg, "cannot parse JSON: %s at %s", what, buf);
    } else {
        snprintf(msg, sizeof msg, "cannot parse JSON: %s", what);
    }
    if (jp_err_catching) {
        memcpy(jp_err_msg, msg, sizeof msg);
        longjmp(jp_err_jmp, 1);
    }
    rt_panic(msg);
}

static void jp_ws(JP *j) {
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') j->p++;
        else break;
    }
}

static int jp_peek(JP *j) {
    jp_ws(j);
    return j->p < j->end ? (unsigned char)*j->p : -1;
}

static void jp_want(JP *j, char c, const JPath *path) {
    if (jp_peek(j) != c) {
        char what[32];
        snprintf(what, sizeof what, "expected '%c'", c);
        jp_fail(path, what);
    }
    j->p++;
}

static int jp_lit(JP *j, const char *word) {
    size_t n = strlen(word);
    jp_ws(j);
    if ((size_t)(j->end - j->p) < n || memcmp(j->p, word, n) != 0) return 0;
    j->p += n;
    return 1;
}

static void sb_utf8(SB *sb, unsigned cp) {
    if (cp < 0x80) {
        sb_char(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_char(sb, (char)(0xC0 | (cp >> 6)));
        sb_char(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_char(sb, (char)(0xE0 | (cp >> 12)));
        sb_char(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_char(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_char(sb, (char)(0xF0 | (cp >> 18)));
        sb_char(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_char(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_char(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

static int jp_hex4(JP *j, unsigned *out) {
    if (j->end - j->p < 4) return 0;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = j->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    j->p += 4;
    *out = v;
    return 1;
}

// One scratch buffer for string bodies with escapes, shared by the whole file.
// The runtime is single-threaded and a parse never nests. The buffer holds one
// decoded string at a time, so a consumer must copy it before the next string
// is read.
static SB jp_scratch;

static SB *jp_scratch_reset(void) {
    if (!jp_scratch.buf) sb_init(&jp_scratch);
    jp_scratch.len = 0;
    return &jp_scratch;
}

// Scans a string body, from after the opening quote to past the closing one.
// With sb NULL the body is only validated.
static void jp_string_tail(JP *j, SB *sb, const JPath *path) {
    for (;;) {
        if (j->p >= j->end) jp_fail(path, "unterminated string");
        const char *run = j->p;
        while (j->p < j->end && *j->p != '"' && *j->p != '\\') j->p++;
        if (sb && j->p > run) sb_write(sb, run, (size_t)(j->p - run));
        if (j->p >= j->end) jp_fail(path, "unterminated string");
        char c = *j->p++;
        if (c == '"') return;
        if (j->p >= j->end) jp_fail(path, "unterminated string escape");
        char e = *j->p++;
        switch (e) {
        case '"': if (sb) sb_char(sb, '"'); break;
        case '\\': if (sb) sb_char(sb, '\\'); break;
        case '/': if (sb) sb_char(sb, '/'); break;
        case 'b': if (sb) sb_char(sb, '\b'); break;
        case 'f': if (sb) sb_char(sb, '\f'); break;
        case 'n': if (sb) sb_char(sb, '\n'); break;
        case 'r': if (sb) sb_char(sb, '\r'); break;
        case 't': if (sb) sb_char(sb, '\t'); break;
        case 'u': {
            unsigned cp;
            if (!jp_hex4(j, &cp)) jp_fail(path, "bad \\u escape");
            // A high surrogate needs its low half. Anything else becomes the
            // replacement character, so the output stays valid UTF-8.
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                unsigned lo;
                if (j->end - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                    const char *save = j->p;
                    j->p += 2;
                    if (jp_hex4(j, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        j->p = save;
                        cp = 0xFFFD;
                    }
                } else {
                    cp = 0xFFFD;
                }
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                cp = 0xFFFD;
            }
            if (sb) sb_utf8(sb, cp);
            break;
        }
        default:
            jp_fail(path, "unknown string escape");
        }
    }
}

// The descriptor for a byte[]. The _b entry points root their input with it,
// and jp_record roots a wide record's bookkeeping with it.
static TypeDesc td_byte_array = {TD_ARRAY, &rt_td_uint8, 0, NULL, NULL, 0};

// Reads a JSON string and returns where its bytes are, with no allocation.
// The span is valid only until the next string is read, so a caller that holds
// a key across a nested parse must copy it.
static void jp_string_span(JP *j, const char **out, int64_t *len, const JPath *path) {
    jp_want(j, '"', path);
    const char *start = j->p;
    const char *q = memchr(j->p, '"', (size_t)(j->end - j->p));
    if (!q) jp_fail(path, "unterminated string");
    const char *bs = memchr(start, '\\', (size_t)(q - start));
    if (!bs) {
        *out = start;
        *len = q - start;
        j->p = q + 1;
        return;
    }
    SB *sb = jp_scratch_reset();
    sb_write(sb, start, (size_t)(bs - start));
    j->p = bs;
    jp_string_tail(j, sb, path);
    *out = sb->buf;
    *len = (int64_t)sb->len;
}

// Consumes and validates a JSON string. It builds nothing.
static void jp_string_skip(JP *j, const JPath *path) {
    jp_want(j, '"', path);
    const char *q = memchr(j->p, '"', (size_t)(j->end - j->p));
    if (!q) jp_fail(path, "unterminated string");
    if (!memchr(j->p, '\\', (size_t)(q - j->p))) {
        j->p = q + 1;
        return;
    }
    jp_string_tail(j, NULL, path);
}

// ---- number fast paths ----
//
// These read the common forms in a document: plain digits, a sign, one dot, no
// exponent. They return 0 for other forms, and the caller then uses strtod.
// Thus acceptance and rounding are the same on both paths.

// Powers of ten that a double holds exactly. 10^22 is the last one.
static const double jp_pow10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

// Reads sign/digits[.digits], 15 digits at most, no exponent. The mantissa and
// the power of ten are both exact, so one IEEE divide gives the strtod result.
static int jp_double_fast(const char *s, const char *e, double *out) {
    int neg = 0;
    if (s < e && *s == '-') {
        neg = 1;
        s++;
    }
    if (s == e) return 0;
    uint64_t m = 0;
    int digits = 0, frac = -1;
    for (; s < e; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') {
            m = m * 10 + (uint64_t)(c - '0');
            digits++;
            if (frac >= 0) frac++;
        } else if (c == '.' && frac < 0) {
            frac = 0;
        } else {
            return 0; // an exponent, a second dot, a stray sign: strtod's case
        }
    }
    if (digits == 0 || digits > 15) return 0;
    if (frac == 0) return 0; // "5.": strtod decides what that is
    double d = (double)m;
    if (frac > 0) d /= jp_pow10[frac];
    *out = neg ? -d : d;
    return 1;
}

// Digits only, 15 at most, so the value is exact in both an int64 and a double.
static int jp_int_fast(const char *s, const char *e, int64_t *out) {
    int neg = 0;
    if (s < e && *s == '-') {
        neg = 1;
        s++;
    }
    if (s == e || e - s > 15) return 0;
    int64_t v = 0;
    for (; s < e; s++) {
        char c = *s;
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (c - '0');
    }
    *out = neg ? -v : v;
    return 1;
}

// Plain digits, 19 at most, which cannot overflow a uint64.
static int jp_uint_fast(const char *s, const char *e, uint64_t *out) {
    if (s == e || e - s > 19) return 0;
    uint64_t v = 0;
    for (; s < e; s++) {
        char c = *s;
        if (c < '0' || c > '9') return 0;
        v = v * 10 + (uint64_t)(c - '0');
    }
    *out = v;
    return 1;
}

// Delimits the number at the current position, before any conversion.
static const char *jp_number_span(JP *j, const JPath *path) {
    jp_ws(j);
    const char *start = j->p;
    if (j->p < j->end && (*j->p == '-' || *j->p == '+')) j->p++;
    while (j->p < j->end) {
        char c = *j->p;
        if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') j->p++;
        else break;
    }
    if (j->p == start) jp_fail(path, "expected a number");
    return start;
}

static void jp_skip_value(JP *j, const JPath *path);

static void jp_skip_members(JP *j, char close, const JPath *path) {
    j->p++; // the opening bracket
    if (jp_peek(j) == close) {
        j->p++;
        return;
    }
    for (;;) {
        if (close == '}') {
            jp_string_skip(j, path);
            jp_want(j, ':', path);
        }
        jp_skip_value(j, path);
        int c = jp_peek(j);
        if (c == ',') {
            j->p++;
            continue;
        }
        jp_want(j, close, path);
        return;
    }
}

// Consumes one JSON value without building anything.
static void jp_skip_value(JP *j, const JPath *path) {
    int c = jp_peek(j);
    switch (c) {
    case '{': jp_skip_members(j, '}', path); return;
    case '[': jp_skip_members(j, ']', path); return;
    case '"':
        jp_string_skip(j, path);
        return;
    case 't':
        if (jp_lit(j, "true")) return;
        break;
    case 'f':
        if (jp_lit(j, "false")) return;
        break;
    case 'n':
        if (jp_lit(j, "null")) return;
        break;
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            jp_number_span(j, path);
            return;
        }
    }
    jp_fail(path, "expected a value");
}

// Whether the parse refuses keys that the target type does not declare
// (`as T strict`, §6.3). It applies at every depth. It is a file static and not
// a parameter, because the runtime is single-threaded and a parse never nests.
static int jp_strict;

// Stores one collected key in a record's catch-all object. The caller roots
// the key.
static void jp_rest_put(JsonNode *rest, Str *key, JsonNode *val);

// Reads one JSON value of any shape into a tree.
static JsonNode *jp_node(JP *j, const JPath *path);

static int64_t jp_value(JP *j, TypeDesc *td, const JPath *path);

static int64_t jp_array(JP *j, TypeDesc *td, const JPath *path) {
    // A fixed-size array shares its descriptor with the growable array of the
    // same element type, so this function cannot check the length. Thus the
    // checker keeps T[N] out of json.parse.
    Arr *a = rt_arr_new(0, rt_arr_width(td->elem->kind));

    // The array is live across every element's allocation.
    TypeDesc *tds[1] = {td};
    int64_t slots[1] = {(int64_t)(intptr_t)a};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    jp_want(j, '[', path);
    if (jp_peek(j) != ']') {
        for (int64_t i = 0;; i++) {
            JPath elem = {path, NULL, 0, i};
            if (i > 0) jp_want(j, ',', path);
            rt_arr_push(a, jp_value(j, td->elem, &elem), td);
            if (jp_peek(j) != ',') break;
        }
    }
    jp_want(j, ']', path);

    rt_gc_top = f.prev;
    return (int64_t)(intptr_t)a;
}

static int64_t jp_record(JP *j, TypeDesc *td, const JPath *path) {
    // rt_rec_set knows the offset and width of a field (§5.7). The parsed value
    // is always the 8-byte unit.
    void *rec = rt_rec_new(td);

    // The half-built record is a valid root: its slots are zero, and a null
    // slot traces as absent. Slot 1 holds a collected key during the parse of
    // its value. Slot 2 holds the tables of a wide record.
    TypeDesc *tds[3] = {td, &rt_td_string, &td_byte_array};
    int64_t slots[3] = {(int64_t)(intptr_t)rec, 0, 0};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;

    // Field-name lengths, measured once per record. A field with no key of its
    // own gets -1, which no key length can equal. A wide record takes both
    // tables from the GC heap, because jp_fail's longjmp would leak a malloc.
    char small[64];
    int64_t lsmall[64];
    char *seen;
    int64_t *flen;
    if (td->nfields <= 64) {
        seen = small;
        flen = lsmall;
    } else {
        Arr *bk = rt_arr_new(td->nfields * 9, 1);
        slots[2] = (int64_t)(intptr_t)bk;
        flen = (int64_t *)rt_arr_bytes(bk);
        seen = (char *)rt_arr_bytes(bk) + td->nfields * 8;
    }
    memset(seen, 0, (size_t)td->nfields);
    for (int64_t i = 0; i < td->nfields; i++) {
        if (json_field_is_rest(td, i) || !json_has_form(td->field_types[i])) {
            flen[i] = -1;
        } else {
            flen[i] = (int64_t)strlen(td->field_names[i]);
        }
    }

    // A preserving record starts with an empty object in its catch-all, so
    // `c.extra["k"] = v` works on a document with no unknown keys.
    if (td->rest) {
        JsonNode *rest = rt_json_empty(JSON_OBJECT);
        rt_rec_set((void *)(intptr_t)slots[0], td->rest - 1,
                   (int64_t)(intptr_t)rest, td);
    }

    jp_want(j, '{', path);
    if (jp_peek(j) != '}') {
        for (;;) {
            const char *key;
            int64_t klen;
            jp_string_span(j, &key, &klen, path);
            jp_want(j, ':', path);

            int64_t idx = -1;
            for (int64_t i = 0; i < td->nfields; i++) {
                if (flen[i] == klen && memcmp(td->field_names[i], key, (size_t)klen) == 0) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0) {
                // Not a field of this type. The three policies of §6.3 differ
                // only here: preserve, refuse, or drop.
                if (td->rest) {
                    // The key's span ends at the parse of the value, so copy it
                    // first into a rooted Str. A malloc would leak through
                    // jp_fail's longjmp.
                    Str *ks = rt_str_alloc(klen);
                    memcpy(ks->data, key, (size_t)klen);
                    slots[1] = (int64_t)(intptr_t)ks;
                    JPath fp = {path, ks->data, klen, 0};
                    int64_t v = jp_value(j, &rt_td_json, &fp);
                    int64_t restRaw = rt_rec_get((void *)(intptr_t)slots[0],
                                                 td->rest - 1, td);
                    JsonNode *rest = (JsonNode *)(intptr_t)restRaw;
                    jp_rest_put(rest, (Str *)(intptr_t)slots[1],
                                (JsonNode *)(intptr_t)v);
                    slots[1] = 0;
                } else if (jp_strict) {
                    char what[128];
                    snprintf(what, sizeof what, "unknown key \"%.*s\"", (int)klen, key);
                    jp_fail(path, what);
                } else {
                    jp_skip_value(j, path);
                }
            } else {
                JPath fp = {path, td->field_names[idx], -1, 0};
                int64_t v = jp_value(j, td->field_types[idx], &fp);
                rt_rec_set(rec, idx, v, td);
                seen[idx] = 1;
            }
            if (jp_peek(j) == ',') {
                j->p++;
                continue;
            }
            break;
        }
    }
    jp_want(j, '}', path);

    // An optional field defaults to null, and a function-typed field has no
    // JSON form. The catch-all is never missing.
    for (int64_t i = 0; i < td->nfields; i++) {
        if (seen[i] || json_field_is_rest(td, i)) continue;
        TypeDesc *ft = td->field_types[i];
        if (ft->kind == TD_OPTIONAL || !json_has_form(ft)) continue;
        JPath fp = {path, td->field_names[i], -1, 0};
        jp_fail(&fp, "missing field");
    }

    rt_gc_top = f.prev;
    return (int64_t)(intptr_t)rec;
}

// Reads a JSON object into a Map<String, V>, in document order. A duplicate
// key overwrites its earlier value.
static int64_t jp_map(JP *j, TypeDesc *td, const JPath *path) {
    Map *m = rt_map_new();

    // The map and the key of each entry must survive the parse of the value.
    TypeDesc *tds[2] = {td, &rt_td_string};
    int64_t slots[2] = {(int64_t)(intptr_t)m, 0};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    jp_want(j, '{', path);
    if (jp_peek(j) != '}') {
        for (;;) {
            const char *kb;
            int64_t klen;
            jp_string_span(j, &kb, &klen, path);
            jp_want(j, ':', path);

            // Build the key before the value is parsed, because the span ends
            // at the next string. Nothing here comes from malloc, so a failing
            // value cannot leak through jp_fail's longjmp.
            Str *ks = rt_str_alloc(klen);
            memcpy(ks->data, kb, (size_t)klen);
            slots[1] = (int64_t)(intptr_t)ks;

            JPath ep = {path, ks->data, klen, 0};
            int64_t v = jp_value(j, td->field_types[1], &ep);
            rt_map_set(m, (int64_t)(intptr_t)ks, v, td);
            slots[1] = 0;

            if (jp_peek(j) == ',') {
                j->p++;
                continue;
            }
            break;
        }
    }
    jp_want(j, '}', path);

    rt_gc_top = f.prev;
    return (int64_t)(intptr_t)m;
}

// ---- unions (§2.11) ----
//
// A TD_UNION is the static descriptor of a slot that holds one of the members
// of a union (runtime.h). The JSON kind of the value selects the member. The
// kind classes here copy jsonKindClass in src/checker.nio. The checker admits
// a union into a parse only when no two members read from the same class.

// The class that a payload of TypeDesc kind k is read from: 's'tring,
// 'n'umber, 'b'ool, 'a'rray, 'o'bject, or 0 for a kind that no document value
// reaches.
static char jp_kind_class(int64_t k) {
    switch (k) {
    case TD_STRING:
    case TD_DATETIME:
        return 's';
    case TD_BOOL:
        return 'b';
    case TD_ARRAY:
        return 'a';
    case TD_RECORD:
    case TD_MAP:
        return 'o';
    case TD_INT8:
    case TD_INT16:
    case TD_INT32:
    case TD_INT:
    case TD_UINT8:
    case TD_UINT16:
    case TD_UINT32:
    case TD_UINT:
    case TD_FLOAT:
    case TD_FLOAT32:
    case TD_DURATION:
    case TD_ENUM:
        return 'n';
    default:
        return 0;
    }
}

static const char *jp_class_name(char c) {
    switch (c) {
    case 's': return "a string";
    case 'n': return "a number";
    case 'b': return "true or false";
    case 'a': return "an array";
    default: return "an object";
    }
}

// The member read from class cls, or NULL. A member descriptor is the member's
// own TD_RECORD, and its single field is the payload (runtime.h).
static TypeDesc *jp_union_pick(const TypeDesc *td, char cls) {
    for (int64_t i = 0; i < td->nfields; i++) {
        TypeDesc *m = td->field_types[i];
        if (m->nfields > 0 && jp_kind_class(m->field_types[0]->kind) == cls) return m;
    }
    return NULL;
}

// Text such as "a string or a number": the classes that the members of the
// union are read from.
static void jp_union_want(const TypeDesc *td, char *buf, size_t cap) {
    const char *names[8];
    int64_t n = 0;
    for (int64_t i = 0; i < td->nfields && n < 8; i++) {
        const TypeDesc *m = td->field_types[i];
        if (m->nfields > 0) {
            char c = jp_kind_class(m->field_types[0]->kind);
            if (c) {
                names[n] = jp_class_name(c);
                n++;
            }
        }
    }
    size_t off = 0;
    buf[0] = 0;
    for (int64_t i = 0; i < n; i++) {
        const char *sep = i == 0 ? "" : (i + 1 == n ? " or " : ", ");
        int w = snprintf(buf + off, cap - off, "%s%s", sep, names[i]);
        if (w < 0 || (size_t)w >= cap - off) break;
        off += (size_t)w;
    }
}

static void jp_union_fail(const TypeDesc *td, const JPath *path) {
    char want[96];
    jp_union_want(td, want, sizeof want);
    char what[128];
    snprintf(what, sizeof what, "expected %s", want);
    jp_fail(path, what);
}

static int64_t jp_union(JP *j, TypeDesc *td, const JPath *path) {
    char cls = 0;
    char c = jp_peek(j);
    if (c == '"') cls = 's';
    else if (c == 't' || c == 'f') cls = 'b';
    else if (c == '[') cls = 'a';
    else if (c == '{') cls = 'o';
    else if (c == '-' || (c >= '0' && c <= '9')) cls = 'n';
    TypeDesc *m = cls ? jp_union_pick(td, cls) : NULL;
    if (!m) jp_union_fail(td, path);

    // The half-built member roots its payload during the parse of the payload.
    void *rec = rt_rec_new(m);
    TypeDesc *tds[1] = {m};
    int64_t slots[1] = {(int64_t)(intptr_t)rec};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    // The path does not change, because the member is only representation.
    int64_t v = jp_value(j, m->field_types[0], path);
    rt_rec_set((void *)(intptr_t)slots[0], 0, v, m);

    int64_t out = slots[0];
    rt_gc_top = f.prev;
    return out;
}

static int64_t jp_value(JP *j, TypeDesc *td, const JPath *path) {
    // Only an optional can take a JSON null. A Json is the exception, because
    // null is one of its seven shapes.
    if (td->kind != TD_OPTIONAL && td->kind != TD_JSON && jp_peek(j) == 'n' &&
        jp_lit(j, "null")) {
        jp_fail(path, "unexpected null (the type is not optional)");
    }
    switch (td->kind) {
    case TD_OPTIONAL: {
        if (jp_lit(j, "null")) return 0;
        int64_t inner = jp_value(j, td->elem, path);
        // The inner value must survive the allocation of the box. The caller of
        // rt_box roots it.
        TypeDesc *tds[1] = {td->elem};
        int64_t slots[1] = {inner};
        GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
        rt_gc_top = &f;
        void *box = rt_box(slots[0], td->elem);
        rt_gc_top = f.prev;
        return (int64_t)(intptr_t)box;
    }
    case TD_BOOL:
        if (jp_lit(j, "true")) return 1;
        if (jp_lit(j, "false")) return 0;
        jp_fail(path, "expected true or false");
        return 0;
    case TD_INT8:
    case TD_INT16:
    case TD_INT32:
    case TD_INT:
    case TD_DURATION:
    case TD_ENUM: {
        const char *start = jp_number_span(j, path);
        int64_t v;
        if (!jp_int_fast(start, j->p, &v)) {
            // Digits past the 15 of the fast path can still give an exact
            // int64, so read the integer first. strtod reads the other forms: a
            // fraction, an exponent, or a value outside the type.
            char *stop = NULL;
            errno = 0;
            long long iv = strtoll(start, &stop, 10);
            if (stop == j->p && errno != ERANGE) {
                v = (int64_t)iv;
            } else {
                double d = strtod(start, &stop);
                if (stop != j->p) jp_fail(path, "expected a whole number");
                // Check the range first. A conversion out of range is undefined
                // behavior, and results differ between hosts.
                if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0)) {
                    jp_fail(path, "expected a whole number an int can hold");
                }
                if (d != (double)(int64_t)d) jp_fail(path, "expected a whole number");
                v = (int64_t)d;
            }
        }
        // A narrow field guarantees its range to the rest of the program.
        switch (td->kind) {
        case TD_INT8:
            if (v < -128 || v > 127) jp_fail(path, "int8 out of range");
            break;
        case TD_INT16:
            if (v < -32768 || v > 32767) jp_fail(path, "int16 out of range");
            break;
        case TD_INT32:
            if (v < -2147483648LL || v > 2147483647LL) jp_fail(path, "int32 out of range");
            break;
        default:
            break;
        }
        return v;
    }
    case TD_UINT8:
    case TD_UINT16:
    case TD_UINT32:
    case TD_UINT: {
        // Plain digits go through strtoull and not strtod.
        // 18446744073709551615 is not a double, so a double rounds it up and
        // then rejects the largest value of the type.
        const char *start = jp_number_span(j, path);
        char *stop = NULL;
        uint64_t v;
        if (jp_uint_fast(start, j->p, &v)) {
            // Plain digits, the same text that strtoull below reads.
        } else if (*start >= '0' && *start <= '9') {
            errno = 0;
            v = (uint64_t)strtoull(start, &stop, 10);
            if (stop != j->p || errno == ERANGE) {
                // The span holds a fraction or an exponent, where strtoull
                // stopped, or the digits are too large for the type. Only the
                // first case can still give a whole number (1e3).
                errno = 0;
                double d = strtod(start, &stop);
                if (stop != j->p) jp_fail(path, "expected a whole number");
                if (!(d >= 0 && d < UINT64_LIMIT)) {
                    jp_fail(path, uint_range_message(td->kind));
                }
                v = (uint64_t)d;
                if ((double)v != d) jp_fail(path, "expected a whole number");
            }
        } else {
            // A sign or a leading '.', where strtoull cannot start. A negative
            // value is out of range. It is not reported as "not a whole
            // number".
            double d = strtod(start, &stop);
            if (stop != j->p) jp_fail(path, "expected a whole number");
            if (!(d >= 0 && d < UINT64_LIMIT)) {
                jp_fail(path, uint_range_message(td->kind));
            }
            v = (uint64_t)d;
            if ((double)v != d) jp_fail(path, "expected a whole number");
        }
        if (v > uint_kind_max(td->kind)) jp_fail(path, uint_range_message(td->kind));
        return (int64_t)v;
    }
    case TD_FLOAT:
    case TD_FLOAT32: {
        const char *start = jp_number_span(j, path);
        double d;
        if (!jp_double_fast(start, j->p, &d)) {
            char *stop = NULL;
            d = strtod(start, &stop);
            if (stop != j->p) jp_fail(path, "expected a number");
        }
        if (td->kind == TD_FLOAT32) {
            // A float32 rounds to binary32. It must not overflow to infinity.
            float f = (float)d;
            if (isinf(f) && !isinf(d)) jp_fail(path, "float32 out of range");
            d = (double)f;
        }
        int64_t bits;
        memcpy(&bits, &d, 8);
        return bits;
    }
    case TD_STRING: {
        const char *b;
        int64_t n;
        jp_string_span(j, &b, &n, path);
        // rt_str_alloc can collect. b points into the rooted input text or
        // into jp_scratch, so it survives in both cases.
        Str *s = rt_str_alloc(n);
        memcpy(s->data, b, (size_t)n);
        return (int64_t)(intptr_t)s;
    }
    case TD_DATETIME: {
        const char *b;
        int64_t n;
        jp_string_span(j, &b, &n, path);
        int64_t ms = 0;
        if (n >= 64 || !rt_date_parse_iso(b, n, &ms))
            jp_fail(path, "expected a date string (\"YYYY-MM-DD\" or \"YYYY-MM-DDTHH:MM:SSZ\")");
        return ms;
    }
    case TD_ARRAY:
        if (jp_peek(j) != '[') jp_fail(path, "expected an array");
        return jp_array(j, td, path);
    case TD_MAP:
        if (jp_peek(j) != '{') jp_fail(path, "expected an object");
        return jp_map(j, td, path);
    case TD_RECORD:
        if (jp_peek(j) != '{') jp_fail(path, "expected an object");
        return jp_record(j, td, path);
    case TD_UNION:
        return jp_union(j, td, path);
    case TD_JSON:
        // No type to check against, so the tree records what the document
        // holds.
        return (int64_t)(intptr_t)jp_node(j, path);
    default:
        jp_fail(path, "this type has no JSON form");
        return 0;
    }
}

int64_t rt_json_parse(Str *text, TypeDesc *td) {
    JP j = {text->data, text->data + text->len};
    // The text is an unrooted parameter held across every allocation below.
    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)text};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t v = jp_value(&j, td, NULL);
    jp_ws(&j);
    if (j.p != j.end) jp_fail(NULL, "trailing text after the value");

    rt_gc_top = f.prev;
    return v;
}

// ---- Json values (§6.3) ----
//
// JsonNode trees: `json.parse(s)` with no type builds one, and a `Json` field
// or variable holds one. The contract is in runtime.h, and gc.c walks a node.
//
// Two rules apply everywhere. A NULL node is MISSING, so no call site needs a
// null guard, and a chain through an absent key cannot trap. Assignment copies,
// so no two places in a tree share a node, and `raw["self"] = raw` is a finite
// document and not a cycle.

static JsonNode *jn_new(int64_t kind) {
    JsonNode *n = rt_alloc((int64_t)sizeof(JsonNode));
    n->kind = kind;
    return n;
}

static JsonVec *jv_new(int64_t cap) {
    if (cap < 4) cap = 4;
    JsonVec *v = rt_alloc((int64_t)sizeof(JsonVec) + cap * 8);
    v->cap = cap;
    return v;
}

// Appends val to the vector in *slot, and grows the vector when it is full. The
// caller must root the owner of *slot. This function roots val, because growth
// allocates and val is an unrooted C local until it is in the vector.
static void jv_push(void **slot, int64_t val, TypeDesc *vtd) {
    JsonVec *v = *slot;
    if (v->len == v->cap) {
        TypeDesc *tds[1] = {vtd};
        int64_t slots[1] = {val};
        GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
        rt_gc_top = &f;
        JsonVec *nv = jv_new(v->cap * 2);
        rt_gc_top = f.prev;

        nv->len = v->len;
        memcpy(nv->slots, v->slots, (size_t)v->len * 8);
        *slot = nv;
        v = nv;
    }
    v->slots[v->len++] = val;
}

int64_t rt_json_kind(JsonNode *v) { return v ? v->kind : JSON_MISSING; }

int64_t rt_json_length(JsonNode *v) {
    if (!v) return 0;
    if (v->kind == JSON_ARRAY) return v->a ? ((JsonVec *)v->a)->len : 0;
    if (v->kind == JSON_OBJECT) return v->a ? ((JsonVec *)v->a)->len : 0;
    return 0;
}

// The index of key in the key vector of an object, or -1. A linear scan keeps
// insertion order, which is document order. Serialization writes that order.
static int64_t jn_find(JsonNode *v, Str *key) {
    if (!v || v->kind != JSON_OBJECT || !v->a) return -1;
    JsonVec *keys = v->a;
    for (int64_t i = 0; i < keys->len; i++) {
        Str *k = (Str *)(intptr_t)keys->slots[i];
        if (k->len == key->len && memcmp(k->data, key->data, (size_t)k->len) == 0) return i;
    }
    return -1;
}

JsonNode *rt_json_get(JsonNode *v, Str *key) {
    int64_t i = jn_find(v, key);
    if (i < 0) return NULL;
    return (JsonNode *)(intptr_t)((JsonVec *)v->b)->slots[i];
}

JsonNode *rt_json_at(JsonNode *v, int64_t i) {
    if (!v || v->kind != JSON_ARRAY || !v->a) return NULL;
    JsonVec *items = v->a;
    if (i < 0 || i >= items->len) return NULL;
    return (JsonNode *)(intptr_t)items->slots[i];
}

Arr *rt_json_keys(JsonNode *v) {
    int64_t n = (v && v->kind == JSON_OBJECT && v->a) ? ((JsonVec *)v->a)->len : 0;
    // The node is rooted across the two allocations of the array.
    TypeDesc *tds[1] = {&rt_td_json};
    int64_t slots[1] = {(int64_t)(intptr_t)v};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *out = rt_arr_new(n, 8);
    for (int64_t i = 0; i < n; i++) {
        JsonNode *node = (JsonNode *)(intptr_t)slots[0];
        rt_arr_units(out)[i] = ((JsonVec *)node->a)->slots[i];
    }

    rt_gc_top = f.prev;
    return out;
}

// ---- deep copy ----

static JsonNode *jn_copy(JsonNode *src);

static void jn_copy_into(JsonNode *dst, JsonNode *src) {
    TypeDesc *tds[3] = {&rt_td_json, &rt_td_json, &rt_td_json};
    int64_t slots[3] = {(int64_t)(intptr_t)dst, (int64_t)(intptr_t)src, 0};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;
#define DST ((JsonNode *)(intptr_t)slots[0])
#define SRC ((JsonNode *)(intptr_t)slots[1])

    DST->kind = SRC->kind;
    DST->num = SRC->num;
    DST->isint = SRC->isint;
    switch (SRC->kind) {
    case JSON_STRING:
        // Shared and not copied. A Str is immutable, so the two trees cannot
        // change each other through it.
        DST->a = SRC->a;
        break;
    case JSON_ARRAY: {
        int64_t n = SRC->a ? ((JsonVec *)SRC->a)->len : 0;
        DST->a = jv_new(n);
        for (int64_t i = 0; i < n; i++) {
            slots[2] = (int64_t)(intptr_t)jn_copy(
                (JsonNode *)(intptr_t)((JsonVec *)SRC->a)->slots[i]);
            jv_push(&DST->a, slots[2], &rt_td_json);
            slots[2] = 0;
        }
        break;
    }
    case JSON_OBJECT: {
        int64_t n = SRC->a ? ((JsonVec *)SRC->a)->len : 0;
        DST->a = jv_new(n);
        DST->b = jv_new(n);
        for (int64_t i = 0; i < n; i++) {
            // The key is an immutable Str shared with the source.
            jv_push(&DST->a, ((JsonVec *)SRC->a)->slots[i], &rt_td_string);
            slots[2] = (int64_t)(intptr_t)jn_copy(
                (JsonNode *)(intptr_t)((JsonVec *)SRC->b)->slots[i]);
            jv_push(&DST->b, slots[2], &rt_td_json);
            slots[2] = 0;
        }
        break;
    }
    default:
        break;
    }
#undef DST
#undef SRC
    rt_gc_top = f.prev;
}

static JsonNode *jn_copy(JsonNode *src) {
    if (!src) return NULL;
    TypeDesc *tds[1] = {&rt_td_json};
    int64_t slots[1] = {(int64_t)(intptr_t)src};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    JsonNode *dst = jn_new(JSON_MISSING);
    jn_copy_into(dst, (JsonNode *)(intptr_t)slots[0]);

    rt_gc_top = f.prev;
    return dst;
}

// ---- mutation ----
//
// A write into a shape that the write did not expect panics. It does not raise
// a catchable Error, because the document already stated its shape. There is
// no auto-vivification: `raw["tsl"]["x"] = 1` with a typo must not build a
// subtree.

static void jn_want(JsonNode *v, int64_t kind, const char *what) {
    if (!v) rt_panic("cannot write into a Json that is missing");
    if (v->kind != kind) {
        char msg[128];
        static const char *names[] = {"missing", "null",  "a bool",  "a number",
                                      "a string", "an array", "an object"};
        const char *had = (v->kind >= 0 && v->kind <= JSON_OBJECT) ? names[v->kind] : "a value";
        snprintf(msg, sizeof msg, "cannot %s: this Json is %s", what, had);
        rt_panic(msg);
    }
}

void rt_json_set(JsonNode *v, Str *key, JsonNode *val) {
    jn_want(v, JSON_OBJECT, "set a key");
    TypeDesc *tds[3] = {&rt_td_json, &rt_td_string, &rt_td_json};
    int64_t slots[3] = {(int64_t)(intptr_t)v, (int64_t)(intptr_t)key, (int64_t)(intptr_t)val};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;

    // Store the copy in the frame and not in a C local. The push of the key can
    // grow that vector, and growth allocates.
    slots[2] = (int64_t)(intptr_t)jn_copy((JsonNode *)(intptr_t)slots[2]);
    JsonNode *node = (JsonNode *)(intptr_t)slots[0];
    int64_t i = jn_find(node, (Str *)(intptr_t)slots[1]);
    if (i >= 0) {
        ((JsonVec *)node->b)->slots[i] = slots[2];
    } else {
        if (!node->a) node->a = jv_new(4);
        if (!node->b) node->b = jv_new(4);
        jv_push(&node->a, slots[1], &rt_td_string);
        jv_push(&((JsonNode *)(intptr_t)slots[0])->b, slots[2], &rt_td_json);
    }

    rt_gc_top = f.prev;
}

void rt_json_set_at(JsonNode *v, int64_t i, JsonNode *val) {
    jn_want(v, JSON_ARRAY, "set an element");
    int64_t n = v->a ? ((JsonVec *)v->a)->len : 0;
    if (i < 0 || i >= n) {
        // Only in range, as for an array index (§5.5). json.push makes an
        // array grow.
        char msg[96];
        snprintf(msg, sizeof msg, "Json index %lld out of range (length %lld)", (long long)i,
                 (long long)n);
        rt_panic(msg);
    }
    TypeDesc *tds[2] = {&rt_td_json, &rt_td_json};
    int64_t slots[2] = {(int64_t)(intptr_t)v, (int64_t)(intptr_t)val};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    JsonNode *copy = jn_copy((JsonNode *)(intptr_t)slots[1]);
    ((JsonVec *)((JsonNode *)(intptr_t)slots[0])->a)->slots[i] = (int64_t)(intptr_t)copy;

    rt_gc_top = f.prev;
}

void rt_json_push(JsonNode *v, JsonNode *val) {
    jn_want(v, JSON_ARRAY, "append an element");
    TypeDesc *tds[2] = {&rt_td_json, &rt_td_json};
    int64_t slots[2] = {(int64_t)(intptr_t)v, (int64_t)(intptr_t)val};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    // Store it in the frame, as rt_json_set does. The vector can still need
    // to be created, and that allocates.
    slots[1] = (int64_t)(intptr_t)jn_copy((JsonNode *)(intptr_t)slots[1]);
    JsonNode *node = (JsonNode *)(intptr_t)slots[0];
    if (!node->a) node->a = jv_new(4);
    jv_push(&((JsonNode *)(intptr_t)slots[0])->a, slots[1], &rt_td_json);

    rt_gc_top = f.prev;
}

// Removes a key and closes the gap, so the remaining keys keep document order.
// Removing a key that is not present is not an error.
void rt_json_remove(JsonNode *v, Str *key) {
    int64_t i = jn_find(v, key);
    if (i < 0) return;
    JsonVec *keys = v->a;
    JsonVec *vals = v->b;
    for (int64_t k = i + 1; k < keys->len; k++) {
        keys->slots[k - 1] = keys->slots[k];
        vals->slots[k - 1] = vals->slots[k];
    }
    keys->len--;
    vals->len--;
}

// ---- extraction ----

static void *json_kind_error(const char *want, JsonNode *v) {
    static const char *names[] = {"missing", "null",     "a bool",   "a number",
                                 "a string", "an array", "an object"};
    int64_t k = rt_json_kind(v);
    const char *had = (k >= 0 && k <= JSON_OBJECT) ? names[k] : "a value";
    char msg[128];
    snprintf(msg, sizeof msg, "json.as%s: this Json is %s", want, had);
    return rt_error_new(msg, NIO_ERR_INVALID);
}

int64_t rt_json_as_int(JsonNode *v, void **err) {
    if (rt_json_kind(v) != JSON_NUMBER) {
        *err = json_kind_error("Int", v);
        return 0;
    }
    if (v->isint == JN_UINT) {
        // Above the range of an int64, no int can hold the value.
        if (v->num < 0) {
            *err = rt_error_new("json.asInt: this number is not a whole number an int can hold",
                                NIO_ERR_INVALID);
            return 0;
        }
        return v->num;
    }
    if (v->isint) return v->num;
    double d;
    memcpy(&d, &v->num, 8);
    if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0) || d != (double)(int64_t)d) {
        *err = rt_error_new("json.asInt: this number is not a whole number an int can hold",
                            NIO_ERR_INVALID);
        return 0;
    }
    return (int64_t)d;
}

double rt_json_as_float(JsonNode *v, void **err) {
    if (rt_json_kind(v) != JSON_NUMBER) {
        *err = json_kind_error("Float", v);
        return 0;
    }
    if (v->isint) return (double)v->num;
    double d;
    memcpy(&d, &v->num, 8);
    return d;
}

Str *rt_json_as_text(JsonNode *v, void **err) {
    if (rt_json_kind(v) != JSON_STRING) {
        *err = json_kind_error("Text", v);
        return NULL;
    }
    return (Str *)v->a;
}

int64_t rt_json_as_bool(JsonNode *v, void **err) {
    if (rt_json_kind(v) != JSON_BOOL) {
        *err = json_kind_error("Bool", v);
        return 0;
    }
    return v->num;
}

JsonNode *rt_json_empty(int64_t kind) {
    JsonNode *n = jn_new(kind);
    if (kind != JSON_ARRAY && kind != JSON_OBJECT) return n;

    // Root the node across the allocations of its vectors, because nothing
    // else can reach the new block.
    TypeDesc *tds[1] = {&rt_td_json};
    int64_t slots[1] = {(int64_t)(intptr_t)n};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    ((JsonNode *)(intptr_t)slots[0])->a = jv_new(4);
    if (kind == JSON_OBJECT) ((JsonNode *)(intptr_t)slots[0])->b = jv_new(4);
    n = (JsonNode *)(intptr_t)slots[0];

    rt_gc_top = f.prev;
    return n;
}

// ---- serializing a node ----

// A MISSING node has nothing to write, so it writes `null`.
static void json_node_value(SB *sb, JsonNode *n) {
    switch (rt_json_kind(n)) {
    case JSON_MISSING:
    case JSON_NULL:
        sb_cstr(sb, "null");
        break;
    case JSON_BOOL:
        sb_cstr(sb, n->num ? "true" : "false");
        break;
    case JSON_NUMBER:
        if (n->isint == JN_UINT) {
            sb_u64(sb, (uint64_t)n->num);
        } else if (n->isint) {
            sb_i64(sb, n->num);
        } else {
            double d;
            memcpy(&d, &n->num, 8);
            if (isnan(d) || isinf(d)) rt_panic("cannot serialize a non-finite float to JSON");
            sb_double(sb, d, 0);
        }
        break;
    case JSON_STRING: {
        Str *s = n->a;
        sb_json_string(sb, s->data, s->len);
        break;
    }
    case JSON_ARRAY: {
        JsonVec *items = n->a;
        sb_char(sb, '[');
        for (int64_t i = 0; items && i < items->len; i++) {
            if (i) sb_char(sb, ',');
            json_node_value(sb, (JsonNode *)(intptr_t)items->slots[i]);
        }
        sb_char(sb, ']');
        break;
    }
    case JSON_OBJECT: {
        JsonVec *keys = n->a;
        JsonVec *vals = n->b;
        sb_char(sb, '{');
        for (int64_t i = 0; keys && i < keys->len; i++) {
            if (i) sb_char(sb, ',');
            Str *k = (Str *)(intptr_t)keys->slots[i];
            sb_json_string(sb, k->data, k->len);
            sb_char(sb, ':');
            json_node_value(sb, (JsonNode *)(intptr_t)vals->slots[i]);
        }
        sb_char(sb, '}');
        break;
    }
    }
}

// ---- building a node from a Nio value ----

// The inverse of `as T`: turns any value with a JSON form (§6.3) into a tree.
// An assignment into a Json calls it. It accepts the types that json.toText
// accepts.
JsonNode *rt_json_of(int64_t raw, TypeDesc *td) {
    // The source value is an unrooted parameter, held across every allocation.
    TypeDesc *tds[3] = {td, &rt_td_json, &rt_td_json};
    int64_t slots[3] = {raw, 0, 0};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;
    JsonNode *out = NULL;

    switch (td->kind) {
    case TD_JSON:
        // Assignment copies, so `raw["a"] = raw` cannot make a cycle.
        out = jn_copy((JsonNode *)(intptr_t)raw);
        break;
    case TD_INT8:
    case TD_INT16:
    case TD_INT32:
    case TD_INT:
    case TD_DURATION:
    case TD_ENUM:
        out = jn_new(JSON_NUMBER);
        out->num = raw;
        out->isint = JN_INT;
        break;
    case TD_UINT8:
    case TD_UINT16:
    case TD_UINT32:
    case TD_UINT:
        out = jn_new(JSON_NUMBER);
        out->num = raw;
        out->isint = JN_UINT;
        break;
    case TD_FLOAT:
    case TD_FLOAT32: {
        double d;
        memcpy(&d, &raw, 8);
        if (isnan(d) || isinf(d)) rt_panic("cannot serialize a non-finite float to JSON");
        out = jn_new(JSON_NUMBER);
        out->num = raw;
        break;
    }
    case TD_BOOL:
        out = jn_new(JSON_BOOL);
        out->num = raw ? 1 : 0;
        break;
    case TD_STRING:
        out = jn_new(JSON_STRING);
        out->a = (void *)(intptr_t)slots[0];
        break;
    case TD_DATETIME: {
        // A DateTime is text in JSON (§6.3). rt_date_to_text is core, so this
        // needs no lib/time.c.
        Str *s = rt_date_to_text(raw);
        slots[1] = (int64_t)(intptr_t)s;
        out = jn_new(JSON_STRING);
        out->a = (void *)(intptr_t)slots[1];
        break;
    }
    case TD_OPTIONAL: {
        void *p = (void *)(intptr_t)slots[0];
        if (!p) {
            out = jn_new(JSON_NULL);
            break;
        }
        out = rt_json_of(rt_box_get(p, td->elem), td->elem);
        break;
    }
    case TD_ARRAY: {
        out = rt_json_empty(JSON_ARRAY);
        slots[1] = (int64_t)(intptr_t)out;
        Arr *a = (Arr *)(intptr_t)slots[0];
        for (int64_t i = 0; i < a->len; i++) {
            slots[2] = (int64_t)(intptr_t)rt_json_of(
                rt_arr_get((Arr *)(intptr_t)slots[0], i, td->elem), td->elem);
            jv_push(&((JsonNode *)(intptr_t)slots[1])->a, slots[2], &rt_td_json);
            slots[2] = 0;
        }
        out = (JsonNode *)(intptr_t)slots[1];
        break;
    }
    case TD_MAP: {
        out = rt_json_empty(JSON_OBJECT);
        slots[1] = (int64_t)(intptr_t)out;
        Map *m = (Map *)(intptr_t)slots[0];
        for (int64_t i = 0; i < m->len; i++) {
            Map *cur = (Map *)(intptr_t)slots[0];
            int64_t key = rt_map_key_at(cur, i, td->field_types[0]);
            slots[2] = (int64_t)(intptr_t)rt_json_of(
                rt_map_val_at(cur, i, td->field_types[1]), td->field_types[1]);
            JsonNode *o = (JsonNode *)(intptr_t)slots[1];
            jv_push(&o->a, key, &rt_td_string);
            jv_push(&o->b, slots[2], &rt_td_json);
            slots[2] = 0;
        }
        out = (JsonNode *)(intptr_t)slots[1];
        break;
    }
    case TD_UNION:
    case TD_RECORD: {
        // The dynamic type of the value decides its shape here too. See
        // json_value.
        if (!slots[0]) {
            out = jn_new(JSON_NULL);
            break;
        }
        if (REC_TD((char *)(intptr_t)slots[0])) td = REC_TD((char *)(intptr_t)slots[0]);
        if (td->uni) {
            out = rt_json_of(rt_rec_get((void *)(intptr_t)slots[0], 0, td),
                             td->field_types[0]);
            break;
        }
        out = rt_json_empty(JSON_OBJECT);
        slots[1] = (int64_t)(intptr_t)out;
        for (int64_t i = 0; i < td->nfields; i++) {
            if (!json_has_form(td->field_types[i])) continue;
            int64_t fraw = rt_rec_get((void *)(intptr_t)slots[0], i, td);
            if (json_field_is_rest(td, i)) {
                // Spliced, as in the text form.
                JsonNode *rest = (JsonNode *)(intptr_t)fraw;
                if (rt_json_kind(rest) != JSON_OBJECT) continue;
                JsonVec *keys = rest->a;
                for (int64_t k = 0; keys && k < keys->len; k++) {
                    Str *key = (Str *)(intptr_t)((JsonVec *)rest->a)->slots[k];
                    if (json_declares(td, key)) continue;
                    slots[2] = (int64_t)(intptr_t)jn_copy(
                        (JsonNode *)(intptr_t)((JsonVec *)rest->b)->slots[k]);
                    JsonNode *o = (JsonNode *)(intptr_t)slots[1];
                    jv_push(&o->a, (int64_t)(intptr_t)key, &rt_td_string);
                    jv_push(&o->b, slots[2], &rt_td_json);
                    slots[2] = 0;
                }
                continue;
            }
            if (td->field_types[i]->kind == TD_OPTIONAL && fraw == 0) continue;
            slots[2] = (int64_t)(intptr_t)rt_json_of(fraw, td->field_types[i]);
            JsonNode *o = (JsonNode *)(intptr_t)slots[1];
            Str *name = rt_str_from_c(td->field_names[i]);
            jv_push(&o->a, (int64_t)(intptr_t)name, &rt_td_string);
            jv_push(&o->b, slots[2], &rt_td_json);
            slots[2] = 0;
        }
        out = (JsonNode *)(intptr_t)slots[1];
        break;
    }
    default:
        rt_panic("this type has no JSON form");
    }

    rt_gc_top = f.prev;
    return out;
}

// ---- parsing into a tree ----
//
// The counterpart of jp_value. jp_value follows a type descriptor and rejects
// what the type cannot hold. This function follows the text and accepts what
// is there.

static void jp_rest_put(JsonNode *rest, Str *key, JsonNode *val) {
    // Root the value across the push of the key, because growth allocates.
    TypeDesc *tds[2] = {&rt_td_json, &rt_td_json};
    int64_t slots[2] = {(int64_t)(intptr_t)rest, (int64_t)(intptr_t)val};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    JsonNode *node = (JsonNode *)(intptr_t)slots[0];
    jv_push(&node->a, (int64_t)(intptr_t)key, &rt_td_string);
    jv_push(&((JsonNode *)(intptr_t)slots[0])->b, slots[1], &rt_td_json);

    rt_gc_top = f.prev;
}

static JsonNode *jp_node(JP *j, const JPath *path) {
    int c = jp_peek(j);
    switch (c) {
    case 'n':
        if (jp_lit(j, "null")) return jn_new(JSON_NULL);
        break;
    case 't':
        if (jp_lit(j, "true")) {
            JsonNode *n = jn_new(JSON_BOOL);
            n->num = 1;
            return n;
        }
        break;
    case 'f':
        if (jp_lit(j, "false")) return jn_new(JSON_BOOL);
        break;
    case '"': {
        const char *b;
        int64_t blen;
        jp_string_span(j, &b, &blen, path);
        Str *s = rt_str_alloc(blen);
        memcpy(s->data, b, (size_t)blen);
        // Root the string across the allocation of the node.
        TypeDesc *tds[1] = {&rt_td_string};
        int64_t slots[1] = {(int64_t)(intptr_t)s};
        GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
        rt_gc_top = &f;
        JsonNode *n = jn_new(JSON_STRING);
        n->a = (void *)(intptr_t)slots[0];
        rt_gc_top = f.prev;
        return n;
    }
    case '[': {
        JsonNode *arr = rt_json_empty(JSON_ARRAY);
        TypeDesc *tds[2] = {&rt_td_json, &rt_td_json};
        int64_t slots[2] = {(int64_t)(intptr_t)arr, 0};
        GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
        rt_gc_top = &f;

        j->p++;
        if (jp_peek(j) != ']') {
            for (int64_t i = 0;; i++) {
                JPath ep = {path, NULL, 0, i};
                slots[1] = (int64_t)(intptr_t)jp_node(j, &ep);
                jv_push(&((JsonNode *)(intptr_t)slots[0])->a, slots[1], &rt_td_json);
                slots[1] = 0;
                if (jp_peek(j) == ',') {
                    j->p++;
                    continue;
                }
                break;
            }
        }
        jp_want(j, ']', path);

        JsonNode *out = (JsonNode *)(intptr_t)slots[0];
        rt_gc_top = f.prev;
        return out;
    }
    case '{': {
        JsonNode *obj = rt_json_empty(JSON_OBJECT);
        TypeDesc *tds[3] = {&rt_td_json, &rt_td_json, &rt_td_string};
        int64_t slots[3] = {(int64_t)(intptr_t)obj, 0, 0};
        GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
        rt_gc_top = &f;

        j->p++;
        if (jp_peek(j) != '}') {
            for (;;) {
                const char *kb;
                int64_t klen;
                jp_string_span(j, &kb, &klen, path);
                jp_want(j, ':', path);
                // The span ends at the parse of the value, so make the key a
                // rooted Str now. A malloc would leak through jp_fail's
                // longjmp.
                Str *ks = rt_str_alloc(klen);
                memcpy(ks->data, kb, (size_t)klen);
                slots[2] = (int64_t)(intptr_t)ks;
                JPath kp = {path, ks->data, klen, 0};
                slots[1] = (int64_t)(intptr_t)jp_node(j, &kp);
                // A duplicate key overwrites, as `m[k] = v` does (§2.8). The
                // entry keeps the position of the first one.
                JsonNode *o = (JsonNode *)(intptr_t)slots[0];
                int64_t at = -1;
                JsonVec *keys = o->a;
                for (int64_t i = 0; keys && i < keys->len; i++) {
                    Str *k = (Str *)(intptr_t)keys->slots[i];
                    if (k->len == klen && memcmp(k->data, ks->data, (size_t)klen) == 0) {
                        at = i;
                        break;
                    }
                }
                if (at >= 0) {
                    ((JsonVec *)o->b)->slots[at] = slots[1];
                } else {
                    jp_rest_put(o, ks, (JsonNode *)(intptr_t)slots[1]);
                }
                slots[1] = 0;
                slots[2] = 0;
                if (jp_peek(j) == ',') {
                    j->p++;
                    continue;
                }
                break;
            }
        }
        jp_want(j, '}', path);

        JsonNode *out = (JsonNode *)(intptr_t)slots[0];
        rt_gc_top = f.prev;
        return out;
    }
    default:
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char *start = jp_number_span(j, path);
            size_t n = (size_t)(j->p - start);
            JsonNode *node;
            // The fast paths classify a number as the slow chain below does.
            int64_t iv;
            double dv;
            if (jp_int_fast(start, j->p, &iv)) {
                node = jn_new(JSON_NUMBER);
                node->num = iv;
                node->isint = JN_INT;
                return node;
            }
            if (jp_double_fast(start, j->p, &dv)) {
                node = jn_new(JSON_NUMBER);
                memcpy(&node->num, &dv, 8);
                return node;
            }
            char buf[64];
            if (n >= sizeof buf) jp_fail(path, "number is too long");
            memcpy(buf, start, n);
            buf[n] = 0;
            node = jn_new(JSON_NUMBER);
            // A number written as an integer stays an integer, so a value
            // outside the exact range of a double survives a round trip.
            char *endp = NULL;
            errno = 0;
            long long as_int = strtoll(buf, &endp, 10);
            if (endp && *endp == 0 && errno != ERANGE) {
                node->num = (int64_t)as_int;
                node->isint = JN_INT;
                return node;
            }
            // Past the range of an int64, one exact form remains: a whole
            // number that is not negative.
            if (buf[0] != '-') {
                endp = NULL;
                errno = 0;
                unsigned long long as_uint = strtoull(buf, &endp, 10);
                if (endp && *endp == 0 && errno != ERANGE) {
                    node->num = (int64_t)(uint64_t)as_uint;
                    node->isint = JN_UINT;
                    return node;
                }
            }
            double d = strtod(buf, NULL);
            memcpy(&node->num, &d, 8);
            return node;
        }
    }
    jp_fail(path, "expected a value");
    return NULL;
}

// ---- entry points ----

// Runs one parse and turns failures into a catchable Error (§2.9). rt_gc_top
// is restored on the error path, because the longjmp skips the frames that the
// scan pushed.
#define JP_GUARD(errslot, failval)                                             \
    GCFrame *save_top = rt_gc_top;                                             \
    int save_strict = jp_strict;                                               \
    int save_catch = jp_err_catching;                                          \
    jp_err_catching = 1;                                                       \
    if (setjmp(jp_err_jmp)) {                                                  \
        rt_gc_top = save_top;                                                  \
        jp_err_catching = save_catch;                                          \
        jp_strict = save_strict;                                               \
        *(errslot) = rt_error_new(jp_err_msg, NIO_ERR_INVALID);                \
        return failval;                                                        \
    }

#define JP_UNGUARD()                                                           \
    jp_err_catching = save_catch;                                              \
    jp_strict = save_strict;

JsonNode *rt_json_parse_value(Str *text, void **err) {
    JP j = {text->data, text->data + text->len};
    JP_GUARD(err, NULL)
    jp_strict = 0;

    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)text};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    JsonNode *v = jp_node(&j, NULL);
    jp_ws(&j);
    if (j.p != j.end) jp_fail(NULL, "trailing text after the value");

    rt_gc_top = f.prev;
    JP_UNGUARD()
    return v;
}

int64_t rt_json_parse_typed(Str *text, TypeDesc *td, int64_t strict, void **err) {
    JP j = {text->data, text->data + text->len};
    JP_GUARD(err, 0)
    jp_strict = strict ? 1 : 0;

    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)text};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t v = jp_value(&j, td, NULL);
    jp_ws(&j);
    if (j.p != j.end) jp_fail(NULL, "trailing text after the value");

    rt_gc_top = f.prev;
    JP_UNGUARD()
    return v;
}

// The byte[] forms of the two parse entry points (§6.3). A byte[] stores one
// byte per element (§5.7), so the data block of the Arr is the text, and the
// document is read in place. No pointer into it stays after the return.

JsonNode *rt_json_parse_value_b(Arr *bytes, void **err) {
    const char *base = (const char *)rt_arr_bytes(bytes);
    JP j = {base, base + bytes->len};
    JP_GUARD(err, NULL)
    jp_strict = 0;

    TypeDesc *tds[1] = {&td_byte_array};
    int64_t slots[1] = {(int64_t)(intptr_t)bytes};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    JsonNode *v = jp_node(&j, NULL);
    jp_ws(&j);
    if (j.p != j.end) jp_fail(NULL, "trailing text after the value");

    rt_gc_top = f.prev;
    JP_UNGUARD()
    return v;
}

int64_t rt_json_parse_typed_b(Arr *bytes, TypeDesc *td, int64_t strict, void **err) {
    const char *base = (const char *)rt_arr_bytes(bytes);
    JP j = {base, base + bytes->len};
    JP_GUARD(err, 0)
    jp_strict = strict ? 1 : 0;

    TypeDesc *tds[1] = {&td_byte_array};
    int64_t slots[1] = {(int64_t)(intptr_t)bytes};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t v = jp_value(&j, td, NULL);
    jp_ws(&j);
    if (j.p != j.end) jp_fail(NULL, "trailing text after the value");

    rt_gc_top = f.prev;
    JP_UNGUARD()
    return v;
}

// ---- reading a tree into a typed value ----
//
// The second half of `as T`, for JSON that is already a tree (§3.6). It does
// the walk of jp_value, but a node controls it in place of a scanner. It
// allocates only the result and shares the strings of the tree. A round trip
// through text cannot share them.

static int64_t jn_value(JsonNode *n, TypeDesc *td, const JPath *path);

// The kind names used in this file's diagnostics, indexed by JSON_*.
static const char *jn_kind_name(int64_t k) {
    static const char *names[] = {"missing", "null",     "a bool",   "a number",
                                 "a string", "an array", "an object"};
    return (k >= 0 && k <= JSON_OBJECT) ? names[k] : "a value";
}

static void jn_wrong(const JPath *path, const char *want, JsonNode *n) {
    char what[96];
    snprintf(what, sizeof what, "expected %s, found %s", want, jn_kind_name(rt_json_kind(n)));
    jp_fail(path, what);
}

// The value of a NUMBER node as a double, in any of its three storage forms.
static double jn_double(JsonNode *n) {
    if (n->isint == JN_UINT) return (double)(uint64_t)n->num;
    if (n->isint) return (double)n->num;
    double d;
    memcpy(&d, &n->num, 8);
    return d;
}

static int64_t jn_array(JsonNode *n, TypeDesc *td, const JPath *path) {
    JsonVec *items = n->a;
    int64_t len = items ? items->len : 0;

    // Root the source node across the allocation of each element. The result
    // array is a valid root from the start: its slots are zero, and a null slot
    // traces as absent.
    TypeDesc *tds[2] = {&rt_td_json, td};
    int64_t slots[2] = {(int64_t)(intptr_t)n, 0};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    slots[1] = (int64_t)(intptr_t)rt_arr_new(len, rt_arr_width(td->elem->kind));
    for (int64_t i = 0; i < len; i++) {
        JPath ep = {path, NULL, 0, i};
        JsonNode *src = (JsonNode *)(intptr_t)((JsonVec *)((JsonNode *)(intptr_t)slots[0])->a)
                            ->slots[i];
        int64_t v = jn_value(src, td->elem, &ep);
        rt_arr_set((Arr *)(intptr_t)slots[1], i, v, td->elem);
    }

    int64_t out = slots[1];
    rt_gc_top = f.prev;
    return out;
}

static int64_t jn_map(JsonNode *n, TypeDesc *td, const JPath *path) {
    JsonVec *keys = n->a;
    int64_t len = keys ? keys->len : 0;

    TypeDesc *tds[3] = {&rt_td_json, td, td->field_types[1]};
    int64_t slots[3] = {(int64_t)(intptr_t)n, 0, 0};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;

    slots[1] = (int64_t)(intptr_t)rt_map_new();
    for (int64_t i = 0; i < len; i++) {
        JsonNode *node = (JsonNode *)(intptr_t)slots[0];
        Str *k = (Str *)(intptr_t)((JsonVec *)node->a)->slots[i];
        JPath kp = {path, k->data, k->len, 0};
        JsonNode *src = (JsonNode *)(intptr_t)((JsonVec *)node->b)->slots[i];
        slots[2] = jn_value(src, td->field_types[1], &kp);
        // Read the key again. The allocation of the value can collect, and the
        // key is reachable only through the rooted node.
        node = (JsonNode *)(intptr_t)slots[0];
        Str *key = (Str *)(intptr_t)((JsonVec *)node->a)->slots[i];
        rt_map_set((Map *)(intptr_t)slots[1], (int64_t)(intptr_t)key, slots[2], td);
        slots[2] = 0;
    }

    int64_t out = slots[1];
    rt_gc_top = f.prev;
    return out;
}

static int64_t jn_record(JsonNode *n, TypeDesc *td, const JPath *path) {
    JsonVec *keys = n->a;
    int64_t len = keys ? keys->len : 0;

    TypeDesc *tds[3] = {&rt_td_json, td, &td_byte_array};
    int64_t slots[3] = {(int64_t)(intptr_t)n, 0, 0};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;
    slots[1] = (int64_t)(intptr_t)rt_rec_new(td);

    // A wide record takes `seen` from the GC heap, as jp_record does, because
    // jp_fail's longjmp would leak a malloc.
    char small[64];
    char *seen;
    if (td->nfields <= (int64_t)sizeof small) {
        seen = small;
    } else {
        Arr *bk = rt_arr_new(td->nfields, 1);
        slots[2] = (int64_t)(intptr_t)bk;
        seen = (char *)rt_arr_bytes(bk);
    }
    memset(seen, 0, (size_t)td->nfields);

    if (td->rest) {
        JsonNode *rest = rt_json_empty(JSON_OBJECT);
        rt_rec_set((void *)(intptr_t)slots[1], td->rest - 1,
                   (int64_t)(intptr_t)rest, td);
    }

    for (int64_t i = 0; i < len; i++) {
        JsonNode *node = (JsonNode *)(intptr_t)slots[0];
        Str *key = (Str *)(intptr_t)((JsonVec *)node->a)->slots[i];

        int64_t idx = -1;
        for (int64_t k = 0; k < td->nfields; k++) {
            if (json_field_is_rest(td, k)) continue;
            const char *fn = td->field_names[k];
            size_t fl = strlen(fn);
            if ((int64_t)fl == key->len && memcmp(fn, key->data, fl) == 0) {
                idx = k;
                break;
            }
        }

        JsonNode *src = (JsonNode *)(intptr_t)((JsonVec *)node->b)->slots[i];
        if (idx < 0 || !json_has_form(td->field_types[idx])) {
            // The three policies of §6.3: preserve, refuse, or drop.
            if (td->rest) {
                // Use a copy of the node, because `a as T` must not alias `a`.
                JsonNode *copy = jn_copy(src);
                node = (JsonNode *)(intptr_t)slots[0];
                key = (Str *)(intptr_t)((JsonVec *)node->a)->slots[i];
                int64_t restRaw = rt_rec_get((void *)(intptr_t)slots[1],
                                             td->rest - 1, td);
                jp_rest_put((JsonNode *)(intptr_t)restRaw, key, copy);
            } else if (jp_strict) {
                char what[128];
                snprintf(what, sizeof what, "unknown key \"%.*s\"", (int)key->len, key->data);
                jp_fail(path, what);
            }
            continue;
        }

        JPath fp = {path, td->field_names[idx], -1, 0};
        int64_t v = jn_value(src, td->field_types[idx], &fp);
        rt_rec_set((void *)(intptr_t)slots[1], idx, v, td);
        seen[idx] = 1;
    }

    for (int64_t i = 0; i < td->nfields; i++) {
        if (seen[i] || json_field_is_rest(td, i)) continue;
        TypeDesc *ft = td->field_types[i];
        if (ft->kind == TD_OPTIONAL || !json_has_form(ft)) continue;
        JPath fp = {path, td->field_names[i], -1, 0};
        jp_fail(&fp, "missing field");
    }

    int64_t out = slots[1];
    rt_gc_top = f.prev;
    return out;
}

static int64_t jn_value(JsonNode *n, TypeDesc *td, const JPath *path) {
    int64_t k = rt_json_kind(n);

    // Only an optional can take a JSON null, and an absent key reads the same
    // way (§6.3). A Json takes both, because both are shapes of a Json.
    if (td->kind != TD_OPTIONAL && td->kind != TD_JSON &&
        (k == JSON_NULL || k == JSON_MISSING)) {
        jp_fail(path, k == JSON_MISSING ? "missing field"
                                        : "unexpected null (the type is not optional)");
    }

    switch (td->kind) {
    case TD_OPTIONAL: {
        if (k == JSON_NULL || k == JSON_MISSING) return 0;
        int64_t inner = jn_value(n, td->elem, path);
        TypeDesc *tds[1] = {td->elem};
        int64_t slots[1] = {inner};
        GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
        rt_gc_top = &f;
        void *box = rt_box(slots[0], td->elem);
        rt_gc_top = f.prev;
        return (int64_t)(intptr_t)box;
    }
    case TD_BOOL:
        if (k != JSON_BOOL) jn_wrong(path, "true or false", n);
        return n->num;
    case TD_INT8:
    case TD_INT16:
    case TD_INT32:
    case TD_INT:
    case TD_DURATION:
    case TD_ENUM: {
        if (k != JSON_NUMBER) jn_wrong(path, "a number", n);
        int64_t v;
        if (n->isint == JN_UINT) {
            // The node holds a number past an int64's range.
            if (n->num < 0) jp_fail(path, "out of range for a signed integer");
            v = n->num;
        } else if (n->isint) {
            v = n->num;
        } else {
            // Test the value and not the spelling: 1e3 is a whole number.
            double d = jn_double(n);
            if (!(d >= -9223372036854775808.0 && d < 9223372036854775808.0) ||
                d != (double)(int64_t)d) {
                jp_fail(path, "expected a whole number");
            }
            v = (int64_t)d;
        }
        switch (td->kind) {
        case TD_INT8:
            if (v < -128 || v > 127) jp_fail(path, "int8 out of range");
            break;
        case TD_INT16:
            if (v < -32768 || v > 32767) jp_fail(path, "int16 out of range");
            break;
        case TD_INT32:
            if (v < -2147483648LL || v > 2147483647LL) jp_fail(path, "int32 out of range");
            break;
        default:
            break;
        }
        return v;
    }
    case TD_UINT8:
    case TD_UINT16:
    case TD_UINT32:
    case TD_UINT: {
        if (k != JSON_NUMBER) jn_wrong(path, "a number", n);
        uint64_t v;
        if (n->isint == JN_UINT) {
            v = (uint64_t)n->num;
        } else if (n->isint) {
            if (n->num < 0) jp_fail(path, uint_range_message(td->kind));
            v = (uint64_t)n->num;
        } else {
            double d = jn_double(n);
            if (!(d >= 0 && d < UINT64_LIMIT)) jp_fail(path, uint_range_message(td->kind));
            v = (uint64_t)d;
            if ((double)v != d) jp_fail(path, "expected a whole number");
        }
        if (v > uint_kind_max(td->kind)) jp_fail(path, uint_range_message(td->kind));
        return (int64_t)v;
    }
    case TD_FLOAT:
    case TD_FLOAT32: {
        if (k != JSON_NUMBER) jn_wrong(path, "a number", n);
        double d = jn_double(n);
        if (td->kind == TD_FLOAT32) {
            float f = (float)d;
            if (isinf(f) && !isinf(d)) jp_fail(path, "float32 out of range");
            d = (double)f;
        }
        int64_t bits;
        memcpy(&bits, &d, 8);
        return bits;
    }
    case TD_STRING:
        if (k != JSON_STRING) jn_wrong(path, "a string", n);
        // Shared and not copied. A Str is immutable, so the typed value and the
        // tree cannot change each other through it.
        return (int64_t)(intptr_t)n->a;
    case TD_DATETIME: {
        if (k != JSON_STRING) jn_wrong(path, "a date string", n);
        Str *s = n->a;
        int64_t ms = 0;
        if (s->len >= 64 || !rt_date_parse_iso(s->data, s->len, &ms)) {
            jp_fail(path, "expected a date string (\"YYYY-MM-DD\" or \"YYYY-MM-DDTHH:MM:SSZ\")");
        }
        return ms;
    }
    case TD_ARRAY:
        if (k != JSON_ARRAY) jn_wrong(path, "an array", n);
        return jn_array(n, td, path);
    case TD_MAP:
        if (k != JSON_OBJECT) jn_wrong(path, "an object", n);
        return jn_map(n, td, path);
    case TD_RECORD:
        if (k != JSON_OBJECT) jn_wrong(path, "an object", n);
        return jn_record(n, td, path);
    case TD_UNION: {
        char cls = 0;
        if (k == JSON_STRING) cls = 's';
        else if (k == JSON_NUMBER) cls = 'n';
        else if (k == JSON_BOOL) cls = 'b';
        else if (k == JSON_ARRAY) cls = 'a';
        else if (k == JSON_OBJECT) cls = 'o';
        TypeDesc *m = cls ? jp_union_pick(td, cls) : NULL;
        if (!m) jp_union_fail(td, path);
        // The source node roots the subtree across the allocation of the
        // member.
        TypeDesc *tds[2] = {&rt_td_json, m};
        int64_t slots[2] = {(int64_t)(intptr_t)n, 0};
        GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
        rt_gc_top = &f;
        slots[1] = (int64_t)(intptr_t)rt_rec_new(m);
        int64_t v = jn_value((JsonNode *)(intptr_t)slots[0], m->field_types[0], path);
        rt_rec_set((void *)(intptr_t)slots[1], 0, v, m);
        int64_t out = slots[1];
        rt_gc_top = f.prev;
        return out;
    }
    case TD_JSON:
        // A Json target takes a copy of the subtree, so a change to the result
        // does not change the source, and the reverse.
        return (int64_t)(intptr_t)jn_copy(n);
    default:
        jp_fail(path, "this type has no JSON form");
        return 0;
    }
}

// `expr as T` where expr is already a tree (§3.6).
int64_t rt_json_from_node(JsonNode *v, TypeDesc *td, int64_t strict, void **err) {
    JP_GUARD(err, 0)
    jp_strict = strict ? 1 : 0;

    TypeDesc *tds[1] = {&rt_td_json};
    int64_t slots[1] = {(int64_t)(intptr_t)v};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t out = jn_value((JsonNode *)(intptr_t)slots[0], td, NULL);

    rt_gc_top = f.prev;
    JP_UNGUARD()
    return out;
}
