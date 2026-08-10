#!/usr/bin/env python3
"""
Generates a hand-built animated, SKINNED armature test fixture for Phase
4 (see phi.md's "Phase 4 -- Animation Editor"): a 3-joint bone chain
(root -> mid -> tip, each offset (0,2,0) from its parent) with a skin
(inverse bind matrices), one animation clip that rotates the mid joint
90 degrees about Z (LINEAR, 2 keyframes), AND a real skinned mesh -- a
6-vertex/4-triangle strip running the length of the arm, with real
POSITION/NORMAL/JOINTS_0/WEIGHTS_0 attributes -- exercising
client/skinned_mesh.c's loading path, not just armature.c/animation.c's
skeleton-only parsing (see this file's own earlier revision, before the
mesh was added, for that narrower scope).

Weighting is deliberately NOT all-single-bone: most vertices are 100%
weighted to their nearest joint, but one (the mid/+x cross-section
vertex) is a real 60/40 blend between the mid and tip joints, so the
loader's WEIGHTS_0 handling is exercised against genuine multi-bone
blending, not just the trivial (1,0,0,0) case.

JOINTS_0 indices are into skin.joints (per the glTF spec), which is
itself in the same deliberately-scrambled [tip, root, mid] order
armature_test's own fixture already used -- so joint-array index 0 is
"tip", 1 is "root", 2 is "mid" here, NOT node index order.

Follows the same "hand-authored via a one-off script, no external
tooling" precedent assets/cube.gltf/tools/gen_test_assets.py already
established.

Usage: python3 tools/gen_test_armature.py
"""
import json
import math
import os
import struct

OUT_DIR = os.path.join(os.path.dirname(__file__), '..', 'assets', 'test')

# Rest-pose local translations (each relative to its own parent) -- root at
# origin, mid and tip each offset (0,2,0) from their parent, so world rest
# positions are root=(0,0,0), mid=(0,2,0), tip=(0,4,0).
BONE_LOCAL_T = [(0.0, 0.0, 0.0), (0.0, 2.0, 0.0), (0.0, 2.0, 0.0)]
BONE_WORLD_T = [(0.0, 0.0, 0.0), (0.0, 2.0, 0.0), (0.0, 4.0, 0.0)]
BONE_PARENT = [-1, 0, 1]
BONE_NAMES = ["root", "mid", "tip"]

# Animation: mid joint (index 1) rotates from identity to 90 degrees about
# Z over 1 second, LINEAR. Quaternion (x,y,z,w) for a +Z-axis rotation by
# angle theta is (0,0,sin(theta/2),cos(theta/2)).
HALF90 = math.radians(90.0) * 0.5
KEYFRAME_TIMES = [0.0, 1.0]
KEYFRAME_QUATS = [(0.0, 0.0, 0.0, 1.0), (0.0, 0.0, math.sin(HALF90), math.cos(HALF90))]


def inverse_bind_matrix(world_t):
    """Inverse of a pure-translation world rest transform: translate by
    -world_t, column-major mat4 (glTF convention)."""
    x, y, z = world_t
    return [
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        -x, -y, -z, 1.0,
    ]


