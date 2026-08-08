# Phi — Engine Brief

This document is a complete context hand-off for the Phi game engine project.
It covers architecture decisions, the full development roadmap, and the
current development strategy. Intended to be passed to a fresh Claude instance
as a starting point for any phase of work.

Merged from `phi_brief.md` and `phi_desktop.md` (2026-08-08). Superseding
decisions made in this merge: FEA is out of scope (split into separate CAD
software) and the editor UI is the custom Blender DNA/RNA-style system, not
Dear ImGui.

Revised 2026-08-08 (same day, second pass): the client-authored/server-persisted
pattern — validated in `qek/`'s octree editor — is now stated explicitly as a
cross-cutting principle (see Core Pattern, below) rather than left implicit
per-phase. glTF 2.0 (`.glb`/`.gltf`) is now the mesh file format from Phase 1
onward — there is no bespoke `.pmesh` format; see Core Pattern and Phase 1.

---

## Project Identity

- **Engine name:** Phi
- **Company:** Float64 LLC
- **Codebase location:** `qek/` (legacy project name, engine is being renamed Phi)
- **File extensions:** `.glb` / `.gltf` (meshes, skeletons, animation clips), `.panim` (runtime-optimised animation repack), `.pscene` (scene file)
- **Python API prefix:** `phi.*` (e.g. `phi.mesh_object()`, `phi.spawn()`)

---

## Vision

A browser-native and desktop game engine. Two and only two build targets:

- `make wasm` → `engine.wasm` (browser, WebGL 2)
- `make native` → native executable (OpenGL 3.3 core, `glext.h`)

Distributed as an opaque `engine.wasm`. Users get a mesh editor, animation
editor, and a node graph system. All game logic, NPC behaviour, asset authoring,
and node definitions are written in Python — the same Python everywhere, with no
conceptual boundary between scripting the game and building the tools.
Multiplayer runs peer-to-peer via WebRTC; the server becomes a thin signaling
process.

---

## Core Pattern: Client-Authored, Server-Persisted

This is the architectural principle underlying every editor in Phi — the mesh
editor, the animation editor, the node graph, the scene/level layout, even
panel-layout customisation. It was validated in `qek/` before Phi existed: the
in-viewport octree editor lets a client sculpt geometry locally, sends the edit
to the server as a small delta, the server mutates its authoritative copy and
persists it to disk, and rebroadcasts the resulting state to every connected
client. The same optimistic-local / authoritative-remote / reconcile loop
already used for player movement turned out to work unmodified for content
authoring. Phi generalizes this rather than inventing a separate mechanism
per editor:

1. The client applies an edit optimistically to its local in-memory copy the
   instant the user acts — a dragged vertex, a repainted material, a moved
   keyframe, a new node — so the UI never waits on a round trip.
2. The edit is serialized as a small delta (not a full-asset re-upload) and
   sent to the server.
3. The server is authoritative: it applies the same mutation to its copy,
   persists it (to a `.glb`, a `.pscene`, a project file — whatever the asset
   type is), and rebroadcasts the resulting state to every connected client,
   including the one that sent the edit.
4. The client reconciles: in the common case the optimistic local edit matches
   what comes back and nothing visibly changes; on conflict, the server's
   version wins and the client snaps to it.

### Re-earning low latency, per editor

