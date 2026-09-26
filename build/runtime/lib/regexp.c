// The `regexp` module (import 'regexp'), linked only when a program imports
// it. Signatures must agree with regexpCallType in src/checker.nio and
// genRegexpCall in src/codegen.nio.
//
// A pattern goes from text to a tree to an instruction array. Only the array is
// kept. The tree is scratch memory that the collector never sees. The program
// is copied into one GC block with no pointers in it (runtime.h), so a RegExp
// costs the collector one mark.
//
// Matching is a Thompson NFA simulation (the Pike VM) and does not backtrack.
// The cost is the subject length times the program size. Patterns come from
// outside the program (§6.14). An engine whose run time the input controls
// would let an attacker cause a denial of service.
//
// Everything works on bytes, like lib/string.c: `.` matches one byte, `[a-z]`
// is a byte range, and case folding under the `i` flag is ASCII.
//
// A pattern that does not compile fails through the `void **err` out-parameter
// convention (lib/fs.c). Matching cannot fail.

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- limits ----
//
// Each limit changes a pattern that would cost unbounded time or memory into a
// compile error that the caller can catch. The instruction limit is for counted
// repeats, which multiply: the short pattern `(a{1000}){1000}` asks for 10^6
// instructions.

#define RX_MAX_PATTERN 32768 // bytes of pattern text
#define RX_MAX_PROG 100000   // instructions, after counted repeats are expanded
#define RX_MAX_DEPTH 200     // nested groups, which bound the parser's recursion
#define RX_MAX_REPEAT 1000   // the largest {n,m} bound

// The match scratch a program of about 64 instructions needs. Below this the
// whole simulation runs out of one stack buffer; see rx_run.
#define RX_SMALL_SCRATCH 4096

// ---- the compiled program ----
//
// One instruction per step of the match. SPLIT is the only branch: x is the
// preferred alternative, which makes a quantifier greedy or lazy. Jump targets
// are absolute instruction indices.

enum {
    RX_CHAR,  // x: the byte that must come next
    RX_ANY,   // any byte; a newline only under NIO_RX_DOTALL
    RX_CLASS, // x: the index of the bitmap the next byte must be in
    RX_SPLIT, // try x first, then y
    RX_JMP,   // x
    RX_BOL,   // the assertions: zero-width, so they are tested when threads
    RX_EOL,   // are added, not when a byte is consumed
    RX_WORDB,
    RX_NWORDB,
    RX_MATCH
};

typedef struct {
    int32_t op;
    int32_t x;
    int32_t y;
} RxInst;

// The four ops that test a position and not the byte at it. They consume
// nothing.
static int rx_is_assert(int op) {
    return op == RX_BOL || op == RX_EOL || op == RX_WORDB || op == RX_NWORDB;
}

#define RX_CLASS_BYTES 32 // 256 bits: one per possible byte

// How a search that has nothing live finds the next position worth trying.
// rx_first_bytes decides which of the three a pattern gets.
enum {
    RX_SKIP_NONE = 0, // any position can begin a match: try every one
    RX_SKIP_BYTE = 1, // exactly one byte can, and memchr finds it
    RX_SKIP_SET = 2   // a few can, named by the bitmap after the classes
};

// A filter that admits more than this many of the 256 bytes rejects too few to
// pay for the pass that applies it. Such a pattern gets RX_SKIP_NONE.
#define RX_SKIP_MAX_SET 250

// The maximum number of leading literal bytes kept as a prefix that a match
// must begin with. A compare then rejects a candidate position without the
// machine. This helps a pattern whose first byte is common.
#define RX_MAX_PREFIX 24

// The instructions follow the header, then the class bitmaps, then the
// start-byte bitmap when there is one. Nothing outside this file reads the
// block, so only the header is in runtime.h.
static RxInst *rx_prog(Regexp *r) {
    return (RxInst *)(void *)((char *)r + sizeof(Regexp));
}

static uint8_t *rx_classes(Regexp *r) {
    return (uint8_t *)(void *)(rx_prog(r) + r->nprog);
}

// Valid only under RX_SKIP_SET, the only case that allocates the 32 bytes.
static uint8_t *rx_first_set(Regexp *r) {
    return rx_classes(r) + r->nclass * RX_CLASS_BYTES;
}

static int rx_class_has(const uint8_t *set, int c) {
    return (set[c >> 3] >> (c & 7)) & 1;
}

static void rx_class_add(uint8_t *set, int c) {
    set[c >> 3] |= (uint8_t)(1u << (c & 7));
}

static int rx_is_word(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || c == '_';
}

