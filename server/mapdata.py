#!/usr/bin/env python3
"""
Pure-Python port of client/octree.c + client/cmap.c.

This is what makes the server authoritative for level geometry: it can
apply the same solid/empty/material region edits the client's octree.c
supports, and read/write the identical binary .cmap format (v2) so a
client's cmap_deserialize() can load bytes straight off the wire.

It intentionally does NOT replace the server's collision/physics model —
GameWorld's movement simulation stays the existing simplified flat-floor
approximation in server.py. This module only backs persistence + broadcast.
"""

import os
import re
import struct

WORLD_SIZE       = 1024   # must match client/octree.h's WORLD_SIZE exactly —
MAX_OCTREE_DEPTH = 10     # apply_edit() clamps to WORLD_SIZE, so a mismatch
LEAF_SIZE        = WORLD_SIZE >> (MAX_OCTREE_DEPTH - 1)   # = 2   silently truncates/corrupts edits near or past the smaller bound.

NODE_EMPTY, NODE_SOLID, NODE_DEFORMED, NODE_SUBDIVIDED = 0, 1, 2, 3
FACE_NEG_X, FACE_POS_X, FACE_NEG_Y, FACE_POS_Y, FACE_NEG_Z, FACE_POS_Z = range(6)

CMAP_VERSION = 2
NAME_RE = re.compile(r'^[A-Za-z0-9_-]{1,32}$')


def valid_name(name: str) -> bool:
    return bool(NAME_RE.match(name))


class Node:
    __slots__ = ('type', 'corners', 'faces', 'children')

    def __init__(self, type_=NODE_EMPTY):
        self.type     = type_
        self.corners  = [0] * 8
        self.faces    = [0] * 6
        self.children = [None] * 8


def _child_idx(cx, cy, cz):
    return (cx & 1) | ((cy & 1) << 1) | ((cz & 1) << 2)


def _ensure_children(node):
    for i in range(8):
        if node.children[i] is None:
            node.children[i] = Node(NODE_EMPTY)


def _split_leaf(node):
    """Turn a leaf into 8 SUBDIVIDED children, propagating its old
    type/corners/faces so painted materials survive a later partial edit."""
    old_type, old_corners, old_faces = node.type, list(node.corners), list(node.faces)
    node.type = NODE_SUBDIVIDED
    _ensure_children(node)
    for c in node.children:
        c.type    = old_type
        c.corners = list(old_corners)
        c.faces   = list(old_faces)


def _try_merge(node):
    """Merge 8 children back into a leaf if identical (type, and faces when
    SOLID) — skips the merge rather than lose distinct face materials."""
    t0 = node.children[0].type
    if t0 not in (NODE_EMPTY, NODE_SOLID):
        return
    same = all(c.type == t0 for c in node.children)
    if same and t0 == NODE_SOLID:
        for f in range(6):
            f0 = node.children[0].faces[f]
            if any(c.faces[f] != f0 for c in node.children):
                same = False
                break
    if not same:
        return
    if t0 == NODE_SOLID:
        node.faces = list(node.children[0].faces)
    node.children = [None] * 8
    node.type = t0


def _set_region(node, nx, ny, nz, nsize,
                 rminx, rminy, rminz, rmaxx, rmaxy, rmaxz, target_type):
    if (nx >= rminx and ny >= rminy and nz >= rminz and
            nx + nsize <= rmaxx and ny + nsize <= rmaxy and nz + nsize <= rmaxz):
        node.children = [None] * 8
        node.type = target_type
        return
    if (nx + nsize <= rminx or nx >= rmaxx or
            ny + nsize <= rminy or ny >= rmaxy or
            nz + nsize <= rminz or nz >= rmaxz):
        return
    if nsize <= LEAF_SIZE:
        node.type = target_type
        return
    if node.type != NODE_SUBDIVIDED:
        _split_leaf(node)
    half = nsize >> 1
    for cz in range(2):
        for cy in range(2):
            for cx in range(2):
                idx = _child_idx(cx, cy, cz)
                _set_region(node.children[idx],
                            nx + cx*half, ny + cy*half, nz + cz*half, half,
                            rminx, rminy, rminz, rmaxx, rmaxy, rmaxz, target_type)
    _try_merge(node)


