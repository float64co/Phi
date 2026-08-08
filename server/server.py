#!/usr/bin/env python3
"""
Pure-Python HTTP + WebSocket game server.
No third-party dependencies. Uses only stdlib.

Serves:
  GET /          → www/index.html
  GET /game.js   → www/game.js   (emscripten glue)
  GET /game.wasm → www/game.wasm
  GET /assets/*  → www/assets/*
  WS  /ws        → game server

RFC 6455 WebSocket implemented by hand.
Game tick runs at 20Hz in a background thread.
"""

import socket
import threading
import struct
import hashlib
import base64
import os
import sys
import time
import math
import logging
from http import HTTPStatus

import mapdata

logging.basicConfig(level=logging.INFO, format='[%(levelname)s] %(message)s')
log = logging.getLogger('server')

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
HOST        = '0.0.0.0'
PORT        = 8765
WWW_DIR     = os.path.join(os.path.dirname(__file__), '..', 'www')
MAPS_DIR    = os.path.join(os.path.dirname(__file__), '..', 'maps')
TICK_RATE   = 20          # Hz
TICK_DT     = 1.0 / TICK_RATE
MAX_PLAYERS = 16

# ---------------------------------------------------------------------------
# Binary protocol constants  (must match client net.h)
# ---------------------------------------------------------------------------
PKT_HELLO        = 0x01
PKT_STATE        = 0x02
PKT_INPUT        = 0x03
PKT_SPAWN_ROCKET = 0x04
PKT_EXPLODE      = 0x05
PKT_DAMAGE       = 0x06
PKT_OBITUARY     = 0x07
PKT_FIRE         = 0x08
PKT_EDIT_REGION  = 0x09   # C→S: [op:u8 face:u8 mat:u8 minx,miny,minz,maxx,maxy,maxz:u16]
PKT_MAP_FULL     = 0x0A   # S→C: raw .cmap bytes
PKT_SAVE_MAP     = 0x0B   # C→S: [name_len:u8 name:bytes]
PKT_LOAD_MAP     = 0x0C   # C→S: [name_len:u8 name:bytes]
PKT_CONSOLE_MSG  = 0x0D   # S→C: [len:u16 utf8:bytes]
PKT_NEW_MAP      = 0x0E   # C→S: no payload
PKT_LIST_MAPS    = 0x0F   # C→S: no payload

EDIT_OP_EMPTY    = 0
EDIT_OP_SOLID    = 1
EDIT_OP_MATERIAL = 2


def _console_payload(text: str) -> bytes:
    b = text.encode('utf-8')
    return struct.pack('<H', len(b)) + b

# ---------------------------------------------------------------------------
# Minimal 3-vector
# ---------------------------------------------------------------------------
class Vec3:
    __slots__ = ('x','y','z')
    def __init__(self, x=0.0, y=0.0, z=0.0):
        self.x=float(x); self.y=float(y); self.z=float(z)
    def __sub__(self, o): return Vec3(self.x-o.x, self.y-o.y, self.z-o.z)
    def __add__(self, o): return Vec3(self.x+o.x, self.y+o.y, self.z+o.z)
    def __mul__(self, s): return Vec3(self.x*s,   self.y*s,   self.z*s)
    def length(self):     return math.sqrt(self.x**2+self.y**2+self.z**2)
    def normalized(self):
        l=self.length(); return Vec3(self.x/l,self.y/l,self.z/l) if l>1e-6 else Vec3()
    def pack(self): return struct.pack('<fff', self.x, self.y, self.z)

# ---------------------------------------------------------------------------
# Game entities
# ---------------------------------------------------------------------------
GRAVITY           = 600.0
JUMP_SPEED        = 220.0
MOVE_SPEED        = 200.0
FRICTION          = 12.0   # must match client/physics.h's FRICTION or server-authoritative
                            # movement will fight client-side prediction after every stop
PLAYER_HALFWIDTH  = 14.0
PLAYER_HEIGHT     = 56.0
PLAYER_EYE_H      = 48.0
PLAYER_EYE_H_CROUCH = PLAYER_EYE_H * 0.5   # matches client/physics.h's PLAYER_EYE_H_CROUCH
ROCKET_SPEED      = 800.0
ROCKET_RADIUS     = 120.0
ROCKET_FORCE      = 900.0
ROCKET_SPLASH_DMG = 100.0
ROCKET_SELF_DMG   = 0.7

