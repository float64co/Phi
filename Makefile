# Phi — dual-target build (Phase 0: platform abstraction)
# Requires: emcc (Emscripten SDK) for wasm, gcc + X11/GLX dev headers for native
#
# Usage:
#   make wasm     — build www/game.js + www/game.wasm via emcc
#   make native   — build build/phi_native (Xlib/GLX, OpenGL 3.3 core)
#   make          — both
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
	$(SRCDIR)/console.c

.PHONY: all wasm native run clean debug watch

all: wasm native

# ---------------------------------------------------------------
# WASM (Emscripten / WebGL1)
# ---------------------------------------------------------------
WASM_CC   := emcc
WASM_SRCS := $(COMMON_SRCS) $(SRCDIR)/phi_platform_wasm.c

WASM_CFLAGS := \
	-O2 \
	-Wall \
	-Wextra \
	-I$(SRCDIR) \
	-DEMSCRIPTEN

EMFLAGS := \
	-s WASM=1 \
	-s USE_WEBGL2=0 \
	-s LEGACY_GL_EMULATION=0 \
	-s FULL_ES2=1 \
	-s USE_PTHREADS=0 \
	-s ALLOW_MEMORY_GROWTH=1 \
	-s INITIAL_MEMORY=134217728 \
	-s EXPORTED_FUNCTIONS='["_main","_net_connect_js","_malloc","_free","_input_set_pointer_locked"]' \
	-s EXPORTED_RUNTIME_METHODS='["allocateUTF8","ccall","cwrap"]' \
	-s NO_EXIT_RUNTIME=1 \
	-s MODULARIZE=0 \
	-s ENVIRONMENT=web \
	--js-library $(SRCDIR)/library_ws_stub.js \
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

$(OUT_JS): $(WASM_SRCS) $(HDRS) | $(WWWDIR)
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
clean:
	rm -f $(OUT_JS) $(OUT_WASM) $(WWWDIR)/game.wasm.map $(OUT_NATIVE)

watch:
	@echo "Watching for changes..."
	while inotifywait -e modify $(SRCDIR)/*.c $(SRCDIR)/*.h 2>/dev/null; do \
		$(MAKE) wasm; \
	done
