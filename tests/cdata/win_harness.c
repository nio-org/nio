// The harness for tests/path_win_test.nio: lib/path.c compiled for a faked
// Windows target, with stubs for the runtime entry points it calls.
//
// tests/path_win_test.nio puts its cases at the marker below and gives the
// result to clang. This is a separate file because §1.6 has no raw string, and
// a Windows path test is mostly backslashes.
//
// It prints nothing when all cases pass. Any output is a failure report.
#define _WIN32 1
// On a Windows host the real CRT marks strcpy and getenv deprecated, which
// fails under -Werror. runtime.h also defines this, but lib/path.c includes
// runtime.h after the headers below.
#define _CRT_SECURE_NO_WARNINGS
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/path.c"

GCFrame *rt_gc_top = NULL;
// All members are written because this file compiles under -Werror.
// -Wmissing-field-initializers does not know that the members after `rest`
// are unused for a scalar.
TypeDesc rt_td_string = {TD_STRING, NULL, 0, NULL, NULL, 0, NULL, 0, NULL};

void rt_panic(const char *msg) {
    fprintf(stderr, "panic: %s\n", msg);
    exit(1);
}

// Allocates a Str with malloc. These runs have no collector.
Str *rt_str_alloc(int64_t len) {
    Str *s = malloc(sizeof(Str) + (size_t)len + 1);
    if (!s) exit(2);
    s->len = len;
    s->data[len] = 0;
    return s;
}

Str *rt_str_from_c(const char *c) {
    Str *s = rt_str_alloc((int64_t)strlen(c));
    memcpy(s->data, c, strlen(c));
    return s;
}

char *_getcwd(char *buf, size_t n) {
    const char *fake = "C:\\work";
    if (n <= strlen(fake)) return NULL;
    strcpy(buf, fake);
    return buf;
}

// userData raises through this when APPDATA is not set. These runs set it, so
// a call here is a failure.
void *rt_error_new(const char *msg, int64_t code) {
    fprintf(stderr, "unexpected error (%lld): %s\n", (long long)code, msg);
    exit(3);
}

static int failed = 0;

// Joins n elements and reports only a disagreement.
static void jt(const char *want, int n, ...) {
    const char *e[8];
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) e[i] = va_arg(ap, const char *);
    va_end(ap);

    int64_t slots[8];
    for (int i = 1; i < n; i++) slots[i - 1] = (int64_t)(intptr_t)rt_str_from_c(e[i]);
    // A String[] holds 8-byte pointers (§5.7), so `slots` is the element
    // block as it is.
    Arr rest = {n - 1, n - 1, 8, slots};
    Str *got = rt_path_join(rt_str_from_c(e[0]), &rest);

    if (strcmp(got->data, want) != 0 || got->len != (int64_t)strlen(want)) {
        failed = 1;
        printf("join(");
        for (int i = 0; i < n; i++) printf("%s\"%s\"", i ? ", " : "", e[i]);
        printf(") = \"%s\", want \"%s\"\n", got->data, want);
    }
}

// Sets the variable with the call of the real host, because only the target
// is faked. The MSVC C runtime has no setenv.
static void set_appdata(const char *v) {
#if defined(_WIN32) && (defined(_MSC_VER) || defined(__MINGW32__))
    _putenv_s("APPDATA", v);
#else
    setenv("APPDATA", v, 1);
#endif
}

int main(void) {
/*<cases>*/

    Str *loc = rt_path_localize(rt_str_from_c("./thisdir/file"));
    if (strcmp(loc->data, ".\\thisdir\\file") != 0) {
        failed = 1;
        printf("localize = \"%s\", want \".\\\\thisdir\\\\file\"\n", loc->data);
    }
    Str *cwd = rt_path_getcwd();
    if (strcmp(cwd->data, "C:\\work") != 0) {
        failed = 1;
        printf("getCwd = \"%s\", want \"C:\\\\work\"\n", cwd->data);
    }

    // userData on Windows: %APPDATA%\<app>. The base keeps the spelling of
    // the host, and a trailing separator is not doubled.
    void *e = NULL;
    set_appdata("C:\\Users\\me\\AppData\\Roaming");
    Str *ud = rt_path_user_data(rt_str_from_c("myapp"), &e);
    if (strcmp(ud->data, "C:\\Users\\me\\AppData\\Roaming\\myapp") != 0) {
        failed = 1;
        printf("userData = \"%s\", want \"C:\\\\Users\\\\me\\\\AppData\\\\Roaming\\\\myapp\"\n",
               ud->data);
    }
    set_appdata("C:\\Users\\me\\AppData\\Roaming\\");
    ud = rt_path_user_data(rt_str_from_c("myapp"), &e);
    if (strcmp(ud->data, "C:\\Users\\me\\AppData\\Roaming\\myapp") != 0) {
        failed = 1;
        printf("userData (trailing sep) = \"%s\"\n", ud->data);
    }
    return failed;
}
