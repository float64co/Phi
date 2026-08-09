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

COMMON_SRCS := \
	$(SRCDIR)/main.c          \
	$(SRCDIR)/octree.c        \
	$(SRCDIR)/octree_render.c \
	$(SRCDIR)/octree_stl.c    \
	$(SRCDIR)/cmap.c          \
	$(SRCDIR)/physics.c       \
	$(SRCDIR)/renderer.c      \
	$(SRCDIR)/net.c           \
	$(SRCDIR)/input.c         \
	$(SRCDIR)/editor.c        \
	$(SRCDIR)/console.c       \
	$(SRCDIR)/halfedge.c      \
	$(SRCDIR)/halfedge_gltf.c \
	$(SRCDIR)/meshobject.c    \
	$(SRCDIR)/font.c          \
	$(SRCDIR)/svg_icon.c      \
	$(SRCDIR)/ui.c

.PHONY: all wasm native run clean debug watch mp_test mp_test_win32 mp_test_wasm mp_stress

all: wasm native

# ---------------------------------------------------------------
# WASM (Emscripten / WebGL1)
# ---------------------------------------------------------------
WASM_CC   := emcc
WASM_SRCS := $(COMMON_SRCS) $(SRCDIR)/phi_platform_wasm.c $(SRCDIR)/gbuffer.c

WASM_CFLAGS := \
	-O2 \
	-Wall \
	-Wextra \
	-I$(SRCDIR) \
	-DEMSCRIPTEN

EMFLAGS := \
	-s WASM=1 \
	-s USE_WEBGL2=1 \
	-s LEGACY_GL_EMULATION=0 \
	-s FULL_ES3=1 \
	-s USE_PTHREADS=0 \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s INITIAL_MEMORY=134217728 \
	-s EXPORTED_FUNCTIONS='["_main","_net_connect_js","_malloc","_free","_input_set_pointer_locked"]' \
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
NATIVE_SRCS := $(COMMON_SRCS) $(SRCDIR)/phi_platform_native.c $(SRCDIR)/gl_native.c $(SRCDIR)/gbuffer.c $(SRCDIR)/ws_client_native.c

NATIVE_CFLAGS := \
	-O2 \
	-Wall \
	-Wextra \
	-I$(SRCDIR)

NATIVE_LDFLAGS := -lX11 -lGL -lm -lcrypto

OUT_NATIVE := $(BUILDDIR)/phi_native

native: $(OUT_NATIVE)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(OUT_NATIVE): $(NATIVE_SRCS) $(HDRS) | $(BUILDDIR)
	$(NATIVE_CC) $(NATIVE_CFLAGS) $(NATIVE_SRCS) -o $(OUT_NATIVE) $(NATIVE_LDFLAGS)
	@echo "native build complete -> $(OUT_NATIVE)"

# ---------------------------------------------------------------
# Win32 (WGL, OpenGL 3.3 core) — built via a Windows-side MinGW-w64
# toolchain reached through WSL interop; produces a real Windows .exe.
# Windowing + GL context + rendering only for now — no Win32 input or
# Winsock networking yet (see phi_platform_win32.c's header comment).
# ---------------------------------------------------------------
WIN32_CC   := /mnt/c/msys64/mingw64/bin/gcc.exe
WIN32_SRCS := $(COMMON_SRCS) $(SRCDIR)/phi_platform_win32.c $(SRCDIR)/gl_native.c $(SRCDIR)/gbuffer.c $(SRCDIR)/ws_client_win32.c

WIN32_CFLAGS := \
	-O2 \
	-Wall \
	-Wextra \
	-I$(SRCDIR)

WIN32_LDFLAGS := -lopengl32 -lgdi32 -luser32 -lkernel32 -lws2_32 -lbcrypt

OUT_WIN32 := $(BUILDDIR)/phi_win32.exe

win32: $(OUT_WIN32)

$(OUT_WIN32): $(WIN32_SRCS) $(HDRS) | $(BUILDDIR)
	$(WIN32_CC) $(WIN32_CFLAGS) $(WIN32_SRCS) -o $(OUT_WIN32) $(WIN32_LDFLAGS)
	chmod +x $(OUT_WIN32)
	@echo "win32 build complete -> $(OUT_WIN32)"

# ---------------------------------------------------------------
# MicroPython embedding self-test (Phase 5 first slice) — NOT part of the
# game build. client/micropython_embed/ is generated output from
# MicroPython's own ports/embed tooling (see client/mpconfigport.h's header
# comment); client/mp_port.c and client/mp_test_main.c are hand-written.
# Exists to prove the embedding + decorator patterns phi.md's Phase 1/6
# design depends on actually work in real MicroPython, independent of the
# game loop, before any of that gets built into the shipped binary.
# ---------------------------------------------------------------
MP_EMBED_DIR  := $(SRCDIR)/micropython_embed
MP_EMBED_SRCS := $(wildcard $(MP_EMBED_DIR)/*/*.c) $(wildcard $(MP_EMBED_DIR)/*/*/*.c)
MP_TEST_SRCS  := $(SRCDIR)/mp_test_main.c $(SRCDIR)/mp_port.c $(MP_EMBED_SRCS)
MP_TEST_CFLAGS := -O1 -Wall -Wno-unused-parameter -I$(SRCDIR) -I$(MP_EMBED_DIR) -I$(MP_EMBED_DIR)/port

OUT_MP_TEST := $(BUILDDIR)/mp_test

mp_test: $(OUT_MP_TEST)

$(OUT_MP_TEST): $(MP_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) $(MP_TEST_CFLAGS) $(MP_TEST_SRCS) -o $(OUT_MP_TEST) -lm
	@echo "mp_test build complete -> $(OUT_MP_TEST)"

OUT_MP_TEST_WIN32 := $(BUILDDIR)/mp_test_win32.exe

mp_test_win32: $(OUT_MP_TEST_WIN32)

$(OUT_MP_TEST_WIN32): $(MP_TEST_SRCS) | $(BUILDDIR)
	$(WIN32_CC) $(MP_TEST_CFLAGS) $(MP_TEST_SRCS) -o $(OUT_MP_TEST_WIN32) -lm
	chmod +x $(OUT_MP_TEST_WIN32)
	@echo "mp_test_win32 build complete -> $(OUT_MP_TEST_WIN32)"

MP_STRESS_SRCS := $(SRCDIR)/mp_stress_test_main.c $(SRCDIR)/mp_port.c $(MP_EMBED_SRCS)
OUT_MP_STRESS  := $(BUILDDIR)/mp_stress

mp_stress: $(OUT_MP_STRESS)

$(OUT_MP_STRESS): $(MP_STRESS_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) $(MP_TEST_CFLAGS) $(MP_STRESS_SRCS) -o $(OUT_MP_STRESS) -lm
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
