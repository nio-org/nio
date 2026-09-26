// The `async` standard library module. The build links it only for a program
// that imports 'async'. The run queue, the await and the drain are core,
// because an async function and an await work with no import.
//
// The signatures must match asyncCallType in the checker and the async case of
// genMemberCall in codegen.
//
// async.race is core, beside the scheduler that decides which future completes
// first. This file holds only async.run.

#include "runtime.h"

// Runs cb with the result of fu when fu completes. A future that is already
// complete calls back at once. Otherwise the callback runs at the completion,
// through an await or through the drain at the end of the program.
void rt_async_run(Future *fu, void *clos) {
    if (!fu) rt_panic("async.run on a future that was never assigned");
    if (!clos) rt_panic("called a function value that was never assigned");
    if (fu->state == FUT_DONE) {
        rt_future_call_cb(fu, clos);
        return;
    }

    TypeDesc *tds[2] = {&rt_td_future, &rt_td_func};
    int64_t slots[2] = {(int64_t)(intptr_t)fu, (int64_t)(intptr_t)clos};
    GCFrame f = {rt_gc_top, 2, tds, slots, NULL};
    rt_gc_top = &f;

    FutCB *node = rt_alloc(sizeof(FutCB));
    node->clos = (void *)(intptr_t)slots[1];
    rt_future_add_cb((Future *)(intptr_t)slots[0], node);

    rt_gc_top = f.prev;
}