next_rocket_id = 0

class Player:
    def __init__(self, pid):
        self.id           = pid
        self.name         = f'player{pid}'
        self.pos          = Vec3(128, 48, 128)
        self.vel          = Vec3()
        self.yaw          = 0.0
        self.pitch        = 0.0
        self.hp           = 100
        self.alive        = True
        self.respawn_t    = 0.0
        self.on_ground    = False
        self.editing      = False   # KEY_EDITING bit — immune to damage/knockback while editing
        self.god          = False  # KEY_GOD bit — immune to damage/knockback via 'god' console command
        # latest input
        self.inp_forward  = False
        self.inp_back     = False
        self.inp_left     = False
        self.inp_right    = False
        self.inp_jump     = False

    def pack_wire(self):
        """25-byte WirePlayerState"""
        return struct.pack('<BfffffffB',
            self.id,
            self.pos.x, self.pos.y, self.pos.z,
            self.vel.x, self.vel.y, self.vel.z,
            self.yaw,
            max(0, min(255, self.hp))
        )

class Rocket:
    def __init__(self, rid, owner_id, pos, vel):
        self.id       = rid
        self.owner_id = owner_id
        self.pos      = pos
        self.vel      = vel
        self.lifetime = 8.0
        self.active   = True

# ---------------------------------------------------------------------------
# Authoritative game state
# ---------------------------------------------------------------------------
class GameWorld:
    """Server-side physics is simplified (flat floor + boundary).
    Full octree collision would require porting octree.c to Python —
    for the server we trust client-side prediction and run simplified checks."""

    def __init__(self):
        self.players  : dict[int, Player] = {}
        self.rockets  : list[Rocket]      = []
        self.lock      = threading.Lock()
        self.time      = 0.0

        # Authoritative octree geometry — backs persistence/broadcast only,
        # NOT the movement collision above (see class docstring).
        self.octree_root    = mapdata.load(MAPS_DIR, 'default') or mapdata.make_default_map()
        self.current_map_name = 'default'
        mapdata.save(self.octree_root, MAPS_DIR, self.current_map_name)

    def apply_edit(self, op, face, mat, minx, miny, minz, maxx, maxy, maxz):
        c = lambda v: max(0, min(mapdata.WORLD_SIZE, v))
        minx, miny, minz = c(minx), c(miny), c(minz)
        maxx, maxy, maxz = c(maxx), c(maxy), c(maxz)
        if maxx <= minx or maxy <= miny or maxz <= minz:
            return
        with self.lock:
            if op == EDIT_OP_SOLID:
                mapdata.set_solid(self.octree_root, minx, miny, minz, maxx, maxy, maxz)
            elif op == EDIT_OP_EMPTY:
                mapdata.set_empty(self.octree_root, minx, miny, minz, maxx, maxy, maxz)
            elif op == EDIT_OP_MATERIAL:
                face = max(0, min(5, face))
                mat  = max(0, min(255, mat))
                mapdata.set_material(self.octree_root, minx, miny, minz, maxx, maxy, maxz, face, mat)
            else:
                return
            mapdata.save(self.octree_root, MAPS_DIR, self.current_map_name)

    def serialize_map(self) -> bytes:
        with self.lock:
            return mapdata.serialize(self.octree_root)

    def save_map(self, name: str):
        with self.lock:
            mapdata.save(self.octree_root, MAPS_DIR, name)
            self.current_map_name = name

    def load_map(self, name: str) -> bool:
        with self.lock:
            root = mapdata.load(MAPS_DIR, name)
            if root is None:
                return False
            self.octree_root      = root
            self.current_map_name = name
            return True

    def new_map(self):
        with self.lock:
            self.octree_root = mapdata.make_default_map()
            mapdata.save(self.octree_root, MAPS_DIR, self.current_map_name)

    @staticmethod
    def list_maps() -> list[str]:
        if not os.path.isdir(MAPS_DIR):
            return []
        return sorted(f[:-5] for f in os.listdir(MAPS_DIR) if f.endswith('.cmap'))

    def add_player(self, pid) -> Player:
        p = Player(pid)
        with self.lock:
            self.players[pid] = p
        return p

    def remove_player(self, pid):
        with self.lock:
            self.players.pop(pid, None)

    def spawn_rocket(self, owner: Player, crouch: bool = False) -> Rocket:
        global next_rocket_id
        next_rocket_id = (next_rocket_id + 1) & 0xFF
        sy, cy = math.sin(owner.yaw),   math.cos(owner.yaw)
        sp, cp = math.sin(owner.pitch), math.cos(owner.pitch)
        eye_h = PLAYER_EYE_H_CROUCH if crouch else PLAYER_EYE_H
        eye = Vec3(owner.pos.x, owner.pos.y + eye_h, owner.pos.z)
        d   = Vec3(-sy*cp, sp, -cy*cp).normalized()
        off = PLAYER_HALFWIDTH + 2
        pos = eye + d * off
        vel = d   * ROCKET_SPEED
        r   = Rocket(next_rocket_id, owner.id, pos, vel)
        with self.lock:
            self.rockets.append(r)
        return r

    def tick(self, dt: float, broadcast_fn):
        """20Hz authoritative tick."""
        with self.lock:
            self.time += dt

            # --- Rockets ---
            exploded = []
            for r in self.rockets:
                if not r.active: continue
                r.lifetime -= dt
                if r.lifetime <= 0:
                    r.active = False
                    continue
                # Simple linear movement; wall detection via floor/ceiling/bounds
                r.pos = r.pos + r.vel * dt
                # Floor collision
                if r.pos.y <= 0:
                    exploded.append(r)
                    r.active = False
                    continue
                # World bounds — follows mapdata.WORLD_SIZE so rockets don't
                # spuriously explode once players carve past the old edge.
                if not (0 <= r.pos.x <= mapdata.WORLD_SIZE and 0 <= r.pos.z <= mapdata.WORLD_SIZE):
                    exploded.append(r)
                    r.active = False
                    continue

            # Process explosions
            for r in exploded:
                self._explode(r, broadcast_fn)

            # Clean inactive rockets
            self.rockets = [r for r in self.rockets if r.active]

            # --- Players ---
            for p in self.players.values():
                if not p.alive:
                    p.respawn_t -= dt
                    if p.respawn_t <= 0:
                        p.pos = Vec3(128, 48, 128)
                        p.vel = Vec3()
                        p.hp  = 100
                        p.alive = True
                    continue

                # Gravity
                on_ground = p.pos.y <= 16.1
                p.on_ground = on_ground
                if not on_ground:
                    p.vel.y -= GRAVITY * dt
                else:
                    if p.vel.y < 0: p.vel.y = 0

                # Jump
                if p.inp_jump and on_ground:
                    p.vel.y = JUMP_SPEED

                # Movement
                sy, cy = math.sin(p.yaw), math.cos(p.yaw)
                fwd   = Vec3(-sy, 0, -cy)
                right = Vec3( cy, 0, -sy)
                wish  = Vec3()
                if p.inp_forward: wish = wish + fwd
                if p.inp_back:    wish = wish + fwd*-1
                if p.inp_left:    wish = wish + right*-1
                if p.inp_right:   wish = wish + right
                wl = math.sqrt(wish.x**2 + wish.z**2)
                if wl > 0: wish.x /= wl; wish.z /= wl

                if on_ground and wl == 0:
                    # No movement key held: stop almost instantly instead
                    # of coasting on the friction decay curve below.
                    p.vel.x = 0.0
                    p.vel.z = 0.0
                else:
                    accel = 10.0
                    ws = MOVE_SPEED
                    curr = p.vel.x*wish.x + p.vel.z*wish.z
                    add  = ws - curr
                    if add > 0:
                        asp = min(accel*dt*ws, add)
                        p.vel.x += asp*wish.x
                        p.vel.z += asp*wish.z

                    # Friction (ground only)
                    if on_ground:
                        speed = math.sqrt(p.vel.x**2+p.vel.z**2)
                        if speed > 0:
                            drop = max(speed, 100.0)*FRICTION*dt
                            ns   = max(0, speed-drop)
                            if speed > 0:
                                p.vel.x *= ns/speed
                                p.vel.z *= ns/speed

                # Integrate position
                p.pos = p.pos + p.vel * dt

                # Floor clamp
                if p.pos.y < 16:
                    p.pos.y = 16
                    if p.vel.y < 0: p.vel.y = 0

                # OOB kill
                if p.pos.y < -200:
                    p.alive = False
                    p.respawn_t = 2.0

    def _explode(self, r: Rocket, broadcast_fn):
        """Apply splash damage and impulse to all nearby players."""
        dmg_events  = []
        obit_events = []

        for p in self.players.values():
            if not p.alive or p.editing or p.god: continue   # immune to damage/knockback
            eye   = Vec3(p.pos.x, p.pos.y+PLAYER_EYE_H, p.pos.z)
            delta = eye - r.pos
            dist  = delta.length()
            if dist >= ROCKET_RADIUS: continue

            # Occlusion: skip if solid geometry (e.g. a player-built floor)
            # blocks the straight line from the explosion to this player —
            # mirrors client/physics.c's rocket_explode() so a real human
            # player can't be hurt through a wall in multiplayer either.
            if dist > 2.0:
                d = delta.normalized()
                origin = r.pos + d   # nudge off the impact surface — see physics.c's comment
                if mapdata.ray_cast(self.octree_root, (origin.x, origin.y, origin.z),
                                     (d.x, d.y, d.z), dist - 2.0) is not None:
                    continue

            frac    = 1.0 - dist/ROCKET_RADIUS
            impulse = delta.normalized() * (frac * ROCKET_FORCE)
            p.vel   = p.vel + impulse

            dmg = int(ROCKET_SPLASH_DMG * frac)
            if p.id == r.owner_id: dmg = int(dmg * ROCKET_SELF_DMG)

            p.hp -= dmg
            if p.id == r.owner_id and p.hp < 1: p.hp = 1
            dmg_events.append((p.id, -dmg))

            if p.hp <= 0 and p.id != r.owner_id:
                p.alive     = False
                p.respawn_t = 3.0
                obit_events.append((r.owner_id, p.id))

        # Broadcast explode packet
        pkt = struct.pack('<BBfff', PKT_EXPLODE, r.id,
                           r.pos.x, r.pos.y, r.pos.z)
        broadcast_fn(pkt)

        for pid, delta in dmg_events:
            pkt = struct.pack('<BBb', PKT_DAMAGE, pid, max(-128, delta))
            broadcast_fn(pkt)

        for killer, victim in obit_events:
            pkt = struct.pack('<BBB', PKT_OBITUARY, killer, victim)
            broadcast_fn(pkt)

    def build_state_packet(self) -> bytes:
        """PKT_STATE for all alive players."""
        with self.lock:
            players = list(self.players.values())
        n    = min(len(players), 255)
        data = bytes([PKT_STATE, n])
        for p in players[:n]:
            data += p.pack_wire()
        return data

