#include "halfedge.h"
#include <stdlib.h>
#include <string.h>

static void *grow(void *p, int *cap, int need, size_t elem_size) {
    if (need <= *cap) return p;
    int new_cap = *cap ? *cap * 2 : 8;
    while (new_cap < need) new_cap *= 2;
    p = realloc(p, (size_t)new_cap * elem_size);
    *cap = new_cap;
    return p;
}

HalfEdgeMesh *halfedge_create(void) {
    return (HalfEdgeMesh *)calloc(1, sizeof(HalfEdgeMesh));
}

void halfedge_destroy(HalfEdgeMesh *hem) {
    if (!hem) return;
    free(hem->verts);
    free(hem->edges);
    free(hem->faces);
    free(hem->edge_hash);
    free(hem);
}

/* -- Twin-edge hash map (see halfedge.h's top comment) -------------------
 * Open addressing, linear probing, no removal (matches this structure's
 * own append-only/tombstone-don't-scrub model -- see halfedge_delete_face).
 * A slot holds an edge index or -1 (empty). Both insert and lookup follow
 * the SAME deterministic probe sequence starting at hash(origin,dest); since
 * insert always lands in the first truly-empty slot along that sequence and
 * slots are never cleared, a lookup that walks the same sequence is
 * guaranteed to pass through every edge ever inserted under that key before
 * it can reach an empty slot -- correct even with hash collisions (verified
 * by comparing the edge's own origin/dest, not just the hash) and even with
 * tombstoned (deleted-face) entries left in place (lookup just keeps
 * probing past those, exactly like the original linear scan did). */

static size_t edge_hash_of(int from, int to) {
    size_t h = (size_t)from * 0x9E3779B97F4A7C15ULL + (size_t)to * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 33;
    return h;
}

static size_t next_pow2(size_t n) {
    size_t p = 16;
    while (p < n) p *= 2;
    return p;
}

/* Rebuilds the hash table from scratch against every edge currently in
 * hem->edges (0..edge_count), sized so the table stays well under a load
 * factor of 0.5. Called whenever the table would otherwise get too full --
 * amortized O(1) per edge over the mesh's whole build, same doubling
 * amortization grow() already gives vert/edge/face arrays. */
static void edge_hash_rebuild(HalfEdgeMesh *hem, int min_edge_room) {
    size_t new_cap = next_pow2((size_t)(hem->edge_count + min_edge_room) * 4);
    int *table = (int *)malloc(new_cap * sizeof(int));
    for (size_t i = 0; i < new_cap; i++) table[i] = -1;

    free(hem->edge_hash);
    hem->edge_hash = table;
    hem->edge_hash_cap = (int)new_cap;

    size_t mask = new_cap - 1;
    for (int e = 0; e < hem->edge_count; e++) {
        int from = hem->edges[e].origin;
        int to   = hem->edges[hem->edges[e].next].origin;
        size_t slot = edge_hash_of(from, to) & mask;
        while (table[slot] != -1) slot = (slot + 1) & mask;
        table[slot] = e;
    }
}

static void edge_hash_insert(HalfEdgeMesh *hem, int edge_idx) {
    size_t mask = (size_t)hem->edge_hash_cap - 1;
    int from = hem->edges[edge_idx].origin;
    int to   = hem->edges[hem->edges[edge_idx].next].origin;
    size_t slot = edge_hash_of(from, to) & mask;
    while (hem->edge_hash[slot] != -1) slot = (slot + 1) & mask;
    hem->edge_hash[slot] = edge_idx;
}

int halfedge_add_vertex(HalfEdgeMesh *hem, float x, float y, float z) {
    hem->verts = (HEVertex *)grow(hem->verts, &hem->vert_cap, hem->vert_count + 1, sizeof(HEVertex));
    HEVertex *v = &hem->verts[hem->vert_count];
    v->pos[0] = x; v->pos[1] = y; v->pos[2] = z;
    v->uv[0] = 0.0f; v->uv[1] = 0.0f;
    v->edge = -1;
    return hem->vert_count++;
}

void halfedge_set_vertex_uv(HalfEdgeMesh *hem, int v, float u, float vcoord) {
    if (v < 0 || v >= hem->vert_count) return;
    hem->verts[v].uv[0] = u;
    hem->verts[v].uv[1] = vcoord;
}

