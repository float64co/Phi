#!/usr/bin/env python3
"""
Standalone LIVE test for the Chat + tool-use introspection feature (see
phi.md's "Where AI fits" / server/anthropic_client.py). Unlike
test_asset_protocol.py's WS-only checks, this one makes REAL calls to the
Anthropic API (ANTHROPIC_API_KEY must be set in the server's environment)
-- it is not run as part of the regular no-network standalone-harness
suite, same "needs a live server.py, run manually" carve-out
test_asset_protocol.py already documents for itself, extended here to
also mean "and costs real API tokens, don't run it in a loop".

Reuses test_asset_protocol.py's WsTestClient verbatim (see that file for
why it's an independent Python WS implementation rather than reusing
client/ws_client_native.c). This harness plays BOTH roles a real editor
client would: it answers PKT_SCENE_STATE_REQUEST with a canned JSON
payload (standing in for main.c's real scene_state_handler), which lets
the get_scene_state tool-call round trip be verified end to end without
needing a real windowed client running.

Usage: ANTHROPIC_API_KEY=... python3 test_chat_protocol.py [host] [port]
"""
import socket
import struct
import sys
import time

import test_asset_protocol
from test_asset_protocol import WsTestClient, check
from anthropic_client import model_display_name, DEFAULT_MODEL

EXPECTED_PREFIX = model_display_name(DEFAULT_MODEL) + ': '

PKT_CHAT_MSG             = 0x20
PKT_CHAT_REPLY           = 0x21
PKT_SCENE_STATE_REQUEST  = 0x22
PKT_SCENE_STATE_REPLY    = 0x23

FAKE_SCENE_STATE = (
    '{"mesh_loaded":true,"position":[128.0,100.0,90.0],'
    '"orientation":[0.0,0.0,0.0,1.0],"is_static":false,'
    '"vert_count":4242,"face_count":6,"has_physics":true,'
    '"velocity":[0.0,-1.5,0.0],"physics_gravity":[0.0,-9.81,0.0],'
    '"selected_face":null}'
)


def build_chat_msg(text: str) -> bytes:
    b = text.encode('utf-8')
    return bytes([PKT_CHAT_MSG]) + struct.pack('<H', len(b)) + b


def build_scene_state_reply(req_id: int, json_text: str) -> bytes:
    b = json_text.encode('utf-8')
    return bytes([PKT_SCENE_STATE_REPLY]) + struct.pack('<I', req_id) + struct.pack('<H', len(b)) + b


def recv_until_chat_reply(c: WsTestClient, timeout_total=40.0):
    """Reads frames until PKT_CHAT_REPLY, answering any
    PKT_SCENE_STATE_REQUEST it sees along the way with FAKE_SCENE_STATE --
    the real round trip a live client would perform, just with a scripted
    answer instead of real engine state."""
    deadline = time.time() + timeout_total
    saw_scene_request = False
    while time.time() < deadline:
        frame = c.recv_frame(timeout=max(1.0, deadline - time.time()))
        if frame[0] == PKT_SCENE_STATE_REQUEST:
            saw_scene_request = True
            req_id = struct.unpack_from('<I', frame, 1)[0]
            c.send(build_scene_state_reply(req_id, FAKE_SCENE_STATE))
            continue
        if frame[0] == PKT_CHAT_REPLY:
            slen = struct.unpack_from('<H', frame, 1)[0]
            text = frame[3:3 + slen].decode(errors='replace')
            return text, saw_scene_request
    raise TimeoutError("no PKT_CHAT_REPLY within timeout")


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8765

    print(f"[test_chat_protocol] connecting to {host}:{port}/ws ...")
    c = WsTestClient(host, port)
    hello = c.recv_frame()
    check(hello[0] == 0x01, f"first message is PKT_HELLO (got 0x{hello[0]:02x})")

    print("[test_chat_protocol] === 1: a message WITHOUT @llm gets no reply at all ===")
    c.send(build_chat_msg("Reply with exactly one word: PONG"))   # deliberately no @llm
    try:
        stray = c.recv_frame(timeout=3.0)
        check(False, f"unexpected reply to a non-@llm message: 0x{stray[0]:02x}")
    except (socket.timeout, TimeoutError):
        check(True, "server stayed silent -- the model is never invoked without @llm in the message")

    print("[test_chat_protocol] === 2: plain round trip, no tool use, real reply prefixed with the model's display name ===")
    c.send(build_chat_msg("@llm Reply with exactly one word: PONG"))
    reply, used_tool = recv_until_chat_reply(c)
    print(f"  reply: {reply!r}")
    check(reply.startswith(EXPECTED_PREFIX), f"reply starts with {EXPECTED_PREFIX!r} (the bold-username prefix draw_panel_chat parses)")
    check("PONG" in reply.upper(), "the real assistant reply contains PONG")
    check(not used_tool, "a plain question doesn't trigger a tool call")

    print("[test_chat_protocol] === 3: get_scene_state tool call round-trips through THIS process ===")
    c.send(build_chat_msg(
        "@llm Call the get_scene_state tool right now and reply with ONLY the "
        "vert_count number it returns, nothing else."
    ))
    reply2, used_tool2 = recv_until_chat_reply(c)
    print(f"  reply: {reply2!r}")
    check(used_tool2, "the model actually issued a get_scene_state tool call (we answered a PKT_SCENE_STATE_REQUEST)")
    check("4242" in reply2, "the final reply reflects the EXACT canned value (4242) this test process sent back, not a guess")

    print("[test_chat_protocol] === 4: get_asset_list tool call reflects the REAL server-side DB ===")
    c.send(build_chat_msg(
        "@llm Call get_asset_list and reply with ONLY the total count of assets listed, as a number, nothing else."
    ))
    reply3, _ = recv_until_chat_reply(c)
    print(f"  reply: {reply3!r}")
    check(any(ch.isdigit() for ch in reply3), "reply contains a real number (not just prose)")

    failed = test_asset_protocol._fail
    print("\n[test_chat_protocol] RESULT:", "FAIL" if failed else "PASS (all checks passed, all live)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
