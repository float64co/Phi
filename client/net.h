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

/* Asset CRUD (see phi.md's "Wire protocol: CRUD over a hybrid HTTP + WS
 * split"): List/Update/Delete are small structured messages that benefit
 * from this channel's low-latency push, including the CHANGED broadcast
 * so every connected editor's Asset Browser panel stays in sync. Create
 * and the raw-bytes half of Read are HTTP instead (server.py's POST
 * /assets and GET /assets/<path>) -- arbitrary-size binary blobs, not a
 * fit for this small-packet channel. */
#define PKT_ASSET_LIST_REQUEST  0x10   /* C->S: [qlen:u8 query:bytes] (qlen=0 = no filter) */
#define PKT_ASSET_LIST_REPLY    0x11   /* S->C: [count:u16] then count * {id:u32 name_len:u8 name path_len:u8 path tags_len:u8 tags(csv)} */
#define PKT_ASSET_UPDATE        0x12   /* C->S: [id:u32 name_len:u8 name tags_len:u8 tags(csv)] */
#define PKT_ASSET_DELETE        0x13   /* C->S: [id:u32] */
#define PKT_ASSET_CHANGED       0x14   /* S->C: no payload -- "the asset list changed, re-request if you care" */

/* Chat + live introspection (see phi.md's "Where AI fits" / the Chat
 * panel's status note): a chat message goes over this same low-latency
 * WS channel rather than HTTP, same reasoning as Asset List/Update/Delete
 * above -- small structured messages, not a fit for the asset-blob HTTP
 * endpoints. PKT_SCENE_STATE_REQUEST/REPLY is the server asking THIS
 * client to report its own live engine state (server.py has no access to
 * it otherwise -- per this project's client-authored architecture, the
 * authoritative live MeshObject/physics state lives in the client
 * process, not the server) as one of the Anthropic tool-use loop's real
 * tools (server/anthropic_client.py's get_scene_state). req_id round-trips
 * so the server can match a reply to the specific pending tool call that
 * asked for it (a client could, in principle, receive a second request
 * before answering the first). */
#define PKT_CHAT_MSG             0x20   /* C->S: [len:u16 utf8:bytes] -- one chat message from the local user */
#define PKT_CHAT_REPLY           0x21   /* S->C: [len:u16 utf8:bytes] -- the assistant's final text reply for this client's Chat panel */
#define PKT_SCENE_STATE_REQUEST  0x22   /* S->C: [req_id:u32] -- "report your live scene/engine state" */
#define PKT_SCENE_STATE_REPLY    0x23   /* C->S: [req_id:u32 len:u16 utf8-json:bytes] -- reply to a PKT_SCENE_STATE_REQUEST */

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

/* query="" (or NULL) requests the full, unfiltered list. */
void net_send_asset_list_request(NetState *ns, const char *query);
void net_send_asset_update(NetState *ns, uint32_t id, const char *name, const char *tags_csv);
void net_send_asset_delete(NetState *ns, uint32_t id);

void net_send_chat_msg(NetState *ns, const char *text);
void net_send_scene_state_reply(NetState *ns, uint32_t req_id, const char *json);

/* Registers a callback net_on_message invokes on PKT_SCENE_STATE_REQUEST --
 * net.c has no access to MeshObject/physics-world state itself (that's
 * main.c's global state, see phi.md's client-authored architecture), so it
 * hands the req_id off to whoever main_init() registered instead of
 * building the reply itself, the same division of labor console.c/
 * asset_browser.c already have for parsing-vs-owning-the-data. */
void net_set_scene_state_handler(void (*handler)(uint32_t req_id));

#ifndef __EMSCRIPTEN__
/* Native only: pumps the WebSocket socket (non-blocking) once per frame.
 * wasm has no equivalent — messages arrive via an async JS callback instead. */
void net_poll_native(void);
#endif
