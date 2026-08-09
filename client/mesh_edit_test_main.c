/* Standalone, no-GL/no-window test harness for mesh_edit.c's extrude/
 * inset/loop-cut half-edge topology mutations — same spirit as
 * mp_test_main.c/mp_stress_test_main.c (a real, runnable check of one
 * subsystem in isolation, not a mock). Exists because this environment's
 * live-GUI verification path (X11 window + XTest + screenshot, used
 * throughout this session for the picking/gizmo work) turned out to be
 * unreliable specifically for this iteration's background-job context —
 * the native binary's window kept receiving a WM_DELETE_WINDOW
 * ClientMessage and exiting on its own after an inconsistent few seconds
 * to several minutes, reproducing even before any of this file's changes
 * were in place, so it's an environment issue rather than a regression to
 * chase down here. Topology correctness is fully checkable without any
 * rendering at all, so that's what this harness actually proves; see
 * phi.md for the honest note on what remains screenshot-unverified this
 * pass (the context-menu row wiring/click routing itself). */
#include "halfedge.h"
#include "halfedge_gltf.h"
#include "mesh_edit.h"
#include <stdio.h>
#include <math.h>

static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

/* Counts live (non-deleted) faces and boundary (twin==-1) half-edges among
 * live faces -- a watertight/closed mesh (like the test cube, and like any
 * mesh these ops should preserve closedness on) has zero boundary edges;
 * a nonzero count means an edit op left a hole or a dangling twin link. */
static void mesh_stats(const HalfEdgeMesh *hem, int *live_faces, int *boundary_edges) {
    *live_faces = 0;
    *boundary_edges = 0;
    for (int f = 0; f < hem->face_count; f++) {
        if (hem->faces[f].deleted) continue;
        (*live_faces)++;
        int e = hem->faces[f].edge;
        for (int i = 0; i < hem->faces[f].count; i++) {
            if (hem->edges[e].twin < 0) (*boundary_edges)++;
            e = hem->edges[e].next;
        }
    }
}

/* Every live face's every edge, if it has a twin, must point at a LIVE
 * face, and that twin's own twin must point back here -- catches the
 * exact "twinned against dead geometry" bug class find_edge's deleted-face
 * guard (halfedge.c) was added to prevent. */
static int check_twin_integrity(const HalfEdgeMesh *hem) {
    for (int f = 0; f < hem->face_count; f++) {
        if (hem->faces[f].deleted) continue;
        int e = hem->faces[f].edge;
        for (int i = 0; i < hem->faces[f].count; i++) {
            int t = hem->edges[e].twin;
            if (t >= 0) {
                if (hem->faces[hem->edges[t].face].deleted) return 0;
                if (hem->edges[t].twin != e) return 0;
            }
            e = hem->edges[e].next;
        }
    }
    return 1;
}

