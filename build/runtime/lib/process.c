// The `process` module (import 'process'). Signatures must match
// src/builtins.nio and genProcessCall/genProcessNsCall in src/codegen.nio.
//
// The stdin reads are fallible and use the `void **err` convention. End of
// input is not a failure. process.child.run reports every failure by
// completing its future with the Error (§6.12).
//
// stdout shares rt_print's stdio stream and buffer.

#include "runtime.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
// The CRT's spellings, so the code below is one path.
#define environ _environ
#define nio_chdir _chdir
#define nio_getcwd _getcwd
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#define nio_chdir chdir
#define nio_getcwd getcwd
#endif

static TypeDesc td_string_array = {TD_ARRAY, &rt_td_string, 0, NULL, NULL, 0};
static TypeDesc td_byte_array = {TD_ARRAY, &rt_td_uint8, 0, NULL, NULL, 0};

void rt_process_exit(int64_t code) {
    exit((int)code);
}

// argv[0] is left out. The array is rooted while its strings are built; an
// unfilled slot is zero and tracing skips it.
Arr *rt_process_get_args(void) {
    int64_t n = rt_argc > 1 ? rt_argc - 1 : 0;

    TypeDesc *tds[1] = {&td_string_array};
    int64_t slots[1] = {0};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *a = rt_arr_new(n, 8);
    slots[0] = (int64_t)(intptr_t)a;
    for (int64_t i = 0; i < n; i++) {
        rt_arr_units(a)[i] = (int64_t)(intptr_t)rt_str_from_c(rt_argv[i + 1]);
    }

    rt_gc_top = f.prev;
    return a;
}

// Write errors are ignored, as rt_print ignores them.
void rt_process_stdout_write(Str *s) { fwrite(s->data, 1, (size_t)s->len, stdout); }

void rt_process_stderr_write(Str *s) { fwrite(s->data, 1, (size_t)s->len, stderr); }

// A stdout on a pipe is fully buffered, so a prompt-and-read protocol
// (`nio lsp`) must flush before it waits for the reply.
void rt_process_stdout_flush(void) { fflush(stdout); }

void rt_process_stderr_flush(void) { fflush(stderr); }

// errno is read first: a later call can overwrite it.
static void *stdin_errno(const char *fn) {
    int e = errno;
    char msg[256];
    snprintf(msg, sizeof msg, "process.stdin.%s: %s", fn, strerror(e));
    return rt_error_new(msg, rt_err_from_errno(e));
}

// Windows stdin defaults to text mode, which folds CRLF and stops at Ctrl+Z.
// readLine strips a trailing '\r' itself, so it agrees in either mode.
static void stdin_binary(void) {
#if defined(_WIN32)
    static int done = 0;
    if (!done) {
        _setmode(_fileno(stdin), _O_BINARY);
        done = 1;
    }
#endif
}

// The bytes accumulate in malloc memory, so nothing is live across the single
// Nio allocation.
Arr *rt_process_stdin_read(void **err) {
    stdin_binary();
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) rt_panic("out of memory");
    for (;;) {
        if (len == cap) {
            size_t grown = cap * 2;
            char *nb = realloc(buf, grown);
            if (!nb) {
                free(buf);
                rt_panic("out of memory");
            }
            buf = nb;
            cap = grown;
        }
        size_t got = fread(buf + len, 1, cap - len, stdin);
        len += got;
        if (got == 0) {
            if (ferror(stdin)) {
                free(buf);
                *err = stdin_errno("read");
                return NULL;
            }
            break;
        }
    }

    Arr *a = rt_arr_new((int64_t)len, 1);
    memcpy(rt_arr_bytes(a), buf, len);
    free(buf);
    return a;
}

// One line without its '\n' or trailing '\r'. A last line with no newline is
// still a line. The answer is null after end of input.
void *rt_process_stdin_read_line(void **err) {
    size_t cap = 128, len = 0;
    char *buf = malloc(cap);
    if (!buf) rt_panic("out of memory");
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) {
            if (ferror(stdin)) {
                free(buf);
                *err = stdin_errno("readLine");
                return NULL;
            }
            if (len == 0) {
                free(buf);
                return NULL;
            }
            break;
        }
        if (c == '\n') break;
        if (len == cap) {
            size_t grown = cap * 2;
            char *nb = realloc(buf, grown);
            if (!nb) {
                free(buf);
                rt_panic("out of memory");
            }
            buf = nb;
            cap = grown;
        }
        buf[len++] = (char)c;
    }
    if (len > 0 && buf[len - 1] == '\r') len--;

    Str *s = rt_str_alloc((int64_t)len);
    memcpy(s->data, buf, len);
    free(buf);

    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)s};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    void *box = rt_box((int64_t)(intptr_t)s, &rt_td_string);

    rt_gc_top = f.prev;
    return box;
}

