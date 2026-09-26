// Core runtime, linked into every binary. runtime.h holds the shared
// declarations and the codegen contract. Each standard library module lives
// under lib/ and links only when imported. Memory management is in gc.c.
//
// A function that holds a heap pointer across an allocation must root it in a
// GCFrame of its own. C locals and parameters are not roots. Do not suppress
// the collection with rt_gc_disable instead: a program whose only allocation
// site is in a disabled region never collects.

#include "runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The C library has no portable monotonic clock or sleep. Windows has them in
// the system API.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

// ---- panic ----

// Prints the call chain from the shadow stack of the collector.
//
// The chain can be partial. A function that holds no pointer pushes no frame,
// so it cannot be named, and the chain can have a gap. A frame in every
// function measured 2x slower on a recursive scalar function.
//
// Only named frames appear. A runtime helper pushes a frame to hold a pointer
// across an allocation and leaves name NULL, so it is skipped. An async step
// function has no shadow frame, because its locals live in a heap frame that
// the scheduler owns. An async call chain shows only the callers of the
// scheduler.
//
// NIO_NO_TRACE=1 turns it off, for a test that compares stderr byte for byte.
#define TRACE_MAX 64

static void print_trace(void) {
    const char *off = getenv("NIO_NO_TRACE");
    if (off && off[0] == '1') return;

    int64_t named = 0;
    for (GCFrame *f = rt_gc_top; f; f = f->prev)
        if (f->name) named++;
    if (named == 0) return;

    fprintf(stderr, "\nin:\n");
    int64_t n = 0;
    for (GCFrame *f = rt_gc_top; f; f = f->prev) {
        if (!f->name) continue;
        if (n == TRACE_MAX) {
            fprintf(stderr, "  ... %lld more\n", (long long)(named - n));
            break;
        }
        fprintf(stderr, "  %s\n", f->name);
        n++;
    }
}

void rt_panic(const char *msg) {
    fprintf(stderr, "runtime error: %s\n", msg);
    print_trace();
    exit(1);
}

void rt_error_abort(void *err) {
    Str *msg = (Str *)(intptr_t)rt_rec_get(err, 0, &rt_td_error);
    fprintf(stderr, "runtime error: %.*s\n", (int)msg->len, msg->data);
    print_trace();
    exit(1);
}

// Allocates a zeroed record and stamps its descriptor into slot [0] (REC_TD,
// runtime.h), so the collector reads the value's own type out of the object.
void *rt_rec_new(TypeDesc *td) {
    if (td->size < 8 || (td->nfields > 0 && !td->field_offsets))
        rt_panic("record TypeDesc without field offsets (runtime.h)");
    void *p = rt_alloc(td->size);
    REC_TD(p) = td;
    return p;
}

// Builds the two-slot Error record of §2.9.
void *rt_error_new(const char *msg, int64_t code) {
    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {0};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    slots[0] = (int64_t)(intptr_t)rt_str_from_c(msg);
    void *err = rt_rec_new(&rt_td_error);
    rt_rec_set(err, 0, slots[0], &rt_td_error);
    rt_rec_set(err, 1, code, &rt_td_error);

    rt_gc_top = f.prev;
    return err;
}

// Maps a platform errno onto the NIO_ERR_* set (runtime.h). An unknown errno
// becomes NIO_ERR_OTHER, so no raw number escapes.
int64_t rt_err_from_errno(int e) {
    switch (e) {
    case ENOENT:
        return NIO_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:
        return NIO_ERR_PERMISSION;
    case EEXIST:
        return NIO_ERR_EXISTS;
    case ENOTDIR:
        return NIO_ERR_NOT_DIRECTORY;
    case EISDIR:
        return NIO_ERR_IS_DIRECTORY;
#ifdef ENOTEMPTY
    case ENOTEMPTY:
        return NIO_ERR_NOT_EMPTY;
#endif
    case EINVAL:
        return NIO_ERR_INVALID;
    case EIO:
        return NIO_ERR_IO;
    case ENOSPC:
        return NIO_ERR_NO_SPACE;
    case EMFILE:
    case ENFILE:
        return NIO_ERR_TOO_MANY_FILES;
    case ENAMETOOLONG:
        return NIO_ERR_NAME_TOO_LONG;
    case EINTR:
        return NIO_ERR_INTERRUPTED;
#ifdef ELOOP
    case ELOOP:
        return NIO_ERR_LOOP;
#endif
    case EROFS:
        return NIO_ERR_READ_ONLY;
    // The network codes, for lib/net.c. Each guard lets this file compile on a
    // host whose C library does not define the name.
#ifdef ECONNREFUSED
    case ECONNREFUSED:
        return NIO_ERR_CONNECTION_REFUSED;
#endif
#ifdef ECONNRESET
    case ECONNRESET:
        return NIO_ERR_CONNECTION_RESET;
#endif
#ifdef ECONNABORTED
    case ECONNABORTED:
        return NIO_ERR_CONNECTION_ABORTED;
#endif
#ifdef ENOTCONN
    case ENOTCONN:
        return NIO_ERR_NOT_CONNECTED;
#endif
#ifdef EISCONN
    case EISCONN:
        return NIO_ERR_ALREADY_CONNECTED;
#endif
#ifdef EADDRINUSE
    case EADDRINUSE:
        return NIO_ERR_ADDRESS_IN_USE;
#endif
#ifdef EADDRNOTAVAIL
    case EADDRNOTAVAIL:
        return NIO_ERR_ADDRESS_NOT_AVAILABLE;
#endif
#ifdef ENETUNREACH
    case ENETUNREACH:
        return NIO_ERR_NETWORK_UNREACHABLE;
#endif
#ifdef EHOSTUNREACH
    case EHOSTUNREACH:
        return NIO_ERR_HOST_UNREACHABLE;
#endif
#ifdef EPIPE
    case EPIPE:
        return NIO_ERR_BROKEN_PIPE;
#endif
#ifdef EMSGSIZE
    case EMSGSIZE:
        return NIO_ERR_MESSAGE_TOO_LONG;
#endif
#ifdef ETIMEDOUT
    case ETIMEDOUT:
        return NIO_ERR_TIMED_OUT;
#endif
    default:
        return NIO_ERR_OTHER;
    }
}

// Integer division has no runtime entry point. Codegen emits the divide and
// its guards in place.

// ---- the command line ----

// The command line exists only as the parameters of main, so generated main
// calls rt_args_init first, whether or not the program imports 'process'.
int rt_argc = 0;
char **rt_argv = NULL;

