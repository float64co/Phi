#!/usr/bin/env python3
"""
Pure-Python HTTP + WebSocket server.
No third-party dependencies. Uses only stdlib.

Serves:
  GET /          → www/index.html
  GET /game.js   → www/game.js   (emscripten glue)
  GET /game.wasm → www/game.wasm
  GET /assets/*  → www/assets/*
  WS  /ws        → editor/game transport

RFC 6455 WebSocket implemented by hand.

This used to also run Qek's 20Hz authoritative FPS simulation (Player/
Rocket/GameWorld, octree-backed map persistence, PKT_INPUT/FIRE/
EDIT_REGION/SAVE_MAP/... handlers) — all removed along with the rest of
that gameplay/world code (see phi.md's Phase 1 status, "Client/server
model"). The HTTP file serving and WebSocket framing below are fully
generic and were kept as-is; the connection/handshake plumbing is kept
too, repurposed as the transport a future general multiplayer game-
authoring/gameplay layer will build on, rather than deleted.
"""

import socket
import threading
import struct
import hashlib
import base64
import os
import logging

logging.basicConfig(level=logging.INFO, format='[%(levelname)s] %(message)s')
log = logging.getLogger('server')

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
HOST    = '0.0.0.0'
PORT    = 8765
WWW_DIR = os.path.join(os.path.dirname(__file__), '..', 'www')

# ---------------------------------------------------------------------------
# Binary protocol constants  (must match client/net.h)
# ---------------------------------------------------------------------------
PKT_HELLO       = 0x01   # C->S: [name:16 bytes, NUL-padded]. S->C: [id:u8]
PKT_CONSOLE_MSG = 0x0D   # S->C: [len:u16 utf8:bytes], appended to the client's Python panel log


def _console_payload(text: str) -> bytes:
    b = text.encode('utf-8')
    return struct.pack('<H', len(b)) + b

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
    def __init__(self, sock, addr, pid, broadcast):
        self.sock      = sock
        self.addr      = addr
        self.pid       = pid
        self.name      = f'player{pid}'
        self.broadcast = broadcast
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

        if t == PKT_HELLO:
            name = data[1:17].rstrip(b'\x00').decode(errors='replace')
            self.name = name or self.name
            log.info(f'Player {self.pid} name: {self.name}')
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
                c = Client(sock, addr, pid, self.broadcast)
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
