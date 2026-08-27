#pragma once

/* Phi's public C API -- the header a standalone game's `game/src/ *.c`
 * includes (see phi.md's Phase 9, "Shipping a Standalone Game"). This is
 * an AGGREGATING header, not an independently-curated/re-declared one:
 * every subsystem it exposes already has its own clean, real, opaque-
 * handle, plain-C header (phi_physics.h's opaque PhiPhysicsWorld and
 * PhiRigidBody pointer types being the clearest example -- no Bullet or
 * C++ type ever leaks through it),
 * so there is nothing to hide or rewrite. Re-declaring the same functions
 * a second time here would just be a second copy to keep in sync with
 * the first every time one of these subsystems changes -- see mp_port.c's
 * own recent "excessive duplication" pass for why that's a real, worth-
 * avoiding cost, not a hypothetical one. If a subsystem's current header
 * ever turns out to leak something genuinely internal, the fix is to
 * clean up THAT header (as phi_physics.h already was, deliberately, from
 * day one), not to paper over it with a second, hand-curated one here.
 *
 * Native `game/src/ *.c` builds always have a real local C toolchain (see
 * Phase 9's Distribution Model note), so this is compiled and statically
 * linked directly into the shipped executable -- no cloud compilation
 * endpoint, no `.wasm` side module, no `emcc` involved anywhere in this
 * path (that machinery exists only for the separate browser-hosted
 * distribution channel, see phi.md's Distribution Model section).
 *
 * Covers physics, geometry, animation, node graphs, and C-level render
 * hooks. NOT included: the Phase 0 Python `@phi.render_pass` decorator
 * itself (Python-only, needs its own MicroPython binding work -- see
 * render_hooks.h's own note on this being the C half only) and Phase 6's
 * MicroPython-side @phi.node/phi.Graph binding (mp_port.c) -- a game/
 * src/ *.c author gets node_graph.h's raw C topology/evaluator-adjacent
 * data structure, not the Python decorator sugar built on top of it in
 * the editor/MicroPython context, since that's a different runtime.
 * Real, separate future work either way, not silently promised here.
 *
 * Startup-only calls, worth flagging explicitly since nothing in C
 * enforces this: scene_objects_init(), phi_graph_system_init(), and
 * render_hooks_init() each clear their whole registry -- call once at
 * program start (mirroring how player_main.c's future init sequence
 * would call them, the same way editor_main.c's real main() already
 * does), never mid-game, or every live MeshObject/graph/render hook a
 * script has already built goes away. phi_physics_world_create() is the
 * equivalent for physics -- called once to get the PhiPhysicsWorld* every
 * other phi_physics_* call in this header then takes as a parameter. */

#include "vec3.h"

/* Geometry: MeshObject entities (meshobject.h), the live scene registry
 * (scene_objects.h), the half-edge editable mesh structure and its
 * incremental CRUD (halfedge.h), and the extrude/inset/loop-cut editing
 * operations built on top of it (mesh_edit.h) -- the identical C
 * primitives mp_port.c's phi.create_mesh/get_vertices/add_face/
 * extrude_face/etc. Python bindings already call, just linked directly
 * rather than reached through MicroPython. */
#include "meshobject.h"
#include "scene_objects.h"
#include "halfedge.h"
#include "mesh_edit.h"

/* glTF import: halfedge_load_gltf (a full multi-mesh/multi-material/
 * textured scene, see its own comment) and texture_cache_load (the real
 * GL texture loader halfedge_load_gltf/skinned_mesh_load_gltf both need
 * registered once at startup via their own _register_texture_loader
 * calls -- player_main.c's game_init already does this before calling
 * into game/src/main.c, so a game/src/ *.c author only ever needs to call
 * the loaders themselves, not register anything). */
#include "halfedge_gltf.h"
#include "texture_cache.h"

/* Physics: Bullet, via phi_physics.h's own real C wrapper (opaque
 * PhiPhysicsWorld, PhiRigidBody, and PhiConstraint pointer types -- see
 * that header's own comment for why it's already public-shaped as-is: no
 * Bullet/C++ type ever appears in its signatures). */
#include "phi_physics.h"

/* Animation: Armature (bone hierarchy), AnimClip/AnimPlayback (Phase 4's
 * Clip/Curve/Playback layer -- a channel's keyframes ARE its curve), and
 * SkinnedMesh/SkinnedMeshObject (the GPU-skinning entity, mesh+armature+
 * clips+playback+transform in one struct) -- skinned_mesh_object.h
 * itself pulls in armature.h/animation.h/skinned_mesh.h, so only it needs
 * including here. All plain C, no GL (GPU upload/draw stays in
 * renderer.c, matching this codebase's existing "data model here, GL
 * elsewhere" split -- see skinned_mesh_object.h's own comment). */