void rt_args_init(int argc, char **argv) {
    rt_argc = argc;
    rt_argv = argv;
#if defined(_WIN32)
    // The CRT opens the standard streams in text mode, which writes \n as
    // \r\n, reads \r\n as \n and ends stdin at a Ctrl-Z. A program's output
    // and input are the same bytes on every host.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

// ---- strings (immutable, length-prefixed) ----

Str *rt_str_alloc(int64_t len) {
    Str *s = rt_alloc((int64_t)sizeof(Str) + len);
    s->len = len;
    return s;
}

Str *rt_str_from_c(const char *c) {
    int64_t n = (int64_t)strlen(c);
    Str *s = rt_str_alloc(n);
    memcpy(s->data, c, (size_t)n);
    return s;
}

Str *rt_str_concat(Str *a, Str *b) {
    TypeDesc *tds[2] = {&rt_td_string, &rt_td_string};
    int64_t slots[2] = {(int64_t)(intptr_t)a, (int64_t)(intptr_t)b};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    Str *s = rt_str_alloc(a->len + b->len);
    memcpy(s->data, a->data, (size_t)a->len);
    memcpy(s->data + a->len, b->data, (size_t)b->len);

    rt_gc_top = f.prev;
    return s;
}

// A chain of `+` in one call. parts must point at n consecutive slots of the
// caller's GC frame (runtime.h): the operands are roots already, so this
// function pushes no frame of its own.
Str *rt_str_concat_n(Str **parts, int64_t n) {
    int64_t total = 0;
    for (int64_t i = 0; i < n; i++) total += parts[i]->len;

    Str *s = rt_str_alloc(total);
    char *out = s->data;
    for (int64_t i = 0; i < n; i++) {
        memcpy(out, parts[i]->data, (size_t)parts[i]->len);
        out += parts[i]->len;
    }
    return s;
}

// The byte at i, zero-extended, because `s[i]` is a `byte` (a uint8).
int64_t rt_str_byte(Str *s, int64_t i) {
    if (i < 0 || i >= s->len) {
        char msg[96];
        snprintf(msg, sizeof msg, "index %lld out of range (string length %lld)",
                 (long long)i, (long long)s->len);
        rt_panic(msg);
    }
    return (int64_t)(uint8_t)s->data[i];
}

int64_t rt_str_eq(Str *a, Str *b) {
    return a->len == b->len && memcmp(a->data, b->data, (size_t)a->len) == 0;
}

int64_t rt_str_cmp(Str *a, Str *b) {
    int64_t min = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->data, b->data, (size_t)min);
    if (c != 0) return c < 0 ? -1 : 1;
    if (a->len == b->len) return 0;
    return a->len < b->len ? -1 : 1;
}

// ---- arrays (packed elements) ----

// The width §5.7 gives each element type. Every kind not named here is a
// pointer or a number at the 8-byte unit.
int64_t rt_arr_width(int64_t elem_kind) {
    switch (elem_kind) {
    case TD_BOOL:
    case TD_INT8:
    case TD_UINT8:  return 1;
    case TD_INT16:
    case TD_UINT16: return 2;
    case TD_INT32:
    case TD_UINT32:
    case TD_FLOAT32: return 4;
    default: return 8;
    }
}

// Push is in the core because lib/json.c grows an array through it. One library
// cannot depend on another, because the imports decide what links.
void rt_arr_push(Arr *a, int64_t v, TypeDesc *arr_td) {
    if (a->len == a->cap) {
        TypeDesc *tds[2] = {arr_td, arr_td->elem};
        int64_t slots[2] = {(int64_t)(intptr_t)a, v};
        GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
        rt_gc_top = &f;

        int64_t ncap = a->cap < 4 ? 4 : a->cap * 2;
        void *ns = rt_alloc(ncap * a->width);
        memcpy(ns, a->data, (size_t)a->len * a->width);
        a->data = ns;
        a->cap = ncap;

        rt_gc_top = f.prev;
    }
    rt_arr_set(a, a->len, v, arr_td->elem);
    a->len++;
}

Arr *rt_arr_new(int64_t len, int64_t width) {
    if (len < 0) rt_panic("negative array length");
    // The header roots as an opaque block across the second allocation. The
    // collector cannot walk it yet, because its elements hold no value.
    TypeDesc *tds[1] = {&rt_td_block};
    int64_t slots[1] = {0};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *a = rt_alloc(sizeof(Arr));
    slots[0] = (int64_t)(intptr_t)a;
    a->len = len;
    a->cap = len > 0 ? len : 1;
    a->width = width;
    a->data = rt_alloc(a->cap * width);

    rt_gc_top = f.prev;
    return a;
}

int64_t rt_arr_len(Arr *a) { return a->len; }

void *rt_arr_slot(Arr *a, int64_t i) {
    if (i < 0 || i >= a->len) {
        char msg[96];
        snprintf(msg, sizeof msg, "index %lld out of range (array length %lld)",
                 (long long)i, (long long)a->len);
        rt_panic(msg);
    }
    return (char *)a->data + i * a->width;
}

// The 8-byte unit of the value of kind `kind` at p. Packing (§5.7) adds this
// one conversion, and arrays and records share it, so an int8 reads the same
// out of both. A narrow integer extends by its family (§2.1). A float32 widens
// from the binary32 that holds it. Every other kind is already the unit.
static int64_t load_unit(const void *p, int64_t kind) {
    switch (kind) {
    case TD_BOOL:
    case TD_UINT8:  return (int64_t)*(const uint8_t *)p;
    case TD_INT8:   return (int64_t)*(const int8_t *)p;
    case TD_UINT16: return (int64_t)*(const uint16_t *)p;
    case TD_INT16:  return (int64_t)*(const int16_t *)p;
    case TD_UINT32: return (int64_t)*(const uint32_t *)p;
    case TD_INT32:  return (int64_t)*(const int32_t *)p;
    case TD_FLOAT32: {
        float f;
        memcpy(&f, p, sizeof f);
        double d = (double)f;
        int64_t bits;
        memcpy(&bits, &d, sizeof bits);
        return bits;
    }
    default: {
        int64_t v;
        memcpy(&v, p, sizeof v);
        return v;
    }
    }
}

// The reverse. A narrow integer keeps its low bits, which is the wrap of §3.3.
// A float32 rounds to binary32.
static void store_unit(void *p, int64_t raw, int64_t kind) {
    switch (kind) {
    case TD_BOOL:
    case TD_UINT8:
    case TD_INT8:   *(uint8_t *)p = (uint8_t)raw; break;
    case TD_UINT16:
    case TD_INT16:  *(uint16_t *)p = (uint16_t)raw; break;
    case TD_UINT32:
    case TD_INT32:  *(uint32_t *)p = (uint32_t)raw; break;
    case TD_FLOAT32: {
        double d;
        memcpy(&d, &raw, sizeof d);
        float f = (float)d;
        memcpy(p, &f, sizeof f);
        break;
    }
    default: memcpy(p, &raw, sizeof raw); break;
    }
}

int64_t rt_arr_get(Arr *a, int64_t i, TypeDesc *elem) {
    return load_unit((const char *)a->data + i * a->width, elem->kind);
}

void rt_arr_set(Arr *a, int64_t i, int64_t raw, TypeDesc *elem) {
    store_unit((char *)a->data + i * a->width, raw, elem->kind);
}

int64_t rt_rec_get(void *rec, int64_t i, TypeDesc *td) {
    return load_unit((char *)rec + td->field_offsets[i], td->field_types[i]->kind);
}

