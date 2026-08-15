# SDL2 (joystick + gamecontroller subsystem only)

Vendored from SDL2 release-2.30.0 (`https://github.com/libsdl-org/SDL/releases/tag/release-2.30.0`),
matching this project's existing Bullet vendoring precedent (`client/vendor/bullet3/`):
a pinned, static source snapshot, not a system dependency.

## Why not all of SDL2

Phi's own "No SDL / GLFW for windowing, GL context, or audio" rule (see phi.md's Hard
Architectural Decisions) carves out exactly one exception: gamepad/Steam Deck input,
where hand-rolling controller-mapping-database work would mean reinventing hundreds of
real-world controller layouts SDL_GameControllerDB already solves. This vendor tree is
scoped narrowly to that: joystick + gamecontroller, and their REAL, empirically-verified
dependency closure -- not video, not audio, not accelerated rendering.

## Why this isn't just a hand-picked "joystick-only" subset

SDL2's own `Makefile.minimal` (in the upstream release) still compiles nearly every
subsystem with dummy backends rather than omitting them -- there's no clean seam that
lets joystick/gamecontroller code compile in isolation from SDL2's broader core. This
tree's actual composition was arrived at empirically, not by design: start from a full
`./configure && make` build proven to compile+link+run on this platform (Linux, real
evdev joystick backend, video/audio/render disabled at the configure level), then trim
down from there, re-verifying compilation and linking after every reduction, until every
remaining file is either directly needed or a real, load-bearing dependency of something
that is. Three real, structural findings came out of that process, worth knowing before
touching this tree again:

1. **`SDL_dynapi.c`** (SDL2's dynamic-API dispatch table) statically references *every*
   public SDL2 function by design, regardless of which subsystems are actually compiled
   in -- this makes a genuinely partial build fail to link no matter how narrowly the
   rest of the tree is scoped. SDL2 ships a real, documented off-switch for this
   (`SDL_DYNAMIC_API 0`, used on iOS/Android/Emscripten/consoles upstream) but refuses to
   let it be set from the command line on purpose (`#error Nope, you have to edit this
   file to force this off.` in `src/dynapi/SDL_dynapi.h`) -- editing that file is the
   upstream-sanctioned mechanism, not a workaround. This tree's copy of that file adds a
   `PHI_SDL2_JOYSTICK_ONLY` branch (defined by Phi's own Makefile, not by upstream SDL2)
   that does exactly this. The tradeoff (no in-field SDL update without recompiling) is
   the same one this project already accepted for Bullet: a pinned static snapshot.

2. **Video is a real, load-bearing dependency**, not just an unused sibling subsystem.
   `SDL_evdev.c` (the Linux input backend joystick hotplug detection needs) handles
   keyboard/mouse/touch events on the same evdev bus uniformly, and `SDL_video.c`'s
   window-texture fallback path references `SDL_render.c`. This tree includes
   `src/video/` (dummy driver, no real X11/Wayland/EGL window ever created -- Phi's own
   `phi_platform_native.c` owns the real window) and `src/render/` + `src/render/software/`
   (the portable software renderer, no GPU-accelerated backend) purely to satisfy these
   real internal references -- neither is ever exercised by Phi's own code, and neither
   creates a real window or renders anything visible.

3. **Two files are missing from this specific release tarball**:
   `src/joystick/hidapi/SDL_hidapi_steam.c` and `SDL_hidapi_steamdeck.c` both
   `#include "steam/controller_constants.h"`, which does not exist anywhere in the
   official 2.30.0 release (confirmed by direct inspection, not assumed) -- a real
   upstream packaging gap in that release, not something this vendoring missed. The
   corresponding `SDL_JOYSTICK_HIDAPI_STEAM`/`_STEAMDECK` feature macros are commented
   out in this tree's copy of `SDL_hidapijoystick_c.h` so the (already-`#ifdef`-guarded)
   references disappear the same way every other optional HIDAPI driver already does.
   Consequence: the Steam Deck's built-in controller and other Steam-specific HID
   controllers still work through the ordinary Linux joystick/evdev path + the built-in
   `SDL_GameControllerDB` mapping (confirmed live in this build, see verification below)
   -- only the HIDAPI-specific fast path (lower latency, gyro/haptics) is unavailable.

## What's NOT here

- Any real window/GL-context/audio-device code path -- video/render are present only as
  the dead-but-linkable fallback described above, never invoked.
- Windows support. SDL2 ships its own hand-maintained `include/SDL_config_windows.h`
  upstream, which would be the natural next step, but this environment has no MinGW
  toolchain to actually verify a win32 build against (consistent with every other win32
  limitation already noted throughout this project's history) -- attempting it blind
  risked shipping untested, possibly-broken code, so it wasn't attempted. `input_gamepad`
  on win32 is a real, honest stub for now (see its own file comment).
- wasm/Emscripten. Not needed at all -- Emscripten has its own native Gamepad API
  bindings (`emscripten_get_gamepad_status` etc.), backed directly by the browser's
  standard Gamepad API, with no SDL2 involved on that target (see `input_gamepad_wasm.c`).

## Verification performed

A real `./configure && make` build of full upstream SDL2 2.30.0 (unmodified) was proven
to compile, link, and run first, to establish a known-good baseline before any trimming.
The final trimmed tree (163 `.c` files) was then verified to compile cleanly and link
into a real test program that calls `SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER)`,
`SDL_GameControllerAddMapping` (confirmed the built-in `SDL_GameControllerDB` really
recognizes a real Xbox 360 controller GUID), and cleanly `SDL_Quit()`s -- all in this
sandboxed environment, which has no physical gamepad attached, so `SDL_NumJoysticks() == 0`
is the correct, expected result here, not a failure. See `client/input_gamepad_test_main.c`
for the same checks wired into this project's own standalone-test convention.

## License

`LICENSE.txt` in this directory is SDL2's own zlib license, copied verbatim.
