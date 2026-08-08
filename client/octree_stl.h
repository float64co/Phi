#pragma once
#include "octree.h"

/*
 * Binary STL export from the octree.
 * Walks the octree, emits one STL facet per triangle (2 per visible quad).
 * Non-planar quads are split on the shorter diagonal (Sauerbraten convention).
 *
 * STL Binary format:
 *   80-byte header
 *   uint32_t  triangle_count
 *   per triangle (50 bytes):
 *     float[3]  normal
 *     float[3]  v0
 *     float[3]  v1
 *     float[3]  v2
 *     uint16_t  attribute (0)
 */

/* Returns heap-allocated STL buffer. Caller must free(). Sets *out_size. */
uint8_t *octree_export_stl(const Octree *ot, size_t *out_size);

/* Trigger browser download of the STL file.
 * Calls JS via Emscripten to create a Blob and click a download link.
 * Filename: "map.stl"
 */
void octree_stl_download(const Octree *ot);
