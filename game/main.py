"""Phase 9 reference example -- a real, checked-in game/main.py demonstrating
the genuine init -> tick(dt) shape player_main.c expects (see phi.md's
"./game/ directory" section and client/player_main.c's own top comment),
UPDATED 2026-08-18 to also exercise Phase 9's gap-closing bindings (see
phi.md's Phase 9 "Known gaps"): a real positioned camera (phi.set_camera),
real whole-object movement (phi.set_object_position, not the vertex-
mutation hack the previous version of this file used), real keyboard input
(phi.key_down) with gamepad left-stick as an alternate input source when
one's connected (phi.gamepad_connected/axis), and real object-id-keyed
physics (phi.object_enable_physics) on objects with no editor "selection"
involved at all.

Not a throwaway: every call below is a real, already-proven `phi.*` binding
(see client/mp_phase9_gap_test_main.c for the same calls exercised directly
against MicroPython), and this script is what `make player &&
./build/phi_player` actually boots when run from the repo root. It
deliberately avoids assets/cube.gltf (an editor test fixture) -- phi.
create_mesh builds real geometry from scratch, so this example has no
dependency outside the engine itself. A real shipped game would instead
load real content from game/assets/ (see phi.md's Asset Browser "marking
assets for ./game/" note) via phi.mesh_object(path, x, y, z).
"""

# NOTE: no `import phi` -- phi_mp_init's bootstrap installs `phi` directly
# into this script's own global namespace (see mp_port.c's PHI_BOOTSTRAP,
# `phi = _PhiNamespace()`), it isn't a real importable module. Every
# existing mp_*_test_main.c harness calls phi.* the same bare way.

print("[game] main.py: initializing...")


def _box(hx, hy, hz):
    """8 verts/12 tris, centered on the object's own origin -- the one
    shape this whole example needs (player, ground, and the physics drop
    all reuse it at different scales), built from scratch via phi.
    create_mesh the same way the previous version of this file built its
    tetrahedron, just parametrized."""
    positions = [
        -hx, -hy, -hz,   hx, -hy, -hz,   hx, hy, -hz,   -hx, hy, -hz,
        -hx, -hy,  hz,   hx, -hy,  hz,   hx, hy,  hz,   -hx, hy,  hz,
    ]
    # Wound so each triangle's normal (right-hand rule over its own listed
    # vertex order) points OUTWARD from the box's center -- verified by
    # computing all 12 cross products by hand before landing this fix (the
    # first version had every single face backwards, which is exactly why
    # the scene rendered flat/dim: a backwards face only ever gets this
    # pipeline's fixed 0.3 ambient term, never the diffuse contribution,
    # since dot(normal, light_dir) comes out negative and clamps to 0).
    indices = [
        0, 2, 1,  0, 3, 2,      # -Z
        5, 7, 4,  5, 6, 7,      # +Z
        4, 3, 0,  4, 7, 3,      # -X
        1, 6, 5,  1, 2, 6,      # +X
        3, 6, 2,  3, 7, 6,      # +Y
        4, 1, 5,  4, 0, 1,      # -Y
    ]
    return positions, indices


def _paint(oid, color):
    """Every _box() is always 12 real triangles -- paints all of them the
    same flat color via phi.set_face_material (per-face, no "paint the
    whole object at once" call exists, so this just loops). Added after
    live feedback that ground/drop/player were all the same default
    (0.7,0.7,0.7) gray and impossible to tell apart on screen -- a real,
    already-proven API (see client/mp_node_test_main.c's own set_face_
    material checks), just not used by this script until now."""
    for f in range(12):
        phi.set_face_material(oid, f, color, 0.0, 0.6, (0.0, 0.0, 0.0))


# ---- Camera (phi.set_camera -- Phase 9 gap-closing) -- a real, fixed
# vantage point looking down at the play area, replacing player_main.c's
# own generic fallback camera entirely. ----
phi.set_camera(0.0, 9.0, 14.0, 0.0, -0.45)

