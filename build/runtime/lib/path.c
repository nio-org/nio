// The `path` standard library module (import 'path'), linked only when a
// program imports it. Signatures must match pathCallType in the checker and
// genPathCall in codegen.
//
// join and localize are lexical. They read only their arguments and never the
// file system, so neither can fail.
//
// Both '/' and '\' are separators on every platform, and both functions write
// the host separator. A backslash is a legal character in a POSIX file name,
// so these functions cannot name a POSIX file that contains one.
//
// tests/path_win_test.nio compiles the Windows code with _WIN32 defined and
// runs a fixed table of Windows join results through it. No test reaches
// _getcwd.

#include "runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define NIO_SEP '\\'
#define nio_getcwd _getcwd
#else
#include <unistd.h>
#define NIO_SEP '/'
#define nio_getcwd getcwd
#endif

static int is_sep(char c) { return c == '/' || c == '\\'; }

static Str *part(Arr *a, int64_t i) { return (Str *)(intptr_t)rt_arr_units(a)[i]; }

// The length of the volume prefix of p: the leading part that no ".." can go
// above. POSIX paths have none. A Windows path has a drive letter ("C:") or a
// UNC share ("\\host\share"), up to the second separator after the "\\".
// Device namespace prefixes (`\\?\`, `\\.\`, `\??\`) clean as UNC paths.
static int64_t volume_len(const char *p, int64_t n) {
#if defined(_WIN32)
    if (n >= 2 && p[1] == ':') return 2; // a drive letter, any letter
    if (n < 2 || !is_sep(p[0]) || !is_sep(p[1])) return 0;
    int seps = 0;
    for (int64_t i = 2; i < n; i++) {
        if (is_sep(p[i]) && ++seps == 2) return i;
    }
    return n;
#else
    (void)p;
    (void)n;
    return 0;
#endif
}

// Cleans buf in place and returns the cleaned length. Separators are already
// NIO_SEP, and the volume prefix is already removed. Empty and "." elements are
// removed, ".." cancels the element before it, and a trailing separator is
// removed. A ".." with nothing to cancel stays in a relative path and is
// removed from a rooted one. An empty result is ".".
//
// Cleaning only makes buf shorter, so the write index never passes the read
// index.
static int64_t clean(char *buf, int64_t n) {
    int rooted = n > 0 && buf[0] == NIO_SEP;
    int64_t r = 0, w = 0;
    int64_t floor = 0; // a ".." below this is already handled
    if (rooted) {
        buf[w++] = NIO_SEP;
        r = 1;
        floor = 1;
    }
    while (r < n) {
        if (buf[r] == NIO_SEP) {
            r++; // empty element
        } else if (buf[r] == '.' && (r + 1 == n || buf[r + 1] == NIO_SEP)) {
            r++; // "." element
        } else if (buf[r] == '.' && r + 1 < n && buf[r + 1] == '.' &&
                   (r + 2 == n || buf[r + 2] == NIO_SEP)) {
            r += 2;
            if (w > floor) {
                // Back up over the element just written.
                for (w--; w > floor && buf[w] != NIO_SEP; w--) {
                }
            } else if (!rooted) {
                // There is no root to stop at, so the ".." stays in the result
                // and no later element can cancel it.
                if (w > 0) buf[w++] = NIO_SEP;
                buf[w++] = '.';
                buf[w++] = '.';
                floor = w;
            }
        } else {
            if ((rooted && w != 1) || (!rooted && w != 0)) buf[w++] = NIO_SEP;
            while (r < n && buf[r] != NIO_SEP) buf[w++] = buf[r++];
        }
    }
    if (w == 0) buf[w++] = '.';
    return w;
}

// On Windows, "C:a" is relative to the drive and "C:\a" is at the drive root,
// so no separator is added after a trailing colon. On POSIX a colon is an
// ordinary character.
static int suppresses_sep(char c) {
#if defined(_WIN32)
    return c == ':';
#else
    (void)c;
    return 0;
#endif
}

// Appends one element at len with its separators localized. An empty element
// adds nothing, so join("a", "", "b") is "a/b". When buf already ends in a
// separator, the leading separators of the element are removed. Thus ordinary
// elements cannot make a "\\" UNC prefix on Windows.
static int64_t append_part(char *buf, int64_t len, Str *p) {
    if (p->len == 0) return len;
    int64_t from = 0;
    if (len == 0) {
        // The first element is copied as written.
    } else if (is_sep(buf[len - 1])) {
        while (from < p->len && is_sep(p->data[from])) from++;
    } else if (!suppresses_sep(buf[len - 1])) {
        buf[len++] = NIO_SEP;
    }
    for (int64_t i = from; i < p->len; i++) {
        buf[len++] = is_sep(p->data[i]) ? NIO_SEP : p->data[i];
    }
    return len;
}