# ---------------------------------------------------------------------------
# WebSocket frame codec  (RFC 6455)
# ---------------------------------------------------------------------------
WS_MAGIC = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'

def ws_handshake_response(key: str) -> bytes:
    accept = base64.b64encode(
        hashlib.sha1((key + WS_MAGIC).encode()).digest()
    ).decode()
    return (
        'HTTP/1.1 101 Switching Protocols\r\n'
        'Upgrade: websocket\r\n'
        'Connection: Upgrade\r\n'
        f'Sec-WebSocket-Accept: {accept}\r\n'
        '\r\n'
    ).encode()

def ws_encode(payload: bytes, opcode: int = 0x2) -> bytes:
    """Encode a binary (or text) frame without masking (server→client)."""
    n = len(payload)
    if n < 126:
        header = bytes([0x80 | opcode, n])
    elif n < 65536:
        header = bytes([0x80 | opcode, 126]) + struct.pack('>H', n)
    else:
        header = bytes([0x80 | opcode, 127]) + struct.pack('>Q', n)
    return header + payload

def ws_decode_frames(buf: bytearray):
    """Yield (opcode, payload) from buffer, mutate buf in place."""
    frames = []
    while len(buf) >= 2:
        b0, b1 = buf[0], buf[1]
        # fin  = (b0 & 0x80) != 0   # we ignore fragmentation for now
        opcode = b0 & 0x0F
        masked = (b1 & 0x80) != 0
        plen   = b1 & 0x7F
        idx    = 2
        if plen == 126:
            if len(buf) < 4: break
            plen = struct.unpack('>H', buf[2:4])[0]; idx = 4
        elif plen == 127:
            if len(buf) < 10: break
            plen = struct.unpack('>Q', buf[2:10])[0]; idx = 10
        mask_end = idx + (4 if masked else 0)
        if len(buf) < mask_end + plen: break
        mask_key = buf[idx:idx+4] if masked else None
        idx = mask_end
        payload = bytearray(buf[idx:idx+plen])
        if masked and mask_key:
            for i in range(len(payload)):
                payload[i] ^= mask_key[i % 4]
        frames.append((opcode, bytes(payload)))
        del buf[:idx + plen]
    return frames