Client-side prediction is not free — it has to be built again for every editor
that wants edits to feel instant, because what counts as "the state" differs
each time (an octree region, a mesh half-edge structure, a keyframe list, a
node graph). Phase 1's mesh editor is where this pattern gets built out fully
in Phi for the first time; later editors (Phase 4's animation timeline, Phase
6's node graph) reuse the transport and persistence conventions but still need
their own delta format and their own optimistic-apply/reconcile logic for
their specific data structure. This is real, repeated work, not a one-time
framework cost — budgeted into each phase's estimate below rather than assumed
to be solved once in Phase 1.

### Multi-user editing as an emergent property

Because the server is already authoritative and already rebroadcasts on every
edit, two people editing the same asset at the same time is not a separate
feature to design — it falls out of the pattern for free, the same way a
second player joining a Qek match just works because the server was already
broadcasting octree edits to everyone. Conflict resolution stays simple
(last-write-wins per delta) rather than requiring operational transforms or
CRDTs, because deltas are small and edits are human-paced.

### Where AI fits

An Anthropic API key can be configured on the authoring server (never shipped
to any client, never embedded in `engine.wasm`) to power an in-editor
assistant: defining `@phi.panel` UI from a description, scaffolding a
`@phi.node` function, writing a first pass of NPC coroutine behaviour, or
suggesting fixes to a script. The assistant participates in the same
client-authored/server-persisted loop as a human would — it proposes edits,
the server applies and persists them the same way it would for a mouse drag,
and every connected client sees the result. It never gets a privileged path
that bypasses the authoritative server, and no client ever holds the key.

### Authoring vs. shipping

The authoring server (with its `.glb`/`.pscene`/project-file persistence, its
AI assistant, its multi-user broadcast) is a development-time system. It is
not what ships to players. A **publish** step bakes the authored project —
meshes, animation repacks, node graphs "ejected" to plain Python where
desired, scene files — into the flat, opaque asset bundle described under
Distribution Model, below. Shipped `engine.wasm` runtimes never talk to an
authoring server, never see an API key, and never receive live edit
broadcasts; they load baked assets exactly the way the existing Qek client
loads a `.cmap` handed to it at connect time, minus the "keep editing it
live" half of the loop.

---

## Hard Architectural Decisions (Non-Negotiable)

These have been explicitly decided and must not be revisited without a conscious
conversation:

| Decision | Rationale |
|---|---|
| No ImGui | Native UI system modelled on Blender DNA/RNA, written in C, panels authored in Python via `@phi.panel` |
| No Tauri | Output is `.wasm` or `glext.h`-based native executable only |
| No SDL / GLFW | Native platform APIs directly; wrapped behind `phi_platform.h` |
| No ammo.js | Bullet physics compiled directly via `emcc` |
| WebGL 2 from day one | `USE_WEBGL2=1`, `FULL_ES3=1`; no WebGL 1.0 constraints anywhere |
| MicroPython (not CPython/Pyodide) | ~200–400 KB compiled into `engine.wasm` |
| glTF 2.0 (`.glb`/`.gltf`) as the native mesh format, from Phase 1 onward | `cgltf` single-header parser; no bespoke `.pmesh` format, ever |
| Live-edited mesh state is not glTF buffers in memory | Editor holds a half-edge/winged-edge structure; glTF is the load/save interchange format, not the working representation |
| Anthropic API key lives server-side only | Never shipped to a client or embedded in `engine.wasm`; see Core Pattern |
| `glext.h` for desktop GL | No GL loader library |
| No FEA in this engine | Split out into separate CAD software — not this project's scope |

---

## Current Development Strategy: Native-First

Development happens natively (OpenGL 3.3 core on Linux/Windows/macOS). WASM is
a compile target, not a daily driver. When the codebase stays within the GLES 3.0
feature set, `emcc` translates it to WebGL 2 with no porting step — just a
different compiler invocation.

### The one discipline constraint

Only use GL features that exist in GLES 3.0 / WebGL 2. In practice this means:

| Forbidden on desktop | Use instead |
|---|---|
| `glMapBuffer` / `glMapBufferRange` | `bufferSubData` / `getBufferSubData` |
| `glPolygonMode(GL_LINE)` | Barycentric wireframe in fragment shader |
| Geometry shaders | Expand in vertex shader or CPU |
| Tessellation shaders | CPU subdivision or transform feedback |
| Compute shaders | Fullscreen fragment shader pass or CPU |
| `glDrawElementsBaseVertex` | Bake base vertex into index buffer |

The G-buffer texture formats already specified (`RGBA8`, `RGB10_A2`,
`R11F_G11F_B10F`, `RG16F`, `DEPTH24_STENCIL8`, `R32UI`) are all native WebGL 2
— no issues there.

### Platform split

```
phi_platform_native.c   — Win32/Xlib/Cocoa windowing, glext.h proc loading
phi_platform_wasm.c     — emscripten_set_main_loop, canvas queries, EM_ASM
phi_platform.h          — the interface both implement
everything else         — plain C, compiles with gcc/clang or emcc unchanged
```

`phi_platform_wasm.c` is deferred until a WASM build is needed. Everything
outside `phi_platform.c` contains zero `#ifdef EMSCRIPTEN` guards.

### WASM cadence

Compile to WASM at the end of each phase milestone to catch GL feature drift
before it compounds. A CI job that runs `emcc` and checks it links (without
running) is sufficient between milestones.

---

## Distribution Model

```
Distributed to users:
  engine.wasm      — opaque binary: engine + editor + MicroPython + Bullet
  phi.h            — public C API header (no implementation)
  editor.html      — editor shell
  runtime.html     — game runtime shell

User-authored:
  *.py             — gameplay logic, node types, NPC behaviours
  *.glb / *.gltf   — meshes, skeletons, animation clips — from the mesh
                      editor (Phase 1 onward) or imported from a DCC tool
  *.panim          — runtime-optimised animation clip (repack of glTF animation)
  *.pscene         — scene file referencing assets + script bindings
  *.wasm           — compiled C modules (side modules, cloud-compiled)
```

**Python scripts** are interpreted at runtime by the embedded MicroPython — zero
toolchain required. **C modules** are written in the editor, compiled server-side
via an `emcc` endpoint, and returned as WASM side modules. The engine source is
never shipped. Users keep their `.c` source; they receive a `.wasm` side module.

WASM binaries are not cryptographically opaque (`wasm2c` can decompile them), but
this is equivalent to shipping a compiled `.dll` — sufficient for practical IP
protection.

---

## Phase 0 — Platform Abstraction and Renderer Foundation

### Goal

Establish the platform abstraction layer before any feature work. Every
subsequent phase builds on this foundation. The codebase contains no framework,
no packaging shell, no webview — just C.

### Platform abstraction (`phi_platform.h`)

```c
void phi_platform_init(PhiPlatformConfig *cfg);
void phi_platform_set_main_loop(PhiMainLoopFn fn, void *userdata);
void phi_platform_swap();
void phi_platform_get_window_size(int *w, int *h);
void *phi_gl_get_proc(const char *name);
```

File I/O, timing, and high-resolution timers live in the same layer.

### Networking abstraction (`phi_net.h`)

| | Browser | Desktop |
|---|---|---|
| WebSocket | `library_ws_stub.js` (Emscripten) | BSD sockets |
| WebRTC | Browser WebRTC API via JS | libdatachannel (C API, MIT) |

### Deferred Renderer and G-Buffer

The renderer is deferred from day one.

#### G-buffer layout

| Name | Format | Contents |
|---|---|---|
| `gbuf.albedo` | `RGBA8` | base colour (RGB) + ambient occlusion (A) |
| `gbuf.normal` | `RGB10_A2` | world-space normal (RGB) + metallic (A) |
| `gbuf.material` | `RGBA8` | roughness (R), emissive mask (G), object tag (B), spare (A) |
| `gbuf.emissive` | `R11F_G11F_B10F` | emissive colour |
| `gbuf.velocity` | `RG16F` | screen-space motion vectors (TAA, motion blur) |
| `gbuf.depth` | `DEPTH24_STENCIL8` | depth (samplable) + stencil |
| `gbuf.object_id` | `R32UI` | per-pixel object identity (pixel-perfect selection) |

#### Render pipeline insertion points

```
[geometry]     → writes G-buffer
[shadow]       → DEPTH_COMPONENT32F shadow maps
               ← "after_gbuffer"   user passes insert here
[lighting]     → HDR accumulation (RGBA16F)
[transparent]  → renders into HDR buffer
               ← "after_lighting"  user passes insert here
[taa]          → resolves RGBA16F using velocity buffer
[bloom]        → R11F_G11F_B10F
               ← "after_resolve"   user passes insert here
[tonemap]      → LDR RGBA8
[fxaa]         → final output
               ← "after_tonemap"   user passes insert here
```

#### User-defined passes (Python)

```python
@phi.render_pass(insert="after_gbuffer")
class SSAO(phi.Pass):
    radius  = phi.FloatProperty(default=0.5, min=0.01, max=2.0)
    samples = phi.IntProperty(default=16, min=4, max=64)
    output  = phi.TextureOutput(format="R8")

    def setup(self, ctx):
        ctx.bind("u_normal",  phi.gbuffer.normal)
        ctx.bind("u_depth",   phi.gbuffer.depth)
        ctx.bind("u_noise",   phi.texture("noise4x4.png"))
        ctx.uniform("u_radius",  self.radius)
        ctx.uniform("u_samples", self.samples)
        ctx.fullscreen("ssao.glsl")
```

### Deliverables

- [x] Two build targets from a single codebase: `make wasm` and `make native`
- [x] Existing arena game running as a native executable via the new platform
      layer — `phi_platform.h`/`phi_platform_wasm.c`/`phi_platform_native.c`
      (Xlib/GLX, OpenGL 3.3 core via `glXCreateContextAttribsARB`), native
      keyboard/mouse input (X11, pointer-grab-and-warp for FPS look), a
      GLSL 330 core shader variant + persistent VAO alongside the existing
      GLES2/WebGL1 path. Verified end to end on Linux/WSL2 (Mesa/llvmpipe):
      real simulation ticks, `glReadPixels` confirms lit-geometry output
      (not a black frame), and a synthetic X11 keypress moves the player.
      Native networking now works too — `client/ws_client_native.h`/`.c`
      is a real RFC 6455 client over BSD sockets (handshake, masked
      client→server framing, non-blocking per-frame poll), verified
      against the actual Python server: full handshake, `PKT_HELLO`
      round-trip, and a 100KB+ `PKT_MAP_FULL` correctly received. wss://
      (TLS) isn't implemented — `ws://` only, matching what the local dev
      server itself speaks.
- [x] `glext.h` GL loading verified on Linux and Windows — **macOS
      explicitly out of scope for now** (no access to a Mac in this
      environment; not attempted). Windows verification runs through
      `client/phi_platform_win32.c` (Win32/WGL, a third `phi_platform.h`
      implementation — the existing "native" backend is Xlib/GLX,
      Linux/X11-specific, not portable, despite the name), built with a
      Windows-side MinGW-w64 toolchain invoked via WSL interop
      (`make win32` — see the Makefile). Verified against **real
      hardware**, not a software rasterizer: `GL_RENDERER=Intel(R)
      Arc(TM) Pro Graphics`, `GL_VERSION=3.3.0`. The G-buffer/lighting/
      tonemap pipeline (`gbuffer.c`, already written purely against the
      portable `phi_gl_get_proc` abstraction, no Linux-specific code)
      worked with zero changes, and the center-pixel readback came back
      pixel-identical to the Linux native build — `(90,90,99)` on both,
      despite running on entirely different OS/GPU/driver stacks. Started
      as windowing + GL context + rendering only, matching how the Linux
      native backend also got a rendering-only milestone before input/
      networking followed — **since then, Win32 keyboard/mouse (WndProc
      forwarding to `input_native_handle_event`, verified with a
      `PostMessageA`-based synthetic-input test harness: real player
      movement) and Winsock networking (a port of `ws_client_native.c`
      onto Winsock2, SHA1 via Windows' own CNG/BCrypt API instead of
      OpenSSL since this MinGW install doesn't have it, verified against
      the real server the same way as Linux) have both landed** — Windows
      native now has input/rendering/networking parity with Linux native.
- [x] Deferred renderer with G-buffer writing and basic lighting pass —
      **wasm and native, both confirmed working** (see below)
- [x] `readPixels` object ID selection working — **wasm and native**
- [x] CI builds for both targets — `scripts/ci_check.sh` (local script, not
      hosted; runs `make native` always and `make wasm` when `emcc` is on
      `PATH`)

**On the G-buffer/readPixels scope split (now closed):** when this was
first built, wasm was still WebGL1/GLES2, which can't do multiple render
targets, float textures, or integer textures — no faithful G-buffer
possible there. Rather than block the deferred renderer on the separate
WebGL2 upgrade, `client/gbuffer.h`/`.c` implemented the full layout from
this section natively only (`GL_RGBA8`/`GL_RGB10_A2`/
`GL_R11F_G11F_B10F`/`GL_RG16F`/`GL_R32UI`/`GL_DEPTH24_STENCIL8`, lighting
pass to an `RGBA16F` accumulation buffer, tonemap pass to the default
framebuffer, `gbuffer_pick_object_id` reading the object_id attachment),
leaving wasm on its original forward path. **wasm was then upgraded to
WebGL2/GLES3** (`USE_WEBGL2=1`/`FULL_ES3=1`, GLSL ES 3.00 shaders,
confirmed working in a real browser by the user), **and the G-buffer
itself has since been extended there too** — `gbuffer.c` now compiles
and runs on both targets from the same source (Emscripten declares
`glGenFramebuffers`/`glDrawBuffers`/`glGenVertexArrays`/
`glClearBufferuiv` etc. as directly-linkable GLES3 functions, no
proc-fetching shim needed there unlike native's GL 3.3 core path), with
`EXT_color_buffer_float` explicitly enabled for the float-format
targets. Verified end-to-end by the user running the actual browser
build over the network, across three rounds — the first two surfaced
real, WebGL2-specific bugs neither of us could have caught without a
live browser: (1) `gbuffer.c`'s own fullscreen-quad VAO silently
stealing the vertex-attribute binding away from the world-mesh draw
call, an exact repeat of a bug already fixed on native once, that
resurfaced on wasm the moment it started sharing `gbuffer.c` too —
fixed by making the defensive per-draw VAO rebind unconditional on both
targets instead of native-only; (2) two separate illegal `readPixels`
format/type combinations that desktop GL accepts but WebGL2/ANGLE
rejects outright (`GL_DEPTH_COMPONENT`/`GL_FLOAT` against a depth-only
FBO, and plain `GL_RGB`/`GL_UNSIGNED_BYTE` instead of the universally-
legal `GL_RGBA`) — the first guarded to native-only, the second fixed
to use the portable format; a failed `readPixels` call doesn't write
its output buffer at all, which is why one of these read back as a
stuck, unchanging value across ~2000 frames and briefly looked like
rendering itself had frozen, when the real bug was just in how the
diagnostic sampled it. Third round: GL-errorless, center-pixel samples
now visibly vary with the player's real position and view direction.
`material`/`emissive`/`velocity` are allocated per
the layout above but not yet meaningfully populated — this renderer has
no PBR params, no emissive surfaces, and no motion-vector tracking yet,
so future work slots into the existing format rather than needing a
G-buffer schema change. **Shadow maps have since landed on all three
targets** — `gbuffer_render_shadow_map` renders the world mesh
depth-only from a fixed directional light's point of view into a
`DEPTH_COMPONENT32F` map; the lighting pass reconstructs each fragment's
world position from G-buffer depth via the camera's inverse view-
projection, projects it into light space, and dims the diffuse term
when occluded. Scoped narrowly: only the static world mesh casts/
receives shadows (not players/rockets), the light-space ortho volume is
a fixed box hand-picked to cover the default arena rather than fitting
itself to whatever's actually built (editor-built geometry far outside
that footprint won't shadow correctly), and there's no PCF/soft edges —
a first pass the rest of shadowing can build on, not the finished thing.
Verified on native two ways: the matrix math (a general 4x4 inverse,
needed for world-position reconstruction) was checked numerically in
isolation first (`VP · VP⁻¹` = identity, a world point round-trips
exactly through clip space and back) before it was trusted in the
shader; and the shadow map's depth values were read back directly and
found genuinely varied (not degenerate/uniform) — `0.3093 1.0000 1.0000
0.3850 0.3854` across 5 sample points, **identical** on Linux
(Mesa/llvmpipe) and Windows (Intel Arc Pro Graphics). That specific
depth-readback diagnostic doesn't run on wasm (`GL_DEPTH_COMPONENT`/
`GL_FLOAT` isn't a legal WebGL2 `readPixels` combination — see below),
but the shadow map itself is built and sampled through ordinary texture
sampling in the shader there, a fully portable code path unrelated to
that restriction, and the user confirmed a GL-errorless, correctly-
updating browser session after the wasm G-buffer extension landed.
**FXAA, bloom, and transparency have since landed** on top of the
shadow-mapped deferred pipeline above, in that order (all three confirmed
building on native/win32/wasm; FXAA+bloom additionally confirmed
GL-errorless in a real browser). FXAA: tonemap now writes to an
intermediate LDR texture (`ldr_fbo`/`ldr_tex`) instead of the default
framebuffer directly, and a new FXAA pass (standard luma-edge-detection
formulation, NVIDIA-whitepaper-derived, not invented) reads it and writes
the actually-presented frame. Bloom: threshold-extract (>1.0 luminance) →
2-pass separable Gaussian blur (the widely-circulated LearnOpenGL 9-tap
weight set) → additive composite back into the HDR buffer before
tonemap. Honest caveat, verified two ways rather than assumed: current
lighting math never exceeds ~1.0 HDR (no emissive materials, no
over-bright lights), so this is correct, working plumbing with nothing
to visibly bloom under current game content — confirmed on native by
reading back the bright-pass center pixel at the real threshold (0,0,0),
then temporarily lowering the threshold to 0.01 and confirming genuine
non-zero extraction (0.2998,0.5000,0.7998), then reverting. Transparency:
a forward-blended pass into the HDR buffer, depth-tested (not
depth-writing) against the opaque scene's depth — `hdr_fbo` now shares
`tex_depth_stencil` with the G-buffer's own `fbo` (a texture can be
attached to more than one FBO), so `glDepthMask(GL_FALSE)` +
`glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` correctly composites
without occluding other transparent draws or the opaque pass. There's no
transparent content in the game yet to exercise this with, so a small
fixed NDC-space test quad (bypassing any world/camera transform
entirely, at a shallow near-plane depth) stands in — verified on native
by reading the HDR center pixel immediately before and after the blend
within the same frame (so scene content is identical between the two
reads) and confirming the result matches the standard over-blend
equation exactly: `bg=(0.2998,0.5000,0.7998) expected=(0.6499,0.2500,
0.3999) actual=(0.6499,0.2500,0.3999)`.

**TAA has since landed too** (native/win32/wasm build-confirmed),
completing everything in this set that doesn't need MicroPython — only
the `@phi.render_pass` insertion-point system is still unstarted, and
deliberately so (Phase 5). Scoped-down first pass, stated plainly rather
than overclaimed: the velocity buffer (`renderer.c`'s new `u_prev_mvp`
uniform, `Renderer.prev_vp` snapshotted once per frame by the new
`renderer_end_frame()`) captures **camera motion only** — every draw
call reuses its own current-frame model matrix for the "previous"
reprojection too, so a fast-moving player/rocket's own motion isn't
captured, only the parallax from camera movement. The resolve pass
(`gbuffer.c`'s `taa_tex_a`/`taa_tex_b` ping-pong pair, between tonemap
and FXAA) reprojects a history buffer via that velocity, clamps it
against a 4-tap cross neighborhood AABB (a cheaper variant of the
standard neighborhood-clamping anti-ghosting technique) to guard against
ghosting, and blends 90% history / 10% current — matching the shadow
map's "static-only" precedent for a correct, first-pass scope rather
than the finished thing. Verified on native by reconstructing the exact
same neighborhood-clamp-and-blend arithmetic on the CPU from raw texel
reads (ldr_tex's center + 4 neighbors, the reprojected history texel via
NEAREST sampling, tex_velocity) and comparing bit-exact against the
GPU's actual output — done twice, once with the trivial zero-velocity
case (camera not yet moved) and once with genuine non-zero velocity
(`vel=(0.00001,0.01775)`, driven by a real X11 synthetic-input test, the
same `XSendEvent`-based harness used earlier in this phase for input
verification) — both matched exactly. Honest limit, stated rather than
glossed over: whether the result is actually ghosting-free in practice
is a visual judgment call that can't be fully proven headlessly: the
mechanical parts (velocity values, history read/write, clamp/blend math)
are what's numerically verified here, not the subjective visual outcome.

**Effort:** 5–7 weeks *(platform abstraction across all three targets
(wasm/Linux-native/Windows-native) — all with real input and real
networking now, not just Linux — the wasm WebGL2 upgrade, CI check, and
the deferred renderer/G-buffer/shadow-map/FXAA/bloom/transparency/TAA
pipeline (all confirmed building on wasm, Linux native, and Windows
native alike, FXAA+bloom additionally confirmed GL-errorless in a real
browser): done — see the scope-split note above for what "done" means
and how each platform was actually verified, not just built. Remaining:
macOS (explicitly deferred, no access), and the `@phi.render_pass`
insertion-point system (deliberately deferred to Phase 5/MicroPython).)*

---

## Phase 1 — Mesh Editor

### What it is

An in-browser editor for creating and modifying discrete mesh objects. The octree
world remains for static terrain; mesh objects are a new entity class that floats
above it, can be transformed, and participates in physics.

### Status: non-interactive foundation landed, editor itself not started

The **non-interactive foundation** this phase depends on is done and build-
verified on all three targets (wasm/native/win32) — deliberately scoped this
way rather than attempting the whole phase at once, since the interactive
editor (UI system, gizmos, picking, editing operations, fracturing, physics)
involves real design decisions that need a conversation, not autonomous
implementation:

- **cgltf** (single-header C99 glTF 2.0 parser, MIT, vendored at
  `client/cgltf.h`) — the Hard Architectural Decisions table already named
  this as the intended parser, so it's a pre-approved dependency, not a new
  one. Confirmed compiling cleanly with gcc, MinGW-w64, and emcc.
- **Half-edge (winged-edge) mesh structure** (`client/halfedge.c`/`.h`) — the
  standard textbook formulation (same family Blender/most mesh editors use
  internally), scoped to triangles only for now (n-gon support isn't
  exercised yet). Twin-edge lookup is a linear scan, O(n) per edge add — fine
  for this phase's small test assets, flagged honestly as something a real
  editor-scale mesh would want a hash map for instead. Verified via a
  standalone test on a unit cube: correct vertex/face/edge counts (8/12/36),
  zero boundary edges (a closed 2-manifold), every twin relationship checked
  both symmetric AND geometrically opposite (not just structurally
  plausible), and an exact round-trip through build → flatten.
- **glTF load/save** (`client/halfedge_gltf.c`/`.h`) — loads mesh 0/primitive
  0's POSITION + indices via cgltf into a HalfEdgeMesh; saves by flattening
  back to a hand-written minimal single-primitive glTF (JSON text + a
  sibling `.bin`, cgltf itself is read-only). Verified with a real
  load → save → reload round-trip through the actual cgltf parser (not just
  hand-rolled test data) against `assets/cube.gltf` (a hand-authored,
  8-vertex/12-triangle unit cube test asset, generated via a one-off Python
  script) — positions and face topology matched exactly.
- **Quaternion → rotation matrix** (`client/meshobject.c`'s `quat_to_mat4`) —
  verified numerically before use (identity in → identity out; a known
  90-degree rotation checked against the exact expected axis mapping;
  determinant 1 confirmed, i.e. a proper rotation with no skew/reflection),
  matching this codebase's established bar for matrix/rotation code (see
  `renderer.c`'s `mat4_inverse`).
- **MeshObject rendering**: a minimal `MeshObject` entity (id, position,
  quaternion orientation, `RenderMesh`, `is_static` — deliberately without
  the `ConvexHull`/Bullet physics field from the original sketch below,
  since Phase 2 owns physics and a placeholder field would just be dead
  weight) loads `assets/cube.gltf` at startup and renders through the
  *existing* G-buffer geometry pass (`renderer_draw_mesh_object`, sharing
  the same shader every other draw call uses) as a fixed, static, non-
  interactive object. Verified on native via a deterministic diagnostic
  camera override (the real gameplay camera's position depends on physics/
  network timing, so it can't be relied on to actually be looking at the
  object) plus a coarse object-id framebuffer scan — consistent across
  repeated runs once the test object was moved clear of ground-level bot
  traffic (an early run intermittently showed 0 visibility hits; root-
  caused to a bot occasionally standing in the direct camera-to-object line
  of sight, not a rendering bug — confirmed via debug output showing
  identical camera/MVP math every run, only the occluding object-id
  differing). The wasm build embeds `assets/` directly into the module via
  `--embed-file` (Makefile) so `fopen("assets/cube.gltf")` resolves the same
  way native's real filesystem access does — confirmed functionally (not
  just via the build succeeding) by running the actual embedded-FS + cgltf
  path under Node with the environment restriction temporarily lifted, since
  a plain text search of the minified output isn't a reliable way to confirm
  embedded binary data is really there. **Confirmed end-to-end in a real
  browser**: the embedded asset loads, the object-id readback finds the
  cube genuinely rasterized on screen (10609 hits at one checked frame),
  and the whole session runs GL-errorless — same real-browser verification
  standard as every render-pipeline feature in Phase 0.

**Not started**: the native UI system (DNA/RNA property system, SDF font
widget rendering, panel layout — see below), transform gizmos, ray-vs-mesh
picking, extrude/inset/loop-cut editing operations, PBR material assignment,
Voronoi fracture tooling, and Bullet physics integration for mesh objects.

### Architecture

A new `MeshObject` entity type sits alongside `Player` and `Rocket` in the game
state. WebGL 2 is the rendering target from day one (`USE_WEBGL2=1`,
`FULL_ES3=1`), so the vertex format carries a full PBR material index rather than
a flat-palette slot, and integer vertex attributes (bone indices) work natively.
A quaternion replaces Euler angles for the object transform to support arbitrary
tumbling under physics.

```c
typedef struct {
    int       id;
    vec3      position;
    quat      orientation;
    RenderMesh render_mesh;
    ConvexHull phys_hull;     // for Bullet
    int        is_static;
} MeshObject;
```

### Editor operations

- Ray-vs-mesh picking (mouse selection)
- Transform gizmo (translate, rotate, scale)
- Face/vertex/edge selection modes
- Extrude, inset, loop cut (basic operations)
- PBR material assignment per face (baseColor, metallic, roughness, emission)
- Import from binary STL (the existing STL exporter gives the inverse template)
- Import/export `.glb` and `.gltf` directly — this is the save/load format,
  not a conversion step
- Voronoi fracture tool (pre-computes N fragments, stored via a Phi glTF
  extension — see Fracturing, below)

### Editable mesh structure vs. glTF

glTF is an interchange format: flat vertex/index buffers, good for GPU upload
and for round-tripping through DCC tools, bad for editing — it has no concept
of an edge or a loop, so operations like extrude or loop-cut have nothing to
walk. The editor's live in-memory representation is a **half-edge (winged-edge)
mesh structure**, the same family of data structure Blender and most mesh
editors use internally. glTF is only ever touched at the boundary:

- **Load**: parse `.glb`/`.gltf` via `cgltf`, build a half-edge structure from
  its vertex/index buffers.
- **Edit**: every editor operation (extrude, inset, loop cut, material
  assignment, fracture) mutates the half-edge structure. This is also the
  representation that generates the small deltas sent to the server under the
  Core Pattern.
- **Save**: flatten the half-edge structure back to glTF vertex/index buffers
  (triangulating as needed) and write `.glb`/`.gltf`.

Phi-specific data that has no glTF equivalent — fracture fragment groups,
precomputed convex hulls for Bullet — rides in glTF's standard
`extensions`/`extras` mechanism (a namespaced `PHI_fracture_fragments` /
`PHI_physics_hull` extension) rather than forking the format or falling back
to a side-channel file. A `.glb` produced by Phi's editor still opens cleanly
in Blender; Phi-specific data is simply data Blender doesn't render.

### Native UI System

Built in C, rendered entirely via WebGL 2. No third-party UI library — this is
the resolved decision over Dear ImGui (see Hard Architectural Decisions).

**Design reference**: evaluated a separate, mature CAD tool's C++/Python UI
codebase as reference material (brought in temporarily, read-only, removed
once this phase is far enough along — nothing here should assume its files
still exist). Confirms the core bet: a hand-rolled immediate-mode C++ UI
(no ImGui) driven by Python panel callbacks is a proven, working pattern, not
just a plan. Two decisions it directly informed:
- Its Python layer is CPython + pybind11, not MicroPython — the *shape* of
  the binding pattern (global setter functions registering live engine
  pointers, e.g. `zenith_set_scene(SceneGraph*)`, panels holding a Python
  callable invoked each frame) ports cleanly; the binding *code* doesn't,
  since pybind11 doesn't exist for MicroPython and its C API is narrower.
  Budget real design time for the binding layer rather than assuming a port.
- Its own node-graph-style canvas (an LLM-conversation graph, not geometry —
  but the interaction mechanics are the same: pan/zoom, draggable/
  resizable nodes, socket hit-testing, wire drag-and-drop) is built entirely
  in Python over a generic 2D immediate-mode drawing primitive (rect/line/
  circle/text), with no dedicated C++ node-graph widget at all. Phi
  deliberately diverges from that for its own node graph and curve editor —
  see below and Phase 6 — because large graphs need C++ to own the
  topology, not just the rendering.

#### Property System (DNA/RNA analogue)

```c
typedef struct PhiProp {
    const char *identifier;
    const char *name;
    PhiPropType  type;
    float        range[2];
    float        default_val;
    UpdateFn     on_update;
} PhiProp;
```

Single source of truth used by UI widgets, animation system, node graph, and
Python API. Anything visible in a panel is automatically animatable,
node-driveable, and scriptable.

#### Widget rendering

All widgets are textured quads in the WebGL 2 context. SDF fonts — one atlas
baked at startup, crisp at any size. Hit testing is a rectangle walk.

**Panel layout**: Blender's actual model, not a simpler fixed-slot dock (the
reference CAD tool above uses the latter — a curated 4-slot icon-strip dock,
one active slot at a time — which is simpler to build but isn't what was
asked for here). Screen space is a recursive tree of areas; any area can
split horizontally or vertically, the split's edge is draggable, and
dragging a shared corner between four areas resizes all of them at once.
Each leaf area hosts exactly one editor type (3D viewport, node graph, curve
editor, property panel, ...) chosen from a type dropdown, same as Blender.
This is user-defined layout, not a fixed set of panel positions — the
specific split tree is per-project saved state, not engine-hardcoded.