def set_solid(root, minx, miny, minz, maxx, maxy, maxz):
    _set_region(root, 0, 0, 0, WORLD_SIZE, minx, miny, minz, maxx, maxy, maxz, NODE_SOLID)


def set_empty(root, minx, miny, minz, maxx, maxy, maxz):
    _set_region(root, 0, 0, 0, WORLD_SIZE, minx, miny, minz, maxx, maxy, maxz, NODE_EMPTY)


def _set_material(node, nx, ny, nz, nsize,
                   rminx, rminy, rminz, rmaxx, rmaxy, rmaxz, face, mat):
    if node.type == NODE_EMPTY:
        return
    if (nx + nsize <= rminx or nx >= rmaxx or
            ny + nsize <= rminy or ny >= rmaxy or
            nz + nsize <= rminz or nz >= rmaxz):
        return
    fully_inside = (nx >= rminx and ny >= rminy and nz >= rminz and
                     nx + nsize <= rmaxx and ny + nsize <= rmaxy and nz + nsize <= rmaxz)
    if fully_inside and node.type != NODE_SUBDIVIDED:
        node.faces[face] = mat
        return
    if nsize <= LEAF_SIZE:
        node.faces[face] = mat
        return
    if node.type != NODE_SUBDIVIDED:
        _split_leaf(node)
    half = nsize >> 1
    for cz in range(2):
        for cy in range(2):
            for cx in range(2):
                idx = _child_idx(cx, cy, cz)
                _set_material(node.children[idx],
                              nx + cx*half, ny + cy*half, nz + cz*half, half,
                              rminx, rminy, rminz, rmaxx, rmaxy, rmaxz, face, mat)
    # Deliberately no _try_merge() — sibling leaves now carry distinct
    # face materials by design.


def set_material(root, minx, miny, minz, maxx, maxy, maxz, face, mat):
    _set_material(root, 0, 0, 0, WORLD_SIZE, minx, miny, minz, maxx, maxy, maxz, face, mat)


# ---------------------------------------------------------------------------
# Default map — line-for-line port of octree_make_default_map() in
# client/octree.c, so the server's baseline matches what a client would
# generate on its own before ever talking to the server.
# ---------------------------------------------------------------------------

def _solid_cube(root, x, y, z, s): set_solid(root, x, y, z, x + s, y + s, z + s)
def _empty_cube(root, x, y, z, s): set_empty(root, x, y, z, x + s, y + s, z + s)