# ---------------------------------------------------------------------------
# Client connection handler
# ---------------------------------------------------------------------------
class Client:
    def __init__(self, sock, addr, pid, world, broadcast):
        self.sock      = sock
        self.addr      = addr
        self.pid       = pid
        self.world     = world
        self.broadcast = broadcast
        self.player    = world.add_player(pid)
        self.alive     = True
        self._buf      = bytearray()
        self._lock     = threading.Lock()

    def send(self, data: bytes):
        try:
            with self._lock:
                self.sock.sendall(ws_encode(data))
        except Exception:
            self.alive = False

    def run(self):
        try:
            self._run()
        finally:
            self.alive = False
            self.world.remove_player(self.pid)
            try: self.sock.close()
            except Exception: pass
            log.info(f'Client {self.pid} ({self.addr}) disconnected')

    def _run(self):
        # Send assigned ID (reuse PKT_HELLO with id byte)
        self.send(struct.pack('<BB', PKT_HELLO, self.pid))
        # Hand over the authoritative map so this client (re)joins in sync
        # with any prior edits instead of generating its own default map.
        self.send(struct.pack('<B', PKT_MAP_FULL) + self.world.serialize_map())
        log.info(f'Client {self.pid} assigned ({self.addr})')

        self.sock.settimeout(60.0)
        while self.alive:
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                continue
            except Exception:
                break
            if not chunk:
                break
            self._buf.extend(chunk)
            for opcode, payload in ws_decode_frames(self._buf):
                if opcode == 0x8:   # close
                    self.alive = False; return
                if opcode == 0x9:   # ping → pong
                    self.send_raw(bytes([0x8A, 0x00]))
                if opcode == 0x2 or opcode == 0x1:  # binary or text
                    self._on_message(payload)

    def send_raw(self, data: bytes):
        try:
            with self._lock: self.sock.sendall(data)
        except Exception:
            self.alive = False

    def _on_message(self, data: bytes):
        if not data: return
        t = data[0]
        p = self.player

        if t == PKT_HELLO:
            name = data[1:17].rstrip(b'\x00').decode(errors='replace')
            p.name = name or p.name
            log.info(f'Player {self.pid} name: {p.name}')

        elif t == PKT_INPUT:
            # [seq: u16, yaw: f32, pitch: f32, keys: u8]
            if len(data) < 9: return
            _seq, yaw, pitch, keys = struct.unpack_from('<Hffb', data, 1)
            p.yaw   = yaw
            p.pitch = pitch
            p.inp_forward = bool(keys & 0x01)
            p.inp_back    = bool(keys & 0x02)
            p.inp_left    = bool(keys & 0x04)
            p.inp_right   = bool(keys & 0x08)
            p.inp_jump    = bool(keys & 0x10)
            p.editing     = bool(keys & 0x40)   # KEY_EDITING
            p.god         = bool(keys & 0x80)   # KEY_GOD

        elif t == PKT_FIRE:
            # [yaw: f32, pitch: f32, crouch: u8]
            if len(data) < 9: return
            yaw, pitch = struct.unpack_from('<ff', data, 1)
            crouch = data[9] != 0 if len(data) >= 10 else False
            p.yaw   = yaw
            p.pitch = pitch
            if p.alive:
                r = self.world.spawn_rocket(p, crouch)
                # Broadcast SPAWN_ROCKET
                pkt = struct.pack('<BBBffffff',
                    PKT_SPAWN_ROCKET,
                    r.id, r.owner_id,
                    r.pos.x, r.pos.y, r.pos.z,
                    r.vel.x, r.vel.y, r.vel.z
                )
                self.broadcast(pkt)

        elif t == PKT_EDIT_REGION:
            # [op:u8 face:u8 mat:u8 minx,miny,minz,maxx,maxy,maxz:u16]
            if len(data) < 16: return
            op, face, mat = data[1], data[2], data[3]
            minx, miny, minz, maxx, maxy, maxz = struct.unpack_from('<HHHHHH', data, 4)
            self.world.apply_edit(op, face, mat, minx, miny, minz, maxx, maxy, maxz)
            self.broadcast(struct.pack('<B', PKT_MAP_FULL) + self.world.serialize_map())

        elif t == PKT_SAVE_MAP:
            if len(data) < 2: return
            nlen = data[1]
            name = data[2:2+nlen].decode(errors='replace')
            try:
                self.world.save_map(name)
                self.send(struct.pack('<B', PKT_CONSOLE_MSG) + _console_payload(f"saved '{name}'"))
            except ValueError as e:
                self.send(struct.pack('<B', PKT_CONSOLE_MSG) + _console_payload(f'save failed: {e}'))

        elif t == PKT_LOAD_MAP:
            if len(data) < 2: return
            nlen = data[1]
            name = data[2:2+nlen].decode(errors='replace')
            try:
                if self.world.load_map(name):
                    self.broadcast(struct.pack('<B', PKT_MAP_FULL) + self.world.serialize_map())
                    self.broadcast(struct.pack('<B', PKT_CONSOLE_MSG) + _console_payload(f"loaded '{name}'"))
                else:
                    self.send(struct.pack('<B', PKT_CONSOLE_MSG) + _console_payload(f"no such map: '{name}'"))
            except ValueError as e:
                self.send(struct.pack('<B', PKT_CONSOLE_MSG) + _console_payload(f'load failed: {e}'))

        elif t == PKT_NEW_MAP:
            self.world.new_map()
            self.broadcast(struct.pack('<B', PKT_MAP_FULL) + self.world.serialize_map())
            self.broadcast(struct.pack('<B', PKT_CONSOLE_MSG) + _console_payload('map reset to default'))

        elif t == PKT_LIST_MAPS:
            names = self.world.list_maps()
            text = 'maps: ' + ', '.join(names) if names else 'maps: (none saved)'
            self.send(struct.pack('<B', PKT_CONSOLE_MSG) + _console_payload(text))

