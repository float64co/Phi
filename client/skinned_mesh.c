#include "skinned_mesh.h"
#include "cgltf.h"
#include "cgltf_util.h"
#include "vecmath_simd.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* See skinned_mesh.h's own comment on why this is a function pointer,
 * not a direct call -- mirrors halfedge_gltf_register_texture_loader
 * exactly. */
static unsigned int (*s_load_texture)(const char *path) = NULL;

void skinned_mesh_register_texture_loader(unsigned int (*load_texture)(const char *path)) {
    s_load_texture = load_texture;
}

static const cgltf_accessor *find_attribute(const cgltf_primitive *prim, cgltf_attribute_type type, cgltf_int index) {
    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == type && prim->attributes[i].index == index)
            return prim->attributes[i].data;
    }
    return NULL;
}

/* Real GL name transform + material resolution -- deliberately a near-
 * duplicate of halfedge_gltf.c's own resolve_material/resolve_relative_
 * path rather than a shared header: these are two genuinely independent
 * loaders (static vs. skinned), the shared logic is small (well under a
 * hundred lines total), and a real shared abstraction would couple them
 * for no functional benefit -- the same "small, well-justified
 * duplication beats a forced shared module" judgment call this
 * codebase's own gl_native.h/.c X-macro lists already make for a
 * similar-sized case. mat4_transform_point itself, though, now comes
 * from vecmath_simd.h as phi_mat4_transform_point (see its own top
 * comment) -- that one specific function WAS genuinely identical,
 * general-purpose 4x4 point-transform math with zero coupling risk,
 * unlike resolve_material/resolve_relative_path's real per-loader logic. */

static char *resolve_relative_path(const char *base_file, const char *rel_uri) {
    const char *slash = strrchr(base_file, '/');
    size_t dir_len = slash ? (size_t)(slash - base_file + 1) : 0;
    char *decoded = strdup(rel_uri);
    cgltf_decode_uri(decoded);
    char *out = (char *)malloc(dir_len + strlen(decoded) + 1);
    if (dir_len) memcpy(out, base_file, dir_len);
    strcpy(out + dir_len, decoded);
    free(decoded);
    return out;
}

static void resolve_material(const cgltf_material *mat, const char *gltf_path,
                              float out_color[3], unsigned int *out_texture) {
    out_color[0] = out_color[1] = out_color[2] = 1.0f;
    *out_texture = 0;
    if (!mat || !mat->has_pbr_metallic_roughness) return;
    out_color[0] = mat->pbr_metallic_roughness.base_color_factor[0];
    out_color[1] = mat->pbr_metallic_roughness.base_color_factor[1];
    out_color[2] = mat->pbr_metallic_roughness.base_color_factor[2];
    const cgltf_texture_view *tv = &mat->pbr_metallic_roughness.base_color_texture;
    if (s_load_texture && tv->texture && tv->texture->image && tv->texture->image->uri) {
        char *resolved = resolve_relative_path(gltf_path, tv->texture->image->uri);
        *out_texture = s_load_texture(resolved);
        free(resolved);
    }
}

/* Nearest ancestor of `node` that is itself one of `skin`'s joints --
 * walks node->parent (real cgltf pointer identity, not name matching)
 * until it finds one or runs out of ancestors. Returns that joint's
 * index within skin->joints (still the file's own raw order -- the
 * caller remaps to the sorted Armature the same way JOINTS_0 values
 * already do, see this file's own top comment), or -1 if no ancestor is
 * a joint of this skin at all. Used to rigidly bind an unskinned
 * attachment (a helmet, a weapon) to whichever bone it's actually
 * parented under in the file's own node hierarchy. */
static int find_ancestor_joint(const cgltf_node *node, const cgltf_skin *skin) {
    for (const cgltf_node *n = node->parent; n; n = n->parent) {
        for (cgltf_size j = 0; j < skin->joints_count; j++) {
            if (skin->joints[j] == n) return (int)j;
        }
    }
    return -1;
}

#define GROW_CAP(cap) ((cap) < 64 ? 64 : (cap) * 2)

/* Appends one primitive's real vertices/indices into out_mesh, starting
 * a new SkinnedSubmesh for it (see skinned_mesh.h's own comment on the
 * two real vertex shapes -- skinned vs. rigidly-attached -- this
 * function is called for both, with `world`/`rigid_bone_idx` set
 * appropriately by the caller for each). world may be NULL for a truly
 * skinned primitive (its own POSITION/NORMAL are already in the right
 * space, no baking needed); rigid_bone_idx is -1 for a truly skinned
 * primitive (real per-vertex JOINTS_0/WEIGHTS_0 used instead). */
