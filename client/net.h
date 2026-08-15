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

/* Chat-driven scene MUTATION (see phi.md's Phase 3 status/"Where AI
 * fits") -- same "server asks THIS client to act, since the live scene
 * state only exists in the client process" reasoning PKT_SCENE_STATE_*
 * already established, just for a write instead of a read. Wired to
 * three of the Anthropic tool-use loop's real tools (server/
 * anthropic_client.py's add_light/set_light_property/set_render_samples/
 * delete_light) -- Claude's access to lights/render settings is
 * explicitly read-WRITE per this project's own decision, unlike the
 * read-only get_asset_list/get_scene_state tools. req_id round-trips the
 * same way PKT_SCENE_STATE_REQUEST/REPLY's already does, for the same
 * reason (matching a reply to the specific pending tool call). */
#define PKT_PROP_SET_REQUEST     0x24   /* S->C: [req_id:u32 target_len:u8 target:bytes ident_len:u8 ident:bytes is_vec3:u8 v0:f32 v1:f32 v2:f32] -- target is "light:<id>" or "render", same strings scene_resolve_target() already accepts */
#define PKT_PROP_SET_REPLY       0x25   /* C->S: [req_id:u32 ok:u8] */
#define PKT_ADD_LIGHT_REQUEST    0x26   /* S->C: [req_id:u32 type:u8 x:f32 y:f32 z:f32] -- type: 0=point 1=sun 2=spot 3=area */
#define PKT_ADD_LIGHT_REPLY      0x27   /* C->S: [req_id:u32 ok:u8 light_id:u32] -- light_id is 0 (never a real id) on failure */
#define PKT_DELETE_LIGHT_REQUEST 0x28   /* S->C: [req_id:u32 light_id:u32] */
#define PKT_DELETE_LIGHT_REPLY   0x29   /* C->S: [req_id:u32 ok:u8] */

/* Phase 5's real geometry-creation/vertex-editing tools (see phi.md --
 * "make it so Claude can create geometry and redefine the verts of
 * existing scene geometry"), same "server asks THIS client to act"
 * shape as PKT_ADD_LIGHT_REQUEST/PKT_PROP_SET_REQUEST above -- wired to
 * three more of the Anthropic tool-use loop's real tools (server/
 * anthropic_client.py's create_mesh_object/set_mesh_vertices/delete_
 * mesh_object). vert_count/tri_count are each capped at PKT_MESH_MAX_
 * VERTS/PKT_MESH_MAX_TRIS -- a real, deliberately modest wire-protocol
 * bound (distinct from the LOCAL Python console's phi.create_mesh/
 * set_vertices, which have no such extra cap beyond the uint16_t index
 * width this codebase's other index arrays already use) matched to what
 * a chat-driven request would realistically ask for, not an arbitrary
 * dense mesh import (that's what the Asset Browser/glTF path is for). */
#define PKT_MESH_MAX_VERTS 2048
#define PKT_MESH_MAX_TRIS  4096

#define PKT_CREATE_MESH_REQUEST        0x2A   /* S->C: [req_id:u32 x:f32 y:f32 z:f32 vert_count:u16 (vert_count*3)*f32 positions tri_count:u16 (tri_count*3)*u16 indices] */
#define PKT_CREATE_MESH_REPLY          0x2B   /* C->S: [req_id:u32 ok:u8 object_id:u32] -- object_id is 0 (never a real id) on failure */
#define PKT_SET_VERTICES_REQUEST       0x2C   /* S->C: [req_id:u32 object_id:u32 vert_count:u16 (vert_count*3)*f32 positions] -- vert_count MUST match the target object's existing vertex count */
#define PKT_SET_VERTICES_REPLY         0x2D   /* C->S: [req_id:u32 ok:u8] */
#define PKT_DELETE_MESH_OBJECT_REQUEST 0x2E   /* S->C: [req_id:u32 object_id:u32] */
#define PKT_DELETE_MESH_OBJECT_REPLY   0x2F   /* C->S: [req_id:u32 ok:u8] */

/* Incremental mesh editing for chat -- "see where vertices are, add
 * vertices to an existing mesh, move individual vertices" (see phi.md).
 * Same "server asks THIS client to act" shape as everything above; the
 * two GET_MESH_* pairs are the read side create_mesh_object/set_mesh_
 * vertices never needed before (that pair could only build-from-scratch
 * or replace-everything-at-the-same-count -- neither lets the model see
 * or incrementally grow existing geometry). Mirrors mp_port.c's phi.
 * get_vertices/get_faces/add_vertex/add_face/set_vertex wire-for-wire in
 * spirit -- same underlying halfedge.c calls, just reached over the
 * network instead of an embedded interpreter. ADD_MESH_FACE isn't
 * something the user literally asked for, but a lone added vertex is
 * invisible (the render mesh only ever reflects live FACES, see
 * mp_port.c's native_add_vertex comment) -- without it, "add vertices"
 * would be a capability that visibly does nothing. */