static int rx_is_space(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// Adds every byte \d, \w or \s names; kind is the lower-case letter.
static void rx_add_shorthand(uint8_t *set, int kind) {
    for (int c = 0; c < 256; c++) {
        int in = kind == 'd'   ? (c >= '0' && c <= '9')
                 : kind == 'w' ? rx_is_word(c)
                               : rx_is_space(c);
        if (in) rx_class_add(set, c);
    }
}

// Adds the other case of every ASCII letter already in the set. This is all
// that the `i` flag does. It runs at compile time, so matching costs nothing
// extra.
static void rx_fold(uint8_t *set) {
    for (int c = 'a'; c <= 'z'; c++) {
        int upper = c - 'a' + 'A';
        if (rx_class_has(set, c)) rx_class_add(set, upper);
        if (rx_class_has(set, upper)) rx_class_add(set, c);
    }
}

static void rx_negate(uint8_t *set) {
    for (int i = 0; i < RX_CLASS_BYTES; i++) set[i] = (uint8_t)~set[i];
}

// ---- the pattern tree ----
//
// Parsing builds a tree and does not emit instructions directly, because the
// emitter walks the child of a counted repeat again for each copy. Every
// quantifier is one N_REPEAT, so the emitter has one case for all four
// spellings.

enum {
    N_EMPTY,
    N_CHAR,
    N_ANY,
    N_CLASS,
    N_CAT,
    N_ALT,
    N_REPEAT,
    N_BOL,
    N_EOL,
    N_WORDB,
    N_NWORDB
};

typedef struct RxNode RxNode;
struct RxNode {
    int op;
    int ch;       // N_CHAR: the byte; N_CLASS: the bitmap's index
    int min, max; // N_REPEAT: max < 0 is unbounded
    int lazy;     // N_REPEAT: the `?` suffix, which swaps the SPLIT's arms
    RxNode *a, *b;
};

typedef struct {
    const char *p;
    int64_t len, i;
    int64_t flags;
    int ci; // the `i` flag, which compiles into classes and is not kept
    // The node arena and the class bitmaps are sized from the pattern's length
    // and never grow, so a node pointer stays valid for the whole parse.
    RxNode *nodes;
    int64_t nnodes, capnodes;
    uint8_t *classes;
    int64_t nclass, capclass;
    const char *err; // the first failure; every parse function stops on it
    char errbuf[64]; // backs the messages that name a character
} RxParse;

// Both failure helpers keep the first reason. After it is recorded, every parse
// function unwinds, and a later reason would describe a broken state.
static RxNode *rx_fail(RxParse *ps, const char *why) {
    if (!ps->err) ps->err = why;
    return NULL;
}

static RxNode *rx_failc(RxParse *ps, const char *fmt, int c) {
    if (!ps->err) {
        snprintf(ps->errbuf, sizeof ps->errbuf, fmt, (char)c);
        ps->err = ps->errbuf;
    }
    return NULL;
}

static RxNode *rx_node(RxParse *ps, int op) {
    if (ps->nnodes >= ps->capnodes) return rx_fail(ps, "pattern too large");
    RxNode *n = &ps->nodes[ps->nnodes++];
    memset(n, 0, sizeof *n);
    n->op = op;
    return n;
}

// Reserves a zeroed bitmap and returns its index, or -1 when there is no room.
static int rx_new_class(RxParse *ps) {
    if (ps->nclass >= ps->capclass) return -1;
    uint8_t *set = ps->classes + ps->nclass * RX_CLASS_BYTES;
    memset(set, 0, RX_CLASS_BYTES);
    return (int)ps->nclass++;
}

// One literal byte. Under the `i` flag a letter becomes a two-byte class, which
// is how case-insensitivity reaches the matcher.
static RxNode *rx_char_node(RxParse *ps, int c) {
    int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    if (ps->ci && letter) {
        int idx = rx_new_class(ps);
        if (idx < 0) return rx_fail(ps, "pattern too large");
        uint8_t *set = ps->classes + (int64_t)idx * RX_CLASS_BYTES;
        rx_class_add(set, c);
        rx_fold(set);
        RxNode *n = rx_node(ps, N_CLASS);
        if (n) n->ch = idx;
        return n;
    }
    RxNode *n = rx_node(ps, N_CHAR);
    if (n) n->ch = c;
    return n;
}

static int rx_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The byte for the escape `\c`, or -1 when there is none. ps->i is just past c,
// so \xHH can read its two digits. A non-alphanumeric byte escapes to itself.
// An alphanumeric escape not named here is rejected, so an unknown escape fails
// at create time and does not match the letter.
static int rx_escape_byte(RxParse *ps, int c) {
    switch (c) {
    case 'n':
        return '\n';
    case 'r':
        return '\r';
    case 't':
        return '\t';
    case 'f':
        return '\f';
    case 'v':
        return '\v';
    case '0':
        return '\0';
    case 'x': {
        if (ps->i + 1 >= ps->len) return -1;
        int hi = rx_hex((unsigned char)ps->p[ps->i]);
        int lo = rx_hex((unsigned char)ps->p[ps->i + 1]);
        if (hi < 0 || lo < 0) return -1;
        ps->i += 2;
        return hi * 16 + lo;
    }
    default:
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            return -1;
        }
        return c;
    }
}

// An escape outside a character class, which may be an assertion (\b, \B) or
// a class (\d, \w, \s and their negations) as well as a byte.
static RxNode *rx_escape_atom(RxParse *ps) {
    ps->i++; // the backslash
    if (ps->i >= ps->len) return rx_fail(ps, "trailing backslash");
    int c = (unsigned char)ps->p[ps->i++];
    switch (c) {
    case 'b':
        return rx_node(ps, N_WORDB);
    case 'B':
        return rx_node(ps, N_NWORDB);
    case 'd':
    case 'D':
    case 'w':
    case 'W':
    case 's':
    case 'S': {
        int idx = rx_new_class(ps);
        if (idx < 0) return rx_fail(ps, "pattern too large");
        uint8_t *set = ps->classes + (int64_t)idx * RX_CLASS_BYTES;
        int lower = c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
        rx_add_shorthand(set, lower);
        // Fold before negating, so `\W` under `i` is still every byte that is
        // not a word byte.
        if (ps->ci) rx_fold(set);
        if (c != lower) rx_negate(set);
        RxNode *n = rx_node(ps, N_CLASS);
        if (n) n->ch = idx;
        return n;
    }
    default: {
        int b = rx_escape_byte(ps, c);
        if (b < 0) return rx_failc(ps, "invalid escape \\%c", c);
        return rx_char_node(ps, b);
    }
    }
}

