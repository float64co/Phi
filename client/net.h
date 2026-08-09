#pragma once
#include <stdint.h>

/* ---- Binary protocol (matches server) ----
 * Repurposed from Qek's FPS/octree wire protocol (see phi.md's Phase 1
 * status, "Client/server model") -- every packet type tied to combat/
 * movement state sync (PKT_STATE/INPUT/SPAWN_ROCKET/EXPLODE/DAMAGE/
 * OBITUARY/FIRE) or the octree voxel world (PKT_EDIT_REGION/MAP_FULL/
 * SAVE_MAP/LOAD_MAP/NEW_MAP/LIST_MAPS) is gone along with the client-side
 * code that used it. What's left is the minimal connection handshake plus
 * a generic server->client text-message channel -- the foundation for
 * whatever real multiplayer game-authoring/gameplay state sync gets
 * designed on top of this transport next, not a finished protocol of its
 * own. */
#define PKT_HELLO        0x01   /* C->S: [name:16 bytes, NUL-padded]. S->C: [id:u8] */
#define PKT_CONSOLE_MSG  0x0D   /* S->C: [len:u16 utf8:bytes], appended to the client's Python panel log */

typedef struct {
    int  connected;
    int  local_id;
    char ws_url[128];
} NetState;

/* Initialize WebSocket connection */
void net_connect(NetState *ns, const char *url);

/* Called when a message arrives */
void net_on_message(NetState *ns, const uint8_t *data, int len);

/* Send hello with a client name */
void net_send_hello(NetState *ns, const char *name);

#ifndef __EMSCRIPTEN__
/* Native only: pumps the WebSocket socket (non-blocking) once per frame.
 * wasm has no equivalent — messages arrive via an async JS callback instead. */
void net_poll_native(void);
#endif
