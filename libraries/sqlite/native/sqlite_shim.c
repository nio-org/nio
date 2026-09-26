// sqlite_shim.c: the bridge between sqlite.nio's externs and the vendored
// sqlite3.c beside it. Two constraints shape it: an extern crosses only
// scalars, String and arrays of those (§5.8), and the language is
// single-threaded, which -DSQLITE_THREADSAFE=0 matches.
//
// A database is a slot index into a small fixed table. A copy of a handle
// stays valid, and a closed slot answers "invalid".
//
// Result rows leave as one JSON text, because an extern cannot carry a record.
// sqlite.nio parses it once.
//
// A return code is 0 for ok, a positive sqlite result code, or one of the
// shim's own: -1 bad handle, -3 parameter count mismatch, -1000 table full.
// The wrapper reads the message buffer back at once.
//
// GC contract (§5.8): the only language values here are the caller's own
// arguments, rooted in the caller's frame, and this file never allocates a
// language value while it holds another. So no function needs a GCFrame.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sqlite3.h"
#include "runtime.h"

#define NIO_SQLITE_MAX 32

static sqlite3 *nio_sqlite_dbs[NIO_SQLITE_MAX];
static char *nio_sqlite_results[NIO_SQLITE_MAX];
static size_t nio_sqlite_result_lens[NIO_SQLITE_MAX];
static char nio_sqlite_errbuf[1024];

static void shim_set_err(const char *msg) {
    size_t n = msg ? strlen(msg) : 0;
    if (n >= sizeof nio_sqlite_errbuf) n = sizeof nio_sqlite_errbuf - 1;
    memcpy(nio_sqlite_errbuf, msg, n);
    nio_sqlite_errbuf[n] = 0;
}

static sqlite3 *shim_db(int64_t h) {
    if (h < 0 || h >= NIO_SQLITE_MAX) return NULL;
    return nio_sqlite_dbs[h];
}

// A Str is length-prefixed and not NUL-terminated. sqlite3_open and the tail
// walk in sqlite3_prepare_v2 need C strings.
static char *shim_cstr(Str *s) {
    char *p = malloc((size_t)s->len + 1);
    if (!p) return NULL;
    memcpy(p, s->data, (size_t)s->len);
    p[s->len] = 0;
    return p;
}

// A growable text buffer for the JSON result.
typedef struct {
    char *p;
    size_t len, cap;
} SB;

static int sb_reserve(SB *b, size_t need) {
    if (b->len + need <= b->cap) return 1;
    size_t cap = b->cap ? b->cap * 2 : 256;
    while (cap < b->len + need) cap *= 2;
    char *p = realloc(b->p, cap);
    if (!p) return 0;
    b->p = p;
    b->cap = cap;
    return 1;
}

static int sb_put(SB *b, const char *s, size_t n) {
    if (!sb_reserve(b, n)) return 0;
    memcpy(b->p + b->len, s, n);
    b->len += n;
    return 1;
}

static int sb_ch(SB *b, char c) { return sb_put(b, &c, 1); }

// A JSON string literal. The stored values are attacker-influenced text, so
// every byte below 0x20, the quote and the backslash go out as an escape.
static int sb_json_str(SB *b, const unsigned char *s, size_t n) {
    if (!sb_ch(b, '"')) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') {
            if (!sb_ch(b, '\\') || !sb_ch(b, (char)c)) return 0;
        } else if (c >= 0x20) {
            if (!sb_ch(b, (char)c)) return 0;
        } else {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            if (!sb_put(b, esc, 6)) return 0;
        }
    }
    return sb_ch(b, '"');
}

// `params` is a String[], which is an Arr whose units are Str*. Every value
// binds as text, and sqlite's column affinity converts it for an INTEGER or
// REAL column.
static int shim_bind(sqlite3_stmt *st, Arr *params) {
    int want = sqlite3_bind_parameter_count(st);
    int64_t have = params ? params->len : 0;
    if ((int64_t)want != have) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "statement wants %d parameters, got %lld", want, (long long)have);
        shim_set_err(msg);
        return -3;
    }
    for (int i = 0; i < want; i++) {
        Str *s = (Str *)(intptr_t)rt_arr_units(params)[i];
        int rc = sqlite3_bind_text(st, i + 1, s->data, (int)s->len, SQLITE_TRANSIENT);
        if (rc != SQLITE_OK) return rc;
    }
    return 0;
}