/* Finds a LIVE half-edge running from `from` to `to` -- used to find the
 * just-added edge going the OPPOSITE direction of a new one, i.e. its twin.
 * Hash-map-backed (see this file's edge_hash_* helpers + halfedge.h's top
 * comment), not a linear scan. */
static int find_edge(const HalfEdgeMesh *hem, int from, int to) {
    if (hem->edge_hash_cap == 0) return -1;
    size_t mask = (size_t)hem->edge_hash_cap - 1;
    size_t slot = edge_hash_of(from, to) & mask;
    for (;;) {
        int e = hem->edge_hash[slot];
        if (e == -1) return -1;
        /* Skip edges belonging to a soft-deleted face (halfedge_delete_face)
         * -- their origin/next fields are left untouched (tombstoning
         * doesn't scrub edge data, see the struct comment), so without this
         * check a newly-added face re-triangulating the same vertex
         * neighborhood a deleted face used to occupy (extrude/inset/
         * loop-cut all do exactly this, see mesh_edit.c) could accidentally
         * twin against dead geometry instead of a real live neighbor. Keep
         * probing rather than stopping -- a live match may still be further
         * along this key's probe sequence (see edge_hash_rebuild's comment). */
        if (!hem->faces[hem->edges[e].face].deleted &&
            hem->edges[e].origin == from && hem->edges[hem->edges[e].next].origin == to)
            return e;
        slot = (slot + 1) & mask;
    }
}

int halfedge_add_face(HalfEdgeMesh *hem, const int *vert_indices, int n) {
    hem->faces = (HEFace *)grow(hem->faces, &hem->face_cap, hem->face_count + 1, sizeof(HEFace));
    int face_idx = hem->face_count++;

    hem->edges = (HEEdge *)grow(hem->edges, &hem->edge_cap, hem->edge_count + n, sizeof(HEEdge));
    int first_edge = hem->edge_count;
    for (int i = 0; i < n; i++) {
        HEEdge *e = &hem->edges[hem->edge_count + i];
        e->origin = vert_indices[i];
        e->face   = face_idx;
        e->twin   = -1;
        e->next   = first_edge + (i + 1) % n;
        e->prev   = first_edge + (i - 1 + n) % n;
    }
    hem->edge_count += n;

    /* Keep the twin-lookup hash table under a 0.5 load factor -- rebuild
     * (rare, amortized) rather than grow in place, same doubling-cost-
     * amortized-to-O(1) trade the vert/edge/face arrays' own grow() makes.
     * edge_hash_rebuild re-inserts every edge up to the now-already-
     * incremented edge_count (the n new ones included), so on a rebuild the
     * per-edge insert loop below would double-insert them -- skipped then. */
    if ((hem->edge_count + 1) * 2 > hem->edge_hash_cap) {
        edge_hash_rebuild(hem, n);
    } else {
        for (int i = 0; i < n; i++)
            edge_hash_insert(hem, first_edge + i);
    }

    for (int i = 0; i < n; i++) {
        int edge_idx = first_edge + i;
        int a = vert_indices[i], b = vert_indices[(i + 1) % n];
        hem->verts[a].edge = edge_idx;
        /* Twin lookup: an existing edge running b->a is this edge's
         * opposite. Search only edges added before this face's own (the
         * ones just added above can't be a twin of each other within the
         * same face for a valid, non-self-adjacent mesh). */
        int twin = find_edge(hem, b, a);
        if (twin >= 0 && twin < first_edge) {
            hem->edges[edge_idx].twin = twin;
            hem->edges[twin].twin = edge_idx;
        }
    }

    hem->faces[face_idx].edge = first_edge;
    hem->faces[face_idx].count = n;
    hem->faces[face_idx].deleted = 0;
    /* Neutral dielectric/rough default -- close to this codebase's
     * pre-PBR flat-lit look (see renderer.c's lighting-pass comment on
     * why the shared, non-editable geometry's own material placeholder
     * was changed to match: metallic 0, roughness near 1 keeps a plain
     * diffuse response, no surprising bright specular out of nowhere). */
    hem->faces[face_idx].base_color[0] = 0.7f;
    hem->faces[face_idx].base_color[1] = 0.7f;
    hem->faces[face_idx].base_color[2] = 0.7f;
    hem->faces[face_idx].metallic  = 0.0f;
    hem->faces[face_idx].roughness = 0.8f;
    hem->faces[face_idx].emission[0] = 0.0f;
    hem->faces[face_idx].emission[1] = 0.0f;
    hem->faces[face_idx].emission[2] = 0.0f;
    hem->faces[face_idx].texture = 0;   /* no texture -- flat base_color only, see HEFace::texture's own comment */
    return face_idx;
}

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

