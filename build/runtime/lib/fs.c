// The `fs` module (import 'fs'). The signatures must match fsCallType in the
// checker and genFSCall in codegen.
//
// Each fallible function takes a trailing `void **err` out-parameter. On a
// failure it stores an Error through it, and its return value is undefined.
// Codegen tests the slot and raises (§2.9). rt_fs_exists cannot fail: it
// returns false for a path it cannot reach.
//
// A path arrives as a counted byte string with no NUL terminator. Each
// function copies it into a C string first (cpath). A path that holds a NUL is
// refused. Truncation would open "etc/passwd" for "etc/passwd\0.txt".
//
// On Windows the calls come from the CRT and not from <windows.h>, so errno
// reporting has one code path.
//
// Whole-file I/O uses descriptor calls instead of stdio. read_all and
// write_all loop over a short read or write.

#include "runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#include <process.h>
#define nio_stat_t struct _stat64
#define nio_stat _stat64
#define nio_fstat _fstat64
#define nio_rmdir _rmdir
// _O_BINARY stops the CRT from changing \n to \r\n on a write and back on a
// read. Without it, a byte[] written to a file can read back different.
#define nio_open(p, f) _open((p), (f), _S_IREAD | _S_IWRITE)
#define nio_read _read
#define nio_write _write
#define nio_close _close
#define NIO_O_RD (_O_RDONLY | _O_BINARY)
#define NIO_O_WR (_O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY)
typedef int nio_ssize;
typedef unsigned nio_iolen;
// _mkdir takes no mode. A new directory inherits from its parent.
#define nio_mkdir(p) _mkdir(p)
// The CRT has no lstat, so this follows a junction. A recursive delete calls
// _rmdir on each directory first. _rmdir removes a junction and keeps its
// target.
#define nio_lstat _stat64
#define NIO_SEP '\\'
#else
#include <dirent.h>
#include <unistd.h>
#define nio_stat_t struct stat
#define nio_stat stat
#define nio_fstat fstat
#define nio_rmdir rmdir
#define nio_open(p, f) open((p), (f), 0666)
#define nio_read read
#define nio_write write
#define nio_close close
#define NIO_O_RD O_RDONLY
#define NIO_O_WR (O_WRONLY | O_CREAT | O_TRUNC)
typedef ssize_t nio_ssize;
typedef size_t nio_iolen;
// The process umask narrows 0777, as it narrows the 0666 of nio_open.
#define nio_mkdir(p) mkdir((p), 0777)
// A recursive delete uses lstat so that it removes a symlink itself and does
// not follow it out of the tree.
#define nio_lstat lstat
#define NIO_SEP '/'
#endif

// The descriptor of the readDir result. The array is rooted with it while its
// entries are built.
static TypeDesc td_string_array = {TD_ARRAY, &rt_td_string, 0, NULL, NULL, 0};

// The largest count that one read or write asks for, because the count is an
// int on Windows. The callers loop until the transfer is complete.
#define NIO_IO_MAX 0x7ffff000

// An fs.Stat record has seven fields in declaration order: [0] name,
// [1] extension, [2] size, [3] isDirectory, [4] isFile, [5] permissions,
// [6] modified. It must match types.StatT.
#define STAT_SLOTS 7

// The descriptor of fs.Stat. Each record that this file builds carries it,
// because json.c reads the shape of a record from the value (REC_TD,
// runtime.h). The names must match types.StatT. The offsets must match the
// layout that codegen computes (§5.7): one byte for each bool at 32 and 33,
// then permissions aligned up to 40.
static const char *stat_field_names[STAT_SLOTS] = {
    "name", "extension", "size", "isDirectory", "isFile", "permissions", "modified",
};
static TypeDesc *stat_field_types[STAT_SLOTS] = {
    &rt_td_string, &rt_td_string, &rt_td_int,  &rt_td_bool,
    &rt_td_bool,   &rt_td_string, &rt_td_datetime,
};
static const int64_t stat_field_offsets[STAT_SLOTS] = {8, 16, 24, 32, 33, 40, 48};
static TypeDesc td_stat = {TD_RECORD, 0,   STAT_SLOTS, stat_field_names,
                           stat_field_types, 0, stat_field_offsets, 56};