void rt_rec_set(void *rec, int64_t i, int64_t raw, TypeDesc *td) {
    store_unit((char *)rec + td->field_offsets[i], raw, td->field_types[i]->kind);
}

// A box is one value at its own width (runtime.h). The caller must root raw
// when it is a pointer, because the allocation below can collect.
void *rt_box(int64_t raw, TypeDesc *inner) {
    void *p = rt_alloc(rt_arr_width(inner->kind));
    store_unit(p, raw, inner->kind);
    return p;
}

int64_t rt_box_get(const void *box, const TypeDesc *inner) {
    return load_unit(box, inner->kind);
}

// ---- maps, which keep insertion order ----
//
// runtime.h describes the layout. Maps are core because indexing and map
// literals need no import, and lib/json.c builds a map for a program that
// never imports 'map'.

// The key and value descriptors of a Map<K, V>. TD_MAP keeps them in the
// record slots (runtime.h).
#define MAP_KEY_TD(td) ((td)->field_types[0])
#define MAP_VAL_TD(td) ((td)->field_types[1])

static uint64_t map_hash(int64_t key, const TypeDesc *ktd) {
    if (ktd->kind == TD_STRING) {
        // FNV-1a over the bytes.
        const Str *s = (const Str *)(intptr_t)key;
        uint64_t h = 1469598103934665603ull;
        for (int64_t i = 0; i < s->len; i++) {
            h ^= (unsigned char)s->data[i];
            h *= 1099511628211ull;
        }
        return h;
    }
    // Every other key type is an int64 scalar. The splitmix64 step spreads
    // keys that follow each other across the table.
    uint64_t h = (uint64_t)key + 0x9e3779b97f4a7c15ull;
    h = (h ^ (h >> 30)) * 0xbf58476d1ce4e5b9ull;
    h = (h ^ (h >> 27)) * 0x94d049bb133111ebull;
    return h ^ (h >> 31);
}

static int map_key_eq(int64_t a, int64_t b, const TypeDesc *ktd) {
    if (ktd->kind == TD_STRING) return rt_str_eq((Str *)(intptr_t)a, (Str *)(intptr_t)b) != 0;
    return a == b;
}

Map *rt_map_new(void) {
    return rt_alloc(sizeof(Map)); // zeroed = empty
}

int64_t rt_map_len(Map *m) { return m->len; }

// The address of the key or the value of entry i. The blocks pack at the
// component width (§5.7). The hash and the probe always work on the loaded
// unit, so a key hashes the same at every width.
static void *map_kslot(Map *m, int64_t i, const TypeDesc *ktd) {
    return (char *)m->keys + i * rt_arr_width(ktd->kind);
}

static void *map_vslot(Map *m, int64_t i, const TypeDesc *vtd) {
    return (char *)m->vals + i * rt_arr_width(vtd->kind);
}

int64_t rt_map_key_at(Map *m, int64_t i, TypeDesc *ktd) {
    return load_unit(map_kslot(m, i, ktd), ktd->kind);
}

int64_t rt_map_val_at(Map *m, int64_t i, TypeDesc *vtd) {
    return load_unit(map_vslot(m, i, vtd), vtd->kind);
}

int64_t rt_map_find(Map *m, int64_t key, TypeDesc *ktd) {
    if (m->icap == 0) return -1;
    uint64_t mask = (uint64_t)m->icap - 1;
    uint64_t i = map_hash(key, ktd) & mask;
    for (;;) {
        int64_t slot = m->index[i];
        if (slot == 0) return -1;
        if (map_key_eq(rt_map_key_at(m, slot - 1, ktd), key, ktd)) return slot - 1;
        i = (i + 1) & mask;
    }
}

// Puts the entry at position pos into the index. The caller must know that the
// index does not hold the key and that the table has a free slot.
static void map_index_add(Map *m, int64_t pos, TypeDesc *ktd) {
    uint64_t mask = (uint64_t)m->icap - 1;
    uint64_t i = map_hash(rt_map_key_at(m, pos, ktd), ktd) & mask;
    while (m->index[i] != 0) i = (i + 1) & mask;
    m->index[i] = pos + 1;
}

void rt_map_reindex(Map *m, TypeDesc *ktd) {
    if (m->icap == 0) return;
    memset(m->index, 0, (size_t)m->icap * 8);
    for (int64_t p = 0; p < m->len; p++) map_index_add(m, p, ktd);
}

// Makes room for one more entry. It allocates, so the caller must root the
// map, the key and the value. The map stays a valid value of its descriptor
// through the growth, because len counts only the entries already in place.
static void map_grow(Map *m, TypeDesc *map_td) {
    TypeDesc *ktd = MAP_KEY_TD(map_td);
    if (m->len == m->cap) {
        int64_t kw = rt_arr_width(ktd->kind);
        int64_t vw = rt_arr_width(MAP_VAL_TD(map_td)->kind);
        int64_t ncap = m->cap ? m->cap * 2 : 4;
        void *nk = rt_alloc(ncap * kw);
        memcpy(nk, m->keys, (size_t)(m->len * kw));
        m->keys = nk;
        void *nv = rt_alloc(ncap * vw);
        memcpy(nv, m->vals, (size_t)(m->len * vw));
        m->vals = nv;
        m->cap = ncap;
    }
    if ((m->len + 1) * 3 > m->icap * 2) {
        int64_t nicap = m->icap ? m->icap * 2 : 8;
        m->index = rt_alloc(nicap * 8);
        m->icap = nicap;
        rt_map_reindex(m, ktd);
    }
}

void rt_map_set(Map *m, int64_t key, int64_t val, TypeDesc *map_td) {
    TypeDesc *ktd = MAP_KEY_TD(map_td);
    TypeDesc *vtd = MAP_VAL_TD(map_td);
    int64_t pos = rt_map_find(m, key, ktd);
    if (pos >= 0) {
        store_unit(map_vslot(m, pos, vtd), val, vtd->kind); // overwrite; no allocation
        return;
    }
    TypeDesc *tds[3] = {map_td, ktd, vtd};
    int64_t slots[3] = {(int64_t)(intptr_t)m, key, val};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;
    map_grow(m, map_td);
    rt_gc_top = f.prev;

    store_unit(map_kslot(m, m->len, ktd), key, ktd->kind);
    store_unit(map_vslot(m, m->len, vtd), val, vtd->kind);
    m->len++;
    map_index_add(m, m->len - 1, ktd);
}

void *rt_map_get(Map *m, int64_t key, TypeDesc *map_td) {
    int64_t pos = rt_map_find(m, key, MAP_KEY_TD(map_td));
    if (pos < 0) return NULL;
    // A lookup gives V?, so the value goes in a box at its own width.
    TypeDesc *vtd = MAP_VAL_TD(map_td);
    TypeDesc *tds[1] = {map_td};
    int64_t slots[1] = {(int64_t)(intptr_t)m};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;
    void *box = rt_box(rt_map_val_at(m, pos, vtd), vtd);
    rt_gc_top = f.prev;
    return box;
}

// ---- type descriptor singletons ----

