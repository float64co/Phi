# Phi — multi-target build (Phase 0: platform abstraction)
# Requires: emcc (Emscripten SDK) for wasm, gcc + X11/GLX dev headers for
# native, and (only under WSL, for the win32 target) a Windows-side
# MinGW-w64 toolchain reachable via WSL interop.
#
# Usage:
#   make wasm     — build www/game.js + www/game.wasm via emcc
#   make native   — build build/phi_native (Xlib/GLX, OpenGL 3.3 core)
#   make win32    — build build/phi_win32.exe (Win32/WGL, OpenGL 3.3 core;
#                    windowing+rendering only — no input/networking yet)
#   make          — wasm + native (not win32 — opt-in, needs WSL + MinGW)
#   make run      — build wasm, then start the Python server
#   make clean    — remove build artifacts
#   make watch    — rebuild wasm on source change (requires inotifywait)

SRCDIR  := client
WWWDIR  := www
BUILDDIR:= build

HDRS := $(wildcard $(SRCDIR)/*.h)

# MicroPython embedding (Phase 5 first slice, now also Phase 1's Console-
# as-real-Python-REPL piece) -- client/micropython_embed/ is generated
# output from MicroPython's own ports/embed tooling (see
# client/mpconfigport.h's header comment); client/mp_port.c/mp_port.h are
# hand-written. Defined here (before COMMON_SRCS, which now includes it)
# rather than down in the self-test section below, since COMMON_SRCS uses
# immediate (:=) expansion and needs these already defined at that point.
MP_EMBED_DIR  := $(SRCDIR)/micropython_embed
MP_EMBED_SRCS := $(wildcard $(MP_EMBED_DIR)/*/*.c) $(wildcard $(MP_EMBED_DIR)/*/*/*.c)
MP_INCLUDES   := -I$(MP_EMBED_DIR) -I$(MP_EMBED_DIR)/port

