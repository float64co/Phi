#pragma once
#include "meshobject.h"

/* Voronoi pre-fracture tool (Phase 1, phi.md's "Fracturing" section) --
 * editor-only precompute. Explicitly NOT runtime fracture-on-impact (phi.md
 * already defers that -- "pre-fracturing covers the vast majority of game
 * use cases" -- and it needs Bullet, which is Phase 2's scope, out of
 * bounds here). No Bullet involvement anywhere in this file.
 *
 * Algorithm: standard mesh-vs-convex-region clipping. For N random seed
 * points within the source mesh's own local-space bounding box, fragment
 * i is the source mesh clipped against the intersection of the (N-1)
 * half-spaces "closer to seed i than seed j" for every other seed j (the
 * perpendicular bisector plane between i and j). Each plane clip uses
 * Sutherland-Hodgman polygon clipping per face, and caps the newly-exposed
 * cross-section with a fresh convex polygon so every fragment stays a
 * closed, watertight solid.
 *
 * SCOPE: this implementation assumes the source mesh is itself convex
 * (true for this phase's cube.gltf test asset). A plane intersects a
 * convex polyhedron in at most one convex cross-section, which is what
 * makes "collect the cut points, sort them into one loop, cap it" correct
 * and simple. A non-convex source could produce a cross-section with
 * multiple disconnected loops (or a non-convex one), which this pass does
 * not attempt to handle -- flagged honestly as a real scope boundary
 * (matching mesh_edit.c's loop-cut's own "single edge, not a full ring"
 * scoping decision), not silently wrong output. */

typedef struct {
    float          *positions;   /* flat xyz, malloc'd, pos_count vec3s */
    int             pos_count;
    unsigned short  *indices;    /* flat triangle-list, malloc'd, index_count entries */
    int             index_count;
} FractureFragment;

/* Computes n_fragments Voronoi-cell fragments of obj's current live
 * half-edge mesh (obj->hem), in the SAME local/object space obj->hem's own
 * vertex positions already live in (not world space -- matches
 * halfedge_save_gltf's existing local-space export convention). Returns a
 * malloc'd array of n_fragments FractureFragments (caller frees via
 * fracture_free_fragments), or NULL if obj->hem is NULL or n_fragments < 1.
 * A fragment can legitimately come out empty (pos_count==0) if its
 * Voronoi cell doesn't intersect the source mesh at all -- an expected,
 * not-erroneous outcome for some random seed placements, not a bug.
 * `seed` drives a self-contained xorshift32 PRNG (not libc rand_r, which
 * isn't portable to the win32/mingw cross-build target) -- pass the same
 * seed for reproducible/testable output, or a real seed value for actual
 * varied seed placement. */
FractureFragment *fracture_voronoi(const MeshObject *obj, int n_fragments, unsigned int seed);

void fracture_free_fragments(FractureFragment *frags, int n);

/* Saves fragments as a PHI_fracture_fragments glTF extension on a hand-
 * rolled minimal .gltf + sibling .bin (same style as halfedge_gltf.c's
 * halfedge_save_gltf, since cgltf itself is read-only, see its own
 * comment) -- each fragment becomes an ordinary glTF mesh/node (position-
 * only, unsigned-short-indexed, mode 4/triangles), not a duplicated
 * separate file; "extensions"/"PHI_fracture_fragments"/"fragments" lists
 * their mesh indices so a PHI-aware consumer can find them as a group
 * while any ordinary glTF viewer just sees N normal meshes. Empty
 * fragments (pos_count==0) are skipped, not written as degenerate zero-
 * vertex meshes. Returns 1 on success, 0 on failure (e.g. couldn't open
 * either output file). */
int fracture_save_glb(const FractureFragment *frags, int n, const char *gltf_path);

/* Computes which fragment pairs are adjacent -- i.e. share a Voronoi
 * bisector-plane cut face -- into a caller-owned n*n row-major matrix
 * (out_adjacent[i*n+j], symmetric, diagonal always 0). Detected by
 * checking whether two fragments have any vertex position in common
 * (within a small epsilon): since every fragment is clipped from the
 * SAME source mesh, the only way two fragments can share any geometry at
 * all is along the exact bisector plane that was clipped between them --
 * that cut face is the identical physical surface on both sides, so its
 * vertices coincide exactly (up to floating-point clip error) in both
 * fragments' own local space. A practical, geometry-based adjacency test
 * rather than threading full Voronoi-cell neighbor bookkeeping through
 * fracture_voronoi's clipping loop -- O(pos_count_i * pos_count_j) per
 * pair, fine at this phase's fragment counts (single digits to low
 * tens), not attempted at a scale where that matters, same "real but
 * scoped" bar this file's other comments already hold themselves to.
 * Intended consumer: connecting adjacent fragments with a real Bullet
 * breaking-threshold constraint at runtime (see phi_physics.h's
 * phi_physics_add_fixed_constraint) so a fractured object still reads as
 * one solid piece until an impact separates it. No-op (out_adjacent left
 * untouched) if n < 1. */
void fracture_compute_adjacency(const FractureFragment *frags, int n, unsigned char *out_adjacent);