Elements that aren't 2D panel chrome are rendered separately, as raw WebGL calls
alongside (not through) the panel system, since they're 3D scene content rather
than UI:

| Element | Rendered by |
|---|---|
| Property panels, toolbar, menus, animation timeline | Custom C UI system (DNA/RNA property panels, above) |
| Node graph canvas, curve editor | Custom C UI system — C owns topology/keyframe data and rendering/interaction (not just drawing), so both stay fast at large graph/curve sizes; see Phase 6 for why and how Python (or an LLM-generated script) still authors a whole graph or curve set in one call rather than only incrementally via mouse drags |
| Transform gizmos | WebGL draw calls, 3D geometry |
| Selection highlights | WebGL stencil outline pass (two-pass: draw selection into stencil, then a scaled-up copy wherever the stencil is clear) |
| Wireframe overlay | Barycentric coordinates as a vertex attribute, thresholded in the fragment shader — WebGL 2 has no `GL_LINE` polygon mode |
| Loading screen, project picker | HTML/CSS (browser shell only) |
| Text input (rename, numeric entry) | Native `<input>` via JS shim — gets browser IME and accessibility for free without building a text editor into the custom UI system |
| Custom user panels | Python via `@phi.panel` decorator |

#### Python-extensible panels

```python
@phi.panel("Fracture Tools")
class FracturePanel(phi.Panel):
    def draw(self, ctx):
        ctx.prop(ctx.active_object, "voronoi_seeds")
        ctx.separator()
        if ctx.button("Preview"):
            phi.emit("preview_fracture")
        if ctx.button("Apply"):
            phi.emit("apply_fracture")
```

