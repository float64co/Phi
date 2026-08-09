#!/usr/bin/env python3
"""
Standalone test client for the asset CRUD wire protocol (see phi.md's
"Wire protocol: CRUD over a hybrid HTTP + WS split"). Exercises the WS
half (LIST/UPDATE/DELETE/CHANGED) against a running server.py -- the HTTP
half (create/read/delete-by-http) is simple enough to smoke-test with
curl and isn't duplicated here. No test framework, no third-party deps
(hashlib/base64/socket/struct are all stdlib) -- matches server.py's own
"no dependencies" constraint and mesh_edit_test.c's own PASS/FAIL-line
style rather than pulling in pytest for a handful of checks.

Usage: python3 test_asset_protocol.py [host] [port]
"""
import socket
import struct
import hashlib
import base64
import sys
import time

PKT_HELLO              = 0x01
PKT_ASSET_LIST_REQUEST = 0x10
PKT_ASSET_LIST_REPLY   = 0x11
PKT_ASSET_UPDATE       = 0x12
PKT_ASSET_DELETE       = 0x13
PKT_ASSET_CHANGED      = 0x14

_fail = False

def check(cond, msg):
    global _fail
    status = "PASS" if cond else "FAIL"
    if not cond:
        _fail = True
    print(f"  {status}: {msg}")
    return cond


class WsTestClient:
    """Minimal RFC 6455 client -- deliberately not reusing anything from
    client/ws_client_native.c (that's C, this is Python) but implements
    the exact same handshake/framing steps, as an independent check that
    server.py's ws_handshake_response/ws_encode/ws_decode_frames are
    actually spec-correct rather than just "whatever the one existing
    client happens to accept"."""

    def __init__(self, host: str, port: int, path: str = "/ws"):
        self.sock = socket.create_connection((host, port), timeout=5.0)
        key = base64.b64encode(b"phi-test-client-key-16b!").decode()
        req = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self.sock.sendall(req.encode())
        raw = b""
        while b"\r\n\r\n" not in raw:
            raw += self.sock.recv(4096)
        # Bytes past the header/body boundary can already be the start of
        # the server's first WS frame (PKT_HELLO) if it arrives bundled
        # with the handshake response over loopback -- must be kept, not
        # discarded, or recv_frame() hangs waiting for bytes that already
        # arrived. Same leftover-bytes hazard server.py's own parse_request
        # handles on the HTTP side.
        header_part, _, leftover = raw.partition(b"\r\n\r\n")
        assert header_part.startswith(b"HTTP/1.1 101"), f"handshake rejected: {header_part[:80]!r}"
        self._buf = bytearray(leftover)
        self.sock.settimeout(5.0)

    def send(self, payload: bytes):
        n = len(payload)
        if n < 126:
            header = bytes([0x82, 0x80 | n])
        else:
            header = bytes([0x82, 0x80 | 126]) + struct.pack('>H', n)
        mask = b"\x01\x02\x03\x04"
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def recv_frame(self, timeout=5.0) -> bytes:
        """Blocks until one full WS frame's payload is available, returns it."""
        self.sock.settimeout(timeout)
        while True:
            frames = self._decode_frames()
            if frames:
                return frames[0]
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed connection")
            self._buf.extend(chunk)

    def _decode_frames(self):
        frames = []
        buf = self._buf
        while len(buf) >= 2:
            b0, b1 = buf[0], buf[1]
            opcode = b0 & 0x0F
            masked = (b1 & 0x80) != 0
            plen = b1 & 0x7F
            idx = 2
            if plen == 126:
                if len(buf) < 4: break
                plen = struct.unpack('>H', buf[2:4])[0]; idx = 4
            elif plen == 127:
                if len(buf) < 10: break
                plen = struct.unpack('>Q', buf[2:10])[0]; idx = 10
            mask_end = idx + (4 if masked else 0)
            if len(buf) < mask_end + plen: break
            payload = bytearray(buf[mask_end:mask_end+plen])
            if masked:
                mask_key = buf[idx:idx+4]
                for i in range(len(payload)):
                    payload[i] ^= mask_key[i % 4]
            if opcode in (0x1, 0x2):
                frames.append(bytes(payload))
            del buf[:mask_end + plen]
        return frames


