#!/usr/bin/env python3
"""
Pure-Python HTTP + WebSocket server.
No third-party dependencies. Uses only stdlib.

Serves:
  GET    /             → www/index.html
  GET    /game.js      → www/game.js   (emscripten glue)
  GET    /game.wasm    → www/game.wasm
  GET    /assets/*     → www/assets/*  (includes uploaded assets/library/*.glb)
  POST   /assets       → create: body is a raw .glb, ?name=&tags= as query params
  DELETE /assets/<id>  → delete: removes the DB row and the backing file
  WS     /ws           → editor/game transport + asset list/update/delete

RFC 6455 WebSocket implemented by hand.

This used to also run Qek's 20Hz authoritative FPS simulation (Player/
Rocket/GameWorld, octree-backed map persistence, PKT_INPUT/FIRE/
EDIT_REGION/SAVE_MAP/... handlers) — all removed along with the rest of
that gameplay/world code (see phi.md's Phase 1 status, "Client/server
model"). The HTTP file serving and WebSocket framing below are fully
generic and were kept as-is; the connection/handshake plumbing is kept
too, repurposed as the transport a future general multiplayer game-
authoring/gameplay layer will build on, rather than deleted.

Asset CRUD (see phi.md's "Wire protocol: CRUD over a hybrid HTTP + WS
split") is the first real thing built on that repurposed transport: Create
and the raw-bytes half of Read go over HTTP (arbitrary-size binary blobs,
the existing GET-only hand-rolled parser here extended for POST/DELETE);
List, Update, and a live Changed-broadcast go over the WS binary protocol
(small structured messages, low-latency push to every connected editor).
"""

import socket
import threading
import struct
import hashlib
import base64
import os
import re
import uuid
import logging
import urllib.parse

from assets_db import AssetDB
from anthropic_client import run_tool_loop, AnthropicError, model_display_name, DEFAULT_MODEL

logging.basicConfig(level=logging.INFO, format='[%(levelname)s] %(message)s')
log = logging.getLogger('server')

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
HOST             = '0.0.0.0'
PORT             = 8765
WWW_DIR          = os.path.join(os.path.dirname(__file__), '..', 'www')
# assets/ lives at the project root, a sibling of www/, NOT inside it --
# native/win32/wasm clients all read straight from this directory (native/
# win32 via a plain fopen relative to their own cwd, wasm via its
# embedded FS baked from this exact directory, see the Makefile's
# --embed-file assets@assets), the same way the existing hand-authored
# assets/cube.gltf test asset already does. Uploaded assets need to land
# somewhere those clients can actually find them, so this has to be here,
# not a www/assets/ that nothing client-side ever actually reads from.
ASSETS_ROOT_DIR  = os.path.join(os.path.dirname(__file__), '..', 'assets')
ASSET_LIBRARY_DIR = os.path.join(ASSETS_ROOT_DIR, 'library')
ASSETS_DB_PATH   = os.path.join(os.path.dirname(__file__), 'assets.db')
MAX_ASSET_BYTES  = 64 * 1024 * 1024

# ---------------------------------------------------------------------------
# Binary protocol constants  (must match client/net.h)
# ---------------------------------------------------------------------------
PKT_HELLO              = 0x01   # C->S: [name:16 bytes, NUL-padded]. S->C: [id:u8]
PKT_CONSOLE_MSG        = 0x0D   # S->C: [len:u16 utf8:bytes], appended to the client's Python panel log
PKT_ASSET_LIST_REQUEST = 0x10   # C->S: [qlen:u8 query:bytes] (qlen=0 = no filter)
PKT_ASSET_LIST_REPLY   = 0x11   # S->C: [count:u16] then count * {id:u32 name_len:u8 name path_len:u8 path tags_len:u8 tags(csv)}
PKT_ASSET_UPDATE       = 0x12   # C->S: [id:u32 name_len:u8 name tags_len:u8 tags(csv)]
PKT_ASSET_DELETE       = 0x13   # C->S: [id:u32]
PKT_ASSET_CHANGED      = 0x14   # S->C: no payload -- "the asset list changed, re-request if you care"

# Chat + live introspection -- see client/net.h's own comment on these four
# for the full rationale (why WS not HTTP, why req_id round-trips).
PKT_CHAT_MSG             = 0x20   # C->S: [len:u16 utf8:bytes]
PKT_CHAT_REPLY           = 0x21   # S->C: [len:u16 utf8:bytes]
PKT_SCENE_STATE_REQUEST  = 0x22   # S->C: [req_id:u32]
PKT_SCENE_STATE_REPLY    = 0x23   # C->S: [req_id:u32 len:u16 utf8-json:bytes]

# Chat-driven scene MUTATION -- see client/net.h's own comment on these
# six. Claude's access to lights/render settings is deliberately read-
# WRITE (unlike get_asset_list/get_scene_state, which stay read-only),
# per this project's own explicit decision.
PKT_PROP_SET_REQUEST     = 0x24   # S->C: [req_id:u32 target_len:u8 target:bytes ident_len:u8 ident:bytes is_vec3:u8 v0:f32 v1:f32 v2:f32]
PKT_PROP_SET_REPLY       = 0x25   # C->S: [req_id:u32 ok:u8]
PKT_ADD_LIGHT_REQUEST    = 0x26   # S->C: [req_id:u32 type:u8 x:f32 y:f32 z:f32]
PKT_ADD_LIGHT_REPLY      = 0x27   # C->S: [req_id:u32 ok:u8 light_id:u32]
PKT_DELETE_LIGHT_REQUEST = 0x28   # S->C: [req_id:u32 light_id:u32]
PKT_DELETE_LIGHT_REPLY   = 0x29   # C->S: [req_id:u32 ok:u8]

# Phase 5's real geometry-creation/vertex-editing tools (see client/net.h's
# matching comment) -- Claude's access here is deliberately read-WRITE too,
# same explicit project decision as the light/render-settings tools above,
# extended per later explicit request ("make it so Claude can create
# geometry and redefine the verts of existing scene geometry").
PKT_CREATE_MESH_REQUEST        = 0x2A   # S->C: [req_id:u32 x:f32 y:f32 z:f32 vert_count:u16 (vert_count*3)*f32 tri_count:u16 (tri_count*3)*u16]
PKT_CREATE_MESH_REPLY          = 0x2B   # C->S: [req_id:u32 ok:u8 object_id:u32]
PKT_SET_VERTICES_REQUEST       = 0x2C   # S->C: [req_id:u32 object_id:u32 vert_count:u16 (vert_count*3)*f32]
PKT_SET_VERTICES_REPLY         = 0x2D   # C->S: [req_id:u32 ok:u8]
PKT_DELETE_MESH_OBJECT_REQUEST = 0x2E   # S->C: [req_id:u32 object_id:u32]
PKT_DELETE_MESH_OBJECT_REPLY   = 0x2F   # C->S: [req_id:u32 ok:u8]

