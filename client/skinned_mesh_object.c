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

/* p' = M * p (column-major 4x4), same convention/formula as halfedge_
 * gltf.c's/skinned_mesh.c's own mat4_transform_point -- duplicated here
 * rather than shared for the same reason armature.c's own mat4_mul is
 * duplicated (see its comment): this file needs to stay GL-free/no-
 * external-header for the same no-window test harness linkage. */
static void mat4_transform_point(const float m[16], const float p[3], float out[3]) {
    out[0] = m[0]*p[0] + m[4]*p[1] + m[8]*p[2]  + m[12];
    out[1] = m[1]*p[0] + m[5]*p[1] + m[9]*p[2]  + m[13];
    out[2] = m[2]*p[0] + m[6]*p[1] + m[10]*p[2] + m[14];
}

int skinned_mesh_object_local_aabb(const SkinnedMeshObject *obj, Vec3f *out_min, Vec3f *out_max) {
    if (!obj || obj->mesh.vert_count <= 0) return 0;

    float mn[3], mx[3];
    int have_bounds = 0;
    for (int i = 0; i < obj->mesh.vert_count; i++) {
        const SkinnedVertex *v = &obj->mesh.verts[i];
        /* Same weighted skin-matrix blend as SKINNED_VERT_SRC_FMT's
         * vertex shader (renderer.c) -- elementwise, all 16 components,
         * not a matrix multiply of the blend itself. */
        float skin[16];
        for (int k = 0; k < 16; k++) {
            skin[k] = v->bone_wgt[0] * obj->skin[v->bone_idx[0]][k]
                    + v->bone_wgt[1] * obj->skin[v->bone_idx[1]][k]
                    + v->bone_wgt[2] * obj->skin[v->bone_idx[2]][k]
                    + v->bone_wgt[3] * obj->skin[v->bone_idx[3]][k];
        }
        float p[3];
        mat4_transform_point(skin, v->pos, p);
        if (!have_bounds) {
            mn[0] = mx[0] = p[0]; mn[1] = mx[1] = p[1]; mn[2] = mx[2] = p[2];
            have_bounds = 1;
        } else {
            for (int a = 0; a < 3; a++) {
                if (p[a] < mn[a]) mn[a] = p[a];
                if (p[a] > mx[a]) mx[a] = p[a];
            }
        }
    }

    out_min->x = mn[0]; out_min->y = mn[1]; out_min->z = mn[2];
    out_max->x = mx[0]; out_max->y = mx[1]; out_max->z = mx[2];
    return 1;
}