// One element inside a class: a byte, -1 on failure, or RX_CLASS_SET for a
// shorthand already merged into set. A shorthand cannot end a range.
#define RX_CLASS_SET (-2)

static int rx_class_item(RxParse *ps, uint8_t *set) {
    int c = (unsigned char)ps->p[ps->i];
    if (c != '\\') {
        ps->i++;
        return c;
    }
    ps->i++;
    if (ps->i >= ps->len) {
        rx_fail(ps, "trailing backslash");
        return -1;
    }
    c = (unsigned char)ps->p[ps->i++];
    switch (c) {
    case 'd':
    case 'D':
    case 'w':
    case 'W':
    case 's':
    case 'S': {
        uint8_t tmp[RX_CLASS_BYTES];
        memset(tmp, 0, sizeof tmp);
        int lower = c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
        rx_add_shorthand(tmp, lower);
        if (c != lower) rx_negate(tmp);
        for (int k = 0; k < RX_CLASS_BYTES; k++) set[k] |= tmp[k];
        return RX_CLASS_SET;
    }
    case 'b':
        // Inside a class \b is a backspace, because an assertion has no meaning
        // there.
        return '\b';
    default: {
        int b = rx_escape_byte(ps, c);
        if (b < 0) {
            rx_failc(ps, "invalid escape \\%c", c);
            return -1;
        }
        return b;
    }
    }
}

// `[abc]`, `[^a-z]`, `[\d.]`. The set is built positive, then folded, then
// negated, in that order, so that `[^a]` under `i` excludes both cases.
static RxNode *rx_class_node(RxParse *ps) {
    ps->i++; // '['
    int neg = 0;
    if (ps->i < ps->len && ps->p[ps->i] == '^') {
        neg = 1;
        ps->i++;
    }
    int idx = rx_new_class(ps);
    if (idx < 0) return rx_fail(ps, "pattern too large");
    uint8_t *set = ps->classes + (int64_t)idx * RX_CLASS_BYTES;

    // A `]` in the first position is a literal, which makes `[]]` the class of
    // one bracket and `[]` an unterminated one.
    int first = 1;
    while (ps->i < ps->len && (ps->p[ps->i] != ']' || first)) {
        first = 0;
        int lo = rx_class_item(ps, set);
        if (lo == -1) return NULL;
        if (lo == RX_CLASS_SET) continue;
        // A `-` is a range only between two single bytes. At the end of the
        // class (`[a-]`) or after a shorthand it is an ordinary byte.
        if (ps->i + 1 < ps->len && ps->p[ps->i] == '-' && ps->p[ps->i + 1] != ']') {
            ps->i++;
            int hi = rx_class_item(ps, set);
            if (hi == -1) return NULL;
            if (hi == RX_CLASS_SET || hi < lo) {
                return rx_fail(ps, "invalid character range");
            }
            for (int c = lo; c <= hi; c++) rx_class_add(set, c);
            continue;
        }
        rx_class_add(set, lo);
    }
    if (ps->i >= ps->len) return rx_fail(ps, "unterminated [");
    ps->i++; // ']'

    if (ps->ci) rx_fold(set);
    if (neg) rx_negate(set);
    RxNode *n = rx_node(ps, N_CLASS);
    if (n) n->ch = idx;
    return n;
}

// Reads `{n}`, `{n,}` or `{n,m}` at the `{` at index `at`, consuming nothing.
// Returns 1 when the text is a repeat, and stores the bounds and the index past
// the `}`. Returns 0 when it is not, and the `{` is then an ordinary byte.
static int rx_peek_bounds(RxParse *ps, int64_t at, int *min, int *max, int64_t *end) {
    int64_t i = at + 1;
    int lo = 0, digits = 0;
    while (i < ps->len && ps->p[i] >= '0' && ps->p[i] <= '9') {
        // The count is capped as it grows, so it cannot overflow before the
        // check below rejects it.
        if (lo <= RX_MAX_REPEAT) lo = lo * 10 + (ps->p[i] - '0');
        i++;
        digits++;
    }
    if (digits == 0) return 0;

    int hi = lo;
    if (i < ps->len && ps->p[i] == ',') {
        i++;
        if (i < ps->len && ps->p[i] == '}') {
            hi = -1; // {n,}: unbounded
        } else {
            hi = 0;
            int more = 0;
            while (i < ps->len && ps->p[i] >= '0' && ps->p[i] <= '9') {
                if (hi <= RX_MAX_REPEAT) hi = hi * 10 + (ps->p[i] - '0');
                i++;
                more++;
            }
            if (more == 0) return 0;
        }
    }
    if (i >= ps->len || ps->p[i] != '}') return 0;
    *min = lo;
    *max = hi;
    *end = i + 1;
    return 1;
}

static RxNode *rx_alt(RxParse *ps, int depth);