# Incremental mesh editing (see client/net.h's matching comment) -- "the
# model can see where vertices are, add vertices to an existing mesh, move
# individual vertices". GET_MESH_VERTICES/GET_MESH_FACES are the read side
# create_mesh_object/set_mesh_vertices never provided (build-from-scratch
# or replace-everything-at-the-same-count, never "what's there now").
PKT_GET_MESH_VERTICES_REQUEST  = 0x30   # S->C: [req_id:u32 object_id:u32]
PKT_GET_MESH_VERTICES_REPLY    = 0x31   # C->S: [req_id:u32 ok:u8 vert_count:u16 (vert_count*3)*f32]
PKT_GET_MESH_FACES_REQUEST     = 0x32   # S->C: [req_id:u32 object_id:u32]
PKT_GET_MESH_FACES_REPLY       = 0x33   # C->S: [req_id:u32 ok:u8 face_count:u16 face_count*{face_index:u16 v0:u16 v1:u16 v2:u16}]
PKT_ADD_MESH_VERTEX_REQUEST    = 0x34   # S->C: [req_id:u32 object_id:u32 x:f32 y:f32 z:f32]
PKT_ADD_MESH_VERTEX_REPLY      = 0x35   # C->S: [req_id:u32 ok:u8 vertex_index:u32]
PKT_ADD_MESH_FACE_REQUEST      = 0x36   # S->C: [req_id:u32 object_id:u32 v0:u16 v1:u16 v2:u16]
PKT_ADD_MESH_FACE_REPLY        = 0x37   # C->S: [req_id:u32 ok:u8 face_index:u32]
PKT_SET_MESH_VERTEX_REQUEST    = 0x38   # S->C: [req_id:u32 object_id:u32 vertex_index:u16 x:f32 y:f32 z:f32]
PKT_SET_MESH_VERTEX_REPLY      = 0x39   # C->S: [req_id:u32 ok:u8]

# Same wire-protocol bounds as client/net.h's own PKT_MESH_MAX_VERTS/
# PKT_MESH_MAX_TRIS -- must match exactly, or a request this server
# considers valid could get silently refused (or worse, misparsed) by the
# client's own bounds check.
PKT_MESH_MAX_VERTS = 2048
PKT_MESH_MAX_TRIS  = 4096

ASSET_NAME_WIRE_MAX = 63
ASSET_PATH_WIRE_MAX = 127
ASSET_TAGS_WIRE_MAX = 95


def _console_payload(text: str) -> bytes:
    b = text.encode('utf-8')
    return struct.pack('<H', len(b)) + b

# ---------------------------------------------------------------------------
# Asset packet encode/decode
# ---------------------------------------------------------------------------
def _lenprefixed(s: str, max_len: int) -> bytes:
    b = s.encode('utf-8')[:max_len]
    return bytes([len(b)]) + b

def pack_asset_list_reply(assets: list[dict]) -> bytes:
    out = bytearray([PKT_ASSET_LIST_REPLY])
    out += struct.pack('<H', len(assets))
    for a in assets:
        out += struct.pack('<I', a['id'])
        out += _lenprefixed(a['name'], ASSET_NAME_WIRE_MAX)
        out += _lenprefixed(a['path'], ASSET_PATH_WIRE_MAX)
        out += _lenprefixed(','.join(a['tags']), ASSET_TAGS_WIRE_MAX)
    return bytes(out)

def _parse_list_request(payload: bytes) -> str:
    if len(payload) < 1: return ''
    qlen = payload[0]
    return payload[1:1+qlen].decode(errors='replace')

def _parse_asset_update(payload: bytes):
    if len(payload) < 5: return None
    asset_id = struct.unpack_from('<I', payload, 0)[0]
    off = 4
    name_len = payload[off]; off += 1
    name = payload[off:off+name_len].decode(errors='replace'); off += name_len
    if off >= len(payload): return None
    tags_len = payload[off]; off += 1
    tags_csv = payload[off:off+tags_len].decode(errors='replace')
    tags = [t for t in tags_csv.split(',') if t]
    return asset_id, name, tags

def _parse_asset_delete(payload: bytes):
    if len(payload) < 4: return None
    return struct.unpack_from('<I', payload, 0)[0]

# ---------------------------------------------------------------------------
# Chat + scene-state packet encode/decode (see PKT_CHAT_*/PKT_SCENE_STATE_*
# above and client/net.h's matching comment)
# ---------------------------------------------------------------------------
def _lenprefixed16(s: str) -> bytes:
    b = s.encode('utf-8')[:65535]
    return struct.pack('<H', len(b)) + b

def pack_chat_reply(text: str) -> bytes:
    return bytes([PKT_CHAT_REPLY]) + _lenprefixed16(text)

def pack_scene_state_request(req_id: int) -> bytes:
    return bytes([PKT_SCENE_STATE_REQUEST]) + struct.pack('<I', req_id)

def _parse_chat_msg(payload: bytes) -> str | None:
    if len(payload) < 2: return None
    slen = struct.unpack_from('<H', payload, 0)[0]
    return payload[2:2+slen].decode(errors='replace')

def _parse_scene_state_reply(payload: bytes):
    if len(payload) < 6: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    slen = struct.unpack_from('<H', payload, 4)[0]
    text = payload[6:6+slen].decode(errors='replace')
    return req_id, text

# ---------------------------------------------------------------------------
# Chat-driven scene mutation packet encode/decode (see PKT_PROP_SET_*/
# PKT_ADD_LIGHT_*/PKT_DELETE_LIGHT_* above and client/net.h's matching
# comment) -- three requests, sharing Client's existing req_id/Event
# pending-reply machinery (see Client._send_and_wait) rather than each
# growing its own copy.
# ---------------------------------------------------------------------------
def pack_prop_set_request(req_id: int, target: str, identifier: str, is_vec3: bool,
                           v0: float, v1: float, v2: float) -> bytes:
    tb = target.encode('utf-8')[:63]
    ib = identifier.encode('utf-8')[:31]
    return (bytes([PKT_PROP_SET_REQUEST]) + struct.pack('<I', req_id) +
            bytes([len(tb)]) + tb + bytes([len(ib)]) + ib +
            bytes([1 if is_vec3 else 0]) + struct.pack('<fff', v0, v1, v2))

def _parse_prop_set_reply(payload: bytes):
    if len(payload) < 5: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    return req_id, payload[4] != 0

def pack_add_light_request(req_id: int, light_type: int, x: float, y: float, z: float) -> bytes:
    return bytes([PKT_ADD_LIGHT_REQUEST]) + struct.pack('<IBfff', req_id, light_type, x, y, z)

def _parse_add_light_reply(payload: bytes):
    if len(payload) < 9: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    ok = payload[4] != 0
    light_id = struct.unpack_from('<I', payload, 5)[0]
    return req_id, ok, light_id

def pack_delete_light_request(req_id: int, light_id: int) -> bytes:
    return bytes([PKT_DELETE_LIGHT_REQUEST]) + struct.pack('<II', req_id, light_id)

def _parse_delete_light_reply(payload: bytes):
    if len(payload) < 5: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    return req_id, payload[4] != 0

# ---------------------------------------------------------------------------
# Phase 5's geometry-creation/vertex-editing packet encode/decode (see
# PKT_CREATE_MESH_*/PKT_SET_VERTICES_*/PKT_DELETE_MESH_OBJECT_* above and
# client/net.h's matching comment) -- same shared req_id/Event pending-
# reply machinery as the light/render-settings packets just above.
# ---------------------------------------------------------------------------
def pack_create_mesh_request(req_id: int, x: float, y: float, z: float,
                              positions: list[float], indices: list[int]) -> bytes:
    if len(positions) % 3 != 0:
        raise ValueError('positions must be a flat x,y,z sequence (length a multiple of 3)')
    if len(indices) % 3 != 0:
        raise ValueError('indices must be a flat triangle-triple sequence (length a multiple of 3)')
    vert_count = len(positions) // 3
    tri_count = len(indices) // 3
    if vert_count > PKT_MESH_MAX_VERTS:
        raise ValueError(f'too many vertices ({vert_count} > {PKT_MESH_MAX_VERTS})')
    if tri_count > PKT_MESH_MAX_TRIS:
        raise ValueError(f'too many triangles ({tri_count} > {PKT_MESH_MAX_TRIS})')
    out = bytearray([PKT_CREATE_MESH_REQUEST])
    out += struct.pack('<Ifff', req_id, x, y, z)
    out += struct.pack('<H', vert_count)
    out += struct.pack(f'<{len(positions)}f', *positions)
    out += struct.pack('<H', tri_count)
    out += struct.pack(f'<{len(indices)}H', *indices)
    return bytes(out)

