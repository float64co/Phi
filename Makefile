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

COMMON_SRCS := \
	$(SRCDIR)/main.c          \
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
	$(SRCDIR)/fracture.c      \
	$(SRCDIR)/gizmo.c         \
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

.PHONY: all wasm native run clean debug watch mp_test mp_test_win32 mp_test_wasm mp_stress mesh_edit_test fracture_test mp_console_test asset_protocol_test area_tree_test phi_prop_test mp_prop_panel_test phi_physics_test phi_physics_meshobject_test mp_physics_test

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
NATIVE_SRCS := $(COMMON_SRCS) $(SRCDIR)/phi_platform_native.c $(SRCDIR)/gl_native.c $(SRCDIR)/gbuffer.c $(SRCDIR)/ws_client_native.c $(SRCDIR)/http_client_native.c

NATIVE_CFLAGS := \
	-O2 \
	-Wall \
	-Wextra \
	-Wno-unused-parameter \
	-I$(SRCDIR) \
	$(MP_INCLUDES) \
	$(BULLET_INCLUDES)

NATIVE_LDFLAGS := -lX11 -lGL -lm -lcrypto -lstdc++

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

# The Python physics API surface (phi.enable_physics/apply_impulse/
# get_velocity/set_velocity, see mp_port.c) against a REAL embedded
# interpreter driving REAL Bullet simulation -- links MicroPython AND
# Bullet together, the slowest single test target here, but the only one
# that actually proves the two subsystems this pass added work together,
# not just each in isolation.
MP_PHYSICS_TEST_SRCS := $(SRCDIR)/mp_physics_test_main.c $(SRCDIR)/mp_port.c \
                         $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                         $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c \
                         $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c \
                         $(MP_EMBED_SRCS) $(BULLET_SRCS)
OUT_MP_PHYSICS_TEST := $(BUILDDIR)/mp_physics_test

mp_physics_test: $(OUT_MP_PHYSICS_TEST)
	./$(OUT_MP_PHYSICS_TEST)

$(OUT_MP_PHYSICS_TEST): $(MP_PHYSICS_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) -O1 -w -I$(SRCDIR) $(MP_INCLUDES) $(BULLET_INCLUDES) $(MP_PHYSICS_TEST_SRCS) -o $(OUT_MP_PHYSICS_TEST) -lstdc++ -lm
	@echo "mp_physics_test build complete -> $(OUT_MP_PHYSICS_TEST)"

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
MP_TEST_SRCS  := $(SRCDIR)/mp_test_main.c $(SRCDIR)/mp_port.c \
                  $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
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
MP_PROP_PANEL_TEST_SRCS := $(SRCDIR)/mp_prop_panel_test_main.c $(SRCDIR)/mp_port.c \
                            $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
                            $(SRCDIR)/phi_physics.cpp $(SRCDIR)/meshobject.c $(BULLET_SRCS) \
                            $(SRCDIR)/halfedge.c $(SRCDIR)/halfedge_gltf.c $(MP_EMBED_SRCS)
OUT_MP_PROP_PANEL_TEST := $(BUILDDIR)/mp_prop_panel_test

mp_prop_panel_test: $(OUT_MP_PROP_PANEL_TEST)
	./$(OUT_MP_PROP_PANEL_TEST)

$(OUT_MP_PROP_PANEL_TEST): $(MP_PROP_PANEL_TEST_SRCS) | $(BUILDDIR)
	$(NATIVE_CC) $(MP_TEST_CFLAGS) $(MP_PROP_PANEL_TEST_SRCS) -o $(OUT_MP_PROP_PANEL_TEST) -lstdc++ -lm
	@echo "mp_prop_panel_test build complete -> $(OUT_MP_PROP_PANEL_TEST)"

# Console-facing glue self-test (phi_mp_init/phi_mp_exec/output capture,
# see mp_port.h) -- distinct from mp_test above (Phase 5 decorator
# patterns, out of scope here): this is what Phase 1's Console-as-real-
# Python-REPL piece actually depends on. No GL dependency, same rationale
# as mesh_edit_test/fracture_test.
MP_CONSOLE_TEST_SRCS := $(SRCDIR)/mp_console_test_main.c $(SRCDIR)/mp_port.c \
                         $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
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

MP_STRESS_SRCS := $(SRCDIR)/mp_stress_test_main.c $(SRCDIR)/mp_port.c \
                   $(SRCDIR)/phi_prop.c $(SRCDIR)/phi_prop_registry.c \
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
