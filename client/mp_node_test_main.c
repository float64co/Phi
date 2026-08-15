/* Standalone MicroPython + node_graph.c integration test -- Phase 6's
 * actual load-bearing ask (@phi.node registers a real node type,
 * phi.Graph builds a real C-owned graph, evaluate() really calls
 * registered Python functions in real topological order), exercised end
 * to end against a REAL embedded interpreter driving REAL node_graph.c
 * calls, not a mock of either. Same "prove it end to end, no GL/window
 * needed" precedent as mp_geometry_test_main.c, extended to cover node
 * graphs specifically. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "mp_port.h"
#include "scene_objects.h"
#include "halfedge_gltf.h"

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

/* Same real-free, not-a-no-op stub as mp_geometry_test_main.c's own --
 * see its comment for why this is needed regardless of whether this
 * test's own calls ever reach it. */
void mesh_destroy(RenderMesh *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

int main(void) {
    int stack_top;
    printf("[mp_node_test] === setup ===\n");
    scene_objects_init();
    phi_graph_system_init();
    phi_mp_init(&stack_top);

    printf("[mp_node_test] === 1: @phi.node registers a real type, phi.node_types() reflects it ===\n");
    char *out = phi_mp_exec(
        "@phi.node(inputs=[('a', float, 0.0), ('b', float, 0.0)], outputs=[('sum', float)], category='math')\n"
        "def add(a, b):\n"
        "    return a + b\n"
        "types = phi.node_types()\n"
        "print('add' in types)\n"
        "print(types['add']['category'])\n"
        "print(len(types['add']['inputs']))\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "True\nmath\n2\n") != NULL, "phi.node_types() reflects the real name/category/input-count exactly as registered (also proves fn.__name__ works in this MicroPython build)");
    free(out);

    printf("[mp_node_test] === 2: a real two-node chain evaluates via real topological execution ===\n");
    out = phi_mp_exec(
        "@phi.node(inputs=[('x', float, 0.0)], outputs=[('y', float)])\n"
        "def double(x):\n"
        "    return x * 2.0\n"
        "g = phi.Graph(kind='geometry')\n"
        "n0 = g.add_node('add', a=3.0, b=4.0)\n"
        "n1 = g.add_node('double', x=0.0)\n"
        "g.connect(n0, 'sum', n1, 'x')\n"
        "result = g.evaluate()\n"
        "print(result)\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "14.0\n") != NULL, "(3+4)*2 == 14.0 -- add's real output really fed double's real input through a real upstream link, not a coincidence of literal params");
    free(out);

    printf("[mp_node_test] === 3: an unconnected input falls back to its literal add_node(**params) value ===\n");
    out = phi_mp_exec(
        "g2 = phi.Graph()\n"
        "m0 = g2.add_node('add', a=10.0, b=5.0)\n"
        "print(g2.evaluate())\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "15.0\n") != NULL, "with no links at all, both inputs came from the node's own literal params (10+5=15)");
    free(out);

    printf("[mp_node_test] === 4: an unconnected input with NO literal param falls back to the function's own Python default ===\n");
    out = phi_mp_exec(
        "@phi.node(inputs=[('v', float, 99.0)], outputs=[('v', float)])\n"
        "def identity(v=99.0):\n"
        "    return v\n"
        "g3 = phi.Graph()\n"
        "i0 = g3.add_node('identity')\n"
        "print(g3.evaluate())\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "99.0\n") != NULL, "no link, no literal param -- the keyword was omitted entirely and identity's own Python default (99.0) took over, not a silently-substituted 0");
    free(out);

    printf("[mp_node_test] === 5: a real diamond -- one node consumes TWO upstream outputs ===\n");
    /*      make5     make7
     *         \        /
     *          \      /
     *           add(a,b)
     * Both of add's inputs come from DIFFERENT upstream nodes -- proves
     * per-socket link resolution, not just "the first link wins for
     * everything". */
    out = phi_mp_exec(
        "@phi.node(inputs=[], outputs=[('n', float)])\n"
        "def make5():\n"
        "    return 5.0\n"
        "@phi.node(inputs=[], outputs=[('n', float)])\n"
        "def make7():\n"
        "    return 7.0\n"
        "g4 = phi.Graph()\n"
        "p0 = g4.add_node('make5')\n"
        "p1 = g4.add_node('make7')\n"
        "p2 = g4.add_node('add', a=999.0, b=999.0)\n"   /* literal params deliberately WRONG -- must be overridden by the real links, not silently used */
        "g4.connect(p0, 'n', p2, 'a')\n"
        "g4.connect(p1, 'n', p2, 'b')\n"
        "print(g4.evaluate())\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "12.0\n") != NULL, "5+7==12.0 -- both real upstream links won over their nodes' own (deliberately wrong, 999) literal params, and each input resolved to the RIGHT upstream node, not just 'a' link");
    free(out);

    printf("[mp_node_test] === 6: a multi-output node's return tuple is unpacked at the right socket index ===\n");
    out = phi_mp_exec(
        "@phi.node(inputs=[('n', float, 0.0)], outputs=[('half', float), ('double', float)])\n"
        "def split(n):\n"
        "    return (n / 2.0, n * 2.0)\n"
        "@phi.node(inputs=[('v', float, 0.0)], outputs=[('v', float)])\n"
        "def passthru(v):\n"
        "    return v\n"
        "g5 = phi.Graph()\n"
        "s0 = g5.add_node('split', n=10.0)\n"
        "h0 = g5.add_node('passthru', v=0.0)\n"
        "d0 = g5.add_node('passthru', v=0.0)\n"
        "g5.connect(s0, 'half', h0, 'v')\n"
        "g5.connect(s0, 'double', d0, 'v')\n"
        "print(g5.evaluate())\n"   /* last-in-topo-order node's output */
    );
    printf("  captured: %s", out);
    /* split(10) -> (5.0, 20.0); d0 gets 'double' (index 1) == 20.0, and
     * since d0 is added after h0 it's the later topological entry (no
     * cross-dependency between h0/d0 to force a specific relative order
     * beyond "after s0", but this codebase's Kahn implementation picks
     * the lowest-index ready node first, so h0 before d0 -- d0 last). */
    check(strstr(out, "20.0\n") != NULL, "split's SECOND output (double, index 1) correctly reached d0 as 20.0, not accidentally the first output (5.0) or the raw unindexed tuple");
    free(out);

    printf("[mp_node_test] === 7: a node type wired to REAL engine state, not just toy math ===\n");
    out = phi_mp_exec(
        "@phi.node(inputs=[('object_id', float, 0.0)], outputs=[('vert_count', float)])\n"
        "def count_vertices(object_id):\n"
        "    return float(len(phi.get_vertices(int(object_id))) // 3)\n"
        "oid = phi.create_mesh([0,0,0, 1,0,0, 1,1,0, 0,1,0], [0,1,2, 0,2,3])\n"
        "g6 = phi.Graph()\n"
        "c0 = g6.add_node('count_vertices', object_id=float(oid))\n"
        "print(g6.evaluate())\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "4.0\n") != NULL, "count_vertices' node function called the REAL phi.get_vertices against a REAL MeshObject created via phi.create_mesh -- 4 real vertices, not a mock");
    free(out);

    printf("[mp_node_test] === 8: an empty (zero-node) graph is a legitimate edge case, not an error ===\n");
    out = phi_mp_exec("print(phi.Graph().evaluate())\n");
    printf("  captured: %s", out);
    check(strstr(out, "None\n") != NULL && strstr(out, "ValueError") == NULL, "evaluate() on a real graph with zero nodes returns None cleanly (see native_graph_evaluate's n>0 check) -- no spurious error for a legitimate edge case");
    free(out);

    printf("[mp_node_test] === 9: real error cases raise cleanly ===\n");
    out = phi_mp_exec(
        "g7 = phi.Graph()\n"
        "bad = g7.add_node('this_type_was_never_registered')\n"
        "g7.evaluate()\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "evaluate() on a node whose type was never @phi.node-registered raises a real ValueError");
    free(out);

    out = phi_mp_exec(
        "g8 = phi.Graph()\n"
        "u0 = g8.add_node('passthru', v=1.0)\n"
        "u1 = g8.add_node('passthru', v=1.0)\n"
        "g8.connect(u0, 'v', u1, 'v')\n"
        "g8.connect(u1, 'v', u0, 'v')\n"   /* closes a real 2-node cycle */
        "g8.evaluate()\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL && strstr(out, "cycle") != NULL, "evaluate() on a real cyclic graph raises a real ValueError naming the cycle, not an infinite loop or a wrong silent order");
    free(out);

    out = phi_mp_exec("_native_graph_add_node(999999, 'add')\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "add_node against a nonexistent graph id raises");
    free(out);

    printf("[mp_node_test] === 10: built-in shader nodes -- principled_bsdf/emission produce real, correct shader dicts ===\n");
    out = phi_mp_exec(
        "types = phi.node_types()\n"
        "print('principled_bsdf' in types and 'emission' in types and 'apply_material' in types)\n"
        "print(types['principled_bsdf']['category'])\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "True\nshader\n") != NULL, "all three built-in shader node types are really registered, with the real category");
    free(out);

    out = phi_mp_exec(
        "bsdf = principled_bsdf(base_color=(1.0, 0.0, 0.0), metallic=0.5, roughness=0.25)\n"
        "print(bsdf['base_color'])\n"
        "print(bsdf['metallic'])\n"
        "print(bsdf['roughness'])\n"
        "print(bsdf['emission'])\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "(1.0, 0.0, 0.0)\n0.5\n0.25\n(0.0, 0.0, 0.0)\n") != NULL, "principled_bsdf returns exactly the real base_color/metallic/roughness it was called with, and zero emission");
    free(out);

    out = phi_mp_exec(
        "e = emission(color=(2.0, 1.0, 0.5), strength=3.0)\n"
        "print(e['emission'])\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "(6.0, 3.0, 1.5)\n") != NULL, "emission really scales color by strength (2,1,0.5)*3 == (6,3,1.5), not a placeholder");
    free(out);

    printf("[mp_node_test] === 11: apply_material -- a shader graph's output really reaches real geometry ===\n");
    /* connect()'s own return value (True/False) is a bare top-level
     * expression here -- phi_mp_exec auto-prints any non-None bare
     * expression result (real REPL behavior, needed for the Console
     * panel), so it's assigned to a throwaway name to suppress that and
     * keep this block's captured output to exactly the one real print()
     * below. Every OTHER exec block in this file that uses a bare
     * .connect(...)/.evaluate() call got away without this because its
     * own check used strstr() (substring match), which the extra "True"
     * text doesn't break -- this one specifically needed the exact
     * printed value, which is what surfaced it. */
    out = phi_mp_exec(
        "shader_oid = phi.create_mesh([0,0,0, 1,0,0, 1,1,0], [0,1,2])\n"
        "g9 = phi.Graph(kind='geometry')\n"
        "n_bsdf = g9.add_node('principled_bsdf', base_color=(0.1, 0.2, 0.3), metallic=0.9, roughness=0.1)\n"
        "n_apply = g9.add_node('apply_material', object_id=float(shader_oid), face_index=0.0)\n"
        "_ = g9.connect(n_bsdf, 'shader', n_apply, 'shader')\n"
        "_ = g9.evaluate()\n"
        "print(shader_oid)\n"
    );
    printf("  captured: %s", out);
    int shader_oid = atoi(out);
    check(shader_oid > 0, "the graph evaluated without raising and the test object id round-tripped");
    free(out);
    MeshObject *shader_obj = scene_object_find(shader_oid);
    check(shader_obj && shader_obj->hem, "the shader test object really exists with real editable geometry");
    if (shader_obj && shader_obj->hem) {
        HEFace *f0 = &shader_obj->hem->faces[0];
        check(fabsf(f0->base_color[0] - 0.1f) < 1e-4f && fabsf(f0->base_color[1] - 0.2f) < 1e-4f && fabsf(f0->base_color[2] - 0.3f) < 1e-4f,
              "face 0's REAL C-side base_color matches exactly what the shader graph computed (0.1,0.2,0.3) -- apply_material really wrote it, not a no-op");
        check(fabsf(f0->metallic - 0.9f) < 1e-4f && fabsf(f0->roughness - 0.1f) < 1e-4f,
              "face 0's real metallic/roughness match the graph's output too (0.9/0.1)");
    }

    printf("[mp_node_test] === 12: built-in geometry nodes -- input_mesh + translate_mesh, real load + real move ===\n");
    out = phi_mp_exec(
        "ref_oid = phi.mesh_object('assets/cube.gltf', 0.0, 0.0, 0.0)\n"
        "ref_verts = phi.get_vertices(ref_oid)\n"
        "g10 = phi.Graph(kind='geometry')\n"
        "n_in = g10.add_node('input_mesh', asset='assets/cube.gltf')\n"
        "n_move = g10.add_node('translate_mesh', dx=5.0, dy=0.0, dz=0.0)\n"
        "g10.connect(n_in, 'object_id', n_move, 'object_id')\n"
        "result_oid = g10.evaluate()\n"
        "print(result_oid != ref_oid)\n"
        "moved_verts = phi.get_vertices(int(result_oid))\n"
        "print(abs(moved_verts[0] - (ref_verts[0] + 5.0)) < 1e-4)\n"
        "print(abs(moved_verts[1] - ref_verts[1]) < 1e-4)\n"
        "print(abs(moved_verts[2] - ref_verts[2]) < 1e-4)\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "True\nTrue\nTrue\nTrue\n") != NULL,
          "input_mesh loaded a REAL, DIFFERENT object from a real asset file, and translate_mesh really moved vertex 0 by exactly (+5,0,0) relative to an unmoved reference load of the same asset");
    free(out);

    printf("[mp_node_test] === 13: phi.set_face_material -- direct binding, real writes + real error cases ===\n");
    out = phi_mp_exec(
        "mat_oid = phi.create_mesh([0,0,0, 1,0,0, 0,1,0], [0,1,2])\n"
        "phi.set_face_material(mat_oid, 0, [0.9, 0.1, 0.1], 0.7, 0.2, [0.0, 5.0, 0.0])\n"
        "print(mat_oid)\n"
    );
    printf("  captured: %s", out);
    int mat_oid = atoi(out);
    free(out);
    MeshObject *mat_obj = scene_object_find(mat_oid);
    check(mat_obj && mat_obj->hem &&
          fabsf(mat_obj->hem->faces[0].base_color[0] - 0.9f) < 1e-4f &&
          fabsf(mat_obj->hem->faces[0].metallic - 0.7f) < 1e-4f &&
          fabsf(mat_obj->hem->faces[0].emission[1] - 5.0f) < 1e-4f,
          "phi.set_face_material wrote real base_color/metallic/emission onto the real HEFace struct, matching exactly what was passed");

    out = phi_mp_exec("phi.set_face_material(999999, 0, [0,0,0], 0, 0, [0,0,0])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "set_face_material on a nonexistent object id raises");
    free(out);

    out = phi_mp_exec("phi.set_face_material(mat_oid, 999, [0,0,0], 0, 0, [0,0,0])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "set_face_material on an out-of-range face index raises");
    free(out);

    out = phi_mp_exec("phi.set_face_material(mat_oid, 0, [0,0], 0, 0, [0,0,0])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "set_face_material with a malformed (2-element) base_color raises");
    free(out);

    printf("[mp_node_test] === 14: animation bindings are honest about being unregistered in this standalone harness ===\n");
    /* This test binary never calls phi_mp_register_animation_callbacks
     * (that's main.c's job against a real g_skinned_test_obj) -- proves
     * the "not available yet" fallback path really works, the same real
     * failure mode phi.render()/phi.activate_ragdoll() already have for
     * the identical reason (see their own native_* comments). */
    out = phi_mp_exec("phi.play_animation('walk')\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL && strstr(out, "not available yet") != NULL,
          "phi.play_animation raises a real, honest 'not available yet' error rather than crashing or silently no-op'ing when unregistered");
    free(out);

    out = phi_mp_exec("print(phi.list_animation_clips())\n");
    printf("  captured: %s", out);
    check(strstr(out, "()\n") != NULL, "phi.list_animation_clips() returns a real empty tuple (not an error) when unregistered -- a real, unsurprising outcome");
    free(out);

    out = phi_mp_exec("print(phi.get_animation_time())\n");
    printf("  captured: %s", out);
    check(strstr(out, "0.0\n") != NULL, "phi.get_animation_time() returns a real 0.0 (not an error) when unregistered");
    free(out);

    printf("\n[mp_node_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