static RxNode *rx_atom(RxParse *ps, int depth) {
    int c = (unsigned char)ps->p[ps->i];
    switch (c) {
    case '(': {
        if (depth >= RX_MAX_DEPTH) return rx_fail(ps, "pattern too deeply nested");
        ps->i++;
        if (ps->i + 1 < ps->len && ps->p[ps->i] == '?' && ps->p[ps->i + 1] == ':') {
            // (?: ... ) means what ( ... ) means: this module has no captures,
            // so every group is already non-capturing.
            ps->i += 2;
        } else if (ps->i < ps->len && ps->p[ps->i] == '?') {
            return rx_fail(ps, "unsupported group: only ( ... ) and (?: ... )");
        }
        RxNode *inner = rx_alt(ps, depth + 1);
        if (!inner) return NULL;
        if (ps->i >= ps->len || ps->p[ps->i] != ')') return rx_fail(ps, "unmatched (");
        ps->i++;
        return inner;
    }
    case '[':
        return rx_class_node(ps);
    case '.':
        ps->i++;
        return rx_node(ps, N_ANY);
    case '^':
        ps->i++;
        return rx_node(ps, N_BOL);
    case '$':
        ps->i++;
        return rx_node(ps, N_EOL);
    case '*':
    case '+':
    case '?':
        return rx_fail(ps, "nothing to repeat");
    case '\\':
        return rx_escape_atom(ps);
    default:
        ps->i++;
        return rx_char_node(ps, c);
    }
}

static RxNode *rx_repeat(RxParse *ps, int depth) {
    RxNode *atom = rx_atom(ps, depth);
    if (!atom) return NULL;
    if (ps->i >= ps->len) return atom;

    int min = 0, max = 0;
    int c = (unsigned char)ps->p[ps->i];
    switch (c) {
    case '*':
        min = 0;
        max = -1;
        ps->i++;
        break;
    case '+':
        min = 1;
        max = -1;
        ps->i++;
        break;
    case '?':
        min = 0;
        max = 1;
        ps->i++;
        break;
    case '{': {
        int64_t end;
        if (!rx_peek_bounds(ps, ps->i, &min, &max, &end)) return atom;
        ps->i = end;
        if (min > RX_MAX_REPEAT || max > RX_MAX_REPEAT) {
            return rx_fail(ps, "repeat count too large");
        }
        if (max >= 0 && max < min) return rx_fail(ps, "invalid repeat count");
        break;
    }
    default:
        return atom;
    }

    RxNode *n = rx_node(ps, N_REPEAT);
    if (!n) return NULL;
    n->a = atom;
    n->min = min;
    n->max = max;
    if (ps->i < ps->len && ps->p[ps->i] == '?') {
        n->lazy = 1;
        ps->i++;
    }

    // A second quantifier in a row has nothing to repeat, so `a**` is an error
    // and does not mean `a*`.
    if (ps->i < ps->len) {
        c = (unsigned char)ps->p[ps->i];
        if (c == '*' || c == '+' || c == '?') return rx_fail(ps, "nothing to repeat");
        if (c == '{') {
            int m2, x2;
            int64_t end2;
            if (rx_peek_bounds(ps, ps->i, &m2, &x2, &end2)) {
                return rx_fail(ps, "nothing to repeat");
            }
        }
    }
    return n;
}

// Appends one piece to a right-leaning chain of `op` nodes: `abc` becomes
// CAT(a, CAT(b, c)). The emitter then walks the spine in a loop, with no C
// stack frame per byte of pattern. `tail` is the chain node whose `b` is the
// open end, and NULL while the chain is a single piece. Returns 0 when there
// was no room for a node.
static int rx_chain(RxParse *ps, int op, RxNode **head, RxNode **tail, RxNode *piece) {
    if (!*head) {
        *head = piece;
        return 1;
    }
    RxNode *link = rx_node(ps, op);
    if (!link) return 0;
    if (!*tail) {
        link->a = *head;
        *head = link;
    } else {
        link->a = (*tail)->b;
        (*tail)->b = link;
    }
    link->b = piece;
    *tail = link;
    return 1;
}

static RxNode *rx_concat(RxParse *ps, int depth) {
    RxNode *head = NULL, *tail = NULL;
    while (ps->i < ps->len && ps->p[ps->i] != '|' && ps->p[ps->i] != ')') {
        RxNode *piece = rx_repeat(ps, depth);
        if (!piece) return NULL;
        if (!rx_chain(ps, N_CAT, &head, &tail, piece)) return NULL;
    }
    // An empty branch is a node, so `a|` and `()` parse into a tree that the
    // emitter can walk.
    return head ? head : rx_node(ps, N_EMPTY);
}

static RxNode *rx_alt(RxParse *ps, int depth) {
    RxNode *head = rx_concat(ps, depth);
    if (!head) return NULL;
    RxNode *tail = NULL;
    while (ps->i < ps->len && ps->p[ps->i] == '|') {
        ps->i++;
        RxNode *branch = rx_concat(ps, depth);
        if (!branch) return NULL;
        if (!rx_chain(ps, N_ALT, &head, &tail, branch)) return NULL;
    }
    return head;
}

// ---- emitting ----

typedef struct {
    RxInst *prog;
    int64_t n, cap;
    const char *err;
} RxEmit;

