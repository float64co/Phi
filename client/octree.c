#include "octree.h"
#include <math.h>
#include <stdio.h>

Octree *octree_create(void) {
    node_pool_reset();
    Octree *ot = (Octree *)malloc(sizeof(Octree));
    ot->depth      = MAX_OCTREE_DEPTH;
    ot->world_size = WORLD_SIZE;
    ot->root       = node_alloc();
    ot->root->type = NODE_EMPTY;
    return ot;
}

void octree_destroy(Octree *ot) {
    /* pool-allocated, just free the octree struct */
    free(ot);
}

/* child index from 3 bits: defined in octree.h */

/* Ensure children exist for a SUBDIVIDED node */
static void ensure_children(OctreeNode *node) {
    for (int i = 0; i < 8; i++) {
        if (!node->children[i]) {
            node->children[i] = node_alloc();
            node->children[i]->type = NODE_EMPTY;
        }
    }
}

/* Split a leaf into 8 SUBDIVIDED children, propagating its old type,
 * corners and faces so painted materials / deformations survive a later
 * partial-overlap edit that only touches part of this node. */
static void split_leaf(OctreeNode *node) {
    NodeType old = node->type;
    int8_t  old_corners[8]; memcpy(old_corners, node->corners, sizeof(old_corners));
    uint8_t old_faces[6];   memcpy(old_faces,   node->faces,   sizeof(old_faces));
    node->type = NODE_SUBDIVIDED;
    ensure_children(node);
    for (int i = 0; i < 8; i++) {
        node->children[i]->type = old;
        memcpy(node->children[i]->corners, old_corners, sizeof(old_corners));
        memcpy(node->children[i]->faces,   old_faces,   sizeof(old_faces));
    }
}

/* Merge 8 children back into a leaf if they're all identical (type, and
 * faces when SOLID) — skips the merge (stays SUBDIVIDED) rather than lose
 * distinct per-child face materials. */
static void try_merge(OctreeNode *node) {
    NodeType t0 = node->children[0]->type;
    if (t0 != NODE_EMPTY && t0 != NODE_SOLID) return;
    int same = 1;
    for (int i = 1; i < 8; i++) {
        if (node->children[i]->type != t0) { same = 0; break; }
    }
    if (same && t0 == NODE_SOLID) {
        for (int f = 0; f < 6 && same; f++) {
            uint8_t f0 = node->children[0]->faces[f];
            for (int i = 1; i < 8; i++) {
                if (node->children[i]->faces[f] != f0) { same = 0; break; }
            }
        }
    }
    if (!same) return;
    if (t0 == NODE_SOLID) memcpy(node->faces, node->children[0]->faces, sizeof(node->faces));
    for (int i = 0; i < 8; i++) node->children[i] = NULL;
    node->type = t0;
}

/* Generic octree descent to set an axis-aligned box [rmin, rmax) */
static void _set_region(OctreeNode *node,
                         int nx, int ny, int nz, int nsize,        /* node bounds */
                         int rminx, int rminy, int rminz,
                         int rmaxx, int rmaxy, int rmaxz,          /* target box */
                         NodeType target_type) {
    /* If this node is fully inside the region */
    if (nx >= rminx && ny >= rminy && nz >= rminz &&
        nx + nsize <= rmaxx &&
        ny + nsize <= rmaxy &&
        nz + nsize <= rmaxz) {
        for (int i = 0; i < 8; i++) node->children[i] = NULL;
        node->type = target_type;
        return;
    }
    /* If node doesn't overlap region at all, skip */
    if (nx + nsize <= rminx || nx >= rmaxx ||
        ny + nsize <= rminy || ny >= rmaxy ||
        nz + nsize <= rminz || nz >= rmaxz) {
        return;
    }
    /* Can't subdivide past leaf granularity — defensive stop for
     * malformed (non-grid-aligned) boxes coming over the network */
    if (nsize <= LEAF_SIZE) {
        node->type = target_type;
        return;
    }
    /* Partial overlap — must subdivide */
    if (node->type != NODE_SUBDIVIDED) split_leaf(node);

    int half = nsize >> 1;
    for (int cz = 0; cz < 2; cz++)
    for (int cy = 0; cy < 2; cy++)
    for (int cx = 0; cx < 2; cx++) {
        int idx = child_idx(cx, cy, cz);
        _set_region(node->children[idx],
                    nx + cx * half, ny + cy * half, nz + cz * half, half,
                    rminx, rminy, rminz, rmaxx, rmaxy, rmaxz, target_type);
    }
    try_merge(node);
}

