#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/*
 * Sauerbraten-exact octree: 10 levels, 1024^3 world units.
 * Root cube = 1024 units. Each subdivision halves edge length.
 * Root at level 0 has size 1024. At level N, size = 1024 >> N.
 * Leaf nodes (level 9) have size 2. Minimum geometry unit = 2 world units.
 *
 * octree_make_default_map() only builds the original arena in the
 * [0,256) corner of this space — the rest is open world for the editor.
 * server/mapdata.py mirrors WORLD_SIZE/MAX_OCTREE_DEPTH exactly; keep
 * them in sync or edits near/past the old 256 bound will get silently
 * clamped/corrupted by the server.
 */

#define WORLD_SIZE      1024
#define MAX_OCTREE_DEPTH 10
#define LEAF_SIZE       (WORLD_SIZE >> (MAX_OCTREE_DEPTH - 1))  /* = 2 */

typedef enum {
    NODE_EMPTY      = 0,
    NODE_SOLID      = 1,
    NODE_DEFORMED   = 2,
    NODE_SUBDIVIDED = 3
} NodeType;

/* Corner indices:
 *   Bottom face (y=min): 0=xmin,zmin  1=xmax,zmin  2=xmax,zmax  3=xmin,zmax
 *   Top face    (y=max): 4=xmin,zmin  5=xmax,zmin  6=xmax,zmax  7=xmin,zmax
 */
typedef struct OctreeNode OctreeNode;
struct OctreeNode {
    uint8_t     type;           /* NodeType */
    int8_t      corners[8];     /* corner height offsets (-128..127) for DEFORMED */
    uint8_t     faces[6];       /* material/texture index per face (NESW, top, bot) */
    OctreeNode *children[8];    /* only valid for SUBDIVIDED */
};

typedef struct {
    OctreeNode *root;
    int         depth;          /* always MAX_OCTREE_DEPTH */
    int         world_size;     /* always WORLD_SIZE */
} Octree;

/* Face index constants */
#define FACE_NEG_X  0
#define FACE_POS_X  1
#define FACE_NEG_Y  2
#define FACE_POS_Y  3
#define FACE_NEG_Z  4
#define FACE_POS_Z  5

/* Vec3 for geometry */
typedef struct { float x, y, z; } Vec3f;
typedef struct { int   x, y, z; } Vec3i;

/* child index from 3 bits: bit0=x, bit1=y, bit2=z */
static inline int child_idx(int cx, int cy, int cz) {
    return (cx & 1) | ((cy & 1) << 1) | ((cz & 1) << 2);
}

/* ---- allocation pool for nodes ---- */
#define NODE_POOL_SIZE (1 << 20)  /* 1M nodes max */
static OctreeNode  s_node_pool[NODE_POOL_SIZE];
static int         s_node_pool_idx = 0;

static inline OctreeNode *node_alloc(void) {
    if (s_node_pool_idx >= NODE_POOL_SIZE) return NULL;
    OctreeNode *n = &s_node_pool[s_node_pool_idx++];
    memset(n, 0, sizeof(*n));
    return n;
}

static inline void node_pool_reset(void) { s_node_pool_idx = 0; }

/* ---- helpers ----
 * octree_set_solid/empty/material take an arbitrary axis-aligned box
 * (min inclusive, max exclusive) so editor drag-selections don't need to
 * be cubes. octree_set_solid_cube/octree_set_empty_cube are thin
 * (x,y,z,size) wrappers kept for the many cube-shaped calls in
 * octree_make_default_map().
 */
Octree *octree_create(void);
void    octree_destroy(Octree *ot);
void    octree_set_solid(Octree *ot, int minx, int miny, int minz,
                                      int maxx, int maxy, int maxz);
void    octree_set_empty(Octree *ot, int minx, int miny, int minz,
                                      int maxx, int maxy, int maxz);
/* Repaint faces[face] = mat on any already-solid/deformed nodes inside the
 * box; empty space is left untouched (can't paint air). */
void    octree_set_material(Octree *ot, int minx, int miny, int minz,
                                         int maxx, int maxy, int maxz,
                                         int face, int mat);

static inline void octree_set_solid_cube(Octree *ot, int x, int y, int z, int size) {
    octree_set_solid(ot, x, y, z, x + size, y + size, z + size);
}
static inline void octree_set_empty_cube(Octree *ot, int x, int y, int z, int size) {
    octree_set_empty(ot, x, y, z, x + size, y + size, z + size);
}

int     octree_is_solid(const Octree *ot, int x, int y, int z);
void    octree_make_default_map(Octree *ot);

/* AABB vs octree collision */
int     octree_aabb_solid(const Octree *ot,
                           float minx, float miny, float minz,
                           float maxx, float maxy, float maxz);

/* ray vs octree (returns t of first hit, -1 if none) */
float   octree_ray_cast(const Octree *ot,
                         Vec3f origin, Vec3f dir, float max_t,
                         Vec3f *hit_pos, Vec3f *hit_normal);