# Skinned mesh: 6 vertices, a 2-wide strip running the length of the arm
# (x = -0.5 / +0.5 at each of the three bone heights), 4 triangles.
# JOINTS_0 values are indices into skin.joints ([tip, root, mid], see
# JOINT_ORDER below) -- 0=tip, 1=root, 2=mid.
MESH_POSITIONS = [
    (-0.5, 0.0, 0.0), (0.5, 0.0, 0.0),   # v0,v1 -- root height
    (-0.5, 2.0, 0.0), (0.5, 2.0, 0.0),   # v2,v3 -- mid height
    (-0.5, 4.0, 0.0), (0.5, 4.0, 0.0),   # v4,v5 -- tip height
]
MESH_NORMALS = [(0.0, 0.0, 1.0)] * 6
# v3 is a real 60/40 blend between mid(2) and tip(0) -- every other
# vertex is a trivial single-bone (1,0,0,0) weight.
MESH_JOINTS = [
    (1, 0, 0, 0), (1, 0, 0, 0),   # v0,v1 -> root
    (2, 0, 0, 0), (2, 0, 0, 0),   # v2,v3 -> mid (v3 overridden below)
    (0, 0, 0, 0), (0, 0, 0, 0),   # v4,v5 -> tip
]
MESH_WEIGHTS = [
    (1.0, 0.0, 0.0, 0.0), (1.0, 0.0, 0.0, 0.0),
    (1.0, 0.0, 0.0, 0.0), (0.6, 0.0, 0.0, 0.0),
    (1.0, 0.0, 0.0, 0.0), (1.0, 0.0, 0.0, 0.0),
]
MESH_JOINTS[3] = (2, 0, 0, 0)   # mid=2, tip=0
MESH_WEIGHTS[3] = (0.6, 0.4, 0.0, 0.0)
MESH_TRIANGLES = [(0, 1, 2), (1, 3, 2), (2, 3, 4), (3, 5, 4)]


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    buf = bytearray()

    def pad_to_4():
        while len(buf) % 4 != 0:
            buf.append(0)

    def add_floats(values):
        pad_to_4()
        off = len(buf)
        for v in values:
            buf.extend(struct.pack('<f', v))
        return off, len(buf) - off

    def add_ushorts(values):
        pad_to_4()
        off = len(buf)
        for v in values:
            buf.extend(struct.pack('<H', v))
        return off, len(buf) - off

    def add_ubytes(values):
        pad_to_4()
        off = len(buf)
        for v in values:
            buf.extend(struct.pack('<B', v))
        return off, len(buf) - off

    # Accessor 0: inverseBindMatrices, MAT4 x3 -- ORDER MUST MATCH
    # skin.joints below (tip, root, mid = node indices 2, 0, 1), not node
    # index order, per the glTF spec (the Nth inverse bind matrix
    # corresponds to the Nth entry of skin.joints).
    JOINT_ORDER = [2, 0, 1]
    ibm_flat = []
    for node_idx in JOINT_ORDER:
        ibm_flat.extend(inverse_bind_matrix(BONE_WORLD_T[node_idx]))
    ibm_off, ibm_len = add_floats(ibm_flat)

    # Accessor 1: animation input (times), SCALAR x2
    time_off, time_len = add_floats(KEYFRAME_TIMES)

    # Accessor 2: animation output (rotations), VEC4 x2
    quat_flat = []
    for q in KEYFRAME_QUATS:
        quat_flat.extend(q)
    quat_off, quat_len = add_floats(quat_flat)

    # Accessor 3: mesh POSITION, VEC3 x6
    pos_flat = [c for v in MESH_POSITIONS for c in v]
    pos_off, pos_len = add_floats(pos_flat)
    pos_min = [min(v[i] for v in MESH_POSITIONS) for i in range(3)]
    pos_max = [max(v[i] for v in MESH_POSITIONS) for i in range(3)]

    # Accessor 4: mesh NORMAL, VEC3 x6
    norm_flat = [c for v in MESH_NORMALS for c in v]
    norm_off, norm_len = add_floats(norm_flat)

    # Accessor 5: mesh JOINTS_0, VEC4 x6, UNSIGNED_BYTE (component type 5121)
    joints_flat = [c for v in MESH_JOINTS for c in v]
    joints_off, joints_len = add_ubytes(joints_flat)

    # Accessor 6: mesh WEIGHTS_0, VEC4 x6, FLOAT
    weights_flat = [c for v in MESH_WEIGHTS for c in v]
    weights_off, weights_len = add_floats(weights_flat)

    # Accessor 7: mesh triangle indices, SCALAR x12, UNSIGNED_SHORT
    indices_flat = [i for tri in MESH_TRIANGLES for i in tri]
    idx_off, idx_len = add_ushorts(indices_flat)

    bin_path = os.path.join(OUT_DIR, 'armature_test.bin')
    with open(bin_path, 'wb') as f:
        f.write(bytes(buf))

    gltf = {
        "asset": {"version": "2.0", "generator": "phi-armature-testgen (one-off script, not part of the build)"},
        "buffers": [{"uri": "armature_test.bin", "byteLength": len(buf)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": ibm_off, "byteLength": ibm_len},                     # 0: IBM
            {"buffer": 0, "byteOffset": time_off, "byteLength": time_len},                   # 1: anim input
            {"buffer": 0, "byteOffset": quat_off, "byteLength": quat_len},                   # 2: anim output
            {"buffer": 0, "byteOffset": pos_off, "byteLength": pos_len, "target": 34962},     # 3: POSITION
            {"buffer": 0, "byteOffset": norm_off, "byteLength": norm_len, "target": 34962},   # 4: NORMAL
            {"buffer": 0, "byteOffset": joints_off, "byteLength": joints_len, "target": 34962}, # 5: JOINTS_0
            {"buffer": 0, "byteOffset": weights_off, "byteLength": weights_len, "target": 34962}, # 6: WEIGHTS_0
            {"buffer": 0, "byteOffset": idx_off, "byteLength": idx_len, "target": 34963},     # 7: indices
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "MAT4"},
            {"bufferView": 1, "componentType": 5126, "count": 2, "type": "SCALAR",
             "min": [KEYFRAME_TIMES[0]], "max": [KEYFRAME_TIMES[-1]]},
            {"bufferView": 2, "componentType": 5126, "count": 2, "type": "VEC4"},
            {"bufferView": 3, "componentType": 5126, "count": 6, "type": "VEC3", "min": pos_min, "max": pos_max},
            {"bufferView": 4, "componentType": 5126, "count": 6, "type": "VEC3"},
            {"bufferView": 5, "componentType": 5121, "count": 6, "type": "VEC4"},
            {"bufferView": 6, "componentType": 5126, "count": 6, "type": "VEC4"},
            {"bufferView": 7, "componentType": 5123, "count": len(indices_flat), "type": "SCALAR"},
        ],
        "nodes": [
            {"name": BONE_NAMES[0], "translation": list(BONE_LOCAL_T[0]), "children": [1]},
            {"name": BONE_NAMES[1], "translation": list(BONE_LOCAL_T[1]), "children": [2]},
            {"name": BONE_NAMES[2], "translation": list(BONE_LOCAL_T[2])},
            {"name": "arm_mesh", "mesh": 0, "skin": 0},
        ],
        # Deliberately NOT parent-before-child order (tip, root, mid) --
        # the glTF spec doesn't guarantee joints-array ordering, so
        # client/armature.c's loader must topologically sort rather than
        # assume it; this fixture exercises that instead of the trivial
        # already-sorted case.
        "skins": [
            {"name": "test_armature", "joints": JOINT_ORDER, "inverseBindMatrices": 0}
        ],
        "meshes": [
            {
                "primitives": [{
                    "attributes": {
                        "POSITION": 3, "NORMAL": 4, "JOINTS_0": 5, "WEIGHTS_0": 6,
                    },
                    "indices": 7,
                    "mode": 4,
                }]
            }
        ],
        "animations": [
            {
                "name": "rotate_mid_90z",
                "samplers": [{"input": 1, "output": 2, "interpolation": "LINEAR"}],
                "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}],
            }
        ],
        # Both the joint hierarchy (node 0's subtree) and the skinned mesh
        # node (node 3) are top-level scene nodes -- a skinned mesh node
        # is conventionally a SIBLING of its skeleton's root, not a child
        # of it (its own vertex positions are already in the same space
        # the joints' world transforms are, via the inverse bind matrices,
        # not relative to a joint's local transform).
        "scenes": [{"nodes": [0, 3]}],
        "scene": 0,
    }

    gltf_path = os.path.join(OUT_DIR, 'armature_test.gltf')
    with open(gltf_path, 'w') as f:
        json.dump(gltf, f, indent=2)

    print(f"wrote {gltf_path} ({os.path.getsize(gltf_path)} bytes)")
    print(f"wrote {bin_path} ({os.path.getsize(bin_path)} bytes)")
    print(f"bone world rest positions: {BONE_WORLD_T}")
    print(f"mid-joint animation: identity at t=0 -> 90deg about Z at t=1, quat {KEYFRAME_QUATS[1]}")
    print(f"mesh: {len(MESH_POSITIONS)} verts, {len(MESH_TRIANGLES)} triangles, v3 blend weights {MESH_WEIGHTS[3]} joints {MESH_JOINTS[3]}")


if __name__ == '__main__':
    main()