static float v3dist(const float *a, const float *b) {
    float dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

int main(void) {
    printf("[mesh_edit_test] loading assets/cube.gltf...\n");
    HalfEdgeMesh *base = halfedge_load_gltf("assets/cube.gltf");
    if (!base) { printf("FAIL: could not load assets/cube.gltf\n"); return 1; }

    int live0, bnd0;
    mesh_stats(base, &live0, &bnd0);
    printf("[mesh_edit_test] base cube: %d verts, %d live faces, %d boundary edges\n",
           base->vert_count, live0, bnd0);
    CHECK(base->vert_count == 8, "base cube has 8 shared vertices");
    CHECK(live0 == 12, "base cube has 12 live triangles");
    CHECK(bnd0 == 0, "base cube is watertight (no boundary edges)");
    halfedge_destroy(base);

    /* ---- Extrude ---- */
    printf("[mesh_edit_test] extrude face 0 by 4.0 along its normal...\n");
    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    float orig_verts_pos[3][3];
    int orig_verts[3];
    halfedge_face_verts(hem, 0, orig_verts);
    for (int i = 0; i < 3; i++)
        for (int a = 0; a < 3; a++) orig_verts_pos[i][a] = hem->verts[orig_verts[i]].pos[a];

    int cap = mesh_edit_extrude_face(hem, 0, 4.0f);
    CHECK(cap >= 0, "extrude returns a valid new cap face index");
    CHECK(hem->vert_count == 11, "extrude added exactly 3 new vertices (8 -> 11)");

    int live1, bnd1;
    mesh_stats(hem, &live1, &bnd1);
    printf("[mesh_edit_test] after extrude: %d live faces, %d boundary edges\n", live1, bnd1);
    CHECK(live1 == live0 - 1 + 1 + 6, "extrude: -1 base +1 cap +6 wall triangles (12 -> 18)");
    CHECK(bnd1 == 0, "mesh stays watertight after extrude (walls correctly reconnect)");
    CHECK(check_twin_integrity(hem), "no live edge twins a deleted face after extrude");

    if (cap >= 0) {
        int cap_verts[3];
        halfedge_face_verts(hem, cap, cap_verts);
        float max_delta_err = 0.0f;
        for (int i = 0; i < 3; i++) {
            /* cap_verts[i] should be one of the 3 new vertices, each at
             * distance ~4.0 from SOME original vertex of face 0 (order
             * within the loop is preserved by extrude_or_inset, so index i
             * lines up with orig_verts[i], but check distance-from-nearest
             * instead of assuming exact index correspondence, to stay
             * honest about what's actually guaranteed vs. implementation
             * detail). */
            float best = 1e9f;
            for (int k = 0; k < 3; k++) {
                float d = v3dist(hem->verts[cap_verts[i]].pos, orig_verts_pos[k]);
                if (d < best) best = d;
            }
            float err = fabsf(best - 4.0f);
            if (err > max_delta_err) max_delta_err = err;
        }
        printf("[mesh_edit_test] extrude offset error vs expected 4.0: %f\n", max_delta_err);
        CHECK(max_delta_err < 0.01f, "each new cap vertex sits ~4.0 units from its source vertex");
    }
    halfedge_destroy(hem);

    /* ---- Inset ---- */
    printf("[mesh_edit_test] inset face 0 by factor 0.4...\n");
    hem = halfedge_load_gltf("assets/cube.gltf");
    int inset_cap = mesh_edit_inset_face(hem, 0, 0.4f);
    CHECK(inset_cap >= 0, "inset returns a valid new face index");
    CHECK(hem->vert_count == 11, "inset added exactly 3 new vertices (8 -> 11)");
    int live2, bnd2;
    mesh_stats(hem, &live2, &bnd2);
    printf("[mesh_edit_test] after inset: %d live faces, %d boundary edges\n", live2, bnd2);
    CHECK(live2 == live0 - 1 + 1 + 6, "inset: -1 base +1 inset-cap +6 wall triangles (12 -> 18)");
    CHECK(bnd2 == 0, "mesh stays watertight after inset");
    CHECK(check_twin_integrity(hem), "no live edge twins a deleted face after inset");
    halfedge_destroy(hem);

    /* ---- Loop cut (single edge + its twin) ---- */
    printf("[mesh_edit_test] loop-cutting edge 0...\n");
    hem = halfedge_load_gltf("assets/cube.gltf");
    int had_twin = hem->edges[0].twin >= 0;
    int mv = mesh_edit_loop_cut_edge(hem, 0);
    CHECK(mv >= 0, "loop cut returns a valid new midpoint vertex index");
    CHECK(hem->vert_count == 9, "loop cut added exactly 1 new vertex (8 -> 9)");
    int live3, bnd3;
    mesh_stats(hem, &live3, &bnd3);
    printf("[mesh_edit_test] edge 0 had twin: %d; after cut: %d live faces, %d boundary edges\n",
           had_twin, live3, bnd3);
    CHECK(live3 == live0 + (had_twin ? 2 : 1), "loop cut nets +2 faces if the edge had a twin, else +1");
    CHECK(bnd3 == 0, "mesh stays watertight after loop cut");
    CHECK(check_twin_integrity(hem), "no live edge twins a deleted face after loop cut");
    halfedge_destroy(hem);

    /* ---- Defensive: invalid inputs fail cleanly, don't crash ---- */
    printf("[mesh_edit_test] defensive checks (bad inputs)...\n");
    hem = halfedge_load_gltf("assets/cube.gltf");
    CHECK(mesh_edit_extrude_face(hem, 999, 1.0f) == -1, "extrude on out-of-range face fails cleanly");
    CHECK(mesh_edit_inset_face(hem, -1, 0.5f) == -1, "inset on negative face index fails cleanly");
    CHECK(mesh_edit_loop_cut_edge(hem, 999) == -1, "loop cut on out-of-range edge fails cleanly");
    halfedge_delete_face(hem, 0);
    CHECK(mesh_edit_extrude_face(hem, 0, 1.0f) == -1, "extrude on an already-deleted face fails cleanly");
    halfedge_destroy(hem);

    if (g_fail) {
        printf("\n[mesh_edit_test] RESULT: FAIL\n");
        return 1;
    }
    printf("\n[mesh_edit_test] RESULT: PASS (all checks passed)\n");
    return 0;
}