TypeDesc rt_td_int8 = {TD_INT8, 0, 0, 0, 0, 0};
TypeDesc rt_td_int16 = {TD_INT16, 0, 0, 0, 0, 0};
TypeDesc rt_td_int32 = {TD_INT32, 0, 0, 0, 0, 0};
TypeDesc rt_td_int = {TD_INT, 0, 0, 0, 0, 0};
TypeDesc rt_td_uint8 = {TD_UINT8, 0, 0, 0, 0, 0};
TypeDesc rt_td_uint16 = {TD_UINT16, 0, 0, 0, 0, 0};
TypeDesc rt_td_uint32 = {TD_UINT32, 0, 0, 0, 0, 0};
TypeDesc rt_td_uint = {TD_UINT, 0, 0, 0, 0, 0};
TypeDesc rt_td_float32 = {TD_FLOAT32, 0, 0, 0, 0, 0};
TypeDesc rt_td_float = {TD_FLOAT, 0, 0, 0, 0, 0};
TypeDesc rt_td_string = {TD_STRING, 0, 0, 0, 0, 0};
TypeDesc rt_td_bool = {TD_BOOL, 0, 0, 0, 0, 0};
TypeDesc rt_td_datetime = {TD_DATETIME, 0, 0, 0, 0, 0};
TypeDesc rt_td_duration = {TD_DURATION, 0, 0, 0, 0, 0};
TypeDesc rt_td_block = {TD_BLOCK, 0, 0, 0, 0, 0};
TypeDesc rt_td_func = {TD_FUNC, 0, 0, 0, 0, 0};
TypeDesc rt_td_future = {TD_FUTURE, 0, 0, 0, 0, 0};

// One descriptor covers every Json, because a node describes itself: the
// collector reads the shape from the kind of the node (gc.c). This descriptor
// and the two below are core because gc.c names them.
TypeDesc rt_td_json = {TD_JSON, 0, 0, 0, 0, 0};

// One descriptor covers every RegExp. The compiled block holds no pointer, so
// to trace it is to mark it (§6.14).
TypeDesc rt_td_regexp = {TD_REGEXP, 0, 0, 0, 0, 0};

// One descriptor covers every Socket and Listener, which are one block and
// differ only in what the checker permits with them (§6.15).
TypeDesc rt_td_socket = {TD_SOCKET, 0, 0, 0, 0, 0};

// The built-in Error record (§2.9), for the error a future carries. Codegen
// emits its own descriptor wherever an Error is a language value.
//
// The offsets are the fold of §5.7 written out: a pointer at 8, an int at 16,
// and 24 in all. They must match what codegen computes for types.ErrorT.
static const char *rt_error_field_names[2] = {"message", "code"};
static TypeDesc *rt_error_field_types[2] = {&rt_td_string, &rt_td_int};
static const int64_t rt_error_field_offsets[2] = {8, 16};
TypeDesc rt_td_error = {TD_RECORD, 0,    2, rt_error_field_names,
                        rt_error_field_types, 0, rt_error_field_offsets, 24};

// ---- futures, for async and await ----
//
// An async body is a compiled state machine (runtime.h) and this code is their
// scheduler. There is one ready queue, in arrival order. A step of a task
// either completes it, which requeues its waiters and runs its callbacks, or
// parks it on the future that it awaits. rt_await drives the scheduler until
// its future is complete, and the last rt_async_drain in main runs the rest.

Future *rt_async_ready = NULL;
static Future *ready_tail = NULL;
static int64_t live_tasks = 0; // spawned (or sleeping) but not yet completed

// The pending timers: a binary heap ordered by deadline and then by seq. seq
// is the order of creation, so two equal deadlines complete in that order.
Future **rt_async_timer_heap = NULL;
int64_t rt_async_timer_count = 0;
static int64_t timer_cap = 0;
static int64_t timer_seq = 0;

static int timer_before(Future *a, Future *b) {
    if (a->deadline != b->deadline) return a->deadline < b->deadline;
    return a->seq < b->seq;
}

static void timer_push(Future *fu) {
    if (rt_async_timer_count == timer_cap) {
        timer_cap = timer_cap ? timer_cap * 2 : 64;
        rt_async_timer_heap = realloc(rt_async_timer_heap, (size_t)timer_cap * sizeof(Future *));
        if (!rt_async_timer_heap) rt_panic("out of memory");
    }
    int64_t i = rt_async_timer_count++;
    while (i > 0) {
        int64_t parent = (i - 1) / 2;
        if (timer_before(rt_async_timer_heap[parent], fu)) break;
        rt_async_timer_heap[i] = rt_async_timer_heap[parent];
        i = parent;
    }
    rt_async_timer_heap[i] = fu;
}

static Future *timer_pop(void) {
    Future *min = rt_async_timer_heap[0];
    Future *last = rt_async_timer_heap[--rt_async_timer_count];
    int64_t i = 0;
    for (;;) {
        int64_t child = 2 * i + 1;
        if (child >= rt_async_timer_count) break;
        if (child + 1 < rt_async_timer_count &&
            timer_before(rt_async_timer_heap[child + 1], rt_async_timer_heap[child])) {
            child++;
        }
        if (timer_before(last, rt_async_timer_heap[child])) break;
        rt_async_timer_heap[i] = rt_async_timer_heap[child];
        i = child;
    }
    rt_async_timer_heap[i] = last;
    return min;
}

// A clock that only moves forward. A change to the wall clock must not make a
// sleep end early or last forever. lib/net.c times its deadlines on this same
// clock, because a timer can limit one of them.
int64_t rt_mono_ms(void) {
#if defined(_WIN32)
    // The two divisions keep the product inside 64 bits at any uptime.
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (c.QuadPart / f.QuadPart) * 1000 + (c.QuadPart % f.QuadPart) * 1000 / f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

// Sleeps for ms. A signal does not end it early: both callers wait for a
// deadline.
static void sleep_ms(int64_t ms) {
    if (ms <= 0) return;
#if defined(_WIN32)
    while (ms > 0) {
        DWORD step = ms > 0x7fffffff ? 0x7fffffff : (DWORD)ms;
        Sleep(step);
        ms -= step;
    }
#else
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) != 0) {
    }
#endif
}

static void ready_push(Future *t) {
    t->qnext = NULL;
    if (ready_tail) {
        ready_tail->qnext = t;
    } else {
        rt_async_ready = t;
    }
    ready_tail = t;
}

static Future *ready_pop(void) {
    Future *t = rt_async_ready;
    if (!t) return NULL;
    rt_async_ready = t->qnext;
    if (!rt_async_ready) ready_tail = NULL;
    t->qnext = NULL;
    return t;
}

Future *rt_async_spawn(int64_t (*step)(void *), void *frame,
                       TypeDesc *frame_td, TypeDesc *result_td) {
    // The caller stored the arguments and rt_alloc cleared the rest, so the
    // frame is already a valid value of frame_td.
    TypeDesc *tds[1] = {frame_td};
    int64_t slots[1] = {(int64_t)(intptr_t)frame};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Future *fu = rt_alloc(sizeof(Future));
    fu->state = FUT_PENDING;
    fu->step = step;
    fu->frame = frame;
    fu->frame_td = frame_td;
    fu->result_td = result_td;
    ready_push(fu);
    live_tasks++;

    rt_gc_top = f.prev;
    return fu;
}