// Up to n bytes. The answer is short only when the input ended first, and empty
// only at end of input. A negative count is a caller bug and panics (§6.6).
Arr *rt_process_stdin_read_bytes(int64_t n, void **err) {
    stdin_binary();
    if (n < 0) rt_panic("process.stdin.readBytes: negative count");
    if (n == 0) return rt_arr_new(0, 1);

    char *buf = malloc((size_t)n);
    if (!buf) rt_panic("out of memory");
    size_t got = fread(buf, 1, (size_t)n, stdin);
    if (got == 0 && ferror(stdin)) {
        free(buf);
        *err = stdin_errno("readBytes");
        return NULL;
    }

    Arr *a = rt_arr_new((int64_t)got, 1);
    memcpy(rt_arr_bytes(a), buf, got);
    free(buf);
    return a;
}

// A process.ChildRunResult record is four slots in declaration order:
// [0] stdout, [1] stderr, [2] code, [3] usage `ChildUsage?`. Must match
// types.ChildRunResultT.
#define RESULT_SLOTS 4

// A process.ChildUsage record is nine slots in declaration order, matching
// types.ChildUsageT. peakMemory is bytes; userTime and systemTime are
// Duration milliseconds.
#define USAGE_SLOTS 9

// The options record is five optional slots in declaration order:
// [0] stdin byte[]?, [1] cwd String?, [2] env Map<String, String>?,
// [3] clearEnv bool?, [4] inherit bool?. Must match types.ChildRunOptionsT.
// A null record is a call that passed no options.

// NULL when the record is absent, the field was left out, or the field is null.
static void *opt_val(int64_t *opts, int at) {
    if (!opts) return NULL;
    TypeDesc *td = REC_TD(opts);
    void *box = (void *)(intptr_t)rt_rec_get(opts, at, td);
    if (!box) return NULL;
    // The slot holds a box; the field descriptor gives its width (§5.7).
    return (void *)(intptr_t)rt_box_get(box, td->field_types[at]->elem);
}

static int opt_flag(int64_t *opts, int at) {
    if (!opts) return 0;
    TypeDesc *td = REC_TD(opts);
    void *box = (void *)(intptr_t)rt_rec_get(opts, at, td);
    return box && rt_box_get(box, td->field_types[at]->elem) != 0;
}

static void *run_error(Str *cmd, const char *reason, int64_t code) {
    char msg[1024];
    int64_t shown = cmd->len > 200 ? 200 : cmd->len;
    snprintf(msg, sizeof msg, "process.child.run \"%.*s\": %s", (int)shown, cmd->data, reason);
    return rt_error_new(msg, code);
}

static void *run_errno(Str *cmd) {
    int e = errno;
    return run_error(cmd, strerror(e), rt_err_from_errno(e));
}

// The same, from a C copy of the name: the child table holds no Nio values.
static void *run_error_c(const char *cmd, const char *reason, int64_t code) {
    char msg[1024];
    snprintf(msg, sizeof msg, "process.child.run \"%.200s\": %s", cmd, reason);
    return rt_error_new(msg, code);
}

static void *run_errno_c(const char *cmd) {
    int e = errno;
    return run_error_c(cmd, strerror(e), rt_err_from_errno(e));
}

// A NUL truncates the C string, so the prefix could name a different program.
static int str_has_nul(Str *s) {
    return memchr(s->data, 0, (size_t)s->len) != NULL;
}

// Callers must reject a NUL in s first.
static char *dup_str(Str *s) {
    char *c = malloc((size_t)s->len + 1);
    if (!c) rt_panic("out of memory");
    memcpy(c, s->data, (size_t)s->len);
    c[s->len] = 0;
    return c;
}

static void cargv_free(char **cargv) {
    for (int64_t i = 0; cargv[i]; i++) free(cargv[i]);
    free(cargv);
}

// A temporary file and not a pipe. A child that fills a pipe that nobody
// drains blocks forever.
static FILE *temp_stream(void) {
#if defined(_WIN32)
    FILE *f = NULL;
    if (tmpfile_s(&f) != 0) return NULL;
    return f;
#else
    return tmpfile();
#endif
}

// Reads f from the top into a malloc'd buffer. NULL with errno set on failure.
static char *slurp_temp(FILE *f, int64_t *out_len) {
    if (fseek(f, 0, SEEK_SET) != 0) return NULL;
    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) rt_panic("out of memory");
    for (;;) {
        if (len == cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                rt_panic("out of memory");
            }
            buf = nb;
            cap *= 2;
        }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) {
            if (ferror(f)) {
                free(buf);
                return NULL;
            }
            break;
        }
    }
    *out_len = (int64_t)len;
    return buf;
}

