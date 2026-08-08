#pragma once
/* Minimal RFC 6455 WebSocket client over BSD sockets — native networking
 * abstraction (phi.md's phi_net.h table: "WebSocket: BSD sockets" for
 * desktop, vs. library_ws_stub.js/emscripten_websocket_* for wasm).
 * Matches server/server.py's hand-rolled server-side implementation
 * (ws_handshake_response/ws_encode/ws_decode_frames) exactly — that
 * server never validates anything beyond "some Sec-WebSocket-Key was
 * given" and always sends unmasked frames, so this client always masks
 * outgoing frames (required by spec regardless) and accepts unmasked
 * incoming ones (also required — servers never mask).
 *
 * No TLS (ws:// only, not wss://) and no threading: the socket is set
 * non-blocking after the handshake and polled once per frame from the
 * native main loop, same single-threaded model as the rest of the native
 * client (input, platform). Good enough for a LAN dev server; a real
 * wss:// client is future work. */

#include <stdint.h>

/* Blocking: opens the TCP connection and performs the HTTP Upgrade
 * handshake against a "ws://host[:port]/path" URL. Returns 0 on success. */
int ws_client_connect(const char *url);

/* Frames and sends payload as a single masked binary (opcode 0x2) frame. */
void ws_client_send_binary(const uint8_t *data, int len);

/* Non-blocking: drains whatever bytes are available, decodes as many
 * complete frames as it can, and calls on_message once per binary frame.
 * Call once per frame from the main loop. */
typedef void (*WsClientOnMessage)(void *user, const uint8_t *data, int len);
void ws_client_poll(WsClientOnMessage on_message, void *user);

int  ws_client_connected(void);
void ws_client_close(void);