static int64_t rx_emit(RxEmit *e, int op, int32_t x, int32_t y) {
    if (e->err) return 0;
    if (e->n >= RX_MAX_PROG) {
        e->err = "pattern too large";
        return 0;
    }
    if (e->n == e->cap) {
        int64_t cap = e->cap ? e->cap * 2 : 32;
        RxInst *grown = realloc(e->prog, (size_t)cap * sizeof(RxInst));
        if (!grown) {
            e->err = "out of memory";
            return 0;
        }
        e->prog = grown;
        e->cap = cap;
    }
    RxInst *in = &e->prog[e->n];
    in->op = op;
    in->x = x;
    in->y = y;
    return e->n++;
}

// Both patch helpers do nothing after emitting fails. rx_emit then returns a
// fake index, and the buffer may not exist.
static void rx_set_split(RxEmit *e, int64_t at, int32_t prefer, int32_t other, int lazy) {
    if (e->err || !e->prog) return;
    e->prog[at].x = lazy ? other : prefer;
    e->prog[at].y = lazy ? prefer : other;
}

static void rx_patch_jmp(RxEmit *e, int64_t at, int32_t target) {
    if (e->err || !e->prog) return;
    e->prog[at].x = target;
}

static void rx_emit_node(RxEmit *e, RxNode *n);

// Emits `count` optional copies of child, nested: (child (child ...)?)?. With
// nesting, `a{1,3}` skips the rest as soon as it skips one, and no list of
// patch sites is needed: the exit of each SPLIT is the end of its subtree.
static void rx_emit_bounded(RxEmit *e, RxNode *child, int count, int lazy) {
    if (e->err || count <= 0) return;
    int64_t sp = rx_emit(e, RX_SPLIT, 0, 0);
    rx_emit_node(e, child);
    rx_emit_bounded(e, child, count - 1, lazy);
    rx_set_split(e, sp, (int32_t)(sp + 1), (int32_t)e->n, lazy);
}

static void rx_emit_node(RxEmit *e, RxNode *n) {
    if (e->err || !n) return;
    switch (n->op) {
    case N_EMPTY:
        break;
    case N_CHAR:
        rx_emit(e, RX_CHAR, n->ch, 0);
        break;
    case N_ANY:
        rx_emit(e, RX_ANY, 0, 0);
        break;
    case N_CLASS:
        rx_emit(e, RX_CLASS, n->ch, 0);
        break;
    case N_BOL:
        rx_emit(e, RX_BOL, 0, 0);
        break;
    case N_EOL:
        rx_emit(e, RX_EOL, 0, 0);
        break;
    case N_WORDB:
        rx_emit(e, RX_WORDB, 0, 0);
        break;
    case N_NWORDB:
        rx_emit(e, RX_NWORDB, 0, 0);
        break;
    case N_CAT:
        // The chain is right-leaning (rx_chain), so a loop walks the spine in
        // one stack frame and not one per byte of pattern.
        while (n->op == N_CAT && !e->err) {
            rx_emit_node(e, n->a);
            n = n->b;
        }
        rx_emit_node(e, n);
        break;
    case N_ALT: {
        //   SPLIT branch, rest
        //   <branch>  JMP ->
        //   rest: SPLIT branch, rest
        //         ...
        //         <last branch>
        // end:
        //
        // The JMP of a branch cannot target the end, which is not known until
        // the last branch is emitted. Each JMP targets the JMP of the next
        // branch, and only the last is patched to the end.
        int64_t prev = -1;
        while (n->op == N_ALT && !e->err) {
            int64_t sp = rx_emit(e, RX_SPLIT, 0, 0);
            rx_emit_node(e, n->a);
            int64_t jp = rx_emit(e, RX_JMP, 0, 0);
            rx_set_split(e, sp, (int32_t)(sp + 1), (int32_t)(jp + 1), 0);
            if (prev >= 0) rx_patch_jmp(e, prev, (int32_t)jp);
            prev = jp;
            n = n->b;
        }
        rx_emit_node(e, n);
        if (prev >= 0) rx_patch_jmp(e, prev, (int32_t)e->n);
        break;
    }
    case N_REPEAT: {
        // The required copies first, then the optional copies that the bound
        // allows:
        // `a{2,}` is `aa*` and `a{2,4}` is `aa(a(a)?)?`.
        for (int k = 0; k < n->min && !e->err; k++) rx_emit_node(e, n->a);
        if (n->max < 0) {
            // loop: SPLIT body, end
            //       <body>  JMP loop
            // end:
            int64_t sp = rx_emit(e, RX_SPLIT, 0, 0);
            rx_emit_node(e, n->a);
            int64_t jp = rx_emit(e, RX_JMP, 0, 0);
            rx_patch_jmp(e, jp, (int32_t)sp);
            rx_set_split(e, sp, (int32_t)(sp + 1), (int32_t)e->n, n->lazy);
        } else {
            rx_emit_bounded(e, n->a, n->max - n->min, n->lazy);
        }
        break;
    }
    }
}

