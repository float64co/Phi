#pragma once
#include "halfedge.h"
#include <stdint.h>

/* glTF load/save boundary for HalfEdgeMesh (see halfedge.h's top comment —
 * glTF is the interchange format, this structure is the live in-memory
 * one). Uses cgltf (vendored, client/cgltf.h) for parsing; writing is done
 * directly (cgltf is read-only) as a minimal single-mesh/single-primitive
 * glTF, matching exactly what this phase's foundation slice needs — not a
 * general-purpose glTF writer. */

/* Loads a glTF/GLB file's ENTIRE default scene via cgltf -- every mesh
 * primitive reachable from the scene graph, positioned by its own node's
 * real world transform (translation/rotation/scale, walked recursively
 * through parent/child nodes), merged into one HalfEdgeMesh. Real per-
 * face material too: each primitive's glTF material resolves to a flat
 * base-color tint (baseColorFactor) and, if present, a real GL texture
 * (baseColorTexture -- see HEFace::texture and texture_cache.h); non-
 * triangle primitives and primitives with no POSITION attribute are
 * skipped (logged, not fatal) rather than misread. Returns NULL on
 * failure (bad path, parse error, or a scene graph that reaches no real
 * triangles at all). */
HalfEdgeMesh *halfedge_load_gltf(const char *path);

/* Registers the real texture loader (texture_cache.c's texture_cache_load,
 * matching phi_mp_register_camera_callback's own function-pointer-not-
 * direct-link shape -- see this file's own comment at the registration
 * site for why). Call once from a real engine executable's startup
 * (editor_main.c, player_main.c); leave unregistered in a no-GL test
 * harness, where every loaded material's texture just stays 0. */
void halfedge_gltf_register_texture_loader(unsigned int (*load_texture)(const char *path));

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
