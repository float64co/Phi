# Qek

**WebAssembly rocket-jumping arena built on a Sauerbraten-exact octree.**

Qek is a small multiplayer arena shooter that runs entirely in the browser as
WebAssembly, with an authoritative Python server over a hand-rolled WebSocket
protocol. Movement and combat follow Quake-style rocket-jump physics
(client-predicted, server-reconciled). Press **E** in-game and the level
itself becomes editable — a Sauerbraten-style click-drag octree editor with
a Source-engine-style console, both backed by the same optimistic-local /
authoritative-server pattern used for player movement, so edits persist to
disk and sync to every connected player live.

```
Language:    C (Emscripten → WASM, or native via Xlib/GLX)
Rendering:   WebGL 2 (GLES3) in-browser, OpenGL 3.3 core natively, flat-shaded
Networking:  WebSockets (RFC 6455 — hand-rolled in pure Python server-side,
             hand-rolled in C for the native client; browser build uses the
             Emscripten WebSocket API)
Server:      Pure Python stdlib — no third-party deps
Geometry:    10-level 1024³ Sauerbraten-style octree
Export:      Binary STL (in-browser, F4 key)
Map format:  .cmap (custom binary octree stream)
```

## Contents

- [Quick Start](#quick-start)
- [Controls](#controls)
- [Editor](#editor)
- [Architecture](#architecture)
- [Binary Protocol](#binary-protocol)
- [Octree & STL Export](#octree--stl-export)
- [Rocket Physics](#rocket-physics)
- [Extending](#extending)

---

## Quick Start

### 1. Install Emscripten

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

### 2. Build

```bash
make          # Builds www/game.js + www/game.wasm
```

### 3. Run

```bash
make run
# or:
cd server && python3 server.py
```

Open `http://localhost:8765` in your browser.

---

## Controls

| Key / Input  | Action              |
|-------------|---------------------|
| `WASD`       | Move                |
| `Space`      | Jump                |
| `C`          | Crouch (lowers eye/rocket-spawn height, halves ground speed) |
| `Mouse`      | Look (Pointer Lock) |
| `LMB`        | Fire rocket         |
| `F4`         | Export map as STL   |
| `E`          | Toggle octree editor |
| `` ` ``      | Toggle console       |

Ground movement has no coasting: releasing WASD zeroes your horizontal
velocity almost instantly (`physics_apply_input` in `physics.c`) rather than
sliding to a stop, while acceleration/turning while a key is held is
unchanged. Rockets never have gravity applied (pure `pos += vel*dt` on both
client and server) and always travel exactly along the direction you're
looking (`view_dir`'s `{-sin(yaw)cos(pitch), sin(pitch), -cos(yaw)cos(pitch)}`
formula, shared by the camera, rocket firing, and the editor's raycast) — the
rendered rocket box now rotates to visibly point along its actual velocity
too, instead of always rendering axis-aligned.

Crouch is bound to `C`, not `Ctrl` — `Ctrl+W` is a browser-reserved "close tab"
shortcut that `preventDefault()` cannot block, so `Ctrl` isn't safe to combine
with any movement key. Escape has the same non-overridable-shortcut problem
(browsers reserve it to exit pointer lock, and can swallow the keydown
entirely when that's what triggered it) — the console's `open` state is kept
in sync by watching `InputState.pointer_locked` directly rather than relying
on ever seeing an Escape keydown.

**Rocket jumping**: hold `C` to crouch (lowers where rockets spawn from,
closer to your feet), aim straight down, jump (`Space`) and fire (`LMB`) in
the same motion. The explosion impulse stacks additively with your velocity
— you keep momentum in the air (Quake-style `sv_airaccelerate`). Self-damage
is reduced but never kills you outright.

Splash damage/knockback is occlusion-checked — a rocket that hits the far
side of a wall or floor you've built won't hurt you through it, on both
client (bots, `rocket_explode()` in `physics.c`) and server (real players,
`_explode()` in `server.py` + `mapdata.ray_cast()`). This surfaced a real bug
in the octree's ray cast itself (`octree.c`'s `_ray_vs_aabb`): when a ray's
origin starts inside a node — true for the root on almost every cast, since
rays always start somewhere inside the world — it was returning that node's
far exit distance instead of `0`, which made short-range casts (rocket
flight, this occlusion check) wrongly reject nodes the ray origin was
sitting right inside of. Fixed at the source, so it also improves general
rocket wall-collision and editor targeting accuracy, not just this check.

---

## Editor

Press **E** to enter a Sauerbraten-style octree editor: the local player
switches to noclip fly (`WASD` + `X`/`Z` for up/down, hold `Shift` to sprint)
and a wireframe cube tracks whatever grid cell the crosshair is over.

| Input             | Action                                             |
|-------------------|-----------------------------------------------------|
| `LMB` drag        | Sweep a box and build solid outward from the face  |
| `RMB` drag        | Sweep a box and carve empty inward from the face   |
| `M` + `LMB` drag   | Repaint face material without changing geometry     |
| `[` / `]` or wheel | Decrease / increase grid size (2..512)            |
| `,` / `.`         | Cycle current paint material (0..7)                |

Every drag is exactly **1 grid unit deep** — repeat drags to build up
thicker structures. The world (`WORLD_SIZE` in `octree.h`) is 1024³, four
times the footprint of the default arena, so there's a lot of open space
around it to build into — the crosshair/highlight and `physics.c`'s hard
player-bounds clamp both follow `WORLD_SIZE`, not the old arena size. Even
if you carve away every cube in the map, the editor still lets you target
the implicit `y>=16` floor plane physics.c hard-clamps every player to, and
build back up from it — rendered as a light-grey reference plane sized
exactly to `WORLD_SIZE`, matching the true editable/highlightable bounds
(real geometry always draws over it; the blue clear color behind
everything else is just the skybox). Back-face culling is off,
so both sides of every triangle
(world geometry, players/bots, the reference plane) render correctly —
noclip flight can put the camera behind/inside geometry in ways normal
collided play never could. Each commit is applied locally right away and sent to
the server, which is authoritative: it persists the edit to
`maps/<name>.cmap` and rebroadcasts the resulting map to every connected
client, so edits survive reconnects and server restarts.

Press **`` ` ``** to open a Source-engine-style console. Commands:

```
help                 list commands
clear                clear the console log
pos                  print player position
tp x y z             teleport
name <name>          rename yourself
players              list connected players/bots with hp
kill                 force your own respawn
grid <2..512>        set editor grid size
mat <0..7>           set current paint material
noclip               toggle editor mode
god                  toggle damage/knockback immunity (independent of noclip)
hp <0..500>          set your own HP
give                 restore HP to 100
speed <n>            set ground move speed (default 280)
gravity <n>          set gravity (default 600, 0 = none)
fov <30..150>        set field of view (default 75)
sensitivity <n>      set mouse look sensitivity (default 0.002)
skybox r g b         set the sky/clear color (each 0..1)
save [name]          save the current map to disk (default: "default")
load [name]          load a named map from disk, broadcast to everyone
newmap               regenerate the default arena, broadcast to everyone
maps                 list maps saved on the server
addbot               spawn one more bot (up to 8 total)
delbot               remove the most-recently-added bot
bind <key> <cmd>     run <cmd> whenever <key> is pressed (browser code,
                     e.g. KeyG, Digit1, F5 — outside the console/editor)
unbind <key>         remove a bind
```

`speed`/`gravity`/`fov`/`sensitivity`/`hp`/`kill` are client-local tuning —
if you're connected to a server, its authoritative corrections (position,
hp) will eventually pull things back in line, so treat them as local
feel/debug knobs rather than a way to change shared game rules. `god` and
`noclip`'s immunity, `name`, `save`/`load`/`newmap`/`maps`, and
`addbot`/`delbot` are unaffected by that since they either go through the
server or stay purely local to your own bot roster.

---

## Architecture

```
project/
├── server/
│   ├── server.py           Pure Python HTTP + WS server (RFC 6455 by hand)
│   └── mapdata.py          Python port of octree.c/cmap.c — authoritative
│                            map state, edit ops, .cmap save/load
│
├── client/
│   ├── main.c              Entry point, Emscripten game loop
│   ├── octree.c/h          10-level 1024³ octree, AABB region edits, ray collision
│   ├── octree_render.c/h   Face extraction → WebGL VBO, face culling
│   ├── octree_stl.c/h      Binary STL export (in-browser download)
│   ├── cmap.c/h            .cmap map format (depth-first node stream, v2)
│   ├── editor.c/h          Sauerbraten-style click-drag box editor
│   ├── console.c/h         Source-engine-style command console
│   ├── physics.c/h         Quake movement, rocket physics, explosion impulse
│   ├── renderer.c/h        WebGL shaders, camera, world/player/rocket/wire draw
│   ├── net.c/h             Emscripten WebSocket client, binary protocol
│   ├── input.c/h           Keyboard + Pointer Lock mouse, input state
│   └── library_ws_stub.js  Emscripten JS library placeholder
│
├── maps/
│   └── default.cmap        Authoritative arena, generated + saved by the
│                            server on first boot, updated on every edit
│
├── www/
│   ├── index.html          Canvas, HUD, console/editor overlay, module loader
│   ├── game.js             (generated by emcc)
│   └── game.wasm           (generated by emcc)
│
└── Makefile
```

---

## Binary Protocol

All packets are binary (little-endian). One byte type prefix.

| Type | Dir | Payload |
|------|-----|---------|
| `0x01 HELLO` | C→S / S→C | C→S: `name[16]` · S→C: `id: u8` |
| `0x02 STATE` | S→C | `n: u8` then `n × player_state (25 bytes)` |
| `0x03 INPUT` | C→S | `seq: u16, yaw: f32, pitch: f32, keys: u8` |
| `0x04 SPAWN_ROCKET` | S→C | `id: u8, owner: u8, pos: 3×f32, vel: 3×f32` |
| `0x05 EXPLODE` | S→C | `rocket_id: u8, pos: 3×f32` |
| `0x06 DAMAGE` | S→C | `player_id: u8, delta_hp: i8` |
| `0x07 OBITUARY` | S→C | `killer: u8, victim: u8` |
| `0x08 FIRE` | C→S | `yaw: f32, pitch: f32, crouch: u8` |
| `0x09 EDIT_REGION` | C→S | `op: u8 (0=empty,1=solid,2=material), face: u8, mat: u8, minx,miny,minz,maxx,maxy,maxz: u16` |
| `0x0A MAP_FULL` | S→C | raw `.cmap` bytes — sent on connect and after every edit/save/load |
| `0x0B SAVE_MAP` | C→S | `name_len: u8, name: bytes` |
| `0x0C LOAD_MAP` | C→S | `name_len: u8, name: bytes` |
| `0x0D CONSOLE_MSG` | S→C | `len: u16, utf8: bytes` — echoed into the client console log |
| `0x0E NEW_MAP` | C→S | no payload — regenerate the default arena, save, broadcast |
| `0x0F LIST_MAPS` | C→S | no payload — server replies with a `CONSOLE_MSG` listing `maps/` |

**player_state** (25 bytes):
`id: u8, x y z: 3×f32, vx vy vz: 3×f32, yaw: f32, hp: u8`

---

## Octree & STL Export

The world uses a Sauerbraten-exact octree:
- **10 levels**, **1024³ world units** — the default arena only occupies the
  `[0,256)` corner of this space; everything beyond it starts empty, open
  for the editor to build into
- Leaf size: **2 world units**
- Each node: `EMPTY | SOLID | DEFORMED | SUBDIVIDED`
- `DEFORMED` nodes have 8 corner height offsets (`i8`) — same as Cube 2

**STL Export (F4):**
- Walks the octree, collects all exposed faces
- Non-planar quads split on shorter diagonal (Sauerbraten convention) → watertight mesh
- Binary STL format, triggers browser download as `map.stl`
- Ready for Blender, PrusaSlicer, or your habitat pipeline

**.cmap Format (v2):**
- 32-byte header: `"CMAP"`, version `u16` (= 2), `world_size u8`, reserved
- Depth-first node stream. SOLID includes `faces[6] u8` (materials now
  persist on flat cubes, not just heightfield geometry). DEFORMED includes
  `corners[8] i8` + `faces[6] u8`.
- The server's `mapdata.py` implements the identical format, so bytes read
  straight off the WebSocket can be handed to the client's `cmap_deserialize()`.

---

## Rocket Physics

```c
// On explosion, per affected player:
vec3 delta   = player_eye_pos - explosion_pos;
float frac   = 1.0 - dist / ROCKET_RADIUS;   // 0..1
vec3 impulse = normalize(delta) * frac * ROCKET_FORCE;
player.vel  += impulse;   // additive — stack-able!

// Self-damage capped: never kills self (min 1hp)
// Enables rocket jumping off floors and walls
```

Key constants (tune in `physics.h`):

| Constant | Value | Effect |
|----------|-------|--------|
| `ROCKET_FORCE` | 900 | Blast impulse strength |
| `ROCKET_RADIUS` | 120 | Explosion radius |
| `ROCKET_SELF_DMG` | 0.7 | Self-damage fraction |
| `AIR_ACCEL` | 10 | Air strafe acceleration |
| `MOVE_SPEED` | 200 | Ground speed |

---

## Extending

**Add a new weapon**: add a packet type, create a `Weapon` struct analogous to `Rocket`, implement projectile physics in `physics.c` and server `tick()`.

**Edit the map**: modify `octree_make_default_map()` in `octree.c` or load a `.cmap` file via `cmap_load()`.

**Vertex colours / textures**: the VBO already carries a `mat_id` float per vertex. The fragment shader receives it via `v_mat_id` — just look up into a palette texture or a uniform array.

**Bot AI**: add a `Bot` struct with a simple state machine (ROAM → AIM → FIRE → ROCKETJUMP). Pathfinding can walk the octree leaf nodes.