// ---- where a match can begin ----
//
// The bytes that a match can begin with are a filter on start positions. A
// search with no live thread scans ahead to the next such byte, and does not
// start an attempt at each position between. The walk collects the set from
// the program start through every instruction that consumes no byte.
//
// Two rules keep the filter sound. The walk goes through assertions and does
// not test them, because a false assertion only rejects more positions than
// the filter does: `\bcat` still gives `c`. If the walk reaches MATCH, there is
// no filter, because a pattern that matches without consuming a byte matches
// at every position: `a*`, `^$` and `\b` get RX_SKIP_NONE.
//
// Returns 0 when there is no filter. Otherwise fills set and returns 1. `seen`
// and `stack` are scratch. Jump targets are in range, because only the emitter
// writes them.
static int rx_first_bytes(RxInst *prog, const uint8_t *classes, int64_t flags,
                          uint8_t *set, uint8_t *seen, int64_t *stack) {
    int64_t top = 0;
    stack[top++] = 0;
    while (top > 0) {
        int64_t q = stack[--top];
        if (seen[q]) continue;
        seen[q] = 1;
        RxInst *in = &prog[q];
        switch (in->op) {
        case RX_JMP:
            stack[top++] = in->x;
            break;
        case RX_SPLIT:
            stack[top++] = in->x;
            stack[top++] = in->y;
            break;
        case RX_BOL:
        case RX_EOL:
        case RX_WORDB:
        case RX_NWORDB:
            stack[top++] = q + 1;
            break;
        case RX_CHAR:
            rx_class_add(set, in->x);
            break;
        case RX_CLASS: {
            const uint8_t *cs = classes + (int64_t)in->x * RX_CLASS_BYTES;
            for (int i = 0; i < RX_CLASS_BYTES; i++) set[i] |= cs[i];
            break;
        }
        case RX_ANY:
            for (int c = 0; c < 256; c++) {
                if (c != '\n' || (flags & NIO_RX_DOTALL)) rx_class_add(set, c);
            }
            break;
        default: // RX_MATCH, reached without consuming a byte
            return 0;
        }
    }
    return 1;
}

// ---- matching ----
//
// One list of live threads per input position. A thread is an instruction index
// plus the position its attempt started at. The list holds them in priority
// order, so the result is the leftmost match. `mark` removes duplicates (one
// entry per instruction per position). This bounds the simulation and stops a
// body that can match nothing (`(a?)*`) from looping forever.

typedef struct {
    int32_t *pcs;
    int64_t *starts;
    int64_t n;
    int64_t *mark; // the generation each instruction was last added in
    int64_t gen;
} RxList;

// Follows every branch and assertion from pc and appends the instructions that
// consume a byte. The walk uses an explicit stack and no recursion, so a
// pattern cannot ask for a C stack RX_MAX_PROG deep.
static void rx_addthread(RxList *l, RxInst *prog, int64_t pc, int64_t start,
                         const char *text, int64_t len, int64_t sp, int64_t flags,
                         int64_t *stack) {
    int64_t top = 0;
    stack[top++] = pc;
    while (top > 0) {
        int64_t q = stack[--top];
        if (l->mark[q] == l->gen) continue;
        l->mark[q] = l->gen;
        RxInst *in = &prog[q];
        switch (in->op) {
        case RX_JMP:
            stack[top++] = in->x;
            break;
        case RX_SPLIT:
            // y goes under x, so the subtree of x is walked first and the list
            // is in the order that the pattern prefers.
            stack[top++] = in->y;
            stack[top++] = in->x;
            break;
        case RX_BOL:
            if (sp == 0 || ((flags & NIO_RX_MULTILINE) && text[sp - 1] == '\n')) {
                stack[top++] = q + 1;
            }
            break;
        case RX_EOL:
            if (sp == len || ((flags & NIO_RX_MULTILINE) && text[sp] == '\n')) {
                stack[top++] = q + 1;
            }
            break;
        case RX_WORDB:
        case RX_NWORDB: {
            int before = sp > 0 && rx_is_word((unsigned char)text[sp - 1]);
            int after = sp < len && rx_is_word((unsigned char)text[sp]);
            if ((before != after) == (in->op == RX_WORDB)) stack[top++] = q + 1;
            break;
        }
        default: // CHAR, ANY, CLASS, MATCH: the thread waits here for the byte
            l->pcs[l->n] = (int32_t)q;
            l->starts[l->n] = start;
            l->n++;
            break;
        }
    }
}

// The next position at or after sp where a match can begin, or -1 when the
// subject has none left. memchr finds one byte. With a longer literal prefix,
// the rest of the prefix is compared here against the program. A common first
// byte then costs a compare and not an attempt for each occurrence.
static int64_t rx_next_start(const char *text, int64_t sp, int64_t len, int64_t skip,
                             int byte, const uint8_t *set, const RxInst *prog,
                             int64_t npref) {
    if (skip != RX_SKIP_BYTE) {
        while (sp < len && !rx_class_has(set, (unsigned char)text[sp])) sp++;
        return sp < len ? sp : -1;
    }
    for (;;) {
        const char *hit = memchr(text + sp, byte, (size_t)(len - sp));
        if (!hit) return -1;
        sp = (int64_t)(hit - text);
        if (npref < 2) return sp;
        // The prefix does not fit here or at any later position.
        if (sp + npref > len) return -1;
        int64_t k = 1;
        while (k < npref && (unsigned char)text[sp + k] == (unsigned char)prog[k].x) k++;
        if (k == npref) return sp;
        sp++;
    }
}

