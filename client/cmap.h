#pragma once
/*
 * .cmap binary format
 *
 * Header (32 bytes):
 *   magic:      4 bytes  "CMAP"
 *   version:    u16      = 2
 *   world_size: u8       log2 of root cube size (8 = 256^3)
 *   reserved:   25 bytes
 *
 * Node stream (depth-first):
 *   type:    u8   (0=EMPTY, 1=SOLID, 2=DEFORMED, 3=SUBDIVIDED)
 *   SOLID:      faces[6] u8              (v2: materials now persist on flat cubes too)
 *   DEFORMED:   corners[8] i8, faces[6] u8
 *   SUBDIVIDED: 8 child nodes follow inline
 */
#include "octree.h"
#include <stdio.h>

int  cmap_save(const Octree *ot, const char *path);
Octree *cmap_load(const char *path);

/* In-memory versions for WASM (returns heap buffer, caller frees) */
uint8_t *cmap_serialize(const Octree *ot, size_t *out_size);
Octree  *cmap_deserialize(const uint8_t *buf, size_t size);