This is structurally identical to how Blender defines its panels in Python — the
panel body is Python, the rendering is C.

### Fracturing

Meshes are authored with a fracture pattern at creation time. The editor provides
a Voronoi fracture tool that pre-computes N fragments and stores them as a
`PHI_fracture_fragments` glTF extension on the `.glb` — each fragment is an
ordinary glTF mesh primitive, referenced by the extension's fragment list
rather than duplicated into a separate file. At runtime, on a fracture event
(threshold impact, explosion, script trigger), the source mesh is hidden and the
fragment rigid bodies are activated with the appropriate impulse.

Runtime Voronoi fracturing is deferred — pre-fracturing covers the vast majority
of game use cases.

**Effort:** 7–10 weeks *(glTF I/O plus the half-edge editing structure now
land in this phase rather than being assumed infrastructure; partially offset
by Phase 4 no longer needing to stand up its own glTF pipeline)*

---

## Phase 2 — Physics

### Bullet Physics via Emscripten

Bullet's C++ source compiles directly with `emcc` and links into `engine.wasm` as
a first-class subsystem — no JS bridge, no ammo.js middleware. The engine calls
Bullet's C API. Bullet adds approximately 2–4 MB to the WASM binary.

Emscripten flags: `USE_WEBGL2=1`, `FULL_ES3=1`, `USE_PTHREADS=0` (Bullet's
deterministic mode requires no threads), `ALLOW_MEMORY_GROWTH=1`.

