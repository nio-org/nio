// A precise, non-moving mark-sweep collector with its own size-classed heap.
// runtime.h describes the object header, the shadow stack and the form of a
// static string literal.
//
// Tracing follows types. A root is an address paired with a TypeDesc, and the
// collector walks the same shape metadata that print and json read, so a heap
// block carries no shape tag.
//
// The heap is chunks: regions aligned to GC_CHUNK_SIZE, carved into cells of
// one size class. Two properties follow from that alignment:
//
//   - The owning chunk is (address & ~(GC_CHUNK_SIZE - 1)), so a block needs no
//     back pointer, list link or size lookup.
//   - The sweep works per chunk. A chunk with no survivor is reclaimed whole in
//     constant time and touches none of its cells. It then pools for a new
//     carve, and the sweep trims that pool (GC_POOL_FLOOR).
//
// A block too large for a chunk (GC_SMALL_MAX) comes from malloc and carries a
// list link before its header.
//
// The collector never moves an object. Codegen holds interior pointers in SSA
// registers that no root set describes.

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// A chunk comes from the operating system and not from malloc, because to
// release one must give its pages back, and free() gives them back to the
// allocator alone. On Windows the aligned malloc of the CRT supplies chunks,
// and that allocator decides when to return pages.
#ifdef _WIN32
#include <malloc.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif
#ifdef __APPLE__
#include <malloc/malloc.h>
#endif
#ifdef __GLIBC__
#include <malloc.h>
#endif

// Asks the C allocator to return its freed pages to the operating system.
// free() alone leaves them in the pools of the allocator, where they are most
// of the idle footprint after a spike of blocks above GC_SMALL_MAX.
static void malloc_relief(void) {
#ifdef __APPLE__
    malloc_zone_pressure_relief(NULL, 0);
#elif defined(__GLIBC__)
    malloc_trim(0);
#endif
}

// ---- collector state ----

GCFrame *rt_gc_top = NULL;

static GCGlobal *globals = NULL;
static int64_t nglobals = 0;

static int64_t live_bytes = 0;   // payload bytes reachable after the last sweep
static int64_t live_objects = 0; // blocks reachable after the last sweep
static int64_t alloc_bytes = 0;  // payload bytes allocated since then
static int64_t collections = 0;  // completed collections
static int64_t chunks_in_use = 0;   // length of all_chunks
static int64_t chunks_pooled = 0;   // length of chunk_pool
static int64_t chunks_released = 0; // ever returned to the OS
static int64_t last_collect_ms = 0; // when the last collection ran, any trigger
static int64_t reserve_chunks = 0;  // chunks the program has recently needed
static int64_t reserve_ms = 0;      // when that number was last met
static int trim_hard = 0;           // this collection may give the reserve back
static int gc_off = 0;

// Collect when the heap has grown to GC_GROWTH x its live size plus
// GC_MIN_HEAP. The factor keeps collection amortized. The constant is added
// and is not a floor. The whole footprint of a batch program is uncollected
// garbage, so this sum is its peak RSS. A server needs the headroom on top of
// its live set.
#define GC_MIN_HEAP (1 << 20)
#define GC_GROWTH 2

// NIO_GC_STRESS=1 collects at every allocation and fills each freed block with
// a known pattern. A missing root then fails at the first collection that can
// show it, and not at a random time.
static int stress = -1; // -1 until the environment is read

static int gc_stress(void) {
    if (stress < 0) {
        const char *v = getenv("NIO_GC_STRESS");
        stress = v && *v && *v != '0';
    }
    return stress;
}

// NIO_GC_STATS=1 writes one line to stderr when the program exits. It reports
// the two phases separately because the mark phase costs per live byte and the sweep
// phase costs per heap byte, so a change that helps one can cost more in the
// other.
static int stats = -1; // -1 until the environment is read

static int gc_stats(void) {
    if (stats < 0) {
        const char *v = getenv("NIO_GC_STATS");
        stats = v && *v && *v != '0';
    }
    return stats;
}

static double stat_mark = 0, stat_sweep = 0;

