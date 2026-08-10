#include "skinned_mesh.h"
#include "cgltf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const cgltf_accessor *find_attribute(const cgltf_primitive *prim, cgltf_attribute_type type, cgltf_int index) {
    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == type && prim->attributes[i].index == index)
            return prim->attributes[i].data;
    }
    return NULL;
}

int skinned_mesh_load_gltf(const char *path, Armature *out_arm, SkinnedMesh *out_mesh) {
    if (!path || !out_arm || !out_mesh) return 0;
    memset(out_mesh, 0, sizeof(*out_mesh));

    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
        printf("[skinned_mesh] failed to parse %s\n", path);
        return 0;
    }
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        printf("[skinned_mesh] failed to load buffers for %s\n", path);
        cgltf_free(data);
        return 0;
    }
    if (data->skins_count == 0) {
        printf("[skinned_mesh] %s has no skin\n", path);
        cgltf_free(data);
        return 0;
    }
    const cgltf_skin *skin = &data->skins[0];
    if (!armature_load_from_skin(skin, out_arm)) {
        cgltf_free(data);
        return 0;
    }
    if (data->meshes_count == 0 || data->meshes[0].primitives_count == 0) {
        printf("[skinned_mesh] %s has no mesh primitives\n", path);
        cgltf_free(data);
        return 0;
    }
    const cgltf_primitive *prim = &data->meshes[0].primitives[0];
    if (prim->type != cgltf_primitive_type_triangles) {
        printf("[skinned_mesh] %s primitive 0 isn't a triangle list (mode=%d)\n", path, (int)prim->type);
        cgltf_free(data);
        return 0;
    }

    const cgltf_accessor *pos_acc     = find_attribute(prim, cgltf_attribute_type_position, 0);
    const cgltf_accessor *norm_acc    = find_attribute(prim, cgltf_attribute_type_normal, 0);
    const cgltf_accessor *joints_acc  = find_attribute(prim, cgltf_attribute_type_joints, 0);
    const cgltf_accessor *weights_acc = find_attribute(prim, cgltf_attribute_type_weights, 0);
    if (!pos_acc || !norm_acc || !joints_acc || !weights_acc || !prim->indices) {
        printf("[skinned_mesh] %s primitive 0 is missing POSITION/NORMAL/JOINTS_0/WEIGHTS_0/indices\n", path);
        cgltf_free(data);
        return 0;
    }

    int vert_count = (int)pos_acc->count;
    SkinnedVertex *verts = (SkinnedVertex *)calloc((size_t)vert_count, sizeof(SkinnedVertex));

    for (int v = 0; v < vert_count; v++) {
        cgltf_accessor_read_float(pos_acc, (cgltf_size)v, verts[v].pos, 3);
        cgltf_accessor_read_float(norm_acc, (cgltf_size)v, verts[v].normal, 3);

        cgltf_uint raw_joints[4] = {0, 0, 0, 0};
        cgltf_accessor_read_uint(joints_acc, (cgltf_size)v, raw_joints, 4);
        float raw_weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        cgltf_accessor_read_float(weights_acc, (cgltf_size)v, raw_weights, 4);

        for (int k = 0; k < 4; k++) {
            /* Remap: raw_joints[k] indexes skin->joints (the file's own,
             * possibly-scrambled order) -- look up that ORIGINAL joint
             * node's name, then find that name in the already-
             * topologically-sorted out_arm to get the real bone index.
             * See this file's header comment for why this remap is
             * necessary at all, not optional. */
            int bone_idx = 0;
            cgltf_uint orig_joint = raw_joints[k];
            if (orig_joint < skin->joints_count && skin->joints[orig_joint]->name) {
                int found = armature_find_bone(out_arm, skin->joints[orig_joint]->name);
                if (found >= 0) bone_idx = found;
            }
            verts[v].bone_idx[k] = (uint8_t)bone_idx;
            verts[v].bone_wgt[k] = raw_weights[k];
        }
    }

    int index_count = (int)prim->indices->count;
    uint16_t *indices = (uint16_t *)malloc((size_t)index_count * sizeof(uint16_t));
    for (int i = 0; i < index_count; i++)
        indices[i] = (uint16_t)cgltf_accessor_read_index(prim->indices, (cgltf_size)i);

    out_mesh->verts = verts;
    out_mesh->vert_count = vert_count;
    out_mesh->indices = indices;
    out_mesh->index_count = index_count;

    cgltf_free(data);
    return 1;
}

void skinned_mesh_free(SkinnedMesh *mesh) {
    if (!mesh) return;
    free(mesh->verts);
    free(mesh->indices);
    mesh->verts = NULL;
    mesh->indices = NULL;
    mesh->vert_count = 0;
    mesh->index_count = 0;
}
