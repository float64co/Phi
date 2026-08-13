/* Standalone, GL-free test for the Phase 2 MeshObject integration this
 * pass actually added on top of the raw Bullet wrapper (already verified
 * in isolation by phi_physics_test_main.c): meshobject_local_aabb_half_
 * extents computing a real box shape from a real loaded mesh, and the
 * exact create-body-then-sync-transform sequence main.c's
 * CTX_ACTION_ENABLE_PHYSICS handler and main_loop's per-frame sync use. */
#include "phi_physics.h"
#include "meshobject.h"
#include "halfedge_gltf.h"
#include <stdio.h>
#include <math.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

int main(void) {
    printf("[phi_physics_meshobject_test] === 1: AABB half-extents from a real loaded mesh ===\n");
    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    check(hem != NULL, "loaded assets/cube.gltf");
    /* cube.gltf is a unit cube, half-extent 0.5 on every axis, same as
     * main.c's spawn_test_mesh_object scales up 16x -- do the same here
     * so this test exercises a realistic scene-scale object, not a tiny
     * 0.5-unit one that would barely interact with gravity meaningfully
     * over a handful of steps. */
    for (int i = 0; i < hem->vert_count; i++)
        for (int a = 0; a < 3; a++)
            hem->verts[i].pos[a] *= 16.0f;

    Vec3f half_extents;
    check(meshobject_local_aabb_half_extents(hem, &half_extents) == 1, "computed real AABB half-extents");
    printf("  half_extents = (%.3f, %.3f, %.3f) (expected ~8.0 on every axis)\n",
           half_extents.x, half_extents.y, half_extents.z);
    check(fabsf(half_extents.x - 8.0f) < 0.01f && fabsf(half_extents.y - 8.0f) < 0.01f && fabsf(half_extents.z - 8.0f) < 0.01f,
          "matches the known scaled-cube geometry exactly, not an approximation");

    check(meshobject_local_aabb_half_extents(NULL, &half_extents) == 0, "NULL hem is a clean failure, not a crash");

    printf("[phi_physics_meshobject_test] === 2: MeshObject.phys_body through the exact main.c create/sync sequence ===\n");
    MeshObject obj = {0};
    obj.id = 1;
    obj.position = (Vec3f){128.0f, 100.0f, 90.0f};
    obj.orientation = quat_identity();
    obj.scale = (Vec3f){1.0f, 1.0f, 1.0f};
    obj.hem = hem;
    check(obj.phys_body == NULL, "starts with no physics body (the common-case default)");

    PhiPhysicsWorld *world = phi_physics_world_create();
    float identity_quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    Vec3f ground_half = {200.0f, 10.0f, 200.0f};
    Vec3f ground_pos  = {128.0f, 40.0f, 90.0f};   /* top surface at y=50, matching main.c's own setup exactly */
    PhiRigidBody *ground = phi_physics_add_box_body(world, ground_half, ground_pos, identity_quat, 0.0f, 0.3f);
    check(ground != NULL, "ground created");

    /* Exactly main.c's CTX_ACTION_ENABLE_PHYSICS handler's own sequence. */
    float orientation[4] = { obj.orientation.x, obj.orientation.y, obj.orientation.z, obj.orientation.w };
    obj.phys_body = phi_physics_add_box_body(world, half_extents, obj.position, orientation, 1.0f, 0.3f);
    check(obj.phys_body != NULL, "MeshObject now owns a real physics body");

    printf("[phi_physics_meshobject_test] === 3: main_loop's per-frame sync actually moves obj.position ===\n");
    Vec3f start_pos = obj.position;
    for (int i = 0; i < 200; i++) {
        phi_physics_world_step(world, 1.0f / 60.0f);
        /* Exactly main_loop's own sync block. */
        float o[4];
        phi_physics_get_transform(obj.phys_body, &obj.position, o);
        obj.orientation.x = o[0]; obj.orientation.y = o[1]; obj.orientation.z = o[2]; obj.orientation.w = o[3];
    }
    printf("  start y=%.2f -> after 200 steps y=%.2f (expected to have fallen and landed near y=58.0 -- "
           "ground top 50 + this object's own 8.0 half-extent)\n", start_pos.y, obj.position.y);
    check(obj.position.y < start_pos.y - 10.0f, "obj.position.y actually decreased a lot -- the sync is really writing MeshObject's own field, not a disconnected copy");
    /* 0.2 tolerance, not 0.1 -- Bullet's default small collision margin
     * (~0.04 units) plus this object being 16x bigger than
     * phi_physics_test_main.c's own settling check means the settled
     * height isn't pixel-perfect, same real behavior, just a proportionally
     * looser (still meaningful) bound. */
    check(fabsf(obj.position.y - 58.0f) < 0.2f, "settled at the height the ground+half-extent geometry predicts");

    printf("[phi_physics_meshobject_test] === 4: cleanup ===\n");
    phi_physics_remove_body(world, obj.phys_body);
    phi_physics_remove_body(world, ground);
    phi_physics_world_destroy(world);
    halfedge_destroy(hem);
    check(1, "torn down without crashing");

    if (g_fail) printf("\n[phi_physics_meshobject_test] RESULT: FAIL\n");
    else printf("\n[phi_physics_meshobject_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