# ---------------------------------------------------------------------------
# HTTP request parser (minimal)
# ---------------------------------------------------------------------------
MIME = {
    '.html': 'text/html',
    '.js':   'application/javascript',
    '.wasm': 'application/wasm',
    '.css':  'text/css',
    '.png':  'image/png',
    '.ico':  'image/x-icon',
    '.stl':  'application/octet-stream',
}

def parse_request_line(sock) -> tuple[str,str,str] | None:
    """Read until \r\n\r\n, return (method, path, headers_dict)."""
    raw = b''
    while b'\r\n\r\n' not in raw:
        chunk = sock.recv(4096)
        if not chunk: return None
        raw += chunk
        if len(raw) > 16384: return None
    header_part = raw.split(b'\r\n\r\n')[0].decode(errors='replace')
    lines  = header_part.split('\r\n')
    parts  = lines[0].split(' ')
    if len(parts) < 2: return None
    method = parts[0]
    path   = parts[1].split('?')[0]
    hdrs   = {}
    for l in lines[1:]:
        if ':' in l:
            k,v = l.split(':',1)
            hdrs[k.strip().lower()] = v.strip()
    return method, path, hdrs

def send_http(sock, status: int, content_type: str, body: bytes,
              extra_headers: dict = None):
    reason = {200:'OK', 404:'Not Found', 405:'Method Not Allowed'}.get(status,'')
    resp  = f'HTTP/1.1 {status} {reason}\r\n'
    resp += f'Content-Type: {content_type}\r\n'
    resp += f'Content-Length: {len(body)}\r\n'
    resp += 'Connection: close\r\n'
    if extra_headers:
        for k,v in extra_headers.items():
            resp += f'{k}: {v}\r\n'
    resp += '\r\n'
    sock.sendall(resp.encode() + body)

