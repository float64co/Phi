/* Standalone MicroPython embedding self-test — Phase 5 first slice.
 * NOT part of the game build (phi_native/phi_win32/game.wasm) yet; this is
 * its own small executable (`make mp_test`) whose only job is proving the
 * embedding itself works, independent of the game loop, so nothing in the
 * shipped binary changes size/behavior until Phase 5's real C API surface
 * is designed. See phi.md's Phase 5 section and its CRITICAL WARNING about
 * validating decorator patterns before any binding code is written.
 *
 * Round-trip covered here:
 *   1. exec a Python script, call a function it defined from C, read the
 *      return value back.
 *   2. expose a C function to Python, call it from a Python script, confirm
 *      the C side actually ran (not just that the call didn't crash).
 *   3. an uncaught Python exception is caught and printed, not a process
 *      crash or hang.
 *   4. the @phi.panel class-decorator pattern (phi.md Phase 1): a class
 *      inheriting from a plain Python base, decorated to register it,
 *      instantiated and its overridden method called from C, with real
 *      persistent instance state (not re-evaluated fresh each call).
 *   5. the @phi.node function-decorator pattern (phi.md Phase 6): a
 *      decorator taking inputs=/outputs= socket-definition arguments,
 *      registering the wrapped function, with the resulting registry
 *      introspectable from C (phi.node_types()' actual mechanism) and the
 *      node function itself callable from C during evaluation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "octree_render.h"
#include "port/micropython_embed.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/objtuple.h"
#include "py/runtime.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/stackctrl.h"

static char mp_heap[64 * 1024];

static int g_native_called_with = -1;
static mp_obj_t native_double(mp_obj_t x) {
    int v = mp_obj_get_int(x);
    g_native_called_with = v;
    return mp_obj_new_int(v * 2);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_double_obj, native_double);

/* Calls a Python callable from C with (a, b), nlr-protected so a Python
 * exception raised during the call is caught here rather than escaping to
 * mp_embed_exec_str's own top-level handler (or, if genuinely unhandled,
 * nlr_jump_fail's infinite loop -- see mp_port.c's comment on why every
 * MicroPython entry point from C needs its own nlr_push). */
static int call_py_add(mp_obj_t fn, int a, int b, int *out, int *raised) {
    nlr_buf_t nlr;
    *raised = 0;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t args[2] = { mp_obj_new_int(a), mp_obj_new_int(b) };
        mp_obj_t result = mp_call_function_n_kw(fn, 2, 0, args);
        *out = mp_obj_get_int(result);
        nlr_pop();
        return 1;
    } else {
        *raised = 1;
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        return 0;
    }
}

static mp_obj_t lookup_global(const char *name) {
    return mp_obj_dict_get(MP_OBJ_FROM_PTR(mp_globals_get()), MP_OBJ_NEW_QSTR(qstr_from_str(name)));
}

/* Stub -- mp_port.c now links scene_objects.c (Phase 5's phi.delete_
 * object etc., see mp_port.c's own comment), whose scene_object_delete
 * references mesh_destroy (octree_render.c's real home, a GL-touching
 * file this no-GL test has no reason to link -- same "stub the one
 * referenced symbol rather than pull in the whole GL-touching
 * translation unit" technique fracture_body_test_main.c already
 * established). This test never actually calls phi.delete_object, but
 * the linker still needs a real definition for the symbol scene_
 * objects.c's translation unit references. Real free(), not a no-op. */