// Joins the elements with the host separator and cleans the result. The
// elements are copied into malloc scratch before the one allocation, so no Str
// is live across it and nothing needs a root.
Str *rt_path_join(Str *first, Arr *rest) {
    // A join adds at most one separator per element. The two extra bytes are for
    // a "." result.
    int64_t cap = first->len + 2;
    for (int64_t i = 0; i < rest->len; i++) cap += part(rest, i)->len + 1;
    char *buf = malloc((size_t)cap);
    if (!buf) rt_panic("out of memory");

    int64_t len = append_part(buf, 0, first);
    for (int64_t i = 0; i < rest->len; i++) len = append_part(buf, len, part(rest, i));

    int64_t vol = volume_len(buf, len);
    int64_t out;
    if (len == vol && vol > 1 && is_sep(buf[0]) && is_sep(buf[1])) {
        // On Windows, a volume with no path: `\\host` is already a path, and to
        // clean the empty remainder to "." would give `\\host.`. A bare drive
        // is different: `C:` becomes `C:.`, the working directory of the drive.
        out = vol;
    } else {
        out = vol + clean(buf + vol, len - vol);
    }

    Str *s = rt_str_alloc(out);
    memcpy(s->data, buf, (size_t)out);
    free(buf);
    return s;
}

// s is live across the allocation, so it gets a root first. A C parameter is
// not a root (see runtime.c).
Str *rt_path_localize(Str *s) {
    int64_t i = 0;
    while (i < s->len && (!is_sep(s->data[i]) || s->data[i] == NIO_SEP)) i++;
    if (i == s->len) return s; // all separators are already native

    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Str *r = rt_str_alloc(s->len);
    for (int64_t j = 0; j < s->len; j++) {
        r->data[j] = is_sep(s->data[j]) ? NIO_SEP : s->data[j];
    }

    rt_gc_top = f.prev;
    return r;
}

// ---- path.userData(app) ----

// The directory where the platform keeps the data of an application, with the
// app name last:
//
//   macOS     $HOME/Library/Application Support/<app>
//   Windows   %APPDATA%\<app>
//   others    $XDG_DATA_HOME/<app>, else $HOME/.local/share/<app>
//
// It is fallible because the base comes from an environment variable that can
// be unset. That raises NOT_FOUND with the name of the variable. An app name
// that is empty or contains a NUL or a separator is INVALID. The name is one
// path element, and a separator would give a deeper path.
//
// It returns the path only. The caller creates the directory with fs.createDir.
Str *rt_path_user_data(Str *app, void **err) {
    if (app->len == 0) {
        *err = rt_error_new("path.userData: the application name is empty", NIO_ERR_INVALID);
        return NULL;
    }
    for (int64_t i = 0; i < app->len; i++) {
        char c = app->data[i];
        if (c == 0 || is_sep(c)) {
            *err = rt_error_new(
                "path.userData: the application name must be a single path element",
                NIO_ERR_INVALID);
            return NULL;
        }
    }

    const char *base = NULL;
    const char *mid = NULL; // the part between the base and the app name
    const char *missing = NULL;
#if defined(_WIN32)
    base = getenv("APPDATA");
    mid = "";
    missing = "APPDATA";
#elif defined(__APPLE__)
    base = getenv("HOME");
    mid = "Library/Application Support";
    missing = "HOME";
#else
    base = getenv("XDG_DATA_HOME");
    mid = "";
    if (!base || !base[0]) {
        base = getenv("HOME");
        mid = ".local/share";
    }
    missing = "XDG_DATA_HOME and HOME";
#endif
    if (!base || !base[0]) {
        char msg[128];
        snprintf(msg, sizeof msg,
                 "path.userData: cannot locate the user data directory (%s unset)", missing);
        *err = rt_error_new(msg, NIO_ERR_NOT_FOUND);
        return NULL;
    }

    // Built and localized in malloc scratch, so no Str is live across the one
    // allocation and nothing needs a root.
    size_t blen = strlen(base), mlen = strlen(mid);
    int64_t cap = (int64_t)blen + (int64_t)mlen + app->len + 3;
    char *buf = malloc((size_t)cap);
    if (!buf) rt_panic("out of memory");
    int64_t w = 0;
    memcpy(buf + w, base, blen);
    w += (int64_t)blen;
    if (w > 0 && is_sep(buf[w - 1])) w--; // a base with a trailing separator
    if (mlen > 0) {
        buf[w++] = NIO_SEP;
        for (size_t i = 0; i < mlen; i++) buf[w++] = mid[i] == '/' ? NIO_SEP : mid[i];
    }
    buf[w++] = NIO_SEP;
    memcpy(buf + w, app->data, (size_t)app->len);
    w += app->len;

    Str *s = rt_str_alloc(w);
    memcpy(s->data, buf, (size_t)w);
    free(buf);
    return s;
}

// The path length has no portable limit, so the buffer grows until the path
// fits. It is malloc'd because a Str would need a root across the retry.
Str *rt_path_getcwd(void) {
    for (size_t n = 256;; n *= 2) {
        char *buf = malloc(n);
        if (!buf) rt_panic("out of memory");
        if (nio_getcwd(buf, n)) {
            Str *s = rt_str_from_c(buf);
            free(buf);
            return s;
        }
        int err = errno;
        free(buf);
        if (err != ERANGE) rt_panic("cannot read the current working directory");
        if (n > (size_t)1 << 20) {
            rt_panic("cannot read the current working directory: path too long");
        }
    }
}