Bullet is deterministic given identical initial conditions and a fixed timestep.
For P2P, the host client runs the authoritative simulation and broadcasts rigid
body states (position + quaternion + linear/angular velocity). Other clients
receive states and interpolate — the same server-authoritative model as today,
with a peer in the server role.

### Python API surface

```python
obj = phi.mesh_object("crate.glb")
obj.position = (10, 5, 0)
obj.mass = 50.0
obj.restitution = 0.4
obj.on_impact(threshold=800, action="fracture")

# apply a one-shot impulse
obj.apply_impulse((0, 1000, 0), at=obj.centroid)
```

**Effort:** 4–6 weeks

---

## Phase 3 — Offline Raytracer and Video Export

### What it is

A CPU path tracer compiled into `engine.wasm` that renders animation frames
offline at arbitrary quality, sends them to the server as PNG files, and
optionally invokes ffmpeg to produce a video. Gives users Blender-style "render
this animation" output from inside the editor with no external tools required.

### The raytracer

The octree already has `octree_ray_cast()` — the primary intersection primitive is
written. A unidirectional path tracer in C builds on top of it:

- **Octree geometry**: uses existing traversal, free
- **Discrete mesh objects**: BVH over convex hulls (from Phase 2)
- **Materials**: full PBR material structs (baseColor, metallic, roughness,
  emission, IOR) stored in a UBO — no 256-slot palette constraint
