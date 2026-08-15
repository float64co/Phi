#include "mesh_edit.h"
#include <math.h>

/* ---- Vector helpers, matching meshobject.c/gizmo.c's own file-static
 * vec3_* convention (no shared header, see their own comments on why). ---- */
typedef struct { float x, y, z; } V3;
static inline V3 v3_sub(V3 a, V3 b) { return (V3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline V3 v3_cross(V3 a, V3 b) {
    return (V3){ a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}
static inline float v3_len(V3 v) { return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z); }
static inline V3 v3_from(const float *p) { return (V3){p[0], p[1], p[2]}; }

static V3 flat_face_normal(const HalfEdgeMesh *hem, const int *verts) {
    V3 a = v3_from(hem->verts[verts[0]].pos);
    V3 b = v3_from(hem->verts[verts[1]].pos);
    V3 c = v3_from(hem->verts[verts[2]].pos);
    V3 n = v3_cross(v3_sub(b, a), v3_sub(c, a));
    float len = v3_len(n);
    if (len > 1e-8f) { n.x /= len; n.y /= len; n.z /= len; }
    return n;
}

/* Shared mechanism for extrude/inset -- see mesh_edit.h's file comment for
 * the full rationale. new_pos[i] is the already-computed world/local-space
 * target position for original boundary vertex verts[i]; this function
 * owns everything else (adding the fresh vertices, deleting the original
 * face, adding the cap, stitching the walls). Returns the new cap face's
 * index. verts/new_pos must both have exactly `n` entries (n == 3 always
 * in this codebase's triangles-only model, but written generally since
 * nothing here actually depends on n==3). */
static int extrude_or_inset(HalfEdgeMesh *hem, int f, const int *verts, int n, const float new_pos[][3]) {
    /* n is always 3 in this codebase (triangles-only, see mesh_edit.h) --
     * a small fixed bound rather than alloca/a VLA, since neither is used
     * anywhere else in this codebase (avoids a portability question for
     * the win32/mingw cross-build target for no real benefit here). */
    int new_v[8];
    if (n > 8) return -1;
    for (int i = 0; i < n; i++)
        new_v[i] = halfedge_add_vertex(hem, new_pos[i][0], new_pos[i][1], new_pos[i][2]);

    /* Copy the source face's material before deleting it -- extruding or
     * insetting a red face should still look red, not silently revert to
     * halfedge_add_face's neutral default. Applied uniformly to the cap
     * and every wall triangle (a real editor might let walls diverge, but
     * uniform inheritance is the honest minimal behavior for this pass). */
    HEFace src = hem->faces[f];

    halfedge_delete_face(hem, f);

    int cap_face = halfedge_add_face(hem, new_v, n);
    halfedge_set_face_material(hem, cap_face, src.base_color, src.metallic, src.roughness, src.emission);

    for (int i = 0; i < n; i++) {
        int a = verts[i], b = verts[(i + 1) % n];
        int na = new_v[i], nb = new_v[(i + 1) % n];
        /* Side wall quad (a, b, nb, na), triangulated -- standard extrude/
         * inset wall winding: with the original loop CCW-from-outside and
         * new_pos displaced outward (extrude) or inward-coplanar (inset),
         * this triangle order keeps the wall's own outward normal facing
         * away from the solid, matching the cap and the untouched rest of
         * the mesh. See mesh_edit.h's file comment for why add_face's own
         * twin-finding is what reconnects these walls to any real neighbor
         * that bordered the original face. */
        int tri1[3] = { a, b, nb };
        int tri2[3] = { a, nb, na };
        int w1 = halfedge_add_face(hem, tri1, 3);
        int w2 = halfedge_add_face(hem, tri2, 3);
        halfedge_set_face_material(hem, w1, src.base_color, src.metallic, src.roughness, src.emission);
        halfedge_set_face_material(hem, w2, src.base_color, src.metallic, src.roughness, src.emission);
    }
    return cap_face;
}

int mesh_edit_extrude_face(HalfEdgeMesh *hem, int f, float dist) {
    if (f < 0 || f >= hem->face_count || hem->faces[f].deleted) return -1;
    if (hem->faces[f].count != 3) return -1;
    int verts[3];
    halfedge_face_verts(hem, f, verts);
    V3 n = flat_face_normal(hem, verts);

    float new_pos[3][3];
    for (int i = 0; i < 3; i++) {
        const float *p = hem->verts[verts[i]].pos;
        new_pos[i][0] = p[0] + n.x * dist;
        new_pos[i][1] = p[1] + n.y * dist;
        new_pos[i][2] = p[2] + n.z * dist;
    }
    return extrude_or_inset(hem, f, verts, 3, new_pos);
}

int mesh_edit_inset_face(HalfEdgeMesh *hem, int f, float factor) {
    if (f < 0 || f >= hem->face_count || hem->faces[f].deleted) return -1;
    if (hem->faces[f].count != 3) return -1;
    int verts[3];
    halfedge_face_verts(hem, f, verts);

    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (int i = 0; i < 3; i++) {
        const float *p = hem->verts[verts[i]].pos;
        cx += p[0]; cy += p[1]; cz += p[2];
    }
    cx /= 3.0f; cy /= 3.0f; cz /= 3.0f;

    float new_pos[3][3];
    for (int i = 0; i < 3; i++) {
        const float *p = hem->verts[verts[i]].pos;
        new_pos[i][0] = p[0] + (cx - p[0]) * factor;
        new_pos[i][1] = p[1] + (cy - p[1]) * factor;
        new_pos[i][2] = p[2] + (cz - p[2]) * factor;
    }
    return extrude_or_inset(hem, f, verts, 3, new_pos);
}

int mesh_edit_loop_cut_edge(HalfEdgeMesh *hem, int e) {
    if (e < 0 || e >= hem->edge_count) return -1;
    int f1 = hem->edges[e].face;
    if (f1 < 0 || hem->faces[f1].deleted || hem->faces[f1].count != 3) return -1;

    int a = hem->edges[e].origin;
    int b = hem->edges[hem->edges[e].next].origin;
    int c = hem->edges[hem->edges[e].prev].origin;
    int twin = hem->edges[e].twin;

    /* Only split across the twin's face too if it's a live triangle -- a
     * boundary edge (twin == -1) or a twin whose face isn't a plain
     * triangle just gets the one side split, matching this operation's
     * documented "triangles only" scope (see mesh_edit.h). */
    int f2 = -1, d = -1;
    if (twin >= 0) {
        int cand = hem->edges[twin].face;
        if (cand >= 0 && !hem->faces[cand].deleted && hem->faces[cand].count == 3) {
            f2 = cand;
            d = hem->edges[hem->edges[twin].prev].origin;
        }
    }

    const float *pa = hem->verts[a].pos;
    const float *pb = hem->verts[b].pos;
    int mv = halfedge_add_vertex(hem, (pa[0] + pb[0]) * 0.5f, (pa[1] + pb[1]) * 0.5f, (pa[2] + pb[2]) * 0.5f);

    /* Copy each source face's material before deleting it, same reasoning
     * as extrude_or_inset -- splitting a face shouldn't silently revert
     * its pieces to the default material. */
    HEFace src1 = hem->faces[f1];
    HEFace src2 = f2 >= 0 ? hem->faces[f2] : src1;

    halfedge_delete_face(hem, f1);
    if (f2 >= 0) halfedge_delete_face(hem, f2);

    /* f1's loop was (a, b, c) CCW; splitting edge a-b at mv fans from c. */
    int tri1[3] = { a, mv, c };
    int tri2[3] = { mv, b, c };
    int nf1 = halfedge_add_face(hem, tri1, 3);
    int nf2 = halfedge_add_face(hem, tri2, 3);
    halfedge_set_face_material(hem, nf1, src1.base_color, src1.metallic, src1.roughness, src1.emission);
    halfedge_set_face_material(hem, nf2, src1.base_color, src1.metallic, src1.roughness, src1.emission);

    if (f2 >= 0) {
        /* f2's loop was (b, a, d) CCW (twin runs the opposite direction of
         * e) -- splitting edge b-a at mv fans from d, same pattern. */
        int tri3[3] = { b, mv, d };
        int tri4[3] = { mv, a, d };
        int nf3 = halfedge_add_face(hem, tri3, 3);
        int nf4 = halfedge_add_face(hem, tri4, 3);
        halfedge_set_face_material(hem, nf3, src2.base_color, src2.metallic, src2.roughness, src2.emission);
        halfedge_set_face_material(hem, nf4, src2.base_color, src2.metallic, src2.roughness, src2.emission);
    }
    return mv;
}

int mesh_edit_flip_normals(HalfEdgeMesh *hem) {
    /* Snapshot the face count first -- faces added below (the re-added,
     * flipped replacements) must NOT themselves be visited again by this
     * same pass; append-only growth (see halfedge.h) means new faces land
     * past this bound. */
    int original_count = hem->face_count;
    int flipped = 0;
    for (int f = 0; f < original_count; f++) {
        if (hem->faces[f].deleted) continue;
        int n = hem->faces[f].count;
        /* n is always 3 in this codebase (triangles-only) -- same fixed
         * bound as extrude_or_inset's new_v[8] above, for the same reason. */
        int verts[8];
        if (n > 8) continue;
        halfedge_face_verts(hem, f, verts);
        for (int i = 0; i < n / 2; i++) {
            int tmp = verts[i]; verts[i] = verts[n - 1 - i]; verts[n - 1 - i] = tmp;
        }

        HEFace src = hem->faces[f];
        halfedge_delete_face(hem, f);
        int newf = halfedge_add_face(hem, verts, n);
        if (newf < 0) continue;
        halfedge_set_face_material(hem, newf, src.base_color, src.metallic, src.roughness, src.emission);
        flipped++;
    }
    return flipped;
}

int mesh_edit_nearest_edge_of_face(const HalfEdgeMesh *hem, int f, float hit_x, float hit_y, float hit_z) {
    if (f < 0 || f >= hem->face_count || hem->faces[f].deleted) return -1;
    V3 hit = { hit_x, hit_y, hit_z };
    int n = hem->faces[f].count;
    int e = hem->faces[f].edge;
    int best_edge = -1;
    float best_d2 = 1e30f;
    for (int i = 0; i < n; i++) {
        int v0 = hem->edges[e].origin;
        int v1 = hem->edges[hem->edges[e].next].origin;
        V3 p0 = v3_from(hem->verts[v0].pos);
        V3 p1 = v3_from(hem->verts[v1].pos);
        V3 mid = { (p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f, (p0.z + p1.z) * 0.5f };
        V3 d = v3_sub(hit, mid);
        float d2 = d.x*d.x + d.y*d.y + d.z*d.z;
        if (d2 < best_d2) { best_d2 = d2; best_edge = e; }
        e = hem->edges[e].next;
    }
    return best_edge;
}