def _parse_create_mesh_reply(payload: bytes):
    if len(payload) < 9: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    ok = payload[4] != 0
    object_id = struct.unpack_from('<I', payload, 5)[0]
    return req_id, ok, object_id

def pack_set_vertices_request(req_id: int, object_id: int, positions: list[float]) -> bytes:
    if len(positions) % 3 != 0:
        raise ValueError('positions must be a flat x,y,z sequence (length a multiple of 3)')
    vert_count = len(positions) // 3
    if vert_count > PKT_MESH_MAX_VERTS:
        raise ValueError(f'too many vertices ({vert_count} > {PKT_MESH_MAX_VERTS})')
    out = bytearray([PKT_SET_VERTICES_REQUEST])
    out += struct.pack('<II', req_id, object_id)
    out += struct.pack('<H', vert_count)
    out += struct.pack(f'<{len(positions)}f', *positions)
    return bytes(out)

def _parse_set_vertices_reply(payload: bytes):
    if len(payload) < 5: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    return req_id, payload[4] != 0

def pack_delete_mesh_object_request(req_id: int, object_id: int) -> bytes:
    return bytes([PKT_DELETE_MESH_OBJECT_REQUEST]) + struct.pack('<II', req_id, object_id)

def _parse_delete_mesh_object_reply(payload: bytes):
    if len(payload) < 5: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    return req_id, payload[4] != 0

# ---------------------------------------------------------------------------
# Incremental mesh editing packet encode/decode (see PKT_GET_MESH_VERTICES_*/
# PKT_GET_MESH_FACES_*/PKT_ADD_MESH_VERTEX_*/PKT_ADD_MESH_FACE_*/PKT_SET_MESH_
# VERTEX_* above and client/net.h's matching comment).
# ---------------------------------------------------------------------------
def pack_get_mesh_vertices_request(req_id: int, object_id: int) -> bytes:
    return bytes([PKT_GET_MESH_VERTICES_REQUEST]) + struct.pack('<II', req_id, object_id)

def _parse_get_mesh_vertices_reply(payload: bytes):
    if len(payload) < 7: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    ok = payload[4] != 0
    vert_count = struct.unpack_from('<H', payload, 5)[0]
    if len(payload) < 7 + vert_count * 3 * 4: return None
    positions = list(struct.unpack_from(f'<{vert_count * 3}f', payload, 7)) if vert_count else []
    return req_id, ok, positions

def pack_get_mesh_faces_request(req_id: int, object_id: int) -> bytes:
    return bytes([PKT_GET_MESH_FACES_REQUEST]) + struct.pack('<II', req_id, object_id)

def _parse_get_mesh_faces_reply(payload: bytes):
    if len(payload) < 7: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    ok = payload[4] != 0
    face_count = struct.unpack_from('<H', payload, 5)[0]
    if len(payload) < 7 + face_count * 8: return None
    faces = []
    off = 7
    for _ in range(face_count):
        face_index, v0, v1, v2 = struct.unpack_from('<HHHH', payload, off)
        faces.append((face_index, (v0, v1, v2)))
        off += 8
    return req_id, ok, faces

def pack_add_mesh_vertex_request(req_id: int, object_id: int, x: float, y: float, z: float) -> bytes:
    return bytes([PKT_ADD_MESH_VERTEX_REQUEST]) + struct.pack('<IIfff', req_id, object_id, x, y, z)

def _parse_add_mesh_vertex_reply(payload: bytes):
    if len(payload) < 9: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    ok = payload[4] != 0
    vertex_index = struct.unpack_from('<I', payload, 5)[0]
    return req_id, ok, vertex_index

def pack_add_mesh_face_request(req_id: int, object_id: int, v0: int, v1: int, v2: int) -> bytes:
    return bytes([PKT_ADD_MESH_FACE_REQUEST]) + struct.pack('<IIHHH', req_id, object_id, v0, v1, v2)

def _parse_add_mesh_face_reply(payload: bytes):
    if len(payload) < 9: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    ok = payload[4] != 0
    face_index = struct.unpack_from('<I', payload, 5)[0]
    return req_id, ok, face_index

def pack_set_mesh_vertex_request(req_id: int, object_id: int, vertex_index: int, x: float, y: float, z: float) -> bytes:
    return bytes([PKT_SET_MESH_VERTEX_REQUEST]) + struct.pack('<IIHfff', req_id, object_id, vertex_index, x, y, z)

def _parse_set_mesh_vertex_reply(payload: bytes):
    if len(payload) < 5: return None
    req_id = struct.unpack_from('<I', payload, 0)[0]
    return req_id, payload[4] != 0

def _slugify(name: str) -> str:
    s = re.sub(r'[^a-zA-Z0-9]+', '_', name).strip('_').lower()
    return s or 'asset'

def _is_glb(data: bytes) -> bool:
    """GLB container magic: 4 bytes 'glTF' + u32 version + u32 total length,
    see the Khronos glTF 2.0 binary format spec -- cgltf.h's own parser
    (client/cgltf.h, cgltf_file_type_glb) checks the same 4 bytes."""
    return len(data) >= 12 and data[0:4] == b'glTF'

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
# Shared asset delete (used by both the HTTP DELETE endpoint and the WS
# PKT_ASSET_DELETE handler, so file-removal/DB-removal only happens in one
# place)
# ---------------------------------------------------------------------------
def _delete_asset_and_file(assets_db: AssetDB, asset_id: int) -> bool:
    rel_path = assets_db.delete_asset(asset_id)
    if rel_path is None:
        return False
    # rel_path is always "assets/library/<file>" (see _handle_asset_create),
    # relative to the project root -- same root ASSETS_ROOT_DIR is defined
    # against, not WWW_DIR (see ASSETS_ROOT_DIR's comment for why).
    project_root = os.path.join(os.path.dirname(__file__), '..')
    fs_path = os.path.realpath(os.path.join(project_root, rel_path))
    assets_real = os.path.realpath(ASSETS_ROOT_DIR)
    if fs_path.startswith(assets_real) and os.path.isfile(fs_path):
        try: os.remove(fs_path)
        except OSError: pass
    return True

