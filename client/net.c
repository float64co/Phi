#include "net.h"
#include "physics.h"
#include "cmap.h"
#include "console.h"
#include <string.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/websocket.h>

static EMSCRIPTEN_WEBSOCKET_T s_ws = 0;
static GameState              *s_gs  = NULL;
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
    if (e->numBytes > 0 && s_gs && s_ns)
        net_on_message(s_gs, s_ns, (const uint8_t *)e->data, (int)e->numBytes);
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

static GameState *s_gs_native = NULL;
static NetState   *s_ns_native = NULL;

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
    if (s_gs_native && s_ns_native) net_on_message(s_gs_native, s_ns_native, data, len);
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

static GameState *s_gs_native = NULL;
static NetState   *s_ns_native = NULL;

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
    if (s_gs_native && s_ns_native) net_on_message(s_gs_native, s_ns_native, data, len);
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
static uint8_t *w_u16(uint8_t *p, uint16_t v) { memcpy(p,&v,2); return p+2; }
static uint8_t *w_f32(uint8_t *p, float    v) { memcpy(p,&v,4); return p+4; }

void net_send_hello(NetState *ns, const char *name) {
    (void)ns;
    uint8_t pkt[17];
    uint8_t *p = pkt;
    p = w_u8(p, PKT_HELLO);
    memset(p, 0, 16);
    strncpy((char *)p, name, 16);
    ws_send_binary(pkt, sizeof(pkt));
}

void net_send_input(NetState *ns, uint16_t seq,
                    float yaw, float pitch, uint8_t keys) {
    (void)ns;
    uint8_t pkt[1+2+4+4+1];
    uint8_t *p = pkt;
    p = w_u8(p,  PKT_INPUT);
    p = w_u16(p, seq);
    p = w_f32(p, yaw);
    p = w_f32(p, pitch);
    p = w_u8(p,  keys);
    ws_send_binary(pkt, sizeof(pkt));
}

void net_send_fire(NetState *ns, float yaw, float pitch, int crouch) {
    (void)ns;
    uint8_t pkt[1+4+4+1];
    uint8_t *p = pkt;
    p = w_u8(p,  PKT_FIRE);
    p = w_f32(p, yaw);
    p = w_f32(p, pitch);
    p = w_u8(p,  (uint8_t)(crouch ? 1 : 0));
    ws_send_binary(pkt, sizeof(pkt));
}

void net_send_edit_region(NetState *ns, int op, int face, int mat,
                          int minx, int miny, int minz,
                          int maxx, int maxy, int maxz) {
    (void)ns;
    uint8_t pkt[1+1+1+1+12];
    uint8_t *p = pkt;
    p = w_u8(p,  PKT_EDIT_REGION);
    p = w_u8(p,  (uint8_t)op);
    p = w_u8(p,  (uint8_t)face);
    p = w_u8(p,  (uint8_t)mat);
    p = w_u16(p, (uint16_t)minx);
    p = w_u16(p, (uint16_t)miny);
    p = w_u16(p, (uint16_t)minz);
    p = w_u16(p, (uint16_t)maxx);
    p = w_u16(p, (uint16_t)maxy);
    p = w_u16(p, (uint16_t)maxz);
    ws_send_binary(pkt, sizeof(pkt));
}

static void send_named_map_pkt(uint8_t type, const char *name) {
    uint8_t pkt[1+1+32];
    size_t nlen = strlen(name);
    if (nlen > 32) nlen = 32;
    uint8_t *p = pkt;
    p = w_u8(p, type);
    p = w_u8(p, (uint8_t)nlen);
    memcpy(p, name, nlen);
    ws_send_binary(pkt, (int)(2 + nlen));
}

void net_send_save_map(NetState *ns, const char *name) {
    (void)ns;
    send_named_map_pkt(PKT_SAVE_MAP, name);
}

void net_send_load_map(NetState *ns, const char *name) {
    (void)ns;
    send_named_map_pkt(PKT_LOAD_MAP, name);
}

static void send_bare_pkt(uint8_t type) {
    uint8_t pkt[1];
    w_u8(pkt, type);
    ws_send_binary(pkt, sizeof(pkt));
}

void net_send_new_map(NetState *ns) {
    (void)ns;
    send_bare_pkt(PKT_NEW_MAP);
}

void net_send_list_maps(NetState *ns) {
    (void)ns;
    send_bare_pkt(PKT_LIST_MAPS);
}

/* ---- Read helpers ---- */
static const uint8_t *r_u8 (const uint8_t *p, uint8_t  *v) { *v=*p;            return p+1; }
static const uint8_t *r_f32(const uint8_t *p, float    *v) { memcpy(v,p,4);   return p+4; }
static const uint8_t *r_i8 (const uint8_t *p, int8_t   *v) { *v=(int8_t)*p;   return p+1; }