int64_t nio_sqlite_open(Str *path) {
    int64_t slot = -1;
    for (int64_t i = 0; i < NIO_SQLITE_MAX; i++) {
        if (!nio_sqlite_dbs[i]) { slot = i; break; }
    }
    if (slot < 0) {
        shim_set_err("too many open databases");
        return -1000;
    }
    char *cpath = shim_cstr(path);
    if (!cpath) { shim_set_err("out of memory"); return -(int64_t)SQLITE_NOMEM; }
    sqlite3 *db = NULL;
    int rc = sqlite3_open(cpath, &db);
    free(cpath);
    if (rc != SQLITE_OK) {
        shim_set_err(db ? sqlite3_errmsg(db) : "cannot open the database");
        if (db) sqlite3_close(db);
        return -(int64_t)rc;
    }
    // The timeout is short. A retry loop in C freezes the program's one
    // thread, so the caller gets the BUSY and decides.
    sqlite3_busy_timeout(db, 250);
    nio_sqlite_dbs[slot] = db;
    return slot;
}

// Runs every statement in `sql` in order. `params` binds afresh to each
// statement that declares placeholders, and any rows are stepped past.
int64_t nio_sqlite_exec(int64_t h, Str *sql, Arr *params) {
    sqlite3 *db = shim_db(h);
    if (!db) { shim_set_err("the database is closed"); return -1; }
    char *text = shim_cstr(sql);
    if (!text) { shim_set_err("out of memory"); return (int64_t)SQLITE_NOMEM; }
    const char *cur = text;
    int64_t out = 0;
    while (*cur) {
        sqlite3_stmt *st = NULL;
        const char *tail = NULL;
        int rc = sqlite3_prepare_v2(db, cur, -1, &st, &tail);
        if (rc != SQLITE_OK) {
            shim_set_err(sqlite3_errmsg(db));
            out = rc;
            break;
        }
        if (!st) { cur = tail; continue; }  // whitespace or a comment
        if (sqlite3_bind_parameter_count(st) > 0) {
            int brc = shim_bind(st, params);
            if (brc != 0) {
                if (brc > 0) shim_set_err(sqlite3_errmsg(db));
                sqlite3_finalize(st);
                out = brc;
                break;
            }
        }
        for (;;) {
            rc = sqlite3_step(st);
            if (rc == SQLITE_ROW) continue;
            break;
        }
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            shim_set_err(sqlite3_errmsg(db));
            out = rc;
            break;
        }
        cur = tail;
    }
    free(text);
    return out;
}

