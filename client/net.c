#include "net.h"
#include "console.h"
#include <string.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/websocket.h>

static EMSCRIPTEN_WEBSOCKET_T s_ws = 0;
static NetState               *s_ns  = NULL;

static EM_BOOL ws_on_open(int type, const EmscriptenWebSocketOpenEvent *e, void *ud) {
    (void)type; (void)e; (void)ud;
    s_ns->connected = 1;
    printf("[net] WebSocket connected\n");
    net_send_hello(s_ns, "player");
    return EM_TRUE;
}

static EM_BOOL ws_on_message(int type, const EmscriptenWebSocketMessageEvent *e, void *ud) {
    (void)type; (void)ud;
    /* All our packets are binary; treat any incoming data as binary */
    if (e->numBytes > 0 && s_ns)
        net_on_message(s_ns, (const uint8_t *)e->data, (int)e->numBytes);
    return EM_TRUE;
}

static EM_BOOL ws_on_close(int type, const EmscriptenWebSocketCloseEvent *e, void *ud) {
    (void)type; (void)e; (void)ud;
    s_ns->connected = 0;
    printf("[net] WebSocket closed\n");
    return EM_TRUE;
}

static EM_BOOL ws_on_error(int type, const EmscriptenWebSocketErrorEvent *e, void *ud) {
    (void)type; (void)e; (void)ud;
    printf("[net] WebSocket error\n");
    return EM_TRUE;
}

void net_connect(NetState *ns, const char *url) {
    s_ns = ns;
    strncpy(ns->ws_url, url, sizeof(ns->ws_url)-1);

    EmscriptenWebSocketCreateAttributes attr;
    emscripten_websocket_init_create_attributes(&attr);
    attr.url = ns->ws_url;

    s_ws = emscripten_websocket_new(&attr);
    emscripten_websocket_set_onopen_callback(s_ws, NULL, ws_on_open);
    emscripten_websocket_set_onmessage_callback(s_ws, NULL, ws_on_message);
    emscripten_websocket_set_onclose_callback(s_ws, NULL, ws_on_close);
    emscripten_websocket_set_onerror_callback(s_ws, NULL, ws_on_error);
}

static void ws_send_binary(const uint8_t *data, int len) {
    if (s_ws && s_ns && s_ns->connected)
        emscripten_websocket_send_binary(s_ws, (void *)data, len);
}

#elif defined(_WIN32)
#include "ws_client_win32.h"

static NetState *s_ns_native = NULL;

static void ws_send_binary(const uint8_t *data, int len) {
    ws_client_send_binary(data, len);
}

void net_connect(NetState *ns, const char *url) {
    strncpy(ns->ws_url, url, sizeof(ns->ws_url)-1);
    s_ns_native = ns;
    ns->connected = 0;
    if (ws_client_connect(url) == 0) {
        ns->connected = 1;
        printf("[net] WebSocket connected (native)\n");
        net_send_hello(ns, "player");
    } else {
        printf("[net] WebSocket connect failed: %s\n", url);
    }
}

static void on_native_ws_message(void *user, const uint8_t *data, int len) {
    (void)user;
    if (s_ns_native) net_on_message(s_ns_native, data, len);
}

void net_poll_native(void) {
    if (!s_ns_native) return;
    if (!ws_client_connected()) {
        if (s_ns_native->connected) {
            s_ns_native->connected = 0;
            printf("[net] WebSocket disconnected\n");
        }
        return;
    }
    ws_client_poll(on_native_ws_message, NULL);
}

#else
#include "ws_client_native.h"

static NetState *s_ns_native = NULL;

static void ws_send_binary(const uint8_t *data, int len) {
    ws_client_send_binary(data, len);
}

void net_connect(NetState *ns, const char *url) {
    strncpy(ns->ws_url, url, sizeof(ns->ws_url)-1);
    s_ns_native = ns;
    ns->connected = 0;
    if (ws_client_connect(url) == 0) {
        ns->connected = 1;
        printf("[net] WebSocket connected (native)\n");
        net_send_hello(ns, "player");
    } else {
        printf("[net] WebSocket connect failed: %s\n", url);
    }
}

static void on_native_ws_message(void *user, const uint8_t *data, int len) {
    (void)user;
    if (s_ns_native) net_on_message(s_ns_native, data, len);
}

/* Called once per frame by main.c (native only) — sockets need active
 * polling, unlike wasm's async ws_on_message callback. */
void net_poll_native(void) {
    if (!s_ns_native) return;
    if (!ws_client_connected()) {
        if (s_ns_native->connected) {
            s_ns_native->connected = 0;
            printf("[net] WebSocket disconnected\n");
        }
        return;
    }
    ws_client_poll(on_native_ws_message, NULL);
}
#endif

/* ---- Write helpers ---- */
static uint8_t *w_u8 (uint8_t *p, uint8_t  v) { *p++=v; return p; }

void net_send_hello(NetState *ns, const char *name) {
    (void)ns;
    uint8_t pkt[17];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_HELLO);
    memset(p, 0, 16);
    strncpy((char *)p, name, 16);
    ws_send_binary(pkt, sizeof(pkt));
}

void net_on_message(NetState *ns, const uint8_t *data, int len) {
    if (len < 1) return;
    const uint8_t *p = data;
    uint8_t type = *p++;

    switch (type) {

    case PKT_HELLO: {
        /* Server sends our assigned client id */
        uint8_t id = *p;
        ns->local_id = id;
        printf("[net] Assigned client id=%d\n", id);
        break;
    }

    case PKT_CONSOLE_MSG: {
        if (len < 3) break;
        uint16_t slen; memcpy(&slen, p, 2); p += 2;
        if (p + slen > data + len) break;
        char buf[512];
        int n = (int)(slen < sizeof(buf) - 1 ? slen : sizeof(buf) - 1);
        memcpy(buf, p, (size_t)n);
        buf[n] = 0;
        console_append(buf);
        break;
    }

    default:
        printf("[net] Unknown packet type 0x%02x\n", type);
        break;
    }
}