// The maximum length of a failure message. A long path is truncated in the
// message only.
#define MSG_MAX 1024

// Builds `fs.<op> "<path>": <reason>`. The path is in quotes, so a name that
// is only spaces is still visible.
static void *fs_error(const char *op, Str *path, const char *reason, int64_t code) {
    char msg[MSG_MAX];
    // The path can hold a NUL, so %.*s limits it by its length.
    int64_t shown = path->len > 200 ? 200 : path->len;
    snprintf(msg, sizeof msg, "fs.%s \"%.*s\": %s", op, (int)shown, path->data, reason);
    return rt_error_new(msg, code);
}

// Reads errno once, before strerror can change it. The message and the code
// both use that value.
static void *fs_errno(const char *op, Str *path) {
    int e = errno;
    return fs_error(op, path, strerror(e), rt_err_from_errno(e));
}

// The same as fs_error, for a C path that this file built. A recursive delete
// names the entry that failed, and not the root of the tree.
static void *fs_error_c(const char *op, const char *path, const char *reason, int64_t code) {
    char msg[MSG_MAX];
    snprintf(msg, sizeof msg, "fs.%s \"%.200s\": %s", op, path, reason);
    return rt_error_new(msg, code);
}

static void *fs_errno_c(const char *op, const char *path) {
    int e = errno;
    return fs_error_c(op, path, strerror(e), rt_err_from_errno(e));
}

// Both paths in one message: `fs.<op> "<from>" -> "<to>": <reason>`.
static void *fs_error2(const char *op, Str *from, Str *to, const char *reason, int64_t code) {
    char msg[MSG_MAX];
    int64_t fshown = from->len > 200 ? 200 : from->len;
    int64_t tshown = to->len > 200 ? 200 : to->len;
    snprintf(msg, sizeof msg, "fs.%s \"%.*s\" -> \"%.*s\": %s", op,
             (int)fshown, from->data, (int)tshown, to->data, reason);
    return rt_error_new(msg, code);
}

static void *fs_errno2(const char *op, Str *from, Str *to) {
    int e = errno;
    return fs_error2(op, from, to, strerror(e), rt_err_from_errno(e));
}

static int has_nul(Str *path) {
    return memchr(path->data, 0, (size_t)path->len) != NULL;
}

// Copies path into a malloc'd C string and adds the NUL terminator. The
// collector does not see this memory. The caller must refuse a path that
// holds a NUL first.
static char *dup_path(Str *path) {
    char *c = malloc((size_t)path->len + 1);
    if (!c) rt_panic("out of memory"); // not a file-system failure, so no Error
    memcpy(c, path->data, (size_t)path->len);
    c[path->len] = 0;
    return c;
}

// Returns path as a C string. If path holds a NUL, it stores an Error and
// returns NULL. Each fallible function here calls it first.
static char *cpath(const char *op, Str *path, void **err) {
    if (has_nul(path)) {
        // This message does not quote the path. An Error message is a C
        // string, so a NUL would cut it.
        char msg[64];
        snprintf(msg, sizeof msg, "fs.%s: a path cannot contain a NUL byte", op);
        *err = rt_error_new(msg, NIO_ERR_INVALID);
        return NULL;
    }
    return dup_path(path);
}

static int is_sep(char c) { return c == '/' || c == '\\'; }

// This function cannot fail. Each stat failure returns false, and a path that
// holds a NUL names no file.
int64_t rt_fs_exists(Str *path) {
    if (has_nul(path)) return 0;
    char *c = dup_path(path);
    nio_stat_t st;
    int ok = nio_stat(c, &st) == 0;
    free(c);
    return ok;
}

// Entry names in malloc memory instead of a Nio array. An allocation of a Nio
// string while the directory handle is open can start a collection.
typedef struct {
    char **names;
    int64_t len, cap;
} NameList;

static void names_free(NameList *l) {
    for (int64_t i = 0; i < l->len; i++) free(l->names[i]);
    free(l->names);
}