// One SELECT. It builds the rows as JSON, which nio_sqlite_result fetches
// after a 0 return. An INTEGER goes out as digits and stays exact, a REAL
// through %.17g, a TEXT escaped, and both a NULL and a BLOB as null.
int64_t nio_sqlite_query(int64_t h, Str *sql, Arr *params) {
    sqlite3 *db = shim_db(h);
    if (!db) { shim_set_err("the database is closed"); return -1; }
    char *text = shim_cstr(sql);
    if (!text) { shim_set_err("out of memory"); return (int64_t)SQLITE_NOMEM; }
    sqlite3_stmt *st = NULL;
    const char *tail = NULL;
    int rc = sqlite3_prepare_v2(db, text, -1, &st, &tail);
    if (rc != SQLITE_OK) {
        shim_set_err(sqlite3_errmsg(db));
        free(text);
        return rc;
    }
    if (!st) {
        shim_set_err("query needs a statement");
        free(text);
        return -1;
    }
    while (tail && *tail) {
        if (*tail != ' ' && *tail != '\t' && *tail != '\n' && *tail != '\r' && *tail != ';') {
            shim_set_err("query takes a single statement; use exec for several");
            sqlite3_finalize(st);
            free(text);
            return -1;
        }
        tail++;
    }
    if (sqlite3_bind_parameter_count(st) > 0) {
        int brc = shim_bind(st, params);
        if (brc != 0) {
            if (brc > 0) shim_set_err(sqlite3_errmsg(db));
            sqlite3_finalize(st);
            free(text);
            return brc;
        }
    }
    SB b = {0};
    int cols = sqlite3_column_count(st);
    int ok = sb_ch(&b, '[');
    int first = 1;
    for (;;) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) break;
        if (!ok) break;
        if (!first) ok = ok && sb_ch(&b, ',');
        first = 0;
        ok = ok && sb_ch(&b, '{');
        for (int i = 0; i < cols && ok; i++) {
            if (i) ok = ok && sb_ch(&b, ',');
            const char *name = sqlite3_column_name(st, i);
            ok = ok && sb_json_str(&b, (const unsigned char *)name, strlen(name));
            ok = ok && sb_ch(&b, ':');
            switch (sqlite3_column_type(st, i)) {
            case SQLITE_INTEGER: {
                char num[32];
                int n = snprintf(num, sizeof num, "%lld",
                                 (long long)sqlite3_column_int64(st, i));
                ok = ok && sb_put(&b, num, (size_t)n);
                break;
            }
            case SQLITE_FLOAT: {
                char num[40];
                int n = snprintf(num, sizeof num, "%.17g", sqlite3_column_double(st, i));
                // %g can spell an infinity or a NaN, which JSON cannot.
                if (strchr(num, 'i') || strchr(num, 'n')) {
                    ok = ok && sb_put(&b, "null", 4);
                } else {
                    ok = ok && sb_put(&b, num, (size_t)n);
                }
                break;
            }
            case SQLITE_TEXT: {
                const unsigned char *v = sqlite3_column_text(st, i);
                int n = sqlite3_column_bytes(st, i);
                ok = ok && sb_json_str(&b, v, (size_t)n);
                break;
            }
            default:
                ok = ok && sb_put(&b, "null", 4);
                break;
            }
        }
        ok = ok && sb_ch(&b, '}');
    }
    sqlite3_finalize(st);
    free(text);
    if (rc != SQLITE_DONE) {
        shim_set_err(sqlite3_errmsg(db));
        free(b.p);
        return rc;
    }
    if (!ok || !sb_ch(&b, ']')) {
        shim_set_err("out of memory");
        free(b.p);
        return (int64_t)SQLITE_NOMEM;
    }
    free(nio_sqlite_results[h]);
    nio_sqlite_results[h] = b.p;
    nio_sqlite_result_lens[h] = b.len;
    return 0;
}

// The rows the last successful query on this handle answered. It copies the
// malloc'd scratch into one language string and frees the scratch.
Str *nio_sqlite_result(int64_t h) {
    if (h < 0 || h >= NIO_SQLITE_MAX || !nio_sqlite_results[h]) {
        Str *out = rt_str_alloc(2);
        memcpy(out->data, "[]", 2);
        return out;
    }
    size_t n = nio_sqlite_result_lens[h];
    Str *out = rt_str_alloc((int64_t)n);
    memcpy(out->data, nio_sqlite_results[h], n);
    free(nio_sqlite_results[h]);
    nio_sqlite_results[h] = NULL;
    nio_sqlite_result_lens[h] = 0;
    return out;
}

Str *nio_sqlite_errmsg() {
    size_t n = strlen(nio_sqlite_errbuf);
    Str *out = rt_str_alloc((int64_t)n);
    memcpy(out->data, nio_sqlite_errbuf, n);
    return out;
}

// A stale or copied handle closes nothing and answers 0.
int64_t nio_sqlite_close(int64_t h) {
    sqlite3 *db = shim_db(h);
    if (!db) return 0;
    sqlite3_close(db);
    nio_sqlite_dbs[h] = NULL;
    free(nio_sqlite_results[h]);
    nio_sqlite_results[h] = NULL;
    nio_sqlite_result_lens[h] = 0;
    return 0;
}