// The child's input, rewound to the start. It is a temporary file, like the
// output streams.
static FILE *stdin_stream(Arr *bytes) {
    FILE *f = temp_stream();
    if (!f) return NULL;
    if (bytes->len > 0 &&
        fwrite(rt_arr_bytes(bytes), 1, (size_t)bytes->len, f) != (size_t)bytes->len) {
        fclose(f);
        return NULL;
    }
    if (fflush(f) != 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    return f;
}

// strdup is POSIX and not standard C. The CRT spells it _strdup.
static char *dup_c(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out) memcpy(out, s, n + 1);
    return out;
}

// "KEY=VALUE" in malloc'd memory. Callers must reject a NUL in either half.
static char *env_entry(Str *k, Str *v) {
    char *out = malloc((size_t)k->len + 1 + (size_t)v->len + 1);
    if (!out) return NULL;
    memcpy(out, k->data, (size_t)k->len);
    out[k->len] = '=';
    memcpy(out + k->len + 1, v->data, (size_t)v->len);
    out[k->len + 1 + v->len] = 0;
    return out;
}

static int env_overrides(Map *m, const char *entry) {
    const char *eq = strchr(entry, '=');
    size_t n = eq ? (size_t)(eq - entry) : strlen(entry);
    for (int64_t i = 0; i < m->len; i++) {
        Str *k = (Str *)(intptr_t)rt_map_key_at(m, i, &rt_td_string);
        if ((size_t)k->len == n && memcmp(k->data, entry, n) == 0) return 1;
    }
    return 0;
}

static void envp_free(char **envp) {
    if (!envp) return;
    for (int64_t i = 0; envp[i]; i++) free(envp[i]);
    free(envp);
}

// The child's environment, argv-style, or NULL with errno set. A map entry
// replaces the parent entry of the same name: platforms disagree about two
// entries for one name. Callers must reject NULs and '=' in keys first.
static char **build_envp(Map *env, int clear) {
    int64_t base = 0;
    if (!clear) {
        while (environ[base]) base++;
    }
    int64_t extra = env ? env->len : 0;
    char **out = malloc((size_t)(base + extra + 1) * sizeof *out);
    if (!out) {
        errno = ENOMEM;
        return NULL;
    }
    int64_t n = 0;
    for (int64_t i = 0; i < base; i++) {
        // The map's own entry is appended below in its place.
        if (env && env_overrides(env, environ[i])) continue;
        if (!(out[n] = dup_c(environ[i]))) {
            out[n] = NULL;
            envp_free(out);
            errno = ENOMEM;
            return NULL;
        }
        n++;
    }
    for (int64_t i = 0; i < extra; i++) {
        Str *k = (Str *)(intptr_t)rt_map_key_at(env, i, &rt_td_string);
        Str *v = (Str *)(intptr_t)rt_map_val_at(env, i, &rt_td_string);
        if (!(out[n] = env_entry(k, v))) {
            out[n] = NULL;
            envp_free(out);
            errno = ENOMEM;
            return NULL;
        }
        n++;
    }
    out[n] = NULL;
    return out;
}

// Moves to `to`, leaving the malloc'd return directory in *saved. Answers 1, or
// 0 with errno set. There is no portable chdir file action, so the parent moves
// across the spawn; one thread runs (§5.2), so nothing observes the move.
static int cwd_push(const char *to, char **saved) {
    // A longer working directory reports ERANGE and is refused.
    char buf[4096];
    if (!nio_getcwd(buf, sizeof buf)) return 0;
    char *copy = dup_c(buf);
    if (!copy) {
        errno = ENOMEM;
        return 0;
    }
    if (nio_chdir(to) != 0) {
        int failed = errno;
        free(copy);
        errno = failed;
        return 0;
    }
    *saved = copy;
    return 1;
}

// errno is carried across, so a caller keeps its spawn failure reason. A failed
// return is fatal: later relative paths would resolve in the wrong directory.
static void cwd_pop(char *saved) {
    int carried = errno;
    if (nio_chdir(saved) != 0) rt_panic("process.child.run: cannot return to the working directory");
    free(saved);
    errno = carried;
}

typedef struct {
    char **cargv;
    char **envp; // NULL: the child inherits this program's environment
    FILE *inf;   // NULL: the child's input is the empty stream
    FILE *outf;  // NULL when inherit: nothing is collected, so nothing is made
    FILE *errf;
    int inherit; // the child is given this program's own three streams
} Spawn;

// errno is preserved for the failure report.
static void spawn_free(Spawn *s) {
    int carried = errno;
    if (s->cargv) cargv_free(s->cargv);
    envp_free(s->envp);
    if (s->inf) fclose(s->inf);
    if (s->outf) fclose(s->outf);
    if (s->errf) fclose(s->errf);
    errno = carried;
}

