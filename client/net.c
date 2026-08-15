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

static void (*s_create_mesh_handler)(uint32_t req_id, float x, float y, float z,
                                       const float *positions, int vert_count,
                                       const unsigned short *indices, int index_count) = NULL;
static void (*s_set_vertices_handler)(uint32_t req_id, uint32_t object_id,
                                        const float *positions, int vert_count) = NULL;
static void (*s_delete_mesh_object_handler)(uint32_t req_id, uint32_t object_id) = NULL;

void net_set_create_mesh_handler(void (*handler)(uint32_t req_id, float x, float y, float z,
                                                    const float *positions, int vert_count,
                                                    const unsigned short *indices, int index_count)) {
    s_create_mesh_handler = handler;
}
void net_set_set_vertices_handler(void (*handler)(uint32_t req_id, uint32_t object_id,
                                                      const float *positions, int vert_count)) {
    s_set_vertices_handler = handler;
}
void net_set_delete_mesh_object_handler(void (*handler)(uint32_t req_id, uint32_t object_id)) {
    s_delete_mesh_object_handler = handler;
}

static void (*s_get_mesh_vertices_handler)(uint32_t req_id, uint32_t object_id) = NULL;
static void (*s_get_mesh_faces_handler)(uint32_t req_id, uint32_t object_id) = NULL;
static void (*s_add_mesh_vertex_handler)(uint32_t req_id, uint32_t object_id, float x, float y, float z) = NULL;
static void (*s_add_mesh_face_handler)(uint32_t req_id, uint32_t object_id, uint16_t v0, uint16_t v1, uint16_t v2) = NULL;
static void (*s_set_mesh_vertex_handler)(uint32_t req_id, uint32_t object_id, uint16_t vertex_index, float x, float y, float z) = NULL;

void net_set_get_mesh_vertices_handler(void (*handler)(uint32_t req_id, uint32_t object_id)) {
    s_get_mesh_vertices_handler = handler;
}
void net_set_get_mesh_faces_handler(void (*handler)(uint32_t req_id, uint32_t object_id)) {
    s_get_mesh_faces_handler = handler;
}
void net_set_add_mesh_vertex_handler(void (*handler)(uint32_t req_id, uint32_t object_id, float x, float y, float z)) {
    s_add_mesh_vertex_handler = handler;
}
void net_set_add_mesh_face_handler(void (*handler)(uint32_t req_id, uint32_t object_id, uint16_t v0, uint16_t v1, uint16_t v2)) {
    s_add_mesh_face_handler = handler;
}
void net_set_set_mesh_vertex_handler(void (*handler)(uint32_t req_id, uint32_t object_id, uint16_t vertex_index, float x, float y, float z)) {
    s_set_mesh_vertex_handler = handler;
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
 * plausibly exceed 255 bytes where names/tags/queries never do.
 *
 * `cap` is the caller's real remaining room for the STRING BODY (not
 * counting the 2-byte length prefix this function always writes) --
 * REQUIRED, not optional, after a real bug found here: this used to trust
 * strlen(s) unconditionally and just memcpy the whole string, and
 * net_send_scene_state_reply's pkt buffer (a stack array sized 3072) was
 * smaller than main.c's scene_state_handler can legitimately produce (its
 * own json[10240] buffer, easily exceeded 3072 with a handful of objects/
 * lights populated) -- a real stack buffer overflow on every scene-state
 * reply past that size, not a theoretical one. Truncating here is a
 * silent-but-safe last resort (the caller should size its own buffer
 * correctly, see net_send_scene_state_reply below for the actual fix) --
 * never trust a destination buffer is big enough just because it always
 * has been so far. */
static uint8_t *w_lenprefixed16(uint8_t *p, const char *s, size_t cap) {
    size_t n = s ? strlen(s) : 0;
    if (n > 65535) n = 65535;
    if (n > cap) n = cap;
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
    p = w_lenprefixed16(p, text, CHAT_INPUT_LEN);
    ws_send_binary(pkt, (int)(p - pkt));
}

/* pkt sized to comfortably exceed main.c's scene_state_handler's own
 * json[10240] buffer (see its comment for how that bound was derived) --
 * the REAL fix for the overflow described above; the cap passed to
 * w_lenprefixed16 is defense in depth, not the primary guard, in case
 * that bound ever grows without this buffer being revisited too. static,
 * not stack-local -- 10KB+ is not something to put on the stack
 * unconditionally on every reply, same reasoning as the mesh vertex/face
 * reply buffers above; this client is single-threaded on the calling
 * side, so static reuse is safe. */