void rt_future_call_cb(Future *fu, void *clos) {
    // The closure contract of codegen: slot 0 holds the code pointer and the
    // block itself is the hidden first argument. The result descriptor chooses
    // the prototype, so a double travels in a float register.
    if (!clos) rt_panic("called a function value that was never assigned");
    void *code;
    memcpy(&code, clos, 8);

    TypeDesc *tds[2] = {&rt_td_func, &rt_td_future};
    int64_t slots[2] = {(int64_t)(intptr_t)clos, (int64_t)(intptr_t)fu};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    if (!fu->result_td) {
        ((void (*)(void *))code)(clos);
    } else if (fu->result_td->kind == TD_FLOAT || fu->result_td->kind == TD_FLOAT32) {
        double d;
        memcpy(&d, &fu->result, 8);
        ((void (*)(void *, double))code)(clos, d);
    } else {
        ((void (*)(void *, int64_t))code)(clos, fu->result);
    }

    rt_gc_top = f.prev;
}

static void future_complete(Future *fu, int64_t result, void *error);

// Runs the completion callbacks of fu in registration order. The caller roots
// fu. A callback may register more callbacks and may start new tasks.

static void future_fire(Future *fu) {
    while (fu->cbs) {
        FutCB *cb = fu->cbs;
        fu->cbs = cb->next;
        if (cb->clos) {
            rt_future_call_cb(fu, cb->clos);
        } else if (cb->race->state == FUT_PENDING) {
            // This argument finished first, so the race answers its position.
            // The race carries no error of its own, even when the argument
            // raised one: the error stays on the argument, and an await of the
            // winner collects it (§2.9).
            future_complete(cb->race, cb->index, NULL);
        }
    }
}

// An error that a task raised, held until an await observes it. Without this
// list a future that nothing awaits would lose its error, and async would be
// the one place where an uncaught error does not stop the program.
// rt_async_drain reports what remains when the program runs out of work.
//
// malloc copies the message, because the list outlives the view the collector
// has of the Error object. Nothing frees the copy: it is read only on the way
// to the exit.
typedef struct PendingErr {
    struct PendingErr *next;
    Future *fu;
    char *msg;
} PendingErr;

static PendingErr *pending_errs;

static void pending_push(Future *fu, void *error) {
    Str *m = (Str *)(intptr_t)rt_rec_get(error, 0, &rt_td_error);
    PendingErr *p = malloc(sizeof(PendingErr));
    if (!p) rt_panic("out of memory");
    p->msg = malloc((size_t)m->len + 1);
    if (!p->msg) rt_panic("out of memory");
    memcpy(p->msg, m->data, (size_t)m->len);
    p->msg[m->len] = 0;
    p->fu = fu;
    p->next = pending_errs;
    pending_errs = p;
}

// Gives the Error of an awaited future to generated code, which raises it
// again (§2.9). A read counts as observing the error.
void *rt_future_error(Future *fu) {
    if (!fu || !fu->error) return NULL;
    for (PendingErr **pp = &pending_errs; *pp; pp = &(*pp)->next) {
        if ((*pp)->fu == fu) {
            PendingErr *dead = *pp;
            *pp = dead->next;
            free(dead->msg);
            free(dead);
            break;
        }
    }
    return fu->error;
}

static void future_complete(Future *fu, int64_t result, void *error) {
    fu->result = result;
    fu->error = error;
    if (error) pending_push(fu, error);
    fu->state = FUT_DONE;
    fu->frame = NULL; // dead once the result is lifted out
    fu->frame_td = NULL;
    live_tasks--;
    while (fu->waiters) {
        Future *w = fu->waiters;
        fu->waiters = w->qnext;
        ready_push(w);
    }
    future_fire(fu);
}

// Appends a completion callback, so callbacks run in registration order.
// lib/async.c uses it too, so a closure node and a race node share that order.
void rt_future_add_cb(Future *fu, FutCB *node) {
    node->next = NULL;
    if (!fu->cbs) {
        fu->cbs = node;
        return;
    }
    FutCB *tail = fu->cbs;
    while (tail->next) tail = tail->next;
    tail->next = node;
}

// Roots the argument array of a race. One static descriptor covers every call,
// because rt_td_future alone serves any Future<T>.
static TypeDesc td_future_array = {TD_ARRAY, &rt_td_future, 0, 0, 0, 0};

// async.race(f1, f2, ...) gives a future of the index of the first argument to
// complete. It answers an index because the arguments need not share a type.
// An await of the winner afterwards costs nothing.
Future *rt_async_race(Arr *futures) {
    if (!futures || futures->len == 0) rt_panic("async.race needs at least one future");

    TypeDesc *tds[2] = {&td_future_array, &rt_td_future};
    int64_t slots[2] = {(int64_t)(intptr_t)futures, 0};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    Future *race = rt_alloc(sizeof(Future));
    race->state = FUT_PENDING;
    race->result_td = &rt_td_int;
    slots[1] = (int64_t)(intptr_t)race;
    live_tasks++;

    // An argument that already finished decides the race before any callback
    // registers. A tie goes to the earliest argument.
    for (int64_t i = 0; i < futures->len; i++) {
        Future *fu = (Future *)(intptr_t)rt_arr_units(futures)[i];
        if (!fu) rt_panic("async.race on a future that was never assigned");
        if (fu->state == FUT_DONE) {
            future_complete(race, i, NULL);
            rt_gc_top = f.prev;
            return race;
        }
    }
    for (int64_t i = 0; i < futures->len; i++) {
        // The allocation comes before the read of the future out of the array,
        // because it can collect and the array is the root that survives it.
        FutCB *node = rt_alloc(sizeof(FutCB));
        node->race = (Future *)(intptr_t)slots[1];
        node->index = i;
        rt_future_add_cb((Future *)(intptr_t)rt_arr_units((Arr *)(intptr_t)slots[0])[i], node);
    }

    rt_gc_top = f.prev;
    return race;
}

// Steps one ready task and returns 0 when the ready queue is empty. The
// scheduler can enter itself again, because a body can call a synchronous
// function that awaits. That nesting is safe.
static int step_task(void) {
    Future *t = ready_pop();
    if (!t) return 0;

    // The root reaches t and the frame of t, which the step can collect.
    TypeDesc *tds[1] = {&rt_td_future};
    int64_t slots[1] = {(int64_t)(intptr_t)t};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    if (t->step(t->frame) == 0) {
        // The body finished. Frame slot 1 holds the result and slot 3 holds
        // the Error it raised, for whoever awaits the future.
        int64_t result;
        void *error;
        memcpy(&result, (char *)t->frame + 8, 8);
        memcpy(&error, (char *)t->frame + 24, 8);
        future_complete(t, result, error);
    } else {
        // The body suspended on the future in frame slot 2, so park the task
        // in arrival order. A future that is already complete has nothing left
        // to wake the task, so the task goes back on the ready queue.
        Future *on;
        memcpy(&on, (char *)t->frame + 16, 8);
        if (!on || on->state == FUT_DONE) {
            ready_push(t);
        } else {
            t->qnext = NULL;
            if (!on->waiters) {
                on->waiters = t;
            } else {
                Future *w = on->waiters;
                while (w->qnext) w = w->qnext;
                w->qnext = t;
            }
        }
    }

    rt_gc_top = f.prev;
    return 1;
}

