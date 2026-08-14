/* Standalone, GL-free test harness for ragdoll.c -- Phase 4's Armature ->
 * Bullet ragdoll handoff (see ragdoll.h). No stub functions needed
 * (unlike fracture_body_test_main.c) -- neither ragdoll.c nor skinned_
 * mesh_object.c ever calls anything GL-touching. */
#include "ragdoll.h"
#include "skinned_mesh_object.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

static float v3dist(Vec3f a, Vec3f b) {
    float dx=a.x-b.x, dy=a.y-b.y, dz=a.z-b.z;
    return sqrtf(dx*dx+dy*dy+dz*dz);
}

/* Rotates v by orientation -- reimplemented locally (same per-file
 * convention every other test/module in this codebase uses for this
 * exact routine, see meshobject.c's own mat4_rotate_vec3). */
static Vec3f rotate_by_quat(Quat q, Vec3f v) {
    float m[16]; quat_to_mat4(&q, m);
    return (Vec3f){
        m[0]*v.x + m[4]*v.y + m[8]*v.z,
        m[1]*v.x + m[5]*v.y + m[9]*v.z,
        m[2]*v.x + m[6]*v.y + m[10]*v.z
    };
}

int main(void) {
    printf("[ragdoll_test] === setup ===\n");
    PhiPhysicsWorld *world = phi_physics_world_create();
    check(world != NULL, "physics world created");

    SkinnedMeshObject obj;
    int loaded = skinned_mesh_object_load("assets/test/armature_test.gltf", (Vec3f){0, 100.0f, 0}, &obj);
    check(loaded, "loaded assets/test/armature_test.gltf");
    check(obj.arm.bone_count == 3, "3 bones (root/mid/tip), matches animation_test's own fixture check");

    ragdoll_system_init();

    printf("[ragdoll_test] === 1: defensive -- NULL obj / empty armature ===\n");
    check(ragdoll_activate(world, NULL, 1.0f, 0.2f) == 0, "NULL obj spawns nothing rather than crashing");
    check(ragdoll_count() == 0, "count stays 0 after a failed activation");

    printf("[ragdoll_test] === 2: real activation -- one capsule body per bone ===\n");
    /* obj->world[] is seeded with the rest pose by skinned_mesh_object_
     * load itself -- no update() call needed before this activation. */
    int spawned = ragdoll_activate(world, &obj, 1.0f, 0.2f);
    check(spawned == 3, "spawned one body per bone (3)");
    check(ragdoll_count() == 3, "ragdoll_count matches what activate() returned");

    Vec3f pos[3]; Quat rot[3];
    for (int i = 0; i < 3; i++) {
        check(ragdoll_get_transform(i, &pos[i], &rot[i]), "ragdoll_get_transform reads a real body transform");
    }
    check(!ragdoll_get_transform(3, NULL, NULL), "an out-of-range index is rejected cleanly");
    printf("    capsule centers: root=(%.2f,%.2f,%.2f) mid=(%.2f,%.2f,%.2f) tip=(%.2f,%.2f,%.2f)\n",
           pos[0].x,pos[0].y,pos[0].z, pos[1].x,pos[1].y,pos[1].z, pos[2].x,pos[2].y,pos[2].z);
    /* Fixture's rest-pose bones are stacked straight up the local Y axis
     * (mid/tip rest_translation.y == 2.0 each, see animation_test_main.c's
     * own checks) -- spawned at obj.position.y=100, so every capsule
     * center should land somewhere above 100 and the three should be in
     * ascending Y order (root lowest, tip highest). */
    check(pos[0].y > 99.0f && pos[1].y > pos[0].y && pos[2].y > pos[1].y,
          "capsule centers are stacked in ascending Y order (root < mid < tip), matching the rig's own straight-up rest pose");

    printf("[ragdoll_test] === 3: joints stay glued under gravity (real point2point constraint) ===\n");
    /* Reconstructs each joint's world-space position from BOTH sides of
     * the constraint (parent body's own local pivot, and child body's
     * own local pivot, each rotated by that body's OWN live orientation)
     * -- these two reconstructions must stay coincident at every step if
     * the constraint is doing its real job, regardless of how far the
     * whole ragdoll has fallen or tumbled. This is the actual invariant
     * a point2point constraint guarantees; raw body-center-to-body-center
     * distance is NOT the same thing (capsules can rotate about a shared
     * pivot, changing their own center separation, without the joint
     * itself ever coming apart). */
    /* Independently re-derives each bone's TRUE world-space head position
     * (same "recompute the expected answer separately, don't trust the
     * module under test to hand it to you" philosophy fracture_test.c's
     * adjacency check already established) -- object_transform_point
     * mirrors ragdoll.c's own private helper of the same name exactly
     * (position + orientation + scale composed the same way renderer_
     * draw_skinned_mesh's model matrix is), reimplemented locally per
     * this codebase's per-file convention for small math helpers. */
    int root_i = armature_find_bone(&obj.arm, "root");
    int mid_i  = armature_find_bone(&obj.arm, "mid");
    int tip_i  = armature_find_bone(&obj.arm, "tip");
    check(root_i >= 0 && mid_i >= 0 && tip_i >= 0, "found root/mid/tip bones by name");
    /* ragdoll_activate spawns bone i into slot i directly (a straight,
     * non-skipping loop over every bone, see ragdoll.c) -- so bone index
     * and ragdoll body slot are the same number, safe to use root_i/
     * mid_i/tip_i directly as ragdoll_get_transform indices below too. */

    Vec3f true_head[3];
    {
        float rotm[16]; quat_to_mat4(&obj.orientation, rotm);
        int idx[3] = { root_i, mid_i, tip_i };
        for (int k = 0; k < 3; k++) {
            Vec3f local = { obj.world[idx[k]][12], obj.world[idx[k]][13], obj.world[idx[k]][14] };
            Vec3f sc = { local.x*obj.scale.x, local.y*obj.scale.y, local.z*obj.scale.z };
            Vec3f rotated = {
                rotm[0]*sc.x + rotm[4]*sc.y + rotm[8]*sc.z,
                rotm[1]*sc.x + rotm[5]*sc.y + rotm[9]*sc.z,
                rotm[2]*sc.x + rotm[6]*sc.y + rotm[10]*sc.z
            };
            true_head[k] = (Vec3f){ rotated.x+obj.position.x, rotated.y+obj.position.y, rotated.z+obj.position.z };
        }
    }

    Vec3f pivot_local_in_parent[2], pivot_local_in_child[2];
    int parent_slot[2] = { root_i, mid_i };
    int child_slot[2]  = { mid_i,  tip_i };
    Vec3f joint_at[2]  = { true_head[1] /* mid's own head */, true_head[2] /* tip's own head */ };
    for (int k = 0; k < 2; k++) {
        Vec3f joint_world = joint_at[k];
        pivot_local_in_parent[k] = rotate_by_quat((Quat){-rot[parent_slot[k]].x,-rot[parent_slot[k]].y,-rot[parent_slot[k]].z,rot[parent_slot[k]].w},
                                                   (Vec3f){joint_world.x-pos[parent_slot[k]].x, joint_world.y-pos[parent_slot[k]].y, joint_world.z-pos[parent_slot[k]].z});
        pivot_local_in_child[k]  = rotate_by_quat((Quat){-rot[child_slot[k]].x,-rot[child_slot[k]].y,-rot[child_slot[k]].z,rot[child_slot[k]].w},
                                                   (Vec3f){joint_world.x-pos[child_slot[k]].x, joint_world.y-pos[child_slot[k]].y, joint_world.z-pos[child_slot[k]].z});
    }

    for (int step = 0; step < 90; step++) phi_physics_world_step(world, 1.0f/60.0f);

    Vec3f pos2[3]; Quat rot2[3];
    for (int i = 0; i < 3; i++) ragdoll_get_transform(i, &pos2[i], &rot2[i]);
    printf("    after 1.5s of freefall: root=(%.2f,%.2f,%.2f) mid=(%.2f,%.2f,%.2f) tip=(%.2f,%.2f,%.2f)\n",
           pos2[0].x,pos2[0].y,pos2[0].z, pos2[1].x,pos2[1].y,pos2[1].z, pos2[2].x,pos2[2].y,pos2[2].z);
    check(pos2[root_i].y < pos[root_i].y - 1.0f, "the ragdoll actually fell under gravity (root is meaningfully lower than at spawn)");

    float max_joint_gap = 0.0f;
    for (int k = 0; k < 2; k++) {
        Vec3f joint_from_parent = (Vec3f){
            pos2[parent_slot[k]].x + rotate_by_quat(rot2[parent_slot[k]], pivot_local_in_parent[k]).x,
            pos2[parent_slot[k]].y + rotate_by_quat(rot2[parent_slot[k]], pivot_local_in_parent[k]).y,
            pos2[parent_slot[k]].z + rotate_by_quat(rot2[parent_slot[k]], pivot_local_in_parent[k]).z
        };
        Vec3f joint_from_child = (Vec3f){
            pos2[child_slot[k]].x + rotate_by_quat(rot2[child_slot[k]], pivot_local_in_child[k]).x,
            pos2[child_slot[k]].y + rotate_by_quat(rot2[child_slot[k]], pivot_local_in_child[k]).y,
            pos2[child_slot[k]].z + rotate_by_quat(rot2[child_slot[k]], pivot_local_in_child[k]).z
        };
        float gap = v3dist(joint_from_parent, joint_from_child);
        printf("    joint %d gap after freefall: %.4f\n", k, gap);
        if (gap > max_joint_gap) max_joint_gap = gap;
    }
    check(max_joint_gap < 0.5f, "every joint stays glued (reconstructed pivot from both sides matches closely) after 1.5s of tumbling freefall");

    printf("[ragdoll_test] === 4: cleanup -- clear empties the registry, no crash ===\n");
    ragdoll_clear(world);
    check(ragdoll_count() == 0, "clear() really empties the registry");
    ragdoll_clear(world);
    check(1, "clearing an already-empty registry is a safe no-op (this line running proves it)");

    printf("[ragdoll_test] === 5: teardown ===\n");
    skinned_mesh_object_free(&obj);
    phi_physics_world_destroy(world);
    check(1, "world + object torn down without crashing (this line running proves it)");

    printf("\n[ragdoll_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