def parse_list_reply(data: bytes):
    assert data[0] == PKT_ASSET_LIST_REPLY, f"expected LIST_REPLY, got 0x{data[0]:02x}"
    count = struct.unpack_from('<H', data, 1)[0]
    off = 3
    items = []
    for _ in range(count):
        asset_id = struct.unpack_from('<I', data, off)[0]; off += 4
        nlen = data[off]; off += 1
        name = data[off:off+nlen].decode(); off += nlen
        plen = data[off]; off += 1
        path = data[off:off+plen].decode(); off += plen
        tlen = data[off]; off += 1
        tags = data[off:off+tlen].decode(); off += tlen
        items.append({"id": asset_id, "name": name, "path": path, "tags": tags})
    return items


def build_update(asset_id: int, name: str, tags_csv: str) -> bytes:
    nb = name.encode(); tb = tags_csv.encode()
    return (bytes([PKT_ASSET_UPDATE]) + struct.pack('<I', asset_id) +
            bytes([len(nb)]) + nb + bytes([len(tb)]) + tb)


def build_delete(asset_id: int) -> bytes:
    return bytes([PKT_ASSET_DELETE]) + struct.pack('<I', asset_id)


def build_list_request(query: str = "") -> bytes:
    qb = query.encode()
    return bytes([PKT_ASSET_LIST_REQUEST, len(qb)]) + qb


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765

    print(f"[test_asset_protocol] connecting to {host}:{port}/ws ...")
    c1 = WsTestClient(host, port)
    hello = c1.recv_frame()
    check(hello[0] == PKT_HELLO, f"first message is PKT_HELLO (got 0x{hello[0]:02x})")
    my_id = hello[1]
    print(f"  assigned id={my_id}")

    print("[test_asset_protocol] === 1: LIST reflects whatever's in the DB right now ===")
    c1.send(build_list_request())
    reply = c1.recv_frame()
    items = parse_list_reply(reply)
    print(f"  {len(items)} asset(s) currently indexed: {[(i['id'], i['name']) for i in items]}")
    check(isinstance(items, list), "LIST_REPLY parses into a list without error")

    if not items:
        print("[test_asset_protocol] no assets indexed yet -- skipping UPDATE/DELETE/CHANGED checks "
              "(run the HTTP create smoke test or the test-asset uploader first)")
        print("\n[test_asset_protocol] RESULT:", "FAIL" if _fail else "PASS (partial -- no assets to test against)")
        return 1 if _fail else 0

    target = items[0]
    print(f"[test_asset_protocol] === 2: UPDATE asset {target['id']} and confirm CHANGED broadcasts to EVERY connected client ===")
    c2 = WsTestClient(host, port)   # a second, otherwise-idle connected editor
    c2.recv_frame()  # HELLO

    new_name = target['name'] + "_renamed"
    c1.send(build_update(target['id'], new_name, "renamed,test"))
    changed = c1.recv_frame()
    check(changed[0] == PKT_ASSET_CHANGED, "sender (c1) receives PKT_ASSET_CHANGED after its own UPDATE")
    changed_c2 = c2.recv_frame()
    check(changed_c2[0] == PKT_ASSET_CHANGED, "a second, uninvolved connected client (c2) also receives PKT_ASSET_CHANGED")

    c1.send(build_list_request())
    reply2 = parse_list_reply(c1.recv_frame())
    updated = next((a for a in reply2 if a['id'] == target['id']), None)
    check(updated is not None, "updated asset still present after rename")
    check(updated and updated['name'] == new_name, f"name actually changed to {new_name!r} (got {updated and updated['name']!r})")
    check(updated and updated['tags'] == 'renamed,test', f"tags actually changed (got {updated and updated['tags']!r})")

    print(f"[test_asset_protocol] === 3: DELETE asset {target['id']} and confirm it's gone ===")
    c1.send(build_delete(target['id']))
    changed2 = c1.recv_frame()
    check(changed2[0] == PKT_ASSET_CHANGED, "PKT_ASSET_CHANGED received after DELETE")
    c1.send(build_list_request())
    reply3 = parse_list_reply(c1.recv_frame())
    check(all(a['id'] != target['id'] for a in reply3), "deleted asset no longer appears in LIST")

    print("[test_asset_protocol] === 4: UPDATE on an unknown id is a silent no-op (no CHANGED broadcast) ===")
    c1.send(build_update(999999, "ghost", ""))
    time.sleep(0.3)
    try:
        stray = c1.recv_frame(timeout=0.5)
        check(False, f"unexpected message after bogus UPDATE: 0x{stray[0]:02x}")
    except (socket.timeout, TimeoutError):
        check(True, "no CHANGED broadcast for an update targeting a nonexistent id")

    print("\n[test_asset_protocol] RESULT:", "FAIL" if _fail else "PASS (all checks passed)")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