void octree_set_solid(Octree *ot, int minx, int miny, int minz,
                                   int maxx, int maxy, int maxz) {
    _set_region(ot->root, 0, 0, 0, ot->world_size,
                minx, miny, minz, maxx, maxy, maxz, NODE_SOLID);
}

void octree_set_empty(Octree *ot, int minx, int miny, int minz,
                                   int maxx, int maxy, int maxz) {
    _set_region(ot->root, 0, 0, 0, ot->world_size,
                minx, miny, minz, maxx, maxy, maxz, NODE_EMPTY);
}

/* Repaint faces[face] on already-solid/deformed nodes inside [rmin,rmax);
 * empty space is left untouched. Subdivides on partial overlap (same as
 * _set_region) so only the intersected portion of a larger merged block
 * gets repainted. */
static void _set_material(OctreeNode *node,
                           int nx, int ny, int nz, int nsize,
                           int rminx, int rminy, int rminz,
                           int rmaxx, int rmaxy, int rmaxz,
                           int face, uint8_t mat) {
    if (node->type == NODE_EMPTY) return;
    if (nx + nsize <= rminx || nx >= rmaxx ||
        ny + nsize <= rminy || ny >= rmaxy ||
        nz + nsize <= rminz || nz >= rmaxz) {
        return;
    }
    int fully_inside = (nx >= rminx && ny >= rminy && nz >= rminz &&
                         nx + nsize <= rmaxx && ny + nsize <= rmaxy && nz + nsize <= rmaxz);
    if (fully_inside && node->type != NODE_SUBDIVIDED) {
        node->faces[face] = mat;
        return;
    }
    if (nsize <= LEAF_SIZE) {
        node->faces[face] = mat;
        return;
    }
    if (node->type != NODE_SUBDIVIDED) split_leaf(node);

    int half = nsize >> 1;
    for (int cz = 0; cz < 2; cz++)
    for (int cy = 0; cy < 2; cy++)
    for (int cx = 0; cx < 2; cx++) {
        int idx = child_idx(cx, cy, cz);
        _set_material(node->children[idx],
                      nx + cx * half, ny + cy * half, nz + cz * half, half,
                      rminx, rminy, rminz, rmaxx, rmaxy, rmaxz, face, mat);
    }
    /* Deliberately no try_merge() here: sibling leaves now carry distinct
     * face materials by design, merging would erase that distinction. */
}

void octree_set_material(Octree *ot, int minx, int miny, int minz,
                                      int maxx, int maxy, int maxz,
                                      int face, int mat) {
    _set_material(ot->root, 0, 0, 0, ot->world_size,
                  minx, miny, minz, maxx, maxy, maxz, face, (uint8_t)mat);
}

static int _is_solid(const OctreeNode *node,
                      int nx, int ny, int nz, int nsize,
                      int px, int py, int pz) {
    if (node->type == NODE_SOLID || node->type == NODE_DEFORMED) return 1;
    if (node->type == NODE_EMPTY) return 0;
    int half = nsize >> 1;
    int cx = (px >= nx + half) ? 1 : 0;
    int cy = (py >= ny + half) ? 1 : 0;
    int cz = (pz >= nz + half) ? 1 : 0;
    int idx = child_idx(cx, cy, cz);
    if (!node->children[idx]) return 0;
    return _is_solid(node->children[idx],
                     nx + cx * half, ny + cy * half, nz + cz * half, half,
                     px, py, pz);
}

int octree_is_solid(const Octree *ot, int x, int y, int z) {
    if (x < 0 || y < 0 || z < 0 ||
        x >= ot->world_size || y >= ot->world_size || z >= ot->world_size)
        return 1; /* treat out-of-bounds as solid */
    return _is_solid(ot->root, 0, 0, 0, ot->world_size, x, y, z);
}

/* ---- AABB collision ---- */
static int _aabb_solid(const OctreeNode *node,
                        int nx, int ny, int nz, int nsize,
                        float minx, float miny, float minz,
                        float maxx, float maxy, float maxz) {
    /* AABB vs node bounds */
    if (maxx <= nx || minx >= nx + nsize ||
        maxy <= ny || miny >= ny + nsize ||
        maxz <= nz || minz >= nz + nsize) return 0;
    if (node->type == NODE_SOLID || node->type == NODE_DEFORMED) return 1;
    if (node->type == NODE_EMPTY) return 0;
    int half = nsize >> 1;
    for (int cz = 0; cz < 2; cz++)
    for (int cy = 0; cy < 2; cy++)
    for (int cx = 0; cx < 2; cx++) {
        int idx = child_idx(cx, cy, cz);
        if (!node->children[idx]) continue;
        if (_aabb_solid(node->children[idx],
                        nx + cx*half, ny + cy*half, nz + cz*half, half,
                        minx, miny, minz, maxx, maxy, maxz)) return 1;
    }
    return 0;
}