// The offset where the leftmost match starts, or -1. With first_only set, the
// search stops at the first match found, which need not be the leftmost.
// regexp.match needs only that.
static int64_t rx_run(Regexp *re, Str *s, int first_only) {
    const char *text = s->data;
    int64_t len = s->len;
    RxInst *prog = rx_prog(re);
    const uint8_t *classes = rx_classes(re);
    int64_t np = re->nprog;

    // One scratch block for both lists and the walk stack. The collector never
    // sees it, which is safe because nothing in the match allocates: no
    // collection can happen while it is held, and `text` points into a Str the
    // caller still owns. The program size bounds every array, so a small
    // program uses the stack and allocates nothing.
    int64_t words = 6 * np + 4;
    size_t bytes = (size_t)words * sizeof(int64_t) + (size_t)(2 * np) * sizeof(int32_t);
    int64_t small[RX_SMALL_SCRATCH / sizeof(int64_t)];
    int64_t *mem = small;
    if (bytes > sizeof small) {
        mem = calloc(1, bytes);
        if (!mem) rt_panic("out of memory");
    } else {
        memset(small, 0, bytes);
    }

    int64_t *startsA = mem, *startsB = mem + np;
    int64_t *markA = mem + 2 * np, *markB = mem + 3 * np;
    int64_t *stack = mem + 4 * np;
    int32_t *pcs = (int32_t *)(void *)(mem + words);

    RxList a = {pcs, startsA, 0, markA, 0};
    RxList b = {pcs + np, startsB, 0, markB, 0};
    RxList *cl = &a, *nl = &b;

    int64_t skip = re->skip;
    const uint8_t *first = skip == RX_SKIP_SET ? rx_first_set(re) : NULL;

    int64_t matched = -1;
    cl->gen++;
    for (int64_t sp = 0; sp <= len; sp++) {
        // Start a new attempt at every position until one succeeds. After that,
        // a later start can only be worse than the current match.
        if (matched < 0) {
            // With no live thread, no current attempt can become a match, so
            // the next one must begin on a byte that the pattern admits
            // (rx_first_bytes). The positions before that byte are skipped.
            if (skip != RX_SKIP_NONE && cl->n == 0) {
                int64_t at = rx_next_start(text, sp, len, skip, (int)re->skipbyte, first,
                                           prog + re->prefpc, re->npref);
                if (at < 0) break; // no byte left that could begin one
                if (at > sp) {
                    // A new position gets a new generation, so the marks of the
                    // failed attempts do not suppress the attempt here.
                    sp = at;
                    cl->gen++;
                }
            }
            rx_addthread(cl, prog, 0, sp, text, len, sp, re->flags, stack);
        }
        if (cl->n == 0) {
            // No thread is live here. A found match or the end of the subject
            // stops the search. Nothing else can stop it, because a pattern
            // that begins with an assertion (`\bcat`) leaves the list empty at
            // every position that the assertion rejects.
            if (matched >= 0 || sp >= len) break;
            // A new generation, so the marks of this position do not suppress
            // the next attempt.
            cl->gen++;
            continue;
        }

        nl->n = 0;
        nl->gen++;
        int c = sp < len ? (unsigned char)text[sp] : -1;
        for (int64_t i = 0; i < cl->n; i++) {
            int64_t pc = cl->pcs[i], start = cl->starts[i];
            RxInst *in = &prog[pc];
            int ok = 0;
            switch (in->op) {
            case RX_CHAR:
                ok = c >= 0 && c == in->x;
                break;
            case RX_ANY:
                ok = c >= 0 && ((re->flags & NIO_RX_DOTALL) || c != '\n');
                break;
            case RX_CLASS:
                ok = c >= 0 && rx_class_has(classes + (int64_t)in->x * RX_CLASS_BYTES, c);
                break;
            default: // RX_MATCH
                matched = start;
                if (first_only) {
                    if (mem != small) free(mem);
                    return matched;
                }
                // Every thread after this one started later, so none can give a
                // better match. The threads before it still can, and their
                // successors are already on the next list.
                goto cut;
            }
            if (ok) {
                rx_addthread(nl, prog, pc + 1, start, text, len, sp + 1, re->flags, stack);
            }
        }
    cut: {
        RxList *t = cl;
        cl = nl;
        nl = t;
    }
    }

    if (mem != small) free(mem);
    return matched;
}

// ---- the module's functions ----

static Regexp *rx_check(void *re) {
    // The zero value of a RegExp, from a declaration without an initializer.
    // To use it is a bug in the program.
    if (!re) rt_panic("use of a RegExp that was never created");
    return re;
}

// The Error for a pattern that does not compile, in the form of lib/string.c:
// the function, the text, and the reason. It shows at most 200 bytes of the
// pattern, by the length of the Nio string, which has no terminator.
static void *rx_error(Str *pattern, const char *reason) {
    char msg[512];
    int64_t shown = pattern->len > 200 ? 200 : pattern->len;
    snprintf(msg, sizeof msg, "regexp.create \"%.*s\": %s", (int)shown, pattern->data,
             reason);
    // Every failure here is text that is not a valid pattern, which is what
    // NIO_ERR_INVALID means (runtime.h).
    return rt_error_new(msg, NIO_ERR_INVALID);
}

