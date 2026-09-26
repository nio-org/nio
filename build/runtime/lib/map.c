// The `map` standard library module. The build links it only for a program
// that imports 'map'. The map type itself, with indexing, a map literal and
// .length, is core in runtime.c.
//
// The signatures must match mapCallType in the checker and genMapCall in
// codegen. The layout rules are with the Map struct in runtime.h.

#include "runtime.h"

#include <string.h>

int64_t rt_map_has(Map *m, int64_t key, TypeDesc *map_td) {
    return rt_map_find(m, key, map_td->field_types[0]) >= 0;
}

int64_t rt_map_remove(Map *m, int64_t key, TypeDesc *map_td) {
    TypeDesc *ktd = map_td->field_types[0];
    int64_t pos = rt_map_find(m, key, ktd);
    if (pos < 0) return 0;
    // Compacting the entry blocks keeps insertion order with no mark for a
    // removed entry, and costs the O(n) reindex, because every position after
    // pos moved. It allocates nothing. The blocks pack at the width of each
    // component (runtime.h), so the byte arithmetic scales by that width.
    int64_t kw = rt_arr_width(ktd->kind);
    int64_t vw = rt_arr_width(map_td->field_types[1]->kind);
    char *k = m->keys, *v = m->vals;
    memmove(k + pos * kw, k + (pos + 1) * kw, (size_t)((m->len - pos - 1) * kw));
    memmove(v + pos * vw, v + (pos + 1) * vw, (size_t)((m->len - pos - 1) * vw));
    m->len--;
    // Nothing traces the entries past len. Clear them so no stale pointer
    // stays in memory.
    memset(k + m->len * kw, 0, (size_t)kw);
    memset(v + m->len * vw, 0, (size_t)vw);
    rt_map_reindex(m, ktd);
    return 1;
}

// Copies one of the entry blocks into a new language array.
static Arr *map_entries(Map *m, TypeDesc *map_td, int vals) {
    TypeDesc *tds[1] = {map_td};
    int64_t slots[1] = {(int64_t)(intptr_t)m};
    GCFrame f = {rt_gc_top, 1, tds, slots, NULL};
    rt_gc_top = &f;

    // The entry block and the array pack the component the same way (§5.7), so
    // the copy is one memcpy.
    TypeDesc *elem = map_td->field_types[vals ? 1 : 0];
    int64_t w = rt_arr_width(elem->kind);
    Arr *a = rt_arr_new(m->len, w);
    memcpy(a->data, vals ? m->vals : m->keys, (size_t)(m->len * w));

    rt_gc_top = f.prev;
    return a;
}

Arr *rt_map_keys(Map *m, TypeDesc *map_td) { return map_entries(m, map_td, 0); }

Arr *rt_map_values(Map *m, TypeDesc *map_td) { return map_entries(m, map_td, 1); }

Map *rt_map_copy(Map *m, TypeDesc *map_td) {
    // The copy roots under its own descriptor and stays a valid map value
    // throughout, because len rises only after the entries are in place.
    TypeDesc *tds[2] = {map_td, map_td};
    int64_t slots[2] = {(int64_t)(intptr_t)m, 0};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    Map *r = rt_map_new();
    slots[1] = (int64_t)(intptr_t)r;
    if (m->len > 0) {
        int64_t kw = rt_arr_width(map_td->field_types[0]->kind);
        int64_t vw = rt_arr_width(map_td->field_types[1]->kind);
        r->keys = rt_alloc(m->len * kw);
        r->vals = rt_alloc(m->len * vw);
        r->cap = m->len;
        int64_t icap = 8;
        while (m->len * 3 > icap * 2) icap *= 2;
        r->index = rt_alloc(icap * 8);
        r->icap = icap;
        memcpy(r->keys, m->keys, (size_t)(m->len * kw));
        memcpy(r->vals, m->vals, (size_t)(m->len * vw));
        r->len = m->len;
        rt_map_reindex(r, map_td->field_types[0]);
    }

    rt_gc_top = f.prev;
    return r;
}
