/* Standalone MicroPython + scene_objects.c integration test -- Phase 5's
 * actual load-bearing ask ("make it so Claude can create geometry and
 * redefine the verts of existing scene geometry via the Python API"),
 * exercised end to end against a REAL embedded interpreter driving REAL
 * scene_objects.c/halfedge.c calls, not a mock of either. Same "prove it
 * end to end, no GL/window needed" precedent as mp_prop_panel_test_
 * main.c/mp_physics_test_main.c, extended to cover geometry creation/
 * vertex editing specifically. */
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

/* Stub -- scene_objects.c's own scene_object_delete references mesh_
 * destroy (octree_render.c, real GL) regardless of whether this test's
 * own calls ever reach it at runtime -- same technique fracture_body_
 * test_main.c/scene_objects_test_main.c already established. Real
 * free(), not a no-op (this test creates real render meshes that need
 * real releasing). phi_physics_remove_body needs no stub here -- this
 * test links the real phi_physics.cpp/Bullet (same as mp_prop_panel_
 * test_main.c/mp_physics_test_main.c already do), since mp_port.c's
 * OTHER physics bindings (enable_physics/apply_impulse/etc.) need real
 * definitions regardless of whether this specific test calls them. */
void mesh_destroy(RenderMesh *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

int main(void) {
    int stack_top;
    printf("[mp_geometry_test] === setup ===\n");
    scene_objects_init();
    phi_mp_init(&stack_top);
    /* Deliberately NOT calling phi_mp_register_targets/scene_target_
     * register -- phi.create_mesh/set_vertices/set_vertex/mesh_object/
     * list_objects/delete_object all operate on explicit object ids (or
     * none), unlike phi.enable_physics/prop_get('object',...), which
     * need "the selected object" resolved through that registration.
     * Proves these new bindings genuinely don't depend on it. */

    printf("[mp_geometry_test] === 1: phi.create_mesh builds a real MeshObject from raw geometry ===\n");
    /* A flat quad -- 4 vertices, 2 triangles (0,1,2) and (0,2,3), the
     * simplest real non-degenerate topology halfedge_build_from_
     * triangles can turn into shared-vertex winged-edge structure. */
    char *out = phi_mp_exec(
        "positions = [0,0,0, 1,0,0, 1,1,0, 0,1,0]\n"
        "indices = [0,1,2, 0,2,3]\n"
        "oid = phi.create_mesh(positions, indices, 5.0, 6.0, 7.0)\n"
        "print(oid)\n"
    );
    printf("  captured: %s", out);
    int created_id = atoi(out);
    check(created_id > 0, "phi.create_mesh returned a real positive object id");
    free(out);

    MeshObject *obj = scene_object_find(created_id);
    check(obj != NULL, "the object genuinely exists in scene_objects.c's own registry, not just a Python-side fiction");
    if (obj) {
        check(obj->hem != NULL && obj->hem->vert_count == 4, "real half-edge geometry with exactly 4 vertices");
        check(fabsf(obj->position.x - 5.0f) < 1e-5f && fabsf(obj->position.y - 6.0f) < 1e-5f && fabsf(obj->position.z - 7.0f) < 1e-5f,
              "the x,y,z passed to create_mesh became the object's real C-side position");
        check(obj->render_mesh != NULL && obj->render_mesh->count == 6, "render mesh flattened to 2 real triangles (6 non-indexed verts)");
    }

    printf("[mp_geometry_test] === 2: phi.set_vertices redefines ALL verts of existing geometry ===\n");
    out = phi_mp_exec(
        "phi.set_vertices(oid, [10,0,0, 11,0,0, 11,1,0, 10,1,0])\n"
    );
    printf("  captured (expect empty -- no exception): %s", out);
    check(strlen(out) == 0, "set_vertices on a real object with the right vertex count raises nothing");
    free(out);
    check(obj && fabsf(obj->hem->verts[0].pos[0] - 10.0f) < 1e-5f,
          "the FIRST vertex's real C-side position actually moved to what Python just set");
    check(obj && fabsf(obj->hem->verts[3].pos[1] - 1.0f) < 1e-5f,
          "the FOURTH vertex's real C-side position moved too (the whole array was rewritten, not just one entry)");

    printf("[mp_geometry_test] === 3: phi.set_vertex redefines ONE vert ===\n");
    out = phi_mp_exec("phi.set_vertex(oid, 1, 99.0, 98.0, 97.0)\n");
    check(strlen(out) == 0, "set_vertex on a valid index raises nothing");
    free(out);
    check(obj && fabsf(obj->hem->verts[1].pos[0] - 99.0f) < 1e-5f && fabsf(obj->hem->verts[1].pos[2] - 97.0f) < 1e-5f,
          "vertex 1's real C-side position matches exactly what set_vertex just wrote");
    check(obj && fabsf(obj->hem->verts[0].pos[0] - 10.0f) < 1e-5f,
          "vertex 0 (untouched by set_vertex) still holds set_vertices' earlier value -- a single-vertex edit didn't clobber its neighbors");

    printf("[mp_geometry_test] === 4: phi.list_objects sees it ===\n");
    out = phi_mp_exec("print(oid in phi.list_objects())\n");
    printf("  captured: %s", out);
    check(strstr(out, "True") != NULL, "the created object's id is really in list_objects()'s tuple");
    free(out);

    printf("[mp_geometry_test] === 5: real error cases raise cleanly, don't crash or silently corrupt ===\n");
    /* [0,0,0, 1,0,0] is only 2 vertices (0 and 1) -- index 2 in the
     * triangle below is genuinely out of range for them, the same real
     * "index outside the position array" case the next check exercises
     * with a more obviously-out-of-range index; this one specifically
     * proves the check catches an off-by-one, not just wildly invalid
     * input. */
    out = phi_mp_exec("phi.create_mesh([0,0,0, 1,0,0], [0,1,2])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "create_mesh with an index one past the last real vertex raises (off-by-one caught, not just wildly-invalid indices)");
    free(out);

    out = phi_mp_exec("phi.create_mesh([0,0,0, 1,0], [0,1,2])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "create_mesh with a positions list whose length isn't a multiple of 3 raises");
    free(out);

    out = phi_mp_exec("phi.create_mesh([0,0,0, 1,0,0, 1,1,0], [0,1,5])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "create_mesh with an out-of-range index raises rather than reading past the position array");
    free(out);

    out = phi_mp_exec("phi.set_vertices(oid, [0,0,0])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "set_vertices with the WRONG vertex count (1 instead of 4) raises, doesn't silently truncate");
    free(out);

    out = phi_mp_exec("phi.set_vertex(oid, 99, 0,0,0)\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "set_vertex with an out-of-range vertex index raises");
    free(out);

    out = phi_mp_exec("phi.set_vertices(999999, [0,0,0])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "set_vertices on a nonexistent object id raises");
    free(out);

    printf("[mp_geometry_test] === 6: phi.mesh_object loads a real file as a NEW object ===\n");
    out = phi_mp_exec("oid2 = phi.mesh_object('assets/cube.gltf', 1.0, 2.0, 3.0)\nprint(oid2)\n");
    printf("  captured: %s", out);
    int loaded_id = atoi(out);
    check(loaded_id > 0 && loaded_id != created_id, "phi.mesh_object returned a real, DIFFERENT object id from the earlier create_mesh call (a real second object, not a replace)");
    free(out);
    MeshObject *obj2 = scene_object_find(loaded_id);
    check(obj2 != NULL && obj2->hem != NULL && obj2->hem->vert_count == 8, "the loaded cube.gltf really has 8 vertices in the new object's own hem");
    check(scene_object_find(created_id) != NULL, "the FIRST object (from create_mesh) still exists -- loading a second one didn't replace it (real multi-object support)");

    printf("[mp_geometry_test] === 7: phi.delete_object removes it for real ===\n");
    out = phi_mp_exec("phi.delete_object(oid)\n");
    check(strlen(out) == 0, "delete_object on a real id raises nothing");
    free(out);
    check(scene_object_find(created_id) == NULL, "the object genuinely no longer exists in scene_objects.c's registry");
    check(scene_object_find(loaded_id) != NULL, "the OTHER object (from mesh_object) is untouched by deleting the first one");

    out = phi_mp_exec("phi.delete_object(999999)\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "delete_object on a nonexistent id raises rather than silently no-op'ing");
    free(out);

    printf("\n[mp_geometry_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
