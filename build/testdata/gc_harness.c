// A standalone test of the collector. It builds object graphs by hand, calls
// rt_gc_collect directly, and checks what survives. It compiles against the
// real runtime sources, so it tests tracing without the compiler.
//
// tests/gc_test.nio runs it. It is in testdata/, so the bake does not put it
// into the runtime that the compiler links.

#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void check(const char *what, int64_t got, int64_t want) {
    if (got != want) {
        printf("FAIL %-42s got %lld want %lld\n", what, (long long)got, (long long)want);
        failures++;
    } else {
        printf("ok   %-42s %lld\n", what, (long long)got);
    }
}

// ---- type descriptors, as codegen emits them ----

extern TypeDesc rt_td_int;
extern TypeDesc rt_td_string;

static TypeDesc td_str_arr = {TD_ARRAY, &rt_td_string, 0, 0, 0};

// type node { label: string; next: node? }
static TypeDesc td_node;
static TypeDesc td_node_opt = {TD_OPTIONAL, &td_node, 0, 0, 0};
static const char *node_field_names[] = {"label", "next"};
static TypeDesc *node_field_types[] = {&rt_td_string, &td_node_opt};
// Both fields are pointers, so the layout rule of §5.7 gives 8 + i*8 and a
// 24-byte block.
static const int64_t node_field_offsets[] = {8, 16};

static void init_tds(void) {
    td_node.kind = TD_RECORD;
    td_node.elem = 0;
    td_node.nfields = 2;
    td_node.field_names = node_field_names;
    td_node.field_types = node_field_types;
    td_node.field_offsets = node_field_offsets;
    td_node.size = 24;
}

// ---- helpers that build values the way codegen does ----

// Both helpers hold a heap pointer in a C local across an allocation, so both
// push a GC frame, as the runtime does.

static void *new_node(const char *label) {
    rt_gc_disable();
    void *rec = rt_rec_new(&td_node);
    Str *s = rt_str_from_c(label);
    rt_rec_set(rec, 0, (int64_t)(intptr_t)s, &td_node);
    rt_gc_enable();
    return rec;
}

static void set_next(void *node, void *target) {
    rt_gc_disable();
    int64_t raw = (int64_t)(intptr_t)target;
    void *box = rt_box(raw, &td_node); // the box holds a Node pointer
    rt_rec_set(node, 1, (int64_t)(intptr_t)box, &td_node);
    rt_gc_enable();
}