#include "skinned_mesh_object.h"
/* Skinned character scene registry (skinned_scene_objects.h) -- the
 * SkinnedMeshObject twin of scene_objects.h's MeshObject registry above:
 * a game/src/ *.c author calls skinned_scene_object_add() + skinned_mesh_
 * object_load() to spawn a real animated character and player_main.c's
 * own render/update loop picks it up automatically from then on, same
 * "just add it to the registry" ergonomics as a MeshObject. */
#include "skinned_scene_objects.h"

/* Phase 6 node graphs: node_graph.h's C-owned topology (nodes, typed
 * links, params, editor positions) and Kahn's-algorithm topological
 * sort. This is the raw data structure a game/src/ *.c author would
 * evaluate against their own node-type registry (there's no MicroPython
 * here to reuse mp_port.c's @phi.node/phi.Graph sugar) -- see this
 * file's own top comment for why the Python decorator layer isn't
 * included. */
#include "node_graph.h"

/* C-level render-pass hooks: register a real callback at one of 4 named
 * pipeline insertion points (after_gbuffer/after_lighting/after_resolve/
 * after_tonemap) and get called mid-frame with gbuffer.c's real GBuffer*
 * -- see render_hooks.h's own comment on why that's the real struct, not
 * a faked opaque handle (game/src/ *.c is statically linked into the
 * same trusted binary as the engine core, so there's no real safety
 * boundary a thinner wrapper would actually buy). */
#include "render_hooks.h"

/* Gamepad/Steam Deck input: phi_gamepad_init/poll/shutdown and a real
 * per-slot PhiGamepadState (see input_gamepad.h for the full 15-button/
 * 6-axis layout). Since this header covers Phase 9's NATIVE distribution
 * channel specifically (see this file's own top comment -- game/src/
 * *.c always has a real local toolchain, no emcc involved), the backend
 * a game/src/ *.c author actually links against is native's real
 * vendored-SDL2 implementation (client/input_gamepad_native.c, Linux) or
 * win32's honest stub (client/input_gamepad_win32_stub.c, always reports
 * 0 gamepads -- see client/vendor/SDL2/VENDORED.md for why Windows isn't
 * wired up yet). The interface itself is fully portable either way; only
 * the backend a given target links determines whether it does anything. */
#include "input_gamepad.h"

/* Keyboard/mouse: real, portable held-key state (see input.h's own
 * PhiKey/InputState comment). Added alongside phi.md's Phase 9 "Known
 * gaps" close, 2026-08-18 -- the player build had no keyboard/mouse
 * input source of any kind before this. player_main.c's game_init(...)
 * call (see this file's own top comment) hands a `const InputState *`
 * straight through -- the real, live instance its own platform event
 * callbacks feed every frame -- so a game/src/main.c author reads it the
 * same way mp_port.c's phi.key_down/mouse_pos/mouse_button_down do
 * internally, just without the Python round trip. */
#include "input.h"

/* Camera control: renderer_set_camera/renderer_get_sky_color/etc. -- see
 * renderer.h's own comment on the Renderer struct's real fields (cam_pos/
 * cam_yaw/cam_pitch among them). Added alongside phi.md's Phase 9 "Known
 * gaps" close, 2026-08-18: player_main.c's game_init(...) call hands the
 * real, live Renderer* straight through, so a game/src/main.c author can
 * call renderer_set_camera on it directly -- no Python, no phi.
 * set_camera round trip needed on this path. */
#include "renderer.h"

/* Audio (Phase 10, see phi.md's "Phase 10 -- Audio" -- a genuinely
 * unaddressed system before this, not a gap in an existing phase):
 * phi_audio_load_sound/play/play_3d/stop/set_listener. Real ALSA
 * playback on native Linux IF this build environment has libasound2-dev
 * installed (checked at `make` time, see the Makefile's ALSA_HEADER
 * comment); sounds still load and decode for real either way (audio_
 * wav.c has no ALSA dependency at all), only actual playback is
 * affected. win32 is a real, honest stub for now (see audio_win32_
 * stub.c), matching input_gamepad_win32_stub.c's own precedent. */
#include "phi_audio.h"

/* Frame-pacing diagnostics: frame_pacer_get_stats() -- the same real load
 * average (and most recent frame's own work/present/idle split) editor_
 * main.c's/player_main.c's own PHI_TARGET_CPU_FRACTION cap measures and
 * acts on internally every frame, exposed read-only so a game/src/ *.c
 * author can build a debug HUD or perf overlay against real numbers
 * instead of guessing. frame_pacer.c itself never prints or logs any of
 * this on its own -- see frame_pacer.h's own comment on FramePacerStats
 * for the full field list and what each one means. */
#include "frame_pacer.h"
