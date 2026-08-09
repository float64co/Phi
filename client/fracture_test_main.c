/* Standalone, no-GL/no-window test harness for fracture.c's Voronoi
 * pre-fracture tool -- same rationale as mesh_edit_test_main.c (this
 * session's X11 environment has been unreliable-to-fully-unresponsive,
 * see phi.md; topology/geometry correctness doesn't need a GL context to
 * verify at all).
 *
 * The two real checks this relies on are both standard divergence-theorem
 * identities for a closed triangle mesh, not approximations:
 *   - enclosed volume = (1/6) * sum over triangles of v0 . (v1 x v2)
 *     (origin-relative signed tetrahedra -- works for ANY closed mesh
 *     with consistent outward/CCW winding, not just convex ones)
 *   - "closedness": sum over triangles of (area * unit_normal) must be
 *     the zero vector for a truly watertight closed mesh, regardless of
 *     shape -- a nonzero sum means there's a hole or a winding error.
 * Both are exact identities, not heuristics -- if these numbers come out
 * right, the fragments really are the closed solids they're supposed to
 * be, not just "looked plausible". */
#include "halfedge.h"
#include "halfedge_gltf.h"
#include "fracture.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); } \
    else      { printf("  FAIL: %s\n", msg); g_fail = 1; } \
} while (0)

