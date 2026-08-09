#pragma once
#include "halfedge.h"
#include <stdint.h>

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

/* Same flatten as halfedge_save_gltf, but packages the result as a single
 * self-contained GLB binary buffer in memory (malloc'd via *out_data,
 * caller frees) instead of writing loose .gltf+.bin files -- this is the
 * form the Asset Browser's CRUD upload endpoint requires (see phi.md's
 * "Wire protocol: CRUD over a hybrid HTTP + WS split": create only
 * accepts .glb). Same container shape tools/gen_test_assets.py's own
 * write_glb hand-rolls in Python (12-byte header + JSON chunk + BIN
 * chunk, Khronos glTF 2.0 binary spec) -- this is the C-side twin of
 * that, needed because main.c has to build one at runtime from whatever
 * MeshObject is currently selected, not offline from a script. Returns 1
 * on success, 0 on failure (e.g. hem has no vertices). */
int halfedge_save_glb_buffer(const HalfEdgeMesh *hem, uint8_t **out_data, int *out_len);
