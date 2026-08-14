/* Standalone, GL-free test harness for skinned_mesh_object.c -- Phase 4's
 * pose->world->skin-matrix CPU pipeline that sits between the already-
 * proven armature.c/animation.c/skinned_mesh.c data layer (see animation_
 * test_main.c) and the new GPU skinning draw path (renderer.c, not
 * testable here -- no GL). No stub functions needed, same as ragdoll_
 * test_main.c. */
#include "skinned_mesh_object.h"
#include <stdio.h>
#include <math.h>

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}
static int is_identity16(const float m[16], float eps) {
    for (int i = 0; i < 16; i++) {
        float expect = (i % 5 == 0) ? 1.0f : 0.0f;   /* diagonal entries at 0,5,10,15 */
        if (fabsf(m[i] - expect) > eps) return 0;
    }
    return 1;
}

int main(void) {
    printf("[skinned_mesh_object_test] === setup ===\n");
    SkinnedMeshObject obj;
    int ok = skinned_mesh_object_load("assets/test/armature_test.gltf", (Vec3f){0,0,0}, &obj);
    check(ok, "loaded assets/test/armature_test.gltf");
    check(obj.mesh.vert_count > 0 && obj.mesh.index_count > 0, "real skinned mesh geometry loaded");
    check(obj.arm.bone_count == 3, "3 bones");
    check(obj.clip_count >= 1, "at least one animation clip loaded");
    check(obj.playback.clip == &obj.clips[0] && obj.playback.loop && obj.playback.playing,
          "first clip auto-starts playing, looped");

    printf("[skinned_mesh_object_test] === 1: rest-pose skin matrices are identity ===\n");
    /* obj is seeded with rest pose at load time, before any update() call
     * -- armature.h's own documented invariant (see armature_compute_
     * skinning_matrices' header comment): at rest, every skin matrix is
     * exactly identity by construction. */
    int all_identity = 1;
    for (int i = 0; i < obj.arm.bone_count; i++) {
        if (!is_identity16(obj.skin[i], 1e-4f)) all_identity = 0;
    }
    check(all_identity, "every bone's skin matrix is identity at the seeded rest pose");

    printf("[skinned_mesh_object_test] === 2: playback actually changes the pose over time ===\n");
    float duration = obj.playback.clip->duration;
    check(duration > 0.0f, "the playing clip has a real, positive duration");
    skinned_mesh_object_update(&obj, duration * 0.5f);   /* jump to the clip's midpoint in one step */
    int any_non_identity = 0;
    for (int i = 0; i < obj.arm.bone_count; i++) {
        if (!is_identity16(obj.skin[i], 1e-3f)) any_non_identity = 1;
    }
    check(any_non_identity, "at least one bone's skin matrix is no longer identity mid-animation (real pose change happened)");
    check(obj.playback.time > 0.0f, "playback.time actually advanced");

    printf("[skinned_mesh_object_test] === 3: paused/scrubbed pose is stable, not drifting ===\n");
    obj.playback.playing = 0;
    float scrub_time = duration * 0.25f;
    obj.playback.time = scrub_time;
    skinned_mesh_object_update(&obj, 1.0f);   /* large dt -- must NOT advance since playing=0 */
    check(obj.playback.time == scrub_time, "a paused playback's time does not advance regardless of dt");
    float snapshot[3][16];
    for (int i = 0; i < obj.arm.bone_count; i++) for (int j = 0; j < 16; j++) snapshot[i][j] = obj.skin[i][j];
    skinned_mesh_object_update(&obj, 1.0f);   /* update again at the same scrubbed time */
    int stable = 1;
    for (int i = 0; i < obj.arm.bone_count; i++)
        for (int j = 0; j < 16; j++)
            if (fabsf(obj.skin[i][j] - snapshot[i][j]) > 1e-6f) stable = 0;
    check(stable, "re-sampling the same scrubbed time twice produces the identical pose (deterministic, no drift)");

    printf("[skinned_mesh_object_test] === 4: looping wraps rather than freezing at the end ===\n");
    obj.playback.playing = 1;
    obj.playback.time = duration - 0.001f;
    skinned_mesh_object_update(&obj, 0.5f);   /* pushes well past duration -- loop=1 was set at load time */
    check(obj.playback.time >= 0.0f && obj.playback.time < duration, "a looping clip's time wraps back into [0, duration) rather than running past the end");
    check(obj.playback.playing, "a looping clip keeps playing=1 after wrapping (does not stop itself, unlike a non-looping clip)");

    printf("[skinned_mesh_object_test] === 5: teardown ===\n");
    skinned_mesh_object_free(&obj);
    check(obj.clip_count == 0 && obj.mesh.vert_count == 0, "free() really clears the object (zeroed, not just leaked-and-forgotten)");

    printf("\n[skinned_mesh_object_test] RESULT: %s\n", g_fail ? "FAIL (see above)" : "PASS (all checks passed)");
    return g_fail;
}
