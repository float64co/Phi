#pragma once
#include "halfedge.h"

/* glTF load/save boundary for HalfEdgeMesh (see halfedge.h's top comment —
 * glTF is the interchange format, this structure is the live in-memory
 * one). Uses cgltf (vendored, client/cgltf.h) for parsing; writing is done
 * directly (cgltf is read-only) as a minimal single-mesh/single-primitive
 * glTF, matching exactly what this phase's foundation slice needs — not a
 * general-purpose glTF writer. */

/* Loads mesh[0]'s primitive[0] from a glTF/GLB file via cgltf and builds a
 * HalfEdgeMesh from its POSITION accessor + triangle-list indices. Returns
 * NULL on failure (bad path, parse error, missing POSITION attribute,
 * non-triangle primitive mode, or no meshes/primitives). */
HalfEdgeMesh *halfedge_load_gltf(const char *path);

/* Flattens hem back to a flat indexed triangle buffer (halfedge_flatten_
 * triangles) and writes a minimal glTF (JSON + a sibling .bin file, same
 * basename as gltf_path) — position-only, single mesh/primitive/node,
 * mirroring the shape of the hand-authored assets/cube.gltf test asset
 * this was built to round-trip against. Returns 1 on success, 0 on
 * failure (e.g. couldn't open either output file for writing). */
int halfedge_save_gltf(const HalfEdgeMesh *hem, const char *gltf_path);
