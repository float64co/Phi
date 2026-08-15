"""Phase 9 reference example -- a real, checked-in game/main.py demonstrating
the genuine init -> tick(dt) shape player_main.c expects (see phi.md's
"./game/ directory" section and client/player_main.c's own top comment).

Not a throwaway: every call below is a real, already-proven `phi.*` binding
(see client/mp_geometry_test_main.c / mp_node_test_main.c for the same calls
exercised directly against MicroPython), and this script is what
`make player && ./build/phi_player` actually boots when run from the repo
root. It deliberately avoids assets/cube.gltf (an editor test fixture) --
phi.create_mesh builds real geometry from scratch, so this example has no
dependency outside the engine itself. A real shipped game would instead
load real content from game/assets/ (see phi.md's Asset Browser "marking
assets for ./game/" note) via phi.mesh_object(path, x, y, z).
"""
import math

# NOTE: no `import phi` -- phi_mp_init's bootstrap installs `phi` directly
# into this script's own global namespace (see mp_port.c's PHI_BOOTSTRAP,
# `phi = _PhiNamespace()`), it isn't a real importable module. Every
# existing mp_*_test_main.c harness calls phi.* the same bare way.

print("[game] main.py: initializing...")

# A real, hand-authored tetrahedron (4 verts, 4 triangles) -- see
# phi.create_mesh's own signature: a flat x,y,z position sequence plus a
# flat triangle-index sequence, this engine's editable meshes are
# triangles-only.
_positions = [
    0.0, 0.0, 0.0,
    2.0, 0.0, 0.0,
    1.0, 0.0, 2.0,
    1.0, 2.0, 1.0,
]
_indices = [
    0, 1, 2,
    0, 1, 3,
    1, 2, 3,
    2, 0, 3,
]
oid = phi.create_mesh(_positions, _indices)
print("[game] created mesh object_id =", oid)

light_id = phi.add_light('sun', 50.0, 100.0, 50.0)
print("[game] created light_id =", light_id)

_t = 0.0


def tick(dt):
    """Called once per frame by player_main.c with the real, capped frame
    delta -- see player_main.c's player_loop. Bobs the tetrahedron up and
    down by actually re-translating its live vertices every frame
    (translate_mesh, one of the built-in geometry nodes from phi.md's
    Phase 6 vocabulary -- a thin wrapper over phi.get_vertices/
    set_vertices), proving this is a genuine per-frame callback and not a
    one-shot init script.
    """
    global _t
    _t += dt
    dy = math.sin(_t * 2.0) * dt * 4.0
    translate_mesh(oid, 0.0, dy, 0.0)
