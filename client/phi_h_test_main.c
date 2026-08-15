/* Standalone smoke test for phi.h itself -- proves the AGGREGATING
 * header (see its own comment) is genuinely self-contained and
 * compilable from a single #include with no hidden ordering/missing-
 * declaration dependency, the exact position a real game/src/main.c is
 * in (see phi.md's Phase 9). This is NOT a re-test of the underlying
 * subsystems (phi_physics.h/scene_objects.h/halfedge.h/mesh_edit.h each
 * already have their own real test coverage elsewhere -- phi_physics_
 * test_main.c, scene_objects_test_main.c, mesh_edit_test_main.c) -- it
 * only proves one real call from EACH area works when reached purely
 * through phi.h, nothing else included. */
#include "phi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

/* Same real-free-not-a-no-op stub every other standalone test needing
 * scene_object_delete uses -- see mp_geometry_test_main.c's own comment. */
void mesh_destroy(RenderMesh *m) {
    if (!m) return;
    free(m->data);
    free(m);
}

/* Real render_hooks.h callback -- writes through userdata (an int*) so
 * the test can prove invocation really happened, same technique
 * render_hooks_test_main.c's own hook_records_userdata already uses. */
static void phi_h_test_hook(GBuffer *gbuf, void *userdata) {
    (void)gbuf;
    *(int *)userdata = 1;
}

int main(void) {
    printf("[phi_h_test] === geometry, reached purely through phi.h ===\n");
    scene_objects_init();

    float positions[] = {0,0,0, 1,0,0, 1,1,0, 0,1,0};
    unsigned short indices[] = {0,1,2, 0,2,3};
    HalfEdgeMesh *hem = halfedge_build_from_triangles(positions, 4, indices, 6);
    check(hem != NULL && hem->vert_count == 4, "halfedge_build_from_triangles (halfedge.h, via phi.h) built a real 4-vertex mesh");

    MeshObject *obj = scene_object_add();
    check(obj != NULL, "scene_object_add (scene_objects.h, via phi.h) returned a real object slot");
    obj->position = (Vec3f){0, 0, 0};
    obj->orientation = quat_identity();
    obj->scale = (Vec3f){1, 1, 1};
    obj->is_static = 1;
    obj->hem = hem;
    obj->render_mesh = (RenderMesh *)calloc(1, sizeof(RenderMesh));
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, hem);
    check(obj->render_mesh->count == 6, "meshobject_build_render_mesh_from_halfedge (meshobject.h, via phi.h) flattened 2 real triangles");

    /* Face 0 is a TRIANGLE (verts 0,1,2 -- the quad was built from 2
     * triangles, see the indices above), so extruding it adds 3 new cap
     * vertices, not 4 -- 4 (original) + 3 (cap) == 7. */
    int cap = mesh_edit_extrude_face(hem, 0, 1.0f);
    check(cap >= 0 && hem->vert_count == 7, "mesh_edit_extrude_face (mesh_edit.h, via phi.h) really extruded -- 3 new cap vertices on the triangular face");

    printf("[phi_h_test] === physics, reached purely through phi.h ===\n");
    PhiPhysicsWorld *world = phi_physics_world_create();
    check(world != NULL, "phi_physics_world_create (phi_physics.h, via phi.h) returned a real world");

    float identity_quat[4] = {0, 0, 0, 1};
    PhiRigidBody *body = phi_physics_add_box_body(world, (Vec3f){1, 1, 1}, (Vec3f){0, 10, 0}, identity_quat, 1.0f, 0.3f);
    check(body != NULL, "phi_physics_add_box_body created a real dynamic body");

    phi_physics_apply_impulse(body, (Vec3f){0, 0, 5.0f}, (Vec3f){0, 0, 0});
    Vec3f v = phi_physics_get_linear_velocity(body);
    check(fabsf(v.z - 5.0f) < 1e-4f, "phi_physics_apply_impulse really changed the body's real linear velocity (unit mass, impulse 5 -> v.z==5)");

    /* A single 1/60s step's real displacement is small enough to be a
     * risky exact-equality/precision check on its own -- phi_physics_
     * test_main.c's own existing checks never assert after just one step
     * either (always a loop first, e.g. 30-60 steps), same convention
     * followed here rather than a fresh, untested assumption. */
    for (int i = 0; i < 30; i++) phi_physics_world_step(world, 1.0f / 60.0f);
    Vec3f pos_after;
    phi_physics_get_transform(body, &pos_after, NULL);
    check(pos_after.y < 10.0f, "phi_physics_world_step really advanced the simulation -- gravity pulled the body down from y=10 over 30 real steps");
    check(pos_after.z > 0.0f, "the earlier Z impulse's real velocity carried the body forward in Z across those same real steps");

    phi_physics_remove_body(world, body);
    phi_physics_world_destroy(world);

    int obj_id = obj->id;   /* captured before delete -- obj must not be used again afterward, see scene_object_delete's own contract */
    scene_object_delete(obj, NULL);
    check(scene_object_find(obj_id) == NULL, "scene_object_delete really removed the object");

    printf("[phi_h_test] === animation, reached purely through phi.h ===\n");
    SkinnedMeshObject skinned = {0};
    int loaded = skinned_mesh_object_load("assets/test/armature_test.gltf", (Vec3f){0, 0, 0}, &skinned);
    check(loaded, "skinned_mesh_object_load (skinned_mesh_object.h, via phi.h) loaded a real skinned test asset");
    if (loaded) {
        check(skinned.arm.bone_count > 0, "the loaded Armature (armature.h, via phi.h) has real bones");
        check(skinned.clip_count > 0, "the loaded asset has at least one real AnimClip (animation.h, via phi.h)");
        anim_playback_play(&skinned.playback, &skinned.clips[0], 1);
        float t0 = skinned.playback.time;
        skinned_mesh_object_update(&skinned, 0.5f);
        check(skinned.playback.time > t0, "skinned_mesh_object_update really advanced real playback time (CPU pose->world->skin pipeline ran)");
        skinned_mesh_object_free(&skinned);
    }

    printf("[phi_h_test] === node graphs, reached purely through phi.h ===\n");
    phi_graph_system_init();
    int gid = phi_graph_create(PHI_GRAPH_KIND_GEOMETRY);
    check(gid > 0, "phi_graph_create (node_graph.h, via phi.h) returned a real graph id");
    int na = phi_graph_add_node(gid, "a");
    int nb = phi_graph_add_node(gid, "b");
    check(phi_graph_connect(gid, na, "out", nb, "in"), "phi_graph_connect really linked two real nodes");
    int order[PHI_GRAPH_MAX_NODES];
    PhiGraph *g = phi_graph_find(gid);
    int n = phi_graph_topological_order(g, order);
    check(n == 2 && order[0] == na && order[1] == nb, "phi_graph_topological_order returned the real, correct dependency order (a before b)");

    printf("[phi_h_test] === render hooks, reached purely through phi.h ===\n");
    render_hooks_init();
    int hook_fired = 0;
    check(render_hooks_register(PHI_HOOK_AFTER_GBUFFER, phi_h_test_hook, &hook_fired), "render_hooks_register (render_hooks.h, via phi.h) accepted a real callback");
    GBuffer dummy_gbuf;
    memset(&dummy_gbuf, 0, sizeof(dummy_gbuf));
    render_hooks_invoke(PHI_HOOK_AFTER_GBUFFER, &dummy_gbuf);
    check(hook_fired == 1, "render_hooks_invoke really called the registered callback");

    printf("\n[phi_h_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