void *rt_regexp_create(Str *pattern, Str *flags, void **err) {
    // Both are live across the allocation of the block and of the Error.
    // `flags` is NULL when the call gave none, and a null root traces as
    // nothing.
    TypeDesc *tds[2] = {&rt_td_string, &rt_td_string};
    int64_t slots[2] = {(int64_t)(intptr_t)pattern, (int64_t)(intptr_t)flags};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    const char *why = NULL;
    char flagbuf[48];
    int64_t fl = 0;
    int ci = 0;
    for (int64_t i = 0; flags && i < flags->len && !why; i++) {
        char c = flags->data[i];
        switch (c) {
        case 'i':
            ci = 1;
            break;
        case 'm':
            fl |= NIO_RX_MULTILINE;
            break;
        case 's':
            fl |= NIO_RX_DOTALL;
            break;
        default:
            if (c >= 0x20 && c < 0x7f) {
                snprintf(flagbuf, sizeof flagbuf, "unknown flag '%c' (i, m, s)", c);
            } else {
                snprintf(flagbuf, sizeof flagbuf, "unknown flag \\x%02x (i, m, s)",
                         (unsigned)(unsigned char)c);
            }
            why = flagbuf;
        }
    }
    if (!why && pattern->len > RX_MAX_PATTERN) why = "pattern too long";

    RxParse ps;
    RxEmit em;
    memset(&ps, 0, sizeof ps);
    memset(&em, 0, sizeof em);

    if (!why) {
        ps.p = pattern->data;
        ps.len = pattern->len;
        ps.flags = fl;
        ps.ci = ci;
        // Both arenas are sized from the pattern and never grow. Five nodes per
        // byte covers the worst case, and each class uses at least one byte of
        // the pattern.
        ps.capnodes = 5 * pattern->len + 8;
        ps.capclass = pattern->len + 1;
        ps.nodes = calloc((size_t)ps.capnodes, sizeof(RxNode));
        ps.classes = calloc((size_t)ps.capclass, RX_CLASS_BYTES);
        if (!ps.nodes || !ps.classes) rt_panic("out of memory");

        RxNode *root = rx_alt(&ps, 0);
        if (!root) {
            why = ps.err ? ps.err : "invalid pattern";
        } else if (ps.i < ps.len) {
            // rx_concat stops at a `)` that has no group. Nothing else can leave
            // text unread.
            why = "unmatched )";
        } else {
            rx_emit_node(&em, root);
            rx_emit(&em, RX_MATCH, 0, 0);
            why = em.err;
        }
    }

    Regexp *re = NULL;
    if (!why) {
        // The bytes that a match can begin with, computed once for all searches
        // with this pattern. A pattern that admits nearly every byte gets no
        // filter, because the filter would not help.
        int64_t skip = RX_SKIP_NONE;
        int64_t skipbyte = 0;
        uint8_t firstset[RX_CLASS_BYTES];
        memset(firstset, 0, sizeof firstset);
        uint8_t *seen = calloc((size_t)em.n, 1);
        int64_t *stack = calloc((size_t)(2 * em.n + 1), sizeof(int64_t));
        if (!seen || !stack) rt_panic("out of memory");
        if (rx_first_bytes(em.prog, ps.classes, fl, firstset, seen, stack)) {
            int64_t n = 0;
            for (int c = 0; c < 256; c++) {
                if (rx_class_has(firstset, c)) {
                    n++;
                    skipbyte = c;
                }
            }
            // An empty set is correct: `[^\x00-\xff]x` can begin nowhere, and
            // an empty filter finds that in a single scan.
            skip = n == 1 ? RX_SKIP_BYTE : n <= RX_SKIP_MAX_SET ? RX_SKIP_SET : RX_SKIP_NONE;
        }
        free(seen);
        free(stack);

        // The number of RX_CHAR instructions at the program start. Every attempt
        // runs a leading run of RX_CHAR in order, so those bytes are required.
        // An assertion before the run consumes nothing, so it is skipped. A
        // pattern under the `i` flag has no run, because a letter there is a
        // two-byte class.
        int64_t prefpc = 0, npref = 0;
        if (skip == RX_SKIP_BYTE) {
            while (rx_is_assert(em.prog[prefpc].op)) prefpc++;
            while (npref < RX_MAX_PREFIX && em.prog[prefpc + npref].op == RX_CHAR) npref++;
        }

        size_t bytes = sizeof(Regexp) + (size_t)em.n * sizeof(RxInst) +
                       (size_t)ps.nclass * RX_CLASS_BYTES +
                       (skip == RX_SKIP_SET ? RX_CLASS_BYTES : 0);
        // The only allocation in the function. The block is filled from scratch
        // memory immediately after it, so no partly built RegExp is reachable.
        re = rt_alloc((int64_t)bytes);
        re->nprog = em.n;
        re->nclass = ps.nclass;
        re->flags = fl;
        re->skip = skip;
        re->skipbyte = skipbyte;
        re->prefpc = prefpc;
        re->npref = npref;
        if (skip == RX_SKIP_SET) memcpy(rx_first_set(re), firstset, RX_CLASS_BYTES);
        memcpy(rx_prog(re), em.prog, (size_t)em.n * sizeof(RxInst));
        memcpy(rx_classes(re), ps.classes, (size_t)ps.nclass * RX_CLASS_BYTES);
    }

    free(ps.nodes);
    free(ps.classes);
    free(em.prog);
    if (why) *err = rx_error(pattern, why);

    rt_gc_top = f.prev;
    return re;
}

int64_t rt_regexp_find(Str *s, void *re) { return rx_run(rx_check(re), s, 0); }

int64_t rt_regexp_match(Str *s, void *re) { return rx_run(rx_check(re), s, 1) >= 0; }