static Str *node_label(void *node) {
    return (Str *)(intptr_t)rt_rec_get(node, 0, &td_node);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // Step 13 needs an idle scheduler. Set this before the first collection,
    // because gc.c reads the variable once.
#if defined(_WIN32)
    _putenv_s("NIO_GC_IDLE_MS", "2");
#else
    setenv("NIO_GC_IDLE_MS", "2", 1);
#endif
    init_tds();

    // A frame with two roots, as generated code sets one up.
    int64_t slots[2] = {0, 0};
    TypeDesc *tds[2] = {&td_node, &td_str_arr};
    GCFrame frame = {NULL, 2, tds, slots};
    rt_gc_top = &frame;

    // ---- 1. unreachable garbage is reclaimed ----
    for (int i = 0; i < 1000; i++) new_node("garbage");
    rt_gc_collect();
    check("garbage collected: live objects", rt_gc_live_objects(), 0);

    // ---- 2. a rooted object and everything it reaches survives ----
    void *a = new_node("alpha");
    slots[0] = (int64_t)(intptr_t)a;
    for (int i = 0; i < 1000; i++) new_node("garbage");
    rt_gc_collect();
    // record + its label string = 2 blocks
    check("rooted node survives: live objects", rt_gc_live_objects(), 2);
    check("rooted node payload intact", node_label(a)->len, 5);

    // ---- 3. cycles are reclaimed when the whole cycle is unreachable ----
    void *b = new_node("beta");
    void *c = new_node("gamma");
    set_next(b, c);
    set_next(c, b); // b <-> c, reachable from nothing
    rt_gc_collect();
    check("unreachable cycle collected", rt_gc_live_objects(), 2);

    // ---- 4. a rooted cycle survives, and tracing terminates ----
    void *d = new_node("delta");
    void *e = new_node("epsi");
    set_next(d, e);
    set_next(e, d);
    slots[0] = (int64_t)(intptr_t)d;
    rt_gc_collect();
    // 2 records + 2 labels + 2 boxes = 6, and "alpha" is now unrooted
    check("rooted cycle survives", rt_gc_live_objects(), 6);
    check("cycle payload intact", node_label(d)->len, 5);

    // ---- 5. arrays trace their slot block and their elements ----
    Arr *arr = rt_arr_new(3, 8);
    slots[1] = (int64_t)(intptr_t)arr;
    for (int i = 0; i < 3; i++) {
        Str *s = rt_str_from_c("element");
        rt_arr_units(arr)[i] = (int64_t)(intptr_t)s;
    }
    slots[0] = 0; // drop the cycle
    rt_gc_collect();
    // Arr header + slot block + 3 strings = 5
    check("array traces elements", rt_gc_live_objects(), 5);
    check("array element intact", ((Str *)(intptr_t)rt_arr_units(arr)[2])->len, 7);

    // ---- 6. absent optionals and null fields are not followed ----
    void *f = new_node("zeta"); // next left NULL
    slots[0] = (int64_t)(intptr_t)f;
    slots[1] = 0;
    rt_gc_collect();
    check("null optional field is safe", rt_gc_live_objects(), 2);

    // ---- 7. popping the frame makes everything unreachable ----
    rt_gc_top = NULL;
    rt_gc_collect();
    check("empty root set collects all", rt_gc_live_objects(), 0);

    // ---- 8. allocation alone triggers collection past the threshold ----
    // rt_alloc with no GC frame, as codegen emits a record or array literal.
    // Only such a call site can start a collection.
    rt_gc_top = &frame;
    slots[0] = slots[1] = 0;
    int64_t before = rt_gc_collections();
    for (int i = 0; i < 100000; i++) rt_alloc(64); // ~7 MB, past GC_MIN_HEAP
    check("threshold triggers collection", rt_gc_collections() > before, 1);
    check("garbage does not accumulate", rt_gc_live_bytes() < (1 << 20), 1);

    // ---- 9. rt_gc_disable defers it ----
    rt_gc_collect();
    before = rt_gc_collections();
    rt_gc_disable();
    for (int i = 0; i < 50000; i++) rt_alloc(64);
    check("disable defers collection", rt_gc_collections(), before);
    rt_gc_enable();
    rt_gc_collect();
    check("deferred garbage then collected", rt_gc_live_objects(), 0);

    // ---- 10. a long chain traces without losing anything ----
    void *head = new_node("chain");
    slots[0] = (int64_t)(intptr_t)head;
    void *tail = head;
    for (int i = 0; i < 5000; i++) {
        void *n = new_node("link");
        set_next(tail, n);
        tail = n;
    }
    rt_gc_collect();
    // 5001 records + 5001 labels + 5000 boxes
    check("long chain fully traced", rt_gc_live_objects(), 5001 + 5001 + 5000);

    // ---- 11. every size class round-trips, survivors intact ----
    // These sizes are at the boundaries of gc.c's heap: the smallest cell,
    // the step from 8-byte to 32-byte classes, the largest cell (504 bytes of
    // payload), and the first size that comes from malloc. Half are rooted, so
    // a wrong class or an overlapping cell corrupts a payload.
    static const int64_t sizes[] = {1, 8, 9, 16, 24, 121, 128, 129, 248, 504, 505, 4096, 100000};
    const int nsizes = (int)(sizeof sizes / sizeof *sizes);

    int64_t keep_slots[13] = {0};
    TypeDesc *keep_tds[13];
    for (int i = 0; i < nsizes; i++) keep_tds[i] = &rt_td_block;
    GCFrame keep = {NULL, nsizes, keep_tds, keep_slots};
    rt_gc_top = &keep;

    for (int i = 0; i < nsizes; i++) {
        char *dropped = rt_alloc(sizes[i]);
        memset(dropped, 0xEE, (size_t)sizes[i]);
        char *kept = rt_alloc(sizes[i]);
        memset(kept, (int)(i + 1), (size_t)sizes[i]);
        keep_slots[i] = (int64_t)(intptr_t)kept;
    }
    rt_gc_collect();
    check("every size class: survivors", rt_gc_live_objects(), nsizes);

    int intact = 1;
    int64_t kept_bytes = 0;
    for (int i = 0; i < nsizes; i++) {
        const unsigned char *p = (const unsigned char *)(intptr_t)keep_slots[i];
        for (int64_t j = 0; j < sizes[i]; j++) {
            if (p[j] != (unsigned char)(i + 1)) intact = 0;
        }
        kept_bytes += sizes[i];
    }
    check("every size class: payloads intact", intact, 1);
    check("every size class: live bytes", rt_gc_live_bytes(), kept_bytes);

    // Dropping them frees both small cells and large blocks.
    for (int i = 0; i < nsizes; i++) keep_slots[i] = 0;
    rt_gc_collect();
    check("every size class: all reclaimed", rt_gc_live_objects(), 0);

    // ---- 12. cells and chunks are recycled, not just abandoned ----
    // Much allocation in one class past the threshold must reach a steady
    // state: the same chunks, carved again, with the same single survivor.
    void *survivor = rt_alloc(24);
    memset(survivor, 0x5A, 24);
    keep_slots[0] = (int64_t)(intptr_t)survivor;
    keep.nroots = 1;
    before = rt_gc_collections();
    for (int i = 0; i < 200000; i++) {
        char *tmp = rt_alloc(24);
        memset(tmp, 0xEE, 24);
    }
    check("recycling: collections happened", rt_gc_collections() > before, 1);
    check("recycling: heap stays bounded", rt_gc_live_bytes() <= 64, 1);
    intact = 1;
    for (int i = 0; i < 24; i++) {
        if (((unsigned char *)survivor)[i] != 0x5A) intact = 0;
    }
    check("recycling: survivor intact", intact, 1);

    rt_gc_top = NULL;
    rt_gc_collect();
    check("recycling: everything reclaimed", rt_gc_live_objects(), 0);

    // ---- 13. a spike's chunks are kept, then given back when it is over ----
    // One burst of garbage, built with collection deferred so that all of it
    // exists at once. The sweep pools its ~190 chunks and must keep them. The
    // reserve in gc.c records that the program just needed that many. A
    // server between two waves of load has the same state.
    int64_t rel = rt_gc_chunks_released();
    rt_gc_disable();
    for (int i = 0; i < 100000; i++) rt_alloc(120);
    rt_gc_enable();
    rt_gc_collect();
    check("chunk release: a spike's chunks are kept for the next wave",
          rt_gc_chunks_released(), rel);
    check("chunk release: and held in the pool, not in use",
          rt_gc_chunks_pooled() > 100, 1);

    // Only the collection that an idle scheduler requests lowers the reserve.
    // The hint does nothing when nothing was allocated since the last
    // collection, so the program must allocate first.
    rt_alloc(8);
    int64_t t = rt_mono_ms();
    while (rt_mono_ms() - t < 4) { }
    rt_gc_idle_hint();
    check("chunk release: going idle hands them to the OS",
          rt_gc_chunks_released() > rel, 1);
    check("chunk release: leaving only the pool floor",
          rt_gc_chunks_pooled() <= 16, 1);

    printf(failures ? "\nFAILED (%d)\n" : "\nPASS\n", failures);
    return failures != 0;
}
