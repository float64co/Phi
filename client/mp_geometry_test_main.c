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

    printf("[mp_geometry_test] === 8: phi.get_vertices/get_faces read back real geometry ===\n");
    out = phi_mp_exec(
        "verts = phi.get_vertices(oid2)\n"
        "faces = phi.get_faces(oid2)\n"
        "print(len(verts))\n"
        "print(len(faces))\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "24\n12\n") != NULL, "get_vertices returns 3 floats/vertex (8*3=24), get_faces returns one entry per live face (the cube's 12 triangles)");
    free(out);

    out = phi_mp_exec(
        "print(len(faces[0]))\n"
        "print(len(faces[0][1]))\n"
        "print(all(0 <= i < len(verts)//3 for pair in faces for i in pair[1]))\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "2\n3\nTrue\n") != NULL, "each get_faces entry is (face_index, 3-vertex-tuple), and every vertex index is genuinely valid against get_vertices' own count");
    free(out);

    out = phi_mp_exec("phi.get_vertices(999999)\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "get_vertices on a nonexistent object id raises");
    free(out);

    printf("[mp_geometry_test] === 9: phi.add_vertex/add_face grow EXISTING topology (not a replace) ===\n");
    out = phi_mp_exec(
        "positions3 = [0,0,0, 1,0,0, 1,1,0, 0,1,0]\n"
        "indices3 = [0,1,2, 0,2,3]\n"
        "oid3 = phi.create_mesh(positions3, indices3)\n"
        "new_v = phi.add_vertex(oid3, 0.5, 0.5, 1.0)\n"
        "print(new_v)\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "4\n") != NULL, "add_vertex on the fresh 4-vertex quad returns the next real index (4), appended not replacing");
    free(out);

    out = phi_mp_exec(
        "new_f = phi.add_face(oid3, [2, 3, new_v])\n"
        "print(new_f)\n"
        "print(len(phi.get_faces(oid3)))\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "2\n3\n") != NULL, "add_face returns the next real face index (2), and the object genuinely now has 3 live faces");
    free(out);

    out = phi_mp_exec("print(oid3)\n");
    MeshObject *obj3 = scene_object_find(atoi(out));
    free(out);
    check(obj3 != NULL && obj3->hem->vert_count == 5, "the object's real C-side vert_count actually grew to 5");
    check(obj3->render_mesh != NULL && obj3->render_mesh->count == 9, "render mesh rebuilt to 3 live triangles (9 non-indexed verts)");

    out = phi_mp_exec("phi.add_face(oid3, [0, 1])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "add_face with only 2 indices raises (triangles-only)");
    free(out);

    out = phi_mp_exec("phi.add_face(oid3, [0, 1, 999])\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "add_face with an out-of-range vertex index raises rather than corrupting memory past hem->verts");
    free(out);

    out = phi_mp_exec("phi.add_vertex(999999, 0,0,0)\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "add_vertex on a nonexistent object id raises");
    free(out);

    printf("[mp_geometry_test] === 10: phi.delete_face removes a real face ===\n");
    out = phi_mp_exec(
        "phi.delete_face(oid3, new_f)\n"
        "print(len(phi.get_faces(oid3)))\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "2\n") != NULL, "after deleting the just-added 3rd face, get_faces is back down to the original 2 live faces");
    free(out);
    check(obj3->render_mesh->count == 6, "render mesh rebuilt back down to 2 triangles (6 non-indexed verts)");

    out = phi_mp_exec("phi.delete_face(oid3, new_f)\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "deleting an already-deleted (tombstoned) face raises rather than double-tombstoning silently");
    free(out);

    out = phi_mp_exec("phi.delete_face(oid3, 999)\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "delete_face with an out-of-range face index raises");
    free(out);

    printf("[mp_geometry_test] === 11: phi.flip_normals reverses winding across a whole object ===\n");
    out = phi_mp_exec(
        "oid4 = phi.create_mesh(positions3, indices3)\n"
        "before = phi.get_faces(oid4)\n"
        "n_flipped = phi.flip_normals(oid4)\n"
        "after = phi.get_faces(oid4)\n"
        "print(n_flipped)\n"
        "print(len(after))\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "2\n2\n") != NULL, "flip_normals reports flipping both live faces, and the live face count is unchanged (2 before, 2 after)");
    free(out);

    out = phi_mp_exec(
        "def rev(t): return tuple(reversed(t))\n"
        "before_reversed = set(rev(pair[1]) for pair in before)\n"
        "after_set = set(pair[1] for pair in after)\n"
        "print(before_reversed == after_set)\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "True\n") != NULL, "every face's reversed winding from before flip_normals is EXACTLY the set of windings after -- a real, complete flip, not a no-op or partial one");
    free(out);

    out = phi_mp_exec(
        "phi.flip_normals(oid4)\n"
        "after2 = phi.get_faces(oid4)\n"
        "before_set = set(pair[1] for pair in before)\n"
        "after2_set = set(pair[1] for pair in after2)\n"
        "print(before_set == after2_set)\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "True\n") != NULL, "flipping twice returns winding to exactly the original set -- flip_normals is its own inverse");
    free(out);

    printf("[mp_geometry_test] === 12: phi.extrude_face is a real binding onto mesh_edit.c's already-proven operation ===\n");
    out = phi_mp_exec(
        "oid5 = phi.create_mesh([0,0,0, 1,0,0, 0,1,0], [0,1,2])\n"
        "cap = phi.extrude_face(oid5, 0, 0.5)\n"
        "print(cap)\n"
        "print(len(phi.get_faces(oid5)))\n"
        "print(len(phi.get_vertices(oid5)) // 3)\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "1\n7\n6\n") != NULL, "extrude_face on a lone triangle: new cap face index 1, 7 live faces after (cap + 3 side-wall quads x2 tris, original tombstoned), 6 vertices after (3 original + 3 new cap corners)");
    free(out);

    out = phi_mp_exec("phi.extrude_face(oid5, 999, 1.0)\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "extrude_face on an out-of-range face raises");
    free(out);

    printf("[mp_geometry_test] === 13: phi.inset_face is a real binding onto mesh_edit.c's already-proven operation ===\n");
    out = phi_mp_exec(
        "oid6 = phi.create_mesh([0,0,0, 1,0,0, 0,1,0], [0,1,2])\n"
        "inset = phi.inset_face(oid6, 0, 0.3)\n"
        "print(inset)\n"
        "print(len(phi.get_faces(oid6)))\n"
        "print(len(phi.get_vertices(oid6)) // 3)\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "1\n7\n6\n") != NULL, "inset_face on a lone triangle: same structural shape as extrude (new cap + 6 wall triangles, +3 new vertices)");
    free(out);

    printf("[mp_geometry_test] === 14: phi.nearest_edge_of_face + phi.loop_cut ===\n");
    out = phi_mp_exec(
        "oid7 = phi.create_mesh([0,0,0, 1,0,0, 0,1,0], [0,1,2])\n"
        /* Edge (v0=(0,0,0), v1=(1,0,0))'s real midpoint -- resolving it
         * back to a real edge index is exactly what a script needs before
         * it can call loop_cut at all (loop_cut has no other way to name
         * an edge). */
        "edge = phi.nearest_edge_of_face(oid7, 0, 0.5, 0.0, 0.0)\n"
        "print(edge)\n"
        "mv = phi.loop_cut(oid7, edge)\n"
        "print(mv)\n"
        "print(len(phi.get_vertices(oid7)) // 3)\n"
        "print(len(phi.get_faces(oid7)))\n"
    );
    printf("  captured: %s", out);
    check(strstr(out, "\n3\n") != NULL, "loop_cut returns the new midpoint vertex's real index (3 -- the 4th vertex on a 3-vertex triangle)");
    check(strstr(out, "3\n4\n2\n") != NULL, "loop_cut on an isolated (twin-less) triangle: midpoint vertex 3, 4 total vertices after, 2 live faces after (original tombstoned, 2 new triangles fan from the opposite corner)");
    free(out);

    out = phi_mp_exec("phi.nearest_edge_of_face(oid7, 999, 0,0,0)\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "nearest_edge_of_face on an out-of-range face raises");
    free(out);

    out = phi_mp_exec("phi.loop_cut(oid7, 999999)\n");
    printf("  captured: %s", out);
    check(strstr(out, "ValueError") != NULL, "loop_cut with an out-of-range edge index raises");
    free(out);

    printf("\n[mp_geometry_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
