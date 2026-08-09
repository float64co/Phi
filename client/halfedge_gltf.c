#include "halfedge_gltf.h"
/* Exactly one translation unit defines CGLTF_IMPLEMENTATION to get cgltf's
 * function bodies (not just declarations) — this is that one. */
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

HalfEdgeMesh *halfedge_load_gltf(const char *path) {
    cgltf_options options;
    memset(&options, 0, sizeof(options));
    cgltf_data *data = NULL;

    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) {
        printf("[halfedge_gltf] failed to parse %s\n", path);
        return NULL;
    }
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        printf("[halfedge_gltf] failed to load buffers for %s\n", path);
        cgltf_free(data);
        return NULL;
    }
    if (data->meshes_count == 0 || data->meshes[0].primitives_count == 0) {
        printf("[halfedge_gltf] %s has no mesh primitives\n", path);
        cgltf_free(data);
        return NULL;
    }

    cgltf_primitive *prim = &data->meshes[0].primitives[0];
    if (prim->type != cgltf_primitive_type_triangles) {
        printf("[halfedge_gltf] %s primitive 0 isn't a triangle list (mode=%d) — "
               "only triangles are supported in this pass\n", path, (int)prim->type);
        cgltf_free(data);
        return NULL;
    }

    cgltf_accessor *pos_accessor = NULL;
    for (cgltf_size i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == cgltf_attribute_type_position) {
            pos_accessor = prim->attributes[i].data;
            break;
        }
    }
    if (!pos_accessor) {
        printf("[halfedge_gltf] %s primitive 0 has no POSITION attribute\n", path);
        cgltf_free(data);
        return NULL;
    }

    int pos_count = (int)pos_accessor->count;
    float *positions = (float *)malloc(sizeof(float) * 3 * (size_t)pos_count);
    cgltf_accessor_unpack_floats(pos_accessor, positions, (cgltf_size)pos_count * 3);

    int index_count = prim->indices ? (int)prim->indices->count : 0;
    unsigned short *indices = NULL;
    if (index_count > 0) {
        indices = (unsigned short *)malloc(sizeof(unsigned short) * (size_t)index_count);
        for (int i = 0; i < index_count; i++)
            indices[i] = (unsigned short)cgltf_accessor_read_index(prim->indices, (cgltf_size)i);
    }

    HalfEdgeMesh *hem = halfedge_build_from_triangles(positions, pos_count, indices, index_count);
    free(positions);
    free(indices);
    cgltf_free(data);
    return hem;
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

    size_t pos_bytes = sizeof(float) * 3 * (size_t)pos_count;
    size_t idx_bytes = sizeof(unsigned short) * (size_t)index_count;
    size_t bin_bytes = pos_bytes + idx_bytes;

    float pmin[3], pmax[3];
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
