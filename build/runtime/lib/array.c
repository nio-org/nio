// The `array` standard library module (import 'array'), linked only when a
// program imports it. Signatures must match arrayCallType in the checker and
// genArrayCall in codegen.
//
// What mutates does so in place: the header keeps its address, and only the
// slot block moves.
//
// An element may be a pointer whose only root is the array that holds it, so
// nothing here may keep one where the collector cannot see it while anything
// allocates. This matters most for sort: its comparator may allocate on every
// call, so sort merges through a second Nio array.

#include "runtime.h"

#include <stdio.h>
#include <string.h>

// rt_arr_push is in runtime.c: lib/json.c grows an array through it, and one
// library cannot depend on another.

int64_t rt_arr_pop(Arr *a, TypeDesc *arr_td) {
    if (a->len == 0) rt_panic("pop from an empty array");
    a->len--;
    int64_t v = rt_arr_get(a, a->len, arr_td->elem);
    // Nothing traces an element past len, so clear it rather than leave a
    // pointer the collector cannot see.
    rt_arr_set(a, a->len, 0, arr_td->elem);
    return v;
}

Arr *rt_arr_copy(Arr *a, TypeDesc *arr_td) {
    // Nothing roots the source parameter, and it is held across rt_arr_new.
    TypeDesc *tds[1] = {arr_td};
    int64_t slots[1] = {(int64_t)(intptr_t)a};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *r = rt_arr_new(a->len, a->width);
    memcpy(r->data, a->data, (size_t)a->len * a->width);

    rt_gc_top = f.prev;
    return r;
}

int64_t rt_arr_index_of(Arr *a, int64_t v, TypeDesc *arr_td) {
    // rt_eq allocates nothing, so nothing needs a root across the scan. The
    // element types that reach here are the ones == accepts (§3.2).
    for (int64_t i = 0; i < a->len; i++) {
        if (rt_eq(rt_arr_get(a, i, arr_td->elem), v, arr_td->elem)) return i;
    }
    return -1;
}

Arr *rt_arr_slice(Arr *a, int64_t start, int64_t end, TypeDesc *arr_td) {
    // The indices come from the caller's arithmetic. A pair outside the array
    // is a defect in the program, so it panics.
    if (start < 0 || end < start || end > a->len) {
        char msg[128];
        snprintf(msg, sizeof msg, "slice [%lld, %lld) out of range (array length %lld)",
                 (long long)start, (long long)end, (long long)a->len);
        rt_panic(msg);
    }

    TypeDesc *tds[1] = {arr_td};
    int64_t slots[1] = {(int64_t)(intptr_t)a};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *r = rt_arr_new(end - start, a->width);
    memcpy(r->data, (const char *)a->data + start * a->width,
           (size_t)(end - start) * a->width);

    rt_gc_top = f.prev;
    return r;
}

// ---- array.sort(a) and array.sort(a, cmp) ----

// Both forms of sort answer one question: whether an element comes before
// another. This record holds either answer, so the merge is written once.
typedef struct {
    void *clos;     // NULL: natural order, decided from `elem` instead
    TypeDesc *elem; // the element descriptor, for that decision
    int is_float;   // whether a comparator takes its arguments as doubles
    Arr *arr;       // the array being sorted, with the length it had, to catch
    int64_t len;    // a comparator that changes it under the sort
} Order;

static int before(const Order *o, int64_t x, int64_t y) {
    if (o->clos) {
        // The closure contract is codegen's (see rt_future_call_cb): slot 0
        // holds the code pointer and the block is the hidden first argument. A
        // float element travels in a float register, so the prototype says so.
        void *code;
        memcpy(&code, o->clos, 8);
        int64_t answer;
        if (o->is_float) {
            double a, b;
            memcpy(&a, &x, 8);
            memcpy(&b, &y, 8);
            answer = ((int64_t (*)(void *, double, double))code)(o->clos, a, b);
        } else {
            answer = ((int64_t (*)(void *, int64_t, int64_t))code)(o->clos, x, y);
        }
        // A comparator that pushes or pops is a defect. The merge walks indices
        // it read before the call, so the output would be neither array.
        if (o->arr->len != o->len) {
            rt_panic("array.sort: the comparator changed the array's length");
        }
        return answer != 0;
    }
    switch (o->elem->kind) {
    case TD_STRING:
        return rt_str_cmp((Str *)(intptr_t)x, (Str *)(intptr_t)y) < 0;
    case TD_FLOAT:
    case TD_FLOAT32: {
        double a, b;
        memcpy(&a, &x, 8);
        memcpy(&b, &y, 8);
        // NaN comes before every number. Under `a < b` alone it is before
        // nothing and after nothing, so the result would not be sorted.
        return a < b || (a != a && b == b);
    }
    case TD_UINT8:
    case TD_UINT16:
    case TD_UINT32:
    case TD_UINT:
        // `<` on two unsigned operands is unsigned (§2.1). A uint64 with its
        // top bit set is a large positive number.
        return (uint64_t)x < (uint64_t)y;
    default: // the signed integer types, DateTime, Duration, enum
        return x < y;
    }
}