- **Integrator**: diffuse + specular + area lights, Russian roulette termination
- **Output**: RGBA pixel buffer in WASM linear memory, encoded to PNG via
  `stb_image_write.h` (~400 lines of C, no dependencies)

Rendering runs via `emscripten_async_call` so it does not block the editor UI.
Progress events fire to JavaScript as each frame completes, driving a progress bar.

### Python API

```python
# Option A: manual frame loop
renderer = phi.OfflineRenderer(width=1920, height=1080, samples=256, fps=24)

with renderer.job() as job:
    for frame in range(240):
        scene.time = frame / 24.0    # drives animation nodes at this time
        job.render_frame(frame)
    video = job.export(codec='h264')
    video.save("output.mp4")

# Option B: decorator — engine drives the loop
@phi.render_animation(frames=240, fps=24, width=1920, height=1080, samples=256)
def my_film(t):
    camera.position = camera_path.sample(t)
    crane_arm.rotation = phi.lerp(0, 45, t / 10.0)
```

Option B uses the same `t` value the animation nodes consume — a scene already
animated in the editor renders correctly with no extra setup.

### Server side

Three endpoints added to `server.py`:

```python
# POST /render/start              → {"job_id": "a3f9..."}
# POST /render/<id>/frame/<n>     ← PNG bytes, saved to jobs/<id>/frame_<n>.png
# POST /render/<id>/finish        → runs ffmpeg, returns video file
# GET  /render/<id>/output        → download completed video

def handle_finish(job_id, fps=24, codec='h264'):
    out = f"jobs/{job_id}/output.mp4"
    subprocess.run([
        "ffmpeg", "-y",
        "-r", str(fps),
        "-i", f"jobs/{job_id}/frame_%04d.png",
        "-c:v", codec,
        "-pix_fmt", "yuv420p",    # broad player compatibility
        out
    ], check=True)
    return open(out, "rb").read()
```

If ffmpeg is not installed, the server zips the PNG frames and returns that
instead. The client detects which it received by content-type.

### Frame transport

HTTP POST per frame rather than WebSocket streaming. Each frame is independent —
a failed POST can be retried without resending the whole render. At 1080p a PNG
frame is typically 300 KB–1.5 MB. A 10-second 24 fps render is roughly
240 × ~800 KB ≈ 190 MB of PNG before encoding; ffmpeg compresses that to
50–100 MB H.264.

### Why here in the sequence

This phase depends only on Phase 1 (mesh objects to render) and Phase 2 (physics
state to capture). It does not require MicroPython or the animation editor,
making it an early deliverable that gives users high-quality output from even
basic scenes. The Option B decorator integrates naturally with animation nodes
once those are built in Phase 6.

**Effort:** 3–4 weeks

---

## Phase 4 — Animation Editor

### What it is

A timeline-based editor for authoring animation clips that drive mesh object
transforms and skeletal rigs, built directly on the glTF pipeline (`cgltf`
parsing, `.glb`/`.gltf` load/save) already established in Phase 1 — there is
no separate format decision to make here. `.glb` binary containers are used
for runtime loading, `.gltf` + `.bin` for editor round-trips with external DCC
tools. `.panim` is a thin binary repack of glTF animation data optimised for
fast runtime loading; the editor works in glTF natively and exports `.panim`
for the engine. Assets authored externally in Blender, Maya, Houdini, or
Cinema 4D import directly with no conversion step, since `cgltf` covers the
full glTF 2.0 spec including skins, morph targets, and all animation channel
types.

### Armatures and skinning

Each `.glb` may contain a skeleton — a hierarchy of bones with rest-pose
transforms and inverse bind matrices. Meshes are bound to skeletons via skin
weights: each vertex stores up to 4 (bone index, weight) pairs sourced directly
from the glTF `JOINTS_0` and `WEIGHTS_0` vertex attributes.

The vertex format extends to carry skin data:

```c
typedef struct {
    float   pos[3];
    float   normal[3];
    float   mat_id;
    uint8_t bone_idx[4];   // up to 4 influencing bones
    float   bone_wgt[4];   // weights, sum to 1.0
} SkinnedVertex;
```

Skinning runs in the **vertex shader** (GPU skinning). Bone matrices are stored
in a **UBO** (Uniform Buffer Object) — WebGL 2 guarantees a minimum 16 KB UBO,
holding 256 `mat4`s. Full-body rigs including facial bones fit in a single draw
call without zone splitting.

### Armature → Physics (ragdolls)

Each bone in a skeleton can be assigned a Bullet rigid body (capsule or box) and
connected to its neighbours via `btConeTwistConstraint`. Two runtime modes,
switchable per-actor:

- **Animated**: bone transforms drive the rigid bodies kinematically
- **Ragdoll**: Bullet drives the bone transforms

Partial blending (e.g. physics weight 0.4 for secondary motion) is a per-bone
weighted lerp between the two transform sources.

### Architecture

The animation runtime has three layers:

1. **Clip**: a named sequence of keyframes. Each keyframe is `(time, value)` for a
   named channel (`translation`, `rotation`, `scale` per bone node, or a
   user-defined float channel). Sourced directly from `cgltf_animation`.
2. **Curve**: Bezier spline interpolation between keyframes (glTF's cubic spline
   or linear/step modes).
3. **Playback**: per-actor state (current clip, elapsed time, loop mode, blend
   weight, layer stack for additive animation).

### Editor operations

- Timeline scrubber with keyframe handles
- Bezier curve editor per channel — same C-owns-data/Python-authors-
  whole-cloth split as the node graph, see Phase 6's "Graph ownership"
  section (curve editor and node graph are the same architectural pattern
  applied to two different data shapes)
- Multiple clip management per asset (maps to glTF animation array)
- Preview playback with skinned mesh in the viewport
- Event markers on the timeline (trigger Python callbacks at a named frame)
- glTF import: drag in a `.glb`, all meshes + skeleton + clips available immediately

### Python API

```python
# import mesh + skeleton + all named animation clips
actor = phi.import_gltf("soldier.glb")

# playback
actor.play("walk_cycle", loop=True)
actor.blend_to("run_cycle", over=0.2)      # cross-fade in 0.2s
actor.play("wave", layer=1)                # additive layer

# bone access
actor.armature.bone("upper_arm_L").rotation = phi.quat_from_euler(0, 45, 0)
actor.ik_target("hand_R", target=door_handle.position)

# events embedded in the clip
actor.on_anim_event("footstep", lambda: phi.play_sound("step.wav"))

# physics handoff
actor.set_mode("ragdoll")
actor.set_mode("animated")
actor.set_mode("blend", phys=0.4, anim=0.6)

# plain .panim clip (runtime-optimised repack of glTF animation)
clip = phi.anim_clip("door_open.panim")
door.play(clip, loop=False)
```

The ragdoll switch integrates with the async NPC system:

```python
async def soldier(npc):
    await npc.walk_to(post)
    if await phi.event("shot", target=npc):
        npc.actor.set_mode("ragdoll")
        await phi.seconds(3.0)
        npc.actor.set_mode("animated")
        await npc.actor.play("get_up")
```

**Effort:** 6–9 weeks *(reduced from the original 8–12: glTF parsing and
load/save are already in place from Phase 1, so this phase is timeline UI,
curve editing, and skinning/ragdoll — still the largest schedule risk in the
project, but a smaller one than before)*

---

## Phase 5 — MicroPython Integration

### Embedding strategy

