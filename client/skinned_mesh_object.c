#include "skinned_mesh_object.h"
#include "cgltf.h"
#include <string.h>

int skinned_mesh_object_load(const char *path, Vec3f position, SkinnedMeshObject *out) {
    memset(out, 0, sizeof(*out));
    if (!skinned_mesh_load_gltf(path, &out->arm, &out->mesh)) return 0;

    /* animation_load_clips needs a real cgltf_data (see its own
     * signature) -- skinned_mesh_load_gltf parses+frees its OWN cgltf_
     * data internally rather than exposing it (see skinned_mesh.c), so
     * this re-parses the same file once more here. A real, honest
     * double-parse (not sharing work), acceptable for this pass's scope
     * (load-time only, not a hot path) -- the two parses being of the
     * SAME file is what makes this correct: animation_load_clips only
     * needs out->arm for bone-NAME lookups (armature_find_bone), so it
     * doesn't matter that this is a second, independent Armature-load
     * pass under the hood rather than sharing skinned_mesh_load_gltf's
     * own internal one -- both produce the identical bone set/names
     * (armature_load_from_skin's topological sort is deterministic for
     * a given file). */
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&options, path, &data) == cgltf_result_success &&
        cgltf_load_buffers(&options, data, path) == cgltf_result_success) {
        out->clip_count = animation_load_clips(data, &out->arm, out->clips, 8);
    }
    if (data) cgltf_free(data);

    out->position = position;
    out->orientation = quat_identity();
    out->scale = (Vec3f){1.0f, 1.0f, 1.0f};
    /* Same neutral default halfedge_add_face's own new-face material
     * uses (see halfedge.h) -- one consistent "reasonable default
     * material" across this codebase's entity types. */
    out->base_color = (Vec3f){0.7f, 0.7f, 0.7f};
    out->metallic = 0.0f;
    out->roughness = 0.8f;
    out->emission = (Vec3f){0.0f, 0.0f, 0.0f};

    if (out->clip_count > 0) {
        anim_playback_play(&out->playback, &out->clips[0], 1);
    }

    /* Seed world/skin with the rest pose so a freshly-loaded object
     * already has a valid (identity-skinned) pose to draw even before
     * the first skinned_mesh_object_update call. */
    Vec3f t[ARMATURE_MAX_BONES]; Quat r[ARMATURE_MAX_BONES]; Vec3f s[ARMATURE_MAX_BONES];
    armature_rest_pose(&out->arm, t, r, s);
    armature_compute_world_transforms(&out->arm, t, r, s, out->world);
    armature_compute_skinning_matrices(&out->arm, out->world, out->skin);

    out->gpu_uploaded = 0;
    return 1;
}

void skinned_mesh_object_update(SkinnedMeshObject *obj, float dt) {
    Vec3f t[ARMATURE_MAX_BONES]; Quat r[ARMATURE_MAX_BONES]; Vec3f s[ARMATURE_MAX_BONES];
    armature_rest_pose(&obj->arm, t, r, s);
    if (obj->playback.clip && obj->playback.playing) {
        anim_playback_advance(&obj->playback, dt);
    }
    if (obj->playback.clip) {
        /* Sampled unconditionally (not just while playing) so a paused/
         * Timeline-scrubbed pose (obj->playback.time set directly,
         * playing left at 0) still updates the visible pose every
         * frame, not just while actively playing. */
        animation_sample_clip(obj->playback.clip, obj->playback.time, t, r, s);
    }
    armature_compute_world_transforms(&obj->arm, t, r, s, obj->world);
    armature_compute_skinning_matrices(&obj->arm, obj->world, obj->skin);
}

void skinned_mesh_object_free(SkinnedMeshObject *obj) {
    if (!obj) return;
    skinned_mesh_free(&obj->mesh);
    for (int i = 0; i < obj->clip_count; i++) animation_clip_free(&obj->clips[i]);
    memset(obj, 0, sizeof(*obj));
}