void net_on_message(GameState *gs, NetState *ns,
                    const uint8_t *data, int len) {
    if (len < 1) return;
    const uint8_t *p = data;
    uint8_t type = *p++;

    switch (type) {

    case PKT_STATE: {
        /* [n_players: u8] [n × WirePlayerState] */
        if (len < 2) return;
        uint8_t n = *p++;
        for (int i = 0; i < n && i < 16; i++) {
            if (p + 25 > data + len) break;
            WirePlayerState ws;
            memcpy(&ws, p, 25); p += 25;
            /* Find or create player */
            Player *pl = NULL;
            for (int j = 0; j < gs->num_players; j++) {
                if (gs->players[j].id == ws.id) { pl = &gs->players[j]; break; }
            }
            if (!pl && gs->num_players < 16) {
                pl = &gs->players[gs->num_players++];
                memset(pl, 0, sizeof(*pl));
                pl->id    = ws.id;
                pl->alive = 1;
            }
            if (pl && pl->id != (uint8_t)ns->local_id) {
                /* Update remote player state */
                pl->pos = (Vec3f){ws.x,  ws.y,  ws.z};
                pl->vel = (Vec3f){ws.vx, ws.vy, ws.vz};
                pl->yaw = ws.yaw;
                pl->hp  = ws.hp;
            }
        }
        break;
    }

    case PKT_HELLO: {
        /* Server sends our assigned ID */
        uint8_t id; p = r_u8(p, &id);
        ns->local_id = id;
        printf("[net] Assigned player id=%d\n", id);
        /* Init local player */
        Player *lp = &gs->players[0];
        if (gs->num_players == 0) gs->num_players = 1;
        lp->id    = id;
        lp->pos   = (Vec3f){128, 48, 128};
        lp->hp    = 100;
        lp->alive = 1;
        break;
    }

    case PKT_SPAWN_ROCKET: {
        uint8_t rid, oid; float px,py,pz,vx,vy,vz;
        p = r_u8(p,&rid); p = r_u8(p,&oid);
        p = r_f32(p,&px); p = r_f32(p,&py); p = r_f32(p,&pz);
        p = r_f32(p,&vx); p = r_f32(p,&vy); p = r_f32(p,&vz);
        /* Find free rocket slot */
        for (int i = 0; i < MAX_ROCKETS; i++) {
            if (!gs->rockets[i].active) {
                gs->rockets[i].id       = rid;
                gs->rockets[i].owner_id = oid;
                gs->rockets[i].pos      = (Vec3f){px,py,pz};
                gs->rockets[i].vel      = (Vec3f){vx,vy,vz};
                gs->rockets[i].active   = 1;
                gs->rockets[i].lifetime = 8.0f;
                break;
            }
        }
        break;
    }

    case PKT_EXPLODE: {
        uint8_t rid; float px,py,pz;
        p = r_u8(p,&rid);
        p = r_f32(p,&px); p = r_f32(p,&py); p = r_f32(p,&pz);
        for (int i = 0; i < MAX_ROCKETS; i++) {
            if (gs->rockets[i].id == rid) {
                gs->rockets[i].active = 0; break;
            }
        }
        /* TODO: spawn particle effect at px,py,pz */
        break;
    }

    case PKT_DAMAGE: {
        uint8_t pid; int8_t delta;
        p = r_u8(p,&pid); p = r_i8(p,&delta);
        for (int i = 0; i < gs->num_players; i++) {
            if (gs->players[i].id == pid) {
                gs->players[i].hp += delta;
                if (gs->players[i].hp < 0) gs->players[i].hp = 0;
                break;
            }
        }
        break;
    }

    case PKT_OBITUARY: {
        uint8_t killer, victim;
        p = r_u8(p,&killer); p = r_u8(p,&victim);
        printf("[game] Player %d killed player %d\n", killer, victim);
        for (int i = 0; i < gs->num_players; i++) {
            if (gs->players[i].id == victim) {
                gs->players[i].alive = 0;
                gs->players[i].respawn_timer = 3.0f;
            }
        }
        break;
    }

    case PKT_MAP_FULL: {
        /* p points just past the type byte; rest of the message is raw
         * .cmap bytes. main.c polls ns->has_pending_map each frame and
         * swaps it into the live world (net.c doesn't own the mesh/renderer). */
        Octree *ot = cmap_deserialize(p, (size_t)(len - 1));
        if (ot) {
            ns->pending_map     = ot;
            ns->has_pending_map = 1;
        } else {
            printf("[net] Failed to deserialize PKT_MAP_FULL (%d bytes)\n", len - 1);
        }
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

/* Expose gs pointer for ws callback */
void net_set_game_state(GameState *gs) {
#ifdef __EMSCRIPTEN__
    s_gs = gs;
#elif defined(_WIN32)
    s_gs_native = gs;
#else
    s_gs_native = gs;
#endif
}
