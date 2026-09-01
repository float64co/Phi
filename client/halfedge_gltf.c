#include "halfedge_gltf.h"
/* Exactly one translation unit defines CGLTF_IMPLEMENTATION to get cgltf's
 * function bodies (not just declarations) — this is that one. */
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "cgltf_util.h"
#include "vecmath_simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Function-pointer handoff for real texture loading (texture_cache.h's
 * texture_cache_load), NOT a direct call/#include -- the same reason
 * mp_port.c's phi_mp_register_camera_callback/_gamepad_callbacks/_audio_
 * callbacks all exist (see mp_port.h's own comments): texture_cache.c
 * calls real GL functions, and this file is linked, unmodified, into
 * genuinely no-GL standalone test harnesses (mesh_edit_test, phi_h_test
 * -- see the Makefile's own MESH_EDIT_TEST_SRCS/PHI_H_TEST_SRCS). Real
 * engine executables (editor_main.c, player_main.c) register texture_
 * cache_load itself as this callback at startup; the test harnesses
 * never register anything, so every loaded material's texture field
 * just stays 0 (untextured, flat base_color only) there -- a real,
 * harmless degradation, not a build break. */
static unsigned int (*s_load_texture)(const char *path) = NULL;

void halfedge_gltf_register_texture_loader(unsigned int (*load_texture)(const char *path)) {
    s_load_texture = load_texture;
}

/* ---- Full-scene glTF loading (2026-08-19) -- see halfedge.h's HEFace::
 * texture and HEVertex::uv comments for why this replaced the original
 * "just mesh[0]/primitive[0]" loader: real multi-part, multi-material,
 * textured character exports (Sketchfab's own typical shape) need every
 * mesh/primitive in the file, positioned by its owning node's real world
 * transform, not just the first one found. ---- */

/* Point transform (p' = M*p, column-major 4x4, matching cgltf_node_
 * transform_world's own output layout, which is glTF's own matrix
 * convention) now comes from vecmath_simd.h as phi_mat4_transform_point
 * -- cgltf_float IS float (see cgltf.h's own typedef), so no cast needed
 * at call sites. Normals are NOT transformed here -- meshobject_build_
 * render_mesh_from_halfedge derives flat per-face normals from the
 * (already work-transformed) triangle positions themselves, the same
 * "no NORMAL attribute stored on HEVertex at all" design this loader
 * already had before this change, so a separate normal-matrix transform
 * was never needed. */

/* "<dir-of-base_file>/<decoded rel_uri>" -- glTF texture URIs are always
 * relative to the .gltf file's own directory, never to the process's
 * working directory. cgltf_decode_uri does real percent-decoding
 * in-place (spaces as %20 etc.) on a caller-owned copy, the same helper
 * cgltf's own buffer-URI resolution uses internally (see this file's
 * cgltf.h -- not reinvented here). Caller frees the returned string. */
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

/* Resolves a primitive's real material into a flat tint (glTF's
 * baseColorFactor, default white per spec -- multiplies whatever the
 * texture itself samples, not a fallback used only in its absence) and a
 * real GL texture name (0 if the material has none, or the file has no
 * material at all -- untextured geometry, e.g. this codebase's own
 * primitive test/demo shapes, works exactly as before). Only the
 * pbrMetallicRoughness.baseColorTexture channel is loaded -- normal/
 * metallic-roughness/emissive/occlusion maps are real, separate,
 * deliberately out-of-scope future work (this renderer's lighting model
 * has no tangent-space/normal-mapping machinery to consume them yet);
 * see this project's own engineering brief for the honest scope note. */
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

/* Appends one primitive's real triangles into hem, in `world` space
 * (already includes this primitive's owning node's full transform --
 * see walk_node below) with a real per-face material/texture (see
 * resolve_material above). Non-triangle primitives (glTF allows lines/
 * points/fans/strips) are skipped with a log line rather than silently
 * misreading their index buffer as a triangle list. */
static void append_primitive(HalfEdgeMesh *hem, const cgltf_primitive *prim,
                              const cgltf_float world[16], const char *gltf_path) {
    if (prim->type != cgltf_primitive_type_triangles) {
        printf("[halfedge_gltf] skipping a non-triangle primitive (mode=%d) in %s\n", (int)prim->type, gltf_path);
        return;
    }
    const cgltf_accessor *pos_acc = NULL, *uv_acc = NULL;
    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == cgltf_attribute_type_position && !pos_acc) pos_acc = prim->attributes[i].data;
        if (prim->attributes[i].type == cgltf_attribute_type_texcoord && !uv_acc)  uv_acc  = prim->attributes[i].data;
    }
    if (!pos_acc) {
        printf("[halfedge_gltf] skipping a primitive with no POSITION attribute in %s\n", gltf_path);
        return;
    }

    float color[3]; unsigned int tex;
    resolve_material(prim->material, gltf_path, color, &tex);

    int base_vertex = hem->vert_count;
    int pos_count = (int)pos_acc->count;
    for (int v = 0; v < pos_count; v++) {
        float p[3]; cgltf_accessor_read_float(pos_acc, (cgltf_size)v, p, 3);
        float wp[3]; phi_mat4_transform_point(world, p, wp);
        /* Z-up (2026-08-19, see vec3.h's coordinate-convention note):
         * glTF's own spec convention is Y-up; Phi's internal engine space
         * is Z-up. This is the exact +90-degree-about-X rotation vec3.h's
         * vec3_y_up_to_z_up documents (inlined here on raw floats rather
         * than pulling Vec3f/vec3.h into this translation unit just for
         * one call) -- files on disk stay spec-compliant Y-up (see
         * halfedge_save_glb_buffer's own matching inverse), only the
         * live, in-memory HalfEdgeMesh is Z-up. Applied AFTER the node's
         * own world transform above, which is still expressed in the
         * file's own Y-up space. */
        int idx = halfedge_add_vertex(hem, wp[0], -wp[2], wp[1]);
        if (uv_acc) {
            float uv[2]; cgltf_accessor_read_float(uv_acc, (cgltf_size)v, uv, 2);
            halfedge_set_vertex_uv(hem, idx, uv[0], uv[1]);
        }
    }

    int has_indices = prim->indices != NULL;
    int index_count = has_indices ? (int)prim->indices->count : pos_count;
    static const float emission_none[3] = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i + 2 < index_count; i += 3) {
        int tri[3];
        for (int k = 0; k < 3; k++) {
            int local = has_indices ? (int)cgltf_accessor_read_index(prim->indices, (cgltf_size)(i + k)) : (i + k);
            tri[k] = base_vertex + local;
        }
        int f = halfedge_add_face(hem, tri, 3);
        /* roughness 0.8 matches this structure's own existing "neutral
         * dielectric/rough" default (halfedge_add_face's own comment) --
         * glTF's real roughnessFactor isn't read here on purpose: this
         * pass wires up the base-color channel (what actually gives a
         * Sketchfab-style character its recognizable look), leaving
         * metallic/roughness-map support as the same kind of stated,
         * deliberate future work resolve_material's own comment already
         * flags for normal/occlusion maps. */
        halfedge_set_face_material(hem, f, color, 0.0f, 0.8f, emission_none);
        halfedge_set_face_texture(hem, f, tex);
    }
}