void halfedge_set_face_material(HalfEdgeMesh *hem, int f, const float base_color[3],
                                 float metallic, float roughness, const float emission[3]) {
    if (f < 0 || f >= hem->face_count || hem->faces[f].deleted) return;
    HEFace *face = &hem->faces[f];
    face->base_color[0] = base_color[0];
    face->base_color[1] = base_color[1];
    face->base_color[2] = base_color[2];
    face->metallic  = clamp01(metallic);
    face->roughness = clamp01(roughness);
    face->emission[0] = emission[0];
    face->emission[1] = emission[1];
    face->emission[2] = emission[2];
}

void halfedge_set_face_texture(HalfEdgeMesh *hem, int f, unsigned int texture) {
    if (f < 0 || f >= hem->face_count || hem->faces[f].deleted) return;
    hem->faces[f].texture = texture;
}

void halfedge_delete_face(HalfEdgeMesh *hem, int f) {
    HEFace *face = &hem->faces[f];
    if (face->deleted) return;
    int e = face->edge;
    for (int i = 0; i < face->count; i++) {
        HEEdge *he = &hem->edges[e];
        if (he->twin >= 0) hem->edges[he->twin].twin = -1;
        e = he->next;
    }
    face->deleted = 1;
}

HalfEdgeMesh *halfedge_build_from_triangles(const float *positions, int pos_count,
                                             const unsigned short *indices, int index_count) {
    HalfEdgeMesh *hem = halfedge_create();
    for (int i = 0; i < pos_count; i++)
        halfedge_add_vertex(hem, positions[i*3+0], positions[i*3+1], positions[i*3+2]);
    for (int i = 0; i + 2 < index_count; i += 3) {
        int tri[3] = { indices[i], indices[i+1], indices[i+2] };
        halfedge_add_face(hem, tri, 3);
    }
    return hem;
}

void halfedge_face_verts(const HalfEdgeMesh *hem, int f, int *out_verts) {
    const HEFace *face = &hem->faces[f];
    int e = face->edge;
    for (int i = 0; i < face->count; i++) {
        out_verts[i] = hem->edges[e].origin;
        e = hem->edges[e].next;
    }
}

void halfedge_flatten_triangles(const HalfEdgeMesh *hem,
                                 float **out_positions, int *out_pos_count,
                                 unsigned short **out_indices, int *out_index_count) {
    *out_pos_count = hem->vert_count;
    *out_positions = (float *)malloc(sizeof(float) * 3 * (size_t)hem->vert_count);
    for (int i = 0; i < hem->vert_count; i++) {
        (*out_positions)[i*3+0] = hem->verts[i].pos[0];
        (*out_positions)[i*3+1] = hem->verts[i].pos[1];
        (*out_positions)[i*3+2] = hem->verts[i].pos[2];
    }

    /* Every LIVE face is a triangle in this pass (see halfedge.h) — 3
     * indices per face, no fan-triangulation of n-gons needed yet. Deleted
     * faces (soft-tombstoned by editing ops, see halfedge_delete_face) are
     * skipped, so live_count can be less than face_count. */
    int live_count = 0;
    for (int f = 0; f < hem->face_count; f++)
        if (!hem->faces[f].deleted) live_count++;

    *out_index_count = live_count * 3;
    *out_indices = (unsigned short *)malloc(sizeof(unsigned short) * (size_t)*out_index_count);
    int w = 0;
    for (int f = 0; f < hem->face_count; f++) {
        if (hem->faces[f].deleted) continue;
        int verts[3];
        halfedge_face_verts(hem, f, verts);
        (*out_indices)[w*3+0] = (unsigned short)verts[0];
        (*out_indices)[w*3+1] = (unsigned short)verts[1];
        (*out_indices)[w*3+2] = (unsigned short)verts[2];
        w++;
    }
}