int octree_aabb_solid(const Octree *ot,
                       float minx, float miny, float minz,
                       float maxx, float maxy, float maxz) {
    return _aabb_solid(ot->root, 0, 0, 0, ot->world_size,
                       minx, miny, minz, maxx, maxy, maxz);
}

/* ---- Ray vs octree (slab method) ---- */
static float _ray_vs_aabb(float ox, float oy, float oz,
                            float idx, float idy, float idz,
                            int nx, int ny, int nz, int nsize,
                            float *t_out) {
    float t1 = ((float)nx         - ox) * idx;
    float t2 = ((float)(nx+nsize) - ox) * idx;
    float t3 = ((float)ny         - oy) * idy;
    float t4 = ((float)(ny+nsize) - oy) * idy;
    float t5 = ((float)nz         - oz) * idz;
    float t6 = ((float)(nz+nsize) - oz) * idz;
    float tmin = fmaxf(fmaxf(fminf(t1,t2), fminf(t3,t4)), fminf(t5,t6));
    float tmax = fminf(fminf(fmaxf(t1,t2), fmaxf(t3,t4)), fmaxf(t5,t6));
    if (tmax < 0 || tmin > tmax) return -1.0f;
    /* tmin < 0 means the origin is already inside this box (extremely
     * common — true for the root node on almost every cast, since rays
     * always start somewhere inside the world) — the entry distance is 0
     * from here, NOT tmax (the box's far exit point). Returning tmax was
     * inflating the "how far to this node" value used by the caller's
     * `t > max_t` pruning check, causing it to wrongly discard subtrees
     * the ray origin was sitting right inside of whenever max_t was
     * smaller than the distance to that node's far side — i.e. almost any
     * short-range cast (rocket flight this frame, splash-occlusion check,
     * etc.) against the large early nodes near the root. */
    *t_out = tmin < 0 ? 0.0f : tmin;
    return *t_out;
}

static float _ray_cast(const OctreeNode *node,
                         int nx, int ny, int nz, int nsize,
                         float ox, float oy, float oz,
                         float dx, float dy, float dz,
                         float idx, float idy, float idz,
                         float max_t, Vec3f *hit_pos, Vec3f *hit_normal) {
    float t = 0;
    if (_ray_vs_aabb(ox,oy,oz,idx,idy,idz,nx,ny,nz,nsize,&t) < 0) return -1;
    if (t > max_t) return -1;
    if (node->type == NODE_SOLID || node->type == NODE_DEFORMED) {
        if (hit_pos) {
            hit_pos->x = ox + dx * t;
            hit_pos->y = oy + dy * t;
            hit_pos->z = oz + dz * t;
        }
        if (hit_normal) {
            /* Determine which face was hit */
            float cx = nx + nsize * 0.5f, cy2 = ny + nsize * 0.5f, cz = nz + nsize * 0.5f;
            float hx = ox + dx*t - cx, hy = oy + dy*t - cy2, hz = oz + dz*t - cz;
            float ax = fabsf(hx), ay = fabsf(hy), az = fabsf(hz);
            if (ax >= ay && ax >= az) { hit_normal->x = hx>0?1:-1; hit_normal->y=0; hit_normal->z=0; }
            else if (ay >= ax && ay >= az) { hit_normal->y = hy>0?1:-1; hit_normal->x=0; hit_normal->z=0; }
            else { hit_normal->z = hz>0?1:-1; hit_normal->x=0; hit_normal->y=0; }
        }
        return t;
    }
    if (node->type == NODE_EMPTY) return -1;
    /* SUBDIVIDED: check children, keep only the CLOSEST hit.
     * Each child writes into its own local pos/normal first — committing
     * straight to the caller's hit_pos/hit_normal here would let a later,
     * farther child silently overwrite an earlier, closer one even though
     * `best` (the returned distance) stayed correct. */
    int half = nsize >> 1;
    float best = -1;
    Vec3f best_pos = {0,0,0}, best_normal = {0,0,0};
    for (int cz = 0; cz < 2; cz++)
    for (int cy = 0; cy < 2; cy++)
    for (int cx2 = 0; cx2 < 2; cx2++) {
        int idx2 = child_idx(cx2, cy, cz);
        if (!node->children[idx2]) continue;
        Vec3f cp, cn;
        float ht = _ray_cast(node->children[idx2],
                              nx+cx2*half, ny+cy*half, nz+cz*half, half,
                              ox,oy,oz,dx,dy,dz,idx,idy,idz,max_t,&cp,&cn);
        if (ht >= 0 && (best < 0 || ht < best)) { best = ht; best_pos = cp; best_normal = cn; }
    }
    if (best >= 0) {
        if (hit_pos)    *hit_pos    = best_pos;
        if (hit_normal) *hit_normal = best_normal;
    }
    return best;
}

