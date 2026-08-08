#pragma once
#include <stdint.h>
#include "physics.h"
#include "octree.h"

/* ---- Binary protocol (matches server) ---- */
#define PKT_HELLO        0x01
#define PKT_STATE        0x02
#define PKT_INPUT        0x03
#define PKT_SPAWN_ROCKET 0x04
#define PKT_EXPLODE      0x05
#define PKT_DAMAGE       0x06
#define PKT_OBITUARY     0x07
#define PKT_FIRE         0x08   /* C→S: [yaw:f32 pitch:f32 crouch:u8] */

/* ---- Editor / console protocol ---- */
#define PKT_EDIT_REGION  0x09   /* C→S: [op:u8 face:u8 mat:u8 minx,miny,minz,maxx,maxy,maxz:u16] */
#define PKT_MAP_FULL     0x0A   /* S→C: raw .cmap bytes (authoritative world, sent on connect + after any edit) */
#define PKT_SAVE_MAP     0x0B   /* C→S: [name_len:u8 name:bytes] */
#define PKT_LOAD_MAP     0x0C   /* C→S: [name_len:u8 name:bytes] */
#define PKT_CONSOLE_MSG  0x0D   /* S→C: [len:u16 utf8:bytes] echoed into the client console log */
#define PKT_NEW_MAP      0x0E   /* C→S: no payload — regenerate the default arena, save, broadcast */
#define PKT_LIST_MAPS    0x0F   /* C→S: no payload — server replies with a PKT_CONSOLE_MSG listing maps/ */

/* PKT_EDIT_REGION op byte */
#define EDIT_OP_EMPTY    0
#define EDIT_OP_SOLID    1
#define EDIT_OP_MATERIAL 2

/* player_state_t over the wire (25 bytes) */
typedef struct __attribute__((packed)) {
    uint8_t id;
    float   x, y, z;       /* pos */
    float   vx, vy, vz;    /* vel */
    float   yaw;
    uint8_t hp;
} WirePlayerState;          /* 25 bytes */

typedef struct {
    int  connected;
    int  local_id;
    char ws_url[128];

    /* Set by net_on_message on PKT_MAP_FULL; main.c polls this each frame
     * and swaps it into g_world (net.c doesn't own the renderer/mesh). */
    Octree *pending_map;
    int     has_pending_map;
} NetState;

/* Initialize WebSocket connection */
void net_connect(NetState *ns, const char *url);

/* Send our input to the server */
void net_send_input(NetState *ns, uint16_t seq,
                    float yaw, float pitch, uint8_t keys);

/* Send fire request */
void net_send_fire(NetState *ns, float yaw, float pitch, int crouch);

/* Editor: request a region edit (box is [min,max), op is EDIT_OP_*;
 * face/mat only meaningful for EDIT_OP_MATERIAL) */
void net_send_edit_region(NetState *ns, int op, int face, int mat,
                          int minx, int miny, int minz,
                          int maxx, int maxy, int maxz);

/* Console: save/load a named map on the server */
void net_send_save_map(NetState *ns, const char *name);
void net_send_load_map(NetState *ns, const char *name);
void net_send_new_map(NetState *ns);
void net_send_list_maps(NetState *ns);

/* Called when a message arrives - updates game state */
void net_on_message(GameState *gs, NetState *ns,
                    const uint8_t *data, int len);

/* Send hello with player name */
void net_send_hello(NetState *ns, const char *name);

/* Set the game state pointer used by the WebSocket receive callback */
void net_set_game_state(GameState *gs);