// Returns 0 if memory runs out. It frees nothing: the caller frees the list.
static int names_add(NameList *l, const char *name) {
    if (l->len == l->cap) {
        int64_t cap = l->cap ? l->cap * 2 : 16;
        char **grown = realloc(l->names, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        l->names = grown;
        l->cap = cap;
    }
    size_t n = strlen(name);
    char *copy = malloc(n + 1);
    if (!copy) return 0;
    memcpy(copy, name, n + 1);
    l->names[l->len++] = copy;
    return 1;
}

static int is_dot_entry(const char *name) {
    return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0));
}

// Lists dir into l. Returns 1, or 0 with errno set to the reason.
static int list_dir(const char *dir, NameList *l) {
#if defined(_WIN32)
    // _findfirst64 needs a wildcard pattern and reports through errno.
    size_t n = strlen(dir);
    char *pattern = malloc(n + 3);
    if (!pattern) {
        errno = ENOMEM;
        return 0;
    }
    memcpy(pattern, dir, n);
    size_t at = n;
    if (n == 0 || !is_sep(dir[n - 1])) pattern[at++] = '\\';
    pattern[at++] = '*';
    pattern[at] = 0;

    struct __finddata64_t entry;
    intptr_t h = _findfirst64(pattern, &entry);
    free(pattern);
    if (h == -1) {
        // An empty directory still holds "." and "..". If the pattern matches
        // nothing, the directory is missing or cannot be read.
        return 0;
    }
    int ok = 1;
    do {
        if (is_dot_entry(entry.name)) continue;
        if (!names_add(l, entry.name)) {
            errno = ENOMEM;
            ok = 0;
            break;
        }
    } while (_findnext64(h, &entry) == 0);
    _findclose(h);
    return ok;
#else
    DIR *d = opendir(dir);
    if (!d) return 0;
    int ok = 1;
    // readdir returns NULL at the end and on an error. Clear errno first and
    // read it after to tell the two apart.
    errno = 0;
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        if (is_dot_entry(e->d_name)) continue;
        if (!names_add(l, e->d_name)) {
            errno = ENOMEM;
            ok = 0;
            break;
        }
    }
    if (ok && errno != 0) ok = 0;
    int saved = errno;
    closedir(d);
    errno = saved;
    return ok;
#endif
}

// Returns the entry names in the order that the platform reports them. The
// order is not defined.
Arr *rt_fs_read_dir(Str *path, void **err) {
    char *c = cpath("readDir", path, err);
    if (!c) return NULL;

    NameList list = {NULL, 0, 0};
    if (!list_dir(c, &list)) {
        *err = fs_errno("readDir", path);
        names_free(&list);
        free(c);
        return NULL;
    }
    free(c);

    // The array is rooted with its real descriptor. The slots not yet written
    // are zero, and the trace skips null pointers. path is rooted too, because
    // a C parameter is not a root. Each function here roots its parameters.
    TypeDesc *tds[2] = {&rt_td_string, &td_string_array};
    int64_t slots[2] = {(int64_t)(intptr_t)path, 0};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *a = rt_arr_new(list.len, 8);
    slots[1] = (int64_t)(intptr_t)a;
    for (int64_t i = 0; i < list.len; i++) {
        rt_arr_units(a)[i] = (int64_t)(intptr_t)rt_str_from_c(list.names[i]);
    }

    rt_gc_top = f.prev;
    names_free(&list);
    return a;
}

// Reads fd into a malloc'd buffer and stores the length in *out_len. Returns
// NULL with errno set on a failure. The hint (the stat size) sets only the
// initial capacity. A file in /proc reports size zero, so the loop reads until
// a read returns 0.
static char *read_all(int fd, int64_t hint, int64_t *out_len) {
    // One byte more than the hint, so a file of that size needs no growth.
    size_t cap = hint > 0 ? (size_t)hint + 1 : 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        errno = ENOMEM;
        return NULL;
    }
    for (;;) {
        if (len == cap) {
            size_t grown = cap * 2;
            char *nb = realloc(buf, grown);
            if (!nb) {
                free(buf);
                errno = ENOMEM;
                return NULL;
            }
            buf = nb;
            cap = grown;
        }
        size_t room = cap - len;
        if (room > NIO_IO_MAX) room = NIO_IO_MAX;
        nio_ssize got = nio_read(fd, buf + len, (nio_iolen)room);
        if (got < 0) {
            if (errno == EINTR) continue; // a signal interrupted the read
            free(buf);
            return NULL; // errno is the value the read set
        }
        if (got == 0) break;
        len += (size_t)got;
    }
    *out_len = (int64_t)len;
    return buf;
}