# ---------------------------------------------------------------------------
# Client connection handler
# ---------------------------------------------------------------------------
class Client:
    def __init__(self, sock, addr, pid, broadcast, assets_db: AssetDB):
        self.sock      = sock
        self.addr      = addr
        self.pid       = pid
        self.name      = f'player{pid}'
        self.broadcast = broadcast
        self.assets_db = assets_db
        self.alive     = True
        self._buf      = bytearray()
        self._lock     = threading.Lock()

        # Pending request/reply tool calls (get_scene_state, and now the
        # read-write light/render-settings tools too -- see PKT_PROP_SET_*/
        # PKT_ADD_LIGHT_*/PKT_DELETE_LIGHT_* above): a chat-handling thread
        # blocks on an Event here while this connection's own read loop (a
        # DIFFERENT thread, see run()) waits for the matching *_REPLY and
        # wakes it. req_id disambiguates in case a second request goes out
        # before the first is answered -- shared across ALL FOUR request
        # types (one counter, one dict), since req_ids only ever need to be
        # unique per-client, not per-request-type.
        self._pending_lock    = threading.Lock()
        self._pending_events  = {}   # req_id -> threading.Event
        self._pending_results = {}   # req_id -> whatever that request type's reply carries (str/bool/tuple), or None if never arrived
        self._next_req_id     = 1

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
            try: self.sock.close()
            except Exception: pass
            log.info(f'Client {self.pid} ({self.addr}) disconnected')

    def _run(self):
        self.send(struct.pack('<BB', PKT_HELLO, self.pid))
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

    def _alloc_req_id(self) -> int:
        with self._pending_lock:
            req_id = self._next_req_id
            self._next_req_id += 1
        return req_id

    def _send_and_wait(self, req_id: int, packet: bytes, timeout: float = 5.0):
        """Sends an already-packed request (req_id must already be baked
        into it) and blocks (called from the chat-handling thread, never
        the read-loop thread) until the matching *_REPLY arrives or timeout
        elapses. Returns whatever _on_message's handler for that reply type
        stashed into _pending_results, or None on timeout/disconnect --
        shared by request_scene_state/set_prop/add_light/delete_light
        below rather than each hand-rolling this same wait dance."""
        ev = threading.Event()
        with self._pending_lock:
            self._pending_events[req_id] = ev
        self.send(packet)
        got = ev.wait(timeout)
        with self._pending_lock:
            self._pending_events.pop(req_id, None)
            result = self._pending_results.pop(req_id, None)
        return result if got else None

    def _resolve_pending(self, req_id: int, value):
        """Stashes value for req_id and wakes whichever chat-handling
        thread is blocked waiting on it in _send_and_wait -- shared by
        all four *_REPLY handlers in _on_message below rather than each
        repeating this same lock/set dance."""
        with self._pending_lock:
            ev = self._pending_events.get(req_id)
            if ev:
                self._pending_results[req_id] = value
                ev.set()

    def request_scene_state(self, timeout: float = 5.0):
        """Returns the JSON text, or None on timeout/disconnect."""
        req_id = self._alloc_req_id()
        return self._send_and_wait(req_id, pack_scene_state_request(req_id), timeout)

    _LIGHT_TYPE_TO_WIRE = {'point': 0, 'sun': 1, 'spot': 2, 'area': 3}

    def set_prop(self, target: str, identifier: str, value, timeout: float = 5.0):
        """target is 'light:<id>' or 'render' (scene_target.c's own
        resolver strings). value is a float/int for a scalar prop, or a
        3-element sequence for a VEC3 prop (color/position/direction).
        Returns True/False (whether the client accepted it -- an unknown
        target/identifier or a shape mismatch comes back False, not an
        exception), or None on timeout/disconnect."""
        is_vec3 = isinstance(value, (list, tuple))
        if is_vec3:
            v0, v1, v2 = float(value[0]), float(value[1]), float(value[2])
        else:
            v0, v1, v2 = float(value), 0.0, 0.0
        req_id = self._alloc_req_id()
        packet = pack_prop_set_request(req_id, target, identifier, is_vec3, v0, v1, v2)
        return self._send_and_wait(req_id, packet, timeout)

    def add_light(self, light_type: str, x: float, y: float, z: float, timeout: float = 5.0):
        """Returns (ok, light_id) -- light_id is 0 on failure (registry
        full) -- or None on timeout/disconnect."""
        req_id = self._alloc_req_id()
        wire_type = self._LIGHT_TYPE_TO_WIRE.get(light_type, 0)
        packet = pack_add_light_request(req_id, wire_type, x, y, z)
        return self._send_and_wait(req_id, packet, timeout)

    def delete_light(self, light_id: int, timeout: float = 5.0):
        """Returns True/False (False if no light with that id exists), or
        None on timeout/disconnect."""
        req_id = self._alloc_req_id()
        packet = pack_delete_light_request(req_id, light_id)
        return self._send_and_wait(req_id, packet, timeout)

    def create_mesh_object(self, x: float, y: float, z: float,
                            positions: list[float], indices: list[int], timeout: float = 5.0):
        """Builds a real MeshObject from scratch in the running client's
        live scene, from a flat (x,y,z,...) position list and a flat
        triangle-index list -- no file, no pre-authored asset. Returns
        (ok, object_id) -- object_id is 0 on failure (bad topology, or
        the scene is full), or None on timeout/disconnect. Raises
        ValueError itself (before even talking to the client) if
        positions/indices are malformed or exceed PKT_MESH_MAX_VERTS/
        PKT_MESH_MAX_TRIS."""
        req_id = self._alloc_req_id()
        packet = pack_create_mesh_request(req_id, x, y, z, positions, indices)
        return self._send_and_wait(req_id, packet, timeout)

    def set_mesh_vertices(self, object_id: int, positions: list[float], timeout: float = 5.0):
        """Redefines EVERY vertex position of an existing object's live
        geometry -- same vertex COUNT as whatever's already there (this
        rewrites positions, not topology). Returns True/False (False if
        no such object exists, it has no editable geometry, or the count
        doesn't match), or None on timeout/disconnect."""
        req_id = self._alloc_req_id()
        packet = pack_set_vertices_request(req_id, object_id, positions)
        return self._send_and_wait(req_id, packet, timeout)

    def delete_mesh_object(self, object_id: int, timeout: float = 5.0):
        """Returns True/False (False if no object with that id exists), or
        None on timeout/disconnect."""
        req_id = self._alloc_req_id()
        packet = pack_delete_mesh_object_request(req_id, object_id)
        return self._send_and_wait(req_id, packet, timeout)

    def get_mesh_vertices(self, object_id: int, timeout: float = 5.0):
        """Returns (ok, positions) -- positions is a flat [x0,y0,z0,...]
        list (empty if ok is False: no such object, or it has no editable
        geometry), or None on timeout/disconnect."""
        req_id = self._alloc_req_id()
        packet = pack_get_mesh_vertices_request(req_id, object_id)
        result = self._send_and_wait(req_id, packet, timeout)
        return (result[1], result[2]) if result else None

    def get_mesh_faces(self, object_id: int, timeout: float = 5.0):
        """Returns (ok, faces) -- faces is a list of (face_index,
        (v0, v1, v2)) pairs (face_index is what add_mesh_face/a future
        delete would need, NOT just this list's position -- see net.h),
        or None on timeout/disconnect."""
        req_id = self._alloc_req_id()
        packet = pack_get_mesh_faces_request(req_id, object_id)
        result = self._send_and_wait(req_id, packet, timeout)
        return (result[1], result[2]) if result else None

    def add_mesh_vertex(self, object_id: int, x: float, y: float, z: float, timeout: float = 5.0):
        """Adds ONE new vertex to an EXISTING object's live geometry
        (unlike create_mesh_object, which builds a whole new object).
        Returns (ok, vertex_index) -- the new vertex is invisible until a
        face references it, see add_mesh_face -- or None on timeout/
        disconnect."""
        req_id = self._alloc_req_id()
        packet = pack_add_mesh_vertex_request(req_id, object_id, x, y, z)
        return self._send_and_wait(req_id, packet, timeout)

    def add_mesh_face(self, object_id: int, v0: int, v1: int, v2: int, timeout: float = 5.0):
        """Adds ONE new triangular face to an existing object, referencing
        3 vertex indices that must already exist (from the object's
        original geometry or a prior add_mesh_vertex call). Returns (ok,
        face_index), or None on timeout/disconnect."""
        req_id = self._alloc_req_id()
        packet = pack_add_mesh_face_request(req_id, object_id, v0, v1, v2)
        return self._send_and_wait(req_id, packet, timeout)

    def set_mesh_vertex(self, object_id: int, vertex_index: int, x: float, y: float, z: float, timeout: float = 5.0):
        """Moves ONE existing vertex by index -- unlike set_mesh_vertices,
        which replaces the whole array at once, this is the tool for
        nudging a single corner. Returns True/False (False if no such
        object/vertex), or None on timeout/disconnect."""
        req_id = self._alloc_req_id()
        packet = pack_set_mesh_vertex_request(req_id, object_id, vertex_index, x, y, z)
        return self._send_and_wait(req_id, packet, timeout)

    def _handle_chat(self, user_text: str):
        """Runs on its own thread (spawned by _on_message on PKT_CHAT_MSG)
        since a real Anthropic round trip -- itself potentially blocking on
        a get_scene_state round trip back to this same client -- must never
        stall the connection's read loop (that loop is what would deliver
        the PKT_SCENE_STATE_REPLY this thread is waiting on in the first
        place; running inline would deadlock)."""

        def tool_get_asset_list(_input):
            assets = self.assets_db.list_assets(None)
            if not assets:
                return '(no assets uploaded yet)'
            return '\n'.join(
                f"#{a['id']} {a['name']} tags={','.join(a['tags']) or '(none)'}"
                for a in assets
            )

        def tool_get_scene_state(_input):
            result = self.request_scene_state()
            if result is None:
                return '(the client did not respond to the scene-state request in time -- it may be disconnected or busy)'
            return result

        # Read-write light/render-settings/mesh-geometry tools (Phase 3
        # and Phase 5, see phi.md) -- unlike the two read-only tools
        # above, these actually mutate the live client's scene. Explicit
        # project decision: Claude's access to lights/render settings/
        # mesh geometry is read-write (mesh geometry per later explicit
        # request -- "make it so Claude can create geometry and redefine
        # the verts of existing scene geometry"); it still can never post
        # a chat message AS the user (PKT_CHAT_MSG only ever originates
        # from the real human client-side, see chat.c) and has no access
        # to the Asset Browser's library beyond the read-only get_asset_
        # list.
        def tool_add_light(input_):
            light_type = input_.get('type', 'point')
            if light_type not in ('point', 'sun', 'spot', 'area'):
                return f"invalid type {light_type!r} -- must be 'point', 'sun', 'spot', or 'area'"
            x = float(input_.get('x', 0.0)); y = float(input_.get('y', 0.0)); z = float(input_.get('z', 0.0))
            result = self.add_light(light_type, x, y, z)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            ok, light_id = result
            return f'created light id={light_id}' if ok else 'failed -- the light registry is full (PHI_MAX_LIGHTS)'

        def tool_set_light_property(input_):
            light_id = int(input_['light_id'])
            prop = input_['property']
            value = input_['value']
            result = self.set_prop(f'light:{light_id}', prop, value)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            return 'ok' if result else (
                f'failed -- either no light with id {light_id} exists, {prop!r} is not a real light '
                'property, or the value shape (single number vs. [r,g,b]) is wrong for that property'
            )

        def tool_delete_light(input_):
            light_id = int(input_['light_id'])
            result = self.delete_light(light_id)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            return 'ok' if result else f'failed -- no light with id {light_id}'

        def tool_set_render_samples(input_):
            samples = int(input_['samples'])
            result = self.set_prop('render', 'samples', samples)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            return 'ok' if result else 'failed to set the sample count'

        # Phase 5's real geometry-creation/vertex-editing tools (see
        # phi.md) -- per later explicit request, mesh objects are no
        # longer read-only from chat either. create_mesh_object/
        # set_mesh_vertices/delete_mesh_object mutate the running
        # client's live scene the same real way add_light/set_light_
        # property/delete_light already do.
        def tool_create_mesh_object(input_):
            x = float(input_.get('x', 0.0)); y = float(input_.get('y', 0.0)); z = float(input_.get('z', 0.0))
            positions = [float(v) for v in input_['positions']]
            indices = [int(v) for v in input_['indices']]
            try:
                result = self.create_mesh_object(x, y, z, positions, indices)
            except ValueError as e:
                return f'invalid request -- {e}'
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            ok, object_id = result
            return f'created MeshObject id={object_id}' if ok else 'failed -- bad mesh topology, or the scene is full'

        def tool_set_mesh_vertices(input_):
            object_id = int(input_['object_id'])
            positions = [float(v) for v in input_['positions']]
            try:
                result = self.set_mesh_vertices(object_id, positions)
            except ValueError as e:
                return f'invalid request -- {e}'
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            return 'ok' if result else (
                f'failed -- either no MeshObject with id {object_id} exists, or the position count doesn\'t '
                'match its existing vertex count (use get_scene_state\'s objects[].vert_count to check first)'
            )

        def tool_delete_mesh_object(input_):
            object_id = int(input_['object_id'])
            result = self.delete_mesh_object(object_id)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            return 'ok' if result else f'failed -- no MeshObject with id {object_id}'

        # Incremental mesh editing tools (see phi.md -- "the model can see
        # where vertices are, add vertices to an existing mesh, move
        # individual vertices"). Distinct from create_mesh_object/
        # set_mesh_vertices above: those replace a whole object (build
        # from scratch, or same-vertex-count rewrite); these grow/nudge
        # EXISTING geometry one piece at a time.
        def tool_get_mesh_vertices(input_):
            object_id = int(input_['object_id'])
            result = self.get_mesh_vertices(object_id)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            ok, positions = result
            if not ok:
                return f'failed -- no MeshObject with id {object_id}, or it has no editable geometry'
            n = len(positions) // 3
            if n == 0:
                return '(0 vertices)'
            return f'{n} vertices: ' + ', '.join(
                f'{i}=({positions[i*3]:.3f},{positions[i*3+1]:.3f},{positions[i*3+2]:.3f})'
                for i in range(n)
            )

        def tool_get_mesh_faces(input_):
            object_id = int(input_['object_id'])
            result = self.get_mesh_faces(object_id)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            ok, faces = result
            if not ok:
                return f'failed -- no MeshObject with id {object_id}, or it has no editable geometry'
            if not faces:
                return '(0 live faces)'
            return f'{len(faces)} faces: ' + ', '.join(f'{fi}=({v[0]},{v[1]},{v[2]})' for fi, v in faces)

        def tool_add_mesh_vertex(input_):
            object_id = int(input_['object_id'])
            x = float(input_['x']); y = float(input_['y']); z = float(input_['z'])
            result = self.add_mesh_vertex(object_id, x, y, z)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            ok, vertex_index = result
            if not ok:
                return f'failed -- no MeshObject with id {object_id}, or it has no editable geometry'
            return f'added vertex_index={vertex_index} -- invisible until a face references it, call add_mesh_face next'

        def tool_add_mesh_face(input_):
            object_id = int(input_['object_id'])
            v0 = int(input_['v0']); v1 = int(input_['v1']); v2 = int(input_['v2'])
            result = self.add_mesh_face(object_id, v0, v1, v2)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            ok, face_index = result
            return f'created face_index={face_index}' if ok else (
                f'failed -- either no MeshObject with id {object_id} exists, or one of '
                f'{v0},{v1},{v2} is not a valid existing vertex index (check get_mesh_vertices first)'
            )

        def tool_set_mesh_vertex(input_):
            object_id = int(input_['object_id'])
            vertex_index = int(input_['vertex_index'])
            x = float(input_['x']); y = float(input_['y']); z = float(input_['z'])
            result = self.set_mesh_vertex(object_id, vertex_index, x, y, z)
            if result is None:
                return '(the client did not respond in time -- it may be disconnected or busy)'
            return 'ok' if result else (
                f'failed -- either no MeshObject with id {object_id} exists, or vertex_index '
                f'{vertex_index} is out of range (check get_mesh_vertices first)'
            )

        tools = [
            {
                'name': 'get_asset_list',
                'description': (
                    "List every asset currently in this project's asset "
                    "library (id, name, tags). Server-side data -- answers "
                    "immediately, doesn't need the live client to respond."
                ),
                'input_schema': {'type': 'object', 'properties': {}},
            },
            {
                'name': 'get_scene_state',
                'description': (
                    "Query the LIVE state of the specific running editor "
                    "client that sent this chat message: an 'objects' "
                    "array (every MeshObject currently in the scene -- id, "
                    "position/orientation/is_static, vertex/face counts, "
                    "whether it has a physics body and its velocity if "
                    "so), which object_id is currently selected, the "
                    "currently-selected object's ray-picked face's PBR "
                    "material (if any face is selected), the live 'lights' "
                    "array, render settings, and the physics world's "
                    "gravity. This is real introspection of a running "
                    "process, not a cached snapshot -- use it whenever the "
                    "user asks about the current state of their scene "
                    "rather than guessing, and ALWAYS call it before "
                    "create_mesh_object/set_mesh_vertices/delete_mesh_"
                    "object if you don't already know the relevant "
                    "object_id or vertex count."
                ),
                'input_schema': {'type': 'object', 'properties': {}},
            },
            {
                'name': 'add_light',
                'description': (
                    "Spawn a new Light object (Point, Sun, Spot, or Area -- "
                    "same four types Blender has) in the running client's "
                    "live scene, at a given world position. Returns the new "
                    "light's id, needed for set_light_property/delete_light "
                    "afterward. The new light gets reasonable per-type "
                    "defaults (color, energy, etc.) -- use set_light_property "
                    "to change them."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'type': {'type': 'string', 'enum': ['point', 'sun', 'spot', 'area']},
                        'x': {'type': 'number'}, 'y': {'type': 'number'}, 'z': {'type': 'number'},
                    },
                    'required': ['type', 'x', 'y', 'z'],
                },
            },
            {
                'name': 'set_light_property',
                'description': (
                    "Change one property of an existing light in the "
                    "running client's live scene. Valid property names: "
                    "'type' (0=point,1=sun,2=spot,3=area -- an integer, "
                    "changes what kind of light it is), 'position' "
                    "([x,y,z]), 'direction' ([x,y,z], only meaningful for "
                    "sun/spot), 'color' ([r,g,b], each usually 0-1), "
                    "'energy' (a single number, the light's power/"
                    "strength), 'radius' (point lights only), 'spot_size' "
                    "and 'spot_blend' (spot lights only, spot_size in "
                    "radians), 'area_size' (area lights only), 'sun_angle' "
                    "(sun lights only, radians). Pass a single number for "
                    "scalar properties or a 3-element list for the vector "
                    "ones -- get the light's id from add_light or "
                    "get_scene_state's 'lights' array first."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'light_id': {'type': 'integer'},
                        'property': {'type': 'string'},
                        'value': {},
                    },
                    'required': ['light_id', 'property', 'value'],
                },
            },
            {
                'name': 'delete_light',
                'description': "Removes a light from the running client's live scene, by id.",
                'input_schema': {
                    'type': 'object',
                    'properties': {'light_id': {'type': 'integer'}},
                    'required': ['light_id'],
                },
            },
            {
                'name': 'set_render_samples',
                'description': (
                    "Sets the offline raytracer's sample count (Phase 3) "
                    "in the running client's live scene -- how many paths "
                    "are traced per pixel when a render is eventually "
                    "kicked off. Higher = less noise, slower."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {'samples': {'type': 'integer'}},
                    'required': ['samples'],
                },
            },
            {
                'name': 'create_mesh_object',
                'description': (
                    "Build a real MeshObject from scratch in the running "
                    "client's live scene -- no file, no pre-authored "
                    "asset. positions is a flat [x0,y0,z0,x1,y1,z1,...] "
                    "list; indices is a flat triangle-list "
                    "[a0,b0,c0,a1,b1,c1,...] of indices INTO positions "
                    "(0-based, 3 per triangle). x/y/z place the new "
                    "object's origin in world space. Capped at 2048 "
                    "vertices / 4096 triangles per call -- for anything "
                    "bigger, tell the user to author it as a real asset "
                    "and load it via the Asset Browser instead. Returns "
                    "the new object's id, needed for set_mesh_vertices/"
                    "delete_mesh_object afterward."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'x': {'type': 'number'}, 'y': {'type': 'number'}, 'z': {'type': 'number'},
                        'positions': {'type': 'array', 'items': {'type': 'number'}},
                        'indices': {'type': 'array', 'items': {'type': 'integer'}},
                    },
                    'required': ['positions', 'indices'],
                },
            },
            {
                'name': 'set_mesh_vertices',
                'description': (
                    "Redefine EVERY vertex position of an existing "
                    "MeshObject's live geometry in the running client's "
                    "scene -- same flat [x0,y0,z0,x1,y1,z1,...] shape "
                    "create_mesh_object uses. Must supply the SAME number "
                    "of vertices the object already has (check get_scene_"
                    "state's objects[].vert_count first) -- this moves "
                    "existing vertices, it does not add/remove them or "
                    "change topology (use create_mesh_object for that)."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'object_id': {'type': 'integer'},
                        'positions': {'type': 'array', 'items': {'type': 'number'}},
                    },
                    'required': ['object_id', 'positions'],
                },
            },
            {
                'name': 'delete_mesh_object',
                'description': "Removes a MeshObject from the running client's live scene, by id.",
                'input_schema': {
                    'type': 'object',
                    'properties': {'object_id': {'type': 'integer'}},
                    'required': ['object_id'],
                },
            },
            {
                'name': 'get_mesh_vertices',
                'description': (
                    "See exactly where every vertex of an existing "
                    "MeshObject currently is, as a real read of the "
                    "running client's live geometry (not a guess from "
                    "vert_count alone). Call this before add_mesh_vertex/"
                    "add_mesh_face/set_mesh_vertex if you don't already "
                    "know the object's current vertex positions -- "
                    "indices are stable and match what set_mesh_vertices/"
                    "set_mesh_vertex expect."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {'object_id': {'type': 'integer'}},
                    'required': ['object_id'],
                },
            },
            {
                'name': 'get_mesh_faces',
                'description': (
                    "See the current triangle topology of an existing "
                    "MeshObject -- each entry is (face_index, (v0, v1, "
                    "v2)), where face_index is what a future edit call "
                    "would need (NOT just this list's position -- faces "
                    "can have gaps from earlier deletions) and v0/v1/v2 "
                    "are vertex indices matching get_mesh_vertices. "
                    "Useful before add_mesh_face, to see how existing "
                    "geometry connects."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {'object_id': {'type': 'integer'}},
                    'required': ['object_id'],
                },
            },
            {
                'name': 'add_mesh_vertex',
                'description': (
                    "Add ONE new vertex to an EXISTING MeshObject's live "
                    "geometry, growing it rather than replacing it "
                    "(unlike create_mesh_object, which builds a whole new "
                    "object from scratch). Returns the new vertex's "
                    "index. The new vertex is invisible until at least "
                    "one face references it -- call add_mesh_face right "
                    "after with this index to connect it into the "
                    "visible mesh (e.g. to give a house a roof peak, add "
                    "the peak vertex here, then add_mesh_face triangles "
                    "from the existing top edge up to it)."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'object_id': {'type': 'integer'},
                        'x': {'type': 'number'}, 'y': {'type': 'number'}, 'z': {'type': 'number'},
                    },
                    'required': ['object_id', 'x', 'y', 'z'],
                },
            },
            {
                'name': 'add_mesh_face',
                'description': (
                    "Add ONE new triangular face to an existing "
                    "MeshObject, referencing 3 vertex indices that must "
                    "already exist (from the object's original geometry, "
                    "seen via get_mesh_vertices, or a vertex you just "
                    "created with add_mesh_vertex). This is how a newly-"
                    "added vertex actually becomes visible geometry. "
                    "Returns the new face's index."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'object_id': {'type': 'integer'},
                        'v0': {'type': 'integer'}, 'v1': {'type': 'integer'}, 'v2': {'type': 'integer'},
                    },
                    'required': ['object_id', 'v0', 'v1', 'v2'],
                },
            },
            {
                'name': 'set_mesh_vertex',
                'description': (
                    "Move ONE existing vertex of a MeshObject to a new "
                    "position, by index -- unlike set_mesh_vertices "
                    "(which replaces every vertex at once and needs the "
                    "exact same count), this nudges a single corner "
                    "without touching the rest of the mesh. Get the "
                    "vertex's current index/position from get_mesh_"
                    "vertices first."
                ),
                'input_schema': {
                    'type': 'object',
                    'properties': {
                        'object_id': {'type': 'integer'},
                        'vertex_index': {'type': 'integer'},
                        'x': {'type': 'number'}, 'y': {'type': 'number'}, 'z': {'type': 'number'},
                    },
                    'required': ['object_id', 'vertex_index', 'x', 'y', 'z'],
                },
            },
        ]
        dispatch = {
            'get_asset_list': tool_get_asset_list,
            'get_scene_state': tool_get_scene_state,
            'add_light': tool_add_light,
            'set_light_property': tool_set_light_property,
            'delete_light': tool_delete_light,
            'set_render_samples': tool_set_render_samples,
            'create_mesh_object': tool_create_mesh_object,
            'set_mesh_vertices': tool_set_mesh_vertices,
            'delete_mesh_object': tool_delete_mesh_object,
            'get_mesh_vertices': tool_get_mesh_vertices,
            'get_mesh_faces': tool_get_mesh_faces,
            'add_mesh_vertex': tool_add_mesh_vertex,
            'add_mesh_face': tool_add_mesh_face,
            'set_mesh_vertex': tool_set_mesh_vertex,
        }
        system = (
            "You are Claude, embedded as a first-class participant inside "
            "the Phi game engine's editor (see phi.md's \"Where AI fits\"). "
            "You're talking with a user through the editor's Chat panel. "
            "You only ever receive a message here when it contains \"@llm\" "
            "somewhere in it -- that's this chat's addressing convention, "
            "so every message you see IS one directed at you, even if it "
            "doesn't look like a question. You have tools that let you "
            "introspect the specific running client instance this "
            "conversation is attached to -- use them when the user asks "
            "about the current scene/object state rather than guessing or "
            "making something up. You can also ACT: add_light/"
            "set_light_property/delete_light/set_render_samples/"
            "create_mesh_object/set_mesh_vertices/delete_mesh_object/"
            "get_mesh_vertices/get_mesh_faces/add_mesh_vertex/add_mesh_"
            "face/set_mesh_vertex actually change the running client's "
            "live scene, not just describe it -- use them freely when the "
            "user asks you to set up lighting, adjust render settings, or "
            "create/edit geometry, you don't need to ask permission first "
            "for these specifically. create_mesh_object builds real "
            "geometry from positions+indices you compute yourself (e.g. "
            "for a procedural shape the user describes) -- call get_"
            "scene_state first if you need an existing object's id or "
            "vertex count. For editing an EXISTING object incrementally "
            "rather than replacing it wholesale: get_mesh_vertices/get_"
            "mesh_faces show you exactly what's there now (don't guess "
            "positions or indices), add_mesh_vertex adds a new point "
            "(invisible until add_mesh_face connects it to the mesh), and "
            "set_mesh_vertex nudges one existing corner without touching "
            "the rest -- prefer these over create_mesh_object+delete_"
            "mesh_object when the user is asking to modify part of an "
            "object rather than replace it outright (e.g. \"add a "
            "chimney\" or \"move this corner\" vs. \"turn this into a "
            "house\", which is more naturally a full replacement). Assets "
            "(the Asset Browser's library) are still read-only from here. "
            "Keep replies concise: this renders in a small in-editor chat "
            "box, not a document."
        )
        try:
            reply = model_display_name(DEFAULT_MODEL) + ': ' + run_tool_loop(system, user_text, tools, dispatch)
        except AnthropicError as e:
            reply = f'System: {e}'
        except Exception as e:
            reply = f'System: unexpected error: {e}'
        self.send(pack_chat_reply(reply))

    def _on_message(self, data: bytes):
        if not data: return
        t = data[0]
        payload = data[1:]

        if t == PKT_HELLO:
            name = payload[0:16].rstrip(b'\x00').decode(errors='replace')
            self.name = name or self.name
            log.info(f'Player {self.pid} name: {self.name}')

        elif t == PKT_ASSET_LIST_REQUEST:
            query = _parse_list_request(payload)
            assets = self.assets_db.list_assets(query or None)
            self.send(pack_asset_list_reply(assets))

        elif t == PKT_ASSET_UPDATE:
            parsed = _parse_asset_update(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_ASSET_UPDATE')
            else:
                asset_id, name, tags = parsed
                if self.assets_db.update_asset(asset_id, name, tags):
                    log.info(f'asset {asset_id} updated: name={name!r} tags={tags}')
                    self.broadcast(bytes([PKT_ASSET_CHANGED]))
                else:
                    log.info(f'Client {self.pid}: update for unknown asset id {asset_id}')

        elif t == PKT_ASSET_DELETE:
            parsed = _parse_asset_delete(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_ASSET_DELETE')
            elif _delete_asset_and_file(self.assets_db, parsed):
                log.info(f'asset {parsed} deleted (via WS)')
                self.broadcast(bytes([PKT_ASSET_CHANGED]))
            else:
                log.info(f'Client {self.pid}: delete for unknown asset id {parsed}')

        elif t == PKT_CHAT_MSG:
            text = _parse_chat_msg(payload)
            if text is None:
                log.info(f'Client {self.pid}: malformed PKT_CHAT_MSG')
            elif '@llm' not in text:
                # Addressing convention: the model is only invoked (and
                # only ever SEES a message) when "@llm" appears in it --
                # see _handle_chat's own system prompt, which tells the
                # model the same thing, and chat.c's chat_init preamble,
                # which tells the human user. No reply is sent at all here
                # (not even a "you didn't say @llm" nudge) -- deliberately
                # silent, matching a Slack-bot-style @mention convention
                # rather than an always-on chatbot. main.c only sets
                # waiting_for_reply when it sees "@llm" in the outgoing
                # text for exactly this reason (so the "thinking..."
                # indicator never spins forever waiting on a reply that
                # was never going to come).
                log.info(f'Client {self.pid} chat (no @llm, not forwarded to the model): {text!r}')
            else:
                log.info(f'Client {self.pid} chat: {text!r}')
                threading.Thread(target=self._handle_chat, args=(text,), daemon=True).start()

        elif t == PKT_SCENE_STATE_REPLY:
            parsed = _parse_scene_state_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_SCENE_STATE_REPLY')
            else:
                req_id, text = parsed
                self._resolve_pending(req_id, text)

        elif t == PKT_PROP_SET_REPLY:
            parsed = _parse_prop_set_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_PROP_SET_REPLY')
            else:
                req_id, ok = parsed
                self._resolve_pending(req_id, ok)

        elif t == PKT_ADD_LIGHT_REPLY:
            parsed = _parse_add_light_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_ADD_LIGHT_REPLY')
            else:
                req_id, ok, light_id = parsed
                self._resolve_pending(req_id, (ok, light_id))

        elif t == PKT_DELETE_LIGHT_REPLY:
            parsed = _parse_delete_light_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_DELETE_LIGHT_REPLY')
            else:
                req_id, ok = parsed
                self._resolve_pending(req_id, ok)

        elif t == PKT_CREATE_MESH_REPLY:
            parsed = _parse_create_mesh_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_CREATE_MESH_REPLY')
            else:
                req_id, ok, object_id = parsed
                self._resolve_pending(req_id, (ok, object_id))

        elif t == PKT_SET_VERTICES_REPLY:
            parsed = _parse_set_vertices_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_SET_VERTICES_REPLY')
            else:
                req_id, ok = parsed
                self._resolve_pending(req_id, ok)

        elif t == PKT_DELETE_MESH_OBJECT_REPLY:
            parsed = _parse_delete_mesh_object_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_DELETE_MESH_OBJECT_REPLY')
            else:
                req_id, ok = parsed
                self._resolve_pending(req_id, ok)

        elif t == PKT_GET_MESH_VERTICES_REPLY:
            parsed = _parse_get_mesh_vertices_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_GET_MESH_VERTICES_REPLY')
            else:
                req_id, ok, positions = parsed
                self._resolve_pending(req_id, (ok, positions))

        elif t == PKT_GET_MESH_FACES_REPLY:
            parsed = _parse_get_mesh_faces_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_GET_MESH_FACES_REPLY')
            else:
                req_id, ok, faces = parsed
                self._resolve_pending(req_id, (ok, faces))

        elif t == PKT_ADD_MESH_VERTEX_REPLY:
            parsed = _parse_add_mesh_vertex_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_ADD_MESH_VERTEX_REPLY')
            else:
                req_id, ok, vertex_index = parsed
                self._resolve_pending(req_id, (ok, vertex_index))

        elif t == PKT_ADD_MESH_FACE_REPLY:
            parsed = _parse_add_mesh_face_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_ADD_MESH_FACE_REPLY')
            else:
                req_id, ok, face_index = parsed
                self._resolve_pending(req_id, (ok, face_index))

        elif t == PKT_SET_MESH_VERTEX_REPLY:
            parsed = _parse_set_mesh_vertex_reply(payload)
            if parsed is None:
                log.info(f'Client {self.pid}: malformed PKT_SET_MESH_VERTEX_REPLY')
            else:
                req_id, ok = parsed
                self._resolve_pending(req_id, ok)

        else:
            log.info(f'Client {self.pid}: unknown packet type 0x{t:02x} ({len(data)} bytes)')

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
    '.glb':  'model/gltf-binary',
    '.gltf': 'model/gltf+json',
    '.bin':  'application/octet-stream',
    '.ttf':  'font/ttf',
}

def parse_request(sock):
    """Read until the header/body boundary. Returns
    (method, path, query, hdrs, leftover) or None -- `leftover` is any
    bytes already pulled off the socket past the '\r\n\r\n' boundary
    (the start of a POST body, if any); callers that need the body must
    treat `leftover` as its first bytes rather than re-reading them."""
    raw = b''
    while b'\r\n\r\n' not in raw:
        chunk = sock.recv(4096)
        if not chunk: return None
        raw += chunk
        if len(raw) > 16384: return None
    header_part, _, leftover = raw.partition(b'\r\n\r\n')
    lines = header_part.decode(errors='replace').split('\r\n')
    parts = lines[0].split(' ')
    if len(parts) < 2: return None
    method = parts[0]
    raw_path = parts[1]
    path, _, qs = raw_path.partition('?')
    query = {k: v[0] for k, v in urllib.parse.parse_qs(qs).items()}
    hdrs = {}
    for l in lines[1:]:
        if ':' in l:
            k, v = l.split(':', 1)
            hdrs[k.strip().lower()] = v.strip()
    return method, path, query, hdrs, leftover

def read_body(sock, leftover: bytes, content_length: int) -> bytes | None:
    body = bytearray(leftover)
    while len(body) < content_length:
        chunk = sock.recv(min(65536, content_length - len(body)))
        if not chunk: return None
        body += chunk
    return bytes(body[:content_length])

def send_http(sock, status: int, content_type: str, body: bytes,
              extra_headers: dict = None):
    reason = {200: 'OK', 204: 'No Content', 400: 'Bad Request',
              404: 'Not Found', 405: 'Method Not Allowed'}.get(status, '')
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
    # Map URL path to filesystem. /assets/* resolves against the
    # project-root assets/ directory (see ASSETS_ROOT_DIR's comment) --
    # everything else resolves under www/ as before.
    if path == '/':
        fs_path = os.path.join(WWW_DIR, 'index.html')
        root_dir = WWW_DIR
    elif path.startswith('/assets/'):
        fs_path = os.path.join(ASSETS_ROOT_DIR, path[len('/assets/'):])
        root_dir = ASSETS_ROOT_DIR
    else:
        fs_path = os.path.join(WWW_DIR, path.lstrip('/'))
        root_dir = WWW_DIR
    # Security: no path traversal
    fs_path = os.path.realpath(fs_path)
    root_real = os.path.realpath(root_dir)
    if not fs_path.startswith(root_real):
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
        self.clients : list[Client] = []
        self.cli_lock = threading.Lock()
        self._next_pid = 1
        os.makedirs(ASSET_LIBRARY_DIR, exist_ok=True)
        self.assets_db = AssetDB(ASSETS_DB_PATH)

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

    def _handle_asset_create(self, sock, query, hdrs, leftover):
        try:
            content_length = int(hdrs.get('content-length', '0'))
        except ValueError:
            content_length = 0
        if content_length <= 0 or content_length > MAX_ASSET_BYTES:
            send_http(sock, 400, 'text/plain', b'Bad Request: missing/invalid Content-Length')
            return
        body = read_body(sock, leftover, content_length)
        if body is None:
            send_http(sock, 400, 'text/plain', b'Bad Request: truncated body')
            return
        if not _is_glb(body):
            send_http(sock, 400, 'text/plain', b'Bad Request: body is not a .glb (missing glTF binary magic)')
            return

        name = query.get('name', 'Untitled')
        tags = [t for t in query.get('tags', '').split(',') if t]

        # Filename includes a random suffix (not the not-yet-known DB id)
        # so the file can be written before the DB row is created, with no
        # id-reservation race between two concurrent uploads.
        filename = f'{_slugify(name)}_{uuid.uuid4().hex[:8]}.glb'
        fs_path = os.path.join(ASSET_LIBRARY_DIR, filename)
        with open(fs_path, 'wb') as f:
            f.write(body)
        rel_path = f'assets/library/{filename}'

        asset_id = self.assets_db.create_asset(name, rel_path, tags)
        log.info(f'asset created: id={asset_id} name={name!r} path={rel_path} tags={tags}')
        self.broadcast(bytes([PKT_ASSET_CHANGED]))
        send_http(sock, 200, 'text/plain', str(asset_id).encode())

    def _handle_asset_delete_http(self, sock, path):
        id_str = path[len('/assets/'):]
        if not id_str.isdigit():
            send_http(sock, 404, 'text/plain', b'Not Found')
            return
        asset_id = int(id_str)
        if _delete_asset_and_file(self.assets_db, asset_id):
            log.info(f'asset {asset_id} deleted (via HTTP)')
            self.broadcast(bytes([PKT_ASSET_CHANGED]))
            send_http(sock, 204, 'text/plain', b'')
        else:
            send_http(sock, 404, 'text/plain', b'Not Found')

    def _handle_conn(self, sock, addr):
        sock.settimeout(10.0)
        req = parse_request(sock)
        if not req:
            try: sock.close()
            except: pass
            return
        method, path, query, hdrs, leftover = req

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
                c = Client(sock, addr, pid, self.broadcast, self.assets_db)
                self.clients.append(c)
            threading.Thread(target=c.run, daemon=True).start()
            return

        if method == 'GET':
            serve_file(sock, path)
        elif method == 'POST' and path == '/assets':
            self._handle_asset_create(sock, query, hdrs, leftover)
        elif method == 'DELETE' and path.startswith('/assets/'):
            self._handle_asset_delete_http(sock, path)
        else:
            send_http(sock, 405, 'text/plain', b'Method Not Allowed')
        try: sock.close()
        except: pass

    def run(self):
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