#define PKT_GET_MESH_VERTICES_REQUEST  0x30   /* S->C: [req_id:u32 object_id:u32] */
#define PKT_GET_MESH_VERTICES_REPLY    0x31   /* C->S: [req_id:u32 ok:u8 vert_count:u16 (vert_count*3)*f32 positions] */
#define PKT_GET_MESH_FACES_REQUEST     0x32   /* S->C: [req_id:u32 object_id:u32] */
#define PKT_GET_MESH_FACES_REPLY       0x33   /* C->S: [req_id:u32 ok:u8 face_count:u16 face_count*{face_index:u16 v0:u16 v1:u16 v2:u16}] -- triangles only, see halfedge.h */
#define PKT_ADD_MESH_VERTEX_REQUEST    0x34   /* S->C: [req_id:u32 object_id:u32 x:f32 y:f32 z:f32] */
#define PKT_ADD_MESH_VERTEX_REPLY      0x35   /* C->S: [req_id:u32 ok:u8 vertex_index:u32] */
#define PKT_ADD_MESH_FACE_REQUEST      0x36   /* S->C: [req_id:u32 object_id:u32 v0:u16 v1:u16 v2:u16] -- must reference existing (or just-added) vertex indices */
#define PKT_ADD_MESH_FACE_REPLY        0x37   /* C->S: [req_id:u32 ok:u8 face_index:u32] */
#define PKT_SET_MESH_VERTEX_REQUEST    0x38   /* S->C: [req_id:u32 object_id:u32 vertex_index:u16 x:f32 y:f32 z:f32] -- moves ONE existing vertex, unlike PKT_SET_VERTICES_REQUEST's whole-array replace */
#define PKT_SET_MESH_VERTEX_REPLY      0x39   /* C->S: [req_id:u32 ok:u8] */

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
void net_send_prop_set_reply(NetState *ns, uint32_t req_id, int ok);
void net_send_add_light_reply(NetState *ns, uint32_t req_id, int ok, uint32_t light_id);
void net_send_delete_light_reply(NetState *ns, uint32_t req_id, int ok);
void net_send_create_mesh_reply(NetState *ns, uint32_t req_id, int ok, uint32_t object_id);
void net_send_set_vertices_reply(NetState *ns, uint32_t req_id, int ok);
void net_send_delete_mesh_object_reply(NetState *ns, uint32_t req_id, int ok);

/* positions is vert_count*3 floats (borrowed, valid only for the call). */
void net_send_get_mesh_vertices_reply(NetState *ns, uint32_t req_id, int ok, const float *positions, int vert_count);
/* face_indices/verts are parallel arrays, face_count entries each (verts holds 3 per face, flat). */
void net_send_get_mesh_faces_reply(NetState *ns, uint32_t req_id, int ok, const uint16_t *face_indices, const uint16_t *verts, int face_count);
void net_send_add_mesh_vertex_reply(NetState *ns, uint32_t req_id, int ok, uint32_t vertex_index);
void net_send_add_mesh_face_reply(NetState *ns, uint32_t req_id, int ok, uint32_t face_index);
void net_send_set_mesh_vertex_reply(NetState *ns, uint32_t req_id, int ok);

/* Registers a callback net_on_message invokes on PKT_SCENE_STATE_REQUEST --
 * net.c has no access to MeshObject/physics-world state itself (that's
 * main.c's global state, see phi.md's client-authored architecture), so it
 * hands the req_id off to whoever main_init() registered instead of
 * building the reply itself, the same division of labor console.c/
 * asset_browser.c already have for parsing-vs-owning-the-data. */
void net_set_scene_state_handler(void (*handler)(uint32_t req_id));

/* Same division of labor, for the three chat-driven mutation requests
 * (see PKT_PROP_SET_REQUEST's own comment) -- main.c owns the actual
 * light registry/render settings, net.c just parses the wire payload and
 * hands the pieces off. */
void net_set_prop_set_handler(void (*handler)(uint32_t req_id, const char *target, const char *identifier,
                                               int is_vec3, float v0, float v1, float v2));
void net_set_add_light_handler(void (*handler)(uint32_t req_id, int type, float x, float y, float z));
void net_set_delete_light_handler(void (*handler)(uint32_t req_id, uint32_t light_id));

/* Phase 5's geometry tools -- same division of labor (net.c parses, main.c
 * owns/mutates the real scene_objects.c registry). positions/indices are
 * borrowed pointers into net.c's own parse buffer, valid only for the
 * duration of the handler call (main.c must copy anything it needs to
 * keep, though in practice these handlers consume them immediately via
 * scene_objects.c/halfedge.c calls that copy the data themselves). */
void net_set_create_mesh_handler(void (*handler)(uint32_t req_id, float x, float y, float z,
                                                   const float *positions, int vert_count,
                                                   const unsigned short *indices, int index_count));
void net_set_set_vertices_handler(void (*handler)(uint32_t req_id, uint32_t object_id,
                                                     const float *positions, int vert_count));
void net_set_delete_mesh_object_handler(void (*handler)(uint32_t req_id, uint32_t object_id));

/* Incremental mesh editing handlers -- see PKT_GET_MESH_VERTICES_REQUEST's
 * own comment above. Same division of labor as everything else in this
 * section (net.c parses, main.c owns scene_objects.c/halfedge.c). */
void net_set_get_mesh_vertices_handler(void (*handler)(uint32_t req_id, uint32_t object_id));
void net_set_get_mesh_faces_handler(void (*handler)(uint32_t req_id, uint32_t object_id));
void net_set_add_mesh_vertex_handler(void (*handler)(uint32_t req_id, uint32_t object_id, float x, float y, float z));
void net_set_add_mesh_face_handler(void (*handler)(uint32_t req_id, uint32_t object_id, uint16_t v0, uint16_t v1, uint16_t v2));
void net_set_set_mesh_vertex_handler(void (*handler)(uint32_t req_id, uint32_t object_id, uint16_t vertex_index, float x, float y, float z));

#ifndef __EMSCRIPTEN__
/* Native only: pumps the WebSocket socket (non-blocking) once per frame.
 * wasm has no equivalent — messages arrive via an async JS callback instead. */
void net_poll_native(void);
#endif