typedef struct { float x, y, z; } V3;
static V3 v3_sub(V3 a, V3 b) { return (V3){a.x-b.x, a.y-b.y, a.z-b.z}; }
static V3 v3_cross(V3 a, V3 b) { return (V3){a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static float v3_dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float v3_len(V3 v) { return sqrtf(v3_dot(v, v)); }

/* Standard divergence-theorem volume formula, NEGATED: this codebase's
 * actual face-winding convention (confirmed against assets/cube.gltf's
 * real index data, see fracture.c's cap_from_cut_points comment) has
 * cross(v1-v0,v2-v0) point INWARD, opposite the textbook "CCW-from-
 * outside" assumption the un-negated formula expects -- without this
 * negation every correctly-formed closed mesh in this codebase reports a
 * negative volume, which is confusing to read even though internally
 * consistent. */
static float fragment_volume(const FractureFragment *f) {
    double vol = 0.0;
    for (int t = 0; t < f->index_count / 3; t++) {
        V3 v0 = { f->positions[f->indices[t*3+0]*3+0], f->positions[f->indices[t*3+0]*3+1], f->positions[f->indices[t*3+0]*3+2] };
        V3 v1 = { f->positions[f->indices[t*3+1]*3+0], f->positions[f->indices[t*3+1]*3+1], f->positions[f->indices[t*3+1]*3+2] };
        V3 v2 = { f->positions[f->indices[t*3+2]*3+0], f->positions[f->indices[t*3+2]*3+1], f->positions[f->indices[t*3+2]*3+2] };
        vol += (double)v3_dot(v0, v3_cross(v1, v2));
    }
    return (float)(-vol / 6.0);
}

/* Magnitude of sum(area * unit_normal) -- should be ~0 for a closed mesh,
 * see file comment. Normalized by total surface area so the threshold
 * this gets checked against is scale-independent. */
static float fragment_closedness_error(const FractureFragment *f) {
    V3 sum = {0, 0, 0};
    double total_area = 0.0;
    for (int t = 0; t < f->index_count / 3; t++) {
        V3 v0 = { f->positions[f->indices[t*3+0]*3+0], f->positions[f->indices[t*3+0]*3+1], f->positions[f->indices[t*3+0]*3+2] };
        V3 v1 = { f->positions[f->indices[t*3+1]*3+0], f->positions[f->indices[t*3+1]*3+1], f->positions[f->indices[t*3+1]*3+2] };
        V3 v2 = { f->positions[f->indices[t*3+2]*3+0], f->positions[f->indices[t*3+2]*3+1], f->positions[f->indices[t*3+2]*3+2] };
        V3 cr = v3_cross(v3_sub(v1, v0), v3_sub(v2, v0));   /* magnitude = 2*area, direction = normal */
        sum.x += cr.x; sum.y += cr.y; sum.z += cr.z;
        total_area += v3_len(cr) * 0.5;
    }
    if (total_area < 1e-8) return 0.0f;
    return v3_len(sum) / (float)(total_area * 2.0);
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, needle) != NULL;
}

int main(void) {
    printf("[fracture_test] loading assets/cube.gltf...\n");
    HalfEdgeMesh *hem = halfedge_load_gltf("assets/cube.gltf");
    if (!hem) { printf("FAIL: could not load assets/cube.gltf\n"); return 1; }

    /* MeshObject is a plain, non-opaque struct -- fracture_voronoi only
     * ever reads obj->hem, so a zero-initialized stand-in with just that
     * field set is a real, correct MeshObject for this purpose, not a
     * mock (no meshobject.c function is ever called by fracture.c, see
     * fracture.c's own includes -- confirmed by this test harness linking
     * clean without meshobject.c at all). */
    MeshObject obj = {0};
    obj.hem = hem;

    /* Sanity check the volume formula itself against a mesh whose volume
     * is known by construction: cube.gltf is a unit cube (half-extent
     * 0.5), so its enclosed volume must be 1.0*1.0*1.0 = 1.0. */
    FractureFragment whole;
    whole.pos_count = hem->vert_count;
    whole.positions = (float *)malloc(sizeof(float) * 3 * (size_t)hem->vert_count);
    for (int i = 0; i < hem->vert_count; i++)
        for (int a = 0; a < 3; a++) whole.positions[i*3+a] = hem->verts[i].pos[a];
    int live_tris = 0;
    for (int f = 0; f < hem->face_count; f++) if (!hem->faces[f].deleted) live_tris++;
    whole.index_count = live_tris * 3;
    whole.indices = (unsigned short *)malloc(sizeof(unsigned short) * (size_t)whole.index_count);
    int w = 0;
    for (int f = 0; f < hem->face_count; f++) {
        if (hem->faces[f].deleted) continue;
        int verts[3]; halfedge_face_verts(hem, f, verts);
        whole.indices[w++] = (unsigned short)verts[0];
        whole.indices[w++] = (unsigned short)verts[1];
        whole.indices[w++] = (unsigned short)verts[2];
    }
    float orig_volume = fragment_volume(&whole);
    float orig_closed_err = fragment_closedness_error(&whole);
    printf("[fracture_test] original cube volume = %f (expected 1.0), closedness error = %f\n",
           orig_volume, orig_closed_err);
    CHECK(fabsf(orig_volume - 1.0f) < 0.01f, "volume formula gives 1.0 for the known unit cube (sanity check)");
    CHECK(orig_closed_err < 0.01f, "the unloaded cube itself is watertight (sanity check on the check itself)");
    free(whole.positions); free(whole.indices);

    /* ---- Defensive: invalid inputs ---- */
    printf("[fracture_test] defensive checks...\n");
    CHECK(fracture_voronoi(&obj, 0, 42) == NULL, "n_fragments < 1 returns NULL");
    MeshObject empty_obj = {0};
    CHECK(fracture_voronoi(&empty_obj, 4, 42) == NULL, "NULL hem returns NULL");

    /* ---- Real fracture runs at a few fragment counts, fixed seed for
     * reproducibility ---- */
    for (int n = 2; n <= 8; n *= 2) {
        printf("[fracture_test] fracturing into %d pieces (seed=42)...\n", n);
        FractureFragment *frags = fracture_voronoi(&obj, n, 42u);
        CHECK(frags != NULL, "fracture_voronoi returns a non-NULL fragment array");
        if (!frags) continue;

        double total_volume = 0.0;
        int non_empty = 0;
        int all_closed = 1, all_non_negative = 1;
        for (int i = 0; i < n; i++) {
            if (frags[i].pos_count == 0) continue;
            non_empty++;
            float vol = fragment_volume(&frags[i]);
            float cerr = fragment_closedness_error(&frags[i]);
            total_volume += vol;
            if (cerr > 0.02f) { all_closed = 0; printf("    fragment %d closedness error = %f (too high)\n", i, cerr); }
            if (vol < -0.001f) { all_non_negative = 0; printf("    fragment %d volume = %f (negative)\n", i, vol); }
        }
        printf("[fracture_test] n=%d: %d/%d non-empty fragments, total volume = %f (original %f)\n",
               n, non_empty, n, total_volume, orig_volume);
        CHECK(non_empty > 0, "at least one fragment is non-empty");
        CHECK(all_closed, "every non-empty fragment is watertight (divergence-theorem closedness check)");
        CHECK(all_non_negative, "every fragment's enclosed volume is non-negative");
        CHECK(fabsf((float)total_volume - orig_volume) < orig_volume * 0.02f,
              "fragment volumes sum to the original mesh's volume within 2% (no volume gained or lost)");

        if (n == 4) {
            /* Exercise the glTF export path once, at a representative
             * fragment count -- real file I/O, not just in-memory checks. */
            int ok = fracture_save_glb(frags, n, "/tmp/phi_fracture_test.gltf");
            CHECK(ok, "fracture_save_glb reports success");
            CHECK(file_contains("/tmp/phi_fracture_test.gltf", "PHI_fracture_fragments"),
                  "saved .gltf actually contains the PHI_fracture_fragments extension");
            CHECK(file_contains("/tmp/phi_fracture_test.gltf", "\"mode\": 4"),
                  "saved .gltf uses ordinary triangle-list mesh primitives");
        }

        fracture_free_fragments(frags, n);
    }

    halfedge_destroy(hem);

    if (g_fail) { printf("\n[fracture_test] RESULT: FAIL\n"); return 1; }
    printf("\n[fracture_test] RESULT: PASS (all checks passed)\n");
    return 0;
}
