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
 * Twin-edge lookup (used when adding a face, to link each new half-edge to
 * its already-existing opposite if one exists) is backed by an open-
 * addressing hash map keyed on (origin,dest) -- amortized O(1) per add,
 * O(n) overall for a full mesh (see halfedge.c's edge_hash_* helpers).
 * Rewritten 2026-08-19: the original linear-scan-over-all-edges version
 * (O(n) per add, O(n^2) overall) was fine for this phase's small test
 * assets but became a real, measured hang loading a 398k-triangle
 * Sketchfab character import (halfedge_gltf.c's own full-scene loader) --
 * this phase's foundation slice explicitly flagged the hash map as future
 * work rather than building it prematurely; that future arrived. */

typedef struct {
    float pos[3];
    /* Texture coordinate (0,0 default -- every existing caller that never
     * sets one, e.g. mp_port.c's phi.create_mesh/add_vertex, keeps working
     * unchanged: an untextured face just ignores uv entirely, see HEFace's
     * own texture field). Per-VERTEX, not per-face-corner -- a real UV
     * seam (the same position needing two different UVs depending on
     * which face it's part of) becomes two separate HEVertex entries with
     * the same pos, exactly how glTF's own indexed vertex buffers already
     * represent seams (a shared position on one side of a seam and a
     * separate index on the other) -- halfedge_gltf.c's loader imports
     * each glTF vertex as its own HEVertex for this reason, not
     * deduplicating by position, so seams import correctly by construction. */
    float uv[2];
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
    int deleted;  /* soft-delete tombstone, see halfedge_delete_face() */
    /* Real per-face PBR material (Phase 1's "PBR material assignment per
     * face" -- see phi.md). Lives directly on the face rather than in a
     * separate material table/index: this structure has no other indirect-
     * reference machinery (faces already ARE the natural per-face unit),
     * and a session's worth of faces is small enough that a few extra
     * floats per HEFace is negligible. halfedge_add_face() defaults every
     * new face to a neutral dielectric/rough material (see its own
     * comment); halfedge_set_face_material() is the intended way to
     * change it (clamps metallic/roughness to sane ranges), though nothing
     * stops direct field access the way .deleted already gets set/read
     * directly elsewhere in this codebase. */
    float base_color[3];
    float metallic;
    float roughness;
    float emission[3];
    /* Real GL texture name (see texture_cache.h) sampled for this face's
     * base color, multiplied against base_color per glTF's own
     * pbrMetallicRoughness spec (baseColorFactor tints baseColorTexture,
     * not one-or-the-other) -- 0 (GL's own "no texture" name) means "flat
     * base_color only", the default every existing caller that never
     * touches this field already gets for free. Not cleared/freed by
     * halfedge_destroy -- textures are cached and shared (texture_cache.h
     * owns their lifetime, keyed by file path), a HalfEdgeMesh only ever
     * holds a borrowed GL name, never uploads or frees one itself. */
    unsigned int texture;
} HEFace;

typedef struct {
    HEVertex *verts; int vert_count, vert_cap;
    HEEdge   *edges; int edge_count, edge_cap;
    HEFace   *faces; int face_count, face_cap;
    /* Private to halfedge.c's edge_hash_* helpers (see this file's top
     * comment) -- an open-addressing table of edge indices, -1 = empty,
     * keyed on (origin,dest). Not touched by any other file. */
    int *edge_hash; int edge_hash_cap;
} HalfEdgeMesh;

HalfEdgeMesh *halfedge_create(void);
void          halfedge_destroy(HalfEdgeMesh *hem);

/* Adds a vertex, returns its index. uv defaults to (0,0) -- see
 * halfedge_set_vertex_uv() to set a real one (kept as a separate setter
 * rather than widening this function's own signature, so every existing
 * call site -- mp_port.c's phi.add_vertex chief among them -- keeps
 * compiling unchanged). */
int halfedge_add_vertex(HalfEdgeMesh *hem, float x, float y, float z);

/* Sets vertex v's texture coordinate (see HEVertex::uv's own comment).
 * No-op if v is out of range. */
void halfedge_set_vertex_uv(HalfEdgeMesh *hem, int v, float u, float vcoord);

/* Adds a face from a CCW-wound loop of `n` existing vertex indices,
 * creating its half-edges and linking twins against any matching
 * already-existing opposite edges. Returns the new face's index. New
 * faces default to a neutral dielectric/rough material (base_color
 * 0.7,0.7,0.7, metallic 0, roughness 0.8, emission 0) — see
 * halfedge_set_face_material() to change it. */
int halfedge_add_face(HalfEdgeMesh *hem, const int *vert_indices, int n);

/* Sets face f's PBR material, clamping metallic/roughness to [0,1] (base
 * color and emission are left unclamped -- emission in particular is
 * expected to go above 1.0 for a genuinely bright emissive surface). No-op
 * if f is out of range or already deleted. */
void halfedge_set_face_material(HalfEdgeMesh *hem, int f, const float base_color[3],
                                 float metallic, float roughness, const float emission[3]);

/* Sets face f's base-color texture (see HEFace::texture's own comment) --
 * a real GL texture name (0 clears it back to flat-base_color-only). A
 * separate setter from halfedge_set_face_material rather than widening
 * its parameter list, so every existing caller (mp_port.c's phi.
 * set_face_material chief among them) keeps compiling unchanged; the two
 * are independent, not a package deal (a face can have a texture with no
 * explicit halfedge_set_face_material call, keeping the default white-
 * ish base_color glTF's own baseColorFactor default already produces).
 * No-op if f is out of range or already deleted. */
void halfedge_set_face_texture(HalfEdgeMesh *hem, int f, unsigned int texture);

/* Soft-deletes face `f`: marks it (and its edges) as gone and resets any
 * twin link pointing at one of its edges back to -1, so a neighboring face
 * across a shared edge correctly becomes boundary again rather than
 * dangling. This is a tombstone, not a real removal (added() indices stay
 * stable, matching this structure's append-only growth model — see the
 * file comment) — halfedge_face_verts/flatten/meshobject_build_render_
 * mesh_from_halfedge all skip deleted faces. Editing operations that
 * displace or subdivide a face (extrude/inset/loop-cut, see mesh_edit.c)
 * delete the original face and add fresh replacement faces rather than
 * mutating vertex indices in place, since nothing else in this structure
 * supports in-place face mutation. */
void halfedge_delete_face(HalfEdgeMesh *hem, int f);

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
 * — every LIVE (non-deleted) face is assumed already a triangle (see the
 * struct comment; n-gon triangulation isn't implemented in this pass).
 * Deleted faces (see halfedge_delete_face) are skipped entirely, so
 * out_index_count reflects only what's actually still part of the mesh.
 * *out_positions and *out_indices are malloc'd (caller frees);
 * *out_pos_count and *out_index_count receive their lengths (pos_count in
 * vec3s, index_count in indices). */
void halfedge_flatten_triangles(const HalfEdgeMesh *hem,
                                 float **out_positions, int *out_pos_count,
                                 unsigned short **out_indices, int *out_index_count);
