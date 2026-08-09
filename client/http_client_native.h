#pragma once
#include <stdint.h>

/* Minimal blocking native HTTP client -- POST only, no chunked transfer/
 * redirects/keep-alive, matching this codebase's existing "hand-roll
 * exactly what's needed" precedent (ws_client_native.c's own WS
 * handshake is the same kind of minimal, and this reuses its getaddrinfo/
 * connect pattern). Built specifically for the Asset Browser's in-editor
 * Create flow (see phi.md's "Wire protocol..."): POST a .glb body to
 * server.py's /assets endpoint and read back the small plain-text id it
 * replies with.
 *
 * Native-only, same as ws_client_native.c/ws_client_win32.c being
 * separate per-platform files -- no wasm build needs this (a browser can
 * just use fetch()/XMLHttpRequest if/when that lands), and win32 would
 * need its own Winsock twin of this file the way ws_client_win32.c is
 * ws_client_native.c's, not built this pass (flagged, not silently
 * assumed to work). */

/* Connects to host:port, sends `POST path_and_query` with `body` as a raw
 * request body (Content-Length set automatically, Connection: close so
 * the response can be read until EOF), and copies the response body
 * (NUL-terminated, truncated to resp_body_cap-1) into resp_body.
 * *out_status receives the HTTP status code (e.g. 200), or -1 if the
 * request never got a parseable response at all (connect/send/recv
 * failure). Returns 1 on success (got a well-formed response, whatever
 * its status), 0 on failure. */
int http_post_native(const char *host, int port, const char *path_and_query,
                      const uint8_t *body, int body_len,
                      char *resp_body, int resp_body_cap, int *out_status);

/* Percent-encodes `in` into `out` (capacity out_cap, NUL-terminated,
 * truncated rather than overflowed) for safe inclusion in a URL query
 * string -- the Asset Browser's name/tags fields can contain spaces and
 * other characters a raw request line can't. */
void http_url_encode(const char *in, char *out, int out_cap);

/* Extracts host/port from a "ws://host[:port][/path]" URL (path is
 * ignored) -- lets the Create flow point this HTTP client at the same
 * server the game's WS connection (NetState.ws_url) is already talking
 * to, since server.py serves both on the same host:port by design.
 * Returns 0 on success, -1 on a malformed URL. */
int http_parse_ws_host_port(const char *ws_url, char *host, int host_cap, int *port);