# Bullet Physics (Phase 2, see phi.md's "Bullet Physics via Emscripten") --
# vendored minimal subset only: LinearMath+BulletCollision+BulletDynamics
# (see client/vendor/bullet3/), not BulletSoftBody/Bullet3*/examples/etc.
# C++ (Bullet itself, and client/phi_physics.cpp -- the hand-written C
# wrapper over it, since Bullet's core engine ships no real C API of its
# own despite phi.md's original text claiming otherwise, see
# phi_physics.h's correction) mixed into an otherwise all-C build: gcc/
# emcc's driver already dispatches .cpp files to the C++ front end
# correctly by file extension in the same single command every other
# target here already uses, so the only thing actually needed on top is
# -lstdc++ at the link step (added to each target's own LDFLAGS below).
# Adds real compile time (~150 vendored .cpp files, ~75s at -O1 on this
# sandbox) to every full rebuild -- this Makefile has never done per-
# object caching for anything (the MicroPython embed tree already
# recompiles from scratch on every build too), so this is the existing
# convention scaling up with a bigger vendored dependency, not a new
# problem pattern.
BULLET_DIR      := $(SRCDIR)/vendor/bullet3/src
BULLET_SRCS     := $(wildcard $(BULLET_DIR)/*/*.cpp) $(wildcard $(BULLET_DIR)/*/*/*.cpp)
BULLET_INCLUDES := -I$(BULLET_DIR)

# Gamepad/Steam Deck input (Phase 9, see phi.md's Hard Architectural
# Decisions -- narrow, documented exception to "No SDL/GLFW"): vendored
# SDL2 (release-2.30.0), joystick+gamecontroller subsystem ONLY -- native
# Linux only for now (see client/vendor/SDL2/VENDORED.md for the full
# story: what's in this tree and why, the real structural problems hit
# and how each was resolved, what's deliberately NOT here). NOT used on
# wasm (Emscripten has its own native Gamepad API bindings, see client/
# input_gamepad_wasm.c -- no SDL2 involved on that target at all) or
# win32 yet (no verified build environment for it this pass, see client/
# input_gamepad_win32_stub.c). PHI_SDL2_JOYSTICK_ONLY is Phi's own define
# (read by this tree's patched src/dynapi/SDL_dynapi.h), not an upstream
# SDL2 flag.
SDL2_DIR      := $(SRCDIR)/vendor/SDL2
SDL2_INCLUDES := -I$(SDL2_DIR)/include -I$(SDL2_DIR)/src/video/khronos -DPHI_SDL2_JOYSTICK_ONLY
SDL2_JOYSTICK_SRCS := \
	$(SRCDIR)/vendor/SDL2/src/SDL.c \
	$(SRCDIR)/vendor/SDL2/src/SDL_assert.c \
	$(SRCDIR)/vendor/SDL2/src/SDL_dataqueue.c \
	$(SRCDIR)/vendor/SDL2/src/SDL_error.c \
	$(SRCDIR)/vendor/SDL2/src/SDL_guid.c \
	$(SRCDIR)/vendor/SDL2/src/SDL_hints.c \
	$(SRCDIR)/vendor/SDL2/src/SDL_list.c \
	$(SRCDIR)/vendor/SDL2/src/SDL_log.c \
	$(SRCDIR)/vendor/SDL2/src/SDL_utils.c \
	$(SRCDIR)/vendor/SDL2/src/atomic/SDL_atomic.c \
	$(SRCDIR)/vendor/SDL2/src/atomic/SDL_spinlock.c \
	$(SRCDIR)/vendor/SDL2/src/audio/SDL_audio.c \
	$(SRCDIR)/vendor/SDL2/src/audio/SDL_audiocvt.c \
	$(SRCDIR)/vendor/SDL2/src/audio/SDL_audiodev.c \
	$(SRCDIR)/vendor/SDL2/src/audio/SDL_audiotypecvt.c \
	$(SRCDIR)/vendor/SDL2/src/audio/SDL_mixer.c \
	$(SRCDIR)/vendor/SDL2/src/audio/SDL_wave.c \
	$(SRCDIR)/vendor/SDL2/src/audio/dummy/SDL_dummyaudio.c \
	$(SRCDIR)/vendor/SDL2/src/core/freebsd/SDL_evdev_kbd_freebsd.c \
	$(SRCDIR)/vendor/SDL2/src/core/linux/SDL_evdev.c \
	$(SRCDIR)/vendor/SDL2/src/core/linux/SDL_evdev_capabilities.c \
	$(SRCDIR)/vendor/SDL2/src/core/linux/SDL_evdev_kbd.c \
	$(SRCDIR)/vendor/SDL2/src/core/linux/SDL_ime.c \
	$(SRCDIR)/vendor/SDL2/src/core/linux/SDL_sandbox.c \
	$(SRCDIR)/vendor/SDL2/src/core/linux/SDL_threadprio.c \
	$(SRCDIR)/vendor/SDL2/src/core/unix/SDL_poll.c \
	$(SRCDIR)/vendor/SDL2/src/cpuinfo/SDL_cpuinfo.c \
	$(SRCDIR)/vendor/SDL2/src/dynapi/SDL_dynapi.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_clipboardevents.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_displayevents.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_dropevents.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_events.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_gesture.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_keyboard.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_keysym_to_scancode.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_mouse.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_quit.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_scancode_tables.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_touch.c \
	$(SRCDIR)/vendor/SDL2/src/events/SDL_windowevents.c \
	$(SRCDIR)/vendor/SDL2/src/events/imKStoUCS.c \
	$(SRCDIR)/vendor/SDL2/src/file/SDL_rwops.c \
	$(SRCDIR)/vendor/SDL2/src/filesystem/unix/SDL_sysfilesystem.c \
	$(SRCDIR)/vendor/SDL2/src/haptic/SDL_haptic.c \
	$(SRCDIR)/vendor/SDL2/src/haptic/linux/SDL_syshaptic.c \
	$(SRCDIR)/vendor/SDL2/src/hidapi/SDL_hidapi.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/SDL_gamecontroller.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/SDL_joystick.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/SDL_steam_virtual_gamepad.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/controller_type.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_combined.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_gamecube.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_luna.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_ps3.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_ps4.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_ps5.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_rumble.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_shield.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_stadia.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_switch.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_wii.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_xbox360.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_xbox360w.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapi_xboxone.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/hidapi/SDL_hidapijoystick.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/linux/SDL_sysjoystick.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/steam/SDL_steamcontroller.c \
	$(SRCDIR)/vendor/SDL2/src/joystick/virtual/SDL_virtualjoystick.c \
	$(SRCDIR)/vendor/SDL2/src/libm/e_atan2.c \
	$(SRCDIR)/vendor/SDL2/src/libm/e_exp.c \
	$(SRCDIR)/vendor/SDL2/src/libm/e_fmod.c \
	$(SRCDIR)/vendor/SDL2/src/libm/e_log.c \
	$(SRCDIR)/vendor/SDL2/src/libm/e_log10.c \
	$(SRCDIR)/vendor/SDL2/src/libm/e_pow.c \
	$(SRCDIR)/vendor/SDL2/src/libm/e_rem_pio2.c \
	$(SRCDIR)/vendor/SDL2/src/libm/e_sqrt.c \
	$(SRCDIR)/vendor/SDL2/src/libm/k_cos.c \
	$(SRCDIR)/vendor/SDL2/src/libm/k_rem_pio2.c \
	$(SRCDIR)/vendor/SDL2/src/libm/k_sin.c \
	$(SRCDIR)/vendor/SDL2/src/libm/k_tan.c \
	$(SRCDIR)/vendor/SDL2/src/libm/s_atan.c \
	$(SRCDIR)/vendor/SDL2/src/libm/s_copysign.c \
	$(SRCDIR)/vendor/SDL2/src/libm/s_cos.c \
	$(SRCDIR)/vendor/SDL2/src/libm/s_fabs.c \
	$(SRCDIR)/vendor/SDL2/src/libm/s_floor.c \
	$(SRCDIR)/vendor/SDL2/src/libm/s_scalbn.c \
	$(SRCDIR)/vendor/SDL2/src/libm/s_sin.c \
	$(SRCDIR)/vendor/SDL2/src/libm/s_tan.c \
	$(SRCDIR)/vendor/SDL2/src/loadso/dlopen/SDL_sysloadso.c \
	$(SRCDIR)/vendor/SDL2/src/locale/SDL_locale.c \
	$(SRCDIR)/vendor/SDL2/src/locale/unix/SDL_syslocale.c \
	$(SRCDIR)/vendor/SDL2/src/main/dummy/SDL_dummy_main.c \
	$(SRCDIR)/vendor/SDL2/src/misc/SDL_url.c \
	$(SRCDIR)/vendor/SDL2/src/misc/unix/SDL_sysurl.c \
	$(SRCDIR)/vendor/SDL2/src/power/SDL_power.c \
	$(SRCDIR)/vendor/SDL2/src/power/linux/SDL_syspower.c \
	$(SRCDIR)/vendor/SDL2/src/render/SDL_d3dmath.c \
	$(SRCDIR)/vendor/SDL2/src/render/SDL_render.c \
	$(SRCDIR)/vendor/SDL2/src/render/SDL_yuv_sw.c \
	$(SRCDIR)/vendor/SDL2/src/render/software/SDL_blendfillrect.c \
	$(SRCDIR)/vendor/SDL2/src/render/software/SDL_blendline.c \
	$(SRCDIR)/vendor/SDL2/src/render/software/SDL_blendpoint.c \
	$(SRCDIR)/vendor/SDL2/src/render/software/SDL_drawline.c \
	$(SRCDIR)/vendor/SDL2/src/render/software/SDL_drawpoint.c \
	$(SRCDIR)/vendor/SDL2/src/render/software/SDL_render_sw.c \
	$(SRCDIR)/vendor/SDL2/src/render/software/SDL_rotate.c \
	$(SRCDIR)/vendor/SDL2/src/render/software/SDL_triangle.c \
	$(SRCDIR)/vendor/SDL2/src/sensor/SDL_sensor.c \
	$(SRCDIR)/vendor/SDL2/src/sensor/dummy/SDL_dummysensor.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_crc16.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_crc32.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_getenv.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_iconv.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_malloc.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_mslibc.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_qsort.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_stdlib.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_string.c \
	$(SRCDIR)/vendor/SDL2/src/stdlib/SDL_strtokr.c \
	$(SRCDIR)/vendor/SDL2/src/thread/SDL_thread.c \
	$(SRCDIR)/vendor/SDL2/src/thread/pthread/SDL_syscond.c \
	$(SRCDIR)/vendor/SDL2/src/thread/pthread/SDL_sysmutex.c \
	$(SRCDIR)/vendor/SDL2/src/thread/pthread/SDL_syssem.c \
	$(SRCDIR)/vendor/SDL2/src/thread/pthread/SDL_systhread.c \
	$(SRCDIR)/vendor/SDL2/src/thread/pthread/SDL_systls.c \
	$(SRCDIR)/vendor/SDL2/src/timer/SDL_timer.c \
	$(SRCDIR)/vendor/SDL2/src/timer/unix/SDL_systimer.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_RLEaccel.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_blit.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_blit_0.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_blit_1.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_blit_A.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_blit_N.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_blit_auto.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_blit_copy.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_blit_slow.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_bmp.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_clipboard.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_egl.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_fillrect.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_pixels.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_rect.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_shape.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_stretch.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_surface.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_video.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_vulkan_utils.c \
	$(SRCDIR)/vendor/SDL2/src/video/SDL_yuv.c \
	$(SRCDIR)/vendor/SDL2/src/video/dummy/SDL_nullevents.c \
	$(SRCDIR)/vendor/SDL2/src/video/dummy/SDL_nullframebuffer.c \
	$(SRCDIR)/vendor/SDL2/src/video/dummy/SDL_nullvideo.c \
	$(SRCDIR)/vendor/SDL2/src/video/offscreen/SDL_offscreenevents.c \
	$(SRCDIR)/vendor/SDL2/src/video/offscreen/SDL_offscreenframebuffer.c \
	$(SRCDIR)/vendor/SDL2/src/video/offscreen/SDL_offscreenopengles.c \
	$(SRCDIR)/vendor/SDL2/src/video/offscreen/SDL_offscreenvideo.c \
	$(SRCDIR)/vendor/SDL2/src/video/offscreen/SDL_offscreenwindow.c \
	$(SRCDIR)/vendor/SDL2/src/video/yuv2rgb/yuv_rgb_lsx.c \
	$(SRCDIR)/vendor/SDL2/src/video/yuv2rgb/yuv_rgb_sse.c \
	$(SRCDIR)/vendor/SDL2/src/video/yuv2rgb/yuv_rgb_std.c

# Phase 10 (see phi.md's "Phase 10 -- Audio"): real ALSA playback on
# native Linux IF this build environment actually has libasound2-dev's
# headers -- checked here, once, via a real filesystem wildcard rather
# than assumed, same "detect, don't assume" reasoning PHI_HAVE_HTTP_
# CLIENT's own #if in editor_main.c already applies to the native HTTP
# client. Without the header, client/audio_native.c itself still
# compiles and links cleanly (see its own #ifdef PHI_HAVE_ALSA branch) --
# sounds load and decode for real either way, only actual playback is
# affected. Install libasound2-dev and re-run `make` to pick up real
# native audio without any other change needed.
ALSA_HEADER := $(wildcard /usr/include/alsa/asoundlib.h /usr/include/*/alsa/asoundlib.h)
ifneq ($(ALSA_HEADER),)
ALSA_CFLAGS  := -DPHI_HAVE_ALSA
ALSA_LDFLAGS := -lasound
else
ALSA_CFLAGS  :=
ALSA_LDFLAGS :=
endif