static void walk_node(HalfEdgeMesh *hem, const cgltf_node *node, const char *gltf_path) {
    if (node->mesh) {
        cgltf_float world[16];
        cgltf_node_transform_world(node, world);
        for (cgltf_size p = 0; p < node->mesh->primitives_count; p++) {
            append_primitive(hem, &node->mesh->primitives[p], world, gltf_path);
        }
    }
    for (cgltf_size c = 0; c < node->children_count; c++) {
        walk_node(hem, node->children[c], gltf_path);
    }
}

HalfEdgeMesh *halfedge_load_gltf(const char *path) {
    cgltf_data *data = cgltf_parse_and_load(path, "halfedge_gltf");
    if (!data) return NULL;

    /* The default scene if the file declares one; falling back to scene
     * 0 (most real files have exactly one scene either way), and finally
     * to every node with no parent, covers every real glTF file this
     * codebase is likely to ever be handed, not just ones that set
     * "scene" explicitly. */
    const cgltf_scene *scene = data->scene;
    if (!scene && data->scenes_count > 0) scene = &data->scenes[0];

    HalfEdgeMesh *hem = halfedge_create();
    if (scene) {
        for (cgltf_size i = 0; i < scene->nodes_count; i++) walk_node(hem, scene->nodes[i], path);
    } else {
        for (cgltf_size i = 0; i < data->nodes_count; i++) {
            if (!data->nodes[i].parent) walk_node(hem, &data->nodes[i], path);
        }
    }

    if (hem->face_count == 0) {
        printf("[halfedge_gltf] %s produced no real triangles (no mesh primitives reachable from its scene graph)\n", path);
        halfedge_destroy(hem);
        cgltf_free(data);
        return NULL;
    }

    printf("[halfedge_gltf] loaded %s: %d vertices, %d faces\n", path, hem->vert_count, hem->face_count);
    cgltf_free(data);
    return hem;
}

/* Real position-only AABB scan, shared by halfedge_save_gltf and halfedge_
 * save_glb_buffer below -- both need it (glTF's own accessor min/max are
 * required fields for a POSITION accessor per spec), previously computed
 * as two separately-written identical loops. positions is a flat xyz
 * array of pos_count vertices (pos_count must be >= 1 -- both callers
 * already guarantee this before calling). */