float octree_ray_cast(const Octree *ot,
                       Vec3f origin, Vec3f dir, float max_t,
                       Vec3f *hit_pos, Vec3f *hit_normal) {
    float idx = (dir.x != 0) ? 1.0f/dir.x : 1e30f;
    float idy = (dir.y != 0) ? 1.0f/dir.y : 1e30f;
    float idz = (dir.z != 0) ? 1.0f/dir.z : 1e30f;
    return _ray_cast(ot->root, 0, 0, 0, ot->world_size,
                     origin.x, origin.y, origin.z,
                     dir.x, dir.y, dir.z,
                     idx, idy, idz, max_t, hit_pos, hit_normal);
}

/* ---- Default arena map ----
 * Builds a self-contained 256^3 arena in the [0,256) corner of the
 * (now 1024^3) world — everything beyond that is left NODE_EMPTY, open
 * for the editor to build into. */
/*
 * 256^3 arena, 16-unit thick shell.  Interior [16..240] on all axes.
 * 224 units of open space = the full arena.
 *
 * ALL octree_set_* calls use (minX, minY, minZ, size) where size is the
 * edge length of a CUBE.  Coordinates are verified below:
 *
 *   Floor:   y=16 (top face of floor shell)
 *   Ceiling: y=240 (bottom face of ceiling shell)
 *   Walls:   x=16/240, z=16/240
 *   Play height: 224 units (plenty of room)
 *
 * Structure heights above floor (y=16):
 *   Low cover pillars:  y=16..48  (+32)
 *   Mid walkways:       y=16..80  (+64, slab 16 thick → top at y=80)
 *   High platforms:     y=80..112 (+32 slab)
 *   Towers:             y=16..176
 *   Floating island:    y=144..160
 *   Ceiling:            y=240
 */