// Starting a child splits into a spawn and a wait, so several children can be
// outstanding at once. On Windows the CRT's spawn answers a process handle,
// which the system can wait on with a timeout.
#if defined(_WIN32)
// The CRT's spawn joins the arguments with spaces and quotes none of them, so
// each is quoted here by the rule the child's CRT parses with: a run of
// backslashes before a quote is doubled and the quote escaped, and a run at
// the end is doubled.
static char *win_quote(const char *a) {
    int plain = *a != 0;
    for (const char *p = a; plain && *p; p++) {
        if (*p == ' ' || *p == '\t' || *p == '"') plain = 0;
    }
    size_t n = strlen(a);
    char *out = malloc(2 * n + 3);
    if (!out) rt_panic("out of memory");
    if (plain) {
        memcpy(out, a, n + 1);
        return out;
    }
    char *o = out;
    *o++ = '"';
    size_t bs = 0;
    for (const char *p = a; *p; p++) {
        if (*p == '\\') {
            bs++;
            continue;
        }
        if (*p == '"') {
            for (size_t i = 0; i < 2 * bs + 1; i++) *o++ = '\\';
            *o++ = '"';
            bs = 0;
            continue;
        }
        for (size_t i = 0; i < bs; i++) *o++ = '\\';
        bs = 0;
        *o++ = *p;
    }
    for (size_t i = 0; i < 2 * bs; i++) *o++ = '\\';
    *o++ = '"';
    *o = 0;
    return out;
}

static char **win_quoted_argv(char **cargv) {
    int64_t n = 0;
    while (cargv[n]) n++;
    char **q = malloc((size_t)(n + 1) * sizeof *q);
    if (!q) rt_panic("out of memory");
    for (int64_t i = 0; i < n; i++) q[i] = win_quote(cargv[i]);
    q[n] = NULL;
    return q;
}

// The program is looked up by its unquoted name; the quoted copies become the
// child's command line.
static intptr_t crt_spawn(Spawn *sp, char **quoted) {
    return sp->envp ? _spawnvpe(_P_NOWAIT, sp->cargv[0], (const char *const *)quoted,
                                (const char *const *)sp->envp)
                    : _spawnvp(_P_NOWAIT, sp->cargv[0], (const char *const *)quoted);
}