void net_send_scene_state_reply(NetState *ns, uint32_t req_id, const char *json) {
    (void)ns;
    static uint8_t pkt[1 + 4 + 2 + 12288];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_SCENE_STATE_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_lenprefixed16(p, json, 12288);
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

void net_send_create_mesh_reply(NetState *ns, uint32_t req_id, int ok, uint32_t object_id) {
    (void)ns;
    uint8_t pkt[1 + 4 + 1 + 4];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_CREATE_MESH_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    memcpy(p, &object_id, 4); p += 4;
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_set_vertices_reply(NetState *ns, uint32_t req_id, int ok) {
    (void)ns;
    uint8_t pkt[1 + 4 + 1];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_SET_VERTICES_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_delete_mesh_object_reply(NetState *ns, uint32_t req_id, int ok) {
    (void)ns;
    uint8_t pkt[1 + 4 + 1];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_DELETE_MESH_OBJECT_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    ws_send_binary(pkt, (int)(p - pkt));
}

/* static, not stack-local -- same reasoning as PKT_CREATE_MESH_REQUEST's
 * own pos_buf/idx_buf below (a PKT_MESH_MAX_VERTS/TRIS-sized array is
 * tens of KB, not something to put on the stack unconditionally on every
 * call). This client is single-threaded on the calling side (main
 * render/network loop), so static reuse across calls is safe here the
 * same way it already is there. */
void net_send_get_mesh_vertices_reply(NetState *ns, uint32_t req_id, int ok, const float *positions, int vert_count) {
    (void)ns;
    if (vert_count > PKT_MESH_MAX_VERTS) vert_count = PKT_MESH_MAX_VERTS;   /* defensive -- see PKT_GET_MESH_VERTICES_REPLY's own comment */
    static uint8_t pkt[1 + 4 + 1 + 2 + PKT_MESH_MAX_VERTS * 3 * sizeof(float)];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_GET_MESH_VERTICES_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    uint16_t vc = (uint16_t)(ok ? vert_count : 0);
    memcpy(p, &vc, 2); p += 2;
    if (ok && vert_count > 0) {
        size_t n = (size_t)vert_count * 3 * sizeof(float);
        memcpy(p, positions, n); p += n;
    }
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_get_mesh_faces_reply(NetState *ns, uint32_t req_id, int ok, const uint16_t *face_indices, const uint16_t *verts, int face_count) {
    (void)ns;
    if (face_count > PKT_MESH_MAX_TRIS) face_count = PKT_MESH_MAX_TRIS;   /* defensive -- see PKT_GET_MESH_FACES_REPLY's own comment */
    static uint8_t pkt[1 + 4 + 1 + 2 + PKT_MESH_MAX_TRIS * 4 * sizeof(uint16_t)];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_GET_MESH_FACES_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    uint16_t fc = (uint16_t)(ok ? face_count : 0);
    memcpy(p, &fc, 2); p += 2;
    for (int i = 0; i < (ok ? face_count : 0); i++) {
        memcpy(p, &face_indices[i], 2); p += 2;
        memcpy(p, &verts[i*3+0], 2); p += 2;
        memcpy(p, &verts[i*3+1], 2); p += 2;
        memcpy(p, &verts[i*3+2], 2); p += 2;
    }
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_add_mesh_vertex_reply(NetState *ns, uint32_t req_id, int ok, uint32_t vertex_index) {
    (void)ns;
    uint8_t pkt[1 + 4 + 1 + 4];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_ADD_MESH_VERTEX_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    memcpy(p, &vertex_index, 4); p += 4;
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_add_mesh_face_reply(NetState *ns, uint32_t req_id, int ok, uint32_t face_index) {
    (void)ns;
    uint8_t pkt[1 + 4 + 1 + 4];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_ADD_MESH_FACE_REPLY);
    memcpy(p, &req_id, 4); p += 4;
    p = w_u8(p, ok ? 1 : 0);
    memcpy(p, &face_index, 4); p += 4;
    ws_send_binary(pkt, (int)(p - pkt));
}

void net_send_set_mesh_vertex_reply(NetState *ns, uint32_t req_id, int ok) {
    (void)ns;
    uint8_t pkt[1 + 4 + 1];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_SET_MESH_VERTEX_REPLY);
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

    case PKT_CREATE_MESH_REQUEST: {
        /* [req_id:u32 x:f32 y:f32 z:f32 vert_count:u16 (vert_count*3)*f32 tri_count:u16 (tri_count*3)*u16] --
         * positions/indices are memcpy'd into real aligned local arrays
         * (not cast-in-place from the raw byte cursor) -- p has no
         * guaranteed alignment for a multi-byte type on every platform,
         * same reasoning every OTHER multi-byte field in this parser
         * already reads via memcpy rather than a direct pointer cast. */
        if (len < 1 + 4 + 12 + 2) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        float x, y, z;
        memcpy(&x, p, 4); p += 4;
        memcpy(&y, p, 4); p += 4;
        memcpy(&z, p, 4); p += 4;
        uint16_t vert_count; memcpy(&vert_count, p, 2); p += 2;
        if (vert_count > PKT_MESH_MAX_VERTS) break;   /* malformed/oversized -- refuse rather than read past the buffer */
        size_t pos_bytes = (size_t)vert_count * 3 * sizeof(float);
        if (p + pos_bytes + 2 > data + len) break;
        static float pos_buf[PKT_MESH_MAX_VERTS * 3];
        memcpy(pos_buf, p, pos_bytes);
        p += pos_bytes;
        uint16_t tri_count; memcpy(&tri_count, p, 2); p += 2;
        if (tri_count > PKT_MESH_MAX_TRIS) break;
        size_t idx_bytes = (size_t)tri_count * 3 * sizeof(uint16_t);
        if (p + idx_bytes > data + len) break;
        static unsigned short idx_buf[PKT_MESH_MAX_TRIS * 3];
        memcpy(idx_buf, p, idx_bytes);
        if (s_create_mesh_handler) s_create_mesh_handler(req_id, x, y, z, pos_buf, (int)vert_count, idx_buf, (int)tri_count * 3);
        break;
    }

    case PKT_SET_VERTICES_REQUEST: {
        /* [req_id:u32 object_id:u32 vert_count:u16 (vert_count*3)*f32] --
         * same memcpy-into-aligned-local-array reasoning as above. */
        if (len < 1 + 4 + 4 + 2) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint32_t object_id; memcpy(&object_id, p, 4); p += 4;
        uint16_t vert_count; memcpy(&vert_count, p, 2); p += 2;
        if (vert_count > PKT_MESH_MAX_VERTS) break;
        size_t pos_bytes = (size_t)vert_count * 3 * sizeof(float);
        if (p + pos_bytes > data + len) break;
        static float pos_buf[PKT_MESH_MAX_VERTS * 3];
        memcpy(pos_buf, p, pos_bytes);
        if (s_set_vertices_handler) s_set_vertices_handler(req_id, object_id, pos_buf, (int)vert_count);
        break;
    }

    case PKT_DELETE_MESH_OBJECT_REQUEST: {
        if (len < 1 + 4 + 4) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint32_t object_id; memcpy(&object_id, p, 4); p += 4;
        if (s_delete_mesh_object_handler) s_delete_mesh_object_handler(req_id, object_id);
        break;
    }

    case PKT_GET_MESH_VERTICES_REQUEST: {
        if (len < 1 + 4 + 4) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint32_t object_id; memcpy(&object_id, p, 4); p += 4;
        if (s_get_mesh_vertices_handler) s_get_mesh_vertices_handler(req_id, object_id);
        break;
    }

    case PKT_GET_MESH_FACES_REQUEST: {
        if (len < 1 + 4 + 4) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint32_t object_id; memcpy(&object_id, p, 4); p += 4;
        if (s_get_mesh_faces_handler) s_get_mesh_faces_handler(req_id, object_id);
        break;
    }

    case PKT_ADD_MESH_VERTEX_REQUEST: {
        if (len < 1 + 4 + 4 + 12) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint32_t object_id; memcpy(&object_id, p, 4); p += 4;
        float x, y, z;
        memcpy(&x, p, 4); p += 4;
        memcpy(&y, p, 4); p += 4;
        memcpy(&z, p, 4); p += 4;
        if (s_add_mesh_vertex_handler) s_add_mesh_vertex_handler(req_id, object_id, x, y, z);
        break;
    }

    case PKT_ADD_MESH_FACE_REQUEST: {
        if (len < 1 + 4 + 4 + 6) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint32_t object_id; memcpy(&object_id, p, 4); p += 4;
        uint16_t v0, v1, v2;
        memcpy(&v0, p, 2); p += 2;
        memcpy(&v1, p, 2); p += 2;
        memcpy(&v2, p, 2); p += 2;
        if (s_add_mesh_face_handler) s_add_mesh_face_handler(req_id, object_id, v0, v1, v2);
        break;
    }

    case PKT_SET_MESH_VERTEX_REQUEST: {
        if (len < 1 + 4 + 4 + 2 + 12) break;
        uint32_t req_id; memcpy(&req_id, p, 4); p += 4;
        uint32_t object_id; memcpy(&object_id, p, 4); p += 4;
        uint16_t vertex_index; memcpy(&vertex_index, p, 2); p += 2;
        float x, y, z;
        memcpy(&x, p, 4); p += 4;
        memcpy(&y, p, 4); p += 4;
        memcpy(&z, p, 4); p += 4;
        if (s_set_mesh_vertex_handler) s_set_mesh_vertex_handler(req_id, object_id, vertex_index, x, y, z);
        break;
    }

    default:
        printf("[net] Unknown packet type 0x%02x\n", type);
        break;
    }
}