MicroPython's C source is compiled directly into `engine.wasm` alongside the
engine. It is not a separate runtime — the Python interpreter is a subsystem of
the engine binary. Scripts load as plain text (from `.py` files, from the
in-editor code pane, or from WebRTC data channel delivery from the host peer).

MicroPython adds approximately 200–400 KB compressed to the WASM binary.
`uasyncio` is included for async NPC behaviour (see Phase 7).

### CRITICAL WARNING

**ALL PYTHON API PATTERNS MUST BE VALIDATED AGAINST MICROPYTHON SPECIFICALLY
BEFORE THE API IS FROZEN. MICROPYTHON SUPPORTS DECORATORS AND BASIC INHERITANCE
BUT DOES NOT FULLY SUPPORT CPYTHON-STYLE METACLASS MACHINERY (e.g.
`__init_subclass__`, `__class_getitem__`, COMPLEX `__new__` OVERRIDES). ANY
PATTERN USED IN THE API — `@phi.node`, `@phi.render_pass`, `class Foo(phi.Pass)`,
PROPERTY DESCRIPTORS — MUST BE PROTOTYPED AND CONFIRMED WORKING IN MICROPYTHON
BEFORE A SINGLE LINE OF BINDING CODE IS WRITTEN.**

This applies directly to Phase 1's `@phi.panel` class-based panels and Phase 0's
`@phi.render_pass` class-based passes — both use inheritance from a `phi.*` base
class and must be prototyped in MicroPython specifically before Phase 1 UI work
is considered final, not just designed against CPython semantics.

### C API surface (initial)

```python
# entities
phi.mesh_object(path)            → MeshObject
phi.apply_impulse(obj, vec, at)
phi.raycast(origin, dir, dist)   → RayHit | None

# geometry primitives
phi.mesh_subdivide(mesh, level)  → Mesh
phi.mesh_extrude(mesh, faces, dist) → Mesh
phi.mesh_merge(a, b)             → Mesh
phi.noise3(vec3)                 → float

# animation
phi.anim_clip(path)              → Clip
phi.play(entity, clip, loop)

# game state
phi.spawn(type, position)        → Entity
phi.destroy(entity)
phi.players()                    → [Player]
phi.time()                       → float

# events
phi.on(event_name, callback)
phi.emit(event_name, *args)
```

Its design is the highest-leverage design decision in the project — changing it
breaks all user scripts.

### C modules (side modules)

For performance-critical systems, users write C against `phi.h` and compile via
the cloud compilation endpoint. The returned `.wasm` side module is loaded at
startup via Emscripten's dynamic linking. Engine internals remain opaque; users
see only the header.

**Effort:** 6–8 weeks

---

## Phase 6 — Geometry and Animation Nodes

### The core idea

Node graphs and Python scripts are two representations of the same program.
A node graph evaluates by topological sort; each node is a Python function.
The graph compiles down to readable Python at any time ("eject to code"), and any
Python function becomes a node type via a decorator. There is no separate node
graph language to learn.

### Node type definition

```python
@phi.node(
    inputs=[('mesh', Mesh), ('scale', float, 1.0)],
    outputs=[('mesh', Mesh)]
)
def noise_displace(mesh, scale):
    for v in mesh.vertices:
        v.position += phi.noise3(v.position) * scale
    return mesh
```

The decorator reads type annotations, creates socket definitions, and registers
the node type in the editor palette. Built-in node types wrap C primitives for
speed. Custom user nodes are pure Python. Same interface, different backing.

### The Script Node

A special node type with an inline Python body and user-defined sockets, written
directly in the node panel. Sockets are parsed from `inputs:`/`outputs:` comment
lines at the top:

```python
# inputs: mesh: Mesh, t: float
# outputs: mesh: Mesh

angle = math.sin(t * 2.0) * 0.5
return phi.mesh_rotate(mesh, axis=(0, 1, 0), angle=angle)
```

### Graph ownership: C owns topology, Python drives and evaluates it