static void append_primitive(SkinnedMesh *out_mesh, int *vert_cap, int *index_cap,
                              const cgltf_primitive *prim, const cgltf_skin *skin, const Armature *arm,
                              const cgltf_float *world, int rigid_bone_idx, const char *gltf_path) {
    if (prim->type != cgltf_primitive_type_triangles || !prim->indices) {
        printf("[skinned_mesh] skipping a non-triangle or non-indexed primitive in %s\n", gltf_path);
        return;
    }
    const cgltf_accessor *pos_acc  = find_attribute(prim, cgltf_attribute_type_position, 0);
    const cgltf_accessor *norm_acc = find_attribute(prim, cgltf_attribute_type_normal, 0);
    const cgltf_accessor *uv_acc   = find_attribute(prim, cgltf_attribute_type_texcoord, 0);
    const cgltf_accessor *joints_acc  = rigid_bone_idx < 0 ? find_attribute(prim, cgltf_attribute_type_joints, 0)  : NULL;
    const cgltf_accessor *weights_acc = rigid_bone_idx < 0 ? find_attribute(prim, cgltf_attribute_type_weights, 0) : NULL;
    if (!pos_acc || !norm_acc || (rigid_bone_idx < 0 && (!joints_acc || !weights_acc))) {
        printf("[skinned_mesh] skipping a primitive missing required attributes in %s\n", gltf_path);
        return;
    }

    float color[3]; unsigned int tex;
    resolve_material(prim->material, gltf_path, color, &tex);

    int base_vertex = out_mesh->vert_count;
    int add_count = (int)pos_acc->count;
    if (out_mesh->vert_count + add_count > *vert_cap) {
        while (*vert_cap < out_mesh->vert_count + add_count) *vert_cap = GROW_CAP(*vert_cap);
        out_mesh->verts = (SkinnedVertex *)realloc(out_mesh->verts, (size_t)*vert_cap * sizeof(SkinnedVertex));
    }

    for (int v = 0; v < add_count; v++) {
        SkinnedVertex *sv = &out_mesh->verts[base_vertex + v];
        memset(sv, 0, sizeof(*sv));
        float p[3]; cgltf_accessor_read_float(pos_acc, (cgltf_size)v, p, 3);
        if (world) phi_mat4_transform_point(world, p, sv->pos);
        else { sv->pos[0] = p[0]; sv->pos[1] = p[1]; sv->pos[2] = p[2]; }
        cgltf_accessor_read_float(norm_acc, (cgltf_size)v, sv->normal, 3);
        /* Z-up (2026-08-19, see vec3.h's coordinate-convention note):
         * both branches above still leave real glTF-file (Y-up) data in
         * sv->pos -- world-baked or not, neither one has been through an
         * axis conversion yet. sv->normal is a real per-vertex direction
         * (read straight from the file, never recomputed later the way
         * halfedge_gltf.c's mesh normals are) -- vec3_y_up_to_z_up is a
         * pure rotation, so it converts a direction exactly like a
         * position, no separate inverse-transpose handling needed. */
        Vec3f pos_conv = vec3_y_up_to_z_up((Vec3f){sv->pos[0], sv->pos[1], sv->pos[2]});
        sv->pos[0] = pos_conv.x; sv->pos[1] = pos_conv.y; sv->pos[2] = pos_conv.z;
        Vec3f norm_conv = vec3_y_up_to_z_up((Vec3f){sv->normal[0], sv->normal[1], sv->normal[2]});
        sv->normal[0] = norm_conv.x; sv->normal[1] = norm_conv.y; sv->normal[2] = norm_conv.z;
        if (uv_acc) cgltf_accessor_read_float(uv_acc, (cgltf_size)v, sv->uv, 2);

        if (rigid_bone_idx >= 0) {
            sv->bone_idx[0] = (uint8_t)rigid_bone_idx;
            sv->bone_wgt[0] = 1.0f;
        } else {
            cgltf_uint raw_joints[4] = {0, 0, 0, 0};
            cgltf_accessor_read_uint(joints_acc, (cgltf_size)v, raw_joints, 4);
            float raw_weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            cgltf_accessor_read_float(weights_acc, (cgltf_size)v, raw_weights, 4);
            for (int k = 0; k < 4; k++) {
                int bone_idx = 0;
                cgltf_uint orig_joint = raw_joints[k];
                if (orig_joint < skin->joints_count && skin->joints[orig_joint]->name) {
                    int found = armature_find_bone(arm, skin->joints[orig_joint]->name);
                    if (found >= 0) bone_idx = found;
                }
                sv->bone_idx[k] = (uint8_t)bone_idx;
                sv->bone_wgt[k] = raw_weights[k];
            }
        }
    }
    out_mesh->vert_count += add_count;

    int submesh_index_start = out_mesh->index_count;
    int add_indices = (int)prim->indices->count;
    if (out_mesh->index_count + add_indices > *index_cap) {
        while (*index_cap < out_mesh->index_count + add_indices) *index_cap = GROW_CAP(*index_cap);
        out_mesh->indices = (uint32_t *)realloc(out_mesh->indices, (size_t)*index_cap * sizeof(uint32_t));
    }
    for (int i = 0; i < add_indices; i++) {
        out_mesh->indices[out_mesh->index_count + i] =
            (uint32_t)(base_vertex + (int)cgltf_accessor_read_index(prim->indices, (cgltf_size)i));
    }
    out_mesh->index_count += add_indices;

    if (out_mesh->submesh_count < SKINNED_MESH_MAX_SUBMESHES) {
        SkinnedSubmesh *sm = &out_mesh->submeshes[out_mesh->submesh_count++];
        sm->index_start = submesh_index_start;
        sm->index_count = add_indices;
        sm->base_color[0] = color[0]; sm->base_color[1] = color[1]; sm->base_color[2] = color[2];
        sm->texture = tex;
    } else {
        printf("[skinned_mesh] SKINNED_MESH_MAX_SUBMESHES (%d) reached in %s -- this primitive's material "
               "will visually merge with whichever submesh comes right before it\n", SKINNED_MESH_MAX_SUBMESHES, gltf_path);
    }
}

