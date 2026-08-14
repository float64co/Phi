#include "net.h"
#include "console.h"
#include "asset_browser.h"
#include "chat.h"
#include <string.h>
#include <stdio.h>

static void (*s_scene_state_handler)(uint32_t req_id) = NULL;

void net_set_scene_state_handler(void (*handler)(uint32_t req_id)) {
    s_scene_state_handler = handler;
}

static void (*s_prop_set_handler)(uint32_t req_id, const char *target, const char *identifier,
                                   int is_vec3, float v0, float v1, float v2) = NULL;
static void (*s_add_light_handler)(uint32_t req_id, int type, float x, float y, float z) = NULL;
static void (*s_delete_light_handler)(uint32_t req_id, uint32_t light_id) = NULL;

void net_set_prop_set_handler(void (*handler)(uint32_t req_id, const char *target, const char *identifier,
                                               int is_vec3, float v0, float v1, float v2)) {
    s_prop_set_handler = handler;
}
void net_set_add_light_handler(void (*handler)(uint32_t req_id, int type, float x, float y, float z)) {
    s_add_light_handler = handler;
}
void net_set_delete_light_handler(void (*handler)(uint32_t req_id, uint32_t light_id)) {
    s_delete_light_handler = handler;
}

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
    net_send_asset_list_request(s_ns, NULL);  /* prime the Asset Browser's cache as soon as we're actually connected */
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
        net_send_asset_list_request(ns, NULL);  /* prime the Asset Browser's cache as soon as we're actually connected */
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
        net_send_asset_list_request(ns, NULL);  /* prime the Asset Browser's cache as soon as we're actually connected */
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

/* Writes a [len:u8 bytes] field, truncating to 255 bytes -- matches the
 * wire format server.py's pack_asset_list_reply/_parse_asset_update use.
 * Returns the new write cursor. */
static uint8_t *w_lenprefixed(uint8_t *p, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 255) n = 255;
    p = w_u8(p, (uint8_t)n);
    if (n) { memcpy(p, s, n); p += n; }
    return p;
}

void net_send_asset_list_request(NetState *ns, const char *query) {
    (void)ns;
    uint8_t pkt[2 + 255];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_ASSET_LIST_REQUEST);
    p = w_lenprefixed(p, query);
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_asset_update(NetState *ns, uint32_t id, const char *name, const char *tags_csv) {
    (void)ns;
    uint8_t pkt[1 + 4 + 2 + 255 + 255];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_ASSET_UPDATE);
    memcpy(p, &id, 4); p += 4;
    p = w_lenprefixed(p, name);
    p = w_lenprefixed(p, tags_csv);
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_asset_delete(NetState *ns, uint32_t id) {
    (void)ns;
    uint8_t pkt[5];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_ASSET_DELETE);
    memcpy(p, &id, 4); p += 4;
    ws_send_binary(pkt, (int)(p - pkt));
}

/* Writes a [len:u16 bytes] field -- the wider-length twin of
 * w_lenprefixed above, needed because chat text/scene-state JSON can
 * plausibly exceed 255 bytes where names/tags/queries never do. */
static uint8_t *w_lenprefixed16(uint8_t *p, const char *s) {
    size_t n = s ? strlen(s) : 0;
    if (n > 65535) n = 65535;
    uint16_t n16 = (uint16_t)n;
    memcpy(p, &n16, 2); p += 2;
    if (n) { memcpy(p, s, n); p += n; }
    return p;
}

void net_send_chat_msg(NetState *ns, const char *text) {
    (void)ns;
    uint8_t pkt[1 + 2 + CHAT_INPUT_LEN];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_CHAT_MSG);
    p = w_lenprefixed16(p, text);
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_scene_state_reply(NetState *ns, uint32_t req_id, const char *json) {
    (void)ns;
    uint8_t pkt[1 + 4 + 2 + 3072];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_SCENE_STATE_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_lenprefixed16(p, json);
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_prop_set_reply(NetState *ns, uint32_t req_id, int ok) {
    (void)ns;
    uint8_t pkt[1 + 4 + 1];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_PROP_SET_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_add_light_reply(NetState *ns, uint32_t req_id, int ok, uint32_t light_id) {
    (void)ns;
    uint8_t pkt[1 + 4 + 1 + 4];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_ADD_LIGHT_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    memcpy(p, &light_id, 4); p += 4;
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_delete_light_reply(NetState *ns, uint32_t req_id, int ok) {
    (void)ns;
    uint8_t pkt[1 + 4 + 1];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_DELETE_LIGHT_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    ws_send_binary(pkt, (int)(p - pkt));
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
        pyconsole_append(buf);
        break;
    }

    case PKT_ASSET_LIST_REPLY: {
        asset_browser_ingest_list_reply(p, len - 1);
        break;
    }

    case PKT_ASSET_CHANGED: {
        asset_browser_mark_dirty();
        break;
    }

    case PKT_CHAT_REPLY: {
        if (len < 3) break;
        uint16_t slen; memcpy(&slen, p, 2); p += 2;
        if (p + slen > data + len) break;
        char buf[2048];
        int n = (int)(slen < sizeof(buf) - 1 ? slen : sizeof(buf) - 1);
        memcpy(buf, p, (size_t)n);
        buf[n] = 0;
        chat_on_reply(buf);
        break;
    }

    case PKT_SCENE_STATE_REQUEST: {
        if (len < 5) break;
        uint32_t req_id; memcpy(&req_id, p, 4);
        if (s_scene_state_handler) s_scene_state_handler(req_id);
        break;
    }

    case PKT_PROP_SET_REQUEST: {
        /* [req_id:u32 target_len:u8 target:bytes ident_len:u8 ident:bytes is_vec3:u8 v0:f32 v1:f32 v2:f32] */
        if (len < 6) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint8_t target_len = *p++;
        if (p + target_len > data + len) break;
        char target[64];
        int tn = (int)(target_len < sizeof(target) - 1 ? target_len : sizeof(target) - 1);
        memcpy(target, p, (size_t)tn); target[tn] = 0;
        p += target_len;
        if (p + 1 > data + len) break;
        uint8_t ident_len = *p++;
        if (p + ident_len > data + len) break;
        char identifier[32];
        int in_ = (int)(ident_len < sizeof(identifier) - 1 ? ident_len : sizeof(identifier) - 1);
        memcpy(identifier, p, (size_t)in_); identifier[in_] = 0;
        p += ident_len;
        if (p + 1 + 12 > data + len) break;
        uint8_t is_vec3 = *p++;
        float v0, v1, v2;
        memcpy(&v0, p, 4); p += 4;
        memcpy(&v1, p, 4); p += 4;
        memcpy(&v2, p, 4); p += 4;
        if (s_prop_set_handler) s_prop_set_handler(req_id, target, identifier, is_vec3 != 0, v0, v1, v2);
        break;
    }

    case PKT_ADD_LIGHT_REQUEST: {
        if (len < 1 + 4 + 1 + 12) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint8_t type = *p++;
        float x, y, z;
        memcpy(&x, p, 4); p += 4;
        memcpy(&y, p, 4); p += 4;
        memcpy(&z, p, 4); p += 4;
        if (s_add_light_handler) s_add_light_handler(req_id, type, x, y, z);
        break;
    }

    case PKT_DELETE_LIGHT_REQUEST: {
        if (len < 1 + 4 + 4) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint32_t light_id; memcpy(&light_id, p, 4); p += 4;
        if (s_delete_light_handler) s_delete_light_handler(req_id, light_id);
        break;
    }

    default:
        printf("[net] Unknown packet type 0x%02x\n", type);
        break;
    }
}
