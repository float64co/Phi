#include "fracture.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct { float x, y, z; } V3;
static inline V3   v3_add(V3 a, V3 b) { return (V3){a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline V3   v3_sub(V3 a, V3 b) { return (V3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline V3   v3_scale(V3 a, float s) { return (V3){a.x*s, a.y*s, a.z*s}; }
static inline float v3_dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline V3   v3_cross(V3 a, V3 b) {
    return (V3){ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static inline float v3_len(V3 v) { return sqrtf(v3_dot(v, v)); }
static inline V3   v3_norm(V3 v) {
    float l = v3_len(v);
    return l > 1e-8f ? v3_scale(v, 1.0f / l) : (V3){0, 0, 1};
}
static inline V3   v3_lerp(V3 a, V3 b, float t) { return v3_add(a, v3_scale(v3_sub(b, a), t)); }

/* ---- Self-contained xorshift32 PRNG -- see fracture.h's own comment on
 * why this isn't libc rand_r (portability to the win32/mingw target). ---- */
static unsigned int xorshift32(unsigned int *state) {
    unsigned int x = *state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *state = x ? x : 1;  /* xorshift's one degenerate state is 0 -- never let it stick there */
    return *state;
}
static float rand01(unsigned int *state) {
    return (float)(xorshift32(state) & 0xFFFFFFu) / (float)0xFFFFFFu;
}

/* ---- Dynamic polygon/poly-mesh (working representation during clipping,
 * separate from HalfEdgeMesh -- clip planes can turn a triangle into an
 * n-gon, and this doesn't need twin/edge topology, just ordered vertex
 * loops per face, so a simpler structure is the honest fit rather than
 * forcing HalfEdgeMesh's triangles-only model to do double duty). ---- */
typedef struct { V3 *v; int count, cap; } Poly;
typedef struct { Poly *polys; int count, cap; } PMesh;

static void poly_push(Poly *p, V3 v) {
    if (p->count >= p->cap) {
        p->cap = p->cap ? p->cap * 2 : 4;
        p->v = (V3 *)realloc(p->v, (size_t)p->cap * sizeof(V3));
    }
    p->v[p->count++] = v;
}

static void pmesh_push(PMesh *m, Poly p) {
    if (m->count >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->polys = (Poly *)realloc(m->polys, (size_t)m->cap * sizeof(Poly));
    }
    m->polys[m->count++] = p;
}

static void pmesh_free(PMesh *m) {
    for (int i = 0; i < m->count; i++) free(m->polys[i].v);
    free(m->polys);
    memset(m, 0, sizeof(*m));
}

/* Builds the initial PMesh from obj's live half-edge triangles (local
 * space, matching hem's own vertex positions). */
static PMesh pmesh_from_halfedge(const HalfEdgeMesh *hem) {
    PMesh m = {0};
    for (int f = 0; f < hem->face_count; f++) {
        if (hem->faces[f].deleted) continue;
        int verts[3];
        halfedge_face_verts(hem, f, verts);
        Poly p = {0};
        for (int i = 0; i < 3; i++) {
            const float *pos = hem->verts[verts[i]].pos;
            poly_push(&p, (V3){pos[0], pos[1], pos[2]});
        }
        pmesh_push(&m, p);
    }
    return m;
}

static PMesh pmesh_clone(const PMesh *src) {
    PMesh out = {0};
    for (int i = 0; i < src->count; i++) {
        Poly p = {0};
        for (int k = 0; k < src->polys[i].count; k++) poly_push(&p, src->polys[i].v[k]);
        pmesh_push(&out, p);
    }
    return out;
}

/* Sutherland-Hodgman clip of a single (assumed-convex, CCW-from-outside)
 * polygon against the half-space "dot(p - plane_point, plane_normal) <=
 * EPS" (that side is kept). Any edge that crosses the plane contributes
 * its intersection point to *cuts (the shared cross-section collector for
 * this whole clip pass, see clip_pmesh_by_plane) in addition to the
 * clipped polygon itself, so the caller can build a cap afterward. */
#define CLIP_EPS 1e-5f
static void clip_polygon(const V3 *in, int n, V3 plane_point, V3 plane_normal,
                          Poly *out, PMesh *cuts_as_points /* reused as a flat point bag, see caller */) {
    if (n == 0) return;
    for (int i = 0; i < n; i++) {
        V3 a = in[i], b = in[(i + 1) % n];
        float da = v3_dot(v3_sub(a, plane_point), plane_normal);
        float db = v3_dot(v3_sub(b, plane_point), plane_normal);
        int a_in = da <= CLIP_EPS;
        int b_in = db <= CLIP_EPS;
        if (a_in) poly_push(out, a);
        if (a_in != b_in) {
            float denom = da - db;
            float t = fabsf(denom) > 1e-8f ? da / denom : 0.5f;
            V3 ip = v3_lerp(a, b, t);
            poly_push(out, ip);
            /* cuts_as_points is a PMesh with exactly one growing Poly at
             * index 0 -- see clip_pmesh_by_plane, which allocates it that
             * way purely as a convenient dynamic-V3-array reuse. */
            poly_push(&cuts_as_points->polys[0], ip);
        }
    }
}

/* Merges points within CLIP_EPS*4 of an already-kept point -- the same
 * physical intersection point gets computed twice (once per polygon
 * sharing the cut mesh edge), and small floating-point drift between the
 * two computations would otherwise produce near-duplicate loop vertices. */
static void dedup_points(Poly *pts) {
    Poly out = {0};
    for (int i = 0; i < pts->count; i++) {
        int dup = 0;
        for (int j = 0; j < out.count; j++) {
            if (v3_len(v3_sub(pts->v[i], out.v[j])) < CLIP_EPS * 4.0f) { dup = 1; break; }
        }
        if (!dup) poly_push(&out, pts->v[i]);
    }
    free(pts->v);
    *pts = out;
}

/* Sorts a coplanar, convex point set into a CCW-from-outside loop (see
 * this file's own header comment on the convex-source scope assumption --
 * a plane cutting a convex fragment always yields exactly one convex
 * cross-section) and appends it to `mesh` as the new cap polygon for this
 * clip. Desired outward normal is `plane_normal` (the direction from
 * "kept" toward "discarded" -- the cap must face that way to close the
 * solid correctly, same reasoning mesh_edit.c's wall-winding comment
 * uses). Picks an arbitrary right-handed in-plane basis (u,v) with
 * u×v == plane_normal, then sorts by angle in that basis -- a standard
 * "sort points around a convex polygon's centroid" technique. */
static void cap_from_cut_points(PMesh *mesh, Poly *pts, V3 plane_normal) {
    if (pts->count < 3) return;
    V3 centroid = {0, 0, 0};
    for (int i = 0; i < pts->count; i++) centroid = v3_add(centroid, pts->v[i]);
    centroid = v3_scale(centroid, 1.0f / (float)pts->count);

    V3 n = v3_norm(plane_normal);
    V3 arbitrary = fabsf(n.x) < 0.9f ? (V3){1, 0, 0} : (V3){0, 1, 0};
    V3 u = v3_norm(v3_cross(arbitrary, n));   /* perpendicular to n */
    /* u x v == -plane_normal, NOT +plane_normal -- this codebase's actual
     * face-winding convention (confirmed against assets/cube.gltf's real
     * index data: cross(v1-v0,v2-v0) points INWARD, opposite the textbook
     * "CCW-from-outside" assumption this function's own comment above
     * originally stated) means a loop sorted CCW-as-seen-from-N has its
     * standard cross(edge1,edge2) normal pointing INTO the solid, not out
     * of it -- so to make the cap's actual outward face point along
     * plane_normal, the sort basis needs u x v == -plane_normal instead.
     * Caught by fracture_test's divergence-theorem closedness/volume
     * checks (mixed winding between original faces and caps showed up as
     * exactly the closedness error and sign-flipped volume those checks
     * exist to catch), not by inspection -- see fracture_test_main.c. */
    V3 v = v3_cross(u, n);

    /* Simple O(n^2) insertion sort by angle -- cut loops are small
     * (bounded by how many original polygon edges a single plane can
     * cross), no need for a general-purpose sort here. */
    int n_pts = pts->count;
    float *ang = (float *)malloc(sizeof(float) * (size_t)n_pts);
    for (int i = 0; i < n_pts; i++) {
        V3 d = v3_sub(pts->v[i], centroid);
        ang[i] = atan2f(v3_dot(d, v), v3_dot(d, u));
    }
    for (int i = 1; i < n_pts; i++) {
        float a = ang[i]; V3 p = pts->v[i];
        int j = i - 1;
        while (j >= 0 && ang[j] > a) { ang[j+1] = ang[j]; pts->v[j+1] = pts->v[j]; j--; }
        ang[j+1] = a; pts->v[j+1] = p;
    }
    free(ang);

    Poly cap = {0};
    for (int i = 0; i < n_pts; i++) poly_push(&cap, pts->v[i]);
    pmesh_push(mesh, cap);
}

/* Clips every polygon in `mesh` (in place, replacing it) against the
 * half-space "dot(p-plane_point,plane_normal) <= EPS", dropping polygons
 * that clip away to nothing, and caps the resulting cross-section (if
 * this plane actually cut through the fragment) with a fresh polygon so
 * the fragment stays a closed solid. */
static void clip_pmesh_by_plane(PMesh *mesh, V3 plane_point, V3 plane_normal) {
    PMesh out = {0};
    PMesh cuts_holder = {0};
    Poly cuts = {0};
    pmesh_push(&cuts_holder, cuts);  /* index 0 is the shared point bag clip_polygon appends to */

    for (int i = 0; i < mesh->count; i++) {
        Poly clipped = {0};
        clip_polygon(mesh->polys[i].v, mesh->polys[i].count, plane_point, plane_normal, &clipped, &cuts_holder);
        if (clipped.count >= 3) {
            pmesh_push(&out, clipped);
        } else {
            free(clipped.v);
        }
    }

    dedup_points(&cuts_holder.polys[0]);
    cap_from_cut_points(&out, &cuts_holder.polys[0], plane_normal);
    free(cuts_holder.polys[0].v);
    free(cuts_holder.polys);

    pmesh_free(mesh);
    *mesh = out;
}

static void pmesh_bounds(const PMesh *m, V3 *out_min, V3 *out_max) {
    V3 mn = {1e30f, 1e30f, 1e30f}, mx = {-1e30f, -1e30f, -1e30f};
    for (int i = 0; i < m->count; i++) {
        for (int k = 0; k < m->polys[i].count; k++) {
            V3 p = m->polys[i].v[k];
            if (p.x < mn.x) mn.x = p.x;
            if (p.y < mn.y) mn.y = p.y;
            if (p.z < mn.z) mn.z = p.z;
            if (p.x > mx.x) mx.x = p.x;
            if (p.y > mx.y) mx.y = p.y;
            if (p.z > mx.z) mx.z = p.z;
        }
    }
    *out_min = mn; *out_max = mx;
}

/* Fan-triangulates every (convex, by construction) polygon in `mesh` into
 * a flat, non-indexed-but-trivially-indexed (0,1,2,3,...) triangle list --
 * no vertex welding/deduplication in this pass (see fracture.h). */
static void pmesh_flatten(const PMesh *mesh, FractureFragment *out) {
    int tri_count = 0;
    for (int i = 0; i < mesh->count; i++)
        if (mesh->polys[i].count >= 3) tri_count += mesh->polys[i].count - 2;

    if (tri_count == 0) { out->positions = NULL; out->pos_count = 0; out->indices = NULL; out->index_count = 0; return; }

    out->pos_count = tri_count * 3;
    out->positions = (float *)malloc(sizeof(float) * 3 * (size_t)out->pos_count);
    out->index_count = tri_count * 3;
    out->indices = (unsigned short *)malloc(sizeof(unsigned short) * (size_t)out->index_count);

    int w = 0;
    for (int i = 0; i < mesh->count; i++) {
        const Poly *p = &mesh->polys[i];
        if (p->count < 3) continue;
        for (int k = 1; k + 1 < p->count; k++) {
            V3 tri[3] = { p->v[0], p->v[k], p->v[k+1] };
            for (int c = 0; c < 3; c++) {
                out->positions[w*3+0] = tri[c].x;
                out->positions[w*3+1] = tri[c].y;
                out->positions[w*3+2] = tri[c].z;
                out->indices[w] = (unsigned short)w;
                w++;
            }
        }
    }
}

FractureFragment *fracture_voronoi(const MeshObject *obj, int n_fragments, unsigned int seed) {
    if (!obj->hem || n_fragments < 1) return NULL;

    PMesh base = pmesh_from_halfedge(obj->hem);
    V3 bmin, bmax;
    pmesh_bounds(&base, &bmin, &bmax);

    V3 *seeds = (V3 *)malloc(sizeof(V3) * (size_t)n_fragments);
    unsigned int rng = seed ? seed : 1;
    for (int i = 0; i < n_fragments; i++) {
        seeds[i] = (V3){
            bmin.x + rand01(&rng) * (bmax.x - bmin.x),
            bmin.y + rand01(&rng) * (bmax.y - bmin.y),
            bmin.z + rand01(&rng) * (bmax.z - bmin.z),
        };
    }

    FractureFragment *frags = (FractureFragment *)calloc((size_t)n_fragments, sizeof(FractureFragment));
    for (int i = 0; i < n_fragments; i++) {
        PMesh frag = pmesh_clone(&base);
        for (int j = 0; j < n_fragments && frag.count > 0; j++) {
            if (j == i) continue;
            V3 mid = v3_scale(v3_add(seeds[i], seeds[j]), 0.5f);
            V3 dir = v3_sub(seeds[j], seeds[i]);
            if (v3_len(dir) < 1e-6f) continue;   /* coincident seeds -- degenerate bisector, skip rather than divide by ~0 */
            V3 n = v3_norm(dir);
            clip_pmesh_by_plane(&frag, mid, n);
        }
        pmesh_flatten(&frag, &frags[i]);
        pmesh_free(&frag);
    }

    pmesh_free(&base);
    free(seeds);
    return frags;
}

void fracture_free_fragments(FractureFragment *frags, int n) {
    if (!frags) return;
    for (int i = 0; i < n; i++) {
        free(frags[i].positions);
        free(frags[i].indices);
    }
    free(frags);
}

static char *bin_path_for(const char *gltf_path) {
    const char *dot = strrchr(gltf_path, '.');
    const char *slash = strrchr(gltf_path, '/');
    size_t base_len = (dot && dot > slash) ? (size_t)(dot - gltf_path) : strlen(gltf_path);
    char *out = (char *)malloc(base_len + 5);
    memcpy(out, gltf_path, base_len);
    memcpy(out + base_len, ".bin", 5);
    return out;
}

int fracture_save_glb(const FractureFragment *frags, int n, const char *gltf_path) {
    /* Only non-empty fragments get written (see fracture.h) -- build the
     * index list up front so byteOffsets/JSON indices are computed
     * against what's actually going to be written. */
    int *live = (int *)malloc(sizeof(int) * (size_t)n);
    int live_n = 0;
    for (int i = 0; i < n; i++) if (frags[i].pos_count > 0) live[live_n++] = i;
    if (live_n == 0) { free(live); printf("[fracture] no non-empty fragments to save\n"); return 0; }

    char *bin_path = bin_path_for(gltf_path);
    const char *bin_basename = strrchr(bin_path, '/');
    bin_basename = bin_basename ? bin_basename + 1 : bin_path;

    FILE *bf = fopen(bin_path, "wb");
    if (!bf) { printf("[fracture] failed to open %s for writing\n", bin_path); free(bin_path); free(live); return 0; }

    /* One bufferView pair (pos, indices) per fragment, concatenated in
     * order -- same single-buffer-multiple-views shape halfedge_gltf.c's
     * halfedge_save_gltf already uses, just repeated N times. */
    size_t *pos_off = (size_t *)malloc(sizeof(size_t) * (size_t)live_n);
    size_t *idx_off = (size_t *)malloc(sizeof(size_t) * (size_t)live_n);
    size_t cursor = 0;
    for (int k = 0; k < live_n; k++) {
        const FractureFragment *f = &frags[live[k]];
        pos_off[k] = cursor;
        size_t pos_bytes = sizeof(float) * 3 * (size_t)f->pos_count;
        fwrite(f->positions, 1, pos_bytes, bf);
        cursor += pos_bytes;
        idx_off[k] = cursor;
        size_t idx_bytes = sizeof(unsigned short) * (size_t)f->index_count;
        fwrite(f->indices, 1, idx_bytes, bf);
        cursor += idx_bytes;
    }
    fclose(bf);

    FILE *gf = fopen(gltf_path, "w");
    if (!gf) {
        printf("[fracture] failed to open %s for writing\n", gltf_path);
        free(bin_path); free(live); free(pos_off); free(idx_off);
        return 0;
    }

    fprintf(gf, "{\n  \"asset\": { \"version\": \"2.0\", \"generator\": \"phi fracture_save_glb\" },\n");
    fprintf(gf, "  \"buffers\": [ { \"uri\": \"%s\", \"byteLength\": %zu } ],\n", bin_basename, cursor);

    fprintf(gf, "  \"bufferViews\": [\n");
    for (int k = 0; k < live_n; k++) {
        const FractureFragment *f = &frags[live[k]];
        fprintf(gf, "    { \"buffer\": 0, \"byteOffset\": %zu, \"byteLength\": %zu, \"target\": 34962 },\n",
                pos_off[k], sizeof(float) * 3 * (size_t)f->pos_count);
        fprintf(gf, "    { \"buffer\": 0, \"byteOffset\": %zu, \"byteLength\": %zu, \"target\": 34963 }%s\n",
                idx_off[k], sizeof(unsigned short) * (size_t)f->index_count, k + 1 < live_n ? "," : "");
    }
    fprintf(gf, "  ],\n");

    fprintf(gf, "  \"accessors\": [\n");
    for (int k = 0; k < live_n; k++) {
        const FractureFragment *f = &frags[live[k]];
        float pmin[3], pmax[3];
        pmin[0] = pmax[0] = f->positions[0]; pmin[1] = pmax[1] = f->positions[1]; pmin[2] = pmax[2] = f->positions[2];
        for (int i = 1; i < f->pos_count; i++)
            for (int a = 0; a < 3; a++) {
                float v = f->positions[i*3+a];
                if (v < pmin[a]) pmin[a] = v;
                if (v > pmax[a]) pmax[a] = v;
            }
        fprintf(gf, "    { \"bufferView\": %d, \"byteOffset\": 0, \"componentType\": 5126, \"count\": %d, "
                    "\"type\": \"VEC3\", \"min\": [%g,%g,%g], \"max\": [%g,%g,%g] },\n",
                k*2, f->pos_count, pmin[0], pmin[1], pmin[2], pmax[0], pmax[1], pmax[2]);
        fprintf(gf, "    { \"bufferView\": %d, \"byteOffset\": 0, \"componentType\": 5123, \"count\": %d, \"type\": \"SCALAR\" }%s\n",
                k*2+1, f->index_count, k + 1 < live_n ? "," : "");
    }
    fprintf(gf, "  ],\n");

    fprintf(gf, "  \"meshes\": [\n");
    for (int k = 0; k < live_n; k++)
        fprintf(gf, "    { \"primitives\": [ { \"attributes\": { \"POSITION\": %d }, \"indices\": %d, \"mode\": 4 } ] }%s\n",
                k*2, k*2+1, k + 1 < live_n ? "," : "");
    fprintf(gf, "  ],\n");

    fprintf(gf, "  \"nodes\": [\n");
    for (int k = 0; k < live_n; k++)
        fprintf(gf, "    { \"mesh\": %d }%s\n", k, k + 1 < live_n ? "," : "");
    fprintf(gf, "  ],\n");

    fprintf(gf, "  \"scenes\": [ { \"nodes\": [");
    for (int k = 0; k < live_n; k++) fprintf(gf, "%d%s", k, k + 1 < live_n ? "," : "");
    fprintf(gf, "] } ],\n  \"scene\": 0,\n");

    /* PHI_fracture_fragments: a namespaced extension pointing at the mesh
     * indices that make up this fracture group -- see fracture.h's own
     * comment. Anything that doesn't understand this extension still sees
     * N perfectly ordinary meshes/nodes/primitives. */
    fprintf(gf, "  \"extensionsUsed\": [ \"PHI_fracture_fragments\" ],\n");
    fprintf(gf, "  \"extensions\": { \"PHI_fracture_fragments\": { \"fragment_count\": %d, \"fragments\": [", live_n);
    for (int k = 0; k < live_n; k++) fprintf(gf, "%d%s", k, k + 1 < live_n ? "," : "");
    fprintf(gf, "] } }\n}\n");

    fclose(gf);
    free(bin_path); free(live); free(pos_off); free(idx_off);
    return 1;
}

/* True if fragments a and b share at least one vertex position (within
 * EPS) -- see fracture_compute_adjacency's own comment for why that's
 * exactly the right test. */
static int fragments_share_a_vertex(const FractureFragment *a, const FractureFragment *b) {
    const float EPS2 = 1e-4f * 1e-4f;
    for (int i = 0; i < a->pos_count; i++) {
        float ax = a->positions[i*3+0], ay = a->positions[i*3+1], az = a->positions[i*3+2];
        for (int j = 0; j < b->pos_count; j++) {
            float dx = ax - b->positions[j*3+0];
            float dy = ay - b->positions[j*3+1];
            float dz = az - b->positions[j*3+2];
            if (dx*dx + dy*dy + dz*dz < EPS2) return 1;
        }
    }
    return 0;
}

void fracture_compute_adjacency(const FractureFragment *frags, int n, unsigned char *out_adjacent) {
    if (n < 1) return;
    memset(out_adjacent, 0, (size_t)n * (size_t)n);
    for (int i = 0; i < n; i++) {
        if (frags[i].pos_count == 0) continue;
        for (int j = i + 1; j < n; j++) {
            if (frags[j].pos_count == 0) continue;
            if (fragments_share_a_vertex(&frags[i], &frags[j])) {
                out_adjacent[i*n + j] = 1;
                out_adjacent[j*n + i] = 1;
            }
        }
    }
}