Future *rt_async_timer_new(int64_t ms) {
    if (ms < 0) ms = 0;
    Future *fu = rt_alloc(sizeof(Future));
    fu->state = FUT_PENDING;
    fu->deadline = rt_mono_ms() + ms;
    fu->seq = timer_seq++;
    timer_push(fu);
    live_tasks++;
    return fu;
}

// Completes the earliest pending timer, and sleeps until its deadline first.
// It returns 0 when no timer is pending.
static int timer_wait(void) {
    if (rt_async_timer_count == 0) return 0;

    // A sleep long enough to hold back a due idle collection happens in parts,
    // until that collection has run.
    for (;;) {
        int64_t left = rt_async_timer_heap[0]->deadline - rt_mono_ms();
        int64_t owed = rt_gc_idle_owed();
        if (left <= 0 || !owed) {
            sleep_ms(left);
            break;
        }
        sleep_ms(left < owed ? left : owed);
        rt_gc_idle_hint();
    }
    Future *fu = timer_pop();

    // fu left the timer heap, so nothing else roots it.
    TypeDesc *tds[1] = {&rt_td_future};
    int64_t slots[1] = {(int64_t)(intptr_t)fu};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;
    future_complete(fu, 0, NULL);
    rt_gc_top = f.prev;
    return 1;
}

// ---- futures completed from outside the scheduler (runtime.h) ----

ExternWork *rt_async_extern = NULL;
int64_t rt_async_extern_count = 0;
static int64_t extern_cap = 0;

// The distinct waiters, with the count of futures each one has outstanding. A
// waiter is a whole standard library module that speaks to the operating
// system, and there are two: process and net. A full table means a new library
// arrived without a change here. The code reports that and drops no waiter,
// because a dropped waiter leaves its program with no progress.
#define EXTERN_WAITERS 4
static struct {
    RtExternWait fn;
    int64_t n;
} waiters[EXTERN_WAITERS];

// How long the scheduler sleeps between rounds of polling when more than one
// waiter has work and none of them can block. It is short enough that a running
// child does not visibly delay a reply on a socket, and long enough that the
// loop does not spin.
#define EXTERN_POLL_MS 2

static void waiter_add(RtExternWait fn, int64_t delta) {
    for (int i = 0; i < EXTERN_WAITERS; i++) {
        if (waiters[i].fn == fn) {
            waiters[i].n += delta;
            return;
        }
        if (!waiters[i].fn) {
            waiters[i].fn = fn;
            waiters[i].n = delta;
            return;
        }
    }
    rt_panic("too many external waiters registered");
}

Future *rt_async_extern_new(RtExternWait wait, void *aux, TypeDesc *aux_td) {
    if (!wait) rt_panic("external future enrolled without a waiter");
    // aux becomes a root only after it enters the list, so hold it across the
    // allocation below.
    TypeDesc *tds[1] = {aux_td};
    int64_t slots[1] = {(int64_t)(intptr_t)aux};
    GCFrame f = {rt_gc_top, aux_td ? 1 : 0, tds, slots, NULL};
    rt_gc_top = &f;

    Future *fu = rt_alloc(sizeof(Future));
    fu->state = FUT_PENDING;
    if (rt_async_extern_count == extern_cap) {
        extern_cap = extern_cap ? extern_cap * 2 : 8;
        rt_async_extern = realloc(rt_async_extern, (size_t)extern_cap * sizeof(ExternWork));
        if (!rt_async_extern) rt_panic("out of memory");
    }
    ExternWork *w = &rt_async_extern[rt_async_extern_count++];
    w->fu = fu;
    w->wait = wait;
    w->aux = (void *)(intptr_t)slots[0];
    w->aux_td = aux_td;
    waiter_add(wait, 1);
    live_tasks++;

    rt_gc_top = f.prev;
    return fu;
}

void rt_async_extern_done(Future *fu, int64_t result, TypeDesc *result_td, void *error) {
    for (int64_t i = 0; i < rt_async_extern_count; i++) {
        if (rt_async_extern[i].fu != fu) continue;
        waiter_add(rt_async_extern[i].wait, -1);
        rt_async_extern[i] = rt_async_extern[--rt_async_extern_count];
        break;
    }
    // The result descriptor must be in place before the store. future_complete
    // runs the callbacks, which allocate, and fu is then the only holder of the
    // result.
    fu->result_td = result_td;

    TypeDesc *tds[1] = {&rt_td_future};
    int64_t slots[1] = {(int64_t)(intptr_t)fu};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;
    future_complete(fu, result, error);
    rt_gc_top = f.prev;
}

// Gives the outstanding external work its turn, inside the budget the pending
// timers leave. It reports whether anything completed. The budget ends at the
// deadline of the earliest timer, so neither kind of work starves the other.
//
// One library may block in the system call it knows. Two may not: a wait for a
// child cannot be woken by a socket, so a block in either leaves the futures of
// the other pending. The scheduler then polls each one in turn.
static int extern_turn(void) {
    RtExternWait only = NULL;
    int n = 0;
    for (int i = 0; i < EXTERN_WAITERS && waiters[i].fn; i++) {
        if (waiters[i].n == 0) continue;
        only = waiters[i].fn;
        n++;
    }
    if (n == 0) return 0;

    // A wait for something outside the program is the quiet point where a
    // collection stops nothing.
    rt_gc_idle_hint();

    int64_t budget = -1; // nothing else pending: block until one is ready
    if (rt_async_timer_count > 0) {
        budget = rt_async_timer_heap[0]->deadline - rt_mono_ms();
        if (budget < 0) budget = 0;
    }
    // One waiter can block, but not past a collection the idle hint still owes.
    // An idle program gives the hint no later chance, so while a collection is
    // due the wait happens in parts and the hint runs between them.
    if (n == 1) {
        for (;;) {
            int64_t owed = rt_gc_idle_owed();
            int64_t slice = budget;
            if (owed && (slice < 0 || slice > owed)) slice = owed;
            if (only(slice)) return 1;
            if (slice == budget) return 0;
            rt_gc_idle_hint();
        }
    }

    // Poll first and decide after. A budget of 0 means a timer is already due,
    // and every waiter still gets the one look that can complete its work.
    for (int64_t waited = 0;;) {
        for (int i = 0; i < EXTERN_WAITERS && waiters[i].fn; i++) {
            if (waiters[i].n > 0 && waiters[i].fn(0)) return 1;
        }
        if (budget == 0) return 0;
        int64_t slice = EXTERN_POLL_MS;
        if (budget > 0 && budget - waited < slice) slice = budget - waited;
        if (slice <= 0) return 0;
        rt_gc_idle_hint(); // rate-limited; this loop already never blocks long
        sleep_ms(slice);
        waited += slice;
    }
}

