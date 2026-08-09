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