void octree_make_default_map(Octree *ot) {

    /* ---- 1. Solid fill ---- */
    octree_set_solid_cube(ot, 0, 0, 0, 256);

    /* ---- 2. Carve interior [16..240]^3 ----
       224 = 128 + 64 + 32.  Split each axis into those three segments.
       We need 3^3 = 27 non-overlapping cubes.  Axes:
         A: [16..144]  size=128
         B: [144..208] size=64
         C: [208..240] size=32
    */
    /* xA yA zA */ octree_set_empty_cube(ot,  16,  16,  16, 128);
    /* xA yA yB */ octree_set_empty_cube(ot,  16,  16, 144,  64);
    /* xA yA zC */ octree_set_empty_cube(ot,  16,  16, 208,  32);
    /* xA yB zA */ octree_set_empty_cube(ot,  16, 144,  16,  64);  /* NOTE: y here is 2nd arg */
    /* xA yB zB */ octree_set_empty_cube(ot,  16, 144, 144,  64);
    /* xA yB zC */ octree_set_empty_cube(ot,  16, 144, 208,  32);
    /* xA yC zA */ octree_set_empty_cube(ot,  16, 208,  16,  32);
    /* xA yC zB */ octree_set_empty_cube(ot,  16, 208, 144,  32);
    /* xA yC zC */ octree_set_empty_cube(ot,  16, 208, 208,  32);

    /* xB yA zA */ octree_set_empty_cube(ot, 144,  16,  16,  64);
    /* xB yA zB */ octree_set_empty_cube(ot, 144,  16, 144,  64);
    /* xB yA zC */ octree_set_empty_cube(ot, 144,  16, 208,  32);
    /* xB yB zA */ octree_set_empty_cube(ot, 144, 144,  16,  64);
    /* xB yB zB */ octree_set_empty_cube(ot, 144, 144, 144,  64);
    /* xB yB zC */ octree_set_empty_cube(ot, 144, 144, 208,  32);
    /* xB yC zA */ octree_set_empty_cube(ot, 144, 208,  16,  32);
    /* xB yC zB */ octree_set_empty_cube(ot, 144, 208, 144,  32);
    /* xB yC zC */ octree_set_empty_cube(ot, 144, 208, 208,  32);

    /* xC yA zA */ octree_set_empty_cube(ot, 208,  16,  16,  32);
    /* xC yA zB */ octree_set_empty_cube(ot, 208,  16, 144,  32);
    /* xC yA zC */ octree_set_empty_cube(ot, 208,  16, 208,  32);
    /* xC yB zA */ octree_set_empty_cube(ot, 208, 144,  16,  32);
    /* xC yB zB */ octree_set_empty_cube(ot, 208, 144, 144,  32);
    /* xC yB zC */ octree_set_empty_cube(ot, 208, 144, 208,  32);
    /* xC yC zA */ octree_set_empty_cube(ot, 208, 208,  16,  32);
    /* xC yC zB */ octree_set_empty_cube(ot, 208, 208, 144,  32);
    /* xC yC zC */ octree_set_empty_cube(ot, 208, 208, 208,  32);

    /* ---- 3. Structures ---- */

    /* === Low cover: six 32x32x32 pillars on the floor ===
       Sit on floor (y=16), rise 32 units to y=48. */
    octree_set_solid_cube(ot,  48, 16,  48, 32);   /* NW */
    octree_set_solid_cube(ot, 176, 16,  48, 32);   /* NE */
    octree_set_solid_cube(ot,  48, 16, 176, 32);   /* SW */
    octree_set_solid_cube(ot, 176, 16, 176, 32);   /* SE */
    octree_set_solid_cube(ot, 112, 16,  80, 32);   /* N-centre */
    octree_set_solid_cube(ot, 112, 16, 144, 32);   /* S-centre */

    /* === Central raised platform: 64x32x64 at centre ===
       Base at y=16, top face at y=48, air above from y=48. */
    octree_set_solid_cube(ot, 96, 16, 96, 64);
    octree_set_empty_cube(ot, 96, 48, 96, 64);     /* hollow above slab */

    /* === Two mid-level shelves along west wall (x=16..48) ===
       Shelf A: z[80..144], y[80..96]  (16-unit tall slab) */
    octree_set_solid_cube(ot, 16, 80,  80, 64);
    octree_set_empty_cube(ot, 16, 96,  80, 64);    /* air above shelf A */

    /* Shelf B: z[144..208], y[112..128] */
    octree_set_solid_cube(ot, 16, 112, 144, 64);
    octree_set_empty_cube(ot, 16, 128, 144, 64);

    /* === Two mid-level shelves along east wall (x=192..240) === */
    octree_set_solid_cube(ot, 192, 80,  80, 64);
    octree_set_empty_cube(ot, 192, 96,  80, 64);

    octree_set_solid_cube(ot, 192, 112, 144, 64);
    octree_set_empty_cube(ot, 192, 128, 144, 64);

    /* === High bridge: spans full z, sits at y=144, 16 thick ===
       x[112..128], y[144..160], z[16..240]
       Use two 128-wide z segments to cover z[16..240]:  */
    octree_set_solid_cube(ot, 112, 144,  16, 16);   /* x=16 cube for alignment */
    octree_set_solid_cube(ot, 112, 144,  16, 128);  /* z[16..144] */
    octree_set_empty_cube(ot, 112, 160,  16, 128);  /* air above */
    octree_set_solid_cube(ot, 112, 144, 144,  64);  /* z[144..208] */
    octree_set_empty_cube(ot, 112, 160, 144,  64);
    octree_set_solid_cube(ot, 112, 144, 208,  32);  /* z[208..240] */
    octree_set_empty_cube(ot, 112, 160, 208,  32);

    /* === Floating island: 64x16x64 at y=176 ===
       Centre of the map, requires rocket jump to reach. */
    octree_set_solid_cube(ot, 96, 176, 96, 64);
    octree_set_empty_cube(ot, 96, 192, 96, 64);     /* open top */

    /* === NW tower: x[16..48], z[16..48], y[16..176] ===
       Leave 16-unit interior hollow so player can stand inside. */
    octree_set_solid_cube(ot, 16, 48, 16, 32);      /* tower body above floor */
    octree_set_empty_cube(ot, 24, 48, 24, 16);      /* interior hollow */
    octree_set_solid_cube(ot, 16, 160, 16, 32);     /* cap */
    octree_set_empty_cube(ot, 24, 160, 24, 16);     /* open top room */

    /* === SE tower: x[208..240], z[208..240] === */
    octree_set_solid_cube(ot, 208, 48, 208, 32);
    octree_set_empty_cube(ot, 216, 48, 216, 16);
    octree_set_solid_cube(ot, 208, 160, 208, 32);
    octree_set_empty_cube(ot, 216, 160, 216, 16);
}
