/* Synthetic stress-test harness — Phase 5 first slice, part 3.
 * NOT the real node-graph or NPC system (neither exists yet); this exists
 * only to get real numbers against the two concrete scale targets given
 * for this work: ~5000 nodes in a graph, ~280 (200 NPCs + 80 vehicles)
 * concurrent lightweight scripted entities. Native only — this is about
 * getting honest measurements to inform later design, not a build-target
 * parity requirement the way correctness tests are.
 *
 * Test 1: call a trivial Python function from C 5000 times in a row,
 * timed — matches the node evaluator's "dispatch once per node" cost
 * model (phi.md Phase 6): a topological walk calling each node's fn once.
 *
 * Test 2: create ~280 Python generator objects (a plain `yield`-based
 * generator here, not uasyncio specifically — real uasyncio needs a
 * filesystem/frozen-module import this no-filesystem embedding config
 * doesn't have, see client/mpconfigport.h; generators and async-def
 * coroutines are driven through the exact same C-level resume mechanism,
 * mp_obj_gen_resume, so this measures the same underlying per-resume
 * interpreter overhead uasyncio's scheduler would pay per task tick) and
 * manually round-robin-drive them for a few rounds each, timed — matches
 * "many mostly-idle NPCs/vehicles, each doing a small amount of work per
 * tick" rather than "run each one's full behavior synchronously". */
#include <stdio.h>
#include <time.h>
#include "port/micropython_embed.h"
#include "py/obj.h"
#include "py/objgenerator.h"
#include "py/objlist.h"
#include "py/runtime.h"
#include "py/stackctrl.h"

static char mp_heap[256 * 1024];  /* bigger heap than mp_test_main.c -- 5000+280 live objects */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static mp_obj_t lookup_global(const char *name) {
    return mp_obj_dict_get(MP_OBJ_FROM_PTR(mp_globals_get()), MP_OBJ_NEW_QSTR(qstr_from_str(name)));
}

int main(void) {
    int stack_top;
    int fail = 0;
    mp_embed_init(&mp_heap[0], sizeof(mp_heap), &stack_top);
    mp_stack_set_limit(64 * 1024);

    /* ---- Test 1: 5000 node-like dispatches ---- */
    printf("[stress] === Test 1: 5000 sequential Python function calls from C ===\n");
    mp_embed_exec_str("def node_fn(x):\n    return x + 1\n");
    mp_obj_t node_fn = lookup_global("node_fn");

    nlr_buf_t nlr;
    const int NUM_NODES = 5000;
    if (nlr_push(&nlr) == 0) {
        double t0 = now_s();
        mp_obj_t v = mp_obj_new_int(0);
        for (int i = 0; i < NUM_NODES; i++) {
            mp_obj_t args[1] = { v };
            v = mp_call_function_n_kw(node_fn, 1, 0, args);
        }
        double t1 = now_s();
        int final_val = mp_obj_get_int(v);
        double total_ms = (t1 - t0) * 1000.0;
        double per_call_us = (t1 - t0) * 1e6 / NUM_NODES;
        printf("[stress] %d dispatches: %.2f ms total, %.3f us/call, final=%d (expected %d)\n",
               NUM_NODES, total_ms, per_call_us, final_val, NUM_NODES);
        printf("[stress] at 16.6ms/frame budget, this leaves room for a %.0fx larger graph "
               "fully re-evaluating every single frame, or (more realistically per phi.md's "
               "dirty-flagging model) a huge margin if only a small changed subgraph "
               "re-evaluates per frame instead of all %d nodes\n",
               16.6 / total_ms, NUM_NODES);
        if (final_val != NUM_NODES) fail = 1;
        nlr_pop();
    } else {
        printf("[stress] EXCEPTION in test 1:\n");
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        fail = 1;
    }

    /* ---- Test 2: ~280 concurrent lightweight generator/coroutine ticks ---- */
    printf("[stress] === Test 2: 280 concurrent generators (3 yields, 4 resumes to finish each) ===\n");
    mp_embed_exec_str(
        "def make_tasks(n):\n"
        "    def task(idx):\n"
        "        total = 0\n"
        "        for i in range(3):\n"
        "            yield\n"
        "            total += i\n"
        "        return total\n"
        "    return [task(i) for i in range(n)]\n"
    );
    const int NUM_ENTITIES = 280;
    /* 4, not 3: a generator with 3 `yield` statements suspends AT each of
     * them (resume 1 -> reaches yield #1, resume 2 -> yield #2, resume 3 ->
     * yield #3) and only actually returns (raises StopIteration) on a 4th
     * resume that runs the rest of the loop body past the last yield --
     * standard generator semantics, confirmed by an off-by-one in this
     * test's first version (expected 280 finished after 3 rounds, got 0;
     * root cause was this arithmetic, not a MicroPython bug). */
    const int TICKS_PER_ENTITY = 4;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t make_tasks = lookup_global("make_tasks");
        mp_obj_t n_arg[1] = { mp_obj_new_int(NUM_ENTITIES) };
        mp_obj_t tasks_list = mp_call_function_n_kw(make_tasks, 1, 0, n_arg);
        size_t n_tasks; mp_obj_t *tasks;
        mp_obj_list_get(tasks_list, &n_tasks, &tasks);
        printf("[stress] created %zu generator objects (expected %d)\n", n_tasks, NUM_ENTITIES);
        if (n_tasks != (size_t)NUM_ENTITIES) fail = 1;

        double t0 = now_s();
        int total_resumes = 0, total_finished = 0;
        for (int round = 0; round < TICKS_PER_ENTITY; round++) {
            for (size_t i = 0; i < n_tasks; i++) {
                if (tasks[i] == MP_OBJ_NULL) continue;  /* already finished */
                mp_obj_t ret_val;
                mp_vm_return_kind_t kind = mp_obj_gen_resume(tasks[i], mp_const_none, MP_OBJ_NULL, &ret_val);
                total_resumes++;
                if (kind == MP_VM_RETURN_NORMAL) {
                    tasks[i] = MP_OBJ_NULL;
                    total_finished++;
                } else if (kind == MP_VM_RETURN_EXCEPTION) {
                    printf("[stress] unexpected exception resuming task %zu\n", i);
                    fail = 1;
                }
            }
        }
        double t1 = now_s();
        double total_ms = (t1 - t0) * 1000.0;
        double per_resume_us = (t1 - t0) * 1e6 / total_resumes;
        printf("[stress] %d resumes across %d rounds: %.2f ms total, %.3f us/resume, "
               "%d/%d tasks finished (expected %d finished after %d rounds)\n",
               total_resumes, TICKS_PER_ENTITY, total_ms, per_resume_us,
               total_finished, NUM_ENTITIES, NUM_ENTITIES, TICKS_PER_ENTITY);
        printf("[stress] at 16.6ms/frame budget, ticking all %d entities once costs %.3f ms "
               "(%.1f%% of one frame) in this synthetic worst case (every entity resumes every "
               "single frame -- the real design staggers/LODs this, see phi.md Phase 7)\n",
               NUM_ENTITIES, (total_ms / TICKS_PER_ENTITY), (total_ms / TICKS_PER_ENTITY) / 16.6 * 100.0);
        if (total_finished != NUM_ENTITIES) fail = 1;
        nlr_pop();
    } else {
        printf("[stress] EXCEPTION in test 2:\n");
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        fail = 1;
    }

    mp_embed_deinit();
    printf(fail ? "[stress] FAIL (correctness check failed, not just a timing report)\n" : "[stress] PASS\n");
    return fail;
}
