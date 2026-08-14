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

Claude runs *inside* the editor, not beside it as a chat-window bolt-on. An
Anthropic API key configured on the authoring server (never shipped to any
client, never embedded in `engine.wasm`) lets Claude act as a first-class
editor participant with the same reach a human co-author has — modeling and
editing meshes, laying out scenes, writing gameplay logic and NPC behaviour,
defining node types, scaffolding `@phi.panel` UI, organizing and tagging
assets, even proposing what a game *should be* when a user's (or a group of
users') brief is loose. This is deliberately broad rather than a fixed menu
of assist actions: the boundary of what Claude can help build is meant to be
the boundary of the editor's own tools, not a hand-picked subset of them —
"free to define basically anything about a game a user or set of users want
to make" is the working framing, not a specific scoped feature list. Earlier
language here described a narrower "in-editor assistant" (defining
`@phi.panel` UI from a description, scaffolding a `@phi.node` function,
writing a first NPC-behaviour pass, suggesting script fixes) — those are all
still true, they're examples of this broader role now rather than its edge.

None of that needs a privileged path. Claude proposes edits through the
exact same client-authored/server-persisted loop a human editing client
uses — it applies a delta, the server validates and persists it the same way
it would for a mouse drag, and every connected client sees the result. It
never bypasses the authoritative server, and no client (including whatever
session is driving Claude's edits) ever holds the API key itself — the key
lives only on the authoring server, gating whether Claude gets invoked at
all, not how its edits reach the world.

**2026-08-09**: this reframing is also why the networking layer inherited
from Qek (the original rocket-arena game this codebase was extracted from,
see Phase 1's status section) is being *repurposed* rather than deleted
during the ongoing Qek-cleanup pass — see "Client/server model" under Phase
1 for exactly what that means for `net.c`/the wire protocol/`server.py`.

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

**Major bug found and fixed (2026-08-09), affecting every draw call in
this pipeline**: `gbuffer_begin_geometry_pass()` never explicitly enabled
`GL_DEPTH_TEST`, silently relying on whatever state a *previous* pass
happened to leave it in — and `gbuffer_resolve()` ends its own frame by
calling `glDisable(GL_DEPTH_TEST)` for its tonemap/FXAA fullscreen-quad
passes (which don't need it) without ever re-enabling it. Per the GL
spec, when `GL_DEPTH_TEST` is disabled the depth buffer is never updated
*at all*, regardless of `glDepthMask` — so every frame's geometry pass
was silently failing to write real depth values, while still writing
color/normal/object-id normally (those aren't gated by depth test the
same way). The lighting pass's background check
(`if (depth >= 0.999999) { out_hdr = sky_color; return; }`) then fired
for genuinely-drawn geometry too, since depth was permanently stuck at
its cleared far-plane value — every object in the scene composited as
flat sky color despite correct object-id and albedo, for every target,
apparently for as long as `gbuffer_resolve()`'s depth-disable has existed
in the pipeline. Fixed with one line: `gbuffer_begin_geometry_pass()` now
explicitly `glEnable(GL_DEPTH_TEST)` + `glDepthFunc(GL_LESS)` itself,
rather than trusting implicit persistence from unrelated code — this pass
now owns the GL state it depends on.

This is why it went unnoticed through every verification above: shadow
map, FXAA, bloom, transparency, and TAA were all checked by reading back
*specific G-buffer/HDR buffer values* (object-id, HDR RGB at a known
pixel, velocity vectors) and comparing them against hand-computed
expected numbers — genuine, rigorous checks of their own math, but none
of them happened to also ask "does the final composited pixel show this
object's own lit color, or just sky?" The bug was found only once actual
*pixel-level visual verification* entered the toolkit this session
(`python-xlib`'s XTest extension driving real synthetic input + a real
running window, screenshotted with ImageMagick's `import -window`) —
while debugging why a newly-added transform gizmo (Phase 1, see below)
wasn't visible despite its geometry pass rasterizing correctly (confirmed
via a direct object-id G-buffer readback, which is exactly the kind of
check that couldn't have caught this). Once found, the same symptom was
confirmed for the existing MeshObject test object *and* the world mesh
itself — not gizmo-specific at all. Native + win32 + wasm all
build-verified after the fix; native re-confirmed live (not just
build-verified) via the same screenshot technique: `frame 30` (a
diagnostic camera pointed at the test MeshObject) now reads its real
purple material color `(134,63,153)` at the previously sky-blue center
pixel, and `frame 120` (the normal gameplay camera) reads a real bot's
lit color `(61,15,15)` instead of flat sky-blue `(76,128,204)` — both via
the same `[main] frame N Scene panel center pixel RGB` log line that had
been (unknowingly) reporting sky-blue at that exact spot all along.

**Effort:** 5–7 weeks *(platform abstraction across all three targets
(wasm/Linux-native/Windows-native) — all with real input and real
networking now, not just Linux — the wasm WebGL2 upgrade, CI check, and
the deferred renderer/G-buffer/shadow-map/FXAA/bloom/transparency/TAA
pipeline (all confirmed building on wasm, Linux native, and Windows
native alike, FXAA+bloom additionally confirmed GL-errorless in a real
browser): done — see the scope-split note above for what "done" means
and how each platform was actually verified, not just built, and the
depth-test bug note above for a real gap that verification missed and
how it was actually found. Remaining: macOS (explicitly deferred, no
access), and the `@phi.render_pass` insertion-point system (deliberately
deferred to Phase 5/MicroPython).)*

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

**Not started** (as of this section's original writing; see the Native UI
System section below and the dated status note after it for what's landed
since): the native UI system (DNA/RNA property system, SDF font widget
rendering, panel layout), transform gizmos, ray-vs-mesh picking, extrude/
inset/loop-cut editing operations, PBR material assignment, Voronoi
fracture tooling, and Bullet physics integration for mesh objects (the
last of these is genuinely Phase 2's own scope — see that section — and
isn't being pursued under Phase 1 despite being listed here originally).

### Architecture

A new `MeshObject` entity type is the scene's own content — `Player`/`Rocket`
were Qek's gameplay types this sat alongside originally; both are gone now
(see "Client/server model" above), so `MeshObject` no longer shares the game
state with anything else. WebGL 2 is the rendering target from day one (`USE_WEBGL2=1`,
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

### Asset tracking and the Asset Browser panel

**Status: not started — design intention, recorded 2026-08-09.** A flat
directory of `.glb`/`.gltf` files (the mesh/skeleton/animation-clip assets
already described under Distribution Model) doesn't stay findable once a
project accumulates more than a handful of them — no search by name, no way
to group related assets, nothing browsable in the editor. The plan:

- **A directory of glTF files remains the actual asset store** — this
  doesn't change; the Core Pattern already has the server persisting each
  edited asset to its own `.glb` on disk. What's new is an **index**, not a
  new source of truth: a SQLite database on the authoring server that
  tracks each asset's file path, display name, and a simple many-to-many
  tag table (`asset(id, path, name, created_at, ...)` /
  `tag(asset_id, tag)` — the obvious normalized shape for "search by name
  or by tag intersection", not yet finalized). The index is rebuildable by
  re-scanning the asset directory if it's ever lost or gets out of sync —
  a deliberate constraint, so the SQLite file is a cache/index the
  filesystem can always regenerate, never the only copy of anything that
  matters.
- **A new Asset Browser panel** — another entry in the Native UI System's
  panel type-switcher (`ui.h`'s `PanelType` enum: currently
  `PANEL_SCENE`/`OUTLINER`/`PROPERTIES`/`CONSOLE`/`CHAT`/`NODE_EDITOR`/
  `CURVE_EDITOR`), listing/searching indexed assets by name or tag and
  letting a user pick one into the current scene — the same role
  Blender's/UE5's asset browser plays, scoped to Phi's own glTF-centric
  asset model rather than a generic multi-format one.
- **Scope for now**: glTF assets specifically (meshes/skeletons/animation
  clips, per Project Identity's file-extension list), since that's the
  asset type that exists today. The index's schema is asset-type-generic
  by construction (path + name + tags doesn't care what the file *is*),
  so `.panim` (Phase 4) and `.pscene` (scene files, referenced under
  Distribution Model) are expected to land in the same index later rather
  than each phase inventing its own separate tracking mechanism — but
  that extension isn't designed yet, flagged as a real open question
  rather than assumed to just work.
- Where exactly this lives relative to the authoring/shipping split (see
  "Authoring vs. shipping" above) — whether a shipped/published game's
  runtime ever needs live asset search, or whether publishing always
  bakes a fixed asset set the way it already bakes everything else — is
  also not decided yet; recorded as an open question rather than resolved
  by assumption.

#### Wire protocol: CRUD over a hybrid HTTP + WS split (design landed 2026-08-09)

Executing the plan above needs an actual protocol between an editor client
and the authoring server. The two verbs that move an arbitrary-size binary
blob (Create, Read) go over the existing hand-rolled HTTP layer
(`server.py`'s `serve_file`/`send_http` already do exactly this for static
files); the three verbs that are small structured messages where a
connected editor benefits from a low-latency push (List, Update, Delete)
go over the existing WS binary game-transport (`net.h`/`net.c`) instead of
inventing a second HTTP+JSON API — this also lets the server broadcast
"something changed" to every connected client, the multi-user hook the
"Client/server model" section's item (B) repurposing was already aimed at.
No JSON parser gets added to the C client anywhere in this design: list
replies reuse the exact length-prefixed binary encoding `PKT_CONSOLE_MSG`
already established, not a new parsing dependency.

**CRUD operates on `.glb` blobs only**, never loose `.gltf`+`.bin` pairs —
Distribution Model already says assets persist as `.glb` on the server, and
a single self-contained binary is what makes "upload = one HTTP POST body"
clean; `cgltf_parse_file` already auto-detects and parses GLB transparently
(`cgltf_file_type_glb`), so the client-side loader (`halfedge_load_gltf`)
needs zero changes to accept them. Loose `.gltf`+`.bin` pairs (like the
existing hand-authored `assets/cube.gltf`) remain loadable by direct path
the way they always were — they just aren't what CRUD *creates*.

**HTTP** (extends `server.py`'s existing GET-only hand-rolled parser to
also read a `Content-Length` body and dispatch POST/DELETE):
- `POST /assets?name=<name>&tags=<csv>` — body is the raw `.glb` bytes.
  Rejects if the body doesn't start with the GLB magic (`glTF` + version 2
  header) — CRUD-created assets are GLB, full stop, not sniffed/guessed.
  Writes to `assets/library/<slug>_<id>.glb`, inserts the SQLite row,
  broadcasts `PKT_ASSET_CHANGED` to every connected WS client, responds
  `200` with the new decimal asset id as a plain-text body (no JSON, so no
  client-side parsing is needed for the one thing native/wasm might want
  back from a create call).
- `DELETE /assets/<id>` — deletes the DB row and the backing file,
  broadcasts `PKT_ASSET_CHANGED`, responds `204`.
- `GET /assets/<path>` — unchanged, already existed for static files; also
  how a client fetches an asset's actual bytes once it knows the path from
  a list reply.

**WS** (new packets in `net.h`, continuing past the existing
`PKT_HELLO`/`PKT_CONSOLE_MSG`):
- `PKT_ASSET_LIST_REQUEST = 0x10` C→S: `[qlen:u8 query:bytes]` (`qlen=0` =
  no filter, list everything).
- `PKT_ASSET_LIST_REPLY = 0x11` S→C: `[count:u16]` then `count` ×
  `[id:u32 name_len:u8 name:bytes path_len:u8 path:bytes tags_len:u8
  tags:bytes(csv)]`.
- `PKT_ASSET_UPDATE = 0x12` C→S: `[id:u32 name_len:u8 name:bytes
  tags_len:u8 tags:bytes(csv)]` — renames/retags an existing row (does not
  touch the backing file). Server applies it and broadcasts
  `PKT_ASSET_CHANGED`, including back to the sender, so every client's
  view updates from the same authoritative source rather than the sender
  optimistically patching its own local cache.
- `PKT_ASSET_DELETE = 0x13` C→S: `[id:u32]` — same delete as the HTTP verb,
  exposed over WS too since it's small and structured; both paths call the
  same `assets_db.delete_asset`.
- `PKT_ASSET_CHANGED = 0x14` S→C: no payload — "the list changed, re-request
  if you care." Broadcast after every successful create/update/delete
  regardless of which transport (HTTP or WS) triggered it.

**SQLite schema** (`server/assets_db.py`, finalizing the sketch above):
```sql
CREATE TABLE asset (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT NOT NULL,        -- relative to www/, e.g. "assets/library/pyramid_3.glb"
    name TEXT NOT NULL,
    created_at REAL NOT NULL
);
CREATE TABLE tag (
    asset_id INTEGER NOT NULL REFERENCES asset(id) ON DELETE CASCADE,
    tag TEXT NOT NULL
);
```
Index file lives at `server/assets.db` (outside `www/`, so it's never
accidentally served as a static file). Rebuildable by re-scanning
`assets/library/` if ever lost, matching the "index, not source of truth"
constraint above — not implemented this pass (no rescan tool yet), but the
schema doesn't preclude it.

**Client-side data model** (`client/asset_browser.c`/`.h`, new module):
owns the cached `AssetSummary` list, the binary list-reply parser, and
simple one-shot request flags (`refresh_requested`, `load_requested_id`,
`delete_requested_id`) that `main.c` polls once per frame and turns into
real `net_send_asset_*` calls — the same shape `ui_poll_context_menu_action`
already established for "UI raises intent, main.c executes it against the
engine/network," rather than having `ui.c` reach into `net.c` directly
(today it doesn't include `net.h` at all).

**What's explicitly not built this pass**: no in-editor "export current
MeshObject as a new asset" flow yet (there's nothing to Create from inside
the editor, so Create is exercised directly against the HTTP endpoint, not
through the Asset Browser panel's UI); no inline rename/retag UI in the
panel (the `PKT_ASSET_UPDATE` protocol path exists and is server-tested,
just not wired to a button yet); no real HTTP GET client for a native/wasm
process that isn't colocated with the server's filesystem (native still
loads a selected asset via direct `fopen` against the shared `assets/`
directory, exactly like the existing test cube already does — correct for
this dev setup, not correct in general, flagged rather than assumed away).

**Implemented and verified, 2026-08-09.** Server: `server/assets_db.py`
(the SQLite index, exactly the schema above), `server/server.py` extended
with real `Content-Length`-body reading, `POST`/`DELETE` dispatch, and the
five new WS packet handlers. Client: `client/net.h`/`.c` gained the
packet builders/parser calls; `client/asset_browser.c`/`.h` (new module)
owns the cached list + request flags; `client/ui.c` gained a real
`PANEL_ASSET_BROWSER` (row list, Refresh/Load/Delete, all hit-tested
against the exact geometry helpers the draw call uses, same
`type_icon_rect()`-style shared-geometry pattern the rest of `ui.c`
already established); `main.c`'s old `spawn_test_mesh_object` became
`load_mesh_object_from_path(path, scale)`, freeing whatever was
previously in the one test-object slot before loading the new one (the
old spawn-once code never had to handle that case; Load now can be
clicked while something's already loaded, so it does).

Three hand-built test assets (`tools/gen_test_assets.py`, a pyramid, an
oblate spheroid, and a non-cube cuboid) exercise both glTF forms the
protocol touches: loose `.gltf`+`.bin` (pyramid/spheroid, matching the
existing hand-authored `assets/cube.gltf`) and self-contained `.glb`
(all three, since CRUD create only accepts `.glb`). Each shape's
triangle winding is corrected programmatically (checked outward against
the centroid, not hand-derived) and cross-verified two ways: a per-face
outward-normal check and a divergence-theorem volume calculation
matching `fracture_test_main.c`'s own volume formula exactly.

**A real bug this testing pass caught**: the first version of the CRUD
create endpoint wrote uploaded files under `www/assets/library/` and
stored that as the asset's `path`. That's wrong — every client
(`assets/cube.gltf`'s own established convention, the Makefile's
`--embed-file assets@assets` for wasm) reads assets from the **project-
root** `assets/` directory, a sibling of `www/`, not a directory inside
it (`www/assets/` was, before this, an empty, functionally dead path —
nothing client-side ever read from it). A path stored as
`assets/library/x.glb` therefore resolved to two *different* files
depending on whether you were `server.py` (which would've resolved it
under `www/`) or a client doing a direct `fopen` (project root). Caught
by the C client integration test below — not by the earlier Python-only
WS protocol test, which never actually opened the file the server told
it about, only checked that the *bytes describing the path* round-
tripped correctly. Fixed by moving `ASSET_LIBRARY_DIR` to the real
`assets/library/` and teaching `serve_file` to resolve `/assets/*`
requests against that same root instead of `www/`.

Verification, in increasing order of how much of the real stack each one
actually exercises:
- `server/test_asset_protocol.py` — an independent hand-rolled Python WS
  client (deliberately not sharing code with `ws_client_native.c`) driving
  LIST/UPDATE/DELETE/CHANGED against a live server. Confirms the server's
  framing and packet semantics are spec-correct, including that
  `PKT_ASSET_CHANGED` reaches every connected client, not just whichever
  one triggered the mutation. Caught its own bug the first time it ran:
  the test client's handshake reader could silently drop the start of the
  server's first WS frame if it arrived bundled with the HTTP 101
  response on loopback — same "leftover bytes past a parsed boundary"
  hazard `server.py`'s own HTTP layer has to handle for POST bodies,
  just independently rediscovered in the test client's own code.
- `client/asset_protocol_test_main.c` (`make asset_protocol_test`,
  run manually against a live server, see the Makefile target's own
  comment) — the real C encoder/decoder in `net.c`/`asset_browser.c`,
  not a reimplementation, driven against a live server: connects, gets
  the initial list automatically (no explicit request — `net.c` fires it
  right after `PKT_HELLO`, the one moment guaranteed-connected across all
  three platforms), loads every listed asset's path through the real
  `halfedge_load_gltf`/cgltf pipeline, then round-trips a real
  C-encoded `PKT_ASSET_UPDATE` and `PKT_ASSET_DELETE` through the live
  server. This is what actually caught the path bug above.
- `make native`/`make wasm`/`make win32` all still link clean, zero new
  warnings. `mesh_edit_test`/`fracture_test`/`mp_console_test` still pass
  (unaffected code paths, confirmed not assumed). Live GUI verification
  (clicking Refresh/Load/Delete in an actual window) was not possible
  this pass for the same pre-existing sandbox `XOpenDisplay()` hang noted
  earlier in this file, not a regression from this work — the C
  integration test above is what substitutes for it, same tradeoff this
  session made for the A/C/D removal pass.

**Search bar, 2026-08-09.** The server side of search already existed —
`assets_db.list_assets(query)` was always a SQLite `LIKE` against both
`name` and `tag`, and `PKT_ASSET_LIST_REQUEST` already carried a query
string — this only needed a client-side text field and something to send
it. The interesting part turned out to be focus, not search: `console.c`
was deliberately built as "the only text field, always focused, no
toggle", so a second text field needed this codebase's first actual
focus concept. Solution: `AssetBrowserState.search_focused`, set by a
click inside the search bar rect and cleared by clicking anywhere else
(the same click-away-dismisses convention the type-switcher dropdown and
context menu already use — `find_area_by_type_r`, a small new tree-search
helper, generalizes the pattern `find_scene_rect_r` already used for
`PANEL_SCENE` specifically). `main.c`'s frame loop picks exactly one
consumer for this frame's `InputState` text-entry fields based on that
flag: `asset_browser_update_search` when focused, `pyconsole_update`
otherwise — the fallback is always correct, not just usual, because
`asset_browser_update_search` is a true no-op (doesn't touch `InputState`
at all) when unfocused. Also handled: switching a leaf's panel type away
from `PANEL_ASSET_BROWSER` while its search bar had focus clears that
focus too, since the box it belonged to just disappeared from the layout
— without this, the console would've been silently locked out with no
visible box left to click away from.

Submits on Enter or a Refresh click, not live-as-you-type (one network
round trip per keystroke isn't worth it, and nothing else in this
codebase's text entry live-submits either); Refresh always means "re-run
whatever's currently in the search bar," including when that's empty
(server-side: empty/`NULL` query means no filter, unchanged).

The panel's own "Asset Browser" title text was dropped to make room —
the type-switcher icon in the corner already identifies the panel type,
the same way every other panel's icon does, so the label was the one
piece of the header actually free to give up for the search bar to sit
to the left of Refresh, as asked for.

Verified by extending `client/asset_protocol_test_main.c` (the real C
`net_send_asset_list_request`, not a reimplementation) with checks that a
query matching a real asset returns a strict, correctly-filtered subset;
that a query matching nothing returns empty rather than a stale dump;
and that a `NULL` query restores the full list — all pass against a live
server. UI-side focus/blur/type-away behavior itself still needs a real
window to click through, same outstanding limitation as the rest of this
panel's live-GUI verification.

**Create + rename gaps closed, 2026-08-09.** The two things flagged as
"not built this pass" above now are.

*Create* needed two new pieces of infrastructure that didn't exist yet:
`halfedge_save_glb_buffer` (`halfedge_gltf.c`) is `halfedge_save_gltf`'s
twin, producing a self-contained GLB in a `malloc`'d memory buffer
instead of writing loose `.gltf`+`.bin` files — needed because main.c has
to build one at runtime from whatever `MeshObject` is currently selected,
not offline from a script. `client/http_client_native.c`/`.h` (new,
native-Linux-only — no win32/Winsock twin or wasm/`fetch()` equivalent
yet, flagged rather than silently assumed) is a minimal blocking POST
client reusing `ws_client_native.c`'s own `getaddrinfo`/`connect`
pattern, plus a percent-encoder (asset names/tags can contain spaces) and
a `ws://host:port/path` parser so the HTTP client points at the same
server the WS connection is already talking to. The Scene context menu's
new "Save as Asset" row (`CTX_ACTION_SAVE_AS_ASSET`) starts a pending
create in the Asset Browser's edit form (see below) rather than
uploading immediately — the actual flatten-encode-POST only happens once
that form is submitted with a real name.

*Rename* and *Create* ended up sharing one implementation, once it became
clear they're the same interaction shape (a name field, a tags field,
Save/Cancel) pointed at two different backends (`PKT_ASSET_UPDATE` for an
existing asset vs. an HTTP POST for a new one). `AssetBrowserState` gained
`editing_id` (a signed sentinel: `AB_EDITING_NONE` / `AB_EDITING_NEW` /
an existing asset's real id) plus one shared `edit_name`/`edit_tags`
buffer pair, and `AssetBrowserFocus` grew from a bool (just search) to a
real enum covering search/edit-name/edit-tags, since three text fields
can now compete with the Python console for keystrokes instead of one.
The panel gained a Rename button alongside Load/Delete, and an edit form
that appears above the list (pushing it down by its own height) whenever
`editing_id != AB_EDITING_NONE` — Blender-style click-away blurs
*keyboard focus* on the open form but never discards it (an in-progress
edit is a draft, not lost work just because you clicked elsewhere or
switched the panel to something else and back).

Verified: a new integration test (ad hoc, not committed as a permanent
harness — see the pattern below for what *is* kept) drove the exact
sequence `main.c`'s `create_requested` branch runs — flatten
`assets/cube.gltf`'s loaded `HalfEdgeMesh` to a GLB buffer, percent-encode
a name with a space and an ampersand in it, parse host/port out of a real
`ws://` URL, POST — against the live server, got back a real new asset
id, and confirmed the uploaded file loads correctly through the same
`halfedge_load_gltf` path Load already used. All three build targets
(native/wasm/win32) still link clean; `mesh_edit_test`/`fracture_test`/
`mp_console_test`/`asset_protocol_test` all still pass. Live GUI
click-through (actually seeing the edit form, clicking Rename, watching
the caret) is still not verified in this sandbox, same `XOpenDisplay()`
limitation as everything else in this session that needs a real window.

### Native UI System

Built in C, rendered entirely via WebGL 2. No third-party UI library — this is
the resolved decision over Dear ImGui (see Hard Architectural Decisions).

**Status (2026-08-09): first real editor shell landed, then three same-day
feedback rounds** — build-verified on native, win32 (real Intel Arc
hardware), and wasm throughout, `emcc` reachable via `source
~/src/c/emsdk/emsdk_env.sh` (not on `PATH` by default). Strategic context:
Phi is no longer aimed at being Qek's (the rocket-arena game's) engine
specifically — Qek is its own separate project — Phi is now aimed at
general UE5/Blender-territory editor work. This is the first UI that isn't
Qek's HUD/dev console.

**Mouse-capture bug (found and fixed the same day)**: native and win32 both
called their platform's pointer-lock-equivalent APIs *unconditionally* at
startup — `input_install_callbacks()` did `XGrabPointer(..., confine_to=
s_win, ...)` plus an invisible cursor and a per-motion-event recenter warp
on native, and `ShowCursor(FALSE)` + `ClipCursor(&clip)` on win32 — a
leftover from Qek's always-on FPS mouse-look, where the window just owns
input unconditionally once focused. For an editor this is actively
harmful two ways at once: (1) the confined, invisible, snap-to-center
cursor makes it physically impossible to click any of the rendered panel/
menu/outliner/context-menu chrome, and (2) `XGrabPointer`'s `confine_to`
and `ClipCursor`'s clip rect restrict the OS cursor to the window's client
area, which also makes it impossible to reach the window's own border/
title-bar resize handles — almost certainly the actual cause behind "not
responsive to window resizing" being reported in the same breath as the
mouse-stealing complaint, not a separate bug in the resize-handling code
itself. Fixed by not grabbing/hiding/confining/warping the cursor at
startup at all; `InputState.pointer_locked` now starts (and stays) `0` on
native/win32 until something opts back in, and `handle_motion` on both
platforms now returns immediately when not locked rather than computing a
delta and re-warping the cursor. No click-to-engage gesture (mirroring the
web build's canvas-click → pointer lock) exists on native/win32 yet, so
mouse-look for the FPS camera is off by default there now — reasonable
given the editor-first pivot, and building an opt-in re-engage gesture is
follow-up work, not done here. Verified live: resized the actual X11
window (`XResizeWindow` via a throwaway `python-xlib` script, screenshotted
with ImageMagick's `import -window`) from 1280×720 to 1600×900 while
running — Scene/Outliner/Properties/branding bar/menu row all correctly
re-laid-out at the new size, confirming the underlying per-frame
`phi_platform_get_window_size()` → `ui_layout()` flow (already
platform-agnostic, shared by all three targets, and already wired to each
platform's own resize event — `ConfigureNotify`/`WM_SIZE`/
`emscripten_set_resize_callback`) was already correct; win32 and wasm
share that exact code path but weren't independently execute-tested here
(no Windows runtime/Wine, no real browser in this environment).

**Stale-viewport bug (found and fixed the same day)**: after the palette/
menu-row round landed, the Outliner and Properties panels rendered visibly
too far left — squashed into roughly the Scene panel's own on-screen
rectangle instead of occupying their own column to its right, with a dead
black strip along the window's right edge. Root cause: `draw_panel_scene`'s
G-buffer FXAA blit sets `glViewport` to the Scene panel's own sub-rectangle
(`gbuffer_set_viewport_offset`), and nothing reset it back to the full
window before `draw_area_chrome` (Scene's own border/icon) or the *next*
panels in the tree drew their 2D UI rects — every `ui_rect`/`ui_text_draw`/
`ui_icon_draw` call computes its NDC position assuming a full-window
viewport via the `u_screen_size` uniform, so those draws got remapped by
the GPU into whatever smaller viewport the Scene panel had left behind. The
previous single `glViewport` reset in `ui_render()` ran only once, after
the *entire* panel tree had already been walked and drawn — too late for
everything except the branding bar/menu row that come after it. Fixed by
resetting the viewport to the full window unconditionally in `draw_leaf()`,
right after each panel's own content-drawing call and before its chrome —
diagnosed via a temporary `glReadPixels` full-frame PPM dump (added, used,
then removed — not a shipped feature) rather than guessing from the
symptom description, confirming the exact pixel boundaries before and
after the fix.

- **SDF text rendering** (`client/font.c`/`.h`) — real SDF, not a plain-
  bitmap compromise: `stb_truetype.h` (vendored, public domain/MIT, same
  precedent as `cgltf.h`) has `stbtt_GetCodepointSDF` built in. Bakes IBM
  Plex Sans (Regular/Bold/BoldItalic) and IBM Plex Mono into GL_R8 atlases
  at startup via a simple shelf packer (not bin-packing-optimal, fine for
  ~95 ASCII glyphs per font); a 3-line fragment shader (`UI_SDF_FRAG_SRC`
  in `client/ui.c`) thresholds and antialiases via `fwidth`-scaled
  `smoothstep`, the standard SDF text technique. IBM Plex fonts vendored
  from a separate reference project (Bold, Mono — SIL OFL, freely
  embeddable) plus IBM's own GitHub (Regular, BoldItalic — that project
  only had Bold). Baked directly into the wasm binary via the existing
  `--embed-file assets@assets` mechanism, no runtime font loading.
- **SVG icon rendering** (`client/svg_icon.c`/`.h`) — vendored `nanosvg.h`/
  `nanosvgrast.h` (zlib license) from the same reference project; the
  loading *logic* (parse → rasterize → recolor to a white/alpha coverage
  mask → upload as a texture) is the same technique that project used, but
  reimplemented against Phi's own `gl_native.h` proc-fetching rather than
  the `glad`-dependent original, since "no GL loader library" is a Hard
  Architectural Decision. `repl.svg`/`undo.svg`/`redo.svg` reused verbatim
  from that project (repl.svg is its Chat icon); `scene.svg`/`outliner.svg`/
  `properties.svg`/`console.svg`/`node_editor.svg`/`curve_editor.svg` are
  new, hand-authored to match the same simple-line-art style.
- **Panel/area layout** (`client/ui.c`/`.h`) — Blender's actual recursive
  area-split model as previously specified, not yet the interactive
  drag-to-resize/drag-to-split version: `Area` is a binary tree
  (`AREA_LEAF`/`AREA_SPLIT_H`/`AREA_SPLIT_V`), each leaf has its own
  Blender-authentic type-switcher icon in its own top-left corner (not a
  single global strip; moved from the top-right in a later pass, scaled
  down, clicking it now genuinely swaps that panel's type via a real
  dropdown — see the input-plumbing note below). The dropdown's width
  (`type_menu_width()`, shared between drawing and hit-testing the same
  way `type_icon_rect()` already was, so the two can't disagree) is now
  measured against the actual widest row text instead of a fixed guess —
  "Node Editor (not implemented yet)"/"Curve Editor (not implemented yet)"
  were being cut off at the old fixed 170px. A rounded-rect button-outline
  affordance was tried around the icon and then explicitly reverted at the
  user's request ("terrible idea") — noted here so a future pass doesn't
  re-add it without knowing that was already tried and rejected. Default
  layout is real golden ratio (`UI_PHI`/
  `UI_INV_PHI` — the actual constant, matching the reasoning the reference
  project used for its own UI sizing): Scene occupies the left column at
  the larger golden fraction (~61.8% width), the right column splits the
  same way into Outliner (top) / Properties (bottom). Split edges aren't
  draggable yet — the tree structure supports it (each split already
  stores its own fraction), the mouse-drag interaction is follow-up work.
- **Branding bar** — a fixed top strip using https://float64co.github.io's
  *actual* color palette (`--bg #f5f7fa`, `--border #e0e4ea`, and the brand
  mark's own two-span pill colors), not an adapted or deliberately-distinct
  one: Phi is a Float64 project, so this is Phi's own brand identity,
  correctly reused rather than avoided. The brand mark itself matches that
  site's real markup structure exactly (`<span id=float64>` + `<span
  id=welcome>`, two conjoined pills, no gap): "Float64" (bold italic, white
  on `#87CEEB`) directly adjacent to "Phi" (bold, white on black, not
  italic — the site's own second span isn't italic either despite the
  parent's italic font-style). The pills sit flush against the viewport's
  left edge (x=0) and flush against the bar's own top and bottom (pill
  height == `UI_BAR_H` exactly), unlike the site's version which has nav
  padding and its own internal pill padding. Bar height is derived from
  `UI_PHI`, not a flat pixel constant (`UI_MENU_H = UI_FONT_SIZE * UI_PHI`,
  `UI_BAR_H = UI_MENU_H * UI_PHI`, together forming `UI_TOP_CHROME_H`) — a
  real, noticeably shorter bar than the original flat 48px version, per
  explicit request. Undo/redo icon buttons live here (Zenith's icons, not
  yet wired to a real undo stack — no undoable actions exist yet to drive
  one).
- **Main menu row** — a second strip directly under the branding bar
  (height `UI_MENU_H`), plain File/Edit/View/Help labels with no dropdown
  content yet — proves the chrome has a place for a real menu system,
  isn't a finished one. Everything below the branding bar (this row, every
  panel's chrome/content, the type-switcher dropdown, the context menu)
  uses a separate reference project's actual dark editor palette instead
  of float64's (`col::PANEL_BG`/`WIDGET`/`WIDGET_H`/`TEXT`/`TEXT_DIM`/
  `BORDER` from that project's `DrawBatch.h`, converted 0–255 → 0–1) — a
  light branded top bar over a dark professional editor body, the split
  VSCode/Blender and most serious creative tools use.
- **Outliner row backgrounds** — full-panel-width zebra-striped row
  highlights (`outliner_row_bg()`), replacing bare left-aligned text on an
  otherwise blank panel. The Outliner is ~38% of window width by design
  (the golden-ratio split), so short rows ("World Mesh (3798 verts)") were
  reading as broken/empty space on the right rather than intentional
  layout — this is Blender's own full-width list-row convention, applied
  for the same reason Blender uses it.
- **De-Qek'd `www/index.html`** — the WASM shell's HTML still carried Qek's
  FPS-game framing: a "CLICK TO PLAY" overlay that auto-reappeared over the
  *entire* editor on every pointer-unlock (alt-tab, clicking a panel — i.e.
  constantly), plus "QEK" branding in the `<title>`/loading screen/overlay
  heading. Fixed at the DOM/JS level (no C/WASM change needed): the overlay
  no longer auto-shows on load or re-shows on unlock (an editor doesn't
  gate itself behind a play button), and the loading screen and overlay
  both say "Phi".
  A second de-Qek pass, once real clicking (below) made the actual bug
  reachable: `index.html` also had a "click canvas directly to re-lock if
  focus is lost" handler left over from Qek's always-on mouse-look — since
  every 2D UI panel click IS a canvas click (the panels render *inside*
  the canvas via WebGL, no separate DOM elements), this was silently
  hijacking every single UI click into `requestPointerLock()`, hiding the
  OS cursor the instant you clicked anything. Removed entirely, matching
  native/win32 which already dropped click-to-engage this session. Also
  hid `#hud` (HP/ammo/bots readout) and `#crosshair` (aim reticle) by
  default via CSS — permanently-visible Qek gameplay chrome sitting over
  the panel UI — and removed the old `#console` DOM overlay (a fixed bar
  across the top, shown/hidden via `update_console_ui`'s `EM_ASM` calls on
  `` ` ``) entirely, since it's now redundant with the real in-canvas
  Console panel reading the exact same `ConsoleState`; `update_console_ui`
  is now a deliberate no-op on both platforms rather than deleted outright,
  since `cs->open`/`console_open` still does real work gating WASD/mouse
  input while typing (see `console.c`), only its DOM visibility side
  effect was removed. `#hud`/`#crosshair` are still updated by C every
  frame on a hidden element (harmless) so a future "play mode" toggle can
  un-hide them via CSS alone. Native+win32+wasm build-verified; the
  DOM/CSS/JS side of this (which is most of it) could not be independently
  browser-tested in this environment — flagged honestly, not claimed as
  verified.
- **Panels**: Scene (the existing G-buffer pipeline, now hosted in an
  arbitrary sub-rectangle instead of always filling the window — see
  `gbuffer_set_viewport_offset()` below), Outliner (lists the real world
  mesh vert count, the Phase 1 test MeshObject, and connected players —
  not a general scene-graph browser yet, there isn't a general scene graph
  yet), Properties (real position/orientation readout for whatever's
  selected, read-only — the DNA/RNA property-widget system above this
  section is still just a design sketch), Console (Phi already had one —
  `client/console.c`, previously DOM-only on wasm and invisible on native/
  win32 — now also renders through this system, giving native/win32 a
  visible console for the first time), Chat (UI shell only — text input +
  scrollback, explicitly not connected to a real LLM: that needs a
  server-side API proxy per the Hard Architectural Decision that the
  Anthropic API key never ships to a client, which is separate, substantial
  work). Node Editor and Curve Editor are selectable-but-honestly-stubbed
  entries in the type-switcher (Phase 6 and Phase 4 respectively, neither
  started) rather than faked panels.
- **Real mouse-click routing** — `ui_on_mouse_button()` existed since the
  first shell landed but was never actually called from `main.c`; clicking
  anything (the type-switcher icon, its dropdown, Outliner rows) had zero
  effect. Fixed by giving `InputState` (`input.h`) a real absolute cursor
  position (`mouse_x`/`mouse_y`, tracked unconditionally on every
  mouse-move on all three platforms — previously native/win32 only
  computed pointer-lock *deltas*, and wasm's own mouse handlers were fully
  gated behind `pointer_locked`, which now defaults off) plus rising-edge
  `lmb_click`/`rmb_click` flags (same "main.c drains and clears it"
  convention as `fire`/`export_stl`). `main.c`'s frame loop now calls
  `ui_layout()` and builds `UIRenderContext` *before* gameplay input
  processing (moved up from where `ui_render()` needed it, and reused for
  both), routes any click through `ui_on_mouse_button()`, and discards the
  `fire` edge if the click was UI-consumed — same reasoning editor mode
  already used to discard `fire` while editing. Verified live, not just by
  reading the code: launched native headless, used `python-xlib`'s XTest
  extension to synthesize a real click on the Outliner panel's
  type-switcher icon (confirmed the dropdown opened via a screenshot),
  then a second click on its "Console" row (confirmed the panel actually
  swapped to the real dev console via a second screenshot).
- **3D scene right-click context menu** — now genuinely opens on a real
  right-click, not just the mechanism (`client/ui.c`'s
  `ui_open_scene_context_menu`/`ui_is_context_menu_open`, a menu drawn and
  hit-tested the same way the reference project's `ContextMenuItem` system
  worked, reimplemented not copied). `main.c`'s `rmb_click` handling first
  offers the click to `ui_on_mouse_button()` (dismisses an already-open
  menu, same as any other right-click); if nothing claimed it, the click
  landed inside the Scene panel's own content rect (not chrome), and the
  octree editor isn't active (which already owns RMB for carve-drags, same
  reasoning the LMB fire-gating uses), it calls
  `ui_open_scene_context_menu()` there. **Add > Mesh Object** and
  **Delete** are now wired to real behavior via
  `ui_poll_context_menu_action()` (a `CtxMenuAction` enum, drained once
  per frame in `main.c` the same way `InputState`'s `lmb_click` etc. are —
  `ui.c` sets the pending action on row-select, `main.c` acts on it and
  clears it). Add reuses `spawn_test_mesh_object()` (factored out of what
  was startup-only inline code in `main()`) if the one test-object slot
  isn't already occupied — there's still only a single `g_test_mesh_object`
  slot, not a general spawn-many-objects system, so Add on an already-
  occupied slot just logs that rather than silently doing something
  confusing. Delete frees the slot's `RenderMesh` and clears it via
  `mesh_destroy()`, and clears the UI's own selection if it pointed at
  that object, but only acts when that object is actually selected
  (mirrors normal editor "Delete acts on the selection" semantics) — a
  Delete with nothing selected just logs and no-ops. Frame Selected/Frame
  All/Deselect All still just print which one was clicked — real actions
  behind those three are follow-up work. Verified live end-to-end:
  selected the MeshObject via the Outliner, right-clicked the Scene panel,
  clicked Delete (log confirmed deletion, Outliner row disappeared in a
  screenshot), right-clicked again and clicked Add (log confirmed reload,
  Outliner row reappeared in a screenshot) — all via `python-xlib`'s
  XTest extension, not just read from the code.
- **Ray-vs-mesh picking** (`meshobject.c`'s `meshobject_ray_pick`,
  Phase 1's own first item, started once the UI click-plumbing above made
  it reachable) — real Möller–Trumbore ray/triangle intersection against
  a MeshObject's actual world-space triangle data (`RenderMesh`'s
  flattened, vertex-duplicated-per-triangle layout, transformed by the
  object's position + `quat_to_mat4` rotation — not a bounding-box
  approximation), returning the *nearest* hit across every triangle so
  overlapping geometry resolves to the visually-correct face. `main.c`'s
  `try_pick_object` constructs the click ray from the same camera basis
  `renderer.c`'s `mat4_look_dir`/`editor.c`'s `view_dir` already use
  (matched exactly, not re-derived, so a pick always agrees with what's
  actually rendered), using the Scene panel's own on-screen rect and
  aspect ratio for the NDC conversion. A Scene-panel click (outside octree-
  edit mode) is now always a select-or-deselect action — hit selects via
  the existing `ui_set_selected_object()` 4000+id convention, miss
  deselects — and this counts as UI-consumed for fire-gating too, so
  picking an object can't also fire a rocket on the same click (same
  reasoning the UI-chrome-click gating already established). Verified
  live via `python-xlib`: a deterministic unit-style check (aiming a ray
  straight at the object from a known camera position derived from the
  existing frame-30 diagnostic-camera override) confirmed an exact
  expected hit distance (camera 30 units back, object's 8-unit half-
  extent, hit at t=22.00 — not just "some non-zero result"), then a real
  synthesized mouse click through the full UI pipeline was screenshotted
  hitting (Outliner/Properties updated to show the selection) and missing
  (both cleared) in separate rounds.
- **Found and fixed while verifying picking**: the console (`console.c`)
  had been unusable in practice since the "stop stealing the mouse"
  round — `console_update()` force-closed it every single frame it was
  open unless `pointer_locked` was true, and pointer lock now defaults
  off with no click-to-engage gesture on any platform, so the console
  closed itself one frame after every open. That check existed for one
  specific scenario (Chrome doesn't always deliver an Escape keydown when
  what triggered it was exiting pointer lock, so the console needs to
  watch `pointer_locked` directly to close in sync) but was written as a
  standing condition ("not currently locked") rather than a transition
  ("just lost lock while open"). Fixed by tracking the previous frame's
  `pointer_locked` state and only force-closing on an actual
  locked→unlocked transition — the original Escape scenario is still
  covered (that is such a transition), but the console can now actually
  be opened and used at all now that lock is normally off. Directly
  relevant to the Console-panel-as-Python-REPL work below, which depends
  on the console being reachable in the first place.
- **Transform gizmo** (`client/gizmo.c`/`.h`, new module) — translate-only
  for this first pass, per the plan (rotate/scale stubbed out entirely
  rather than shipped half-working). Three axis handles (X=red, Y=green,
  Z=blue) drawn at the selected MeshObject's world position via the new
  `renderer_draw_solid_box()` (see the depth-test bug note above — this
  started life using the existing `renderer_draw_wire_box`, whose 1-pixel
  `GL_LINES` edges turned out to still be geometry-pass-correct but
  invisible in the final composited frame even *after* the depth fix,
  since a sub-pixel-coverage line is exactly what TAA's temporal
  blending/FXAA's edge-smoothing are designed to suppress — solid filled
  triangles have real per-pixel area and survive). Picking a handle
  (`gizmo_pick_handle()`, real ray/AABB intersection against each handle's
  world-space bounds, not a 2D screen-space guess) takes priority over
  re-picking the object body underneath it, wired into the same
  `try_pick_object()` LMB-click path picking itself uses. Dragging
  (`gizmo_begin_drag()`/`gizmo_update_drag()`) uses the standard
  ray/drag-plane-intersection technique for single-axis constrained
  motion from 2D mouse input (the drag plane contains the grabbed axis
  and is oriented toward the camera, degenerating gracefully — a
  fallback plane — when the axis is nearly parallel to the view
  direction, e.g. looking straight down the handle), updating
  `MeshObject.position` in place every frame the LMB stays held
  (`main.c`'s per-frame loop, not a click-edge — a continuous drag needs
  continuous updates). Verified live via `python-xlib`'s XTest extension
  and real screenshots: the handle boxes render with their own correct
  per-axis color (not sky-blue, post depth-fix), and a deterministic
  ray-straight-at-the-object unit-style check (reusing the existing
  frame-30 diagnostic camera) confirmed exact expected pick/hit math.
  Rotate and scale are explicitly not implemented — shipping translate
  solidly was the stated priority over three half-working modes.
- **Extrude / inset / loop cut** (`client/mesh_edit.c`/`.h`, new module,
  built on `halfedge.c`'s existing append-only primitives rather than
  extending that file directly — it's the "editing operations" layer its
  own header comment already anticipated). `halfedge.c` gained one new
  primitive to support this: `halfedge_delete_face()`, a soft-delete
  tombstone (`HEFace.deleted`) rather than a real removal, since nothing
  else in that structure supports in-place mutation or index reuse.
  Deleting a face resets any twin pointing at one of its edges back to -1,
  so a neighboring face correctly becomes boundary again instead of
  dangling — and `find_edge()`'s twin-lookup scan now skips edges
  belonging to a deleted face, since without that guard a freshly
  re-triangulated area could accidentally twin against the very geometry
  it just replaced (a real bug caught while writing this, not a
  theoretical one — see the standalone test below).
  - **Extrude**/**inset** share one mechanism (`mesh_edit.c`'s static
    `extrude_or_inset`): add fresh vertices at the new (displaced-along-
    normal, or shrunk-toward-centroid) positions, delete the original
    face, add a new cap face at the fresh positions, then stitch a ring of
    side-wall triangles connecting the *untouched* original boundary to
    the new cap boundary — `halfedge_add_face`'s own twin-finding then
    naturally reconnects the walls to whatever real neighbor already
    bordered the original face. This produces the same topology real
    extrude/inset do; the original face's array slot becomes a dead
    tombstone rather than literally being the moved geometry, which is
    invisible to anything that only reads live faces.
  - **Loop cut** is scoped down honestly: it splits one picked edge (and
    its immediate twin face, if any) at the midpoint, re-triangulating the
    one or two adjacent triangles around it. This is the minimal real
    primitive a full multi-face loop cut is built from, not a ring traced
    across a whole quad-topology loop — this codebase's editable meshes
    are triangles-only (see `halfedge.h`), so a "ring" in the Blender
    sense doesn't strictly exist here. Shipping this honestly-scoped
    primitive was the call, not a fake full-ring implementation.
  - Reachable via the Scene right-click context menu (`CTX_ACTION_
    EXTRUDE_FACE`/`INSET_FACE`/`LOOP_CUT`, new rows alongside the existing
    Add/Delete ones). Right-clicking with the MeshObject selected first
    ray-picks the face under the cursor against its live `HalfEdgeMesh`
    directly (`meshobject_ray_pick_face`, same Möller–Trumbore technique
    as object-level picking) and records it (`main.c`'s `g_edit_face`) —
    the menu acts on whatever was under the cursor at open time, matching
    real editor behavior. Loop Cut further narrows the face pick to a
    specific edge (`mesh_edit_nearest_edge_of_face`, nearest edge-midpoint
    to the local-space hit point — `meshobject_world_to_local` added for
    this, the rotation matrix's transpose-as-inverse since it's
    orthonormal). Extrude uses a fixed 4-unit offset, inset a fixed 0.4
    shrink factor — no mouse-driven interactive distance in this pass,
    matching this loop's "minimal, correct, not Blender-grade polish" bar.
    The object's `HalfEdgeMesh` is now kept alive on `MeshObject.hem`
    across its lifetime (previously `spawn_test_mesh_object` built the
    render mesh once and immediately `halfedge_destroy()`'d it) so it can
    be edited more than once; each op re-flattens via the existing
    `meshobject_build_render_mesh_from_halfedge` and logs a before/after
    triangle count.
  - **Verified via a new standalone test harness** (`client/
    mesh_edit_test_main.c`, `make mesh_edit_test` — no GL/window/X11
    dependency at all, same spirit as `mp_test`/`mp_stress`), not a live
    screenshot round this time: this iteration's background-job
    environment turned out to have an unstable X11 connection (WSLg's
    Xwayland; the native binary's window intermittently received
    `WM_DELETE_WINDOW` and exited on its own after anywhere from ~15
    seconds to ~12 minutes, and eventually direct `python-xlib` display
    connections themselves started failing with `ConnectionResetError:
    [Errno 104] Connection reset by peer`) — reproducing before any of
    this feature's own code existed, so it's an environment issue for
    this pass, not a regression, and not worth burning further loop
    iterations chasing (the picking/gizmo work earlier used this exact
    live-screenshot method successfully many times this same session).
    Topology correctness doesn't actually need rendering to verify,
    though, so the harness checks what matters directly against
    `assets/cube.gltf` (8 verts, 12 triangles, watertight): extrude adds
    exactly 3 vertices and nets +7 live triangles (-1 base +1 cap +6
    walls), each new vertex sits exactly `dist` from its source vertex
    along the face normal (measured, not assumed); inset does the same
    topology math without the normal offset; loop-cutting a (twinned,
    since the cube is closed) edge adds exactly 1 vertex and nets +2 live
    triangles; every case is checked to stay fully watertight (zero
    boundary edges) afterward and to have no live edge twinning a deleted
    face (the exact bug class the `find_edge` fix above exists to
    prevent); and out-of-range/already-deleted inputs fail cleanly (-1)
    rather than crashing. All checks pass. The context-menu row wiring
    and click routing itself (as opposed to the topology math it calls)
    is code-reviewed but not live-screenshot-verified this pass — flagged
    honestly rather than claimed either way.
- **Gbuffer extension**: `gbuffer_set_viewport_offset(gb, x, y)` — a small,
  deliberate extension to Phase 0's (already shipped, browser-verified)
  deferred pipeline. Every pass except the very last (FXAA's blit to the
  default framebuffer) already rendered into its own private texture at
  (0,0), so only that one `glViewport` call needed an offset to let the
  Scene panel host the 3D view in an arbitrary sub-rectangle instead of
  always filling the window. Combined with the already-existing
  `gbuffer_resize()`, this is what makes a non-fullscreen 3D viewport
  panel possible at all.
- **PBR material assignment per face** (`halfedge.h`'s `HEFace.base_color/
  metallic/roughness/emission`, `halfedge_set_face_material()`; `client/
  meshobject.h`'s new `MESHOBJ_VERTEX_STRIDE=14` layout; a new dedicated
  `renderer.c` shader/program; `gbuffer.c`'s lighting pass extended to
  actually consume it) — real per-face material, not a cosmetic field.
  - **Data model**: material lives directly on `HEFace` (base_color[3],
    metallic, roughness, emission[3]) rather than a separate material
    table — faces are already the natural per-face unit and this codebase
    has no other indirect-reference machinery, so a table would be
    structure for its own sake. `halfedge_add_face()` defaults every new
    face to a neutral dielectric/rough material (0.7 grey, metallic 0,
    roughness 0.8); `halfedge_set_face_material()` sets it, clamping
    metallic/roughness to [0,1] (base color and emission are left
    unclamped — emission in particular is expected to go above 1.0 for a
    genuinely bright/bloomable surface). `mesh_edit.c`'s extrude/inset/
    loop-cut all copy the source face's material onto whatever new faces
    replace it (captured before `halfedge_delete_face`, since a tombstoned
    face's data stays intact but its slot is about to represent something
    else) — extruding or splitting a red face keeps it red rather than
    silently reverting to the default grey.
  - **Rendering**: MeshObject's own vertex format
    (`meshobject_build_render_mesh_from_halfedge`) grew from the shared
    `VERTEX_STRIDE=7` (pos, normal, an unused `mat_id` float that had no
    fragment-shader consumer at all — dead plumbing since Phase 1's
    foundation slice) to a dedicated 14-float layout duplicating the
    owning face's full material across all 3 corners, the same way
    per-face flat normals already get duplicated. This is deliberately a
    SEPARATE format/shader path from the shared world/ground/players/
    rockets one (`renderer.c`'s new `PBR_VERT_SRC`/`PBR_FRAG_SRC`,
    `pbr_program`) rather than growing the shared format for everyone —
    that geometry has no per-face material concept (one `glUniform3f` per
    whole draw call) and giving every vertex in the game 8 unused extra
    floats would be pure waste. Discovered and fixed two real correctness
    issues this required: `octree_render.c`'s `mesh_upload()` hardcoded
    `VERTEX_STRIDE` for its `glBufferData` byte-size calculation (would
    have silently uploaded only half of each MeshObject vertex's actual
    bytes) — factored into a new `mesh_upload_stride(m, stride_floats)`
    that `mesh_upload()` itself now just calls with the generic constant;
    and the generic `mesh_create()`'s initial buffer allocation is sized
    for the generic stride too, so `meshobject_build_render_mesh_from_
    halfedge` now corrects its own buffer's byte size up front rather than
    trusting whatever it inherited.
  - **Lighting**: `gbuffer.c`'s G-buffer had carried dedicated material/
    emissive render targets since Phase 0, but the lighting pass never
    actually sampled them — every surface rendered flat-diffuse regardless
    of what the (unread) material buffer said. Now it does: a deliberately
    simplified Blinn-Phong-with-real-PBR-inputs approximation (not a full
    Cook-Torrance/GGX BRDF — that's more than this phase needs), with
    metallic surfaces losing their separate diffuse term and gaining an
    albedo-tinted specular one, dielectrics keeping a flat 0.04 specular
    reflectance, and roughness controlling both specular sharpness and
    strength. `gbuffer_resolve()` gained a `cam_pos` parameter (the
    specular term needs a view direction) threaded from `ui.c`'s existing
    `Renderer*`. The shared world/ground/players/rockets shader's own
    material placeholder changed from an arbitrary `(0.5,0,0,0)` to a
    deliberate neutral `(metallic=0, roughness=1)` — plain Lambertian,
    matching that geometry's pre-existing flat-diffuse look now that the
    channel is actually read, rather than an arbitrary half-metallic value
    suddenly becoming visible.
  - **Editor**: a per-face material readout in the Properties panel
    (base_color/metallic/roughness/emission for whichever face was last
    ray-picked — `ui.c`'s `draw_panel_properties`, reading a new
    `UIRenderContext.edit_face`) plus four new console commands
    (`matcolor r g b`, `matmetal v`, `matrough v`, `matemit r g b`) that
    edit it, per this task's "simple readout+editor, not a full
    material-browser UI" scope. `main.c`'s `try_pick_object` now does
    face-level picking (`meshobject_ray_pick_face`, already built for
    extrude/inset/loop-cut) on every Scene-panel click, not just
    right-click, so the Properties readout and the context-menu edit
    target both track "whichever face was last actually clicked."
    `console_update`/`console_submit`/`console_dispatch` all gained
    `MeshObject*`/`edit_face` parameters to reach this — `console.h` now
    includes `meshobject.h`.
  - **Verification**: this iteration hit a deeper environment problem than
    the extrude/inset/loop-cut work earlier in this same loop — the X
    server became fully unresponsive (`xdpyinfo` itself hangs/times out,
    not just the game window's own connection resetting), so not even a
    single native GL context could be created, headless or otherwise, at
    any point while this feature was built. No live or numeric-readback GL
    verification was possible this pass — flagged honestly rather than
    guessed at. What WAS verified: the data-model layer (`halfedge.c`'s
    material defaults/clamping/inheritance) via `mesh_edit_test`'s
    existing no-GL harness, extended with real checks (default values,
    `halfedge_set_face_material`'s clamping behavior, and that extrude's
    new cap AND wall faces both inherit the source face's exact material)
    — all passing. The GL-dependent layer (shader compilation, vertex
    attribute wiring, the lighting pass's new sampling/specular math) was
    verified by careful manual cross-checification instead of execution:
    every attribute name in `PBR_VERT_SRC` matches `link_pbr_program`'s
    `glBindAttribLocation` calls; every vertex attribute's byte offset in
    `renderer_draw_mesh_object` matches `meshobject_build_render_mesh_
    from_halfedge`'s actual write layout field-by-field; every sampler/
    uniform name in `LIGHTING_FRAG_SRC` matches its `glGetUniformLocation`
    call and its `gbuffer_resolve` binding; texture units 0-5 are each
    used exactly once. This is real scrutiny, not a substitute for
    actually running it — the shader logic itself (in particular the
    simplified specular BRDF's visual balance) remains genuinely unseen
    and should be the first thing checked once a real display is
    available again.
- **Voronoi pre-fracture tool** (`client/fracture.h`/`.c`, new module) —
  real computational geometry, not a stub. Editor-only precompute, no
  runtime activation and no Bullet involvement anywhere in this file
  (both already excluded from this loop's scope, see phi.md's Fracturing
  section — pre-fracturing covers the vast majority of game use cases and
  runtime fracture-on-impact needs Bullet, which is Phase 2's job).
  - **Algorithm**: standard mesh-vs-convex-region clipping. For N random
    seed points within the source mesh's own local-space bounding box
    (a self-contained xorshift32 PRNG, not libc `rand_r` — not portable to
    the win32/mingw cross-build target), fragment `i` is the source mesh
    clipped against the intersection of the `N-1` half-spaces "closer to
    seed `i` than seed `j`" (the perpendicular bisector plane between
    every pair) — Sutherland-Hodgman polygon clipping per face, with the
    newly-exposed cross-section capped by a fresh convex polygon after
    each plane so every fragment stays a closed, watertight solid. Working
    representation during clipping is a plain dynamic polygon/poly-mesh
    (`Poly`/`PMesh`, ordered vertex loops, no twin/edge topology) rather
    than forcing `HalfEdgeMesh`'s triangles-only model to do double duty —
    a clip can turn a triangle into an n-gon before the final fan-
    triangulation pass. **Honest scope limit**: this assumes the source
    mesh is itself convex (true for the cube.gltf test asset) — a plane
    intersects a convex polyhedron in at most one convex cross-section,
    which is what makes "collect the cut points, sort by angle into one
    loop, cap it" correct. A non-convex source could produce a
    cross-section with multiple disconnected loops, which this pass
    doesn't attempt to handle — the same kind of explicit scoping decision
    as mesh_edit.c's loop-cut ("one edge, not a full ring"), not a silent
    gap.
  - **A real bug found and fixed by its own test harness**: the first
    working version produced fragments with ~0.5-0.7 "closedness error"
    (see Verification below) and exactly-sign-flipped total volume. Root
    cause: this codebase's actual face-winding convention, confirmed
    against `assets/cube.gltf`'s real index data, has
    `cross(v1-v0,v2-v0)` point INWARD, opposite the textbook "CCW-from-
    outside" assumption `cap_from_cut_points`'s cap-orientation logic
    initially used (an assumption stated, but never actually verified
    against real data, in this session's own earlier mesh_edit.c
    comments too — harmless there since extrude/inset/loop-cut only need
    internal consistency between old and new faces, not an absolute
    outward-vs-inward sign; fracture's cap construction is the first
    place in this project that sign has actually mattered). Original
    (unfractured, all-original-faces) meshes have zero closedness error
    regardless of which way the global convention runs, since they're
    internally consistent with themselves — it's specifically MIXING
    newly-built caps against original faces under the wrong assumed
    convention that breaks closedness, which is exactly what showed up.
    Fixed by flipping the cap sort basis so its winding matches the
    source data's actual (inward-cross-product) convention. This is a
    genuinely load-bearing example of why the harness computes real
    geometric identities instead of eyeballing output.
  - **Output**: `PHI_fracture_fragments`, a namespaced glTF extension on a
    hand-rolled minimal `.gltf` + sibling `.bin` (same style as
    `halfedge_gltf.c`'s `halfedge_save_gltf`, since `cgltf` itself is
    read-only) — each fragment is an ordinary glTF mesh/node (position-
    only, unsigned-short-indexed, `mode: 4`), not a duplicated separate
    file; `extensions.PHI_fracture_fragments.fragments` lists their mesh
    indices so a PHI-aware consumer can find them as a group while any
    ordinary glTF viewer just sees N normal meshes. Empty fragments (a
    seed's cell not intersecting the mesh at all — a legitimate, not
    erroneous, outcome for some random placements) are skipped rather
    than written as degenerate zero-vertex meshes. No vertex welding/
    deduplication in this pass — flat, duplicated-per-triangle output,
    same convention this codebase's other flattening paths already use.
  - **Editor**: reachable via a new "Fracture (Voronoi)" Scene right-click
    context menu row (`CTX_ACTION_FRACTURE`), acting on the whole selected
    MeshObject (not a picked face, unlike Extrude/Inset/Loop Cut) — fixed
    8-fragment count and a real time-based seed, writing to
    `assets/fracture_output.gltf`. No interactive fragment-count picker
    UI in this pass, matching the "precompute tool" scope; doesn't mutate
    the live MeshObject or activate anything at runtime.
  - **Verification**: a new standalone test harness (`client/
    fracture_test_main.c`, `make fracture_test`, no GL dependency, same
    rationale as `mesh_edit_test`) checks two real divergence-theorem
    identities for a closed triangle mesh rather than approximations —
    enclosed volume via `(1/6) * sum(v0 . (v1 x v2))` over all triangles,
    and "closedness" via `sum(triangle_area * unit_normal)`, which must be
    the zero vector for ANY truly watertight closed mesh regardless of
    shape. Both are exact identities: if these numbers come out right, the
    fragments really are the closed solids they're supposed to be, not
    just plausible-looking. Verified: the volume formula itself reads
    exactly 1.0 for the known unit cube (sanity check on the check);
    fracturing into 2/4/8 pieces (fixed seed for reproducibility) at every
    count produces fragments that are all watertight (closedness error
    <0.02, most exactly 0), all have non-negative volume, and sum to
    within 2% of the original mesh's volume (typically exact to floating-
    point precision) — real conservation, not just "didn't crash." The
    glTF export path is also exercised for real (written to `/tmp`, then
    read back and checked for the actual extension string and `mode: 4`
    primitives), plus defensive checks (`n_fragments<1`, `NULL` hem) fail
    cleanly. All checks pass. Like the PBR material work immediately
    before this in the same loop, no live GL verification was possible
    this pass (the X server remained fully unresponsive throughout —
    `xdpyinfo` itself still hangs/times out) — this tool has no rendering
    path of its own to verify that way regardless (it's a pure geometry-
    to-disk precompute), so the gap is smaller here than for the PBR
    shader work, but still noted honestly.
- **MicroPython wired into the real build; Console panel becomes a real
  Python REPL** — the last item of this loop's Phase 1 completion pass.
  Genuine code execution reachable from the shipped editor now, not a
  standalone self-test binary anymore. Explicitly NOT Phase 5's
  `phi.emit`/`ctx.prop`/`@phi.panel`/`@phi.node` decorator API surface —
  the user's own instruction for this loop was "just need the Console
  working," and that's the whole scope of this item.
  - **Build integration**: `client/mp_port.c`'s hand-written glue and the
    generated `client/micropython_embed/` tree (previously linked only
    into the standalone `mp_test`/`mp_stress` self-test binaries) are now
    part of `COMMON_SRCS` and link into all three real targets
    (`phi_native`, `phi_win32.exe`, `game.wasm`) with zero symbol
    conflicts against the existing ~20-file engine codebase or each
    other — genuinely surprising for a from-scratch language runtime this
    size dropped into an existing C project, verified by the build simply
    succeeding rather than assumed. `MP_EMBED_DIR`/`MP_EMBED_SRCS` moved
    up in the Makefile (before `COMMON_SRCS`, which now references them)
    since `COMMON_SRCS` uses immediate `:=` expansion.
  - **New Console-facing API** (`client/mp_port.h`, new header — the
    existing self-tests reached into MicroPython's own headers directly,
    `console.c` shouldn't have to): `phi_mp_init(stack_top)` (same
    `mp_embed_init` + `mp_stack_set_limit` workaround `mp_test_main.c`
    already proved necessary, see its own comment) and `phi_mp_exec(code)`
    — runs one block of Python through the already exception-safe
    `mp_embed_exec_str` (an uncaught exception prints a traceback via
    `mp_plat_print` rather than crashing, see
    `micropython_embed/port/embed_util.c`) and returns everything printed
    during the call as one malloc'd string.
  - **Output capture**: `mpconfigport.h` overrides `MP_PLAT_PRINT_STRN`
    (guarded `#ifndef` in `mpconfig.h`, an intentional port-override
    point, not a hack) to route every `print()`/traceback byte through a
    new `phi_mp_capture_output()` in `mp_port.c` instead of the default
    `mp_hal_stdout_tx_strn_cooked` (`mphalport.c`, plain `printf` — invisible
    from inside the game window). Confirmed `MICROPY_PY_IO`/
    `MICROPY_PY_SYS_STDFILES` are both off (already set, for the no-real-
    filesystem reasons `mpconfigport.h`'s own header comment gives), which
    compiles out `mpprint.h`'s alternate `sys.stdout`-based output path —
    `mp_plat_print` is genuinely the only path, so this one override
    covers everything. **Side effect, worth being honest about**: this
    override is global to the whole embedded interpreter, so it also now
    applies to the pre-existing `mp_test`/`mp_stress` self-test binaries
    — a `print()` call inside one of `mp_test_main.c`'s test scripts (step
    2's diagnostic `"py side saw: 42"`) no longer reaches the terminal,
    silently captured into a buffer that binary never reads. Confirmed
    this doesn't affect correctness: both binaries still `PASS` (rebuilt
    and re-run to check, not assumed) since their actual assertions read
    Python state back via `mp_obj_get_int` etc., not by parsing printed
    text — only that one cosmetic diagnostic line's terminal visibility
    changed.
  - **`main.c` wiring**: `phi_mp_init(&mp_stack_top)` is called once at
    startup, with `mp_stack_top` a local declared at `main()`'s own
    top-level scope (not inside a helper). This matters: MicroPython's GC
    does a conservative scan of the C stack between the current stack
    pointer and this recorded boundary on every collection, so it has to
    stay valid for every stack depth the interpreter could ever be called
    from later in the program's life. Trivially true for native/win32
    (`phi_platform_set_main_loop` blocks in a real loop inside `main()`'s
    own still-active frame for the rest of the process's life) and, for
    wasm, matches the standard Emscripten pattern for exactly this kind of
    conservative GC (`emscripten_set_main_loop`'s `simulate_infinite_loop`
    mode keeps a consistent per-callback stack depth close to the original
    call site — not invented here, documented in `mp_port.h`'s own
    comment).
  - **`console.c` wiring**: `console_dispatch`'s final `else` (previously
    `"unknown command: %s"`) now calls `phi_mp_exec(line)` — the ORIGINAL
    typed line, not just the first token, since a Python statement can be
    more than one word — and splits the captured output on `\n` into the
    scrollback via a new `log_push_multiline` helper, so real multi-line
    `print()` output or a traceback reads as actual separate lines rather
    than one run-on entry. Every existing Qek dev-command (`pos`/`tp`/
    `grid`/... through `matemit`) is still tried FIRST and still wins if
    matched — Python is strictly the fallback for anything unrecognized,
    per the user's own explicit instruction to preserve these. `help`'s
    text and the console's opening banner both updated to mention this.
    `console_update`/`console_submit`/`console_dispatch` all gained
    `MeshObject*`/`edit_face` parameters two commits ago for the
    `matcolor`/etc. commands — no further signature changes needed here,
    `phi_mp_exec` doesn't need that context.
  - **Verification**: a new standalone harness (`client/
    mp_console_test_main.c`, `make mp_console_test`, no GL dependency,
    distinct from `mp_test` which validates Phase 5's decorator patterns —
    out of scope here) checks the actual mechanism `console.c`'s fallback
    now depends on, real interpreter behavior not mocked: `print()` output
    is genuinely captured (not lost to the real stdout); real computation
    happens (`21 * 2` evaluates, doesn't just echo); multi-line output
    round-trips with real line breaks intact; an uncaught exception is
    captured as a readable traceback and the process stays alive
    (confirmed by the next check actually running, not just a return
    code); the capture buffer correctly resets between calls with no
    cross-contamination; and — importantly — globals persist across
    separate `phi_mp_exec` calls (a variable set in one call is visible
    and usable in the next), confirming this behaves like a real REPL
    session and not a fresh throwaway interpreter per line. All checks
    pass, plus `mp_test`/`mp_stress` re-run clean (see the output-capture
    side-effect note above). As with the PBR/fracture work earlier in
    this same loop, the actual Console panel's live keystroke-to-
    scrollback round trip could not be screenshot-verified this pass (the
    X server remained fully unresponsive throughout) — the interpreter
    plumbing underneath it has no GL/window dependency and is fully
    verified; the UI glue connecting it to real keyboard input and the
    on-screen scrollback panel is code-reviewed but not live-verified,
    flagged honestly rather than either claimed or silently skipped.
- **Qek cleanup: bots removed, Console is now a pure Python shell** — a
  deliberate pivot-completion pass, not a feature. "We're in Phi now, not
  Qek": the AI bot system and every one of console.c's bespoke dev-
  commands (Qek's own, plus this session's own `matcolor`/`matmetal`/
  `matrough`/`matemit` stopgap from the PBR material work above — real
  Python execution existed by the time those were added, so keeping them
  as a separate command layer was already redundant) are gone rather than
  ported forward.
  - **Bots**: `physics_spawn_bots`/`physics_add_bot`/`physics_remove_bot`/
    `physics_update_bots` and all bot AI state (`Player.is_bot`/
    `bot_think_timer`/`bot_fire_timer`/`bot_target_pos`/`bot_jump`,
    `MAX_BOTS`/`DEFAULT_BOTS`/`BOT_THINK_RATE`/`BOT_FIRE_RANGE`/
    `BOT_MOVE_SPEED`) deleted from `physics.h`/`.c`, confirmed pure client-
    side (never touched `net.c`'s wire protocol or `server/server.py` at
    all — each client would have simulated bots independently in
    multiplayer, an existing quirk this removal sidesteps rather than
    fixes). `renderer.c`'s bot/player color branch and `ui.c`'s Outliner
    "Bot"/"Player" label both collapsed to just "Player" now that only
    one kind exists.
  - **Console**: `console_dispatch` — the entire `if`/`else if` command
    table — is gone. `console_submit` now just calls `phi_mp_exec(line)`
    directly and shows whatever it printed. This turned out to remove
    ALL of console.c's coupling to the rest of the engine at once: with
    no `pos`/`tp`/`kill`/`hp`/`give`/`god` (needed `Player*`), no `grid`/
    `mat`/`noclip` (needed `EditorState*`), no `name`/`save`/`load`/
    `newmap`/`maps` (needed `NetState*`), no `fov`/`skybox` (needed
    `Renderer*`), and no `matcolor`/etc (needed `MeshObject*`/
    `edit_face`), `console_update`'s signature shrank to just
    `(ConsoleState*, InputState*)` — `console.h` dropped its includes of
    `editor.h`/`net.h`/`physics.h`/`renderer.h`/`meshobject.h` entirely,
    down to just `input.h`. `bind`/`unbind` are gone too (a general
    keybind-to-command-string utility, but with no command layer left to
    bind a key TO, keeping it would mean binding keys to raw Python
    snippets — a real but unrequested feature, not shipped speculatively)
    — `ConsoleState.bind_keys`/`bind_cmds`/`bind_count`/
    `CONSOLE_MAX_BINDS` deleted with it. Face material editing has no
    replacement yet as a result: the Properties panel's read-only
    material readout (base_color/metallic/roughness/emission) still
    works, but there's currently no way to CHANGE a face's material at
    all until Phase 5's real Python↔C API surface exists — an honest
    capability regression versus the immediately-preceding commit, not
    an oversight.
  - **Dead code found and removed along the way**: `bind`'s removal made
    `InputState.pressed_codes`/`pressed_code_count`/
    `PRESSED_CODE_QUEUE_SIZE`/`KEY_CODE_LEN` entirely unused — that
    machinery existed solely to feed `bind`'s keycode matching, was only
    ever populated on the wasm build (`input.c`'s two native/win32
    branches already carried "known gap: bind is a no-op here" comments,
    now removed along with the field they were caveating) and had no
    other consumer anywhere in the codebase (confirmed by grep before
    deleting, not assumed) — a real, if small, dead-code cleanup rather
    than just moving the bot/command removal's mess elsewhere.
    `physics.h`'s `MOVE_SPEED`/`GRAVITY` runtime-tunable globals
    (`g_move_speed`/`g_gravity`) stay — no longer settable from the
    console (the `speed`/`gravity` commands are gone), but still real
    values movement code reads, left alone rather than removed
    speculatively.
  - All three targets (`native`/`win32`/`wasm`) rebuilt clean after this
    pass, zero new warnings from any touched file.
- **Python panel is now an always-focused text input** (`input.c`/
  `console.c`/`ui.c`, same cleanup arc as the bots/commands removal above)
  — no more backquote-toggled open/close modality
  (`ConsoleState.open`/`input_set_console_open()`/`console_toggle`/
  `escape_edge`, all removed). Every keystroke flows to the panel every
  frame now; the only reserved shortcut left is F4 (STL export — a
  function key, so it can't collide with typing). Qek's WASD/Space/X/Z/M/
  Shift/C movement-and-fly bindings and its E/[/]/,/. octree-editor keys
  are gone from all three platform backends (wasm, win32, X11) — those
  letters just type into the panel now. Scroll wheel is currently
  unassigned (handler shells kept, not torn out — its natural next owner
  is Scene-panel camera zoom once real editor navigation lands, not
  something to guess at here). The `InputState` fields those bindings used
  to drive (`forward`/`back`/`left`/`right`/`jump`/`up`/`down`/
  `paint_mod`/`shift`/`crouch`/`edit_toggle`/`grid_inc`/`grid_dec`/
  `mat_inc`/`mat_dec`) still exist and still compile against `editor.c`/
  `main.c` — deliberately left inert here rather than deleted, since
  they're squarely inside the octree-editor/movement scope being decided
  in the next bullet, not this input-model change's own scope. Also
  added: a blinking block caret (`ui.c`'s `draw_panel_console`, wall-
  clock-driven ~1 Hz blink, positioned via real `font_text_width`
  measurement — the panel had no focus indicator at all before this) and
  a `>>> ` input-row prompt matching `console_submit`'s own echo prefix.
  Verified by compiling clean on all three targets; live keystroke
  verification wasn't possible (the X server in this environment has
  stayed down since earlier in this same work — see the MicroPython
  console section above), so this is compile-clean-and-reviewed, not
  click-tested, the same honest bar the rest of this Phase 1 pass has
  used when live GUI verification wasn't available.
- **Client/server model: repurposing Qek's networking layer, not deleting
  it** — a scope decision for the next Qek-cleanup pass, agreed but not
  yet executed; recorded here before the removal PRs land rather than
  only after, matching this project's own practice elsewhere. A full-
  codebase review turned up four remaining buckets of Qek-specific
  material:
  - **(A) Gameplay simulation — remove.** `physics.c`'s combat/movement
    sim (rockets, splash damage, knockback, Quake-style air-accel/
    friction/`STOP_SPEED`, respawn timers), the `Player`/`Rocket`/
    `GameState` types, `renderer_draw_players`/`renderer_draw_rockets`,
    `hp`/`god`/`alive`, and the HUD (`update_hud`, `www/index.html`'s
    `#hud`/`#crosshair`). Nothing in Phi's own roadmap uses any of it —
    Phase 2's Bullet physics is a wholly separate system, sharing no code.
  - **(C) The octree voxel world — remove.** `octree.c`/
    `octree_render.c`/`cmap.c`/`octree_stl.c`/`editor.c` (Sauerbraten-
    style voxel carve/paint editing), STL export, and
    `PKT_EDIT_REGION`/`MAP_FULL`/`SAVE_MAP`/`LOAD_MAP`/`NEW_MAP`/
    `LIST_MAPS`. Phi's mesh editor is half-edge/glTF-based (see "Editable
    mesh structure vs. glTF" above) and shares no code or data model with
    the octree. This removes the only scene content that currently exists
    besides the Phase 1 test cube — the world is expected to render
    visually empty for a stretch until real scene-authoring content (see
    "Asset tracking and the Asset Browser panel" earlier in this phase)
    replaces it. An accepted consequence of the decision, not a surprise
    to be discovered later.
  - **(D) Cheap, low-risk cleanup — remove/rename.** The now-inert
    `InputState` fields from the bullet above, `KEY_FORWARD`/etc. flags
    and `input_get_key_flags`, `pointer_locked`/FPS mouse-look
    (`sensitivity`, yaw/pitch clamping, cursor-warp-to-center — an editor
    wants orbit/pan camera control, not FPS look, and pointer lock
    currently has no way to even engage on native/win32 in the first
    place, see the Native UI System mouse-capture-bug note above, so
    nothing regresses by removing the machinery behind it). Also a
    naming pass: `ConsoleState`/`console_*`/`PKT_CONSOLE_MSG` still say
    "console" throughout the codebase even though the panel is
    conceptually Phi's Python panel now, not Qek's dev console — worth
    reconciling once the removal settles rather than mid-flight.
  - **(B) Networking — keep and repurpose, do not delete.** `net.c`/
    `net.h`, `ws_client_native.c`/`ws_client_win32.c`, `server/server.py`,
    and the underlying wire-protocol transport. The client/server shape
    itself — a client connects to a server that tracks authoritative
    asset and game state — is exactly the transport the Core Pattern's
    client-authored/server-persisted loop and Claude's in-editor
    participation (see "Where AI fits" above) need: general multiplayer
    game authoring and generic multiplayer gameplay for whatever a user
    (or set of users) defines, not just Qek's FPS state sync. What
    actually gets removed from B is just the Qek-specific packet
    payloads riding on top of it once A and C are gone
    (`PKT_SPAWN_ROCKET`/`EXPLODE`/`DAMAGE`/`OBITUARY`/`FIRE` for A;
    `PKT_EDIT_REGION`/`MAP_FULL`/etc. for C, already listed there) — the
    connection/transport/persistence machinery itself stays and becomes
    the foundation the asset-tracking system and future scene/gameplay
    sync build on.

**A/C/D removed, B repurposed — landed.** The plan above is now executed,
not just recorded. `octree.c`/`.h`, `octree_stl.c`/`.h`, `cmap.c`/`.h`,
`editor.c`/`.h`, and `physics.c`/`.h` are deleted outright. `octree_render.c`/
`.h` survives under its old filename, but is now a plain
generic `RenderMesh` module — `mesh_rebuild`/`mesh_push_vertex`/
`mesh_push_quad`/`neighbor_solid`/`emit_node_faces` (the octree-flattening
half) are gone, `mesh_create`/`destroy`/`upload`/`upload_stride`/`draw` (the
half MeshObject's own rendering already depended on) stay. `Vec3f`/`Vec3i`
moved to a new `client/vec3.h` — they'd lived in `octree.h` purely by
historical accident and are used throughout the codebase independent of any
octree. `renderer_set_camera` no longer takes a `Player*`; it takes a raw
`Vec3f eye, float yaw, float pitch`, since there's no more `Player` to read
those from — `main.c` now sets a single fixed vantage point once at startup
and never touches it again. That's the one honest gap this pass leaves open:
there is no real editor camera navigation (orbit/pan/zoom/fly) yet, just a
fixed "look at roughly the right place" default — not built here, not
silently pretended to exist either. `net.h`'s packet set is down to
`PKT_HELLO` and `PKT_CONSOLE_MSG`; `server.py` lost `Vec3`/`Player`/`Rocket`/
`GameWorld` and its 20Hz tick thread entirely and now just accepts a
connection, assigns an id, and relays; `mapdata.py` (octree.c's Python port,
100% voxel-specific) is deleted. `www/index.html` lost the `#hud`/
`#crosshair`/`#editor-status` divs, the WASD/octree-editor controls table,
and the pointer-lock JS glue (`input_set_pointer_locked` no longer exists on
the C side, so nothing calls it anymore).

Verification: native, wasm, and win32 all link cleanly (win32 via the usual
WSL-interop MinGW-w64 toolchain), zero warnings under `-Wall -Wextra` beyond
two pre-existing ones unrelated to this change (a `strncpy` truncation
warning in `net.c`/`console.c`, a sign-compare warning in vendored nanosvg).
The three standalone no-GL harnesses (`mesh_edit_test`, `fracture_test`,
`mp_console_test`) all still pass — expected, since none of that code path
touches anything A/C/D removed, but confirmed rather than assumed. Live
native execution could not be verified this pass: `phi_native` hung, and a
`gdb` backtrace on the stuck process showed it blocked inside `XOpenDisplay()`
itself, before `main()`'s first line of application code runs — a sandbox
X11-connection issue, not a regression from this change (same category of
environment flakiness already noted earlier in this phase's own history).
**Real-browser wasm verification passed**: loaded in an actual browser
against the rewritten `server.py`, renders, zero JS console errors.

**The deferred `ConsoleState`-style naming pass also landed**, once the
removal above settled (as planned). `ConsoleState` → `PyConsoleState`;
`console_init`/`console_update`/`console_append`/`console_submit` →
`pyconsole_init`/`pyconsole_update`/`pyconsole_append`/`pyconsole_submit`;
the type-switcher's panel label is now "Python Console" instead of bare
"Console" (matches how e.g. Blender itself labels the same kind of panel —
"Console" was never wrong terminology on its own, the actual problem was
a reader mistaking it for Qek's old backquote-toggled cheat-command
console, which "Python Console" forecloses without needing an unrelated
new word). Deliberately left alone: the `console.c`/`console.h` filenames
(the module's own doc comment already says plainly what it is; renaming
files buys no reader clarity beyond what the symbol rename already gives,
for real Makefile/`#include` churn risk), the `PANEL_CONSOLE` enum tag
(already scoped under `PanelType`, reads fine as-is), and `PKT_CONSOLE_MSG`
(a wire-protocol constant whose name must stay documentation-only anyway,
already commented as "the client's Python panel log", and renaming it
would touch `server.py`'s matching constant for zero behavioral or
readability gain).

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

#### Blender-style area resize/split/join, 2026-08-10

The "Blender's recursive area-split model, not Zenith's simpler fixed-slot
dock, deliberately" design goal stated at the top of this section (and in
`ui.c`'s own header comment since the very first version) had never
actually been built beyond the split *tree data structure* itself — the
default layout was computed once at startup and then fixed: no drag-to-
resize, no way to split an area into two, no way to join two back
together. All three now exist.

**Resize** is the real gesture, not an approximation: press-and-drag the
border between two split children (a `AREA_BORDER_HIT_PX`-wide hit strip
either side of it), and the split fraction tracks the mouse continuously.
Architecturally this reuses the exact shape the translate gizmo's own
drag already established — recompute from the absolute mouse position
every frame rather than accumulating a delta (immune to a missed mouse-
move event), start on a discrete click, continue via a per-frame call
from `main.c`'s `main_loop` (`ui_update_area_drag`, called right next to
the gizmo's own per-frame drag update) — because that pattern was already
proven correct once in this codebase, not reinvented. A thin accent line
highlights the border on hover too (`ui_on_mouse_move`, which existed as
a declared-but-never-called stub before this — nothing needed live hover
state until this did). No OS cursor-shape change (a resize-arrow icon):
this codebase has no cursor-shape API wired up on any platform yet, and
that's flagged rather than silently assumed — the accent line is the
whole affordance for now.

**Split (create) and join (delete)** are a menu-driven equivalent of
Blender's own corner-drag gesture, not a pixel-for-pixel replication of
it — a deliberate scope tradeoff, since faithfully replicating corner hit
zones, direction detection, and a live drag-preview line is real
additional interaction-design work with its own edge cases, and Blender
itself also exposes the identical operations through a plain right-click
"Area" menu as an alternative to dragging, so this isn't inventing a
non-Blender interaction, just picking the simpler of two real ones.
Right-clicking a panel's own type-switcher icon (mutually exclusive with
left-click's type dropdown on the same icon) opens that menu: Split
Horizontal, Split Vertical, and Join with Sibling (hidden, not just
disabled, when the target has no parent — i.e. it's the sole area filling
the whole window — since there is nothing to join it with).

The tree surgery itself mutates a target `Area` **in place** (leaf
becomes a split with two new leaf children; a split collapses back into
a leaf) rather than splicing a new node into a parent — deliberately,
since `Area` has no parent pointer (nothing in this codebase needed one
before this), so in-place mutation means every existing raw `Area*`
anywhere, `g_ui.root` included, stays valid across a split/join with no
pointer fixup needed. A freshly split area starts as a copy of whatever
you split (matches Blender's own default); a join keeps whichever side
you actually right-clicked, since a menu click can't express Blender's
own "which direction did you drag" disambiguation.

**Extracted into `client/area_tree.c`/`.h`**, a new module with zero GL
dependency — deliberately split out of `ui.c` (which is GL/rendering-
heavy throughout) so this logic could be unit-tested standalone the same
way `mesh_edit.c`'s topology mutations already are, rather than only
being reachable through a live window this sandbox can't open. `ui.c`
keeps the interactive/visual side (drag state, hover highlighting, the
Area menu itself) and calls into `area_tree.c` for the actual tree edits.

Verified: `client/area_tree_test_main.c` (`make area_tree_test`, built
with AddressSanitizer since this module does its own `malloc`/`free` tree
surgery) — 21 checks covering split producing two leaves of the original
panel type, `find_parent` including the "root has no parent" case, border
hit-testing against a hand-laid-out tree (exact position, within the hit
tolerance, and correctly missing both outside the split's own axis-
perpendicular range and far from the border), resize-to-mouse including
both clamp directions, a **nested** split resolving to the correct inner
border instead of the outer one, join collapsing a split back to a leaf
with the right-clicked side's panel type and cleared child pointers, join
on a parentless root being a safe no-op, and `area_tree_free` on both a
real multi-level subtree and `NULL` — all pass clean under ASan (no leak/
use-after-free/double-free reports). All three build targets (native/
wasm/win32) still link clean. Live click-through (actually dragging a
border, opening the Area menu, watching a split appear) is still not
verified in this sandbox, same `XOpenDisplay()` limitation as everything
else in this session needing a real window.

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

**Status: a real, verified first slice landed, 2026-08-10** — deliberately
scoped down from the full sketch above, with every scope cut documented
below rather than silently assumed away. Not the `phi.h` C API surface
from Phase 5 (`phi.raycast`, `phi.spawn`, `phi.mesh_subdivide`, etc.) —
that's still an unimplemented design sketch, out of scope here.

**`PhiProp` (`client/phi_prop.h`/`.c`)**: the DNA/RNA descriptor exactly
as sketched (`identifier`/`display_name`/`type`/`range`/`offset`),
offset-based (`offsetof()` at registration time, `client/
phi_prop_registry.c`) rather than getter/setter-function-pointer-based —
the common case (a plain float/vec3/bool field at a fixed struct offset)
needs zero per-property C code, matching Blender's own actual DNA/RNA
approach, not just its name. Registered this pass: `MeshObject.position`/
`is_static`, `HEFace.base_color`/`metallic`/`roughness`/`emission`
(`metallic`/`roughness` clamp to `[0,1]` on set, matching
`halfedge_set_face_material`'s existing behavior exactly; `base_color`/
`emission` stay unclamped for the same reason `emission` already did —
bloom needs values above 1.0). `ui.c`'s Properties panel now reads
through this registry generically (a loop over `PhiPropGroup.props`, not
a hand-written line per field) instead of hardcoding each field — real
single-source-of-truth, not just a doc claim. Still read-only *from the C
UI* this pass (a click-to-edit numeric widget is real, additional
interaction-design work on top of what's here, not just plumbing) —
editing is fully real, just from Python (`phi.prop_set`) rather than a
UI widget yet.

**MicroPython binding (`mp_port.c`, extended)**: `phi.prop_get(target,
identifier)` / `phi.prop_set(target, identifier, value)`, where `target`
is `"object"` (the one test-object slot, see `main.c`) or `"face"` (the
last ray-picked `HEFace`, `g_edit_face`) — a deliberate simplification of
`ctx.prop(obj, name)`'s object-handle sketch: there's no general Python
object-wrapper system yet (nothing in this engine hands Python a live
reference to an arbitrary C struct), so this pass exposes the *specific*
two live targets that actually exist right now by name, honestly, rather
than half-building a general handle system. `phi_mp_register_targets()`
(called once from `main()`) hands `mp_port.c` the live pointers to read
through — `test_obj_loaded`/`edit_face` are passed *by address*, not by
value, since they change every frame and registration happens once.
Unknown target/identifier, or a target that isn't currently resolvable
(e.g. `"face"` with nothing selected), raises a real Python `ValueError`
rather than returning a wrong/stale value silently.

**`@phi.panel`**: the exact class-decorator pattern already prototyped
and confirmed working against real MicroPython in `mp_test_main.c` step
4 (Phase 5, months earlier) — reused verbatim as the bootstrap script
`phi_mp_init` now installs, not reinvented. New: registration is captured
*eagerly*, the moment a class's decorator actually runs (a native
callback the decorator invokes, `_native_panel_registered`), which
instantiates and caches the instance right then rather than C ever
needing to introspect Python's `_panel_registry` dict from outside it —
simpler than walking a MicroPython dict's internal map, and it's the
natural point to instantiate anyway. `phi_mp_draw_panel(index, ...)`
calls the cached instance's `draw(ctx)` (nlr-protected, same exception-
safety convention every MicroPython entry point in this file already
uses) and expects a **plain list of strings back, one per text row** —
not the interactive `ctx.separator()`/`ctx.button()`/`phi.emit()` widget
calls the original sketch shows. Building real Python-driven immediate-
mode widgets (a shared layout cursor across multiple calls, button click-
state round-tripping back into Python, an event bus) is genuinely more
interaction-design work on top of everything above, not just plumbing —
scoped out of this pass and flagged here rather than silently
under-delivered. A new `PANEL_PYTHON` panel type (`ui.c`) renders
whichever panel is registered first (`index 0` — picking *which*
registered panel to show isn't built either, a known simplification) by
calling `phi_mp_draw_panel` directly from its own draw function every
frame and drawing each returned string as a row; a raised `draw()` shows
an inline error instead of crashing the panel (or the process).

**A real regression this pass caught its own build for**: `mp_port.c`
gained a hard link dependency on `phi_prop.c`/`phi_prop_registry.c`, but
four *pre-existing* Makefile targets that already linked `mp_port.c`
(`mp_test`, `mp_test_win32`, `mp_test_wasm` via `MP_TEST_SRCS`,
`mp_console_test`, `mp_stress`) didn't list them — broke at link time
(`undefined reference to 'g_phi_prop_mesh_object'`) the moment those
targets were rebuilt, caught immediately by this session's own "rebuild
every standalone harness before calling a chunk done" discipline, not
discovered later. Fixed by adding both files to each affected `_SRCS`
list; re-verified all previously-passing harnesses (`mp_test`, `mp_stress`)
still pass with real behavior, not just that they link again.

Verified: `client/mp_prop_panel_test_main.c` (new, `make
mp_prop_panel_test`) drives the real embedded interpreter end to end —
`phi.prop_get`/`phi.prop_set` reading and writing a real `MeshObject`'s
`position` and a real `HEFace`'s `metallic` (including the range clamp
applying when set *from Python*, not just from C callers), an unknown
identifier and an unresolvable target each raising a catchable
`ValueError` (process still alive afterward, checked explicitly), a
registered panel's `draw(ctx)` calling back into `ctx.prop_get` and
returning real values reflecting whatever C-side state was set moments
earlier from a *different* Python call, `self.n` instance state
persisting correctly across two separate `phi_mp_draw_panel` calls (not
re-initialized each time — the exact property `mp_test_main.c` first
proved for this pattern, now proven again for the real cached-instance
path), and a panel whose `draw()` raises reporting failure (`-1`) with
the traceback actually captured, without taking any other registered
panel down with it. All checks pass against the real interpreter, not a
mock. Native/wasm/win32 all still link clean; every other standalone
harness (`mesh_edit_test`/`fracture_test`/`area_tree_test`/
`phi_prop_test`/`asset_protocol_test`/`mp_test`/`mp_console_test`/
`mp_stress`) still passes. Live GUI verification (typing `@phi.panel(...)`
into the real Console, watching the Python Panel render it) is still not
possible in this sandbox, same `XOpenDisplay()` limitation as everything
else this session needing a real window.

#### Chat panel: live Anthropic tool-use introspection, 2026-08-10

**Status: real, verified end to end, including real tool-call CONTENT
against a real model response** (confirmed 2026-08-10 once a funded
account key was swapped in — see the verification note below for the
exact run). This is the first real implementation of "Where AI fits"'s
"Claude runs *inside* the editor" framing (top of this document), scoped
down deliberately: real chat + real tool-calling introspection of a live
running client, not yet Claude *authoring edits* through the
client-authored/server-persisted
loop that section describes as the eventual full reach.

**Client**: the Chat panel (`draw_panel_chat`, `client/chat.h`/`.c`) went
from a static "Not connected to an LLM yet" placeholder to a real text
field. Unlike the Python Console's deliberate "always-focused, no
toggle" design, a chat box is something you click into, the same way a
real chat app works — `ChatFocus` is a new, third contender for the
keyboard alongside `AssetBrowserFocus` and the Console, arbitrated by the
same single per-frame if/else-if/else chain in `main.c` (Asset Browser
fields first, then Chat, then Console as the unconditional fallback),
with click-to-focus/click-away-blur mirroring the Asset Browser fields'
convention exactly (`ui_on_mouse_button`). Sending reuses the
established "UI raises intent, main.c executes" one-shot-flag shape
(`send_requested`/`pending_send`) rather than chat.c reaching into net.c
directly.

**Wire protocol** (`client/net.h`, mirrored in `server.py`): `PKT_CHAT_MSG`
(C→S) / `PKT_CHAT_REPLY` (S→C) for the message itself, plus
`PKT_SCENE_STATE_REQUEST` (S→C) / `PKT_SCENE_STATE_REPLY` (C→S,
`req_id`-tagged) for live introspection — the server asking the
*specific connected client* that sent a chat message to report its own
engine state, since per this project's client-authored architecture the
authoritative live `MeshObject`/physics-world state lives in the client
process, not the server. `main.c`'s `scene_state_handler` hand-builds a
small JSON payload (position/orientation/is_static, vertex/face counts,
physics body presence + velocity, the ray-picked face's PBR material if
any) from its own real globals — no JSON library anywhere in this C
codebase, deliberately not needed since every field here is a number/bool
or one small fixed nested object, never free text requiring escaping.

**Server** (`server/anthropic_client.py`, new): a real Anthropic Messages
API client and tool-use loop written against plain `urllib.request` —
**no `anthropic` package**, matching `server.py`'s own hard "no
third-party dependencies" constraint stated in its module docstring; TLS
is stdlib (the `ssl` module), nothing extra to install. Reads
`ANTHROPIC_API_KEY` from the server's own environment only, matching this
document's Hard Architectural Decision that the key never ships to a
client — chosen over a gitignored config file when this was scoped
(`AskUserQuestion` at the time), user's call. `run_tool_loop` drives a
real multi-round conversation (capped at `MAX_TOOL_ROUNDS = 6` so a
misbehaving loop can't hang a chat-handling thread forever): two tools
are registered per chat message, `get_asset_list` (server-local, answers
immediately from `AssetDB`, no client round trip) and `get_scene_state`
(the interesting one — blocks on a `threading.Event` until the specific
client that sent the message answers its `PKT_SCENE_STATE_REQUEST`, or a
5s timeout). Each `PKT_CHAT_MSG` spawns its own thread
(`Client._handle_chat`) specifically so a `get_scene_state` round trip
back to the SAME connection doesn't deadlock against that connection's
own read loop, which is what would have to deliver the reply.

**Verified for real, live, against the actual running server + actual
network egress to `api.anthropic.com`** (`server/test_chat_protocol.py`,
new — reuses `test_asset_protocol.py`'s independent `WsTestClient` rather
than `client/ws_client_native.c`, same reasoning that file already
documents, extended here to also play the role a real windowed client
would for `get_scene_state` by answering `PKT_SCENE_STATE_REQUEST` with a
scripted JSON payload): the full wire round trip works end to end —
`PKT_CHAT_MSG` in, server spawns a thread, makes a REAL HTTPS POST to
`api.anthropic.com/v1/messages`, and `PKT_CHAT_REPLY` comes back with
real content. **The first key this ran against had no API credits** —
every call came back a real HTTP 400, `"Your credit balance is too low
to access the Anthropic API"` — a real, live-verified account/billing
state, not a bug in this code, and honestly informative rather than a
failure to hide: it proved the entire plumbing (WS framing, threading,
the `AnthropicError` catch-and-report path landing safely as a
`[chat error] ...` reply instead of crashing the connection or the
server) was real and correct, before the tool-call CONTENT itself had
been observed at all. `test_chat_protocol.py`'s own first draft also had
a real bug, caught and fixed in the same pass: it printed "RESULT: PASS"
unconditionally regardless of the individual `check()` results, fixed to
actually gate on `test_asset_protocol._fail`.

**Re-run against a funded key (2026-08-10, server restarted with the new
key in its environment, never written to any file or logged) — all
three checks now pass for real**: a plain "reply with exactly PONG" got
back `'PONG'` with no tool call; asked to call `get_scene_state` and
report only the `vert_count` it got back, the model actually issued the
tool call (a real `PKT_SCENE_STATE_REQUEST` round-tripped to the test
harness playing the client's role) and answered `'4242'` — the exact
canned value the harness sent back, not a guess; asked to call
`get_asset_list` and report the count, it answered `'3'`, matching the
real asset DB's actual row count at the time. `get_scene_state` against
a real windowed client (not the test harness standing in for one) is
still unverified in this sandbox, same `XOpenDisplay()` limitation as
everything else this session needing a real window — the server-side
half and the wire protocol are proven for real either way.

#### Chat panel follow-ups: scrollbars everywhere, word-wrap, @llm addressing, 2026-08-10

Three rounds of real user-reported bugs/asks against the landed Chat
panel, fixed the same session:

**Bug: scrollback clipped the input box.** The gap between the newest
scrollback line and the input box was only 8px; real glyph height meant
its bottom edge bled into the input field. Widened to 20px, matching
`draw_panel_console`'s own already-correct gap.

**Bug/ask: no scrollbar anywhere.** Real mouse-wheel input had never
been wired to anything on any of the three platforms (`input.c` had a
dead callback shell on each, explicitly reserved for "a future
Scene-panel zoom" that hadn't landed yet) — `InputState::scroll_delta`
is now real, and `ui_on_mouse_wheel` routes it by HOVER (`area_tree_
find_leaf_at`, a new area-tree lookup, ASan-verified via `area_tree_
test`), not click-focus, since you shouldn't have to click into a panel
to scroll it. Extended to every panel with real overflow risk, not just
Chat: Console's scrollback, the Asset Browser's item list, and the
Python Panel's returned rows. Two genuinely different scroll-anchor
conventions exist (`ui_draw_scrollbar`'s `anchor_bottom` flag) — Chat/
Console are bottom-anchored logs (offset 0 = pinned to the newest line),
Asset Browser/Python Panel are ordinary top-anchored lists (offset 0 =
scrolled to the top) — mirror-image thumb-position formulas, not shared
math. Each panel's own `draw_panel_*` re-clamps its offset every frame
(not just on a wheel event), since the underlying count can shrink out
from under a stale offset for reasons that have nothing to do with
scrolling (a delete, a new reply, a resize).

**Ask: real word-wrap + bold usernames + @llm addressing.** Chat's
scrollback storage changed from pre-broken fixed lines to logical
(unwrapped) `ChatLogLine` entries, each tagged `is_msg_start`; wrapping
to the panel's actual pixel width happens fresh every frame
(`chat_build_visual_rows` + a real greedy word-wrapper, `wrap_text`) —
the only way it can genuinely be responsive to a live resize, since a
cached pre-wrapped version would go stale the moment the panel or window
changes size. Margins are symmetric: text starts `UI_PANEL_PAD` in from
the left, so it wraps that same distance from the right edge. Only a
`ChatLogLine` marked `is_msg_start` ever has its leading `"Name: "`
parsed out and drawn in `g_ui.font_bold` (colon and message body stay
normal weight) — a real body line that happens to contain `": "` (e.g.
`"Score: 42"`) is never mistaken for a username line, since only the
first wrapped row of a genuine message-start entry is eligible at all.

The model's own username is now a real display name
(`anthropic_client.MODEL_DISPLAY_NAMES`, e.g. `"Sonnet 5"` for
`claude-sonnet-5`) rather than the raw API model id, prefixed onto every
real reply server-side; error replies get `"System: "` instead so they
render with the same bold-prefix convention without being mistaken for
something the model said. The model is now only ever invoked when a
message contains `@llm` (`server.py`'s `PKT_CHAT_MSG` handler silently
does not forward anything else to the model at all, matching a
Slack-bot-style @mention convention, not an always-on chatbot) — told to
the model via its own system prompt, and told to the human user via
`chat_init`'s preamble text, which explains the convention and ends with
two blank scrollback lines before the user's own first message.
`main.c` only sets `waiting_for_reply` (the "Claude is thinking..."
indicator) when the outgoing text contains `@llm`, so the indicator
never spins forever on a reply that server.py was never going to send.

The Chat input box's text is drawn 2px lower than the Asset Browser's
search/name/tags fields, which share the same `draw_text_field` — a new
`text_y_offset` parameter lets each caller choose, rather than moving
the shared default and shifting fields that were never asked to move.

Verified: `area_tree_test` (+6 checks for `area_tree_find_leaf_at`, ASan
clean), a real live re-run of `server/test_chat_protocol.py` against the
funded key confirming `@llm` gating (a non-`@llm` message gets truly no
reply), the `"Sonnet 5: "` prefix, and that tool-calling still works with
the gate in place — all pass. Native/wasm/win32 all rebuilt clean from a
forced clean state as the final check (confirmed via source-vs-binary
mtime comparison, not just a "make said 0" trust). Live GUI verification
of the actual wrap/scrollbar/bold rendering is still not possible in
this sandbox, same `XOpenDisplay()` limitation as everything else this
session needing a real window.

#### Scene panel: reference grid + Blender-style MMB orbit/pan, 2026-08-10

**A real ground-aligned reference grid** (`renderer_draw_grid`, new) —
deliberately built from thin SOLID quads, not `GL_LINES`: this codebase
already has a real, documented precedent (`renderer_draw_wire_box`'s own
comment, and `gizmo.c`'s note on why it switched away from wireframe) for
1-pixel lines genuinely rasterizing in the geometry pass and still
vanishing from the final composited frame, suppressed by TAA/FXAA. The
grid doesn't repeat that bug. Centered/sized to match
`g_ground_phys_body` exactly (top surface y=50), so the one visual
reference plane in the scene is the plane the test object actually lands
on, not an arbitrary y=0. Built once and cached in a static VBO (unlike
`renderer_draw_wire_box`'s per-call dynamic buffer) since it's always the
same fixed geometry every frame.

**A real orbit camera.** The editor camera was a bare eye-position +
yaw/pitch fly camera; Blender-style MMB-drag orbit needs a fixed point to
orbit *around*, which that model has nowhere to keep. Refactored to
pivot + distance + yaw/pitch, with `g_cam_pos` now a derived value
(`cam_recompute_pos`, the one place any of zoom/orbit/pan ever actually
reaches the renderer) rather than an independent source of truth. Wheel-
zoom (landed earlier this session) now moves `distance` instead of
freely dollying, so it stays consistent with orbit's fixed pivot instead
of letting the pivot drift away from what's on screen. `compute_scene_ray`
(object picking's own ray construction) was quietly duplicating the same
yaw/pitch-to-basis formula inline; pulled out into a shared `cam_basis`
so picking, zoom, orbit, and pan all provably use the identical math, not
four copies that happen to agree today.

**MMB-drag orbits, Ctrl+MMB-drag pans** — mode is decided once, from
whether Ctrl was held at the exact moment the drag started (matching
Blender's own "modifier state at click time" behavior, not live-toggled
mid-drag), and both are computed fresh each frame from the drag's START
yaw/pitch/pivot + the total pixel delta since then — the same anti-drift
"recompute from the absolute mouse position, never accumulate
incrementally" pattern this session's own area-border resize drag and
the gizmo's drag already established. A drag only STARTS when the press
lands inside the Scene panel's own rect (`ui_get_scene_rect`), then
continues regardless of where the cursor goes afterward, same
click-must-start-inside/continues-anywhere convention as that resize
drag. Middle mouse button + Ctrl tracking is new to `InputState` across
all three platforms (`input.c`) — wiring this up caught a real bug in
`phi_platform_win32.c`: its `WndProc` explicitly whitelists which Win32
messages get forwarded into `input.c`, and `WM_MBUTTONDOWN`/`WM_MBUTTONUP`
weren't in that list, so the C-side handling would have silently never
received a single middle-click event on Windows until this was added.

**Bug: middle-click didn't register in the browser.** Emscripten's own
`mousedown`/`mouseup` callbacks already return `EM_TRUE` (the
html5.h convention for "preventDefault this"), which should suppress the
browser's native middle-click autoscroll cursor on its own — but that's
been unreliable in practice across browsers, since some fire a separate
`auxclick` event for the middle button that a plain `mousedown`
preventDefault doesn't necessarily cover. Added an explicit JS-level
belt-and-suspenders listener in `www/index.html` (`{passive:false}`,
since a passive listener can't call `preventDefault` at all) on both
events, same spot the existing RMB-context-menu suppression already
lives.

**Bug: the transform gizmo was too big.** `GIZMO_LEN`/`GIZMO_SHAFT_R`/
`GIZMO_HANDLE_R` (`gizmo.c`) all scaled to exactly 1/4 their previous
size, kept in the same proportion to each other rather than only
shrinking the visible arrow — those three constants also define the
handle's hit-test bounds (`axis_handle_bounds`), so scaling only the
drawn geometry would have left a click target that no longer matched
what's on screen.

Verified: native/wasm/win32 all rebuilt clean from a forced clean state.
Live interactive verification of the grid's appearance, the orbit/pan
feel (sign conventions were derived from the camera-space math, not
eyeballed against a real window), and the gizmo's new size are all still
not possible in this sandbox, same `XOpenDisplay()` limitation as
everything else this session needing a real window — flagged honestly
rather than assumed correct.

**Real bug, found from a user report ("MMB still isn't working" in the
browser after the JS-level autoscroll-suppression fix above): `main.c`'s
MMB-drag-start block read `g_inp.mmb_click` but never drained it back to
0.** `lmb_click`/`rmb_click` are cleared immediately inside their own
`if` blocks a few lines below (`main.c`'s established "rising edge,
drained and cleared here" convention) — `mmb_click` was the one place
that convention wasn't followed. Left set, the drag-start block re-ran
*every single frame* for as long as the flag stayed 1 (i.e. forever after
the first press, since nothing ever cleared it), resetting
`g_cam_drag_start_x/y` to the CURRENT mouse position each time —
meaning `dx`/`dy` in `cam_orbit_from_start`/`cam_pan_from_start` were
always exactly 0, so the drag could never produce any camera movement at
all, regardless of how far the mouse actually moved. This is a better,
more specific explanation than the earlier browser-autoscroll theory
(which is still a real, worthwhile defensive fix, just not what was
actually broken here) — fixed by draining `g_inp.mmb_click = 0` the
moment it's read, matching `lmb_click`/`rmb_click` exactly.

Confirmed working live in the real browser build once fixed. Following
that, the vertical orbit direction (`cam_orbit_from_start`'s `dy` ->
`g_cam_pitch` sign) was flipped per an explicit follow-up request — the
math was correct, the feel just wasn't the one wanted.

#### Default panel heights: Properties now the taller pane, 2026-08-10

The right column's Outliner/Properties split (`ui_init`) originally gave
Outliner (top) the larger golden-ratio fraction and Properties (bottom)
the smaller one; swapped per an explicit request, top/bottom order
unchanged — Properties now gets the larger share. Since a `SPLIT_V`'s
`child[0]` height is `h * split` (see `layout_area`), giving `child[1]`
(Properties) the bigger fraction means `split` itself holds the
*smaller* one now (`1 - UI_INV_PHI`, not `UI_INV_PHI`).

#### Scene panel: grid thinned + light grey, background dark grey, 2026-08-10

Two colour/thickness tweaks per explicit request: the grid's world-space
quad width dropped from 0.3 to 0.05 (6x thinner) and recoloured to light
grey (0.65,0.65,0.65); the scene's clear/background colour
(`renderer.c`'s `s_sky_color`, previously `(0.3,0.5,0.8)` — a literal
Qek "sky blue" holdover, that project's actual sky colour, never
revisited since this codebase diverged from it) is now dark, dark grey
`(0.06,0.06,0.06)`. **The grid stays solid-quad geometry, not real
`GL_LINES`**, even though "1px thick" was the literal ask — switching to
actual 1-pixel lines would reintroduce the exact TAA/FXAA-suppression
bug `renderer_draw_wire_box`/`gizmo.c` already document hitting and
fixing earlier in this project's history; flagged explicitly rather than
silently reinterpreted. Verified: native/wasm/win32 all rebuilt clean.

#### Object Mode / Edit Mode, 2026-08-13

Blender-style mode split, per explicit request ("We kinda need to mimic
blender's ux here. I need object mode and edit mode."). Previously there
was no mode concept at all — `try_pick_object` did whole-object select
AND face-level picking unconditionally on every click, and the Scene
right-click context menu showed every row (object-level AND mesh-editing)
at once regardless of what was selected.

- **`EditorMode` enum** (`ui.h`): `EDITOR_MODE_OBJECT` / `EDITOR_MODE_EDIT`.
  `main.c` owns the actual state (`g_editor_mode`); `ui.c` only reads it
  via a new `UIRenderContext::editor_mode` field to decide what to draw/
  hit-test, the same division of labor `edit_face` already established.
- **Tab key** (new `InputState::tab_edge`, wired across wasm/win32/X11
  the same way `enter_edge`/`histup_edge` already are — none of the three
  platforms ever queue Tab into `typed_chars` in the first place, so no
  extra exclusion was needed there) toggles the mode via a shared
  `toggle_editor_mode()` helper in `main.c`. Entering Edit mode is a
  no-op (reported, not silently ignored) unless a mesh object is already
  selected, matching Blender's own rule. `g_edit_face` is Edit-mode-only
  state now — cleared on every mode transition and on every Object-mode
  pick, so it can never carry a stale face selection across a mode
  switch.
- **Picking is now mode-gated** (`main.c`'s `try_pick_object`): Object
  mode keeps the old gizmo-handle-then-whole-object-select/deselect
  behavior, minus face-level state. Edit mode restricts picking to
  face-selection within the already-selected object only — no gizmo, no
  switching which object is selected; a miss just clears the face
  selection (same as clicking empty space in Blender's Edit Mode, which
  doesn't kick you back to Object mode or deselect the object).
- **Context menu row set is now mode-dependent, and a real pre-existing
  bug got fixed along the way**: the menu's `items[]` (and, in the
  hit-test copy, a parallel `actions[]`) array was hand-duplicated
  verbatim between the draw pass (`ui_render`) and the hit-test pass
  (`ui_on_mouse_button`) — two independently-maintained copies of the
  same 11-row list that could silently drift apart. Factored into one
  `build_ctx_menu_rows(ctx, items, actions)` used by both passes. Object
  mode shows the whole-object rows plus a new "Enter Edit Mode" row (only
  when a mesh is actually selected — omitted entirely rather than
  shown-then-rejected, the same convention the Area menu's "Join with
  Sibling" `can_join` check already uses); Edit mode shows Extrude/Inset/
  Loop Cut plus "Exit Edit Mode". Both rows dispatch through the same
  `toggle_editor_mode()` the Tab key uses, via a new
  `CTX_ACTION_TOGGLE_EDIT_MODE`.
- **Mode label**: the Scene panel now draws "Object Mode"/"Edit Mode"
  top-left (same offset the Outliner/Properties panel titles already use
  past the type-switcher icon), dimmed in Object mode and accent-colored
  in Edit mode so the current mode is legible at a glance.
- Also, unrelated one-line fix bundled into this same pass: the Python
  console's opening banner no longer says "Type Python" — the panel
  already reads "Phi Python console" and has no other language it could
  possibly be, so the instruction was pure noise.

Verified: native and wasm both rebuilt clean (only pre-existing vendored-
Bullet warnings, nothing from any touched file); `area_tree_test` still
passes in full. No live GL/input verification was possible in this
sandbox (no real X display, see this doc's standing note on that
limitation) — this is compile-clean-and-reviewed, not click-tested. Win32
is no longer built from this sandbox at all now that `build.bat` lets the
user cross-check it themselves on real Windows with real MSVC.

#### Camera polish + modal Grab/Scale/Rotate transform tool, 2026-08-14

Two small camera fixes plus a real Blender-style modal transform tool,
all per explicit request.

- **MMB-orbit horizontal direction flipped** (`main.c`'s
  `cam_orbit_from_start`, `g_cam_yaw`'s `dx` term) — same "confirmed
  working, just inverted from the feel that was actually wanted"
  situation as the earlier vertical-orbit flip right above it.
- **Real bug found and fixed while leveling the Object/Edit Mode label
  with the panel type-switcher icon**: `draw_panel_scene` (`ui.c`) issues
  its mode-label `ui_text_draw` call — and, as of this pass, the new
  transform-tool HUD readout next to it — while the real GL viewport is
  still pinned to the Scene panel's own on-screen sub-rectangle (left
  that way by `gbuffer_set_viewport_offset` a few lines above), not the
  full window. Every other panel's 2D chrome runs from `draw_leaf`'s
  post-switch `glViewport(0, 0, screen_w, screen_h)` reset, which happens
  AFTER `draw_panel_scene` returns — too late for anything drawn inside
  it. The symptom was exactly what got reported: since text position/
  size is computed in full-window pixel space but was being rasterized
  through the smaller Scene-panel viewport instead, the label's on-screen
  size scaled with the Scene panel's own on-screen dimensions as the user
  dragged area borders, rather than staying a fixed pixel size — not a
  wrong offset constant (the x/y math already matched Outliner/
  Properties exactly). Fixed with a second, earlier `glViewport` reset
  right before the first 2D draw call this function issues.
- **Modal Grab/Scale/Rotate transform tool** (new `client/transform_op.h/
  .c`, ~250 lines, a self-contained mechanism module in the same role
  gizmo.c already plays for handle-click dragging): `G`/`S`/`R` start a
  modal operation on the selected MeshObject, available in BOTH Object
  and Edit mode (Edit mode still just has the one object selected --
  there's no per-vertex editing surface yet, see `g_edit_face`'s own
  comment, so "move the selected thing" means the same object either
  way). `X`/`Y`/`Z` lock the operation to a single world axis (pressing
  the already-locked axis again unlocks it, Blender's own convention);
  Escape restores the object's pre-operation position/orientation/scale
  exactly; Enter or a plain LMB click confirms; a plain RMB click also
  cancels. Rotate additionally accepts digit keys to type an exact
  degree value, overriding mouse control until Backspace clears the
  buffer back to empty.
  - **Entry is hover-gated**, the same "determined by what the mouse is
    hovering over" convention this codebase's MMB-camera-nav/scroll-
    routing already established: `G`/`S`/`R` only start a modal op while
    the cursor is over the Scene panel's own content AND a mesh object
    is selected, consuming just that one typed character out of the
    frame's queue so anything else typed the same frame (e.g. into the
    console) is undisturbed. While a modal op IS active, it owns ALL
    keyboard input -- fully drains `typed_chars`/`backspace_edge` every
    frame (even characters it doesn't itself recognize) so nothing leaks
    to the console/chat text fields underneath, and the console/chat/
    asset-browser text-routing block in `main_loop` is skipped outright
    while one is active.
  - **New `InputState::escape_edge`** (`input.h`/`.c`, wired across wasm/
    win32/X11 the same way `tab_edge` was in the previous pass) --
    resurrects the KEY, not the old console-modality behavior removed
    earlier in this project's history (see that section's own note): this
    is scoped solely to the transform tool's cancel action.
  - **Axis-locked Grab** reuses gizmo.c's own drag-plane technique
    (construct a plane containing the axis and facing the camera, then
    ray-plane-intersect every frame) reimplemented locally in
    `transform_op.c` rather than calling into gizmo.c directly, so the
    modal op owns its own independent lifecycle instead of sharing
    gizmo.c's module-level dragging state (which is specifically about
    handle-click drags and would conflict with a keyboard-driven one).
    Free-move Grab uses the same screen-to-world-via-camera-basis
    technique `cam_pan_from_start` already uses for camera panning.
  - **Scale** is a new capability end to end -- `MeshObject` had no scale
    field at all before this pass (`meshobject.h`'s `Vec3f scale`, `{1,1,
    1}` = untouched, set explicitly at every existing spawn/test-harness
    site). `renderer.c` gained `mat4_scale` and now builds the mesh-
    object model matrix as `T*R*S` (previously just `T*R`). An
    exponential-in-pixels multiplicative factor drives both free
    (uniform) and axis-locked scale, chosen over Blender's own screen-
    space-distance-from-center ratio specifically so scale can never
    cross zero/go negative regardless of drag distance -- a deliberate
    simplification, same "real but not Blender-grade polish" bar as the
    fixed-distance extrude offset. **Known pre-existing gap, not
    introduced by this pass**: the PBR vertex shader passes `a_normal`
    straight through untransformed (`v_normal = a_normal;`, object-space,
    not even rotated) -- lighting on a rotated OR now non-uniformly-
    scaled object is not physically correct. This already applied to
    Bullet-tumbled physics objects before Scale ever existed; flagged
    honestly rather than fixed here, since fixing it (a real normal-
    matrix, inverse-transpose for correctness under non-uniform scale) is
    out of scope for what was actually asked.
  - **Rotate** composes world-space rotations via new file-static
    `quat_mul`/`quat_from_axis_angle` helpers in `transform_op.c`
    (verified against `meshobject.h`'s `quat_to_mat4` convention: a
    90-degree rotation composed with itself via `quat_mul` gives the
    expected 180-degree result, before use). No axis lock defaults to
    rotating around the camera's forward vector (an approximation of
    Blender's real trackball-around-view-axis rotation, simpler than a
    true screen-space-angle-around-center computation but directionally
    correct). Typed-degree entry has no negative-sign support in this
    pass (would need a new dash-key edge across three platforms plus more
    parsing) -- a real, scoped-out gap, not an oversight.
  - **HUD readout**: `transform_op_hud_text` formats a short string
    ("Grab X", "Rotate Z: 45 (deg)") that `main.c` hands to `ui.c` via a
    new `UIRenderContext::xform_hud` field, drawn next to the Object/Edit
    Mode label -- same "main.c formats, ui.c just draws" split the mode
    label itself already established.

Verified: native and wasm both rebuilt clean (only pre-existing vendored-
Bullet warnings, nothing from any touched file); `area_tree_test`,
`phi_prop_test`, `phi_physics_meshobject_test`, `mp_physics_test`, and
`mp_prop_panel_test` all still pass in full (the last four exercise
`MeshObject` directly, the ones most likely to notice a bad `scale`
default). No live GL/input verification was possible in this sandbox
(no real X display) -- compile-clean-and-reviewed plus this test suite,
not click-tested. Win32 remains the user's own `build.bat` responsibility,
not built from this sandbox.

#### Functional top menu bar (File/Edit/View/Help) + modal dialogs, 2026-08-14

The File/Edit/View/Help labels in `draw_menu_row` had been pure static
text since Phase 1's original chrome pass ("this proves the chrome has a
place for a real menu system to land in, not a finished menu bar") --
this pass makes them real click-to-open dropdowns, per explicit request.

- **File > Save/Load** reuse EXISTING mechanisms rather than duplicating
  them: Save is the identical "begin the Asset Browser's create flow for
  the selected MeshObject" `CTX_ACTION_SAVE_AS_ASSET` already does; Load
  sets the exact same `AssetBrowserState::load_requested`/
  `load_requested_id` fields the Asset Browser panel's own per-row Load
  button already sets (for whichever asset is currently selected there)
  -- the existing per-frame block that actually performs a load doesn't
  know or care which UI triggered it, so no new load path was written.
- **Generic modal dialog system** (`ui.c`'s `draw_modal_box`/
  `draw_modal`): centered, screen-dimming overlay, drawn dead last in
  `ui_render` so it sits above literally everything else (the top menu's
  own dropdown included) -- any click anywhere closes it, per the literal
  "(click anywhere to close)" hint drawn on it. New `ui_is_modal_open()`
  gates the G/S/R transform tool's own keyboard-driven entry (which,
  unlike mouse clicks, doesn't route through `ui_on_mouse_button` and so
  wouldn't otherwise know a modal was up) -- mouse-click picking needed
  no separate gate, since `ui_on_mouse_button` already unconditionally
  claims every click while a modal is open, same as it already did for
  the context/area menus.
- **Keyboard Shortcuts modal** (Help menu): every entry checked against a
  real `InputState` field or `typed_chars` check in `input.c`/`main.c`/
  `transform_op.c`, not written from memory of what "should" be bound --
  navigation (MMB drag/Ctrl+MMB drag/scroll), selection (LMB/RMB/Tab),
  the full G/S/R transform tool (including X/Y/Z axis-lock and Rotate's
  digit-key exact-angle entry), and text-field basics.
- **About modal**, bottom of the Help menu as requested.
- Both new dropdown-row content functions (`build_top_menu_dropdown_rows`)
  and the modal box itself follow the same "one function drives both the
  draw pass and the hit-test pass" discipline this file's context menu/
  Properties panel already established (`build_ctx_menu_rows`/
  `properties_panel_walk`), rather than risking a third copy of that
  historical items[]/actions[] drift bug.
- **Explicitly NOT built this pass**: a Preferences modal / client-
  settable Anthropic API key. Originally requested, then the user asked
  to drop it after I flagged that it would reverse this document's own
  "Anthropic API key lives server-side only" Hard Architectural Decision
  (the user's own explicit call earlier this session, made via
  `AskUserQuestion` at the time) -- surfaced rather than silently
  reversed, and dropped rather than built once flagged. The decision
  stands as originally written.

Verified: native and wasm both rebuilt clean; all 11 relevant standalone
harnesses re-verified passing (none of this touches anything they cover
directly, but the shared `ui.c`/`main.c` translation units needed a clean
recompile regardless). No live GL/input verification was possible in this
sandbox (no real X display) -- compile-clean-and-reviewed, not
click-tested. Win32 remains the user's own `build.bat` responsibility.

#### Real multi-object scene graph, 2026-08-14

Closes the single-object-slot limitation every earlier phase this session
flagged and deliberately deferred (Phase 2's fracture-activation note,
Phase 4's skinned-test-object comment, etc. -- all explicitly called this
"a genuinely separate, much larger undertaking" and scoped around it
rather than half-building it). Per explicit request: "make it so the
scene can have multiple meshes at the same time."

- **`client/scene_objects.h`/`.c`** (new): a real bounded `MeshObject`
  registry -- same fixed-array-of-slots pattern `light.c`/`fracture_
  body.c` already established (addresses never move for a live object's
  whole lifetime, no realloc, so every system that holds a `MeshObject*`
  across frames -- Properties, Python, `scene_target.c`'s resolver, the
  gizmo -- can keep doing so safely), monotonically-increasing ids
  (never reused, so a stale reference from a deleted object can't
  silently collide with a new one). `SCENE_MAX_OBJECTS = 32`.
- **`main.c`'s single `g_test_mesh_object` global is gone**, replaced
  throughout by the registry: `scene_content_cb` draws every live
  object; `try_pick_object`'s whole-object select now ray-tests every
  live object (plus every light), nearest hit wins, not "the one
  object, then lights"; a new `selected_mesh_object()` helper (scans the
  registry for whichever id matches the UI's shared selection, mirrors
  `selected_light()`'s own shape) is the one place every context-menu
  action (Extrude/Inset/Loop Cut/Fracture/Enable Physics/Save as
  Asset/Delete), `resolve_xform_target` (G/S/R, see the dated note two
  sections up -- now generalizes cleanly to "whichever object is
  selected" instead of one fixed slot), and the physics-transform-sync
  loop all resolve "the relevant object" through.
- **Loading a mesh now ADDS a new object, not replaces**: the Asset
  Browser's Load button, File > Load, and the Scene context menu's Add >
  Mesh Object row all funnel through a new `add_mesh_object_from_path`
  (renamed/generalized from the old replace-the-one-slot `load_mesh_
  object_from_path`) that allocates a fresh registry slot and selects it
  immediately. Deliberately does NOT check for/avoid overlapping
  existing geometry -- allowed to clip straight through whatever's
  already there, per explicit request ("allow the mesh to clip through
  any existing geometry and then be moved with 'g' to be separated") --
  separating overlapping objects is a manual G-grab, not an automatic
  placement search.
- **Fracture is still deliberately single-target**: `fracture_body.c`'s
  own registry stays a separate, smaller, bounded thing (its own
  established scope), but now tracks WHICH object it was activated from
  by id (`g_fracture_source_id`) rather than assuming there's only ever
  one object that could be fractured -- fracturing one object leaves
  every other live object completely unaffected (drawn/picked
  normally), and deleting the fracture source (and only the source)
  correctly clears its fragments.
- **`phi_mp_register_targets`/`scene_target_register` widened from a
  fixed `MeshObject*`+loaded-bool pair to a resolver CALLBACK**
  (`MeshObject *(*)(void)`, main.c passes `selected_mesh_object` itself)
  -- `phi.prop_get/set("object", ...)` and chat's prop-set tool now
  correctly mean "whichever object is currently selected", matching
  Blender's own `bpy.context.object` (active/selected object) model,
  not a single hardcoded slot. Every standalone test harness that
  registered these directly (`light_test`, `mp_prop_panel_test`, `mp_
  physics_test`) updated to pass an equivalent local resolver.
- **Outliner lists every live object** as its own row (same accent-
  highlight-when-selected convention the Light rows already established,
  calling `scene_object_get_all` directly rather than threading the
  whole list through `UIRenderContext` -- the same "call the registry
  directly, no context-struct plumbing needed" shape `light_get_all`
  already uses from `ui.c`). `UIRenderContext::test_obj` narrowed to mean
  specifically "whichever object is selected, or NULL" (dropped the
  separate `test_obj_loaded` bool entirely -- NULL already means exactly
  that) for Properties' single-object display and the Outliner/context-
  menu's "is a mesh selected" checks.
- **`get_scene_state`'s JSON reshaped**: was one flat set of fields for
  "the" object; now a real `objects` array (id/position/orientation/
  is_static/vert_count/face_count/has_physics/velocity per object) plus
  `selected_object_id`, so Claude (and any future tooling) can see the
  whole scene, not one slot -- a real prerequisite for the geometry-
  creation/vertex-editing tools landing in the same pass (see Phase 5's
  own dated note below).

Verified with a new standalone no-GL harness (`scene_objects_test`, `make
scene_objects_test`): add/find/get_all/count are exercised directly;
address stability is checked explicitly (adding a third object doesn't
invalidate earlier pointers -- the actual property a fixed array
provides that a realloc'd one wouldn't); delete frees the slot and the
deleted id is verified to NEVER be reused by a subsequent add (monotonic
`s_next_id`, checked against a real freed-then-reallocated sequence, not
assumed); registry-full behavior fills to exactly `SCENE_MAX_OBJECTS`
and no further. Native and wasm both rebuilt clean from a forced-clean
state; every other standalone harness in the project re-run and
re-verified passing, since `main.c`/`ui.c`/`ui.h`/`mp_port.c`/`scene_
target.c`/the `Makefile` all changed. No live GUI verification possible
in this sandbox (no real X display) -- multi-object picking/Outliner/G-
grab-to-separate are compile-clean-and-reviewed against the exact same
math the single-object versions already had, not click-tested.

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

### Status: real first slice landed 2026-08-10; convex hulls + breaking-threshold constraints landed 2026-08-14; runtime fracture-activation (bounded FractureBody registry) landed 2026-08-14 (see dated notes below) — the real multi-object scene graph this section's own earlier notes deferred as future work has since landed too (see Phase 1's "Real multi-object scene graph" dated entry, 2026-08-14); fracture itself still deliberately operates on one object at a time (its own separate, smaller registry, tracked by source id), a real scope choice, not a gap

**Correction to this section's original claim, below**: Bullet has **no
official C API** for its core engine — verified by cloning the `bullet3`
source and checking directly rather than trusting this doc's own prior
text. The `*_C_API` files that do exist upstream belong to PyBullet's
unrelated SharedMemory RPC layer, not the physics engine itself. This
project instead ships a hand-written pure-C wrapper (`client/
phi_physics.h`/`.cpp`, `extern "C"`-guarded) over Bullet's real C++
classes — `PhiPhysicsWorld`/`PhiRigidBody` opaque types,
`phi_physics_world_create/destroy/set_gravity/step`,
`phi_physics_add_box_body`, `_remove_body`, `_get_transform`/
`_set_transform`, `_apply_impulse`, `_set_linear_velocity`/
`_get_linear_velocity`. `phi_physics_world_step` internally subdivides
into up to 10 fixed 1/60s substeps, so callers (including `main.c`'s
`main_loop`) can pass a variable frame `dt` straight through.

**Vendored**: only `LinearMath` + `BulletCollision` + `BulletDynamics`
(`client/vendor/bullet3/`, 153 `.cpp`/232 `.h`, zlib-licensed) — not
`BulletSoftBody`, `Bullet3*`, or the examples tree, since only rigid-body
box collision is in scope this pass. Mixed C++ into this project's
all-C build with **plain `gcc`/`emcc`** (both dispatch `.cpp` files to
the C++ frontend automatically by extension, verified with a smoke test
before committing to this approach rather than assumed) plus `-lstdc++`
at the link step only — no need for a separate `g++`/`em++` driver.
Bullet's own vehicle/Featherstone code paths (unused by this project's
box-only integration) emit hundreds of lines of uninitialized-variable
warnings under `-Wall -Wextra`; tolerated as expected vendored-code
noise rather than restructuring the single-command build to suppress
them selectively, matching the existing precedent for vendored nanosvg.

**Collision shapes are AABB-derived boxes only** this pass
(`meshobject_local_aabb_half_extents`, computed from the real half-edge
mesh's vertex bounds) — a deliberately scoped-down first slice, not full
convex-hull generation from arbitrary meshes (explicitly deferred, real
future work, not silently dropped).

**`MeshObject` integration**: gained a `PhiRigidBody *phys_body` field
(`NULL` = no simulation for that object, replacing the old "ConvexHull
not included yet" comment this section used to carry). Reachable from
the editor via a new "Enable Physics" context-menu action
(`CTX_ACTION_ENABLE_PHYSICS`) — computes the object's AABB, creates a
box body (mass 1.0, restitution 0.3). `main.c` creates one physics world
plus a static ground box at startup, steps the world every frame, and
syncs `position`/`orientation` back from Bullet's transform whenever
`phys_body` is set; delete/replace flows call `phi_physics_remove_body`
and clear the pointer so a destroyed `MeshObject` can't leave a dangling
body in the world.

**Python API surface — real, not the sketch below**: `phi.enable_physics
(mass, restitution)`, `phi.apply_impulse(impulse, rel_pos)`,
`phi.get_velocity()`/`set_velocity(v)`, added to `mp_port.c` alongside
the DNA/RNA bindings ([[Property System (DNA/RNA analogue)]] above) —
same `nlr`-protected, `ValueError`-on-misuse convention every other
native binding in this file already uses (calling any of these before
`enable_physics`, or calling `enable_physics` twice, raises cleanly
rather than crashing or silently corrupting state; malformed argument
shapes, e.g. a 2-element vector, raise instead of reading past the
array). `phi_mp_register_targets()` gained a fourth parameter
(`PhiPhysicsWorld *`) to hand the interpreter the live world pointer,
same by-address pattern as the existing `object`/`face` targets.

**A real cross-target regression this pass caught its own build for**
(the third time this session the same pattern has bitten): extending
`mp_port.c`/`meshobject.c` with the physics link dependency broke four
other pre-existing Makefile targets that already linked those modules
(`mp_console_test`, `mp_prop_panel_test`, `mp_stress`, and `mp_test` —
the last one needing a *second*, different fix, since `meshobject.c`
transitively needs `halfedge.c`/`halfedge_gltf.c`, which `MP_TEST_SRCS`
alone had never included even before this pass) — caught by this
session's standing "rebuild every standalone harness before calling a
chunk done" discipline, not by a later report. Fixed by adding the
missing sources (and `-lstdc++`) to every affected `_SRCS` list and link
line.

Verified end to end, no layer trusted in isolation: `phi_physics_test`
(raw Bullet wrapper — real freefall matches the analytical formula,
settles at the exact geometric height, impulse/velocity round-trip);
`phi_physics_meshobject_test` (AABB computed from a real scaled cube
matches exactly, the full create/step/sync sequence `main.c` itself uses
produces a settled height within Bullet's normal collision margin);
`mp_physics_test` (all 7 sections pass against a real interpreter driving
real Bullet simulation — pre-`enable_physics` calls raise, `enable_physics`
creates a real body and a second call raises, gravity moves
`obj.position` when stepped exactly like `main_loop`, get/set_velocity
and apply_impulse round-trip with correct numbers, malformed args raise,
teardown doesn't crash). Every other standalone harness (`mesh_edit_test`/
`fracture_test`/`mp_console_test`/`area_tree_test`/`phi_prop_test`/
`mp_prop_panel_test`/`mp_stress`/`mp_test`) re-verified passing after the
Makefile fixes above, and native/wasm/win32 all rebuilt clean from a
forced clean state (not just incrementally) as the final check. Live GUI
verification (right-clicking an object, choosing Enable Physics, watching
it fall on screen) is still not possible in this sandbox, same
`XOpenDisplay()` limitation as everything else this session needing a
real window.

#### Convex hull collision shapes + breaking-threshold constraints, 2026-08-14

Closes most of this phase's remaining gap, per explicit request ("finish
Phase 2"). Researched against Blender's own real rigid-body code first
(now vendored at `blender/`, added this session specifically as
reference material to check "how does Blender do this" against before
inventing a technique) rather than guessed — `blenkernel/intern/
rigidbody.cc` and `intern/rigidbody/rb_bullet_api.cpp` were read
directly, not the addon/UI layer.

- **Convex hull bodies** (`phi_physics_add_convex_hull_body`, new in
  `phi_physics.h`/`.cpp`): builds a shape from `btConvexHullComputer`
  (already vendored, previously unused) run over the object's own real
  vertex positions, then wraps the reduced hull in a `btConvexHullShape`
  — the exact same two-step technique Blender's `RB_shape_new_convex_hull`
  uses, including its margin-embedding-with-fallback behavior (try to
  shrink the hull inward by Bullet's usual 0.04 margin; if that fails for
  a thin/degenerate shape, fall back to an exact zero-margin hull). The
  Scene context menu's "Enable Physics" action and `phi.enable_physics()`
  (`mp_port.c`) both switched from a box-from-AABB to this — Blender's
  own default collision shape for dynamic ("Active") rigid bodies too
  (confirmed by reading `rigidbody.cc`'s own body-creation defaults: box
  was only ever this project's placeholder because hulls weren't
  implemented yet). The static ground body at startup stays a box, which
  is both correct for a flat plane and matches Blender's own box/trimesh-
  for-passive-objects default.
- **Breaking-threshold fixed constraints** (`phi_physics_add_fixed_
  constraint`/`_remove_constraint`/`_constraint_is_broken`): wraps
  `btFixedConstraint` (already vendored) + `btTypedConstraint::
  setBreakingImpulseThreshold` — verified this is exactly what Blender's
  own Rigid Body Constraint "Fixed" type + "Breaking" option is built on
  (`RB_constraint_new_fixed` wraps the identical class;
  `RB_constraint_set_breaking_threshold` calls the identical setter).
  Bullet's own constraint solver checks the applied impulse against the
  threshold every solved frame and disables the constraint internally
  the moment it's exceeded (confirmed by reading vendored
  `btSequentialImpulseConstraintSolver.cpp` directly) — callers only need
  `phi_physics_constraint_is_broken` to read that flag back, no hand-
  rolled polling logic. `disableCollisionsBetweenLinkedBodies` is hard-
  coded true when adding a constraint, matching Blender's own default so
  two glued bodies don't jitter against each other's collision shapes
  while intact.
- **Fragment adjacency** (`fracture_compute_adjacency`, new in
  `fracture.h`/`.c`): given a set of Voronoi fragments (`fracture_
  voronoi`'s existing output), finds which pairs share a cut face by
  checking for coincident vertex positions — since every fragment is
  clipped from the same source mesh, the only way two fragments can share
  any geometry at all is along the exact bisector plane clipped between
  them, so a shared-vertex test is a correct, practical adjacency check
  without threading full Voronoi-cell neighbor bookkeeping through the
  clipping algorithm itself. This is the missing piece needed to connect
  adjacent fragments with breaking-threshold constraints at runtime, so a
  fractured object reads as one solid piece until an impact separates it
  — Blender's own real "shatter on impact" technique (there is no
  single-body-auto-splits-on-impact primitive in Blender core either,
  confirmed while reading the same source — fragments are simulated
  separately from frame one, connected by constraints, which is the
  model this now supports).
- **Honest scope boundary, not silently punted**: wiring all of the above
  into a live, on-screen "shoot the wall, watch it shatter" demo needs
  spawning N fragment `MeshObject`s into the scene and removing the
  original — this engine has exactly ONE object slot right now
  (`g_test_mesh_object`, see its own comment in `main.c`), not a real
  object list/scene graph. Building that is a genuinely separate,
  substantially larger undertaking (touches picking, the Outliner,
  Properties, the Python bindings, Asset Browser save — everywhere
  `ui_get_selected_object()`'s 4000+id convention is assumed to mean "the
  one object") than this pass's actual scope, so it wasn't attempted here
  rather than half-built. What Phase 2 needed — real convex hulls, real
  breaking constraints, real fragment adjacency, all verified — is done
  and sitting ready for whenever multi-object scene support lands.

Verified: `phi_physics_test` gained two new sections (convex hull body
settles at the same height an equal-size box would, proving real hull
reduction rather than a degenerate/leaking shape; a fixed constraint
stays intact under a gentle impulse and genuinely breaks under a hard
one — with the exact expected momentum-shared magnitudes worked out and
checked, not just "did the flag flip"). `fracture_test` gained an
adjacency section checked against an independently re-derived shared-
vertex test, not by calling the function under test on itself. Every
other physics/prop-touching standalone harness (`phi_physics_meshobject_
test`, `mp_physics_test`, `mp_prop_panel_test`, `mp_console_test`,
`area_tree_test`, `phi_prop_test`) re-verified passing. Native and wasm
both rebuilt clean. Win32 remains the user's own `build.bat`
responsibility, not built from this sandbox.

#### Runtime fracture activation — the actual "shatter on impact" connection, 2026-08-14

Closes Phase 2's real remaining gap ("finish Phase 2, item 3" — the
`on_impact(...)` hookup didn't exist, and collision shapes were the only
piece not yet load-bearing). Rather than building the full multi-object
scene graph the prior dated note above flagged as the "real" way to do
this (a genuinely separate, much larger undertaking — picking, Outliner,
Properties, Python addressing, Asset Browser save all currently assume
exactly one object slot), this took the same **bounded-registry**
approach `light.c`'s `PhiLight` array already established: a new
`client/fracture_body.h`/`.c` module with its own small, fixed-capacity
`PHI_MAX_FRACTURE_BODIES` (64) array of real `MeshObject`s, each with its
own convex-hull `PhiRigidBody`.

- **`fracture_body_activate(...)`**: runs the existing `fracture_voronoi`
  against the selected object's own half-edge mesh, spawns each non-empty
  fragment as a real `MeshObject` (render mesh built directly, sized to
  the fragment's own vertex count rather than `mesh_create()`'s much
  larger default capacity) with a convex-hull body at the source
  object's own transform, computes adjacency (`fracture_compute_
  adjacency`, landed earlier the same day) and glues every adjacent pair
  with a real breaking-threshold fixed constraint at that shared
  transform — correct, not just convenient, since every fragment's own
  local-space vertex offsets are what place it correctly within that
  shared frame.
- **Reachable from the existing Fracture context-menu action**
  (`CTX_ACTION_FRACTURE` in `main.c`): now does both the pre-existing
  save-fragments-to-disk behavior AND a real `fracture_body_activate`
  call (`n_fragments=8`, `breaking_threshold=200.0f` — reasoned from the
  same order of magnitude Blender's own default constraint breaking
  thresholds use, but **not live-tuned against a real on-screen impact
  in this sandbox**, flagged honestly rather than presented as a
  verified-good value). On success the original mesh is hidden from
  rendering/picking/gizmo (`g_fracture_active` flag) rather than deleted,
  so nothing is lost if activation is later undone by reload/delete.
- **`fracture_body_sync_and_draw_all(Renderer *)`**: called once per
  frame from the Scene panel's content callback, after the physics step
  — syncs each live fragment's transform from its own `PhiRigidBody`
  (same "sync FROM the simulated body" pattern `main_loop` already uses
  for the single test object) then draws it through the existing,
  unmodified `renderer_draw_mesh_object`.
- **Fragments are explicitly NOT first-class objects**: not selectable,
  not in the Outliner, not Python-addressable by id — the same scope
  boundary `light.c`'s registry already draws, documented in `fracture_
  body.h`'s own header comment. `fracture_body_clear` (called on
  delete/reload of the source object) removes every live fragment body
  and constraint and empties the registry; safe to call with nothing
  active.

Verified with a new standalone no-GL harness (`fracture_body_test`,
`make fracture_body_test`): defensive cases (NULL source mesh /
`n_fragments < 1` spawn nothing rather than crashing), real activation
(fragments spawn at exactly the source object's own transform, not the
origin), a gentle impulse well under the breaking threshold keeps the
whole cluster glued together (measured max pairwise fragment spread:
0.000), and a hard impulse well over threshold genuinely breaks at least
one fragment free (measured spread: 828.572) — the same "measure the
real numbers, don't just check a flag" bar `phi_physics_test`'s own
constraint section already set. Native and wasm both rebuilt clean from
a forced-clean state; every other standalone harness in the project
(`animation_test`, `area_tree_test`, `asset_protocol_test`,
`fracture_test`, `light_test`, `mesh_edit_test`, `mp_console_test`,
`mp_physics_test`, `mp_prop_panel_test`, `mp_test`, `mp_stress`,
`phi_physics_meshobject_test`, `phi_physics_test`, `phi_prop_test`)
re-run and re-verified passing, since `main.c` and the `Makefile` both
changed. Live GUI verification (right-clicking a mesh, choosing
Fracture, shooting it, watching fragments actually separate on screen)
is still not possible in this sandbox — same `XOpenDisplay()` limitation
noted everywhere else in this doc a real window is needed; the
`breaking_threshold=200.0f` value in particular should be treated as a
starting point to tune once that's possible, not a verified-correct
constant.

### Bullet Physics via Emscripten

Bullet's C++ source compiles directly with `emcc` and links into `engine.wasm` as
a first-class subsystem — no JS bridge, no ammo.js middleware. **Correction,
2026-08-10: there is no single "Bullet's C API" to call — see the Status
section above.** Bullet adds approximately 2–4 MB to the WASM binary.

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

### Status: light-source model landed 2026-08-14; the real path tracer itself landed 2026-08-14 (see dated note below) — single-still-frame rendering, triggered from the editor and from Python, is real and verified; the animation-sequence/ffmpeg video-export half described later in this section (`phi.OfflineRenderer`, the `/render/*` server endpoints) remains explicit deferred future work, not attempted this pass

The tracer needs SOME light source to trace toward for direct lighting, and
this project had neither a dedicated Light object type nor any editor-side
way to designate mesh-face emission (`HEFace.emission` existed as data since
Phase 1's PBR material work, but had no live-editing path -- see Phase 1's
status). Checked how Blender/Cycles actually handles this before building
anything (`blender/` was added this session specifically as reference
material for exactly this kind of question) -- Cycles unifies BOTH
mechanisms, any material with emission strength acting as a mesh light PLUS
dedicated Light objects (Point/Sun/Spot/Area), through one light-sampling
system. This pass builds the "dedicated Light objects" half for real (see
below); the actual path-tracing/light-sampling machinery is still entirely
unbuilt, and mesh emission still only does cosmetic self-glow in the live
deferred-rasterized preview (`gbuffer.c`'s `lit = ... + emissive`), not real
light transport -- both need real new work whenever the tracer itself starts.

- **`PhiLight` objects** (new `client/light.h`/`.c`): Point/Sun/Spot/Area,
  fields mirroring the core of Blender's own `Light` DNA struct
  (`blender/source/blender/makesdna/DNA_light_types.h`, read directly) --
  position, direction (Sun/Spot), color, energy, radius (Point), spot_size/
  spot_blend (Spot), area_size (Area), sun_angle (Sun). Deliberately a real
  subset, not every EEVEE-specific/deprecated field Blender itself carries
  (shadow filter radius, cascade shadow-map tuning, etc. -- meaningless
  without a shadow system for arbitrary lights, which this pass doesn't
  build). A fixed-capacity registry (`PHI_MAX_LIGHTS` = 16), not a real
  dynamic scene graph. Selectable in the 3D viewport (`light_ray_pick`,
  real ray-vs-sphere against a fixed icon radius, "nearest hit wins" same
  as mesh-face picking) via a new `5000+id` selection range alongside
  MeshObjects' existing `4000+id` (see `ui.h`'s own comment). Rendered as
  small color-coded solid-box icons (`renderer_draw_lights`, new in
  `renderer.c` -- deliberately lives there, not in `light.c`, same split
  `renderer_draw_mesh_object` already has from `meshobject.c`, which also
  keeps `light.c` itself completely GL-free). Spawned via new Scene
  context-menu rows ("Add > Light > Point/Sun/Spot/Area", at the camera's
  orbit pivot) and deleted via the existing "Delete" row, now mode-aware
  (deletes whichever of a MeshObject/Light is actually selected).
- **Editable Properties panel** (`ui.c`) -- a real, first-ever click-to-edit
  widget for this codebase's Properties panel, not just a read-only
  registry dump (which is all it was before this pass, see Phase 1's
  status). One shared focus slot, reusing the DNA/RNA property registry
  (`phi_prop.h`) generically: click a row, type a new value (VEC3 props as
  one "x, y, z" line, same convention the old console's `matcolor`/
  `matemit` commands used), Enter commits through `phi_prop_set_float`/
  `_vec3`, click elsewhere cancels. BOOL props (`MeshObject.is_static`) and
  the Light `type` field toggle/cycle immediately on click instead, since a
  raw typed number is a worse interaction for either. A single shared
  `properties_panel_walk(..., do_draw, ...)` function serves both the draw
  pass and the hit-test pass -- the same "one source of truth, not two
  independently-maintained copies" fix this file's context-menu
  `items[]`/`actions[]` history already forced once (see `build_ctx_menu_
  rows`), applied proactively here instead of after the fact. **Found and
  fixed along the way**: the Outliner's click-to-select hit-test had a real
  pre-existing off-by-one-row bug (its hit range was a full row below where
  "MeshObject #N" is actually drawn, confirmed algebraically against
  `draw_panel_outliner`'s own layout, not guessed) -- discovered because the
  new Light rows needed correct row math to work at all. A pinned "Render
  Settings" section (just `samples` so far) shows regardless of selection,
  Blender's own Render Properties tab being similarly selection-independent
  (this project has no tab strip, so it's appended at the bottom instead).
- **`RenderSettings`** (new `client/render_settings.h`): one global instance
  (`main.c`'s `g_render_settings`), registered with the same DNA/RNA system
  as everything else -- `samples` (default 128, clamped `[1, 1000000]`) is
  the only field so far, real enough to grow as Phase 3 itself gets built.
- **Shared "object"/"face"/"light:\<id\>"/"render" resolver** (new
  `client/scene_target.h`/`.c`) -- factored out of `mp_port.c`'s own
  previously-private `resolve_target`, now the ONE place this resolution
  rule lives, shared by the Python bindings AND the new chat-driven wire
  protocol below (same "one source of truth" reasoning as
  `properties_panel_walk` above).
- **Python API** (`mp_port.c`): `phi.prop_get`/`prop_set` now also accept
  `"light:<id>"` and `"render"` targets (through the shared resolver above)
  alongside the existing `"object"`/`"face"`; new `phi.add_light(type, x, y,
  z)` / `phi.delete_light(id)` / `phi.list_lights()`.
- **Chat-driven scene mutation, read-write by explicit project decision**
  (unlike `get_asset_list`/`get_scene_state`, which stay read-only): three
  new wire-protocol request/reply pairs (`PKT_PROP_SET_REQUEST`/`REPLY`,
  `PKT_ADD_LIGHT_REQUEST`/`REPLY`, `PKT_DELETE_LIGHT_REQUEST`/`REPLY`, see
  `client/net.h`), mirroring `PKT_SCENE_STATE_REQUEST`/`REPLY`'s existing
  "server asks THIS client to act, since the live scene state only exists
  in the client process" pattern -- `server.py`'s `Client` class
  generalized its req_id/Event pending-reply machinery
  (`_send_and_wait`/`_resolve_pending`) to serve all four request types
  from one shared implementation rather than three more copies of the same
  wait dance. Four new Anthropic tools (`add_light`/`set_light_property`/
  `delete_light`/`set_render_samples`) actually mutate the running client's
  live scene, not just describe it -- `get_scene_state`'s own JSON also
  grew a `lights` array and `render_settings` object so Claude can see
  what's already there before changing it. Explicitly scoped: no object/
  mesh mutation tool exists even though the underlying wire packet could
  carry one (`server/anthropic_client.py`'s own tool list is what actually
  gates what Claude can invoke) -- lights and render settings only, per the
  user's own explicit instruction. Claude still cannot post a chat message
  AS the user under any of this -- confirmed already structurally
  impossible before this pass, not something newly built: `PKT_CHAT_MSG`
  only ever originates client-side from the real human (`chat.c`), and
  Claude's replies go out over the separate `PKT_CHAT_REPLY`, prefixed with
  the model's display name.

Verified: new `light_test` standalone harness (spawn/delete/find/get_all,
registry-full behavior, real ray-vs-sphere pick math checked against the
actual geometric expectation, and the scene_target resolver's four target
kinds including its failure paths) plus new sections in `phi_prop_test`
(PhiLight/RenderSettings props through the real DNA/RNA accessors,
including range clamping). Native and wasm both rebuilt clean; every
existing standalone harness re-verified passing. `server/server.py` checked
with `python3 -m py_compile` (no live end-to-end chat-tool round trip was
run against a funded API key this pass). No live GL/input verification was
possible in this sandbox (no real X display) -- the light icon rendering,
Properties-panel click-to-edit interaction, and Outliner row fix are
compile-clean-and-reviewed, not click-tested. Win32 remains the user's own
`build.bat` responsibility, not built from this sandbox.

#### G (Grab) now works on a selected Light, 2026-08-14

Per explicit request. `client/transform_op.h`/`.c`'s modal G/S/R tool
took a `MeshObject *` directly, mutating `obj->position`/`orientation`/
`scale` -- widened to plain `Vec3f`/`Quat`/`Vec3f` pointers instead
(orientation/scale may be `NULL` for a Grab-only target, guarded
defensively in `update_scale`/`update_rotate`) rather than duplicating
the whole module for one extra case, the same "generalize once a real
second use case shows up" reasoning `phi_physics_add_point2point_
constraint` used for `PhiConstraint`'s own widened field earlier this
session. New `main.c` helper `resolve_xform_target(kind, ...)` resolves
the right pointers for whatever's currently selected -- the one
MeshObject slot (any of Grab/Scale/Rotate) or a selected Light (Grab
only; a light has no orientation/scale field worth Scale/Rotate-ing, so
`s`/`r` with a light selected simply isn't offered, falling through to
whatever else that keystroke normally does rather than being silently
swallowed). Called fresh at every begin/update/cancel site rather than
cached, safe because selection can't change while a modal op is active.

Also fixed while touching the Timeline panel: its Play/Pause button
label was vertically mispositioned (`ui_text_draw`'s `y` is the TOP of
the glyph box, not a button-center offset -- the original `by + bh*0.5f
+ 4.0f` sat the label near the button's vertical center rather than
centered within it, visibly low). Replaced with a real centering
formula, `by + (bh - label_size) * 0.5f`.

Native and wasm both rebuilt clean; every standalone harness in the
project re-run and re-verified passing (`transform_op.c` has no
standalone harness of its own -- always exercised through `main.c`
directly, unchanged this pass). No live GUI verification possible in
this sandbox, same limitation as everywhere else in this doc.

#### The real path tracer, 2026-08-14

A genuine BVH-accelerated Monte Carlo path tracer (new `client/path_
tracer.h`/`.c`), not a toy/approximation and not the octree-based sketch
the rest of this section originally described below (that sketch predates
this project's own removal of Qek's octree world, see Phase 1's status --
`octree_ray_cast` no longer exists; this implementation builds its own BVH
over the live scene's actual mesh triangles instead, the correct approach
now that geometry means `MeshObject`s, not a voxel octree).

- **Scene**: `pt_scene_build` flattens whatever's actually on screen right
  now (the live test object, or its fracture fragments once Phase 2's
  `fracture_body_activate` has run -- the SAME set `scene_content_cb`
  draws) into world-space triangles, transformed with the identical T*R*S
  model matrix `renderer_draw_mesh_object` uses (position, quaternion
  rotation, then component-wise scale) -- not the scale-ignoring transform
  `meshobject_ray_pick` uses for picking, a real, deliberate divergence
  from that (buggy, already-flagged-elsewhere) approximation, since this
  code's job is to match what's actually rendered.
- **BVH**: real median-split binary tree (longest-axis split, leaf
  threshold 4 triangles), not SAH-optimized -- an honest scope choice for
  this engine's current scene sizes (one live object slot plus its
  fracture fragments), not silently pretended to be SAH. Iterative
  traversal via an explicit stack, slab-tested AABBs bounded by the best
  hit found so far.
- **BSDF**: real microfacet math -- Lambertian diffuse + Cook-Torrance GGX
  specular (`D*G*F/(4 NdotV NdotL)`, Smith-Schlick visibility with Karis'
  UE4 analytic-light `k` remap, Schlick Fresnel), driven by the exact same
  `base_color`/`metallic`/`roughness` fields the Properties panel and live
  rasterizer already read/write (`HEFace`'s material, see Phase 1's PBR
  work) -- not a separate, disconnected "renderer-only" material model.
  Stochastic lobe selection (diffuse via cosine-weighted hemisphere
  sampling, specular via GGX half-vector importance sampling per Walter et
  al. 2007) with a metallic-driven selection probability, each branch
  correctly divided by its own selection probability so the combination
  stays unbiased.
- **Next-event estimation**: every live `PhiLight` is sampled directly
  every bounce (real closed-form radiometry per type -- Point/Spot use
  power->intensity->inverse-square-irradiance, Sun uses Blender's own
  "Strength is already an irradiance" convention with no distance
  falloff, Area approximates its emitting face with a single
  representative-point sample rather than full stratified-area sampling,
  a real, honestly flagged scope limit: correct mean energy, no
  area-driven soft-shadow penumbra), tested against the BVH with a real
  shadow ray. Emissive `HEFace` materials contribute only via BSDF-sampled
  hits (never double-counted against NEE, since NEE never targets
  emissive geometry, only dedicated Light objects -- no MIS bookkeeping
  needed as a result).
- **Integrator**: real path tracing, not a single-bounce direct-lighting-
  only approximation -- Russian-roulette-terminated after 3 bounces
  (probability = max throughput channel, clamped `[0.05, 0.95]`), jittered
  per-pixel supersampling (`render_settings.h`'s `samples`, the exact
  field the Properties panel/`phi.prop_set("render", "samples", N)`
  already exposed on 2026-08-10, now finally consumed by something real).
  Camera ray generation matches `main.c`'s own `cam_basis`/
  `compute_scene_ray` formula exactly (verified against it directly, not
  re-derived), so a rendered frame lines up with whatever the live
  rasterizer shows from the same camera state.
- **Output**: real PNG via a newly-vendored `client/stb_image_write.h`
  (same "vendor a small, proven single-header library" precedent as
  `stb_truetype.h`/`cgltf.h`) -- Reinhard tonemap + gamma 2.2, not a raw
  HDR-to-8-bit clamp.
- **Reachable two ways**: a new File > "Render Still Frame" menu row
  (`TOP_ACTION_FILE_RENDER`, `ui.h`/`ui.c`) and a new `phi.render()`
  Python binding (`mp_port.c`, registered via a plain function-pointer
  callback from `main.c`'s `render_still_frame_to_disk`, same shape
  `phi_mp_register_targets` already uses for cross-file state) -- both
  paths share the exact same rendering logic, matching "Claude has real
  read-write access to this stuff" the same way lights/render-settings
  already do. Fixed 640x480 output resolution this pass (only `samples`
  was ever asked to be user-configurable); synchronous, so the editor
  visibly hangs for the render's duration -- no background-thread/
  progress-bar UI yet, a real, honestly flagged scope limit, fine for a
  still frame at modest sample counts.

Verified with a new standalone no-GL harness (`path_tracer_test`, `make
path_tracer_test`) -- no stub functions needed at all (unlike `fracture_
body_test`), since this module never touches the renderer or GL: 500
random rays checked against an independently re-derived brute-force
ray/triangle scan (BVH traversal matches exactly, hit/miss and t); a real
BSDF furnace test (mean sampled importance-sampling weight over 20,000
samples stays within a real energy-conservation bound for both a
dielectric and a metal, not just "did it run"); closed-form NEE checks
with no Monte Carlo noise involved (point light: doubling distance cuts
contribution to exactly 1/4, doubling power exactly doubles it, a light
below the horizon contributes exactly zero; sun light: linear in
strength, exactly zero when it points away from the surface); and a full
end-to-end render (BVH+BSDF+NEE+camera all together) checked for
finite-only pixels and real image variation (not a flat sky-only miss),
plus a real PNG signature check on the written file. Native and wasm both
rebuilt clean from a forced-clean state; every other standalone harness
in the project re-run and re-verified passing, since `main.c`/`ui.c`/
`mp_port.c`/the `Makefile` all changed. Live GUI verification (clicking
File > Render Still Frame and looking at the resulting PNG) is still not
possible in this sandbox -- same `XOpenDisplay()` limitation as everywhere
else in this doc a real window is needed; the rendered image's actual
visual quality (framing, exposure, noise level at the default sample
count) is therefore compile-clean-and-numerically-verified, not eyeballed.

### The raytracer (original design sketch, largely superseded by the dated note above)

The paragraphs below predate this project's real implementation and
describe an octree-based approach this codebase no longer has (`octree_
ray_cast` was part of Qek's now-removed voxel world, see Phase 1's
status) -- kept for the still-relevant parts (materials/integrator intent,
`stb_image_write.h` choice, the async/progress-bar idea for a FUTURE
multi-frame render) but the geometry/BVH description is stale; see the
dated note above for what's actually built.

- **Octree geometry**: ~~uses existing traversal, free~~ -- N/A, this
  codebase has no octree anymore; superseded by the real triangle BVH above.
- **Discrete mesh objects**: BVH over convex hulls (from Phase 2)
- **Materials**: full PBR material structs (baseColor, metallic, roughness,
  emission, IOR) stored in a UBO — no 256-slot palette constraint
- **Integrator**: diffuse + specular + area lights, Russian roulette termination
- **Output**: RGBA pixel buffer in WASM linear memory, encoded to PNG via
  `stb_image_write.h` (~400 lines of C, no dependencies)

Rendering runs via `emscripten_async_call` so it does not block the editor UI.
Progress events fire to JavaScript as each frame completes, driving a progress bar.
(Both of the above -- async execution and a progress bar -- are FUTURE work
for a real multi-frame animation render; the single-still-frame render
landed 2026-08-14 is synchronous, see the dated note above.)

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

### Status: Armature + Clip/Curve/Playback data foundation landed, 2026-08-10; skin-weight loading landed, 2026-08-10; GPU skinning, Timeline panel, and Bullet ragdoll handoff all landed, 2026-08-14 (see dated note below) — real code across every one of those three pieces, verified where this sandbox's no-live-GL limitation allows and honestly flagged where it doesn't. The bezier-curve-editing "Curve Editor" panel (a genuinely separate piece from the Timeline panel that landed) remains unstarted.

A real, verified first slice of this phase's "Architecture" section below
(Clip/Curve/Playback) plus the Armature half of "Armatures and skinning" —
deliberately scoped to the DATA layer only this pass: no GPU vertex
skinning yet (no mesh is bound to a skeleton anywhere in this slice), no
timeline/curve-editor UI, no ragdoll handoff. Ordered this way
deliberately, same reasoning Phase 1's mesh editor followed (halfedge.c's
pure data layer landed and was standalone-tested before any of it touched
rendering) — the interpolation math and bone hierarchy are worth getting
right and proving in isolation before spending time on the GL/UI layers
that consume them.

**`client/armature.h`/`.c`** (new): loads a real bone hierarchy from a
`cgltf_skin` — rest-pose TRS per bone, inverse bind matrices (read via
`cgltf_accessor_read_float`, not raw buffer pointer math, matching
`halfedge_gltf.c`'s own established precedent for accessor reads that
correctly handles a non-default stride rather than assuming tight
packing). **Topologically sorts joints into parent-before-child order**
rather than trusting the glTF file's own `joints` array order — the spec
does not guarantee that order, and real authoring tools don't always
emit it sorted, so the loader has to be robust to that rather than
assuming the common case. `armature_compute_world_transforms` then
relies on that guarantee to compute every bone's world matrix in a
single forward pass (`parent_world * local`), no recursion needed.

**`client/animation.h`/`.c`** (new): `AnimClip`/`AnimChannel`
(Clip+Curve — a channel's keyframes ARE its curve, interpolation lives
inside `animation_sample_clip`) and `AnimPlayback`. Real interpolation
for all three of glTF's modes — LINEAR (quaternion channels use real
`quat_slerp`, not a lerp-then-normalize approximation), STEP, and CUBIC
SPLINE (the spec's Hermite formula, tangents scaled by the keyframe
interval as the spec requires, with the result renormalized for
rotation channels since Hermite interpolation doesn't preserve unit
quaternion length). A channel targeting a glTF node that isn't one of
the armature's own bones (a camera, a whole-object transform) is
silently skipped, not an error — real files can have those; a clip left
with zero usable channels after that filtering is dropped entirely.
Morph-target (`weights`) channels are skipped too, out of scope this
pass.

**`Quat quat_slerp`/`quat_normalize`** added to `meshobject.h`/`.c`
(the existing Quat-utility home alongside `quat_identity`/
`quat_to_mat4`) — real spherical linear interpolation (shorter-path
fix via the dot-product-sign check, linear-interpolate-then-normalize
fallback near-parallel to avoid a `sin(theta)`-in-the-denominator
blowup), needed by `animation.c`'s rotation-channel sampling.

**A real bug, caught by actually running the new test rather than
trusting "it compiled":** the first version of `AnimChannel` embedded a
fixed `float values[512][3][4]` array directly in the struct. With up
to `ANIM_MAX_CHANNELS` (originally 64) channels per clip,
`sizeof(AnimClip)` came out to roughly **1.7 MB**, and
`animation_test_main.c`'s own stack-allocated `AnimClip clips[8]`
overflowed the stack — a real, immediate segfault (confirmed via
AddressSanitizer: `stack-overflow ... in main`), not a hypothetical
concern. Fixed at the root, not by raising or shrinking the fixed cap
(which would only move the same problem to a different clip size):
`AnimChannel.times`/`.values` are now heap-allocated, sized exactly to
each channel's real keyframe count at load time (`animation_load_clips`),
with a new `animation_clip_free` to release them. No arbitrary
per-channel keyframe cap exists anymore at all.

**Test fixture** (`tools/gen_test_armature.py`, new, hand-rolled glTF
JSON + binary buffer — same "no external tooling" precedent
`tools/gen_test_assets.py`/`assets/cube.gltf` already established): a
3-bone chain (root → mid → tip, each offset (0,2,0) from its parent)
with one animation rotating the mid bone 90° about Z over 1 second,
LINEAR. **The skin's `joints` array is deliberately scrambled
(`[tip, root, mid]`, not parent-before-child)** specifically to exercise
the loader's topological sort against something harder than the trivial
already-sorted case — the inverse bind matrices accessor is reordered to
match (the Nth IBM corresponds to the Nth `joints` entry, per spec, not
node index order — a real detail the generator script has its own
comment on getting right).

Verified (`client/animation_test_main.c`, new, `make animation_test`,
same no-GL standalone-harness precedent as `mesh_edit_test`/
`area_tree_test`): the topological sort resolves `mid`'s and `tip`'s
parents correctly despite the scrambled input; rest-pose world
transforms compose correctly through the hierarchy (tip at world y=4,
i.e. 0+2+2, proving the hierarchy walk is real, not just per-bone local
values echoed back); clip loading finds the right channel/bone/duration;
keyframe sampling checked against **hand-derived exact expected
quaternions**, not just "produced a value" — slerp at the exact
halfway point between identity and a 90° rotation is checked against
the precise 45°-rotation quaternion (the defining mathematical property
of slerp along a fixed axis, not an approximation); and a full
pipeline check (sample the clip, feed the result through
`armature_compute_world_transforms`) is checked against a hand-derived
expected world position, `(-2, 2, 0)`, independently re-derived against
`quat_to_mat4`'s actual column-major convention rather than assumed —
proving the rotation really propagates from a bone to its child, not
just that each function works in isolation. `AnimPlayback`'s loop-wrap
and non-loop-clamp-and-stop behavior both verified too. ASan-clean (no
leaks, confirming `animation_clip_free` actually releases everything
`animation_load_clips` allocates). Native/wasm/win32 all rebuilt clean
from a forced clean state; every other standalone harness re-verified
passing after the `meshobject.h`/`.c` and Makefile `COMMON_SRCS`
changes.

Cubic-spline interpolation is implemented (real Hermite math, matching
the glTF spec) but **not exercised by the current test fixture** — an
honest, documented gap, not silently assumed correct; the fixture only
covers LINEAR.

`armature_compute_skinning_matrices` (world[i] * inverse_bind[i], the
standard second half of GPU skinning a vertex shader would consume) also
landed and is verified against a real, checkable invariant: at rest
pose, with no animation applied, every bone's skin matrix is EXACTLY
identity (not approximately — `world[i]` at rest pose is by construction
the same transform `inverse_bind[i]` was computed to undo). Deliberately
a plain CPU data function with no GL/UBO upload — that's the renderer's
job once a real skinned-mesh render path exists.

#### Skin-weight loading (`client/skinned_mesh.h`/`.c`), 2026-08-10

The vertex-format + `JOINTS_0`/`WEIGHTS_0` loading path phi.md flagged as
the next step above now exists — `SkinnedVertex` (pos/normal/`bone_idx[4]`/
`bone_wgt[4]`, matching this phase's own original sketch) and
`skinned_mesh_load_gltf`, a self-contained loader (own `cgltf_parse_file`/
`cgltf_load_buffers` call, same "just a path in" convention
`halfedge_load_gltf` already established) pulling a skin (via
`armature_load_from_skin`) and a mesh primitive's POSITION/NORMAL/
JOINTS_0/WEIGHTS_0 out of the same glTF file together.

**The one real subtlety, gotten right rather than assumed:** a
`JOINTS_0` value in a glTF file is an index into that skin's OWN
`joints` array, in whatever order the file happens to store it — NOT a
bone index into the `Armature` `armature_load_from_skin` produces, since
that function topologically re-sorts bones into parent-before-child
order (see its own status note above). Using a raw `JOINTS_0` value as a
bone index directly would silently skin vertices to the WRONG bones
whenever the file's `joints` array isn't already sorted — a real,
easy-to-miss bug class for exactly the kind of file this project's own
fixture deliberately exercises. `skinned_mesh_load_gltf` remaps each
joint index through the original joint node's NAME
(`skin->joints[raw_index]->name`) looked up in the already-sorted
`Armature` (`armature_find_bone`) — correct regardless of the file's own
ordering, not just for the specific scrambled order this project's test
fixture happens to use.

`tools/gen_test_armature.py` extended (not a new fixture) with a real
6-vertex/4-triangle mesh spanning the arm — most vertices are trivially
single-bone-weighted, but one (`v3`) is a genuine 60/40 blend between
the mid and tip joints, specifically so the loader's multi-bone
`WEIGHTS_0` handling is exercised against a real blend, not just the
trivial `(1,0,0,0)` case. Weights are copied through as-is, never
renormalized, even if a source file's don't sum to 1.0 — an intentional
"don't silently correct a possible authoring bug" choice, not an
oversight.

Verified (`animation_test`'s own new section 8): loaded vertex/index
counts match the fixture; `v0`'s and `v5`'s single-bone weights resolve
to the correct `root`/`tip` bone indices DESPITE the file's scrambled
`joints` order (proving the remap, not just that loading didn't crash);
`v3`'s real 60/40 blend across `mid`/`tip` reads back exactly. ASan-clean.
Native/wasm/win32 all rebuilt clean.

#### GPU skinning + Timeline panel + ragdoll handoff, 2026-08-14

Closes this phase's three remaining real pieces, per explicit request
("complete phases 2, 3, 4"). Built in dependency order: a real entity
layer wrapping the already-verified data layer, then the GL draw path
that consumes it, then the UI that drives it, then physics on top.

- **`client/skinned_mesh_object.h`/`.c`** (new): `SkinnedMeshObject` --
  wraps `SkinnedMesh`/`Armature`/`AnimClip[]`/`AnimPlayback` (all
  deliberately GL-free, see their own headers) with a real transform
  (position/orientation/scale) and the per-frame CPU pipeline every GPU
  skinning implementation needs: `skinned_mesh_object_update` advances
  playback, samples the current clip, and runs `armature_compute_world_
  transforms`/`_compute_skinning_matrices` (both already existed and were
  verified 2026-08-10) into `world[]`/`skin[]`. Pure CPU, no GL -- same
  "entity data/logic in its own file, GL drawing lives in renderer.c"
  split `meshobject.c`/`renderer.c` already established. A SEPARATE
  bounded slot from `g_test_mesh_object` (main.c's `g_skinned_test_obj`),
  not a rework of the halfedge/PBR-material `MeshObject` pipeline that
  entity type is structurally incompatible with (no per-face material,
  bone indices/weights instead) -- same "own small thing alongside the
  one general object slot" pattern `light.c`/`fracture_body.c` already
  used.
- **Real GPU vertex skinning** (`renderer.c`/`.h`): a THIRD shader
  program (`skinned_program`, alongside `program`/`pbr_program`) --
  vertex shader blends up to 4 bone matrices per vertex
  (`u_bones[SKINNED_SHADER_MAX_BONES]`, capped at 64 -- generous headroom
  over this project's real 3-bone test rig, chosen to stay safely under
  GLES3's guaranteed-minimum per-stage uniform budget rather than
  matching Armature's own 256-bone CPU-side cap 1:1) using bone indices
  read as raw unnormalized `GL_UNSIGNED_BYTE` vertex attributes (arrives
  in-shader as a plain float holding the byte's own integer value,
  exactly what `SkinnedVertex::bone_idx`'s `uint8_t`s need, no remapping
  step). Writes the SAME G-buffer MRT outputs `PBR_FRAG_SRC` does
  (albedo/normal/material/emissive/velocity/object-id), so a skinned
  mesh is lit by the existing deferred pipeline, not a disconnected
  forward-lit oddity -- one fixed per-object uniform material (no
  per-face material data exists for `SkinnedVertex`, a real, honest
  scope limit). Real `glDrawElements` indexed draw (this engine's other
  entity types are all non-indexed/vertex-duplicated -- `SkinnedMesh`
  keeps its glTF-native shared-vertex index buffer instead, a better fit
  since shared vertices carry one set of bone weights each). Object-id
  range `6000+id`, alongside MeshObjects' `4000+id` and Lights' `5000+id`.
- **Timeline panel** (`ui.h`/`ui.c`, new `PANEL_TIMELINE`): a real clip
  name/time readout, a Play/Pause button, and a click-to-scrub progress
  bar -- `timeline_layout` is the one function driving both the draw pass
  and the hit-test pass (`ui_on_mouse_button`'s new `PANEL_TIMELINE`
  case), same discipline `build_ctx_menu_rows`/`properties_panel_walk`
  already established in this file. ui.c never mutates playback state
  directly -- clicks raise one-shot intent (`ui_poll_timeline_scrub`/
  `_play_toggle`, same convention `ui_poll_top_menu_action` already
  uses), main.c drains it each frame and mutates `g_skinned_test_obj.
  playback` itself. Genuinely separate from the still-unstarted
  bezier-curve-editing `PANEL_CURVE_EDITOR` stub -- reuses that panel's
  own icon (no dedicated Timeline SVG exists yet, an honest simplification).
- **Ragdoll handoff** (new `client/ragdoll.h`/`.c` + `phi_physics_add_
  capsule_body`/`_add_point2point_constraint`, new in `phi_physics.h`/
  `.cpp`): one real `btCapsuleShape` body per bone, spanning that bone's
  own head->tail (its own world position to its first child's, or a
  length-matched fallback along the bone's local +Y for a leaf bone with
  no child -- the same real fallback Blender's own ragdoll generator
  uses), connected to its parent's body with a real `btPoint2Point
  Constraint` ball-socket joint at their shared world point (each side's
  pivot correctly expressed in ITS OWN body's local space, not a shared
  world-space frame -- `btPoint2PointConstraint`'s own native
  parameterization). `PhiConstraint`'s internal field widened from
  `btFixedConstraint*` to the `btTypedConstraint*` base class so the
  same wrapper struct correctly serves both this and the existing
  breaking-threshold fixed constraint, a real generalization verified to
  change no existing behavior (every operation either call site performs
  -- add/removeConstraint, `setBreakingImpulseThreshold`, `isEnabled` --
  is already a base-class method). Same bounded-registry pattern as
  `fracture_body.c`. Triggerable from Python (`phi.activate_ragdoll()`,
  same function-pointer-callback registration shape `phi.render()`
  already uses) -- not wired to any UI control this pass (the skinned
  test object isn't selectable/pickable yet), a real, honestly flagged
  scope limit. NOT wired into any visual rendering either -- no capsule-
  mesh draw path exists, ragdoll bodies simulate but aren't drawn.

Verified with two new standalone no-GL harnesses. `skinned_mesh_object_
test`: rest-pose skin matrices are exactly identity (the same invariant
verified 2026-08-10, now checked through the new wrapper); playback
actually changes the pose mid-clip; a paused/scrubbed pose is checked
for exact determinism (re-sampling the identical scrubbed time twice
produces bit-identical matrices); a looping clip's time wraps into
`[0, duration)` rather than running past the end. `ragdoll_test`: real
Bullet physics, not mocked -- defensive NULL/empty-armature cases; real
activation spawns one capsule per bone at the correct rest-pose
positions (independently re-derived, not read back from the module
under test); and the real load-bearing check -- after 1.5s of tumbling
freefall under gravity, each joint's world position is reconstructed
FROM BOTH SIDES of its constraint (parent body's local pivot and child
body's local pivot, each rotated through that body's own CURRENT live
orientation) and checked to still coincide, the actual invariant a
point2point constraint guarantees (raw body-center-to-body-center
distance is NOT the same thing, since capsules can rotate about a
shared pivot without it ever coming apart) -- gap measured at 0.0000
after the full freefall in this session's run. Native and wasm both
rebuilt clean from a forced-clean state; every other standalone harness
in the project re-run and re-verified passing, since `main.c`/`ui.c`/
`mp_port.c`/`phi_physics.h`/`.cpp`/the `Makefile` all changed.

**Honestly flagged, not silently glossed over:** live GUI/GPU
verification (actually seeing a skinned character on screen, clicking
Play in the Timeline panel and watching it animate, watching a ragdoll
tumble) is not possible in this sandbox -- same `XOpenDisplay()`
limitation noted everywhere else in this doc a real window is needed.
The skinned-mesh shader code compiles and links clean (confirmed via a
full native+wasm rebuild) but its actual on-screen visual correctness
(is the skinning math *visually* right, does the Timeline scrub bar feel
right to actually click) is unverified beyond the CPU-side matrix math
`skinned_mesh_object_test` checks numerically. The skinned test object
also isn't selectable/pickable, has no Properties-panel material
editing, and isn't reachable via any "Add" context-menu row (auto-loaded
once at startup instead) -- real, deliberate scope cuts for this pass,
not oversights.

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

### Status: embedding + decorator-pattern prototyping done; wired into the real build with a real Console REPL; a real (if intentionally scoped) C API surface landed 2026-08-14 (see dated note below) — geometry creation/vertex editing specifically, per explicit request; the original "initial" sketch's gameplay-oriented items (`phi.spawn`/`destroy`/`players`, the `phi.on`/`emit` event system) don't apply to this project's current single-player-editor architecture (no gameplay-entity system exists) and were deliberately not built

A bounded first slice is complete and build-verified on all three targets
(native/win32/wasm): MicroPython itself is embedded (`client/micropython_embed/`,
generated via MicroPython's own documented `ports/embed` workflow — see
`client/mpconfigport.h` for config and regeneration instructions), a basic
C↔Python round trip works (call Python from C with arguments and a return
value; expose a C function to Python; an uncaught Python exception is caught
and printed, not a crash or hang), and — the actual point of this slice —
both decorator patterns Phase 1 and Phase 6 depend on are prototyped and
confirmed working against real MicroPython. See the CRITICAL WARNING section
below for exactly what was tested and what was found (including one real bug
in MicroPython's own `ports/embed` — a false-positive recursion-depth
exception from an unset stack limit — root-caused via `gdb`, worked around,
documented at the fix site for when this needs to land in Phi's real
MicroPython init path too).

**Performance against the concrete scale targets given for this work**
(~5000 nodes in a graph, ~280 concurrent lightweight scripted entities —
200 NPCs + 80 vehicles, sized off comparable AAA crowd/graph densities):
a synthetic stress-test harness (`client/mp_stress_test_main.c`, `make
mp_stress`, not the real node-graph or NPC system — neither exists yet)
measured actual per-call overhead rather than assuming it. Node-dispatch
model: 5000 sequential C→Python function calls, matching the evaluator's
"once per node, not per vertex" cost model. Entity-tick model: 280
concurrent Python generator objects (a stand-in for `uasyncio` tasks —
real `uasyncio` needs file/frozen-module import this no-filesystem
embedding doesn't have yet — driven through the same C-level
`mp_obj_gen_resume` mechanism `async`/`await` coroutines use, so it
measures the same underlying per-resume cost), round-robin-driven for a
few ticks each.

| | Native (x86_64) | wasm (via Node, no browser JIT either way) |
|---|---|---|
| 5000 node dispatches | 0.14 ms total (0.03 µs/call) | 10.1 ms total (2.0 µs/call) |
| 1120 entity-tick resumes (280 × 4) | 0.04 ms total (0.03 µs/resume) | 2.1 ms total (1.8 µs/resume) |

wasm is ~50-70x slower per call than native here — expected and consistent
with earlier research into this project's Python-implementation choice
(no WASM target gets a JIT; MicroPython's interpreter loop pays full
per-bytecode cost there). Both are still comfortably inside a 16.6ms frame
budget at these exact target scales, but the margin differs a lot by
platform: native has ~128x headroom even for a naive full-graph-every-
frame re-evaluation; wasm has only ~2x headroom for that same naive case.
This is exactly why Phase 6's dirty-flagging design (re-evaluate only the
subgraph that actually changed, not all 5000 nodes every frame) isn't
just a nice-to-have — on the real deployment target (the browser, not
this native dev loop), it's likely load-bearing at anywhere near the full
5000-node target. Entity ticking has comfortable headroom on both
platforms even in the synthetic worst case (every entity resumes every
frame, no LOD/staggering) — 0.1% of budget on native, 3.1% on wasm — but
that's 280 *trivial* 3-yield tasks; a real NPC/vehicle coroutine doing
actual decision logic per resume will cost more than this floor number,
so this isn't a substitute for testing real behavior scripts once they exist.

**Update, 2026-08-14**: this "Not started" paragraph is now stale on both
counts. MicroPython IS wired into the real game build (`client/mp_port.c`
is in `COMMON_SRCS` -- native/win32/wasm all link it), the Console panel
executes real typed Python (falling through from `console_dispatch`'s own
dev-commands), and a real (if deliberately scoped) part of the C API
surface below -- `phi.mesh_object`/`create_mesh`/`set_vertices`/
`set_vertex`/`list_objects`/`delete_object`, plus the already-longer-
standing `phi.prop_get`/`set`/`enable_physics`/`add_light`/`render`/
`activate_ragdoll` -- is real and build-verified, not a sketch. See this
section's own new dated entry below for what specifically landed and why
the REST of the sketch (`phi.raycast`, `phi.spawn`/`destroy`/`players`,
`phi.on`/`emit`) either remains genuinely unbuilt or doesn't apply to
this project's current architecture at all.

### Embedding strategy

MicroPython's C source is compiled directly into `engine.wasm` alongside the
engine. It is not a separate runtime — the Python interpreter is a subsystem of
the engine binary. Scripts load as plain text (from `.py` files, from the
in-editor code pane, or from WebRTC data channel delivery from the host peer).

MicroPython adds approximately 200–400 KB compressed to the WASM binary.
`uasyncio` is included for async NPC behaviour (see Phase 7).

### CRITICAL WARNING — status: the two patterns below are now confirmed

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

**Update — both patterns prototyped against real MicroPython (not CPython) and
confirmed working**, via a standalone embedding self-test
(`client/mp_test_main.c`, `make mp_test`/`mp_test_win32`/`mp_test_wasm`,
verified on all three targets):

- **`@phi.panel`-style class decorator**: a plain-Python-base class,
  subclassed, decorated to register it by name, instantiated from C, its
  overridden method called from C, with real persistent instance state
  (`self.call_count` surviving across calls, not re-evaluated fresh each
  time). All of it worked exactly as sketched — no `__init_subclass__` or
  other metaclass machinery was needed for this specific pattern (the
  decorator does the registration from *outside* the class, not via a class-
  side hook), so the warning above about metaclass gaps turned out not to
  bite here. Confirmed empirically, not just because the pattern happens
  not to need the risky features.
- **`@phi.node`-style function decorator**: confirmed working, but **not**
  via type-annotation introspection — MicroPython parses PEP 3107 annotation
  syntax (`def f(x: int)`) but never stores it; there is no `__annotations__`
  on function objects at all, confirmed by a direct empirical probe
  (`AttributeError: 'function' object has no attribute '__annotations__'`).
  This does NOT break the `@phi.node` design below, because — on close
  re-reading — the code sketch never actually used annotations in the first
  place: `inputs=[('mesh', Mesh), ('scale', float, 1.0)]` is passed as an
  explicit decorator keyword argument, not derived from the wrapped
  function's parameter annotations. The prose immediately below that sketch
  ("The decorator reads type annotations...") was simply an inaccurate
  description of its own code example — fixed now to describe what the code
  actually does. Net effect: the real design was already annotation-free and
  MicroPython-safe; only the documentation was wrong.

One more finding from this same prototyping pass, unrelated to decorators but
worth flagging since it'll bite anyone embedding MicroPython for Phi work:
**float literals (`1.0`) raise `SyntaxError: decimal numbers not supported`
unless `MICROPY_FLOAT_IMPL` is explicitly set** — it defaults to
`MICROPY_FLOAT_IMPL_NONE` regardless of ROM level (not something
`MICROPY_CONFIG_ROM_LEVEL_FULL_FEATURES` turns on for you). `client/
mpconfigport.h` sets `MICROPY_FLOAT_IMPL_FLOAT` (single-precision, matching
the engine's own `float` convention throughout the C side).

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

**Update, 2026-08-14 -- what actually got built vs. what didn't, and why**:
this was always a design sketch, not a commitment to build every line of
it verbatim. What landed for real, with a real (if different, see below)
signature -- `phi.mesh_object(path, x=0, y=0, z=0)` (loads AND adds a new
scene object, doesn't just return a floating `MeshObject` handle -- this
engine's actual object model is main.c's `scene_objects.c` registry, not
a Python-side value type), `phi.create_mesh(positions, indices, x=0,
y=0, z=0)` (new -- not in the original sketch at all; the actual "create
geometry" capability the sketch's `mesh_subdivide`/`mesh_extrude`/
`mesh_merge` were gesturing at, built as one general raw-geometry
primitive instead of three separate derived-mesh operations),
`phi.set_vertices(id, positions)`/`set_vertex(id, i, x, y, z)` (new --
"redefine the verts of existing scene geometry", the real ask this
landed for), `phi.list_objects()`/`delete_object(id)`. `phi.apply_
impulse` already existed (Phase 2, takes no explicit `obj` argument --
operates on "the selected object" via `phi_mp_register_targets`'s
resolver, see Phase 1's "Real multi-object scene graph" dated entry,
rather than an object value passed as an argument -- a real, deliberate
difference from the sketch's own signature, matching how `phi.prop_get/
set` already worked). **Still genuinely unbuilt**: `phi.raycast`,
`mesh_subdivide`/`extrude`/`merge`/`noise3` as their own distinct ops,
`anim_clip`/`play` (Phase 4's `AnimClip`/`AnimPlayback` exist and are
real, just not Python-bound yet). **Deliberately will not be built as
sketched**: `phi.spawn`/`destroy`/`players`/`time`/`on`/`emit` -- these
describe a multiplayer gameplay-entity/event system (Qek's own domain,
see phi.md's Phase 1 "Client/server model" note on that code being
removed) that has no equivalent in Phi's current single-player-editor
architecture; revisiting this sketch's gameplay half only makes sense
once/if a real entity-and-event system exists to bind it to, not before.

Its design is the highest-leverage design decision in the project — changing it
breaks all user scripts.

#### Geometry creation + vertex editing, real for both the Console and Claude, 2026-08-14

Per explicit request ("make it so Claude can create geometry and
redefine the verts of existing scene geometry via the Python API"). Two
halves, sharing one implementation:

- **The Python bindings themselves** (`client/mp_port.c`): `phi.mesh_
  object`/`create_mesh`/`set_vertices`/`set_vertex`/`list_objects`/
  `delete_object`, calling `scene_objects.c`/`halfedge.c`/`halfedge_
  gltf.c` directly -- no `main.c` callback indirection needed (unlike
  `phi.render()`/`activate_ragdoll()`, which genuinely need main.c-only
  state like `g_cam_pos`/`g_skinned_test_obj`; `scene_objects.c` is a
  self-contained registry module, the same shape `light.c` already is,
  and `phi.add_light`/`delete_light`/`list_lights` already called it
  directly too). `create_mesh` uses the existing `halfedge_build_from_
  triangles` (previously unused by any Python-reachable path) to turn a
  flat position list + a flat triangle-index list into real half-edge
  topology, with real bounds/index-range validation (a bad index raises
  a real `ValueError`, doesn't read past the position array). The new
  `RenderMesh` for each created/loaded object is `calloc`'d directly
  rather than via `octree_render.c`'s `mesh_create()` -- deliberately,
  so `mp_port.c`'s translation unit never needs that GL-touching file
  linked, keeping every existing no-GL `mp_*` standalone test building
  exactly as before (would otherwise force a real GL context onto tests
  that have never needed one).
- **Claude's chat access to the same capability** (`client/net.h`/`.c`,
  `main.c`, `server/server.py`): three new wire-protocol request/reply
  pairs (`PKT_CREATE_MESH_REQUEST`/`REPLY`, `PKT_SET_VERTICES_REQUEST`/
  `REPLY`, `PKT_DELETE_MESH_OBJECT_REQUEST`/`REPLY`), same "server asks
  THIS client to act" shape `PKT_ADD_LIGHT_REQUEST` already established,
  wired to three new Anthropic tools (`create_mesh_object`/`set_mesh_
  vertices`/`delete_mesh_object`) -- explicit project decision, same
  read-write-not-read-only choice already made for lights/render
  settings, extended here per this session's later explicit request.
  `get_scene_state`'s JSON grew a real `objects` array (see Phase 1's
  "Real multi-object scene graph" dated entry) specifically so Claude
  can discover an existing object's id/vertex count before editing it.
  Wire-protocol vertex/triangle counts are capped (`PKT_MESH_MAX_VERTS`
  = 2048, `PKT_MESH_MAX_TRIS` = 4096 -- matching constants in both
  `client/net.h` and `server.py`, checked on both ends) -- a real,
  deliberately modest bound distinct from the LOCAL Python console's own
  `phi.create_mesh`, which has no extra cap beyond this codebase's
  existing `uint16_t` index-width convention; anything bigger belongs in
  a real authored asset via the Asset Browser, not a single chat-tool
  call. Multi-byte fields in the new wire parsers are `memcpy`'d into
  real aligned local arrays rather than cast in place from the raw byte
  cursor -- the existing per-scalar convention every other field in
  `net.c`'s parser already used, just extended to arrays (a cast-in-
  place would risk both strict-aliasing UB and misaligned access on a
  buffer with no guaranteed float alignment).

Verified with two new standalone harnesses. `scene_objects_test` covers
the registry itself (see Phase 1's own dated entry). `mp_geometry_test`
(new, `make mp_geometry_test`) exercises the actual end-to-end ask
against a real embedded interpreter: `create_mesh` builds a real 4-
vertex/2-triangle quad, checked against the real C-side `HalfEdgeMesh`
afterward (not just "Python didn't raise"); `set_vertices` rewrites
every vertex, checked that vertex 0 AND vertex 3 both actually moved
(not just the first one); `set_vertex` changes one vertex, checked that
its NEIGHBORS did NOT change (a single-vertex edit not clobbering the
rest); `list_objects`/`mesh_object`/`delete_object` all checked against
`scene_object_find` directly, including that loading a second object
via `phi.mesh_object` leaves the first (from `create_mesh`) completely
untouched -- real multi-object behavior, not a replace in disguise; five
distinct real error cases (mismatched array lengths, an out-of-range
index, an off-by-one index, a wrong vertex count, a nonexistent object
id) all checked to raise a real Python `ValueError`, not silently
corrupt or crash. Native and wasm both rebuilt clean from a forced-clean
state; every standalone harness in the project re-run and re-verified
passing. `server/server.py` checked with `python3 -m py_compile` (no
live end-to-end chat-tool round trip was run against a funded API key
this pass, same honest limitation every earlier chat-tool addition this
session already flagged). No live GUI verification possible in this
sandbox.

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

The decorator takes `inputs=`/`outputs=` directly as arguments (plain Python
lists of `(name, type)`/`(name, type, default)` tuples — not derived from the
wrapped function's parameter annotations via `__annotations__`, which
MicroPython doesn't support at runtime even though it parses the annotation
syntax; see Phase 5's CRITICAL WARNING section for the empirical check), builds
socket definitions from them, and registers the node type in the editor
palette. Built-in node types wrap C primitives for speed. Custom user nodes
are pure Python. Same interface, different backing.

### Node type introspection

```python
>>> phi.node_types()
{
    'noise_displace': {
        'inputs':  [('mesh', 'Mesh'), ('scale', 'float', 1.0)],
        'outputs': [('mesh', 'Mesh')],
        'category': 'geometry',
    },
    ...
}
```

Every registered node type — built-in or user-defined via `@phi.node` — is
queryable this way, not just invokable. This exists specifically so a script
(or an LLM working in a running instance) can discover what's actually
available before calling `graph.add_node(type_name, ...)`, rather than
needing to already know the full node registry from memory: list what
exists, read each type's socket signature, then build. Without this, whole-
graph programmatic authoring (see "Graph ownership" below) degrades to
guessing type names and parameter shapes — this is what keeps it reliable
instead.

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