// Loops over short writes. A write can accept fewer bytes than it gets.
static int write_all(int fd, const char *buf, int64_t len) {
    for (int64_t off = 0; off < len;) {
        int64_t want = len - off;
        if (want > (int64_t)NIO_IO_MAX) want = (int64_t)NIO_IO_MAX;
        nio_ssize put = nio_write(fd, buf + off, (nio_iolen)want);
        if (put < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += put;
    }
    return 0;
}

// Returns the bytes of the file. byte is uint8 (§2.1), so each element is
// 0..255.
Arr *rt_fs_read_file(Str *path, void **err) {
    char *c = cpath("readFile", path, err);
    if (!c) return NULL;

    // On POSIX a directory opens and the read then fails. On Windows the open
    // fails with a permission error. Both give IS_DIRECTORY. The probe runs
    // only after a failed open, so a successful read costs no extra call.
    int fd = nio_open(c, NIO_O_RD);
    if (fd < 0) {
        nio_stat_t probe;
        if (nio_stat(c, &probe) == 0 && (probe.st_mode & S_IFMT) == S_IFDIR) {
            *err = fs_error("readFile", path, "is a directory", NIO_ERR_IS_DIRECTORY);
        } else {
            *err = fs_errno("readFile", path);
        }
        free(c);
        return NULL;
    }
    free(c);

    nio_stat_t st;
    int stated = nio_fstat(fd, &st) == 0;
    if (stated && (st.st_mode & S_IFMT) == S_IFDIR) {
        nio_close(fd);
        *err = fs_error("readFile", path, "is a directory", NIO_ERR_IS_DIRECTORY);
        return NULL;
    }

    int64_t len = 0;
    char *buf = read_all(fd, stated ? (int64_t)st.st_size : 0, &len);
    if (!buf) {
        int saved = errno;
        nio_close(fd);
        errno = saved;
        *err = fs_errno("readFile", path);
        return NULL;
    }
    nio_close(fd);

    // The bytes are in malloc memory, so the array needs no root. path is
    // rooted across the allocation.
    TypeDesc *tds[1] = {&rt_td_string};
    int64_t slots[1] = {(int64_t)(intptr_t)path};
    GCFrame f2 = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f2;

    // A byte[] stores one byte per element (§5.7), so one memcpy fills it.
    Arr *a = rt_arr_new(len, 1);
    memcpy(rt_arr_bytes(a), buf, (size_t)len);

    rt_gc_top = f2.prev;
    free(buf);
    return a;
}

// ---- fs.writeFile(path, content) ----

// Nothing needs a root: the only allocation is the Error on a failure path. A
// byte[] stores one byte per element (§5.7), so the array storage goes to the
// system call with no buffer between them.
void rt_fs_write_file(Str *path, Arr *content, void **err) {
    char *c = cpath("writeFile", path, err);
    if (!c) return;

    int fd = nio_open(c, NIO_O_WR);
    free(c);
    if (fd < 0) {
        *err = fs_errno("writeFile", path);
        return;
    }

    int failed = write_all(fd, (const char *)rt_arr_bytes(content), content->len);
    int saved = errno;
    // close can report a delayed write failure, so a failed close is a failed
    // write.
    if (nio_close(fd) != 0 && !failed) {
        failed = -1;
        saved = errno;
    }
    if (failed) {
        errno = saved;
        *err = fs_errno("writeFile", path);
    }
}

// ---- fs.rename(from, to) ----

// Replaces an entry that is already at the destination. On POSIX the
// replacement is atomic, so write-then-rename is an atomic save: after a
// crash, the old file or the new file is there. The Windows CRT rename refuses
// an existing destination. There, the destination is removed and the rename
// is tried again. The result is the same, but the operation is not atomic.
void rt_fs_rename(Str *from, Str *to, void **err) {
    char *cf = cpath("rename", from, err);
    if (!cf) return;
    char *ct = cpath("rename", to, err);
    if (!ct) {
        free(cf);
        return;
    }
    int failed = rename(cf, ct) != 0;
#if defined(_WIN32)
    if (failed && (errno == EEXIST || errno == EACCES)) {
        if (remove(ct) == 0) failed = rename(cf, ct) != 0;
    }
#endif
    if (failed) *err = fs_errno2("rename", from, to);
    free(cf);
    free(ct);
}

// ---- fs.copy(from, to) ----

// Copies through a fixed buffer, so the memory use does not depend on the file
// size. A directory source is refused, as in readFile. No metadata is copied,
// as writeFile sets none.
void rt_fs_copy(Str *from, Str *to, void **err) {
    char *cf = cpath("copy", from, err);
    if (!cf) return;
    char *ct = cpath("copy", to, err);
    if (!ct) {
        free(cf);
        return;
    }

    int in = nio_open(cf, NIO_O_RD);
    if (in < 0) {
        // The same directory probe as readFile. POSIX opens a directory and
        // fails later. Windows reports a permission error.
        nio_stat_t probe;
        if (nio_stat(cf, &probe) == 0 && (probe.st_mode & S_IFMT) == S_IFDIR) {
            *err = fs_error("copy", from, "is a directory", NIO_ERR_IS_DIRECTORY);
        } else {
            *err = fs_errno2("copy", from, to);
        }
        free(cf);
        free(ct);
        return;
    }
    nio_stat_t st;
    if (nio_fstat(in, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
        nio_close(in);
        *err = fs_error("copy", from, "is a directory", NIO_ERR_IS_DIRECTORY);
        free(cf);
        free(ct);
        return;
    }

    int out = nio_open(ct, NIO_O_WR);
    if (out < 0) {
        int saved = errno;
        nio_close(in);
        errno = saved;
        *err = fs_errno2("copy", from, to);
        free(cf);
        free(ct);
        return;
    }
    free(cf);
    free(ct);

    enum { COPY_BUF = 1 << 18 };
    char *buf = malloc(COPY_BUF);
    if (!buf) rt_panic("out of memory");
    int failed = 0;
    for (;;) {
        nio_ssize got = nio_read(in, buf, COPY_BUF);
        if (got < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            break;
        }
        if (got == 0) break;
        if (write_all(out, buf, (int64_t)got) != 0) {
            failed = 1;
            break;
        }
    }
    int saved = errno;
    free(buf);
    nio_close(in);
    // close can report a delayed write failure, so a failed close of the
    // destination is a failed copy.
    if (nio_close(out) != 0 && !failed) {
        failed = 1;
        saved = errno;
    }
    if (failed) {
        errno = saved;
        *err = fs_errno2("copy", from, to);
    }
}

// ---- fs.createDir(path) ----

// Makes one directory. The parent must exist. Missing parents are not
// created, so a typo in the path fails.
//
// An entry that already exists at path gives EEXIST, which is reported like
// any other failure. Only the operating system can test and create in one
// step. If EEXIST were ignored, a program could take a file for a directory.
void rt_fs_create_dir(Str *path, void **err) {
    char *c = cpath("createDir", path, err);
    if (!c) return;
    if (nio_mkdir(c) != 0) *err = fs_errno("createDir", path);
    free(c);
}

// ---- fs.createTempDir(prefix) ----

// Reads the variable on each call, so a change by the program or a test
// harness takes effect. POSIX uses TMPDIR. The CRT uses TMP, then TEMP.
static const char *temp_root(void) {
#if defined(_WIN32)
    const char *e = getenv("TMP");
    if (!e || !*e) e = getenv("TEMP");
    if (!e || !*e) e = ".";
    return e;
#else
    const char *e = getenv("TMPDIR");
    if (!e || !*e) e = "/tmp";
    return e;
#endif
}

// Makes a new, unique directory in the system temporary directory. The
// operating system makes it unique: mkdtemp picks the name and creates the
// directory in one atomic step, so two racing programs cannot both get it.
// The CRT has no mkdtemp, so on Windows a loop tries names. _mkdir refuses an
// existing entry, which makes each try atomic. The name is built here and not
// by _tempnam, because _tempnam reads TMP itself and can fall back to the
// working directory. Both can put the directory outside the root above.
//
// Nothing removes the directory. Use fs.delete with { recursive: true }.
Str *rt_fs_create_temp_dir(Str *prefix, void **err) {
    // The argument is a prefix and not a path, so these two errors use their
    // own words instead of the words of cpath.
    if (has_nul(prefix)) {
        *err = rt_error_new("fs.createTempDir: a prefix cannot contain a NUL byte", NIO_ERR_INVALID);
        return NULL;
    }
    char *p = dup_path(prefix);
    // A prefix is a name. A separator would put the directory outside the
    // temporary directory. A caller that wants that uses fs.createDir.
    for (const char *q = p; *q; q++) {
        if (!is_sep(*q)) continue;
        free(p);
        *err = rt_error_new("fs.createTempDir: a prefix cannot contain a path separator",
                            NIO_ERR_INVALID);
        return NULL;
    }

    const char *root = temp_root();
    // Drop trailing separators from the root, so the returned path has no
    // doubled separator.
    size_t rlen = strlen(root);
    while (rlen > 0 && is_sep(root[rlen - 1])) rlen--;

    char path[4096];
#if defined(_WIN32)
    int n = snprintf(path, sizeof path, "%.*s%c%sXXXXXX", (int)rlen, root, NIO_SEP, p);
#else
    // mkdtemp replaces the six X characters at the end.
    int n = snprintf(path, sizeof path, "%.*s%c%sXXXXXX", (int)rlen, root, NIO_SEP, p);
#endif
    if (n < 0 || (size_t)n >= sizeof path) {
        free(p);
        errno = ENAMETOOLONG;
        *err = fs_errno_c("createTempDir", root);
        return NULL;
    }

#if defined(_WIN32)
    // The CRT has no mkdtemp, so this code chooses the six characters. _mkdir
    // refuses an existing entry, which makes each try atomic. The number of
    // tries has a limit: after that many, the failure is not a collision. The
    // clock and the process id seed the counter, so two programs that start
    // together try different names.
    static const char ALPHABET[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    static uint64_t seq = 0;
    if (seq == 0) seq = (uint64_t)rt_mono_ms() * 2654435761u + (uint64_t)_getpid();
    char *slot = path + n - 6;
    Str *out = NULL;
    for (int attempt = 0; attempt < 100; attempt++) {
        seq = seq * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t v = seq >> 16;
        for (int i = 0; i < 6; i++) {
            slot[i] = ALPHABET[v % 36];
            v /= 36;
        }
        if (nio_mkdir(path) == 0) {
            out = rt_str_from_c(path);
            break;
        }
        if (errno != EEXIST) break;
    }
    if (!out) {
        // The message names the template, as on POSIX, so a missing parent
        // gives the same message on both hosts.
        int e = errno;
        snprintf(path, sizeof path, "%.*s%c%sXXXXXX", (int)rlen, root, NIO_SEP, p);
        errno = e;
        *err = fs_errno_c("createTempDir", path);
    }
    free(p);
    return out;
#else
    if (!mkdtemp(path)) {
        // POSIX leaves the template undefined after a failure, so the message
        // is built again from its parts. errno is kept across snprintf, which
        // can change it.
        int e = errno;
        snprintf(path, sizeof path, "%.*s%c%sXXXXXX", (int)rlen, root, NIO_SEP, p);
        errno = e;
        *err = fs_errno_c("createTempDir", path);
        free(p);
        return NULL;
    }
    free(p);
    // No program value is live across this allocation.
    return rt_str_from_c(path);
#endif
}

// ---- fs.delete(path, options) ----

// The options record has one field: [0] recursive, a `bool?`. It must match
// types.DeleteOptionsT. A null record means that the call gave no options.
//
// This file reads the record, and codegen does not convert it to a flag. This
// keeps the null test out of the emitted IR.

// Returns whether the bool? in field `at` is present and true. An absent
// record, an absent field and a null field all return false.
static int opt_flag(void *opts, int at) {
    if (!opts) return 0;
    TypeDesc *td = REC_TD(opts);
    void *box = (void *)(intptr_t)rt_rec_get(opts, at, td);
    // The box of a present bool? is one byte (§5.7). rt_box_get reads it at
    // the width that the field descriptor gives.
    return box && rt_box_get(box, td->field_types[at]->elem) != 0;
}

// Returns dir + name in a new malloc'd C string, or NULL if memory runs out.
static char *join_c(const char *dir, const char *name) {
    size_t d = strlen(dir), n = strlen(name);
    size_t sep = (d > 0 && !is_sep(dir[d - 1])) ? 1 : 0;
    char *out = malloc(d + sep + n + 1);
    if (!out) return NULL;
    memcpy(out, dir, d);
    if (sep) out[d] = NIO_SEP;
    memcpy(out + d + sep, name, n + 1);
    return out;
}

// Records the path where a walk failed, if no path is recorded yet. Only the
// innermost failure sets *at, so the message names the entry that failed.
// errno keeps the value that the failed call set.
static int fail_at(const char *p, char **at) {
    int saved = errno;
    if (!*at) {
        size_t n = strlen(p);
        char *copy = malloc(n + 1);
        // If the copy fails, the message names the path of the caller.
        if (copy) memcpy(copy, p, n + 1);
        *at = copy;
    }
    errno = saved;
    return 0;
}

// Removes p. If p is a directory, removes its contents first, depth first.
// Returns 1, or 0 with errno set and the failed path in *at.
static int remove_tree(const char *p, char **at) {
    nio_stat_t st;
    if (nio_lstat(p, &st) != 0) return fail_at(p, at);
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        // The stat above is an lstat, so a symlink itself is removed. A tree
        // delete must not reach outside the tree.
        return remove(p) == 0 ? 1 : fail_at(p, at);
    }
    // One call removes an empty directory. The same call removes a Windows
    // junction and keeps its target, so it runs before the listing.
    if (nio_rmdir(p) == 0) return 1;

    // errno is kept across each free: it holds the reason for the failure,
    // and free can change it.
    NameList list = {NULL, 0, 0};
    if (!list_dir(p, &list)) {
        int saved = errno;
        names_free(&list);
        errno = saved;
        return fail_at(p, at);
    }
    for (int64_t i = 0; i < list.len; i++) {
        char *child = join_c(p, list.names[i]);
        if (!child) {
            names_free(&list);
            errno = ENOMEM;
            return fail_at(p, at);
        }
        int ok = remove_tree(child, at);
        free(child);
        if (!ok) {
            int saved = errno;
            names_free(&list);
            errno = saved;
            return 0;
        }
    }
    names_free(&list);
    // The directory is now empty, unless another process writes into it.
    return nio_rmdir(p) == 0 ? 1 : fail_at(p, at);
}

// Removes a file or an empty directory. The kind is found first, because the
// Windows CRT remove() refuses directories. Both platforms use one code path.
//
// The operating system refuses a non-empty directory, and that error is
// reported unchanged. A caller must ask for a tree delete: `recursive` in the
// options record starts the walk.
void rt_fs_delete(Str *path, int64_t *opts, void **err) {
    char *c = cpath("delete", path, err);
    if (!c) return;

    if (opt_flag(opts, 0)) {
        char *at = NULL;
        if (!remove_tree(c, &at)) {
            int e = errno;
            const char *reason = strerror(e);
            int64_t code = rt_err_from_errno(e);
            *err = at ? fs_error_c("delete", at, reason, code) : fs_error("delete", path, reason, code);
        }
        free(at);
        free(c);
        return;
    }

    nio_stat_t st;
    if (nio_stat(c, &st) != 0) {
        *err = fs_errno("delete", path);
        free(c);
        return;
    }
    int failed = (st.st_mode & S_IFMT) == S_IFDIR ? nio_rmdir(c) != 0 : remove(c) != 0;
    if (failed) *err = fs_errno("delete", path);
    free(c);
}

// ---- fs.stat(path) ----

// Finds the last element of path. Trailing separators are skipped first, so
// the name of "dir/" is "dir".
static void base_name(Str *path, int64_t *at, int64_t *len) {
    int64_t end = path->len;
    while (end > 0 && is_sep(path->data[end - 1])) end--;
    int64_t start = end;
    while (start > 0 && !is_sep(path->data[start - 1])) start--;
    *at = start;
    *len = end - start;
}

// Returns the index where the extension of the name at path->data[at..at+len)
// starts, or -1. The extension is the last '.' and the text after it.
// ".gitignore" is a hidden file with no extension. A name that ends in '.'
// has no extension.
static int64_t ext_at(Str *path, int64_t at, int64_t len) {
    // A '.' at index `at` is the leading dot of a hidden name.
    for (int64_t i = at + len - 1; i > at; i--) {
        if (path->data[i] == '.') return i + 1 == at + len ? -1 : i;
    }
    return -1;
}

// Writes the low nine mode bits as `ls` shows them. The result is text because
// §1.6 has no octal literals. Windows reports only read-only.
static void mode_text(int mode, char out[9]) {
    static const char letters[3] = {'r', 'w', 'x'};
    for (int i = 0; i < 9; i++) {
        out[i] = (mode & (0400 >> i)) ? letters[i % 3] : '-';
    }
}

void *rt_fs_stat(Str *path, void **err) {
    char *c = cpath("stat", path, err);
    if (!c) return NULL;

    nio_stat_t st;
    if (nio_stat(c, &st) != 0) {
        *err = fs_errno("stat", path);
        free(c);
        return NULL;
    }
    free(c);

    int64_t at, len;
    base_name(path, &at, &len);
    int64_t ext = ext_at(path, at, len);
    char perms[9];
    mode_text((int)(st.st_mode & 0777), perms);

    // Each string is live across the allocations after it, and path is live
    // across all of them, so all four are rooted. No allocation occurs while
    // the record is filled, so the record needs no root.
    TypeDesc *tds[4] = {&rt_td_string, &rt_td_string, &rt_td_string, &rt_td_string};
    int64_t slots[4] = {(int64_t)(intptr_t)path, 0, 0, 0};
    GCFrame f = {rt_gc_top, 4, tds, slots, NULL};
    rt_gc_top = &f;

    Str *name = rt_str_alloc(len);
    memcpy(name->data, path->data + at, (size_t)len);
    slots[1] = (int64_t)(intptr_t)name;

    int64_t ext_len = ext < 0 ? 0 : at + len - ext;
    Str *extension = rt_str_alloc(ext_len);
    if (ext_len > 0) memcpy(extension->data, path->data + ext, (size_t)ext_len);
    slots[2] = (int64_t)(intptr_t)extension;

    Str *permissions = rt_str_alloc(9);
    memcpy(permissions->data, perms, 9);
    slots[3] = (int64_t)(intptr_t)permissions;

    void *stat_rec = rt_rec_new(&td_stat);
    rt_rec_set(stat_rec, 0, slots[1], &td_stat);
    rt_rec_set(stat_rec, 1, slots[2], &td_stat);
    rt_rec_set(stat_rec, 2, (int64_t)st.st_size, &td_stat);
    rt_rec_set(stat_rec, 3, (st.st_mode & S_IFMT) == S_IFDIR, &td_stat);
    rt_rec_set(stat_rec, 4, (st.st_mode & S_IFMT) == S_IFREG, &td_stat);
    rt_rec_set(stat_rec, 5, slots[3], &td_stat);
    // DateTime is Unix milliseconds (runtime.h). st_mtime is whole seconds on
    // every platform, and each platform names the sub-second field
    // differently, so this reports whole seconds.
    rt_rec_set(stat_rec, 6, (int64_t)st.st_mtime * 1000, &td_stat);

    rt_gc_top = f.prev;
    return stat_rec;
}
