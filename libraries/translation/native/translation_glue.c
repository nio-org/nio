// The native half of libraries/translation that clang compiles (§5.8): a
// dlopen bridge to libNioTranslation.dylib, the Swift shim that build.sh makes.
// The work needs Swift, and `native source` compiles only C and Objective-C.
// Thus this file loads the dylib at run time and does not link it.
//
// The build stays clang-only. A machine without swiftc still builds every
// program that imports this library, and translation answers "unavailable"
// until the dylib exists. The program supplies the path, so no link-time flag
// has to guess a working directory.
//
// GC contract (runtime.h): this file reads its Str* parameters before anything
// allocates through the runtime, builds an answer last while it holds no other
// language pointer, and keeps only plain C memory across calls.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"

#define NT_ERR_NOT_LOADED (-20)
#define NT_ERR_NO_LIB (-21)
#define NT_ERR_BAD_LIB (-22)

#ifndef _WIN32

#include <dlfcn.h>

// The Swift shim's surface, resolved once by nt_load. Call on the main thread
// only. Strings are UTF-8, and every char* answer goes back through p_free.
static int32_t (*p_supported)(void);
static char *(*p_detect)(const char *text);
static char *(*p_syslang)(void);
static int64_t (*p_start)(const char *source, const char *target, const char *textsJson);
static int64_t (*p_prepare)(const char *source, const char *target);
static int64_t (*p_avail)(const char *source, const char *target);
static int64_t (*p_langs)(void);
static char *(*p_langname)(const char *tag);
static int32_t (*p_poll)(int64_t job);
static char *(*p_take)(int64_t job);
static void (*p_free)(char *p);
static int nt_ready = 0;

// A malloc'd NUL-terminated copy of a Str, which carries no terminator.
static char *nt_cstr(Str *s) {
    char *p = malloc((size_t)s->len + 1);
    if (!p) {
        return NULL;
    }
    memcpy(p, s->data, (size_t)s->len);
    p[s->len] = 0;
    return p;
}

static Str *nt_str(const char *utf8) {
    size_t n = utf8 ? strlen(utf8) : 0;
    Str *out = rt_str_alloc((int64_t)n);
    memcpy(out->data, utf8, n);
    return out;
}

int64_t nt_load(Str *path) {
    if (nt_ready) {
        return 0;
    }
    char *p = nt_cstr(path);
    if (!p) {
        return NT_ERR_NO_LIB;
    }
    void *lib = dlopen(p, RTLD_NOW | RTLD_LOCAL);
    free(p);
    if (!lib) {
        return NT_ERR_NO_LIB;
    }
    p_supported = (int32_t (*)(void))dlsym(lib, "niotr_supported");
    p_detect = (char *(*)(const char *))dlsym(lib, "niotr_detect");
    p_syslang = (char *(*)(void))dlsym(lib, "niotr_syslang");
    p_start = (int64_t (*)(const char *, const char *, const char *))dlsym(lib, "niotr_start");
    p_prepare = (int64_t (*)(const char *, const char *))dlsym(lib, "niotr_prepare");
    p_avail = (int64_t (*)(const char *, const char *))dlsym(lib, "niotr_avail");
    p_langs = (int64_t (*)(void))dlsym(lib, "niotr_langs");
    p_langname = (char *(*)(const char *))dlsym(lib, "niotr_langname");
    p_poll = (int32_t (*)(int64_t))dlsym(lib, "niotr_poll");
    p_take = (char *(*)(int64_t))dlsym(lib, "niotr_take");
    p_free = (void (*)(char *))dlsym(lib, "niotr_free");
    // Every symbol must be present. A dylib older than this file gives
    // BAD_LIB, which names build.sh. A partial load fails later, at one call.
    if (!p_supported || !p_detect || !p_syslang || !p_start || !p_prepare
        || !p_avail || !p_langs || !p_langname || !p_poll || !p_take || !p_free) {
        dlclose(lib);
        p_supported = NULL;
        return NT_ERR_BAD_LIB;
    }
    // The library stays loaded until the process ends, because the Swift
    // state, the jobs and the host window depend on it.
    nt_ready = 1;
    return 0;
}