def serve_file(sock, path: str):
    # Map URL path to filesystem
    if path == '/':
        fs_path = os.path.join(WWW_DIR, 'index.html')
    else:
        fs_path = os.path.join(WWW_DIR, path.lstrip('/'))
    # Security: no path traversal
    fs_path = os.path.realpath(fs_path)
    www_real = os.path.realpath(WWW_DIR)
    if not fs_path.startswith(www_real):
        send_http(sock, 404, 'text/plain', b'Not Found')
        return
    if not os.path.isfile(fs_path):
        send_http(sock, 404, 'text/plain', b'Not Found')
        return
    ext  = os.path.splitext(fs_path)[1].lower()
    mime = MIME.get(ext, 'application/octet-stream')
    with open(fs_path, 'rb') as f:
        body = f.read()
    extra = {'Cross-Origin-Opener-Policy': 'same-origin',
             'Cross-Origin-Embedder-Policy': 'require-corp',
             # No Cache-Control at all meant browsers could keep serving a
             # stale game.js/game.wasm after a rebuild without a hard
             # refresh — this is a dev server, always revalidate.
             'Cache-Control': 'no-cache, no-store, must-revalidate'}
    send_http(sock, 200, mime, body, extra)

# ---------------------------------------------------------------------------
# Main server
# ---------------------------------------------------------------------------
class Server:
    def __init__(self):
        self.world   = GameWorld()
        self.clients : list[Client] = []
        self.cli_lock = threading.Lock()
        self._next_pid = 1

    def broadcast(self, data: bytes):
        with self.cli_lock:
            dead = []
            for c in self.clients:
                if c.alive:
                    c.send(data)
                else:
                    dead.append(c)
            for c in dead:
                self.clients.remove(c)

    def _game_tick(self):
        while True:
            t0 = time.monotonic()
            self.world.tick(TICK_DT, self.broadcast)
            # Broadcast state to all clients
            state = self.world.build_state_packet()
            self.broadcast(state)
            elapsed = time.monotonic() - t0
            sleep_t = TICK_DT - elapsed
            if sleep_t > 0:
                time.sleep(sleep_t)

    def _handle_conn(self, sock, addr):
        sock.settimeout(10.0)
        req = parse_request_line(sock)
        if not req:
            try: sock.close()
            except: pass
            return
        method, path, hdrs = req

        # WebSocket upgrade?
        if (hdrs.get('upgrade','').lower() == 'websocket' and
                path == '/ws'):
            key = hdrs.get('sec-websocket-key','')
            if not key:
                send_http(sock, 400, 'text/plain', b'Bad Request')
                sock.close(); return
            sock.sendall(ws_handshake_response(key))
            sock.settimeout(None)
            with self.cli_lock:
                pid = self._next_pid
                self._next_pid += 1
                c = Client(sock, addr, pid, self.world, self.broadcast)
                self.clients.append(c)
            threading.Thread(target=c.run, daemon=True).start()
            return

        # HTTP file serving
        if method != 'GET':
            send_http(sock, 405, 'text/plain', b'Method Not Allowed')
        else:
            serve_file(sock, path)
        try: sock.close()
        except: pass

    def run(self):
        tick_thread = threading.Thread(target=self._game_tick, daemon=True)
        tick_thread.start()

        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((HOST, PORT))
        srv.listen(32)
        log.info(f'Server listening on http://{HOST}:{PORT}/')
        log.info(f'Serving files from: {os.path.realpath(WWW_DIR)}')

        while True:
            try:
                conn, addr = srv.accept()
            except KeyboardInterrupt:
                log.info('Shutting down.')
                break
            threading.Thread(
                target=self._handle_conn,
                args=(conn, addr),
                daemon=True
            ).start()

if __name__ == '__main__':
    Server().run()
