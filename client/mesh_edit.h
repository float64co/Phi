#pragma once
#include "halfedge.h"

/* Real half-edge topology mutations — Phase 1's extrude/inset/loop-cut
 * mesh editing operations (see phi.md's "Mesh Editor" section). Built on
 * top of halfedge.c's own append-only primitives (halfedge_add_vertex/
 * halfedge_add_face/halfedge_delete_face) rather than extending that file
 * directly — halfedge.c stays the minimal data-structure layer, this file
 * is the "operations that mutate it" layer its own header comment already
 * anticipated ("later editing operations... would walk [this]").
 *
 * Common mechanism for extrude/inset (mesh_edit.c's own static
 * extrude_or_inset helper): both operations displace a face's boundary
 * loop to a new set of positions while leaving the REST of the mesh
 * undisturbed. Since halfedge.c has no in-place "move this face's
 * vertices" mutation, both work by: add fresh vertices at the new
 * positions -> halfedge_delete_face the original (this also resets any
 * neighbor's twin link back to -1, opening the boundary) -> add a new cap
 * face at the fresh positions -> add a ring of side-wall triangles
 * connecting the untouched original boundary to the new cap boundary
 * (each add_face's own twin-finding then naturally reconnects the walls
 * to whatever real neighbor faces already bordered the original). This is
 * the same topology real extrude/inset produce (Blender et al.) — the
 * original face's *slot* in the array becomes a dead tombstone rather
 * than literally being the moved geometry, which is an implementation
 * detail invisible to anything reading live (non-deleted) faces.
 *
 * All three operations require the target to be a triangle (this
 * codebase's editable meshes are triangles-only for now, see halfedge.h)
 * and fail (-1) on anything else, on an out-of-range index, or on an
 * already-deleted face/edge. */

/* Extrudes face f outward along its own flat face normal by `dist` world
 * units. Returns the new displaced cap face's index, or -1 on failure. */
int mesh_edit_extrude_face(HalfEdgeMesh *hem, int f, float dist);

/* Insets face f: shrinks a fresh coplanar copy of its boundary toward its
 * own centroid by `factor` (0 = no change, approaching 1 = shrinks toward
 * a point) and stitches a ring of side walls to the original, undisturbed
 * boundary. Returns the new inset face's index, or -1 on failure. */
int mesh_edit_inset_face(HalfEdgeMesh *hem, int f, float factor);

/* Cuts the edge at index e: inserts a new vertex at its midpoint and
 * re-triangulates the (up to) two faces sharing it -- e's own face, and
 * its twin's face if e has a twin -- around that midpoint. This is the
 * minimal real primitive a full multi-face loop cut is built from: it
 * splits the picked edge and its immediate twin, not a whole ring traced
 * across a quad-topology loop (this mesh is triangles-only, so a "ring" in
 * the Blender sense doesn't strictly exist here — see phi.md for the
 * honest scoping note on this). Returns the new midpoint vertex's index,
 * or -1 on failure. */
int mesh_edit_loop_cut_edge(HalfEdgeMesh *hem, int e);

/* Turns a face pick into an edge pick: given a hit face f and a hit point
 * already in the mesh's own local space (caller un-transforms by the
 * object's position/orientation first, same space hem's own vertex
 * positions live in), returns the index of f's boundary edge whose
 * midpoint is nearest that point -- so a single ray-cast pick can drive
 * mesh_edit_loop_cut_edge without needing separate edge-picking UI/input.
 * Returns -1 if f is out of range or deleted. */
int mesh_edit_nearest_edge_of_face(const HalfEdgeMesh *hem, int f, float hit_x, float hit_y, float hit_z);