/* Walks the scene graph looking for mesh-bearing nodes, dispatching each
 * primitive to append_primitive with the right world/rigid_bone_idx for
 * whether the node is bound to `skin` or not (see skinned_mesh.h's own
 * comment on the two real cases). */
static void walk_node(SkinnedMesh *out_mesh, int *vert_cap, int *index_cap,
                       const cgltf_node *node, const cgltf_skin *skin, const Armature *arm, const char *gltf_path) {
    if (node->mesh) {
        if (node->skin == skin) {
            for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
                append_primitive(out_mesh, vert_cap, index_cap, &node->mesh->primitives[p], skin, arm, NULL, -1, gltf_path);
        } else {
            cgltf_float world[16];
            cgltf_node_transform_world(node, world);
            int bone_idx = find_ancestor_joint(node, skin);
            if (bone_idx < 0) {
                printf("[skinned_mesh] node '%s' has no skin and no ancestor joint in %s -- "
                       "rigidly attaching to bone 0 as a real, honest fallback\n",
                       node->name ? node->name : "(unnamed)", gltf_path);
                bone_idx = 0;
            } else {
                /* Remap from skin->joints' own raw order to the sorted
                 * Armature, same as JOINTS_0 values -- see this file's
                 * top comment. */
                const char *jname = skin->joints[bone_idx]->name;
                int found = jname ? armature_find_bone(arm, jname) : -1;
                bone_idx = found >= 0 ? found : 0;
            }
            for (cgltf_size p = 0; p < node->mesh->primitives_count; p++)
                append_primitive(out_mesh, vert_cap, index_cap, &node->mesh->primitives[p], skin, arm, world, bone_idx, gltf_path);
        }
    }
    for (cgltf_size c = 0; c < node->children_count; c++)
        walk_node(out_mesh, vert_cap, index_cap, node->children[c], skin, arm, gltf_path);
}

int skinned_mesh_load_gltf(const char *path, Armature *out_arm, SkinnedMesh *out_mesh) {
    if (!path || !out_arm || !out_mesh) return 0;
    memset(out_mesh, 0, sizeof(*out_mesh));

    cgltf_data *data = cgltf_parse_and_load(path, "skinned_mesh");
    if (!data) return 0;
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

    const cgltf_scene *scene = data->scene;
    if (!scene && data->scenes_count > 0) scene = &data->scenes[0];

    int vert_cap = 0, index_cap = 0;
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; i++)
            walk_node(out_mesh, &vert_cap, &index_cap, scene->nodes[i], skin, out_arm, path);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; i++) {
            if (!data->nodes[i].parent) walk_node(out_mesh, &vert_cap, &index_cap, &data->nodes[i], skin, out_arm, path);
        }
    }
    cgltf_free(data);

    if (out_mesh->index_count == 0) {
        printf("[skinned_mesh] %s produced no real triangles\n", path);
        skinned_mesh_free(out_mesh);
        return 0;
    }
    printf("[skinned_mesh] loaded %s: %d vertices, %d indices, %d submeshes\n",
           path, out_mesh->vert_count, out_mesh->index_count, out_mesh->submesh_count);
    return 1;
}

void skinned_mesh_free(SkinnedMesh *mesh) {
    if (!mesh) return;
    free(mesh->verts);
    free(mesh->indices);
    memset(mesh, 0, sizeof(*mesh));
}