// How many ready steps may run back to back before the external waiters get one
// zero-budget look. A saturated server never empties the ready queue, so a
// future parked with the poller would wait for a quiet moment that never comes.
// Due timers get the same look, because a long wave starves them the same way.
#define EXTERN_LOOK_STEPS 64

// One turn of the scheduler. It returns 0 when there is no ready task, no
// external work and no timer, so no work can appear again.
static int sched_turn(void) {
    if (step_task()) {
        static int64_t steps = 0;
        if (++steps >= EXTERN_LOOK_STEPS) {
            steps = 0;
            for (int i = 0; i < EXTERN_WAITERS && waiters[i].fn; i++) {
                if (waiters[i].n > 0) waiters[i].fn(0);
            }
            if (rt_async_timer_count > 0 &&
                rt_async_timer_heap[0]->deadline <= rt_mono_ms()) {
                timer_wait();
            }
        }
        return 1;
    }
    if (extern_turn()) return 1;
    return timer_wait();
}

int64_t rt_await(Future *fu) {
    if (!fu) rt_panic("awaited a future that was never assigned");
    // Outside an async function, an await drives the scheduler until fu is
    // complete, in arrival order. Inside one, codegen suspends and does not
    // call this.
    TypeDesc *tds[1] = {&rt_td_future};
    int64_t slots[1] = {(int64_t)(intptr_t)fu};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    while (fu->state != FUT_DONE) {
        if (!sched_turn()) rt_panic("deadlock: every pending future is awaiting another");
    }

    rt_gc_top = f.prev;
    return fu->result;
}

void rt_async_drain(void) {
    while (sched_turn()) {
    }
    // No work remains, so a task that is still live waits on another one.
    if (live_tasks > 0) rt_panic("deadlock: every pending future is awaiting another");
    // Nothing awaited the future of a task that raised. No caller is left to
    // take the error, so it stops the program (§2.9).
    if (pending_errs) rt_panic(pending_errs->msg);
}

// ---- generic equality (primitives, strings, optionals) ----

int64_t rt_eq(int64_t a, int64_t b, TypeDesc *td) {
    switch (td->kind) {
    case TD_OPTIONAL: {
        void *pa = (void *)(intptr_t)a;
        void *pb = (void *)(intptr_t)b;
        if (!pa && !pb) return 1;
        if (!pa || !pb) return 0;
        return rt_eq(rt_box_get(pa, td->elem), rt_box_get(pb, td->elem), td->elem);
    }
    case TD_STRING:
        return rt_str_eq((Str *)(intptr_t)a, (Str *)(intptr_t)b);
    case TD_FLOAT:
    case TD_FLOAT32: {
        double x, y;
        memcpy(&x, &a, 8);
        memcpy(&y, &b, 8);
        return x == y; // a float compare: -0.0 == 0.0, and NaN equals nothing
    }
    default: // the integer types, bool, DateTime, Duration, enum
        return a == b;
    }
}

// ---- date formatting and parsing ----
//
// This is in the core because a program prints and serializes a DateTime with
// no import of the time module. lib/json.c reads one out of a JSON string with
// no dependency on a module that may not link.