def make_default_map():
    root = Node(NODE_EMPTY)

    _solid_cube(root, 0, 0, 0, 256)

    _empty_cube(root,  16,  16,  16, 128)
    _empty_cube(root,  16,  16, 144,  64)
    _empty_cube(root,  16,  16, 208,  32)
    _empty_cube(root,  16, 144,  16,  64)
    _empty_cube(root,  16, 144, 144,  64)
    _empty_cube(root,  16, 144, 208,  32)
    _empty_cube(root,  16, 208,  16,  32)
    _empty_cube(root,  16, 208, 144,  32)
    _empty_cube(root,  16, 208, 208,  32)

    _empty_cube(root, 144,  16,  16,  64)
    _empty_cube(root, 144,  16, 144,  64)
    _empty_cube(root, 144,  16, 208,  32)
    _empty_cube(root, 144, 144,  16,  64)
    _empty_cube(root, 144, 144, 144,  64)
    _empty_cube(root, 144, 144, 208,  32)
    _empty_cube(root, 144, 208,  16,  32)
    _empty_cube(root, 144, 208, 144,  32)
    _empty_cube(root, 144, 208, 208,  32)

    _empty_cube(root, 208,  16,  16,  32)
    _empty_cube(root, 208,  16, 144,  32)
    _empty_cube(root, 208,  16, 208,  32)
    _empty_cube(root, 208, 144,  16,  32)
    _empty_cube(root, 208, 144, 144,  32)
    _empty_cube(root, 208, 144, 208,  32)
    _empty_cube(root, 208, 208,  16,  32)
    _empty_cube(root, 208, 208, 144,  32)
    _empty_cube(root, 208, 208, 208,  32)

    _solid_cube(root,  48, 16,  48, 32)
    _solid_cube(root, 176, 16,  48, 32)
    _solid_cube(root,  48, 16, 176, 32)
    _solid_cube(root, 176, 16, 176, 32)
    _solid_cube(root, 112, 16,  80, 32)
    _solid_cube(root, 112, 16, 144, 32)

    _solid_cube(root, 96, 16, 96, 64)
    _empty_cube(root, 96, 48, 96, 64)

    _solid_cube(root, 16, 80,  80, 64)
    _empty_cube(root, 16, 96,  80, 64)

    _solid_cube(root, 16, 112, 144, 64)
    _empty_cube(root, 16, 128, 144, 64)

    _solid_cube(root, 192, 80,  80, 64)
    _empty_cube(root, 192, 96,  80, 64)

    _solid_cube(root, 192, 112, 144, 64)
    _empty_cube(root, 192, 128, 144, 64)

    _solid_cube(root, 112, 144,  16,  16)
    _solid_cube(root, 112, 144,  16, 128)
    _empty_cube(root, 112, 160,  16, 128)
    _solid_cube(root, 112, 144, 144,  64)
    _empty_cube(root, 112, 160, 144,  64)
    _solid_cube(root, 112, 144, 208,  32)
    _empty_cube(root, 112, 160, 208,  32)

    _solid_cube(root, 96, 176, 96, 64)
    _empty_cube(root, 96, 192, 96, 64)

    _solid_cube(root, 16,  48, 16, 32)
    _empty_cube(root, 24,  48, 24, 16)
    _solid_cube(root, 16, 160, 16, 32)
    _empty_cube(root, 24, 160, 24, 16)

    _solid_cube(root, 208,  48, 208, 32)
    _empty_cube(root, 216,  48, 216, 16)
    _solid_cube(root, 208, 160, 208, 32)
    _empty_cube(root, 216, 160, 216, 16)

    return root


# ---------------------------------------------------------------------------
# .cmap serialize/deserialize — must match client/cmap.c exactly (v2: faces
# are now written for SOLID nodes too, not just DEFORMED).
# ---------------------------------------------------------------------------

def _serialize_node(node, out: bytearray):
    out.append(node.type)
    if node.type == NODE_SOLID:
        out.extend(b & 0xFF for b in node.faces)
    elif node.type == NODE_DEFORMED:
        out.extend(c & 0xFF for c in node.corners)
        out.extend(b & 0xFF for b in node.faces)
    elif node.type == NODE_SUBDIVIDED:
        for c in node.children:
            if c is not None:
                _serialize_node(c, out)
            else:
                out.append(NODE_EMPTY)


def serialize(root) -> bytes:
    out = bytearray()
    out.extend(b'CMAP')
    out.extend(struct.pack('<H', CMAP_VERSION))
    out.append(10)           # world_size = log2(1024)
    out.extend(bytes(25))    # reserved
    _serialize_node(root, out)
    return bytes(out)


class _Reader:
    __slots__ = ('buf', 'pos')
    def __init__(self, buf, pos):
        self.buf, self.pos = buf, pos
    def u8(self):
        v = self.buf[self.pos]
        self.pos += 1
        return v


def _deserialize_node(r: '_Reader') -> Node:
    node = Node()
    node.type = r.u8()
    if node.type == NODE_SOLID:
        node.faces = [r.u8() for _ in range(6)]
    elif node.type == NODE_DEFORMED:
        raw = [r.u8() for _ in range(8)]
        node.corners = [(b - 256) if b >= 128 else b for b in raw]
        node.faces   = [r.u8() for _ in range(6)]
    elif node.type == NODE_SUBDIVIDED:
        node.children = [_deserialize_node(r) for _ in range(8)]
    return node