The graph's topology, socket connections, and editor-only state (node
positions, selection) live in **C**, not in a Python object — the node
graph *canvas* is a native C UI widget (see Phase 1's Native UI System),
and interactive editing (drag a node, drag-connect a wire, box-select)
mutates that C-owned structure directly, at any graph size, without ever
crossing into Python. This is a deliberate departure from the simpler
"graph as a plain Python object" sketch this section used to have —
necessary once the editor itself is a C widget rather than drawn by Python
(see Phase 1's design-reference note on why), otherwise every drag/connect
interaction would round-trip through Python regardless of graph size.

Two things stay true despite that move:

- **Dispatch cost is still paid once per node, not per vertex.** The
  evaluator walks the C-owned topological order and calls each node's
  Python `fn` through the MicroPython C API — same cost model this section
  always had, just reading from a C structure instead of a Python one.
- **Python (or an LLM-generated script) can still author a whole graph in
  one call, not just interactively.** The binding layer exposes full CRUD
  over the C-owned graph — `graph.add_node(type_name, **params)`,
  `graph.connect(src, src_socket, dst, dst_socket)`, `graph.set_position(...)`
  — so a script can build (or rewrite) an entire graph programmatically in
  one execution, exactly the UX a Claude-assisted authoring session needs:
  describe what you want, get back a fully-wired graph, without anyone
  having dragged a single node by hand. `to_python()`/eject-to-code and
  this whole-graph `build()` path are inverses of each other — script → graph
  and graph → script both need to be lossless through the same binding
  surface, which is a real constraint on that surface's design, not an
  afterthought:

```python
graph = phi.Graph(kind="geometry")
n1 = graph.add_node("input_mesh", asset="rock.glb")
n2 = graph.add_node("noise_displace", scale=0.3)
graph.connect(n1, "mesh", n2, "mesh")
graph.set_position(n2, x=300, y=120)   # editor layout, not required for evaluation
result = graph.evaluate(context)
```

Curves (Phase 4's animation curve editor) follow the identical split for
the identical reason: C owns keyframe data and the curve widget's
rendering/interaction, Python gets full CRUD (`curve.add_keyframe(time,
value, interp="bezier")`) so a script or an LLM can author a complete set
of curves whole-cloth, not just nudge existing keyframes one drag at a
time.

### Two evaluation modes

**Geometry nodes** — reactive, evaluated on demand. The graph has no implicit time
input. It re-evaluates only when an upstream parameter is dirtied (user scrubs a
slider, a script sets a value). Result is a `Mesh` cached on the GPU. Used for
procedural asset authoring — subdivision, noise displacement, Voronoi fracture
pattern generation, procedural buildings.

**Animation nodes** — per-frame, `time` is an implicit input. The graph takes
`time` and optionally game state (entity position, velocity, health) and produces
transforms or mesh deformations. Evaluated every frame before rendering. Used for
driven animation — oscillating platforms, health-bar scale, door angle driven by
a physics joint.

Both are `Graph` objects with the same evaluator; only their trigger policy and
root socket type differ.

The same `@phi.node` decorator also covers **screen-space effects** — nodes
whose sockets carry `Texture` types instead of `Mesh` or scalars, composing
post-process passes out of the deferred renderer's G-buffer. Same system, same
evaluator, same eject-to-code — no third node kind to design.

### What this gives over Blender / Unreal

|                    | Blender Geo Nodes | Unreal Blueprint | Phi           |
|--------------------|-------------------|------------------|---------------|
| Custom node types  | C++ only          | C++ only         | Python (any user) |
| Eject to code      | No                | No               | Yes           |
| Code → visual      | No                | No               | Yes (decorator) |
| Scripting language | Separate          | Separate         | Same Python   |
| Runtime evaluation | Offline only      | Runtime          | Both modes    |

**Effort:** 4–6 weeks

---

## Phase 7 — Async NPC Behaviour

### The problem with state machines

Traditional NPC AI is written as explicit state machines: an enum of states, a
`think()` function that switches on the current state and transitions between
them. The behaviour of a guard who patrols, detects a player, chases them, loses
them, and returns to patrol is spread across a dozen cases and transition
conditions. The sequential story of what the NPC *does* is invisible in the code.

### Coroutines as behaviour scripts

MicroPython's `uasyncio` makes NPC behaviour a sequential script. Each NPC runs a
coroutine. `await` suspends the coroutine — yielding control back to the game loop
— until the awaited condition is satisfied. The behaviour reads exactly like the
design document:

```python
async def guard(npc):
    while True:
        for waypoint in npc.patrol_route:
            await npc.walk_to(waypoint)
            await phi.seconds(1.5)             # pause at each post

            player = npc.sense(radius=60)
            if player:
                await npc.face(player)
                await npc.say("halt")
                await phi.seconds(0.5)

                if npc.dist(player) < 80:
                    async with npc.state("chase"):
                        await npc.chase(player, until=lambda: npc.dist(player) < 8)
                        await npc.attack(player, times=3)
                        await phi.seconds(2.0)  # cooldown
```

No state enum. No transition table. The control flow is the state machine.

### Built-in awaitables

```python
await phi.seconds(t)                  # wall-clock delay
await phi.frames(n)                   # fixed number of ticks
await npc.walk_to(position)           # move until arrived
await npc.face(target)                # rotate until aligned
await npc.sense(radius, type=None)    # block until entity sensed → returns entity
await npc.animation('attack')         # play clip to completion → returns
await phi.any(coro1, coro2)           # first to complete wins, others cancelled
await phi.all(coro1, coro2)           # wait for all
await phi.event('explosion_nearby')   # block on named event
```

`phi.any` enables reactive interruption without restructuring the coroutine:

```python
async def patrolling_guard(npc):
    while True:
        # patrol until shot at — whichever comes first
        result = await phi.any(
            npc.walk_to(next_waypoint),
            phi.event('damage', target=npc)
        )
        if result.event == 'damage':
            await npc.take_cover()
```

### Scheduling

The game loop ticks `uasyncio` once per frame. Each NPC coroutine is a task in
the event loop. The scheduler is a few lines in the main Python entry point:

```python
import uasyncio as asyncio

async def main():
    for npc in phi.npcs():
        asyncio.create_task(npc.behaviour(npc))
    while True:
        await asyncio.sleep(0)   # yield to game loop

asyncio.run(main())
```

### Composability

Because behaviours are coroutines, they compose with standard Python async
patterns. A complex boss can be assembled from primitive behaviour coroutines:

```python
async def boss(npc):
    await phase_one(npc)           # fight until half health
    await npc.animation('enrage')
    await phase_two(npc)           # fight until near death
    await npc.flee_to(spawn)
    await npc.heal(duration=5.0)
    await phase_one(npc)           # repeat
```

### Connection to nodes

Behaviour coroutines and node graphs share the same Python environment. An NPC
coroutine can read from a geometry node graph (sample a procedural patrol path),
trigger animation node transitions (set a blend weight), or emit events that other
coroutines await. The same `@phi.node` decorator that defines a geometry node type
can define a behaviour node — a node whose output is an awaitable:

```python
@phi.node(
    inputs=[('npc', NPC), ('radius', float, 50.0)],
    outputs=[('target', Entity)]
)
async def find_nearest_enemy(npc, radius):
    return await npc.sense(radius=radius, type='enemy')
```

Node graphs, coroutines, and plain Python functions are all the same thing at
different zoom levels.

**Effort:** 2–3 weeks

---

## Phase 8 — WebRTC P2P (One Client as Server)

### Architecture

The current authoritative server (game tick, physics, state broadcast) moves into
the host client. The Python server shrinks to a signaling process: room
management, ICE candidate forwarding, static file serving.

```
Host client
  └── runs authoritative simulation (physics, fracturing, NPC coroutines)
  └── broadcasts entity states over WebRTC data channels

Peer clients
  └── send input to host
  └── receive entity states, interpolate
  └── run local prediction (existing client-side prediction code)

Signaling server (server.py, ~150 lines)
  └── HTTP: serve static files
  └── WebSocket: room create/join, SDP offer/answer relay, ICE candidate relay
  └── no game state, no tick loop
```

The existing WebSocket hand-rolled RFC 6455 implementation in `server.py` is
reused for the signaling channel. The game protocol moves to WebRTC data channels
(ordered + reliable for state sync, unordered + unreliable for input).

### STUN / TURN

STUN: use `stun.l.google.com:19302` — free, no setup, handles ~85% of NAT
configurations. TURN (for symmetric NAT, ~15% of users): optional for initial
versions, necessary for production. A minimal TURN server can be self-hosted with
`coturn`.

### Host migration

When the host disconnects, the signaling server designates a new host from the
remaining peers. The new host reconstructs authoritative state from its last
received snapshot. This is a known-hard problem; a simple version (new host, brief
pause, resync) is sufficient for early versions.

### Why this matters for the scripting model

NPC coroutines, physics simulation, and fracture events all run on the host.
Peers receive the results. Python scripts authored by the game developer run on
the host client — they do not need to run on every peer. This simplifies the
scripting model significantly: scripts are authoritative, determinism is not
required across peers.

**Effort:** 4–5 weeks

---

## Dependency Graph

```
Phase 0: Platform abstraction + deferred renderer
    │
    └── Phase 1: Mesh Editor
            │
            └── Phase 2: Physics (Bullet)
                    │
                    ├── Phase 3: Offline Raytracer & Video Export
                    │
                    └── Phase 4: Animation Editor
                            │
                            └── Phase 6: Geometry & Animation Nodes ──┐
                                                                        │
                    Phase 5: MicroPython ─────────────────────────────┤
                                │                                      │
                                └── Phase 7: Async NPC Behaviour ─────┘

Phase 8: WebRTC P2P  (depends only on Phase 0, runs in parallel)
```

Phase 2 is the main branch point. Phases 3, 4, and 5 can all proceed in parallel
after Phase 2 completes. Phase 6 requires both Phase 4 and Phase 5. Phase 7
requires Phase 5. Phase 8 is transport-layer work independent of all others and
can run in parallel throughout.

---

## Effort Summary

| Phase | Description | Effort |
|---|---|---|
| 0 | Platform abstraction + deferred renderer + G-buffer | 5–7 weeks |
| 1 | Mesh editor + native UI system + glTF pipeline (load/save, half-edge editing structure, extensions) | 7–10 weeks |
| 2 | Bullet physics + fracturing | 4–6 weeks |
| 3 | Offline raytracer + video export | 3–4 weeks |
| 4 | Animation runtime + editor (glTF pipeline already built in Phase 1) | 6–9 weeks |
| 5 | MicroPython integration + C API surface | 6–8 weeks |
| 6 | Geometry and animation nodes | 4–6 weeks |
| 7 | Async NPC coroutine system | 2–3 weeks |
| 8 | WebRTC P2P + signaling server | 4–5 weeks |

Single-developer estimates. Moving glTF I/O and the half-edge editing
structure into Phase 1 front-loads work that was previously implicit
("Phase 4 adopts glTF") without being budgeted anywhere — the net effect
across Phase 1 and Phase 4 combined is roughly a wash (15–18 weeks before vs.
13–19 weeks now), but the risk profile improves: the format decision is
locked in and battle-tested by the time skeletal animation depends on it,
instead of being re-litigated under schedule pressure in Phase 4. Schedule
compresses significantly with >1 developer after Phase 2. Biggest risks:
Phase 4 animation runtime complexity (still real, just smaller), and the
Phase 5 C API surface design (changing it breaks all user scripts) — including
validating every `phi.*` class-based decorator pattern (Phase 0's
`@phi.render_pass`, Phase 1's `@phi.panel`) against MicroPython specifically
before freezing the API.