// Days since 1970-01-01 to a civil date and back, on the proleptic Gregorian
// calendar (Howard Hinnant's algorithms). The C library's gmtime_r and timegm
// answer the same, but the Windows CRT has neither under those names and
// refuses every date before 1970, so the calendar is written here once.
static void civil_from_days(int64_t z, int *y, int *m, int *d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void rt_date_tm(int64_t ms, struct tm *out) {
    int64_t sec = ms / 1000;
    if (ms < 0 && ms % 1000 != 0) sec -= 1;
    int64_t days = sec / 86400;
    int64_t rem = sec % 86400;
    if (rem < 0) {
        rem += 86400;
        days -= 1;
    }
    memset(out, 0, sizeof *out);
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    out->tm_year = y - 1900;
    out->tm_mon = m - 1;
    out->tm_mday = d;
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    // 1970-01-01 was a Thursday.
    out->tm_wday = (int)(((days % 7) + 11) % 7);
    out->tm_yday = (int)(days - days_from_civil(y, 1, 1));
}

Str *rt_date_to_text(int64_t ms) {
    struct tm t;
    rt_date_tm(ms, &t);
    char buf[40];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02dZ",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return rt_str_from_c(buf);
}

int rt_date_parse_iso(const char *buf, int64_t len, int64_t *out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, n = 0;
    if (sscanf(buf, "%d-%d-%dT%d:%d:%dZ%n", &y, &mo, &d, &h, &mi, &se, &n) == 6 && n == (int)len) {
        // full timestamp
    } else {
        n = 0;
        if (sscanf(buf, "%d-%d-%d%n", &y, &mo, &d, &n) == 3 && n == (int)len) {
            h = mi = se = 0;
        } else {
            return 0;
        }
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
        mi < 0 || mi > 59 || se < 0 || se > 60) {
        return 0;
    }
    // A day past the end of its month or a second of 60 carries into the next
    // unit, as timegm does.
    int64_t sec = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    *out = sec * 1000;
    return 1;
}

// ---- print ----

// The shortest form of v that reads back as v at the precision of its type.
// A float32 holds a binary32, so it needs 9 significant digits at most. At 17
// digits it would print 0.10000000149011612 where the program wrote 0.1.
//
// lib/json.c writes its numbers with this too, so a float in JSON and a float
// that print writes agree digit for digit.
void rt_fmt_double(char *b, size_t n, double v, int f32) {
    // A NaN is "nan" whatever its sign bit. The C libraries differ: glibc
    // writes "-nan" and the Windows CRT writes "-nan(ind)", and 0.0 / 0.0 on
    // x86 has the sign bit set.
    if (v != v) {
        snprintf(b, n, "nan");
        return;
    }
    int lo = f32 ? 6 : 15, hi = f32 ? 9 : 17;
    for (int prec = lo; prec <= hi; prec++) {
        snprintf(b, n, "%.*g", prec, v);
        if (f32 ? (float)strtod(b, NULL) == (float)v : strtod(b, NULL) == v) break;
    }
}

// A present optional shows the value in it, at any depth of boxes.
static TypeDesc *unwrap_optional(int64_t *raw, TypeDesc *td) {
    while (td->kind == TD_OPTIONAL && *raw != 0) {
        *raw = rt_box_get((void *)(intptr_t)*raw, td->elem);
        td = td->elem;
    }
    return td;
}

// The decimal text of a signed 64-bit value, built backwards from the end of
// buf and answered by a pointer into it. The buffer must hold 21 bytes: 19
// digits, a sign and the terminator.
static char *int_text(int64_t v, char *buf, size_t n) {
    char *end = buf + n - 1;
    *end = '\0';
    char *p = end;
    // The negation happens in an unsigned type, so INT64_MIN needs no special
    // case: -(uint64_t)v is its magnitude, which an int64_t cannot hold.
    uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
    do {
        *--p = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    if (v < 0) *--p = '-';
    return p;
}

// int_text for an unsigned value, with the same buffer rule. It is a separate
// function so the hot signed path carries no test for the sign.
static char *uint_text(uint64_t u, char *buf, size_t n) {
    char *end = buf + n - 1;
    *end = '\0';
    char *p = end;
    do {
        *--p = (char)('0' + (u % 10));
        u /= 10;
    } while (u);
    return p;
}

// The decimal text of a small non-negative integer, built once and shared.
// Sharing is safe because a String is immutable.
//
// Each block carries the one-word header with GC_STATIC set, as the string
// literals codegen emits do, so the collector never marks, sweeps or traces
// into it. A bound of 1024 covers most such strings, for 24 KB.
#define SMALL_INT_N 1024

typedef struct {
    GCObj hdr;
    int64_t len;   // the Str begins here: len, then its bytes
    char bytes[8];
} SmallIntText;

static SmallIntText small_int[SMALL_INT_N];

static Str *small_int_text(int64_t v) {
    SmallIntText *e = &small_int[v];
    if (e->hdr.info == 0) {
        char b[24];
        const char *t = int_text(v, b, sizeof b);
        int64_t n = (int64_t)strlen(t);
        memcpy(e->bytes, t, (size_t)n);
        e->len = n;
        e->hdr.info = ((uint64_t)(sizeof(Str) + (size_t)n) << GC_SIZE_SHIFT) | GC_STATIC;
    }
    return (Str *)&e->len;
}

// The text of one scalar, for the kinds whose text is a run of bytes. The
// result sits in the buffer of the caller, or is a pointer the TypeDesc holds
// already, which is the case for the name of an enum member.
//
// It does not answer for the three kinds whose text is a Str: an absent
// optional, a string and a DateTime. rt_print and rt_scalar_to_text each
// handle those.
static const char *scalar_text(int64_t raw, TypeDesc *td, char *buf, size_t n) {
    switch (td->kind) {
    case TD_FLOAT:
    case TD_FLOAT32: {
        double d;
        memcpy(&d, &raw, 8);
        rt_fmt_double(buf, n, d, td->kind == TD_FLOAT32);
        return buf;
    }
    case TD_BOOL:
        return raw ? "true" : "false";
    case TD_INT8:
    case TD_INT16:
    case TD_INT32:
    case TD_INT:
    // A Duration prints as its count of milliseconds. == compares that count.
    case TD_DURATION:
        return int_text(raw, buf, n);
    // An unsigned value fills its unit, so the top bit is part of the number.
    // 2^64-1 prints as itself.
    case TD_UINT8:
    case TD_UINT16:
    case TD_UINT32:
    case TD_UINT:
        return uint_text((uint64_t)raw, buf, n);
    case TD_ENUM: {
        // The name of the member the value matches. A value that matches no
        // member prints as its number.
        const int64_t *vals = (const int64_t *)td->field_types;
        for (int64_t i = 0; i < td->nfields; i++) {
            if (vals[i] == raw) return td->field_names[i];
        }
        return int_text(raw, buf, n);
    }
    default:
        rt_panic("cannot print this value"); // does not return
        return "";
    }
}

// print takes a scalar only, because the checker rejects a record and an array.
// The core runtime therefore never depends on the JSON serializer in lib/json.c.
//
// This writes one value and nothing around it. Codegen emits the space between
// two arguments, and the newline after the last one, as their own calls to
// rt_print_space and rt_print_newline.
void rt_print(int64_t raw, TypeDesc *td) {
    td = unwrap_optional(&raw, td);
    switch (td->kind) {
    case TD_OPTIONAL:
        fputs("null", stdout);
        break;
    case TD_STRING: {
        Str *s = (Str *)(intptr_t)raw;
        fwrite(s->data, 1, (size_t)s->len, stdout);
        break;
    }
    case TD_DATETIME: {
        Str *s = rt_date_to_text(raw);
        fwrite(s->data, 1, (size_t)s->len, stdout);
        break;
    }
    default: {
        char b[64];
        fputs(scalar_text(raw, td, b, sizeof b), stdout);
        break;
    }
    }
}

// string.from gives the text that rt_print writes for the same value. It sits
// beside print and shares its formatting, because the two are one definition.
Str *rt_scalar_to_text(int64_t raw, TypeDesc *td) {
    td = unwrap_optional(&raw, td);
    switch (td->kind) {
    case TD_OPTIONAL:
        return rt_str_from_c("null");
    case TD_STRING:
        // A string is immutable, so its text is the string itself. A program
        // asks for a separate one with string.copy.
        return (Str *)(intptr_t)raw;
    case TD_DATETIME:
        return rt_date_to_text(raw);
    case TD_INT8:
    case TD_INT16:
    case TD_INT32:
    case TD_INT:
    case TD_DURATION:
    // The unsigned types join the signed ones. Below SMALL_INT_N the two
    // families write the same digits.
    case TD_UINT8:
    case TD_UINT16:
    case TD_UINT32:
    case TD_UINT: {
        // An unsigned comparison, so every negative value goes to the general
        // path with no second test.
        if ((uint64_t)raw < SMALL_INT_N) return small_int_text(raw);
        char b[24];
        if (TD_IS_UNSIGNED(td->kind)) return rt_str_from_c(uint_text((uint64_t)raw, b, sizeof b));
        return rt_str_from_c(int_text(raw, b, sizeof b));
    }
    default: {
        char b[64];
        return rt_str_from_c(scalar_text(raw, td, b, sizeof b));
    }
    }
}

void rt_print_space(void) { fputc(' ', stdout); }

void rt_print_newline(void) { fputc('\n', stdout); }

// ---- code points (§4.7) ----

// Decodes the UTF-8 sequence at byte offset off (runtime.h). A stray
// continuation byte, a truncated sequence, an overlong form, a surrogate and
// a value past U+10FFFF are each U+FFFD one byte wide. The walk never stalls
// and never skips a byte.
int64_t rt_str_decode(Str *s, int64_t off, int64_t *width) {
    const unsigned char *p = (const unsigned char *)s->data + off;
    int64_t left = s->len - off;
    unsigned char c = p[0];
    if (c < 0x80) {
        *width = 1;
        return c;
    }
    int64_t need;
    int64_t cp;
    int64_t min;
    if ((c & 0xE0) == 0xC0) {
        need = 1;
        cp = c & 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        need = 2;
        cp = c & 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        need = 3;
        cp = c & 0x07;
        min = 0x10000;
    } else {
        *width = 1;
        return 0xFFFD;
    }
    if (left < need + 1) {
        *width = 1;
        return 0xFFFD;
    }
    for (int64_t k = 1; k <= need; k++) {
        if ((p[k] & 0xC0) != 0x80) {
            *width = 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (p[k] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *width = 1;
        return 0xFFFD;
    }
    *width = need + 1;
    return cp;
}