def deserialize(buf: bytes):
    if len(buf) < 32 or buf[0:4] != b'CMAP':
        return None
    ver = struct.unpack_from('<H', buf, 4)[0]
    if ver != CMAP_VERSION:
        return None
    return _deserialize_node(_Reader(buf, 32))


# ---------------------------------------------------------------------------
# Disk persistence
# ---------------------------------------------------------------------------

def save(root, maps_dir: str, name: str) -> str:
    if not valid_name(name):
        raise ValueError(f'invalid map name: {name!r}')
    os.makedirs(maps_dir, exist_ok=True)
    path = os.path.join(maps_dir, f'{name}.cmap')
    with open(path, 'wb') as f:
        f.write(serialize(root))
    return path


def load(maps_dir: str, name: str):
    if not valid_name(name):
        raise ValueError(f'invalid map name: {name!r}')
    path = os.path.join(maps_dir, f'{name}.cmap')
    if not os.path.isfile(path):
        return None
    with open(path, 'rb') as f:
        data = f.read()
    return deserialize(data)


# ---------------------------------------------------------------------------
# Ray cast — port of client/octree.c's octree_ray_cast(), used server-side
# for splash-damage occlusion (see server.py's _explode). Ported with the
# entry-distance fix already applied: when the ray origin starts inside a
# node (tmin < 0 — true for the root on nearly every cast, since rays
# always start somewhere inside the world), the entry distance is 0, not
# the node's far exit point. Returning the far point there made short-range
# casts wrongly reject nodes the origin was sitting right inside of; this
# was a real bug in the original C code, found and fixed alongside this port.
# ---------------------------------------------------------------------------

def _ray_vs_aabb(ox, oy, oz, idx, idy, idz, nx, ny, nz, nsize):
    t1 = (nx - ox) * idx
    t2 = (nx + nsize - ox) * idx
    t3 = (ny - oy) * idy
    t4 = (ny + nsize - oy) * idy
    t5 = (nz - oz) * idz
    t6 = (nz + nsize - oz) * idz
    tmin = max(max(min(t1, t2), min(t3, t4)), min(t5, t6))
    tmax = min(min(max(t1, t2), max(t3, t4)), max(t5, t6))
    if tmax < 0 or tmin > tmax:
        return None
    return 0.0 if tmin < 0 else tmin


def _ray_cast(node, nx, ny, nz, nsize, ox, oy, oz, idx, idy, idz, max_t):
    t = _ray_vs_aabb(ox, oy, oz, idx, idy, idz, nx, ny, nz, nsize)
    if t is None or t > max_t:
        return None
    if node.type in (NODE_SOLID, NODE_DEFORMED):
        return t
    if node.type == NODE_EMPTY:
        return None
    half = nsize >> 1
    best = None
    for cz in range(2):
        for cy in range(2):
            for cx in range(2):
                child = node.children[_child_idx(cx, cy, cz)]
                if child is None:
                    continue
                ht = _ray_cast(child, nx + cx*half, ny + cy*half, nz + cz*half, half,
                                ox, oy, oz, idx, idy, idz, max_t)
                if ht is not None and (best is None or ht < best):
                    best = ht
    return best


def ray_cast(root, origin, direction, max_t):
    """Returns the distance to the first solid hit along (origin, direction)
    within max_t, or None if nothing is hit. direction need not be
    pre-normalized, but max_t is in the same units as direction's length."""
    ox, oy, oz = origin
    dx, dy, dz = direction
    idx = 1.0/dx if dx != 0 else 1e30
    idy = 1.0/dy if dy != 0 else 1e30
    idz = 1.0/dz if dz != 0 else 1e30
    return _ray_cast(root, 0, 0, 0, WORLD_SIZE, ox, oy, oz, idx, idy, idz, max_t)