static void compute_pos_bounds(const float *positions, int pos_count, float pmin[3], float pmax[3]) {
    pmin[0] = pmax[0] = positions[0];
    pmin[1] = pmax[1] = positions[1];
    pmin[2] = pmax[2] = positions[2];
    for (int i = 1; i < pos_count; i++) {
        for (int a = 0; a < 3; a++) {
            float v = positions[i*3+a];
            if (v < pmin[a]) pmin[a] = v;
            if (v > pmax[a]) pmax[a] = v;
        }
    }
}

/* Inverse of the load-side conversion in append_primitive above (see its
 * own comment, and vec3.h's coordinate-convention note) -- Phi's
 * in-memory HalfEdgeMesh is Z-up; both save paths below write spec-
 * compliant Y-up glTF, so this runs once on the flattened position array
 * right after halfedge_flatten_triangles, before it's used for bounds or
 * written to disk. In-place: (x,y,z) -> (x,z,-y). */
static void convert_positions_z_up_to_y_up(float *positions, int pos_count) {
    for (int i = 0; i < pos_count; i++) {
        float y = positions[i*3+1], z = positions[i*3+2];
        positions[i*3+1] = z;
        positions[i*3+2] = -y;
    }
}

/* Derives "<dir>/<base>.bin" from a "<dir>/<base>.gltf"-shaped path (or
 * just appends ".bin" if there's no recognizable extension) — used to
 * name the sibling binary buffer file halfedge_save_gltf writes. Caller
 * frees the returned string. */
static char *bin_path_for(const char *gltf_path) {
    const char *dot = strrchr(gltf_path, '.');
    const char *slash = strrchr(gltf_path, '/');
    size_t base_len = (dot && dot > slash) ? (size_t)(dot - gltf_path) : strlen(gltf_path);
    char *out = (char *)malloc(base_len + 5);
    memcpy(out, gltf_path, base_len);
    memcpy(out + base_len, ".bin", 5);
    return out;
}

int halfedge_save_gltf(const HalfEdgeMesh *hem, const char *gltf_path) {
    float *positions; unsigned short *indices;
    int pos_count, index_count;
    halfedge_flatten_triangles(hem, &positions, &pos_count, &indices, &index_count);
    convert_positions_z_up_to_y_up(positions, pos_count);

    char *bin_path = bin_path_for(gltf_path);
    const char *bin_basename = strrchr(bin_path, '/');
    bin_basename = bin_basename ? bin_basename + 1 : bin_path;

    FILE *bf = fopen(bin_path, "wb");
    if (!bf) {
        printf("[halfedge_gltf] failed to open %s for writing\n", bin_path);
        free(positions); free(indices); free(bin_path);
        return 0;
    }
    size_t pos_bytes = sizeof(float) * 3 * (size_t)pos_count;
    size_t idx_bytes = sizeof(unsigned short) * (size_t)index_count;
    fwrite(positions, 1, pos_bytes, bf);
    fwrite(indices, 1, idx_bytes, bf);
    fclose(bf);

    float pmin[3], pmax[3];
    compute_pos_bounds(positions, pos_count, pmin, pmax);

    FILE *gf = fopen(gltf_path, "w");
    if (!gf) {
        printf("[halfedge_gltf] failed to open %s for writing\n", gltf_path);
        free(positions); free(indices); free(bin_path);
        return 0;
    }
    /* Hand-printed JSON, matching the shape of the hand-authored
     * assets/cube.gltf test asset this format was designed to round-trip
     * against — no JSON library, glTF's JSON is simple enough to emit
     * directly for this minimal position-only/single-primitive case. */
    fprintf(gf,
        "{\n"
        "  \"asset\": { \"version\": \"2.0\", \"generator\": \"phi halfedge_save_gltf\" },\n"
        "  \"buffers\": [ { \"uri\": \"%s\", \"byteLength\": %zu } ],\n"
        "  \"bufferViews\": [\n"
        "    { \"buffer\": 0, \"byteOffset\": 0, \"byteLength\": %zu, \"target\": 34962 },\n"
        "    { \"buffer\": 0, \"byteOffset\": %zu, \"byteLength\": %zu, \"target\": 34963 }\n"
        "  ],\n"
        "  \"accessors\": [\n"
        "    { \"bufferView\": 0, \"byteOffset\": 0, \"componentType\": 5126, \"count\": %d, "
              "\"type\": \"VEC3\", \"min\": [%g,%g,%g], \"max\": [%g,%g,%g] },\n"
        "    { \"bufferView\": 1, \"byteOffset\": 0, \"componentType\": 5123, \"count\": %d, \"type\": \"SCALAR\" }\n"
        "  ],\n"
        "  \"meshes\": [ { \"primitives\": [ { \"attributes\": { \"POSITION\": 0 }, \"indices\": 1, \"mode\": 4 } ] } ],\n"
        "  \"nodes\": [ { \"mesh\": 0 } ],\n"
        "  \"scenes\": [ { \"nodes\": [0] } ],\n"
        "  \"scene\": 0\n"
        "}\n",
        bin_basename, pos_bytes + idx_bytes,
        pos_bytes,
        pos_bytes, idx_bytes,
        pos_count, pmin[0], pmin[1], pmin[2], pmax[0], pmax[1], pmax[2],
        index_count);
    fclose(gf);

    free(positions); free(indices); free(bin_path);
    return 1;
}

