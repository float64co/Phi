#pragma once

/* Phase 10 -- Audio (see phi.md's "Phase 10 -- Audio" section: no sound
 * system existed anywhere in this codebase before this, a genuinely
 * unaddressed gap found 2026-08-18 while assessing what Phi actually
 * needs to ship a real game, not a hole in an existing phase). Same
 * "hand-roll it, thin real C API over a real platform-native mechanism"
 * shape phi_platform.h already established for windowing/GL -- this is
 * the "or audio" clause in Hard Architectural Decisions' "No SDL/GLFW
 * for windowing, GL context, or audio" row, finally built rather than
 * just reserved.
 *
 * Three backends, matching phi_platform.h's own native/wasm/win32 split:
 *   audio_native.c -- Linux, real ALSA PCM output + a hand-rolled
 *                      sample-mixing thread. Real playback IF the build
 *                      environment has ALSA's dev headers (checked at
 *                      `make` time -- see the Makefile's ALSA_HEADER
 *                      detection); an honest, real "no audio device"
 *                      fallback otherwise, same shape input_gamepad_
 *                      win32_stub.c already established for an
 *                      unavailable backend -- never a silent, half-
 *                      working guess.
 *   audio_wasm.c    -- the real, standard Web Audio API via EM_ASM, same
 *                      pattern input_gamepad_wasm.c already established
 *                      for the browser Gamepad API. No SDL involved.
 *   audio_win32_stub.c -- honest stub (see input_gamepad_win32_stub.c's
 *                      own precedent for why Windows isn't wired up yet).
 *
 * Scope, stated plainly: uncompressed PCM WAV only (mono or stereo,
 * 16-bit signed, any sample rate -- resampled via simple linear
 * interpolation to the mixer's fixed internal rate at load time, real
 * but basic, not a high-quality resampler). No streaming (a whole sound
 * is decoded into memory up front), no real DSP/effects chains, no
 * compressed formats (Ogg/MP3) -- all real, separate future work a user
 * could layer on top of these primitives, not promised here. "3D" audio
 * is real but simple: per-voice stereo pan + distance attenuation
 * computed from the listener's position/basis, no HRTF. */

#include "vec3.h"

typedef struct PhiSound PhiSound;   /* opaque: one loaded, decoded clip */
typedef int PhiAudioVoice;          /* a playing instance; -1 = invalid/failed to play */

#ifdef __cplusplus
extern "C" {
#endif

/* Call once at startup, after phi_platform_init (same ordering
 * convention phi_gamepad_init already follows). Safe to call even if no
 * audio device is available -- every other phi_audio_* call below
 * degrades to a real, honest no-op rather than crashing, matching
 * PhiGamepadState's own "safe, just meaningless" convention when nothing
 * is connected. */
void phi_audio_init(void);

/* Call once at shutdown. Safe even if phi_audio_init failed or was never
 * called. */
void phi_audio_shutdown(void);

/* Loads an uncompressed PCM WAV file fully into memory, decoded to the
 * mixer's own internal sample format up front (no per-frame decode
 * cost). Returns NULL on a missing file, a non-WAV/compressed file, or a
 * genuinely unsupported PCM layout -- callers should check for NULL the
 * same way phi_gamepad_get_state's own callers check ->connected, not
 * assume every load succeeds. Ownership: the returned PhiSound is
 * reference-counted internally by however many voices are currently
 * playing it; never call any kind of "unload" function while a voice
 * might still be using it -- there isn't one yet (a real, small, and
 * separately-scoped gap, not silently promised solved here). */
PhiSound *phi_audio_load_sound(const char *path);

/* Plays `sound` as a new, non-positional voice at the given volume
 * (0.0-1.0, values outside that range are clamped) -- loop=1 repeats
 * forever until phi_audio_stop is called, loop=0 plays once and the
 * voice is automatically freed when it finishes. Returns a voice handle
 * (>=0) callers can later pass to phi_audio_stop, or -1 if playback
 * couldn't start (no audio device, or every mixer voice slot is
 * currently busy -- a real, bounded-capacity registry, same convention
 * PHI_GAMEPAD_MAX/SCENE_MAX_OBJECTS/PHI_MAX_LIGHTS already establish
 * elsewhere in this codebase). */
PhiAudioVoice phi_audio_play(PhiSound *sound, float volume, int loop);

/* Same as phi_audio_play, but volume is additionally attenuated by
 * distance from the last phi_audio_set_listener call (inverse-distance
 * falloff, clamped so nearby sounds don't divide-by-near-zero) and
 * panned left/right by `position`'s offset along the listener's own
 * right axis -- a real, simple stereo pan, not full 3D HRTF (see this
 * header's own top comment). */
PhiAudioVoice phi_audio_play_3d(PhiSound *sound, Vec3f position, float volume, int loop);

/* Stops a currently-playing voice immediately. Safe to call with an
 * already-finished or invalid voice handle (a real, harmless no-op, not
 * a caller bug) -- the same "already gone is a fine outcome, not an
 * error" convention phi_physics_remove_body's own callers don't get, but
 * fits this API's own real usage shape better (a game stopping a looped
 * sound has no reliable way to know if it already finished on its own,
 * unlike a body it explicitly owns the lifetime of). */
void phi_audio_stop(PhiAudioVoice voice);

/* Sets the real 3D listener transform phi_audio_play_3d's distance/pan
 * math uses -- call once per frame with the camera's own current
 * position/basis (see renderer.h's cam_basis-shaped fwd/right vectors)
 * if any 3D playback is in use; a game using only phi_audio_play (non-
 * positional) never needs to call this at all. */
void phi_audio_set_listener(Vec3f position, Vec3f forward, Vec3f right);

#ifdef __cplusplus
}
#endif
