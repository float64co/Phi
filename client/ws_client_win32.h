#pragma once
/* Windows port of ws_client_native.h's RFC 6455 client — same interface,
 * same protocol logic (proven against the real server on Linux), Winsock2
 * instead of BSD sockets underneath. See ws_client_native.h for the
 * general design notes (no TLS, no threading, single-threaded per-frame
 * poll) — all of that applies here unchanged. */

#include <stdint.h>

int  ws_client_connect(const char *url);
void ws_client_send_binary(const uint8_t *data, int len);

typedef void (*WsClientOnMessage)(void *user, const uint8_t *data, int len);
void ws_client_poll(WsClientOnMessage on_message, void *user);

int  ws_client_connected(void);
void ws_client_close(void);