// Starts the child and returns 1 with its process handle, or 0 with errno set.
static int spawn_child(Spawn *sp, intptr_t *handle) {
    // Flush first, or this program's buffered output surfaces after the child's.
    fflush(stdout);
    fflush(stderr);
    char **quoted = win_quoted_argv(sp->cargv);
    intptr_t h;
    if (sp->inherit) {
        // The CRT spawn inherits the current descriptors, so nothing is swapped.
        h = crt_spawn(sp, quoted);
        cargv_free(quoted);
        if (h == -1) return 0;
        *handle = h;
        return 1;
    }
    // NUL is the empty input when no stdin was given.
    int in = -1;
    if (sp->inf) {
        in = _dup(_fileno(sp->inf));
        if (in == -1) {
            cargv_free(quoted);
            return 0;
        }
    } else if (_sopen_s(&in, "NUL", _O_RDONLY, _SH_DENYNO, 0) != 0) {
        cargv_free(quoted);
        return 0;
    }
    int save0 = _dup(0), save1 = _dup(1), save2 = _dup(2);
    _dup2(in, 0);
    _dup2(_fileno(sp->outf), 1);
    _dup2(_fileno(sp->errf), 2);
    h = crt_spawn(sp, quoted);
    int spawn_errno = errno;
    _dup2(save0, 0);
    _dup2(save1, 1);
    _dup2(save2, 2);
    _close(save0);
    _close(save1);
    _close(save2);
    _close(in);
    cargv_free(quoted);
    if (h == -1) {
        errno = spawn_errno;
        return 0;
    }
    *handle = h;
    return 1;
}
#else
// Starts the child and returns 1 with its pid, or 0 with errno set.
static int spawn_child(Spawn *sp, pid_t *pid) {
    // With no file actions the child inherits this program's descriptors. Flush
    // first, or this program's buffered output surfaces after the child's.
    if (sp->inherit) {
        fflush(stdout);
        fflush(stderr);
        int rc = posix_spawnp(pid, sp->cargv[0], NULL, NULL, sp->cargv,
                              sp->envp ? sp->envp : environ);
        if (rc != 0) {
            errno = rc;
            return 0;
        }
        return 1;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    // /dev/null is the empty input when the call named no stdin.
    if (sp->inf) {
        posix_spawn_file_actions_adddup2(&fa, fileno(sp->inf), 0);
    } else {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    }
    posix_spawn_file_actions_adddup2(&fa, fileno(sp->outf), 1);
    posix_spawn_file_actions_adddup2(&fa, fileno(sp->errf), 2);
    int rc = posix_spawnp(pid, sp->cargv[0], &fa, NULL, sp->cargv,
                          sp->envp ? sp->envp : environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) {
        errno = rc; // posix_spawnp reports through its return value
        return 0;
    }
    return 1;
}

// 128+signal when a signal ended the child, as shells report it.
static int64_t status_code(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}
#endif

// These descriptors exist only to trace fu->result: the future holds the record
// after the call that made it returns. The offsets must match what codegen
// computes for types.ChildUsageT and types.ChildRunResultT. Every field of both
// records is 8 bytes wide, so §5.7's fold is 8 + i*8.
static const char *usage_field_names[USAGE_SLOTS] = {
    "peakMemory", "userTime", "systemTime", "minorFaults", "majorFaults",
    "blockReads", "blockWrites", "voluntarySwitches", "involuntarySwitches"};
static TypeDesc *usage_field_types[USAGE_SLOTS] = {
    &rt_td_int, &rt_td_duration, &rt_td_duration, &rt_td_int, &rt_td_int,
    &rt_td_int, &rt_td_int, &rt_td_int, &rt_td_int};
static const int64_t usage_field_offsets[USAGE_SLOTS] = {8,  16, 24, 32, 40,
                                                         48, 56, 64, 72};
static TypeDesc td_child_usage = {TD_RECORD, 0,    USAGE_SLOTS, usage_field_names,
                                  usage_field_types, 0, usage_field_offsets, 80};
static TypeDesc td_child_usage_opt = {TD_OPTIONAL, &td_child_usage, 0, NULL, NULL, 0};

static const char *result_field_names[RESULT_SLOTS] = {"stdout", "stderr", "code", "usage"};
static TypeDesc *result_field_types[RESULT_SLOTS] = {
    &td_byte_array, &td_byte_array, &rt_td_int, &td_child_usage_opt};
static const int64_t result_field_offsets[RESULT_SLOTS] = {8, 16, 24, 32};
static TypeDesc td_child_run_result = {TD_RECORD, 0,     RESULT_SLOTS, result_field_names,
                                       result_field_types, 0, result_field_offsets, 40};

// What the kernel charged the child, in an optional box, which is what a `T?`
// slot holds (§2.3). ru_maxrss is bytes on macOS and kibibytes elsewhere, so it
// is normalized to bytes. CPU time rounds to the millisecond a Duration counts.
#if !defined(_WIN32)
static int64_t usage_ms(const struct timeval *tv) {
    return (int64_t)tv->tv_sec * 1000 + (tv->tv_usec + 500) / 1000;
}

static void *build_usage(const struct rusage *ru) {
    void *rec = rt_rec_new(&td_child_usage);
#if defined(__APPLE__)
    rt_rec_set(rec, 0, (int64_t)ru->ru_maxrss, &td_child_usage);        // already bytes
#else
    rt_rec_set(rec, 0, (int64_t)ru->ru_maxrss * 1024, &td_child_usage); // kibibytes
#endif
    rt_rec_set(rec, 1, usage_ms(&ru->ru_utime), &td_child_usage);
    rt_rec_set(rec, 2, usage_ms(&ru->ru_stime), &td_child_usage);
    rt_rec_set(rec, 3, (int64_t)ru->ru_minflt, &td_child_usage);
    rt_rec_set(rec, 4, (int64_t)ru->ru_majflt, &td_child_usage);
    rt_rec_set(rec, 5, (int64_t)ru->ru_inblock, &td_child_usage);
    rt_rec_set(rec, 6, (int64_t)ru->ru_oublock, &td_child_usage);
    rt_rec_set(rec, 7, (int64_t)ru->ru_nvcsw, &td_child_usage);
    rt_rec_set(rec, 8, (int64_t)ru->ru_nivcsw, &td_child_usage);
    // The record is live across rt_box's allocation, so it is rooted.
    TypeDesc *tds[1] = {&td_child_usage};
    int64_t slots[1] = {(int64_t)(intptr_t)rec};
    GCFrame f2 = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f2;
    void *box = rt_box(slots[0], &td_child_usage);
    rt_gc_top = f2.prev;
    return box;
}
#endif

// Builds the ChildRunResult and completes the future with it. The temporary
// streams are read after the child is gone, and closed either way. `outf` is
// NULL for a child that inherited the streams, so both byte[] fields are empty.
static void finish_child(Future *fu, const char *cmd, FILE *outf, FILE *errf, int64_t code,
                        void *usage) {
    int64_t out_len = 0, err_len = 0;
    char *out_buf = NULL, *err_buf = NULL;
    if (outf) {
        out_buf = slurp_temp(outf, &out_len);
        err_buf = out_buf ? slurp_temp(errf, &err_len) : NULL;
        int saved = errno;
        fclose(outf);
        fclose(errf);
        if (!err_buf) {
            free(out_buf);
            errno = saved;
            rt_async_extern_done(fu, 0, NULL, run_errno_c(cmd));
            return;
        }
    }

    // The two arrays and the usage box are live across the allocations that
    // follow them, so all three are rooted; the record is filled with no
    // allocation in between. The extern list roots fu. The usage box was built
    // before this frame existed, so it goes in a slot rather than a C local.
    TypeDesc *tds[3] = {&td_byte_array, &td_byte_array, &td_child_usage_opt};
    int64_t slots[3] = {0, 0, (int64_t)(intptr_t)usage};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *out_arr = rt_arr_new(out_len, 1);
    memcpy(rt_arr_bytes(out_arr), out_buf, (size_t)out_len);
    slots[0] = (int64_t)(intptr_t)out_arr;

    Arr *err_arr = rt_arr_new(err_len, 1);
    memcpy(rt_arr_bytes(err_arr), err_buf, (size_t)err_len);
    slots[1] = (int64_t)(intptr_t)err_arr;

    void *result = rt_rec_new(&td_child_run_result);
    rt_rec_set(result, 0, slots[0], &td_child_run_result);
    rt_rec_set(result, 1, slots[1], &td_child_run_result);
    rt_rec_set(result, 2, code, &td_child_run_result);
    rt_rec_set(result, 3, slots[2], &td_child_run_result);

    rt_gc_top = f.prev;
    free(out_buf);
    free(err_buf);
    rt_async_extern_done(fu, (int64_t)(intptr_t)result, &td_child_run_result, NULL);
}

// Children started and not yet reaped. The linear scan is enough: the operating
// system's limit on concurrent children comes first.
typedef struct {
    intptr_t pid; // a pid; on Windows the process handle
    FILE *outf;
    FILE *errf;
    char *cmd;  // malloc'd copy, for the message if reading its output fails
    Future *fu; // rooted by the core's extern list
} Child;

static Child *children = NULL;
static int64_t nchildren = 0, children_cap = 0;

static void child_add(intptr_t pid, FILE *outf, FILE *errf, char *cmd, Future *fu) {
    if (nchildren == children_cap) {
        children_cap = children_cap ? children_cap * 2 : 8;
        children = realloc(children, (size_t)children_cap * sizeof *children);
        if (!children) rt_panic("out of memory");
    }
    children[nchildren++] = (Child){pid, outf, errf, cmd, fu};
}

static void child_remove(int64_t i) { children[i] = children[--nchildren]; }

#if !defined(_WIN32)
// Reaps the children that have finished, without blocking. Answers how many.
static int reap_ready(void) {
    int done = 0;
    for (int64_t i = 0; i < nchildren;) {
        int status = 0;
        struct rusage ru;
        memset(&ru, 0, sizeof ru);
        // wait4 reports the resource usage, which waitpid cannot. WNOHANG
        // keeps a running child from stopping the scheduler.
        pid_t r = wait4(children[i].pid, &status, WNOHANG, &ru);
        if (r == 0) { // still running
            i++;
            continue;
        }
        Child c = children[i];
        child_remove(i);
        if (r == -1) {
            if (c.outf) { // NULL when the child inherited the streams
                fclose(c.outf);
                fclose(c.errf);
            }
            rt_async_extern_done(c.fu, 0, NULL, run_errno_c(c.cmd));
        } else {
            finish_child(c.fu, c.cmd, c.outf, c.errf, status_code(status), build_usage(&ru));
        }
        free(c.cmd);
        done++;
    }
    return done;
}

// The rt_async_extern_wait hook (runtime.h): waits up to budget_ms for one child
// to finish. A negative budget blocks in wait4; a positive one polls in short
// slices, so a pending timer still completes on time.
static int process_extern_wait(int64_t budget_ms) {
    if (nchildren == 0) return 0;
    if (reap_ready()) return 1;
    if (budget_ms == 0) return 0;

    if (budget_ms < 0) {
        for (;;) {
            int status = 0;
            struct rusage ru;
            memset(&ru, 0, sizeof ru);
            pid_t r = wait4(-1, &status, 0, &ru);
            if (r == -1) {
                if (errno == EINTR) continue;
                return reap_ready() > 0; // no children left to wait for
            }
            for (int64_t i = 0; i < nchildren; i++) {
                if (children[i].pid != r) continue;
                Child c = children[i];
                child_remove(i);
                finish_child(c.fu, c.cmd, c.outf, c.errf, status_code(status),
                             build_usage(&ru));
                free(c.cmd);
                return 1;
            }
            // The child is not in the table, so the wait continues.
        }
    }

    for (int64_t waited = 0; waited < budget_ms;) {
        int64_t slice = budget_ms - waited < 2 ? budget_ms - waited : 2;
        struct timespec ts = {slice / 1000, (slice % 1000) * 1000000};
        while (nanosleep(&ts, &ts) != 0) {
        }
        waited += slice;
        if (reap_ready()) return 1;
    }
    return 0;
}
#else
// A FILETIME counts 100-nanosecond units. Rounded to the millisecond a
// Duration counts.
static int64_t filetime_ms(const FILETIME *ft) {
    uint64_t t = ((uint64_t)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    return (int64_t)((t + 5000) / 10000);
}

// What the system charged the child, in an optional box. Windows reports the
// peak working set, the CPU times and a page fault count; the other five
// fields have no counterpart and stay 0.
static void *build_usage_win(HANDLE h) {
    FILETIME created, ended, kernel, user;
    PROCESS_MEMORY_COUNTERS mem;
    memset(&mem, 0, sizeof mem);
    mem.cb = sizeof mem;
    if (!GetProcessTimes(h, &created, &ended, &kernel, &user)) return NULL;
    if (!GetProcessMemoryInfo(h, &mem, sizeof mem)) return NULL;
    void *rec = rt_rec_new(&td_child_usage);
    rt_rec_set(rec, 0, (int64_t)mem.PeakWorkingSetSize, &td_child_usage);
    rt_rec_set(rec, 1, filetime_ms(&user), &td_child_usage);
    rt_rec_set(rec, 2, filetime_ms(&kernel), &td_child_usage);
    rt_rec_set(rec, 3, (int64_t)mem.PageFaultCount, &td_child_usage);
    for (int i = 4; i < USAGE_SLOTS; i++) rt_rec_set(rec, i, 0, &td_child_usage);
    // The record is live across rt_box's allocation, so it is rooted.
    TypeDesc *tds[1] = {&td_child_usage};
    int64_t slots[1] = {(int64_t)(intptr_t)rec};
    GCFrame f2 = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f2;
    void *box = rt_box(slots[0], &td_child_usage);
    rt_gc_top = f2.prev;
    return box;
}

// Reaps the children that have finished, without blocking. Answers how many.
static int reap_ready(void) {
    int done = 0;
    for (int64_t i = 0; i < nchildren;) {
        HANDLE h = (HANDLE)children[i].pid;
        DWORD w = WaitForSingleObject(h, 0);
        if (w == WAIT_TIMEOUT) { // still running
            i++;
            continue;
        }
        Child c = children[i];
        child_remove(i);
        DWORD code = 0;
        if (w != WAIT_OBJECT_0 || !GetExitCodeProcess(h, &code)) {
            CloseHandle(h);
            if (c.outf) { // NULL when the child inherited the streams
                fclose(c.outf);
                fclose(c.errf);
            }
            errno = EINVAL;
            rt_async_extern_done(c.fu, 0, NULL, run_errno_c(c.cmd));
        } else {
            void *usage = build_usage_win(h);
            CloseHandle(h);
            // The CRT reports the exit code as an int. The cast gives a crash
            // status the same value that _cwait gives.
            finish_child(c.fu, c.cmd, c.outf, c.errf, (int64_t)(int32_t)code, usage);
        }
        free(c.cmd);
        done++;
    }
    return done;
}

// The rt_async_extern_wait hook (runtime.h): waits up to budget_ms for one child
// to finish. A negative budget blocks until one does. The system wait takes at
// most MAXIMUM_WAIT_OBJECTS handles, so with more children than that it looks
// in short slices, and every child is reaped by the scan that follows.
static int process_extern_wait(int64_t budget_ms) {
    if (nchildren == 0) return 0;
    if (reap_ready()) return 1;
    if (budget_ms == 0) return 0;
    int64_t left = budget_ms;
    for (;;) {
        HANDLE hs[MAXIMUM_WAIT_OBJECTS];
        DWORD n = nchildren < MAXIMUM_WAIT_OBJECTS ? (DWORD)nchildren : MAXIMUM_WAIT_OBJECTS;
        for (DWORD i = 0; i < n; i++) hs[i] = (HANDLE)children[i].pid;
        DWORD slice = INFINITE;
        if (left >= 0) slice = left > 0x7fffffff ? 0x7fffffff : (DWORD)left;
        if (nchildren > (int64_t)n && slice > 2) slice = 2;
        DWORD w = WaitForMultipleObjects(n, hs, FALSE, slice);
        if (reap_ready()) return 1;
        if (w == WAIT_FAILED) return 0;
        if (left >= 0) {
            left -= slice;
            if (left <= 0) return 0;
        }
    }
}
#endif

// Rejects what cannot survive the trip through C: a NUL byte in any spawn
// string, and an '=' in an environment name, which would name a different
// variable. Completes fu with the Error and answers 0.
static int reject_bad_strings(Future *fu, Str *cmd, Arr *args, Map *env) {
    const char *bad = NULL;
    if (str_has_nul(cmd)) {
        bad = "a program name cannot contain a NUL byte";
    }
    for (int64_t i = 0; !bad && i < args->len; i++) {
        if (str_has_nul((Str *)(intptr_t)rt_arr_units(args)[i])) bad = "an argument cannot contain a NUL byte";
    }
    for (int64_t i = 0; !bad && env && i < env->len; i++) {
        Str *k = (Str *)(intptr_t)rt_map_key_at(env, i, &rt_td_string);
        if (str_has_nul(k) || str_has_nul((Str *)(intptr_t)rt_map_val_at(env, i, &rt_td_string))) {
            bad = "an environment name or value cannot contain a NUL byte";
        } else if (memchr(k->data, '=', (size_t)k->len)) {
            bad = "an environment name cannot contain '='";
        }
    }
    if (!bad) return 1;
    char msg[256];
    snprintf(msg, sizeof msg, "process.child.run: %s", bad);
    rt_async_extern_done(fu, 0, NULL, rt_error_new(msg, NIO_ERR_INVALID));
    return 0;
}

// Starts cmd with args and answers the future of its result. The call never
// fails: every failure completes the future with the Error, so a caller sees it
// at the await (§2.9, §5.2). A program that ran is not a failure; its exit code
// is a field of the result. cmd, the arguments and the options are copied into C
// memory up front, so nothing here roots args or opts.
Future *rt_process_run(Str *cmd, Arr *args, int64_t *opts) {
    // First, so every failure below has somewhere to go and the future is a root
    // before anything else allocates. The waiter names this library as the
    // owner, which keeps a program that also holds a socket moving (runtime.h).
    Future *fu = rt_async_extern_new(process_extern_wait, NULL, NULL);

    Arr *in_bytes = opt_val(opts, 0);
    Str *cwd = opt_val(opts, 1);
    Map *env = opt_val(opts, 2);
    int clear_env = opt_flag(opts, 3);
    int inherit = opt_flag(opts, 4);
    // An inheriting child reads this program's input directly, so a `stdin`
    // given as well has nothing to be. §6.12 ignores it; the checker cannot see
    // the combination, since both fields are ordinary optionals.
    if (inherit) in_bytes = NULL;

    if (!reject_bad_strings(fu, cmd, args, env)) return fu;
    if (cwd && str_has_nul(cwd)) {
        rt_async_extern_done(fu, 0, NULL,
                             rt_error_new("process.child.run: a directory cannot contain a NUL byte", NIO_ERR_INVALID));
        return fu;
    }

    Spawn sp = {NULL, NULL, NULL, NULL, NULL, inherit};
    sp.cargv = malloc((size_t)(args->len + 2) * sizeof *sp.cargv);
    if (!sp.cargv) rt_panic("out of memory");
    sp.cargv[0] = dup_str(cmd);
    for (int64_t i = 0; i < args->len; i++) {
        sp.cargv[i + 1] = dup_str((Str *)(intptr_t)rt_arr_units(args)[i]);
    }
    sp.cargv[args->len + 1] = NULL;

    if (in_bytes && !(sp.inf = stdin_stream(in_bytes))) {
        spawn_free(&sp);
        rt_async_extern_done(fu, 0, NULL,
                             run_error(cmd, "cannot create a temporary file for its input",
                                       rt_err_from_errno(errno)));
        return fu;
    }
    // An inheriting child writes where this program writes; nothing is collected.
    if (!inherit) {
        sp.outf = temp_stream();
        sp.errf = sp.outf ? temp_stream() : NULL;
        if (!sp.errf) {
            spawn_free(&sp);
            rt_async_extern_done(fu, 0, NULL,
                                 run_error(cmd, "cannot create a temporary file for its output",
                                           rt_err_from_errno(errno)));
            return fu;
        }
    }
    if ((env || clear_env) && !(sp.envp = build_envp(env, clear_env))) {
        spawn_free(&sp);
        rt_async_extern_done(fu, 0, NULL, run_errno(cmd));
        return fu;
    }

    // A copy of the name that outlives this call, for the reaping half.
    char *cmdc = dup_str(cmd);

    // The child looks `cmd` up in the directory it will run in, as a shell does.
    char *saved_cwd = NULL;
    if (cwd) {
        char *to = dup_str(cwd);
        int moved = cwd_push(to, &saved_cwd);
        if (!moved) {
            // The message names the directory. A "no such file" message about
            // a program that exists points the reader at the wrong cause.
            int e = errno;
            char reason[512];
            snprintf(reason, sizeof reason, "cannot enter \"%.200s\": %s", to, strerror(e));
            free(to);
            spawn_free(&sp);
            rt_async_extern_done(fu, 0, NULL, run_error_c(cmdc, reason, rt_err_from_errno(e)));
            free(cmdc);
            return fu;
        }
        free(to);
    }

#if defined(_WIN32)
    intptr_t pid; // the process handle the CRT's spawn answers
#else
    pid_t pid;
#endif
    int ok = spawn_child(&sp, &pid);
    if (saved_cwd) cwd_pop(saved_cwd);
    if (!ok) {
        spawn_free(&sp);
        rt_async_extern_done(fu, 0, NULL, run_errno_c(cmdc));
        free(cmdc);
        return fu;
    }
    // The child holds its own descriptors now, so everything but the two this
    // program still reads is given back.
    FILE *outf = sp.outf, *errf = sp.errf;
    sp.outf = sp.errf = NULL;
    spawn_free(&sp);
    child_add(pid, outf, errf, cmdc, fu);
    return fu;
}