void mesh_destroy(RenderMesh *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

int main(void) {
    int stack_top;
    int fail = 0;

    mp_embed_init(&mp_heap[0], sizeof(mp_heap), &stack_top);
    /* mp_embed_init only calls the deprecated mp_stack_set_top(), which
     * never sets stack_limit -- py/cstack.c's mp_cstack_check() (what's
     * actually compiled in at this ROM level) reads that same state and
     * treats the default-zero limit as "already over budget", raising a
     * false RecursionError on the very first check, caught nowhere, which
     * hangs the process in nlr_jump_fail's infinite loop. Root-caused via
     * gdb backtrace during this slice's development, not guessed --
     * mp_stack_set_limit() is the old (but still functional) stackctrl.c
     * API and writes the same MP_STATE_THREAD(stack_limit) cstack.c reads.
     * A real bug in this exact ports/embed + py/core combination, worth
     * upstreaming a fix for, but working around it here is correct either way. */
    mp_stack_set_limit(32 * 1024);

    printf("[mp_test] === 1: call a Python function from C ===\n");
    mp_embed_exec_str("def add(a, b):\n    return a + b\n");
    mp_obj_t add_fn = lookup_global("add");
    int result = 0, raised = 0;
    int ok = call_py_add(add_fn, 3, 4, &result, &raised);
    printf("[mp_test] add(3,4) -> ok=%d result=%d (expected ok=1 result=7)\n", ok, result);
    if (!(ok == 1 && result == 7)) fail = 1;

    printf("[mp_test] === 2: expose a C function to Python, call it from a script ===\n");
    mp_obj_dict_store(MP_OBJ_FROM_PTR(mp_globals_get()),
                       MP_OBJ_NEW_QSTR(qstr_from_str("native_double")),
                       MP_OBJ_FROM_PTR(&native_double_obj));
    mp_embed_exec_str("result_holder = native_double(21)\nprint('py side saw:', result_holder)");
    mp_obj_t rh = lookup_global("result_holder");
    int rh_val = mp_obj_get_int(rh);
    printf("[mp_test] native_double(21) -> C saw call with %d, python got %d (expected 21, 42)\n",
           g_native_called_with, rh_val);
    if (!(g_native_called_with == 21 && rh_val == 42)) fail = 1;

    printf("[mp_test] === 3: exception safety ===\n");
    mp_embed_exec_str("def bad(a, b):\n    raise ValueError('deliberate test exception')\n");
    mp_obj_t bad_fn = lookup_global("bad");
    int result2 = -999, raised2 = 0;
    int ok2 = call_py_add(bad_fn, 1, 2, &result2, &raised2);
    printf("[mp_test] bad(1,2) -> ok=%d raised=%d (expected ok=0 raised=1) -- process still alive\n", ok2, raised2);
    if (!(ok2 == 0 && raised2 == 1)) fail = 1;

    printf("[mp_test] === 4: @phi.panel class-decorator pattern ===\n");
    mp_embed_exec_str(
        "_panel_registry = {}\n"
        "class Panel:\n"
        "    pass\n"
        "def panel(name):\n"
        "    def decorator(cls):\n"
        "        _panel_registry[name] = cls\n"
        "        return cls\n"
        "    return decorator\n"
        "@panel('Fracture Tools')\n"
        "class FracturePanel(Panel):\n"
        "    def __init__(self):\n"
        "        self.call_count = 0\n"
        "    def draw(self, ctx):\n"
        "        self.call_count += 1\n"
        "        return ctx * 2\n"
    );
    {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_obj_t registry = lookup_global("_panel_registry");
            mp_obj_t cls = mp_obj_dict_get(registry, mp_obj_new_str("Fracture Tools", 14));
            mp_obj_t instance = mp_call_function_n_kw(cls, 0, 0, NULL);
            mp_obj_t draw_bound = mp_load_attr(instance, qstr_from_str("draw"));
            mp_obj_t args[1] = { mp_obj_new_int(21) };
            mp_obj_t result = mp_call_function_n_kw(draw_bound, 1, 0, args);
            int result_val = mp_obj_get_int(result);
            mp_obj_t call_count = mp_load_attr(instance, qstr_from_str("call_count"));
            int cc = mp_obj_get_int(call_count);
            printf("[mp_test] FracturePanel().draw(21) -> %d, call_count -> %d "
                   "(expected 42, 1)\n", result_val, cc);
            if (!(result_val == 42 && cc == 1)) fail = 1;
            nlr_pop();
        } else {
            printf("[mp_test] EXCEPTION in step 4:\n");
            mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
            fail = 1;
        }
    }

    printf("[mp_test] === 5: @phi.node function-decorator pattern ===\n");
    /* Note: NOT __annotations__-based, deliberately -- see mpconfigport.h's
     * comment. MicroPython parses PEP 3107 annotation syntax but never
     * stores it (no __annotations__ on function objects at all, confirmed
     * empirically), which would have broken phi.md's Phase 6 sketch if it
     * had actually relied on that. It doesn't: the sketch already passes
     * inputs=/outputs= as plain decorator keyword arguments, which is what
     * this exercises. */
    mp_embed_exec_str(
        "_node_registry = {}\n"
        "def node(inputs, outputs):\n"
        "    def decorator(fn):\n"
        "        _node_registry[fn.__name__] = {'inputs': inputs, 'outputs': outputs, 'fn': fn}\n"
        "        return fn\n"
        "    return decorator\n"
        "@node(inputs=[('mesh', 'Mesh'), ('scale', 'float', 1.0)], outputs=[('mesh', 'Mesh')])\n"
        "def noise_displace(mesh, scale):\n"
        "    return mesh + scale\n"
    );
    {
        nlr_buf_t nlr;
        if (nlr_push(&nlr) == 0) {
            mp_obj_t registry = lookup_global("_node_registry");
            mp_obj_t entry = mp_obj_dict_get(registry, mp_obj_new_str("noise_displace", 14));
            mp_obj_t inputs = mp_obj_dict_get(entry, mp_obj_new_str("inputs", 6));
            size_t n_inputs; mp_obj_t *input_items;
            mp_obj_list_get(inputs, &n_inputs, &input_items);
            size_t n_fields; mp_obj_t *fields;
            mp_obj_tuple_get(input_items[0], &n_fields, &fields);
            const char *first_name = mp_obj_str_get_str(fields[0]);
            mp_obj_t fn = mp_obj_dict_get(entry, mp_obj_new_str("fn", 2));
            mp_obj_t args[2] = { mp_obj_new_int(10), mp_obj_new_int(5) };
            mp_obj_t result = mp_call_function_n_kw(fn, 2, 0, args);
            int result_val = mp_obj_get_int(result);
            printf("[mp_test] node_types()['noise_displace'].inputs[0][0] -> '%s', "
                   "fn(10,5) -> %d (expected 'mesh', 15)\n", first_name, result_val);
            if (!(strcmp(first_name, "mesh") == 0 && n_inputs == 2 && result_val == 15)) fail = 1;
            nlr_pop();
        } else {
            printf("[mp_test] EXCEPTION in step 5:\n");
            mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
            fail = 1;
        }
    }

    mp_embed_deinit();

    printf(fail ? "[mp_test] FAIL\n" : "[mp_test] PASS\n");
    return fail;
}