# Phase 9 (see phi.md's "Shipping a Standalone Game"): renamed from
# COMMON_SRCS -- this is the shared engine core BOTH drivers link
# (editor_main.c, today's full-chrome editor, and player_main.c, the new
# chromeless game driver), with the actual entry point (main()) added
# separately per target below (EDITOR_SRCS/PLAYER_SRCS). No attempt was
# made to trim editor-only modules (ui.c/asset_browser.c/chat.c/
# console.c/gizmo.c/area_tree.c/font.c/svg_icon.c/net.c/etc.) out of what
# player_main.c links -- they're real, working code that simply never
# gets CALLED from a driver that never calls ui_init()/asset_browser_
# init()/net_connect()/etc., not a correctness risk, just some unused
# object code in the shipped binary. Trimming that is real future work
# (a real "what does a shipped binary actually need" pass), not attempted
# this pass to keep the actual risk surface (a first player_main.c that
# needs to link and run at all) as small as possible.
ENGINE_CORE_SRCS := \
	$(SRCDIR)/octree_render.c \
	$(SRCDIR)/renderer.c      \
	$(SRCDIR)/net.c           \
	$(SRCDIR)/input.c         \
	$(SRCDIR)/console.c       \
	$(SRCDIR)/asset_browser.c \
	$(SRCDIR)/chat.c          \
	$(SRCDIR)/halfedge.c      \
	$(SRCDIR)/halfedge_gltf.c \
	$(SRCDIR)/meshobject.c    \
	$(SRCDIR)/mesh_edit.c     \
	$(SRCDIR)/node_graph.c    \
	$(SRCDIR)/audio_wav.c     \
	$(SRCDIR)/render_hooks.c  \
	$(SRCDIR)/fracture.c      \
	$(SRCDIR)/armature.c      \
	$(SRCDIR)/animation.c     \
	$(SRCDIR)/skinned_mesh.c  \
	$(SRCDIR)/gizmo.c         \
	$(SRCDIR)/transform_op.c  \
	$(SRCDIR)/light.c         \
	$(SRCDIR)/scene_target.c  \
	$(SRCDIR)/scene_objects.c \
	$(SRCDIR)/fracture_body.c \
	$(SRCDIR)/path_tracer.c   \
	$(SRCDIR)/skinned_mesh_object.c \
	$(SRCDIR)/ragdoll.c       \
	$(SRCDIR)/font.c          \
	$(SRCDIR)/svg_icon.c      \
	$(SRCDIR)/ui.c            \
	$(SRCDIR)/area_tree.c     \
	$(SRCDIR)/phi_prop.c      \
	$(SRCDIR)/phi_prop_registry.c \
	$(SRCDIR)/mp_port.c       \
	$(SRCDIR)/phi_physics.cpp \
	$(BULLET_SRCS)            \
	$(MP_EMBED_SRCS)

.PHONY: all wasm native player player_wasm run clean debug watch mp_test mp_test_win32 mp_test_wasm mp_stress mesh_edit_test fracture_test mp_console_test asset_protocol_test area_tree_test phi_prop_test mp_prop_panel_test phi_physics_test phi_physics_meshobject_test mp_physics_test animation_test light_test fracture_body_test path_tracer_test skinned_mesh_object_test ragdoll_test scene_objects_test mp_geometry_test node_graph_test mp_node_test phi_h_test render_hooks_test input_gamepad_test mp_phase9_gap_test audio_wav_test

all: wasm native

# ---------------------------------------------------------------
# WASM (Emscripten / WebGL1)
# ---------------------------------------------------------------
WASM_CC   := emcc
WASM_SRCS := $(ENGINE_CORE_SRCS) $(SRCDIR)/editor_main.c $(SRCDIR)/phi_platform_wasm.c $(SRCDIR)/gbuffer.c $(SRCDIR)/input_gamepad_wasm.c $(SRCDIR)/audio_wasm.c

WASM_CFLAGS := \
	-O2 \
	-Wall \
	-Wextra \
	-Wno-unused-parameter \
	-I$(SRCDIR) \
	$(MP_INCLUDES) \
	$(BULLET_INCLUDES) \
	-DEMSCRIPTEN

EMFLAGS := \
	-s WASM=1 \
	-s USE_WEBGL2=1 \
	-s LEGACY_GL_EMULATION=0 \
	-s FULL_ES3=1 \
	-s USE_PTHREADS=0 \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s INITIAL_MEMORY=134217728 \
	-s EXPORTED_FUNCTIONS='["_main","_net_connect_js","_malloc","_free"]' \
	-s EXPORTED_RUNTIME_METHODS='["allocateUTF8","ccall","cwrap"]' \
	-s NO_EXIT_RUNTIME=1 \
	-s MODULARIZE=0 \
	-s ENVIRONMENT=web \
	--js-library $(SRCDIR)/library_ws_stub.js \
	--embed-file assets@assets \
	-lGL \
	-lwebsocket.js \
	-lm

# Debug build overrides
DEBUG_FLAGS := -O0 -g4 -s ASSERTIONS=2 -s SAFE_HEAP=1 -DDEBUG

OUT_JS   := $(WWWDIR)/game.js
OUT_WASM := $(WWWDIR)/game.wasm

wasm: $(WWWDIR) $(OUT_JS)

$(WWWDIR):
	mkdir -p $(WWWDIR)

$(OUT_JS): $(WASM_SRCS) $(HDRS) assets/cube.gltf assets/cube.bin | $(WWWDIR)
	$(WASM_CC) $(WASM_CFLAGS) $(EMFLAGS) $(WASM_SRCS) -o $(OUT_JS)
	@echo "wasm build complete -> $(OUT_JS) + $(OUT_WASM)"

debug:
	$(WASM_CC) $(WASM_CFLAGS) $(DEBUG_FLAGS) $(EMFLAGS) $(WASM_SRCS) -o $(OUT_JS)

run: wasm
	cd server && python3 server.py

# ---------------------------------------------------------------
# Native (Xlib/GLX, OpenGL 3.3 core)
# ---------------------------------------------------------------
NATIVE_CC   := gcc
NATIVE_SRCS := $(ENGINE_CORE_SRCS) $(SRCDIR)/editor_main.c $(SRCDIR)/phi_platform_native.c $(SRCDIR)/gl_native.c $(SRCDIR)/gbuffer.c $(SRCDIR)/ws_client_native.c $(SRCDIR)/http_client_native.c \
               $(SRCDIR)/input_gamepad_native.c $(SDL2_JOYSTICK_SRCS) $(SRCDIR)/audio_native.c

NATIVE_CFLAGS := \
	-O2 \
	-Wall \
	-Wextra \
	-Wno-unused-parameter \
	-I$(SRCDIR) \
	$(MP_INCLUDES) \
	$(BULLET_INCLUDES) \
	$(SDL2_INCLUDES) \
	$(ALSA_CFLAGS)

# -lpthread/-ldl/-lrt: real SDL2 dependencies (pthread thread backend,
# dlopen-based loadso, POSIX timer), see client/vendor/SDL2/VENDORED.md.
# -lasound: only added when ALSA_LDFLAGS is non-empty (see ALSA_HEADER's
# own detection comment above) -- audio_native.c's mixer thread also
# needs -lpthread, already present here for SDL2's own sake.
NATIVE_LDFLAGS := -lX11 -lGL -lm -lcrypto -lstdc++ -lpthread -ldl -lrt $(ALSA_LDFLAGS)

OUT_NATIVE := $(BUILDDIR)/phi_native

native: $(OUT_NATIVE)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(OUT_NATIVE): $(NATIVE_SRCS) $(HDRS) | $(BUILDDIR)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_SRCS) -o $(OUT_NATIVE) $(NATIVE_LDFLAGS)
	@echo "native build complete -> $(OUT_NATIVE)"

