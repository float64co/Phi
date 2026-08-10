#!/usr/bin/env python3
"""
Generates a minimal, hand-built animated-armature test fixture for Phase
4's Clip/Curve/Playback data layer (see phi.md's "Phase 4 -- Animation
Editor"): a 3-joint bone chain (root -> mid -> tip, each offset (0,2,0)
from its parent) with a skin (inverse bind matrices) and one animation
clip that rotates the mid joint 90 degrees about Z, LINEAR interpolation,
over 2 keyframes (t=0 identity, t=1 rotated).

Deliberately SKELETON-ONLY -- no mesh/skin-weight data -- since this
fixture's whole job is exercising cgltf's animation/skin parsing plus
client/armature.c's hierarchy walk and client/animation.c's keyframe
sampling, both of which are independent of any mesh being bound to the
skeleton. GPU vertex skinning (a real mesh + JOINTS_0/WEIGHTS_0) is later,
separately-scoped work, not this fixture's job. Follows the same
"hand-authored via a one-off script, no external tooling" precedent
assets/cube.gltf and tools/gen_test_assets.py already established.

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


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    buf = bytearray()

    def add_floats(values):
        off = len(buf)
        for v in values:
            buf.extend(struct.pack('<f', v))
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

    bin_path = os.path.join(OUT_DIR, 'armature_test.bin')
    with open(bin_path, 'wb') as f:
        f.write(bytes(buf))

    gltf = {
        "asset": {"version": "2.0", "generator": "phi-armature-testgen (one-off script, not part of the build)"},
        "buffers": [{"uri": "armature_test.bin", "byteLength": len(buf)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": ibm_off, "byteLength": ibm_len},
            {"buffer": 0, "byteOffset": time_off, "byteLength": time_len},
            {"buffer": 0, "byteOffset": quat_off, "byteLength": quat_len},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 3, "type": "MAT4"},
            {"bufferView": 1, "componentType": 5126, "count": 2, "type": "SCALAR",
             "min": [KEYFRAME_TIMES[0]], "max": [KEYFRAME_TIMES[-1]]},
            {"bufferView": 2, "componentType": 5126, "count": 2, "type": "VEC4"},
        ],
        "nodes": [
            {"name": BONE_NAMES[0], "translation": list(BONE_LOCAL_T[0]), "children": [1]},
            {"name": BONE_NAMES[1], "translation": list(BONE_LOCAL_T[1]), "children": [2]},
            {"name": BONE_NAMES[2], "translation": list(BONE_LOCAL_T[2])},
        ],
        # Deliberately NOT parent-before-child order (tip, root, mid) --
        # the glTF spec doesn't guarantee joints-array ordering, so
        # client/armature.c's loader must topologically sort rather than
        # assume it; this fixture exercises that instead of the trivial
        # already-sorted case.
        "skins": [
            {"name": "test_armature", "joints": JOINT_ORDER, "inverseBindMatrices": 0}
        ],
        "animations": [
            {
                "name": "rotate_mid_90z",
                "samplers": [{"input": 1, "output": 2, "interpolation": "LINEAR"}],
                "channels": [{"sampler": 0, "target": {"node": 1, "path": "rotation"}}],
            }
        ],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }

    gltf_path = os.path.join(OUT_DIR, 'armature_test.gltf')
    with open(gltf_path, 'w') as f:
        json.dump(gltf, f, indent=2)

    print(f"wrote {gltf_path} ({os.path.getsize(gltf_path)} bytes)")
    print(f"wrote {bin_path} ({os.path.getsize(bin_path)} bytes)")
    print(f"bone world rest positions: {BONE_WORLD_T}")
    print(f"mid-joint animation: identity at t=0 -> 90deg about Z at t=1, quat {KEYFRAME_QUATS[1]}")


if __name__ == '__main__':
    main()