// A stable bottom-up merge sort. A caller cannot add stability afterwards, the
// worst case equals the average, and elements move only between two rooted
// Nio arrays.
//
// The comparator may allocate, and so collect, on every call. The collector
// cannot see malloc'd scratch and would free elements held there, so `tmp`
// comes from rt_arr_new.
//
// `run` is the length of the runs merged on this pass. Elements move as 8-byte
// units through rt_arr_get/rt_arr_set.
static void merge_sort(Arr *a, Arr *tmp, const Order *o) {
    int64_t n = a->len;
    for (int64_t run = 1; run < n; run *= 2) {
        for (int64_t lo = 0; lo < n; lo += 2 * run) {
            int64_t mid = lo + run, hi = lo + 2 * run;
            if (mid > n) mid = n;
            if (hi > n) hi = n;
            int64_t i = lo, j = mid, k = lo;
            // Take the right element only when it comes strictly before the
            // left one. That is what keeps equal elements in their order.
            while (i < mid && j < hi) {
                int64_t x = rt_arr_get(a, j, o->elem);
                int64_t y = rt_arr_get(a, i, o->elem);
                if (before(o, x, y)) {
                    rt_arr_set(tmp, k++, x, o->elem);
                    j++;
                } else {
                    rt_arr_set(tmp, k++, y, o->elem);
                    i++;
                }
            }
            while (i < mid) rt_arr_set(tmp, k++, rt_arr_get(a, i++, o->elem), o->elem);
            while (j < hi) rt_arr_set(tmp, k++, rt_arr_get(a, j++, o->elem), o->elem);
        }
        // Each pass copies back instead of exchanging the two arrays. The
        // result stays in `a`, so every element stays reachable while a
        // comparator runs.
        memcpy(a->data, tmp->data, (size_t)n * a->width);
    }
}

// `clos` is NULL for the form that sorts in the natural order. Fewer than two
// elements need no sort, which also keeps an empty array from allocating.
static void sort_with(Arr *a, void *clos, TypeDesc *arr_td) {
    if (a->len < 2) return;

    // Three parameters that nothing roots, held across rt_arr_new and every
    // call of the comparator. The scratch holds the same element type, so one
    // descriptor covers both arrays.
    TypeDesc *tds[3] = {arr_td, arr_td, &rt_td_func};
    int64_t slots[3] = {(int64_t)(intptr_t)a, 0, (int64_t)(intptr_t)clos};
    GCFrame f = {rt_gc_top, 3, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *tmp = rt_arr_new(a->len, a->width);
    slots[1] = (int64_t)(intptr_t)tmp;

    int is_float = arr_td->elem->kind == TD_FLOAT || arr_td->elem->kind == TD_FLOAT32;
    Order o = {clos, arr_td->elem, is_float, a, a->len};
    merge_sort(a, tmp, &o);

    rt_gc_top = f.prev;
}

void rt_arr_sort(Arr *a, TypeDesc *arr_td) { sort_with(a, NULL, arr_td); }

void rt_arr_sort_by(Arr *a, void *cmp, TypeDesc *arr_td) {
    // The zero value of a function type, met as a direct call of one meets it.
    if (!cmp) rt_panic("called a function value that was never assigned");
    sort_with(a, cmp, arr_td);
}

// ---- §6.5: the functions that take no callback ----
//
// Each moves or copies elements at the array's own width (§5.7). The two that
// allocate root their arguments across rt_arr_new, as rt_arr_slice does. The
// functions that call back into Nio (map, filter, reduce, find, some, every)
// are not here: codegen emits their loops, so the values they hold are
// ordinary roots.

void rt_arr_reverse(Arr *a) {
    uint8_t *d = (uint8_t *)a->data;
    size_t w = (size_t)a->width;
    uint8_t tmp[8];
    for (int64_t i = 0, j = a->len - 1; i < j; i++, j--) {
        memcpy(tmp, d + (size_t)i * w, w);
        memcpy(d + (size_t)i * w, d + (size_t)j * w, w);
        memcpy(d + (size_t)j * w, tmp, w);
    }
}

void rt_arr_fill(Arr *a, int64_t raw, TypeDesc *arr_td) {
    for (int64_t i = 0; i < a->len; i++) rt_arr_set(a, i, raw, arr_td->elem);
}

Arr *rt_arr_concat(Arr *a, Arr *b, TypeDesc *arr_td) {
    TypeDesc *tds[2] = {arr_td, arr_td};
    int64_t slots[2] = {(int64_t)(intptr_t)a, (int64_t)(intptr_t)b};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    Arr *r = rt_arr_new(a->len + b->len, a->width);
    memcpy(r->data, a->data, (size_t)a->len * (size_t)a->width);
    memcpy((char *)r->data + (size_t)a->len * (size_t)a->width, b->data,
           (size_t)b->len * (size_t)b->width);

    rt_gc_top = f.prev;
    return r;
}

// a is a T[][]: its elements are pointers to T[] arrays, and arr_td->elem
// describes one of them. An element never assigned is null and counts as
// empty. The width comes from the descriptor, since an empty outer array has
// no inner one to ask.
Arr *rt_arr_flatten(Arr *a, TypeDesc *arr_td) {
    TypeDesc *inner_td = arr_td->elem;
    int64_t total = 0;
    for (int64_t i = 0; i < a->len; i++) {
        Arr *x = (Arr *)(intptr_t)rt_arr_units(a)[i];
        if (x) total += x->len;
    }

    TypeDesc *tds[1] = {arr_td};
    int64_t slots[1] = {(int64_t)(intptr_t)a};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    int64_t w = rt_arr_width(inner_td->elem->kind);
    Arr *r = rt_arr_new(total, w);
    char *out = (char *)r->data;
    for (int64_t i = 0; i < a->len; i++) {
        Arr *x = (Arr *)(intptr_t)rt_arr_units(a)[i];
        if (!x) continue;
        memcpy(out, x->data, (size_t)x->len * (size_t)w);
        out += (size_t)x->len * (size_t)w;
    }

    rt_gc_top = f.prev;
    return r;
}
