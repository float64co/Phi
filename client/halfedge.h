#pragma once

/* Half-edge (winged-edge) editable mesh structure — Phase 1 foundation
 * (see phi.md's "Editable mesh structure vs. glTF" section). Standard
 * textbook formulation, the same family Blender/most mesh editors use
 * internally: not invented here, just implemented. glTF is only ever
 * touched at the load/save boundary (halfedge_load_gltf/halfedge_save_gltf
 * in halfedge_gltf.c) — this structure is the live in-memory
 * representation editor operations would mutate.
 *
 * Scope of this first pass: enough to build from and flatten back to flat
 * triangle buffers, plus the basic face/vertex traversal that later
 * editing operations (extrude, inset, loop cut — NOT implemented yet, out
 * of scope for this phase's foundation slice) would walk. Triangles only
 * for now (every face this structure builds from cgltf's triangle-list
 * data has exactly 3 vertices) — n-gon faces aren't exercised yet, though
 * the data structure itself doesn't assume a fixed face size.
 *
 * Twin-edge lookup (used when adding a face, to link each new half-edge
 * to its already-existing opposite if one exists) is a linear scan over
 * existing edges — O(n) per add, O(n^2) overall for a full mesh. Fine for
 * this phase's small test assets; a real editor-scale mesh would want a
 * hash map keyed on (origin,dest) instead. Not built here — flagged
 * honestly rather than silently left slow. */

typedef struct {
    float pos[3];
    int   edge;   /* index of one outgoing half-edge from this vertex, -1 if isolated */
} HEVertex;

typedef struct {
    int origin;   /* vertex index this half-edge starts at */
    int twin;     /* opposite half-edge index, -1 if no twin (open boundary) */
    int next;     /* next half-edge around the same face */
    int prev;     /* previous half-edge around the same face */
    int face;     /* face index this half-edge borders */
} HEEdge;

typedef struct {
    int edge;     /* one half-edge bordering this face */
    int count;    /* number of vertices/edges in this face's loop */
} HEFace;

typedef struct {
    HEVertex *verts; int vert_count, vert_cap;
    HEEdge   *edges; int edge_count, edge_cap;
    HEFace   *faces; int face_count, face_cap;
} HalfEdgeMesh;

HalfEdgeMesh *halfedge_create(void);
void          halfedge_destroy(HalfEdgeMesh *hem);

/* Adds a vertex, returns its index. */
int halfedge_add_vertex(HalfEdgeMesh *hem, float x, float y, float z);

/* Adds a face from a CCW-wound loop of `n` existing vertex indices,
 * creating its half-edges and linking twins against any matching
 * already-existing opposite edges. Returns the new face's index. */
int halfedge_add_face(HalfEdgeMesh *hem, const int *vert_indices, int n);

/* Builds a half-edge mesh from a flat indexed triangle buffer (glTF's
 * on-disk representation) — positions is a flat xyz array of pos_count
 * vertices, indices is a flat triangle-list of index_count indices
 * (index_count must be a multiple of 3). Shared vertex indices in the
 * input correctly become shared HEVertex entries with proper twin-edge
 * topology across face boundaries (not a per-triangle vertex soup). */
HalfEdgeMesh *halfedge_build_from_triangles(const float *positions, int pos_count,
                                             const unsigned short *indices, int index_count);

/* Walks face `f`'s edge loop and writes its vertex indices into
 * out_verts (must have room for hem->faces[f].count entries). */
void halfedge_face_verts(const HalfEdgeMesh *hem, int f, int *out_verts);

/* Flattens back to a flat indexed triangle buffer (glTF's on-disk shape)
 * — every face is assumed already a triangle (see the struct comment;
 * n-gon triangulation isn't implemented in this pass). *out_positions and
 * *out_indices are malloc'd (caller frees); *out_pos_count and
 * *out_index_count receive their lengths (pos_count in vec3s, index_count
 * in indices). */
void halfedge_flatten_triangles(const HalfEdgeMesh *hem,
                                 float **out_positions, int *out_pos_count,
                                 unsigned short **out_indices, int *out_index_count);