# ---------------------------------------------------------------
# Player (Phase 9's "chromeless gameloop", see phi.md's "Editor/Player
# split") — the shipped-game binary. Native-only for now (matching
# player_main.c's own top comment: wasm plays no part in the Distribution
# Model's shipped-game path; win32 is deferred, no verifiable toolchain
# in this environment for it beyond the editor target already built there).
#
# Same ENGINE_CORE_SRCS + native platform/GL/gamepad backend as `native`
# above, swapping player_main.c in for editor_main.c, plus whatever the
# user has actually placed in game/src/ -- GAME_SRC_FILES is a real
# `wildcard`, evaluated at `make` invocation time, not a fixed list, so a
# game with no game/src/ at all (a pure game/main.py game) still builds
# cleanly with an empty addition here. PHI_GAME_HAS_C_ENTRY is defined
# iff game/src/main.c specifically exists -- see player_main.c's own top
# comment for why that one file's presence (not just "some game/src/
# files exist") is what selects the C-entry-point code path.
# ---------------------------------------------------------------
GAME_SRC_FILES := $(wildcard game/src/*.c)
ifneq ($(wildcard game/src/main.c),)
PHI_GAME_HAS_C_ENTRY := -DPHI_GAME_HAS_C_ENTRY
endif

# net.c is one of the not-yet-trimmed-out ENGINE_CORE_SRCS members (see
# that variable's own comment) -- player_main.c never calls net_connect,
# but net.c's own symbols still need a real ws_client_* backend at link
# time regardless, same as http_client_native.c for asset_browser.c's
# symbols -- both included here purely to satisfy the linker, matching
# NATIVE_SRCS' own set for exactly the same reason.
PLAYER_SRCS := $(ENGINE_CORE_SRCS) $(SRCDIR)/player_main.c $(SRCDIR)/phi_platform_native.c $(SRCDIR)/gl_native.c $(SRCDIR)/gbuffer.c $(SRCDIR)/ws_client_native.c $(SRCDIR)/http_client_native.c \
               $(SRCDIR)/input_gamepad_native.c $(SDL2_JOYSTICK_SRCS) $(SRCDIR)/audio_native.c $(GAME_SRC_FILES)

PLAYER_CFLAGS := $(NATIVE_CFLAGS) $(PHI_GAME_HAS_C_ENTRY)

OUT_PLAYER := $(BUILDDIR)/phi_player

player: $(OUT_PLAYER)

$(OUT_PLAYER): $(PLAYER_SRCS) $(HDRS) | $(BUILDDIR)
	$(NATIVE_CC) $(PLAYER_CFLAGS) $(PLAYER_SRCS) -o $(OUT_PLAYER) $(NATIVE_LDFLAGS)
	@echo "player build complete -> $(OUT_PLAYER)"

# ---------------------------------------------------------------
# Player, wasm build -- NOT part of Phase 9's actual shipped-game
# Distribution Model (see phi.md: a shipped standalone game is native-
# only, since Steam ships a real local toolchain and doesn't need a
# browser channel) -- this exists purely so `game/src/main.c`/`game/
# main.py` can be demoed over a real URL, served by the same server.py
# the editor already uses, without needing anyone to have a native build
# environment at all. Same ENGINE_CORE_SRCS + wasm platform/GL/gamepad/
# audio backends as the editor's own `wasm` target, player_main.c in
# place of editor_main.c, plus whatever's in game/src/ (reuses GAME_SRC_
# FILES/PHI_GAME_HAS_C_ENTRY, defined just above for the native player --
# this target must stay below that definition, not next to the editor's
# own `wasm` target above, or these two variables would still be empty/
# undefined at the point Make expands them here). A separate EMFLAGS
# variant (PLAYER_EMFLAGS) drops _net_connect_js from EXPORTED_FUNCTIONS
# -- that JS-callable export is editor_main.c's own symbol (the
# WebSocket-URL-from-JS callback for its live server connection), which
# player_main.c never defines; everything else net.c itself still needs
# at the wasm link level (the --js-library/-lwebsocket.js flags) stays,
# same "net.c isn't trimmed out of ENGINE_CORE_SRCS yet, so its own link
# requirements still apply even though player_main.c never calls net_
# connect" reasoning PLAYER_SRCS' own comment above already gives for the
# native player and ws_client_native.c.
# ---------------------------------------------------------------
PLAYER_WASM_SRCS := $(ENGINE_CORE_SRCS) $(SRCDIR)/player_main.c $(SRCDIR)/phi_platform_wasm.c $(SRCDIR)/gbuffer.c $(SRCDIR)/input_gamepad_wasm.c $(SRCDIR)/audio_wasm.c $(GAME_SRC_FILES)

PLAYER_WASM_CFLAGS := $(WASM_CFLAGS) $(PHI_GAME_HAS_C_ENTRY)

PLAYER_EMFLAGS := \
	-s WASM=1 \
	-s USE_WEBGL2=1 \
	-s LEGACY_GL_EMULATION=0 \
	-s FULL_ES3=1 \
	-s USE_PTHREADS=0 \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s INITIAL_MEMORY=134217728 \
	-s EXPORTED_FUNCTIONS='["_main","_malloc","_free"]' \
	-s EXPORTED_RUNTIME_METHODS='["allocateUTF8","ccall","cwrap"]' \
	-s NO_EXIT_RUNTIME=1 \
	-s MODULARIZE=0 \
	-s ENVIRONMENT=web \
	--js-library $(SRCDIR)/library_ws_stub.js \
	--embed-file assets@assets \
	-lGL \
	-lwebsocket.js \
	-lm

OUT_PLAYER_JS   := $(WWWDIR)/player.js
OUT_PLAYER_WASM := $(WWWDIR)/player.wasm

player_wasm: $(WWWDIR) $(OUT_PLAYER_JS)

$(OUT_PLAYER_JS): $(PLAYER_WASM_SRCS) $(HDRS) assets/cube.gltf assets/cube.bin | $(WWWDIR)
	$(WASM_CC) $(PLAYER_WASM_CFLAGS) $(PLAYER_EMFLAGS) $(PLAYER_WASM_SRCS) -o $(OUT_PLAYER_JS)
	@echo "player_wasm build complete -> $(OUT_PLAYER_JS) + $(OUT_PLAYER_WASM)"

# ---------------------------------------------------------------
# Win32 (WGL, OpenGL 3.3 core) — built via a Windows-side MinGW-w64
# toolchain reached through WSL interop; produces a real Windows .exe.
# Windowing + GL context + rendering only for now — no Win32 input or
# Winsock networking yet (see phi_platform_win32.c's header comment).
# ---------------------------------------------------------------
WIN32_CC   := /mnt/c/msys64/mingw64/bin/gcc.exe
WIN32_SRCS := $(ENGINE_CORE_SRCS) $(SRCDIR)/editor_main.c $(SRCDIR)/phi_platform_win32.c $(SRCDIR)/gl_native.c $(SRCDIR)/gbuffer.c $(SRCDIR)/ws_client_win32.c $(SRCDIR)/input_gamepad_win32_stub.c $(SRCDIR)/audio_win32_stub.c

WIN32_CFLAGS := \
	-O2 \
	-Wall \
	-Wextra \
	-Wno-unused-parameter \
	-I$(SRCDIR) \
	$(MP_INCLUDES) \
	$(BULLET_INCLUDES)

WIN32_LDFLAGS := -lopengl32 -lgdi32 -luser32 -lkernel32 -lws2_32 -lbcrypt -lstdc++

OUT_WIN32 := $(BUILDDIR)/phi_win32.exe

win32: $(OUT_WIN32)

$(OUT_WIN32): $(WIN32_SRCS) $(HDRS) | $(BUILDDIR)
	$(WIN32_CC) $(WIN32_CFLAGS) $(WIN32_SRCS) -o $(OUT_WIN32) $(WIN32_LDFLAGS)
	chmod +x $(OUT_WIN32)
	@echo "win32 build complete -> $(OUT_WIN32)"

# ---------------------------------------------------------------
# mesh_edit.c topology self-test (Phase 1 extrude/inset/loop-cut) — NOT
# part of the game build. Exercises the real half-edge mutations against
# assets/cube.gltf with no GL/window/X11 dependency at all, matching the
# MicroPython self-test's own "prove the subsystem works in isolation
# before/alongside it being wired into the real editor" precedent below.
# ---------------------------------------------------------------
MESH_EDIT_TEST_SRCS := $(SRCDIR)/mesh_edit_test_main.c $(SRCDIR)/halfedge.c \
                        $(SRCDIR)/halfedge_gltf.c $(SRCDIR)/mesh_edit.c
OUT_MESH_EDIT_TEST   := $(BUILDDIR)/mesh_edit_test

mesh_edit_test: $(OUT_MESH_EDIT_TEST)
	./$(OUT_MESH_EDIT_TEST)

$(OUT_MESH_EDIT_TEST): $(MESH_EDIT_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -I$(SRCDIR) $(MESH_EDIT_TEST_SRCS) -o $(OUT_MESH_EDIT_TEST) -lm
	@echo "mesh_edit_test build complete -> $(OUT_MESH_EDIT_TEST)"

# node_graph.c (Phase 6 node graphs) topology/topological-sort self-test --
# same no-GL/no-MicroPython-dependency rationale as mesh_edit_test above.
# Node TYPE resolution + actually calling Python functions is mp_port.c's
# job (see mp_node_test target below) -- this only proves the C-owned
# graph CRUD and Kahn's-algorithm evaluation order are correct.
NODE_GRAPH_TEST_SRCS := $(SRCDIR)/node_graph_test_main.c $(SRCDIR)/node_graph.c
OUT_NODE_GRAPH_TEST   := $(BUILDDIR)/node_graph_test

node_graph_test: $(OUT_NODE_GRAPH_TEST)
	./$(OUT_NODE_GRAPH_TEST)

$(OUT_NODE_GRAPH_TEST): $(NODE_GRAPH_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -I$(SRCDIR) $(NODE_GRAPH_TEST_SRCS) -o $(OUT_NODE_GRAPH_TEST) -lm
	@echo "node_graph_test build complete -> $(OUT_NODE_GRAPH_TEST)"

# area_tree.c (Blender-style area border resize/split/join) self-test --
# same no-GL-dependency rationale as mesh_edit_test above; built with
# AddressSanitizer since this module does its own malloc/free tree
# surgery (area_tree_split/join_with_sibling) and a leak or use-after-free
# here would otherwise only show up as a slow crash much later, deep
# inside ui.c's real GL rendering.
AREA_TREE_TEST_SRCS := $(SRCDIR)/area_tree_test_main.c $(SRCDIR)/area_tree.c
OUT_AREA_TREE_TEST   := $(BUILDDIR)/area_tree_test

area_tree_test: $(OUT_AREA_TREE_TEST)
	./$(OUT_AREA_TREE_TEST)

$(OUT_AREA_TREE_TEST): $(AREA_TREE_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -g -fsanitize=address -Wall -Wextra -I$(SRCDIR) $(AREA_TREE_TEST_SRCS) -o $(OUT_AREA_TREE_TEST) -lm
	@echo "area_tree_test build complete -> $(OUT_AREA_TREE_TEST)"

# phi_prop.c/phi_prop_registry.c (DNA/RNA-style property descriptors)
# self-test -- same no-GL-dependency rationale as area_tree_test above.
PHI_PROP_TEST_SRCS := $(SRCDIR)/phi_prop_test_main.c $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c
OUT_PHI_PROP_TEST   := $(BUILDDIR)/phi_prop_test

phi_prop_test: $(OUT_PHI_PROP_TEST)
	./$(OUT_PHI_PROP_TEST)

$(OUT_PHI_PROP_TEST): $(PHI_PROP_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -Wextra -I$(SRCDIR) $(PHI_PROP_TEST_SRCS) -o $(OUT_PHI_PROP_TEST) -lm
	@echo "phi_prop_test build complete -> $(OUT_PHI_PROP_TEST)"

# phi_physics.h/.cpp (Phase 2, vendored Bullet Physics) self-tests -- same
# no-GL-dependency rationale as the other standalone harnesses, but these
# link the full vendored Bullet source (~150 .cpp files, ~75s at -O1),
# by far the slowest of any test target here -- real, unavoidable cost of
# actually proving the vendoring+wrapper works, not skipped for speed.
# -w on the Bullet sources specifically (not phi_physics.cpp or the test
# itself) since third-party vendored code isn't held to this project's
# own -Wall -Wextra bar, same rationale nanosvg's one tolerated warning
# already established, just at a larger scale.
PHI_PHYSICS_TEST_SRCS := $(SRCDIR)/phi_physics_test_main.c $(SRCDIR)/phi_physics.cpp $(BULLET_SRCS)
OUT_PHI_PHYSICS_TEST   := $(BUILDDIR)/phi_physics_test

phi_physics_test: $(OUT_PHI_PHYSICS_TEST)
	./$(OUT_PHI_PHYSICS_TEST)

$(OUT_PHI_PHYSICS_TEST): $(PHI_PHYSICS_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(BULLET_INCLUDES) $(PHI_PHYSICS_TEST_SRCS) -o $(OUT_PHI_PHYSICS_TEST) -lstdc++ -lm
	@echo "phi_physics_test build complete -> $(OUT_PHI_PHYSICS_TEST)"

# fracture_body.c (Phase 2's runtime "shatter on impact" completion --
# real convex-hull fragments + breaking-threshold constraints, spawned
# from fracture.c's existing precompute) self-test -- same no-GL
# rationale as phi_physics_test above; renderer_draw_mesh_object is
# stubbed in the test main itself (see its own comment) so this doesn't
# need to link renderer.c/gl_native.c/gbuffer.c at all.
FRACTURE_BODY_TEST_SRCS := $(SRCDIR)/fracture_body_test_main.c $(SRCDIR)/fracture_body.c \
                            $(SRCDIR)/fracture.c $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c \
                            $(SRCDIR)/meshobject.c \
                            $(SRCDIR)/phi_physics.cpp $(BULLET_SRCS)
OUT_FRACTURE_BODY_TEST   := $(BUILDDIR)/fracture_body_test

fracture_body_test: $(OUT_FRACTURE_BODY_TEST)
	./$(OUT_FRACTURE_BODY_TEST)

$(OUT_FRACTURE_BODY_TEST): $(FRACTURE_BODY_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(BULLET_INCLUDES) $(FRACTURE_BODY_TEST_SRCS) -o $(OUT_FRACTURE_BODY_TEST) -lstdc++ -lm
	@echo "fracture_body_test build complete -> $(OUT_FRACTURE_BODY_TEST)"

# path_tracer.c (Phase 3's real offline path tracer -- BVH, GGX/Lambertian
# BSDF, next-event estimation against Light objects, PNG output via the
# vendored stb_image_write.h) self-test -- entirely no-GL on its own (see
# path_tracer.h's own header comment), needs no stub functions the way
# fracture_body_test above does.
PATH_TRACER_TEST_SRCS := $(SRCDIR)/path_tracer_test_main.c $(SRCDIR)/path_tracer.c \
                          $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c $(SRCDIR)/meshobject.c
OUT_PATH_TRACER_TEST   := $(BUILDDIR)/path_tracer_test

path_tracer_test: $(OUT_PATH_TRACER_TEST)
	./$(OUT_PATH_TRACER_TEST)

$(OUT_PATH_TRACER_TEST): $(PATH_TRACER_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(PATH_TRACER_TEST_SRCS) -o $(OUT_PATH_TRACER_TEST) -lm
	@echo "path_tracer_test build complete -> $(OUT_PATH_TRACER_TEST)"

# skinned_mesh_object.c (Phase 4's pose->world->skin-matrix CPU pipeline,
# see skinned_mesh_object.h) self-test -- no GL dependency at all, same
# as path_tracer_test above.
SKINNED_MESH_OBJECT_TEST_SRCS := $(SRCDIR)/skinned_mesh_object_test_main.c $(SRCDIR)/skinned_mesh_object.c \
                                  $(SRCDIR)/skinned_mesh.c $(SRCDIR)/armature.c $(SRCDIR)/animation.c \
                                  $(SRCDIR)/meshobject.c $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c
OUT_SKINNED_MESH_OBJECT_TEST   := $(BUILDDIR)/skinned_mesh_object_test

skinned_mesh_object_test: $(OUT_SKINNED_MESH_OBJECT_TEST)
	./$(OUT_SKINNED_MESH_OBJECT_TEST)

$(OUT_SKINNED_MESH_OBJECT_TEST): $(SKINNED_MESH_OBJECT_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(SKINNED_MESH_OBJECT_TEST_SRCS) -o $(OUT_SKINNED_MESH_OBJECT_TEST) -lm
	@echo "skinned_mesh_object_test build complete -> $(OUT_SKINNED_MESH_OBJECT_TEST)"

# ragdoll.c (Phase 4's Armature -> Bullet ragdoll handoff, see ragdoll.h)
# self-test -- real Bullet physics (capsule bodies + point2point joints),
# no GL dependency (neither ragdoll.c nor skinned_mesh_object.c ever
# touches GL).
RAGDOLL_TEST_SRCS := $(SRCDIR)/ragdoll_test_main.c $(SRCDIR)/ragdoll.c $(SRCDIR)/skinned_mesh_object.c \
                      $(SRCDIR)/skinned_mesh.c $(SRCDIR)/armature.c $(SRCDIR)/animation.c \
                      $(SRCDIR)/meshobject.c $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c \
                      $(SRCDIR)/phi_physics.cpp $(BULLET_SRCS)
OUT_RAGDOLL_TEST   := $(BUILDDIR)/ragdoll_test

ragdoll_test: $(OUT_RAGDOLL_TEST)
	./$(OUT_RAGDOLL_TEST)

$(OUT_RAGDOLL_TEST): $(RAGDOLL_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(BULLET_INCLUDES) $(RAGDOLL_TEST_SRCS) -o $(OUT_RAGDOLL_TEST) -lstdc++ -lm
	@echo "ragdoll_test build complete -> $(OUT_RAGDOLL_TEST)"

# scene_objects.c (Phase 5's real multi-object scene graph, see scene_
# objects.h) self-test -- no GL dependency (scene_object_add/find/
# get_all/count never touch GL; scene_object_delete's own mesh_destroy
# reference is stubbed in the test main itself, same technique fracture_
# body_test above already established).
SCENE_OBJECTS_TEST_SRCS := $(SRCDIR)/scene_objects_test_main.c $(SRCDIR)/scene_objects.c \
                            $(SRCDIR)/halfedge.c
OUT_SCENE_OBJECTS_TEST   := $(BUILDDIR)/scene_objects_test

scene_objects_test: $(OUT_SCENE_OBJECTS_TEST)
	./$(OUT_SCENE_OBJECTS_TEST)

$(OUT_SCENE_OBJECTS_TEST): $(SCENE_OBJECTS_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(SCENE_OBJECTS_TEST_SRCS) -o $(OUT_SCENE_OBJECTS_TEST) -lm
	@echo "scene_objects_test build complete -> $(OUT_SCENE_OBJECTS_TEST)"

# Same, but exercising the actual MeshObject integration (AABB-from-mesh,
# the exact create/step/sync sequence main.c's CTX_ACTION_ENABLE_PHYSICS
# and main_loop use) rather than the raw phi_physics.h wrapper directly.
PHI_PHYSICS_MESHOBJ_TEST_SRCS := $(SRCDIR)/phi_physics_meshobject_test_main.c $(SRCDIR)/phi_physics.cpp \
                                  $(SRCDIR)/meshobject.c $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c $(BULLET_SRCS)
OUT_PHI_PHYSICS_MESHOBJ_TEST   := $(BUILDDIR)/phi_physics_meshobject_test

phi_physics_meshobject_test: $(OUT_PHI_PHYSICS_MESHOBJ_TEST)
	./$(OUT_PHI_PHYSICS_MESHOBJ_TEST)

$(OUT_PHI_PHYSICS_MESHOBJ_TEST): $(PHI_PHYSICS_MESHOBJ_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(BULLET_INCLUDES) $(PHI_PHYSICS_MESHOBJ_TEST_SRCS) -o $(OUT_PHI_PHYSICS_MESHOBJ_TEST) -lstdc++ -lm
	@echo "phi_physics_meshobject_test build complete -> $(OUT_PHI_PHYSICS_MESHOBJ_TEST)"

# Smoke test for phi.h itself (Phase 9's public C API for game/src/*.c,
# see phi.h's own header comment) -- proves the aggregating header is
# genuinely self-contained/compilable from a single #include, touching
# one real call from each area (geometry, physics) it currently exposes.
# NOT a re-test of the underlying subsystems, which already have their
# own real coverage elsewhere (phi_physics_test/mesh_edit_test/etc.).
PHI_H_TEST_SRCS := $(SRCDIR)/phi_h_test_main.c $(SRCDIR)/phi_physics.cpp \
                    $(SRCDIR)/meshobject.c $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c \
                    $(SRCDIR)/mesh_edit.c $(SRCDIR)/scene_objects.c $(BULLET_SRCS) \
                    $(SRCDIR)/armature.c $(SRCDIR)/animation.c $(SRCDIR)/skinned_mesh.c \
                    $(SRCDIR)/skinned_mesh_object.c $(SRCDIR)/node_graph.c $(SRCDIR)/render_hooks.c
OUT_PHI_H_TEST   := $(BUILDDIR)/phi_h_test

phi_h_test: $(OUT_PHI_H_TEST)
	./$(OUT_PHI_H_TEST)

$(OUT_PHI_H_TEST): $(PHI_H_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -I$(SRCDIR) $(BULLET_INCLUDES) $(PHI_H_TEST_SRCS) -o $(OUT_PHI_H_TEST) -lstdc++ -lm
	@echo "phi_h_test build complete -> $(OUT_PHI_H_TEST)"

# render_hooks.c (C-level render-pass callback registry, see render_
# hooks.h) self-test -- no GL context needed (render_hooks.c never
# dereferences the GBuffer* it's handed, only passes it through), same
# no-GL-dependency rationale as mesh_edit_test/node_graph_test above.
RENDER_HOOKS_TEST_SRCS := $(SRCDIR)/render_hooks_test_main.c $(SRCDIR)/render_hooks.c
OUT_RENDER_HOOKS_TEST   := $(BUILDDIR)/render_hooks_test

render_hooks_test: $(OUT_RENDER_HOOKS_TEST)
	./$(OUT_RENDER_HOOKS_TEST)

$(OUT_RENDER_HOOKS_TEST): $(RENDER_HOOKS_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -I$(SRCDIR) $(RENDER_HOOKS_TEST_SRCS) -o $(OUT_RENDER_HOOKS_TEST) -lm
	@echo "render_hooks_test build complete -> $(OUT_RENDER_HOOKS_TEST)"

# input_gamepad.h's native backend (input_gamepad_native.c) against the
# REAL vendored SDL2 (client/vendor/SDL2, see its own VENDORED.md) -- no
# GL/window needed, but this DOES link and run the real joystick+
# gamecontroller subsystem, unlike this file's GL-free siblings above.
INPUT_GAMEPAD_TEST_SRCS := $(SRCDIR)/input_gamepad_test_main.c $(SRCDIR)/input_gamepad_native.c $(SDL2_JOYSTICK_SRCS)
OUT_INPUT_GAMEPAD_TEST   := $(BUILDDIR)/input_gamepad_test

input_gamepad_test: $(OUT_INPUT_GAMEPAD_TEST)
	./$(OUT_INPUT_GAMEPAD_TEST)

$(OUT_INPUT_GAMEPAD_TEST): $(INPUT_GAMEPAD_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(SDL2_INCLUDES) $(INPUT_GAMEPAD_TEST_SRCS) -o $(OUT_INPUT_GAMEPAD_TEST) -lpthread -ldl -lm -lrt
	@echo "input_gamepad_test build complete -> $(OUT_INPUT_GAMEPAD_TEST)"

# Phase 10's real WAV decode + resample logic (audio_wav.c, shared by
# every phi_audio.h backend -- see that file's own top comment) -- no GL,
# no ALSA, no MicroPython, same no-dependency self-test precedent as
# every other client/*_test_main.c here.
AUDIO_WAV_TEST_SRCS := $(SRCDIR)/audio_wav_test_main.c $(SRCDIR)/audio_wav.c
OUT_AUDIO_WAV_TEST := $(BUILDDIR)/audio_wav_test

audio_wav_test: $(OUT_AUDIO_WAV_TEST)
	./$(OUT_AUDIO_WAV_TEST)

$(OUT_AUDIO_WAV_TEST): $(AUDIO_WAV_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -I$(SRCDIR) $(AUDIO_WAV_TEST_SRCS) -o $(OUT_AUDIO_WAV_TEST) -lm
	@echo "audio_wav_test build complete -> $(OUT_AUDIO_WAV_TEST)"

# The Python physics API surface (phi.enable_physics/apply_impulse/
# get_velocity/set_velocity, see mp_port.c) against a REAL embedded
# interpreter driving REAL Bullet simulation -- links MicroPython AND
# Bullet together, the slowest single test target here, but the only one
# that actually proves the two subsystems this pass added work together,
# not just each in isolation.
MP_PHYSICS_TEST_SRCS := $(SRCDIR)/mp_physics_test_main.c $(SRCDIR)/mp_port.c \
                         $(SRCDIR)/scene_objects.c $(SRCDIR)/mesh_edit.c $(SRCDIR)/node_graph.c \
                         $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                         $(SRCDIR)/light.c $(SRCDIR)/scene_target.c \
                         $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c \
                         $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c \
                         $(MP_EMBED_SRCS) $(BULLET_SRCS)
OUT_MP_PHYSICS_TEST := $(BUILDDIR)/mp_physics_test

mp_physics_test: $(OUT_MP_PHYSICS_TEST)
	./$(OUT_MP_PHYSICS_TEST)

$(OUT_MP_PHYSICS_TEST): $(MP_PHYSICS_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(MP_INCLUDES) $(BULLET_INCLUDES) $(MP_PHYSICS_TEST_SRCS) -o $(OUT_MP_PHYSICS_TEST) -lstdc++ -lm
	@echo "mp_physics_test build complete -> $(OUT_MP_PHYSICS_TEST)"

# Phase 9 gap-closing bindings (camera, whole-object transforms,
# object-id-keyed physics, keyboard/mouse input, gamepad -- see phi.md's
# Phase 9 "Known gaps" and mp_phase9_gap_test_main.c's own top comment).
# Same real-MicroPython-plus-real-Bullet shape as MP_PHYSICS_TEST_SRCS
# above (needs the same node_graph.c/mesh_edit.c symbols mp_port.c always
# references) -- no renderer.c/GL, no input.c/X11, no input_gamepad_
# native.c/SDL2, all three deliberately kept out via the function-pointer
# handoffs mp_port.h documents.
MP_PHASE9_GAP_TEST_SRCS := $(SRCDIR)/mp_phase9_gap_test_main.c $(SRCDIR)/mp_port.c \
                            $(SRCDIR)/scene_objects.c $(SRCDIR)/mesh_edit.c $(SRCDIR)/node_graph.c \
                            $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                            $(SRCDIR)/light.c $(SRCDIR)/scene_target.c \
                            $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c \
                            $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c \
                            $(MP_EMBED_SRCS) $(BULLET_SRCS)
OUT_MP_PHASE9_GAP_TEST := $(BUILDDIR)/mp_phase9_gap_test

mp_phase9_gap_test: $(OUT_MP_PHASE9_GAP_TEST)
	./$(OUT_MP_PHASE9_GAP_TEST)

$(OUT_MP_PHASE9_GAP_TEST): $(MP_PHASE9_GAP_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(MP_INCLUDES) $(BULLET_INCLUDES) $(MP_PHASE9_GAP_TEST_SRCS) -o $(OUT_MP_PHASE9_GAP_TEST) -lstdc++ -lm
	@echo "mp_phase9_gap_test build complete -> $(OUT_MP_PHASE9_GAP_TEST)"

# Voronoi fracture (client/fracture.c) topology/volume self-test -- same
# no-GL-dependency rationale as mesh_edit_test above.
FRACTURE_TEST_SRCS := $(SRCDIR)/fracture_test_main.c $(SRCDIR)/halfedge.c \
                       $(SRCDIR)/halfedge_gltf.c $(SRCDIR)/fracture.c
OUT_FRACTURE_TEST   := $(BUILDDIR)/fracture_test

fracture_test: $(OUT_FRACTURE_TEST)
	./$(OUT_FRACTURE_TEST)

$(OUT_FRACTURE_TEST): $(FRACTURE_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -I$(SRCDIR) $(FRACTURE_TEST_SRCS) -o $(OUT_FRACTURE_TEST) -lm
	@echo "fracture_test build complete -> $(OUT_FRACTURE_TEST)"

# light.c (Phase 3 Light objects) + scene_target.c (the shared "object"/
# "face"/"light:<id>"/"render" DNA/RNA resolver) self-test -- same no-GL-
# dependency rationale as fracture_test above. Needs halfedge/
# halfedge_gltf for the "object"/"face" resolver checks (a real MeshObject
# against assets/cube.gltf), and phi_prop.c/phi_prop_registry.c since
# scene_target.c resolves to PhiPropGroup*s from there.
LIGHT_TEST_SRCS := $(SRCDIR)/light_test_main.c $(SRCDIR)/light.c $(SRCDIR)/scene_target.c \
                    $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                    $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c
OUT_LIGHT_TEST   := $(BUILDDIR)/light_test

light_test: $(OUT_LIGHT_TEST)
	./$(OUT_LIGHT_TEST)

$(OUT_LIGHT_TEST): $(LIGHT_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -Wextra -I$(SRCDIR) $(LIGHT_TEST_SRCS) -o $(OUT_LIGHT_TEST) -lm
	@echo "light_test build complete -> $(OUT_LIGHT_TEST)"

# armature.c/animation.c (Phase 4's Clip/Curve/Playback + Armature data
# layer) self-test -- same no-GL-dependency rationale as mesh_edit_test/
# fracture_test above, against a real glTF skin+animation fixture
# (tools/gen_test_armature.py) instead of a hand-authored mesh. Links
# meshobject.c for quat_slerp/quat_to_mat4 (needs no Bullet linkage --
# meshobject.c only carries an opaque PhiRigidBody* field, never calls
# into phi_physics.h's functions itself).
ANIMATION_TEST_SRCS := $(SRCDIR)/animation_test_main.c $(SRCDIR)/halfedge.c \
                        $(SRCDIR)/halfedge_gltf.c $(SRCDIR)/meshobject.c \
                        $(SRCDIR)/armature.c $(SRCDIR)/animation.c $(SRCDIR)/skinned_mesh.c
OUT_ANIMATION_TEST   := $(BUILDDIR)/animation_test

animation_test: $(OUT_ANIMATION_TEST)
	./$(OUT_ANIMATION_TEST)

$(OUT_ANIMATION_TEST): $(ANIMATION_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -I$(SRCDIR) $(ANIMATION_TEST_SRCS) -o $(OUT_ANIMATION_TEST) -lm
	@echo "animation_test build complete -> $(OUT_ANIMATION_TEST)"

# ---------------------------------------------------------------
# MicroPython embedding self-test (Phase 5 first slice originally; the
# embedding itself is now ALSO linked into the real native/win32/wasm
# builds above, see COMMON_SRCS/MP_INCLUDES near the top of this file --
# these standalone binaries remain useful as an isolated, game-loop-free
# way to validate the interpreter itself, independent of Phi's own
# console/UI wiring around it). client/micropython_embed/ is generated
# output from MicroPython's own ports/embed tooling (see
# client/mpconfigport.h's header comment); client/mp_port.c/mp_port.h and
# client/mp_test_main.c are hand-written. MP_EMBED_DIR/MP_EMBED_SRCS are
# defined near COMMON_SRCS now, not here, since the real build needs them
# too.
# ---------------------------------------------------------------
MP_TEST_SRCS  := $(SRCDIR)/mp_test_main.c $(SRCDIR)/mp_port.c $(SRCDIR)/mesh_edit.c $(SRCDIR)/node_graph.c \
                         $(SRCDIR)/scene_objects.c \
                  $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                  $(SRCDIR)/light.c $(SRCDIR)/scene_target.c \
                  $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c \
                  $(BULLET_SRCS) $(MP_EMBED_SRCS)
MP_TEST_CFLAGS := -O1 -Wall -Wno-unused-parameter -I$(SRCDIR) $(MP_INCLUDES) $(BULLET_INCLUDES)

OUT_MP_TEST := $(BUILDDIR)/mp_test

mp_test: $(OUT_MP_TEST)

$(OUT_MP_TEST): $(MP_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) $(MP_TEST_CFLAGS) $(MP_TEST_SRCS) -o $(OUT_MP_TEST) -lstdc++ -lm
	@echo "mp_test build complete -> $(OUT_MP_TEST)"

# DNA/RNA property system + @phi.panel binding layer self-test (mp_port.c's
# phi.prop_get/set + phi_mp_panel_count/name/draw_panel, see phi_prop.h) --
# same no-GL-dependency rationale as mp_test above, against a real embedded
# interpreter (not a mock).
MP_PROP_PANEL_TEST_SRCS := $(SRCDIR)/mp_prop_panel_test_main.c $(SRCDIR)/mp_port.c $(SRCDIR)/mesh_edit.c $(SRCDIR)/node_graph.c \
                         $(SRCDIR)/scene_objects.c \
                            $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                            $(SRCDIR)/light.c $(SRCDIR)/scene_target.c \
                            $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c $(BULLET_SRCS) \
                            $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c $(MP_EMBED_SRCS)
OUT_MP_PROP_PANEL_TEST := $(BUILDDIR)/mp_prop_panel_test

mp_prop_panel_test: $(OUT_MP_PROP_PANEL_TEST)
	./$(OUT_MP_PROP_PANEL_TEST)

$(OUT_MP_PROP_PANEL_TEST): $(MP_PROP_PANEL_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) $(MP_TEST_CFLAGS) $(MP_PROP_PANEL_TEST_SRCS) -o $(OUT_MP_PROP_PANEL_TEST) -lstdc++ -lm
	@echo "mp_prop_panel_test build complete -> $(OUT_MP_PROP_PANEL_TEST)"

# Phase 5's real geometry-creation/vertex-editing Python API (phi.
# create_mesh/set_vertices/set_vertex/mesh_object/list_objects/delete_
# object, see mp_port.c) end-to-end against a real embedded interpreter
# driving real scene_objects.c/halfedge.c calls -- same shape as mp_prop_
# panel_test above, same real phi_physics.cpp/Bullet link (mp_port.c's
# OTHER physics bindings need real definitions regardless of whether
# this specific test calls them).
MP_GEOMETRY_TEST_SRCS := $(SRCDIR)/mp_geometry_test_main.c $(SRCDIR)/mp_port.c $(SRCDIR)/mesh_edit.c $(SRCDIR)/node_graph.c \
                          $(SRCDIR)/scene_objects.c \
                          $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                          $(SRCDIR)/light.c $(SRCDIR)/scene_target.c \
                          $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c $(BULLET_SRCS) \
                          $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c $(MP_EMBED_SRCS)
OUT_MP_GEOMETRY_TEST := $(BUILDDIR)/mp_geometry_test

mp_geometry_test: $(OUT_MP_GEOMETRY_TEST)
	./$(OUT_MP_GEOMETRY_TEST)

$(OUT_MP_GEOMETRY_TEST): $(MP_GEOMETRY_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) $(MP_TEST_CFLAGS) $(MP_GEOMETRY_TEST_SRCS) -o $(OUT_MP_GEOMETRY_TEST) -lstdc++ -lm
	@echo "mp_geometry_test build complete -> $(OUT_MP_GEOMETRY_TEST)"

# Phase 6's node graphs (@phi.node/phi.node_types/phi.Graph, see mp_port.c
# and node_graph.c) end-to-end against a real embedded interpreter driving
# real node_graph.c topology + real Python function calls -- same shape as
# mp_geometry_test above, same real phi_physics.cpp/Bullet link (mp_port.c's
# other bindings need real definitions regardless of whether this specific
# test calls them), plus one test node type that calls the real phi.
# create_mesh/get_vertices geometry API to prove genuine engine integration.
MP_NODE_TEST_SRCS := $(SRCDIR)/mp_node_test_main.c $(SRCDIR)/mp_port.c $(SRCDIR)/mesh_edit.c $(SRCDIR)/node_graph.c \
                      $(SRCDIR)/scene_objects.c \
                      $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                      $(SRCDIR)/light.c $(SRCDIR)/scene_target.c \
                      $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c $(BULLET_SRCS) \
                      $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c $(MP_EMBED_SRCS)
OUT_MP_NODE_TEST := $(BUILDDIR)/mp_node_test

mp_node_test: $(OUT_MP_NODE_TEST)
	./$(OUT_MP_NODE_TEST)

$(OUT_MP_NODE_TEST): $(MP_NODE_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) $(MP_TEST_CFLAGS) $(MP_NODE_TEST_SRCS) -o $(OUT_MP_NODE_TEST) -lstdc++ -lm
	@echo "mp_node_test build complete -> $(OUT_MP_NODE_TEST)"

# Console-facing glue self-test (phi_mp_init/phi_mp_exec/output capture,
# see mp_port.h) -- distinct from mp_test above (Phase 5 decorator
# patterns, out of scope here): this is what Phase 1's Console-as-real-
# Python-REPL piece actually depends on. No GL dependency, same rationale
# as mesh_edit_test/fracture_test.
MP_CONSOLE_TEST_SRCS := $(SRCDIR)/mp_console_test_main.c $(SRCDIR)/mp_port.c $(SRCDIR)/mesh_edit.c $(SRCDIR)/node_graph.c \
                         $(SRCDIR)/scene_objects.c \
                         $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                         $(SRCDIR)/light.c $(SRCDIR)/scene_target.c \
                         $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c $(BULLET_SRCS) \
                         $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c $(MP_EMBED_SRCS)
OUT_MP_CONSOLE_TEST   := $(BUILDDIR)/mp_console_test

mp_console_test: $(OUT_MP_CONSOLE_TEST)
	./$(OUT_MP_CONSOLE_TEST)

$(OUT_MP_CONSOLE_TEST): $(MP_CONSOLE_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) $(MP_TEST_CFLAGS) $(MP_CONSOLE_TEST_SRCS) -o $(OUT_MP_CONSOLE_TEST) -lstdc++ -lm
	@echo "mp_console_test build complete -> $(OUT_MP_CONSOLE_TEST)"

# Asset CRUD wire protocol client-side test (client/net.c/asset_browser.c)
# against a REAL, already-running server.py -- unlike mesh_edit_test/
# fracture_test/mp_console_test, this one talks over a real socket to a
# live server rather than being fully self-contained, so it's built here
# but NOT auto-run the way those are (there's nothing meaningful to run
# against without `python3 server/server.py` already up, plus the 3 test
# assets from tools/gen_test_assets.py already POSTed in). No GL/X11
# dependency either way -- native only (needs ws_client_native.c).
ASSET_PROTOCOL_TEST_SRCS := $(SRCDIR)/asset_protocol_test_main.c $(SRCDIR)/net.c \
                             $(SRCDIR)/asset_browser.c $(SRCDIR)/ws_client_native.c \
                             $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c
OUT_ASSET_PROTOCOL_TEST   := $(BUILDDIR)/asset_protocol_test

asset_protocol_test: $(OUT_ASSET_PROTOCOL_TEST)

$(OUT_ASSET_PROTOCOL_TEST): $(ASSET_PROTOCOL_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -Wall -Wno-unused-parameter -I$(SRCDIR) $(ASSET_PROTOCOL_TEST_SRCS) -o $(OUT_ASSET_PROTOCOL_TEST) -lm -lcrypto
	@echo "asset_protocol_test build complete -> $(OUT_ASSET_PROTOCOL_TEST) (run manually against a live server.py)"

OUT_MP_TEST_WIN32 := $(BUILDDIR)/mp_test_win32.exe

mp_test_win32: $(OUT_MP_TEST_WIN32)

$(OUT_MP_TEST_WIN32): $(MP_TEST_SRCS) | $(BUILDDIR)
	$(WIN32_CC) $(MP_TEST_CFLAGS) $(MP_TEST_SRCS) -o $(OUT_MP_TEST_WIN32) -lstdc++ -lm
	chmod +x $(OUT_MP_TEST_WIN32)
	@echo "mp_test_win32 build complete -> $(OUT_MP_TEST_WIN32)"

MP_STRESS_SRCS := $(SRCDIR)/mp_stress_test_main.c $(SRCDIR)/mp_port.c $(SRCDIR)/mesh_edit.c $(SRCDIR)/node_graph.c \
                         $(SRCDIR)/scene_objects.c \
                   $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                   $(SRCDIR)/light.c $(SRCDIR)/scene_target.c \
                   $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c $(BULLET_SRCS) \
                   $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c $(MP_EMBED_SRCS)
OUT_MP_STRESS  := $(BUILDDIR)/mp_stress

mp_stress: $(OUT_MP_STRESS)

$(OUT_MP_STRESS): $(MP_STRESS_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) $(MP_TEST_CFLAGS) $(MP_STRESS_SRCS) -o $(OUT_MP_STRESS) -lstdc++ -lm
	@echo "mp_stress build complete -> $(OUT_MP_STRESS)"

# No -s ENVIRONMENT=web here (unlike the real wasm target) — this is a
# self-test artifact, run under `node` for fast local verification, not
# something shipped to a browser. The real game's wasm build stays
# browser-only; this one deliberately doesn't, so it stays runnable without
# a browser in this environment.
OUT_MP_TEST_WASM := $(BUILDDIR)/mp_test_wasm.js

mp_test_wasm: $(OUT_MP_TEST_WASM)

$(OUT_MP_TEST_WASM): $(MP_TEST_SRCS) | $(BUILDDIR)
	$(WASM_CC) $(MP_TEST_CFLAGS) $(MP_TEST_SRCS) -o $(OUT_MP_TEST_WASM) -lm
	@echo "mp_test_wasm build complete -> $(OUT_MP_TEST_WASM) (run with: node $(OUT_MP_TEST_WASM))"

# ---------------------------------------------------------------
clean:
	rm -f $(OUT_JS) $(OUT_WASM) $(WWWDIR)/game.wasm.map $(OUT_NATIVE) $(OUT_WIN32) $(OUT_MP_TEST) $(OUT_MP_TEST_WIN32) $(OUT_MP_TEST_WASM) $(BUILDDIR)/mp_test_wasm.wasm $(OUT_MP_STRESS)

watch:
	@echo "Watching for changes..."
	while inotifywait -e modify $(SRCDIR)/*.c $(SRCDIR)/*.h 2>/dev/null; do \
		$(MAKE) wasm; \
	done