int64_t nt_loaded(void) {
    return nt_ready;
}

int64_t nt_supported(void) {
    if (!nt_ready) {
        return 0;
    }
    return p_supported();
}

Str *nt_syslang(void) {
    if (!nt_ready) {
        return nt_str("");
    }
    char *answer = p_syslang();
    Str *out = nt_str(answer);
    p_free(answer);
    return out;
}

Str *nt_detect(Str *text) {
    if (!nt_ready) {
        return nt_str("");
    }
    char *t = nt_cstr(text);
    if (!t) {
        return nt_str("");
    }
    char *answer = p_detect(t);
    free(t);
    Str *out = nt_str(answer);
    p_free(answer);
    return out;
}

int64_t nt_start(Str *source, Str *target, Str *textsJson) {
    if (!nt_ready) {
        return NT_ERR_NOT_LOADED;
    }
    char *s = nt_cstr(source);
    char *t = nt_cstr(target);
    char *j = nt_cstr(textsJson);
    int64_t id = (s && t && j) ? p_start(s, t, j) : NT_ERR_NOT_LOADED;
    free(s);
    free(t);
    free(j);
    return id;
}

int64_t nt_prepare(Str *source, Str *target) {
    if (!nt_ready) {
        return NT_ERR_NOT_LOADED;
    }
    char *s = nt_cstr(source);
    char *t = nt_cstr(target);
    int64_t id = (s && t) ? p_prepare(s, t) : NT_ERR_NOT_LOADED;
    free(s);
    free(t);
    return id;
}

int64_t nt_avail(Str *source, Str *target) {
    if (!nt_ready) {
        return NT_ERR_NOT_LOADED;
    }
    char *s = nt_cstr(source);
    char *t = nt_cstr(target);
    int64_t id = (s && t) ? p_avail(s, t) : NT_ERR_NOT_LOADED;
    free(s);
    free(t);
    return id;
}

int64_t nt_langs(void) {
    if (!nt_ready) {
        return NT_ERR_NOT_LOADED;
    }
    return p_langs();
}

Str *nt_langname(Str *tag) {
    if (!nt_ready) {
        return nt_str("");
    }
    char *t = nt_cstr(tag);
    if (!t) {
        return nt_str("");
    }
    char *answer = p_langname(t);
    free(t);
    Str *out = nt_str(answer);
    p_free(answer);
    return out;
}

int64_t nt_poll(int64_t job) {
    if (!nt_ready) {
        return NT_ERR_NOT_LOADED;
    }
    return p_poll(job);
}

Str *nt_take(int64_t job) {
    if (!nt_ready) {
        return nt_str("");
    }
    char *answer = p_take(job);
    Str *out = nt_str(answer);
    p_free(answer);
    return out;
}

#else /* _WIN32: no dylib to load; everything answers "unavailable" */

int64_t nt_load(Str *path) {
    (void)path;
    return NT_ERR_NO_LIB;
}
int64_t nt_loaded(void) { return 0; }
int64_t nt_supported(void) { return 0; }
Str *nt_syslang(void) { return rt_str_alloc(0); }
Str *nt_detect(Str *text) {
    (void)text;
    return rt_str_alloc(0);
}
int64_t nt_start(Str *source, Str *target, Str *textsJson) {
    (void)source; (void)target; (void)textsJson;
    return NT_ERR_NOT_LOADED;
}
int64_t nt_prepare(Str *source, Str *target) {
    (void)source; (void)target;
    return NT_ERR_NOT_LOADED;
}
int64_t nt_avail(Str *source, Str *target) {
    (void)source; (void)target;
    return NT_ERR_NOT_LOADED;
}
int64_t nt_langs(void) { return NT_ERR_NOT_LOADED; }
Str *nt_langname(Str *tag) {
    (void)tag;
    return rt_str_alloc(0);
}
int64_t nt_poll(int64_t job) {
    (void)job;
    return NT_ERR_NOT_LOADED;
}
Str *nt_take(int64_t job) {
    (void)job;
    return rt_str_alloc(0);
}

#endif