// Seconds on the monotonic clock. A change to the wall clock must not make a
// duration negative.
static double gc_now(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

static void gc_report(void) {
    fprintf(stderr,
            "[gc] %lld collections  mark %.1f ms  sweep %.1f ms  live %lld bytes in %lld blocks"
            "  chunks %lld in use, %lld pooled, %lld released\n",
            (long long)collections, stat_mark * 1000, stat_sweep * 1000,
            (long long)live_bytes, (long long)live_objects, (long long)chunks_in_use,
            (long long)chunks_pooled, (long long)chunks_released);
}

void rt_gc_disable(void) { gc_off++; }

void rt_gc_enable(void) {
    if (gc_off > 0) gc_off--;
}

void rt_gc_globals(GCGlobal *table, int64_t n) {
    globals = table;
    nglobals = n;
}

// ---- size classes ----
//
// A cell size covers the header and the payload. The sizes step by 8 bytes up
// to GC_FINE_MAX and by 32 up to GC_MID_MAX, so the waste from rounding stays
// below 12.5% and the number of classes stays small. That number decides how
// many partly filled chunks a program holds open.
//
// Above GC_MID_MAX a coarse band grows by a factor up to a whole chunk. On
// macOS a freed block of about 4 KB to 32 KB stays dirty in the magazines of
// libmalloc: neither free() nor malloc_zone_pressure_relief() returns those
// pages. In a chunk, the pool trim of the sweep does give them back.
//
// Each coarse size is about 65472/m for a whole number of cells m, rounded down
// to a multiple of 8, so a chunk divides into cells with almost no waste. The
// last steps are large, so a block of 33 KB uses a cell of 64 KB.

#define GC_SMALL_MAX 65472 // == GC_CHUNK_SIZE - GC_CELL_BASE, asserted below
#define GC_FINE_MAX 128
#define GC_MID_MAX 512
#define GC_FINE_CLASSES 15  // 16, 24, ... 128
#define GC_MID_CLASSES 12   // 160, 192, ... 512
#define GC_COARSE_CLASSES 22
#define GC_NCLASSES 49

static const int32_t coarse_bytes[GC_COARSE_CLASSES] = {
    528, 656, 824, 1032, 1304, 1632, 2040, 2512, 3112, 3848, 4360,
    5032, 5952, 7272, 8184, 9352, 10912, 13088, 16368, 21824, 32736, 65472,
};

static int size_class(size_t total) {
    if (total <= GC_FINE_MAX) return (int)((total + 7) / 8) - 2;
    if (total <= GC_MID_MAX) {
        return GC_FINE_CLASSES + (int)((total - GC_FINE_MAX + 31) / 32) - 1;
    }
    int i = 0;
    while ((size_t)coarse_bytes[i] < total) i++;
    return GC_FINE_CLASSES + GC_MID_CLASSES + i;
}

static size_t class_bytes(int cls) {
    if (cls < GC_FINE_CLASSES) return (size_t)(cls + 2) * 8;
    if (cls < GC_FINE_CLASSES + GC_MID_CLASSES) {
        return GC_FINE_MAX + (size_t)(cls - GC_FINE_CLASSES + 1) * 32;
    }
    return (size_t)coarse_bytes[cls - GC_FINE_CLASSES - GC_MID_CLASSES];
}

// ---- chunks ----

#define GC_CHUNK_SIZE (64 * 1024)
#define GC_CELL_BASE 64 // cells start here, so the header owns a cache line

typedef struct GCChunk {
    struct GCChunk *next; // all-chunks list, or the reclaimed-chunk pool
    int32_t cls;
    int32_t live;   // cells marked in the collection under way
    int64_t used;   // cells ever handed out by the bump pointer
    int64_t ncells; // cells this chunk holds in total
} GCChunk;

_Static_assert(sizeof(GCChunk) <= GC_CELL_BASE, "chunk header overruns its cells");
_Static_assert(GC_CELL_BASE % 8 == 0, "cells must stay 8-byte aligned");
_Static_assert(GC_SMALL_MAX == GC_CHUNK_SIZE - GC_CELL_BASE,
               "the largest class is exactly one chunk's cells");
_Static_assert(sizeof(GCObj) == 8, "codegen emits a one-word header for statics");

static GCChunk *all_chunks = NULL;          // every chunk in use, any class
static GCChunk *chunk_pool = NULL;          // reclaimed, ready to re-carve
static GCChunk *bump[GC_NCLASSES];          // chunk with untouched cells, per class
static void *freelist[GC_NCLASSES];         // reclaimed cells, per class

// The minimum size of the pool, in chunks. A program that needs nothing keeps
// this many mapped, so a program that becomes idle and starts again needs no
// system call. The end of the sweep unmaps all chunks past the limit that it
// computes. It is the only place where the collector returns memory to the
// operating system.
#define GC_POOL_FLOOR 16

// How long, in milliseconds, the pool keeps chunks that the program does not
// use now.
//
// The pool holds the allocation wave between two collections. The live set
// does not measure that wave: a request body is garbage when the collection
// that frees it runs. At every sweep, a server at a steady rate looks like a
// program that needs almost nothing.
//
// The limit is the reserve: the largest number of chunks that the program
// recently used for cells, sampled at each collection before the sweep
// releases any. It goes up with demand at once. It goes down only when demand
// stays below it for this long.
#define GC_RESERVE_MS 10000

// A sweep also calls malloc_relief only when the whole small heap, in use and
// pooled, fits in this number of chunks (4 MB).
//
// A heap that small means that the phase that used the memory has ended. The
// test must not fire between two waves of load, because the next wave would
// fault the released memory back in a few milliseconds.
#define GC_RELIEF_CHUNKS 64

// Maps one chunk at a multiple of GC_CHUNK_SIZE. mmap gives only page
// alignment, so the code maps one chunk more than it needs and unmaps the
// edges.
static GCChunk *chunk_map(void) {
#ifdef _WIN32
    void *mem = _aligned_malloc(GC_CHUNK_SIZE, GC_CHUNK_SIZE);
    if (!mem) rt_panic("out of memory");
    return mem;
#else
    size_t len = 2 * GC_CHUNK_SIZE;
    char *raw = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (raw == MAP_FAILED) rt_panic("out of memory");
    uintptr_t base = ((uintptr_t)raw + GC_CHUNK_SIZE - 1) & ~(uintptr_t)(GC_CHUNK_SIZE - 1);
    size_t head = (size_t)(base - (uintptr_t)raw);
    if (head) munmap(raw, head);
    size_t tail = len - head - GC_CHUNK_SIZE;
    if (tail) munmap((char *)base + GC_CHUNK_SIZE, tail);
    return (GCChunk *)base;
#endif
}

static void chunk_unmap(GCChunk *c) {
#ifdef _WIN32
    _aligned_free(c);
#else
    munmap(c, GC_CHUNK_SIZE);
#endif
}

// A free cell links to the next through the first word of its payload. Every
// class has room for that word: the smallest cell is a header and 8 bytes.
static void *next_free(void *cell) {
    void *p;
    memcpy(&p, cell, sizeof p);
    return p;
}

static void push_free(int cls, void *cell) {
    memcpy(cell, &freelist[cls], sizeof(void *));
    freelist[cls] = cell;
}

static GCChunk *chunk_of(void *p) {
    return (GCChunk *)((uintptr_t)p & ~(uintptr_t)(GC_CHUNK_SIZE - 1));
}

static void *cell_at(GCChunk *c, int64_t i) {
    return (char *)c + GC_CELL_BASE + (size_t)i * class_bytes(c->cls);
}

static GCChunk *new_chunk(int cls) {
    GCChunk *c = chunk_pool;
    if (c) {
        chunk_pool = c->next;
        chunks_pooled--;
    } else {
        c = chunk_map();
    }
    chunks_in_use++;
    c->cls = cls;
    c->live = 0;
    c->used = 0;
    c->ncells = (GC_CHUNK_SIZE - GC_CELL_BASE) / (int64_t)class_bytes(cls);
    c->next = all_chunks;
    all_chunks = c;
    return c;
}

// ---- large blocks ----
//
// A block that does not fit a chunk cell keeps the same header, so tracing and
// marking treat it in the same way. It adds the list link a chunk would supply.

typedef struct GCLarge {
    struct GCLarge *next;
    GCObj obj; // payload follows
} GCLarge;

static GCLarge *large_objects = NULL;

static void *alloc_large(int64_t n) {
    GCLarge *l = malloc(sizeof(GCLarge) + (size_t)n);
    if (!l) rt_panic("out of memory");
    l->obj.info = ((uint64_t)n << GC_SIZE_SHIFT) | GC_LARGE;
    l->next = large_objects;
    large_objects = l;
    void *payload = &l->obj + 1;
    memset(payload, 0, (size_t)n);
    return payload;
}

// ---- marking ----

// Marks the block that owns the payload p. Returns 1 when the caller must trace
// its children, and 0 when p is null, static, or already marked.
static int mark(void *p) {
    if (!p) return 0;
    GCObj *o = (GCObj *)p - 1;
    uint64_t info = o->info;
    if (info & (GC_STATIC | GC_MARK)) return 0;
    o->info = info | GC_MARK;
    // The count tells the sweep that this chunk holds a survivor. A chunk that
    // ends the cycle with none is reclaimed whole.
    if (!(info & GC_LARGE)) chunk_of(p)->live++;
    return 1;
}

// Bit k is set when a value of TypeDesc kind k can hold a pointer. A kind that
// can hold a pointer must appear here, or the collector never traces it and the
// sweep reclaims its referents while they are still reachable. The zeroes make
// tracing a scalar a shift and a mask, which the array and map loops hoist out.
#define TD_POINTER_KINDS                                                        \
    ((1u << TD_STRING) | (1u << TD_ARRAY) | (1u << TD_OPTIONAL) |               \
     (1u << TD_RECORD) | (1u << TD_BLOCK) | (1u << TD_FUNC) |                   \
     (1u << TD_FUTURE) | (1u << TD_MAP) | (1u << TD_JSON) |                     \
     (1u << TD_REGEXP) | (1u << TD_SLOTS) | (1u << TD_SOCKET) |                 \
     (1u << TD_UNION))

// A kind of 32 or more would shift past the end of the mask, so the mask limits
// the kind codes.
_Static_assert(TD_UNION < 32, "TypeDesc kinds no longer fit TD_POINTER_KINDS");

static inline int td_traced(const TypeDesc *td) {
    return (TD_POINTER_KINDS >> (unsigned)td->kind) & 1u;
}

static void trace_ptr(int64_t raw, TypeDesc *td);

// Traces one value of type td. raw is the 8-byte form of the value: a pointer
// for a string, an array, a record and an optional, and a bit pattern that is
// not a pointer for every scalar type. Tracing therefore follows the type and
// never reads the bits to decide.
static inline void trace(int64_t raw, TypeDesc *td) {
    if (td_traced(td)) trace_ptr(raw, td);
}

// The body of trace, for the kinds that can hold a pointer. The recursion ends
// on a cycle, because mark() reports each block once.
static void trace_ptr(int64_t raw, TypeDesc *td) {
    void *p = (void *)(intptr_t)raw;
    switch (td->kind) {
    case TD_STRING:
        mark(p); // leaf: the characters live inside the same block
        break;
    case TD_BLOCK:
        mark(p); // opaque: keep it, but its contents are not yet values
        break;
    case TD_REGEXP:
        // Opaque: a compiled pattern holds no pointer in its block (runtime.h).
        mark(p);
        break;
    case TD_SOCKET:
        // The collector must not walk this one. A NetSock holds a descriptor
        // and two pointers into the malloc memory of lib/net.c (runtime.h).
        mark(p);
        break;
    case TD_OPTIONAL:
        // The load happens only for a traced inner value, which is a pointer
        // and so is 8 bytes wide. The box of a narrow inner value is narrower
        // than a word (§5.7), and a load here would read past it.
        if (mark(p) && td_traced(td->elem)) {
            int64_t inner;
            memcpy(&inner, p, 8);
            trace_ptr(inner, td->elem);
        }
        break;
    case TD_RECORD:
        // A record describes itself: slot [0] holds its own descriptor
        // (runtime.h). The td of the caller is a lower bound, because the value
        // can have a type that extends it (§2.4), and a trace under the
        // descriptor of the base would leave the extra fields untraced.
        if (mark(p)) {
            // A null descriptor means code is still filling the block in, so
            // the field slots hold zero and the static td is safe to use.
            TypeDesc *actual = REC_TD(p);
            if (!actual) actual = td;
            // Only traced fields are loaded. Such a field is a pointer, of eight
            // bytes at an offset that 8 divides, so the word load is exact. A
            // narrow field (§5.7) is one that td_traced skips.
            for (int64_t i = 0; i < actual->nfields; i++) {
                TypeDesc *ft = actual->field_types[i];
                if (!td_traced(ft)) continue;
                int64_t f;
                memcpy(&f, (char *)p + actual->field_offsets[i], 8);
                trace_ptr(f, ft);
            }
        }
        break;
    case TD_UNION: {
        // A slot with a union type holds the record of a member, never an
        // instance of the base, which nothing can construct (§2.4). The stamp
        // in slot [0] is therefore the TD_RECORD descriptor of a member.
        //
        // The static descriptor here carries member tables and not field
        // tables, so this case cannot fall back to it as TD_RECORD does.
        TypeDesc *actual = p ? REC_TD(p) : NULL;
        if (actual) {
            trace_ptr(raw, actual);
            break;
        }
        mark(p);
        break;
    }
    case TD_SLOTS:
        // Slots from offset 0, described from outside: an async frame
        // (runtime.h). The block carries no descriptor of its own.
        if (mark(p)) {
            for (int64_t i = 0; i < td->nfields; i++) {
                int64_t s;
                memcpy(&s, (char *)p + i * 8, 8);
                trace(s, td->field_types[i]);
            }
        }
        break;
    case TD_ARRAY:
        if (mark(p)) {
            Arr *a = p;
            mark(a->data); // the element block is a second allocation
            // A traced element is a pointer, and a pointer is eight bytes wide,
            // so wherever this walk runs the packed block (§5.7) is still an
            // array of units. td_traced skips every array that is not.
            if (td_traced(td->elem)) {
                int64_t *slots = a->data;
                for (int64_t i = 0; i < a->len; i++) trace_ptr(slots[i], td->elem);
            }
        }
        break;
    case TD_MAP:
        // The entry blocks and the index table are separate allocations. The
        // live entries trace as pairs, through the two descriptors in
        // field_types (runtime.h). A traced component is a pointer, so each
        // packed block is an array of units wherever the loop runs, as in
        // TD_ARRAY above.
        if (mark(p)) {
            Map *m = p;
            mark(m->keys);
            mark(m->vals);
            mark(m->index);
            if (td_traced(td->field_types[0])) {
                int64_t *ks = m->keys;
                for (int64_t i = 0; i < m->len; i++) trace_ptr(ks[i], td->field_types[0]);
            }
            if (td_traced(td->field_types[1])) {
                int64_t *vs = m->vals;
                for (int64_t i = 0; i < m->len; i++) trace_ptr(vs[i], td->field_types[1]);
            }
        }
        break;
    case TD_JSON:
        // A Json node describes itself: its kind says which of its four slots
        // hold pointers (runtime.h). It is the only value whose static type
        // does not fix its shape, so this walk is here and no descriptor
        // controls it. The set of shapes is closed, so tracing stays precise. A NULL
        // node is MISSING and marks nothing.
        if (mark(p)) {
            JsonNode *n = p;
            switch (n->kind) {
            case JSON_STRING:
                mark(n->a);
                break;
            case JSON_ARRAY:
                if (mark(n->a)) {
                    JsonVec *items = n->a;
                    for (int64_t i = 0; i < items->len; i++) {
                        trace(items->slots[i], &rt_td_json);
                    }
                }
                break;
            case JSON_OBJECT:
                if (mark(n->a)) {
                    JsonVec *keys = n->a;
                    for (int64_t i = 0; i < keys->len; i++) {
                        mark((void *)(intptr_t)keys->slots[i]); // Str: a leaf
                    }
                }
                if (mark(n->b)) {
                    JsonVec *vals = n->b;
                    for (int64_t i = 0; i < vals->len; i++) {
                        trace(vals->slots[i], &rt_td_json);
                    }
                }
                break;
            default:
                break; // MISSING, NULL, BOOL, NUMBER: no pointers
            }
        }
        break;
    case TD_FUNC:
        // A closure block describes its own captures: slot 1 holds the
        // descriptor and the slots from 2 hold the capture boxes (runtime.h).
        // A null descriptor means code is still filling the block in.
        if (mark(p)) {
            TypeDesc *etd;
            memcpy(&etd, (char *)p + 8, 8);
            if (!etd) break;
            for (int64_t i = 0; i < etd->nfields; i++) {
                int64_t cap;
                memcpy(&cap, (char *)p + 16 + i * 8, 8);
                trace(cap, etd->field_types[i]);
            }
        }
        break;
    case TD_FUTURE:
        // A future describes itself (runtime.h). The frame carries its own
        // descriptor, the callbacks are closures, the waiters are the tasks
        // suspended on it, and the queue link keeps everything behind it on the
        // ready queue alive. Every step below is safe on a null, because a
        // completed future drops its frame.
        if (mark(p)) {
            Future *fu = p;
            if (fu->frame_td) trace((int64_t)(intptr_t)fu->frame, fu->frame_td);
            if (fu->result_td) trace(fu->result, fu->result_td);
            trace((int64_t)(intptr_t)fu->error, &rt_td_error);
            for (FutCB *cb = fu->cbs; cb && mark(cb); cb = cb->next) {
                // A node holds one of two shapes (runtime.h): a closure, or the
                // race that has this future as an argument. The node keeps that
                // race alive after the program drops it, because its result is
                // still pending.
                trace((int64_t)(intptr_t)cb->clos, &rt_td_func);
                trace((int64_t)(intptr_t)cb->race, &rt_td_future);
            }
            trace((int64_t)(intptr_t)fu->waiters, &rt_td_future);
            trace((int64_t)(intptr_t)fu->qnext, &rt_td_future);
        }
        break;
    default:
        // td_traced excludes every scalar kind, so nothing reaches this. A kind
        // added to TD_POINTER_KINDS without a case here then does nothing, and
        // the behavior stays defined.
        break;
    }
}

// ---- sweeping ----

// Rebuilds every free list from scratch: a cell is free unless this cycle
// marked it, which covers cells that died here and cells already free. A chunk
// reclaimed whole contributes nothing, so no free list points into one.
static void sweep(void) {
    int poison = gc_stress();
    int64_t bytes = 0, count = 0;
    // A chunk joins all_chunks when the allocator carves a cell out of it and
    // leaves that list only here. Its length at entry is the high-water mark of
    // this cycle: what the program used, not what it still holds. The reserve
    // follows that number (GC_RESERVE_MS above).
    int64_t demand = chunks_in_use;

    for (int cls = 0; cls < GC_NCLASSES; cls++) freelist[cls] = NULL;

    GCChunk **link = &all_chunks;
    GCChunk *c = all_chunks;
    while (c) {
        GCChunk *next = c->next;
        if (c->live == 0) {
            *link = next;
            if (bump[c->cls] == c) bump[c->cls] = NULL;
            if (poison) memset((char *)c + GC_CELL_BASE, 0xDD, GC_CHUNK_SIZE - GC_CELL_BASE);
            c->next = chunk_pool;
            chunk_pool = c;
            chunks_in_use--;
            chunks_pooled++;
        } else {
            size_t cell = class_bytes(c->cls);
            char *p = (char *)c + GC_CELL_BASE;
            for (int64_t i = 0; i < c->used; i++, p += cell) {
                GCObj *o = (GCObj *)p;
                if (o->info & GC_MARK) {
                    o->info &= ~(uint64_t)GC_MARK;
                    bytes += (int64_t)(o->info >> GC_SIZE_SHIFT);
                    count++;
                } else {
                    // The cell is dead, or was already free from an earlier
                    // sweep. A cleared header makes the two the same, and stops
                    // a fill pattern from reading back as a mark.
                    void *payload = o + 1;
                    if (poison) memset(payload, 0xDD, cell - sizeof(GCObj));
                    o->info = 0;
                    push_free(c->cls, payload);
                }
            }
            c->live = 0;
            link = &c->next;
        }
        c = next;
    }

    GCLarge **llink = &large_objects;
    GCLarge *l = large_objects;
    while (l) {
        GCLarge *lnext = l->next;
        if (l->obj.info & GC_MARK) {
            l->obj.info &= ~(uint64_t)GC_MARK;
            bytes += (int64_t)(l->obj.info >> GC_SIZE_SHIFT);
            count++;
            llink = &l->next;
        } else {
            *llink = lnext;
            if (poison) memset(l, 0xDD, sizeof(GCLarge) + (size_t)(l->obj.info >> GC_SIZE_SHIFT));
            free(l);
        }
        l = lnext;
    }

    live_bytes = bytes;
    live_objects = count;
    alloc_bytes = 0;

    // Move the reserve (GC_RESERVE_MS above) and then trim the pool to it.
    // The chunks that stay mapped match what the program recently needed. The
    // others go back to the operating system.
    //
    // Only an idle collection can lower the reserve at once, because the phase
    // that set the reserve has ended (rt_gc_idle_hint).
    int64_t now = rt_mono_ms();
    if (trim_hard) {
        reserve_chunks = chunks_in_use;
        reserve_ms = now;
    } else if (demand >= reserve_chunks || now - reserve_ms >= GC_RESERVE_MS) {
        reserve_chunks = demand;
        reserve_ms = now;
    }

    int64_t keep = reserve_chunks - chunks_in_use;
    if (keep < GC_POOL_FLOOR) keep = GC_POOL_FLOOR;
    int64_t released_now = 0;
    while (chunks_pooled > keep) {
        GCChunk *dead = chunk_pool;
        chunk_pool = dead->next;
        chunks_pooled--;
        chunks_released++;
        released_now++;
        chunk_unmap(dead);
    }

    // After a sweep that returned chunks, a small heap leaves the large blocks
    // freed above in the pools of the C allocator, with nothing to use them
    // (malloc_relief and GC_RELIEF_CHUNKS above).
    if (released_now > 0 && chunks_in_use + chunks_pooled <= GC_RELIEF_CHUNKS) {
        malloc_relief();
    }
}

// ---- weak tables (runtime.h) ----

// A library whose heap blocks stand for resources that the operating system
// owns (at present, sockets) keeps a weak table of the blocks that it gave
// out. The table is not a root, so the collector reclaims a dropped block as
// any other. The hook tells the library to release the descriptor.
//
// A hook runs between the mark phase and the sweep phase. At that time GC_MARK
// still shows what survived, and nothing is reclaimed. The hook reads the marks
// through rt_gc_marked. A hook must not allocate, because it runs inside a
// collection.
//
// The hooks are a table, so a second library does not replace the hook of the
// first.
static void (*weak_hooks[GC_WEAK_HOOKS])(void);

void rt_gc_weak_hook(void (*fn)(void)) {
    for (int i = 0; i < GC_WEAK_HOOKS; i++) {
        if (weak_hooks[i] == fn) return;
        if (!weak_hooks[i]) {
            weak_hooks[i] = fn;
            return;
        }
    }
    rt_panic("too many weak hooks registered");
}

int rt_gc_marked(void *p) {
    if (!p) return 0;
    GCObj *o = (GCObj *)p - 1;
    return (o->info & (GC_MARK | GC_STATIC)) != 0;
}

void rt_gc_collect(void) {
    int timed = gc_stats();
    if (timed && collections == 0) atexit(gc_report);
    double t0 = timed ? gc_now() : 0;

    for (int64_t i = 0; i < nglobals; i++) trace(*globals[i].addr, globals[i].td);
    for (GCFrame *f = rt_gc_top; f; f = f->prev) {
        for (int64_t i = 0; i < f->nroots; i++) trace(f->slots[i], f->tds[i]);
    }
    // The ready queue and the timer heap hold futures with work left to do,
    // which must survive when the program keeps no reference of its own. A task
    // that waits is reached through the waiter list of its future.
    trace((int64_t)(intptr_t)rt_async_ready, &rt_td_future);
    for (int64_t i = 0; i < rt_async_timer_count; i++) {
        trace((int64_t)(intptr_t)rt_async_timer_heap[i], &rt_td_future);
    }
    // The same holds for work outside the scheduler. `aux` roots the object of
    // that work: a socket under a read must outlive the read, or the weak sweep
    // below closes it while the read is in flight (runtime.h).
    for (int64_t i = 0; i < rt_async_extern_count; i++) {
        trace((int64_t)(intptr_t)rt_async_extern[i].fu, &rt_td_future);
        if (rt_async_extern[i].aux_td) {
            trace((int64_t)(intptr_t)rt_async_extern[i].aux, rt_async_extern[i].aux_td);
        }
    }
    // The hooks run here and nowhere else: after the mark, before the sweep.
    for (int i = 0; i < GC_WEAK_HOOKS && weak_hooks[i]; i++) weak_hooks[i]();

    double t1 = timed ? gc_now() : 0;
    sweep();
    if (timed) {
        stat_mark += t1 - t0;
        stat_sweep += gc_now() - t1;
    }
    collections++;
    last_collect_ms = rt_mono_ms(); // the idle hint's clock (rt_gc_idle_hint)
}

// How long the collections must have been quiet before the idle hint runs one.
// It separates a phase that has ended from a trough between two waves. The test
// is strict, because this is the only collection that gives the reserve back
// (GC_RESERVE_MS above). A loaded server never reaches it: its own collections
// from the allocation budget reset the clock.
#define GC_IDLE_MS 5000

// The clock of the hint. NIO_GC_IDLE_MS replaces it, and 0 turns the idle
// collection off.
static int64_t idle_ms = -2; // -2 until the environment is read

static int64_t gc_idle_ms(void) {
    if (idle_ms == -2) {
        const char *v = getenv("NIO_GC_IDLE_MS");
        idle_ms = v && *v ? strtoll(v, NULL, 10) : GC_IDLE_MS;
        if (idle_ms < 0) idle_ms = GC_IDLE_MS;
    }
    return idle_ms;
}

// The scheduler is about to wait with nothing left to run (runtime.h). A
// collection there pauses no request, and a program that just went quiet still
// holds a heap sized for the load that ended. The allocation trigger cannot
// reach that program, because it never allocates its way to the threshold
// again.
//
// The clock starts at the last collection from any trigger, so the hint does
// nothing while other collections run. It also does nothing when nothing was
// allocated since the last collection, so a parked program stays parked.
void rt_gc_idle_hint(void) {
    int64_t ms = gc_idle_ms();
    if (ms == 0 || gc_off || alloc_bytes == 0) return;
    if (rt_mono_ms() - last_collect_ms < ms) return;
    trim_hard = 1; // the phase that sized the reserve is over (sweep)
    rt_gc_collect();
    trim_hard = 0;
}

// Returns 0 when the hint has nothing to do. Otherwise returns the milliseconds
// until it can act (runtime.h). The scheduler asks before it blocks with no
// deadline, and uses the result as the limit for that wait. Without that
// limit, an idle program never calls the hint again.
int64_t rt_gc_idle_owed(void) {
    int64_t ms = gc_idle_ms();
    if (ms == 0 || gc_off || alloc_bytes == 0) return 0;
    int64_t wait = last_collect_ms + ms - rt_mono_ms();
    return wait < 1 ? 1 : wait;
}

// Diagnostics for the tests of the collector.

int64_t rt_gc_live_bytes(void) { return live_bytes; }
int64_t rt_gc_live_objects(void) { return live_objects; }
int64_t rt_gc_collections(void) { return collections; }
int64_t rt_gc_chunks_in_use(void) { return chunks_in_use; }
int64_t rt_gc_chunks_pooled(void) { return chunks_pooled; }
int64_t rt_gc_chunks_released(void) { return chunks_released; }

// ---- allocation ----

void *rt_alloc(int64_t n) {
    if (n <= 0) n = 1;

    if (!gc_off) {
        int64_t budget = live_bytes * GC_GROWTH + GC_MIN_HEAP;
        if (gc_stress() || live_bytes + alloc_bytes + n > budget) rt_gc_collect();
    }
    alloc_bytes += n;

    size_t total = sizeof(GCObj) + (size_t)n;
    if (total > GC_SMALL_MAX) return alloc_large(n);

    int cls = size_class(total);
    void *payload = freelist[cls];
    if (payload) {
        freelist[cls] = next_free(payload);
    } else {
        GCChunk *c = bump[cls];
        if (!c || c->used == c->ncells) c = bump[cls] = new_chunk(cls);
        payload = (char *)cell_at(c, c->used++) + sizeof(GCObj);
    }

    ((GCObj *)payload - 1)->info = (uint64_t)n << GC_SIZE_SHIFT;
    memset(payload, 0, (size_t)n);
    return payload;
}