int halfedge_save_glb_buffer(const HalfEdgeMesh *hem, uint8_t **out_data, int *out_len) {
    float *positions; unsigned short *indices;
    int pos_count, index_count;
    halfedge_flatten_triangles(hem, &positions, &pos_count, &indices, &index_count);
    if (pos_count <= 0) {
        printf("[halfedge_gltf] halfedge_save_glb_buffer: empty mesh, nothing to save\n");
        free(positions); free(indices);
        return 0;
    }
    convert_positions_z_up_to_y_up(positions, pos_count);

    size_t pos_bytes = sizeof(float) * 3 * (size_t)pos_count;
    size_t idx_bytes = sizeof(unsigned short) * (size_t)index_count;
    size_t bin_bytes = pos_bytes + idx_bytes;

    float pmin[3], pmax[3];
    compute_pos_bounds(positions, pos_count, pmin, pmax);

    /* Same JSON shape halfedge_save_gltf emits above, minus the buffer's
     * "uri" -- GLB's buffer 0 is implicitly the BIN chunk that follows,
     * no uri needed or allowed per the glTF 2.0 binary container spec. */
    const char *json_fmt =
        "{\"asset\":{\"version\":\"2.0\",\"generator\":\"phi halfedge_save_glb_buffer\"},"
        "\"buffers\":[{\"byteLength\":%zu}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%zu,\"target\":34962},"
        "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":%d,\"type\":\"VEC3\","
        "\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]},"
        "{\"bufferView\":1,\"byteOffset\":0,\"componentType\":5123,\"count\":%d,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1,\"mode\":4}]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"scene\":0}";

    int json_len = snprintf(NULL, 0, json_fmt, bin_bytes, pos_bytes, pos_bytes, idx_bytes,
                             pos_count, (double)pmin[0], (double)pmin[1], (double)pmin[2],
                             (double)pmax[0], (double)pmax[1], (double)pmax[2], index_count);
    if (json_len < 0) { free(positions); free(indices); return 0; }

    char *json_txt = (char *)malloc((size_t)json_len + 1);
    snprintf(json_txt, (size_t)json_len + 1, json_fmt, bin_bytes, pos_bytes, pos_bytes, idx_bytes,
             pos_count, (double)pmin[0], (double)pmin[1], (double)pmin[2],
             (double)pmax[0], (double)pmax[1], (double)pmax[2], index_count);

    int json_pad = (4 - (json_len % 4)) % 4;
    int bin_pad  = (4 - ((int)bin_bytes % 4)) % 4;
    int json_chunk_len = json_len + json_pad;
    int bin_chunk_len  = (int)bin_bytes + bin_pad;
    int total_len = 12 + 8 + json_chunk_len + 8 + bin_chunk_len;

    uint8_t *buf = (uint8_t *)malloc((size_t)total_len);
    uint8_t *p = buf;

    memcpy(p, "glTF", 4); p += 4;
    uint32_t version = 2; memcpy(p, &version, 4); p += 4;
    uint32_t total_len_u32 = (uint32_t)total_len; memcpy(p, &total_len_u32, 4); p += 4;

    uint32_t jclen = (uint32_t)json_chunk_len; memcpy(p, &jclen, 4); p += 4;
    uint32_t jctype = 0x4E4F534A; memcpy(p, &jctype, 4); p += 4;   /* 'JSON' */
    memcpy(p, json_txt, (size_t)json_len); p += json_len;
    for (int i = 0; i < json_pad; i++) *p++ = ' ';

    uint32_t bclen = (uint32_t)bin_chunk_len; memcpy(p, &bclen, 4); p += 4;
    uint32_t bctype = 0x004E4942; memcpy(p, &bctype, 4); p += 4;   /* 'BIN\0' */
    memcpy(p, positions, pos_bytes); p += pos_bytes;
    memcpy(p, indices, idx_bytes); p += idx_bytes;
    for (int i = 0; i < bin_pad; i++) *p++ = 0;

    free(json_txt);
    free(positions);
    free(indices);

    *out_data = buf;
    *out_len = total_len;
    return 1;
}