# ---- Ground: a large, static (mass=0.0) physics body -- phi.
# object_enable_physics works on ANY object by id, no "selected object"
# involved (that concept doesn't exist in this chromeless build at all). ----
_ground_pos, _ground_idx = _box(20.0, 1.0, 20.0)
ground_id = phi.create_mesh(_ground_pos, _ground_idx, 0.0, -1.0, 0.0)
phi.object_enable_physics(ground_id, 0.0, 0.3)
_paint(ground_id, (0.15, 0.32, 0.15))   # dark green
print("[game] ground object_id =", ground_id, "(static physics body)")

# ---- A second box, dropped from height with a REAL dynamic physics body
# -- falls and lands on the ground above, entirely independent of the
# player object below (no selection, no single-global-slot limitation). ----
_drop_pos, _drop_idx = _box(1.0, 1.0, 1.0)
drop_id = phi.create_mesh(_drop_pos, _drop_idx, 4.0, 12.0, -3.0)
phi.object_enable_physics(drop_id, 1.0, 0.4)
_paint(drop_id, (0.9, 0.45, 0.1))   # orange
print("[game] drop object_id =", drop_id, "(dynamic physics body, will fall and land)")

# ---- The player: NOT physics-driven -- moved directly every frame via
# phi.set_object_position (Phase 9 gap-closing: a real whole-object
# transform, replacing the old per-vertex-mutation workaround this file
# used before that API existed). ----
_player_pos, _player_idx = _box(0.8, 0.8, 0.8)
player_id = phi.create_mesh(_player_pos, _player_idx, 0.0, 1.0, 3.0)
_paint(player_id, (0.2, 0.5, 0.9))   # blue
print("[game] player object_id =", player_id, "(keyboard/gamepad-controlled)")

light_id = phi.add_light('sun', 50.0, 100.0, 50.0)
print("[game] created light_id =", light_id)

# ---- Audio (Phase 10, see phi.md's "Phase 10 -- Audio") -- a real, short
# WAV asset (game/assets/beep.wav, procedurally generated -- a decaying
# 0.35s C5 tone, not a placeholder/silent file), loaded and played once
# here to prove the load path, then triggered again on demand from tick()
# below via phi.play_sound_3d, positioned at the player -- real 3D audio
# panned/attenuated against whatever the camera is currently doing (see
# player_main.c's own update_audio_listener, called every frame). On a
# build without a real audio device (see phi_audio.h's own ALSA-detection
# note), this still exercises the exact same real code path -- it just
# doesn't produce sound, the same honest degradation phi.gamepad_* already
# has when nothing's connected. ----
beep = phi.load_sound('game/assets/beep.wav')
print("[game] loaded sound handle =", beep)
phi.play_sound(beep, 0.6)

_px, _py, _pz = 0.0, 1.0, 3.0
_SPEED = 6.0
_space_was_down = False


def tick(dt):
    """Called once per frame by player_main.c with the real, capped frame
    delta -- see player_main.c's player_loop. Moves the player box on the
    X/Z plane: real keyboard input (phi.key_down) by default, or the
    first connected gamepad's left stick (phi.gamepad_connected/axis) when
    one's attached -- both real, previously-nonexistent Python input
    sources as of Phase 9's gap-closing pass."""
    global _px, _py, _pz, _space_was_down

    dx = dz = 0.0
    if phi.gamepad_connected(0):
        dx = phi.gamepad_axis(0, 'leftx')
        dz = phi.gamepad_axis(0, 'lefty')
    else:
        if phi.key_down('d'): dx += 1.0
        if phi.key_down('a'): dx -= 1.0
        if phi.key_down('s'): dz += 1.0
        if phi.key_down('w'): dz -= 1.0

    _px += dx * _SPEED * dt
    _pz += dz * _SPEED * dt
    phi.set_object_position(player_id, _px, _py, _pz)

    # Space (rising edge, tracked by hand -- phi.key_down only exposes
    # held state, same real primitive phi.md's Phase 9 "Known gaps" close
    # actually shipped, not a higher-level "just pressed" API) plays the
    # beep positionally at the player's current location -- real 3D audio,
    # not just the one-shot confirmation beep at init above.
    space_down = phi.key_down('space')
    if space_down and not _space_was_down:
        phi.play_sound_3d(beep, _px, _py, _pz, 0.8)
    _space_was_down = space_down
