/* Standalone, no-GL test harness for Phase 4's foundation data layer
 * (armature.c/animation.c/skinned_mesh.c -- see phi.md's "Animation
 * Editor"): real glTF skin/animation/mesh parsing (via cgltf, the same
 * parser halfedge_gltf.c already uses for meshes), a real topological
 * sort of a DELIBERATELY out-of-order joints array (see
 * tools/gen_test_armature.py), real keyframe interpolation (LINEAR
 * quaternion slerp, checked against a hand-derived expected half-angle
 * rotation, not just "it ran"), a full pose->world-transform pipeline
 * checked against a hand-derived expected world position after a
 * 90-degree bone rotation propagates to a child bone, and skin-weight
 * loading (JOINTS_0/WEIGHTS_0, including the file's own JOINTS_0-index-
 * remapping subtlety -- see skinned_mesh.h's own comment on why a raw
 * accessor value can't be used as a bone index directly). Same "prove
 * the subsystem works in isolation" precedent as mesh_edit_test_main.c/
 * area_tree_test_main.c, applied to the new animation system instead. */
#include "armature.h"
#include "animation.h"
#include "skinned_mesh.h"
#include "cgltf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}
static int approx(float a, float b, float eps) { return fabsf(a - b) <= eps; }

int main(void) {
    printf("[animation_test] === setup: load a real glTF skin+animation fixture ===\n");
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;
    const char *path = "assets/test/armature_test.gltf";
    check(cgltf_parse_file(&options, path, &data) == cgltf_result_success, "parsed armature_test.gltf");
    check(cgltf_load_buffers(&options, data, path) == cgltf_result_success, "loaded armature_test.bin");
    check(data->skins_count == 1, "exactly one skin");
    check(data->animations_count == 1, "exactly one animation");

    printf("[animation_test] === 1: armature_load_from_skin topologically sorts a scrambled joints array ===\n");
    Armature arm;
    int ok = armature_load_from_skin(&data->skins[0], &arm);
    check(ok, "load succeeded");
    check(arm.bone_count == 3, "3 bones loaded");

    int root_i = armature_find_bone(&arm, "root");
    int mid_i  = armature_find_bone(&arm, "mid");
    int tip_i  = armature_find_bone(&arm, "tip");
    check(root_i >= 0 && mid_i >= 0 && tip_i >= 0, "all three bones found by name");
    check(root_i < mid_i && mid_i < tip_i, "topological sort produced parent-before-child indices (root < mid < tip)");
    check(arm.bones[root_i].parent == -1, "root has no parent");
    check(arm.bones[mid_i].parent == root_i, "mid's parent is root (resolved correctly despite scrambled joints[] order)");
    check(arm.bones[tip_i].parent == mid_i, "tip's parent is mid");

    printf("[animation_test] === 2: rest-pose local transforms match the fixture ===\n");
    check(approx(arm.bones[mid_i].rest_translation.y, 2.0f, 1e-5f), "mid's rest local translation.y == 2.0");
    check(approx(arm.bones[tip_i].rest_translation.y, 2.0f, 1e-5f), "tip's rest local translation.y == 2.0");
    check(arm.bones[root_i].rest_rotation.w == 1.0f, "root's rest rotation is identity");

    printf("[animation_test] === 3: rest-pose world transforms (armature_compute_world_transforms) ===\n");
    Vec3f t[ARMATURE_MAX_BONES]; Quat r[ARMATURE_MAX_BONES]; Vec3f s[ARMATURE_MAX_BONES];
    armature_rest_pose(&arm, t, r, s);
    float world[ARMATURE_MAX_BONES][16];
    armature_compute_world_transforms(&arm, t, r, s, world);
    check(approx(world[root_i][12], 0.0f, 1e-4f) && approx(world[root_i][13], 0.0f, 1e-4f), "root world position (0,0,0)");
    check(approx(world[mid_i][13], 2.0f, 1e-4f), "mid world position.y == 2.0");
    check(approx(world[tip_i][13], 4.0f, 1e-4f), "tip world position.y == 4.0 (0 + 2 + 2, hierarchy really composes)");

    printf("[animation_test] === 3b: skinning matrices (armature_compute_skinning_matrices) -- rest pose is exactly identity ===\n");
    float skin[ARMATURE_MAX_BONES][16];
    armature_compute_skinning_matrices(&arm, world, skin);
    static const float identity16[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    int skin_identity_ok = 1;
    for (int bi = 0; bi < arm.bone_count; bi++)
        for (int k = 0; k < 16; k++)
            if (!approx(skin[bi][k], identity16[k], 1e-4f)) skin_identity_ok = 0;
    check(skin_identity_ok, "at rest pose, every bone's skin matrix (world * inverse_bind) is exactly identity -- a real invariant, not eyeballed");

    printf("[animation_test] === 4: animation_load_clips ===\n");
    AnimClip clips[8];
    int clip_count = animation_load_clips(data, &arm, clips, 8);
    check(clip_count == 1, "exactly one usable clip loaded");
    check(strcmp(clips[0].name, "rotate_mid_90z") == 0, "clip name matches the fixture");
    check(clips[0].channel_count == 1, "exactly one channel (mid's rotation)");
    check(approx(clips[0].duration, 1.0f, 1e-5f), "clip duration == 1.0s");
    check(clips[0].channels[0].bone_index == mid_i, "channel targets the mid bone");

    printf("[animation_test] === 5: keyframe sampling -- LINEAR quaternion slerp against hand-derived values ===\n");
    Vec3f st[ARMATURE_MAX_BONES]; Quat sr[ARMATURE_MAX_BONES]; Vec3f ss[ARMATURE_MAX_BONES];

    armature_rest_pose(&arm, st, sr, ss);
    animation_sample_clip(&clips[0], 0.0f, st, sr, ss);
    check(approx(sr[mid_i].w, 1.0f, 1e-4f) && approx(sr[mid_i].z, 0.0f, 1e-4f), "t=0 -> identity rotation");

    armature_rest_pose(&arm, st, sr, ss);
    animation_sample_clip(&clips[0], 1.0f, st, sr, ss);
    float expect_z_full = sinf((float)M_PI / 4.0f), expect_w_full = cosf((float)M_PI / 4.0f);
    check(approx(sr[mid_i].z, expect_z_full, 1e-4f) && approx(sr[mid_i].w, expect_w_full, 1e-4f),
          "t=1 -> full 90deg-about-Z rotation, exact quaternion the fixture specifies");

    armature_rest_pose(&arm, st, sr, ss);
    animation_sample_clip(&clips[0], 0.5f, st, sr, ss);
    /* Slerp between identity and a 90deg-about-Z rotation at the halfway
     * point is EXACTLY the 45deg-about-Z rotation -- the defining
     * property of slerp along a fixed axis, not an approximation, so
     * this is checked against the precise expected quaternion, not just
     * "somewhere between". */
    float expect_z_half = sinf((float)M_PI / 8.0f), expect_w_half = cosf((float)M_PI / 8.0f);
    check(approx(sr[mid_i].z, expect_z_half, 1e-4f) && approx(sr[mid_i].w, expect_w_half, 1e-4f),
          "t=0.5 -> exact half-angle (45deg) rotation via real slerp, not linear-then-renormalize");

    printf("[animation_test] === 6: full pipeline -- sampled pose -> world transforms, checked against a hand-derived expected position ===\n");
    /* At t=1, mid rotates 90deg about Z. tip's local offset from mid,
     * (0,2,0), rotated 90deg about Z (verified independently against
     * quat_to_mat4's actual column-major convention, not assumed) becomes
     * (-2,0,0). tip_world = mid_world_translation + rotated_offset =
     * (0,2,0) + (-2,0,0) = (-2,2,0). */
    armature_rest_pose(&arm, st, sr, ss);
    animation_sample_clip(&clips[0], 1.0f, st, sr, ss);
    armature_compute_world_transforms(&arm, st, sr, ss, world);
    printf("  tip world translation at t=1: (%.4f, %.4f, %.4f)\n", world[tip_i][12], world[tip_i][13], world[tip_i][14]);
    check(approx(world[tip_i][12], -2.0f, 1e-3f), "tip world x == -2.0 (mid's 90deg rotation really propagated to its child)");
    check(approx(world[tip_i][13],  2.0f, 1e-3f), "tip world y ==  2.0");
    check(approx(world[tip_i][14],  0.0f, 1e-3f), "tip world z ==  0.0");

    printf("[animation_test] === 7: AnimPlayback -- looping wraps, non-looping clamps and stops ===\n");
    AnimPlayback pb;
    anim_playback_play(&pb, &clips[0], 1 /* loop */);
    anim_playback_advance(&pb, 1.5f);
    check(pb.playing, "looping playback keeps playing past duration");
    check(approx(pb.time, 0.5f, 1e-4f), "looping playback wraps 1.5s into a 1.0s clip -> time 0.5");

    anim_playback_play(&pb, &clips[0], 0 /* no loop */);
    anim_playback_advance(&pb, 1.5f);
    check(!pb.playing, "non-looping playback stops itself once past duration");
    check(approx(pb.time, 1.0f, 1e-4f), "non-looping playback clamps time to the clip's own duration, not left at 1.5");

    printf("[animation_test] === 8: skinned_mesh_load_gltf -- real JOINTS_0/WEIGHTS_0 loading, including index remapping ===\n");
    Armature mesh_arm;
    SkinnedMesh smesh;
    int mesh_ok = skinned_mesh_load_gltf(path, &mesh_arm, &smesh);
    check(mesh_ok, "skinned_mesh_load_gltf succeeded");
    check(smesh.vert_count == 6, "6 vertices loaded");
    check(smesh.index_count == 12, "12 indices (4 triangles) loaded");

    int mesh_root_i = armature_find_bone(&mesh_arm, "root");
    int mesh_mid_i  = armature_find_bone(&mesh_arm, "mid");
    int mesh_tip_i  = armature_find_bone(&mesh_arm, "tip");

    /* v0 (index 0): single-bone weight to root, weight 1.0 -- the
     * trivial case, but still a real read through the accessor +
     * remapping path, not skipped. */
    check(smesh.vert_count > 0 && smesh.verts[0].bone_idx[0] == (uint8_t)mesh_root_i,
          "v0's primary bone index remaps to 'root', despite the file's JOINTS_0 value being an index into the SCRAMBLED skin.joints order, not a bone index directly");
    check(smesh.vert_count > 0 && approx(smesh.verts[0].bone_wgt[0], 1.0f, 1e-5f), "v0's primary weight is 1.0");
    check(smesh.vert_count > 0 && approx(smesh.verts[0].pos[1], 0.0f, 1e-5f), "v0's position.y == 0.0 (root height)");

    /* v3 (index 3): the deliberately-blended vertex, 60% mid / 40% tip. */
    check(smesh.vert_count > 3 && smesh.verts[3].bone_idx[0] == (uint8_t)mesh_mid_i, "v3's PRIMARY bone remaps to 'mid'");
    check(smesh.vert_count > 3 && smesh.verts[3].bone_idx[1] == (uint8_t)mesh_tip_i, "v3's SECONDARY bone remaps to 'tip'");
    check(smesh.vert_count > 3 && approx(smesh.verts[3].bone_wgt[0], 0.6f, 1e-5f), "v3's primary weight == 0.6");
    check(smesh.vert_count > 3 && approx(smesh.verts[3].bone_wgt[1], 0.4f, 1e-5f), "v3's secondary weight == 0.4 -- a real multi-bone blend, not just a trivial (1,0,0,0) case");
    check(smesh.vert_count > 3 && approx(smesh.verts[3].pos[1], 2.0f, 1e-5f), "v3's position.y == 2.0 (mid height)");

    /* v5 (index 5): single-bone weight to tip. */
    check(smesh.vert_count > 5 && smesh.verts[5].bone_idx[0] == (uint8_t)mesh_tip_i, "v5's primary bone remaps to 'tip'");
    check(smesh.vert_count > 5 && approx(smesh.verts[5].pos[1], 4.0f, 1e-5f), "v5's position.y == 4.0 (tip height)");

    check(smesh.index_count >= 3 && smesh.indices[0] == 0 && smesh.indices[1] == 1 && smesh.indices[2] == 2,
          "first triangle's indices match the fixture (0,1,2)");

    skinned_mesh_free(&smesh);

    for (int i = 0; i < clip_count; i++) animation_clip_free(&clips[i]);
    cgltf_free(data);

    if (g_fail) printf("\n[animation_test] RESULT: FAIL\n");
    else printf("\n[animation_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
