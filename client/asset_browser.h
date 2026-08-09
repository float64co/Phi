#pragma once
#include <stdint.h>
#include "input.h"

/* Client-side cache + intent-queue for the Asset Browser panel (see
 * phi.md's "Asset tracking and the Asset Browser panel" / "Wire protocol:
 * CRUD over a hybrid HTTP + WS split"). Owns:
 *
 *   1. The last list of assets the server sent us (a cache of the
 *      server's SQLite index, never the source of truth itself).
 *   2. One-shot request flags that ui.c sets when the user clicks a
 *      button, and that main.c polls once per frame and turns into real
 *      net_send_asset_*()/HTTP calls — the same "UI raises intent, main.c
 *      executes it" shape ui_poll_context_menu_action() already
 *      established, rather than ui.c reaching into net.c/the HTTP client
 *      directly.
 *   3. This codebase's one text-field-focus model (AssetBrowserFocus) --
 *      needed because console.c was deliberately built as "the only text
 *      field, always focused, no toggle", and this panel now has three
 *      candidate text fields (search, and the shared edit_name/edit_tags
 *      pair used for both rename and create) all competing with it.
 *
 * Parsing PKT_ASSET_LIST_REPLY's bytes lives here too (not net.c), same
 * division of labor console.c/net.c already have for PKT_CONSOLE_MSG:
 * net.c strips the opcode byte and hands the rest off. */

#define ASSET_BROWSER_MAX  64
#define ASSET_NAME_LEN     64
#define ASSET_PATH_LEN     128
#define ASSET_TAGS_LEN     96
#define ASSET_SEARCH_LEN   64

/* editing_id sentinels -- real asset ids are uint32_t and always >= 0, so
 * these live outside that range instead of stealing a bit. */
#define AB_EDITING_NONE  (-1)
#define AB_EDITING_NEW   (-2)   /* edit_name/edit_tags describe a NOT-YET-uploaded asset, see create_requested */

typedef enum {
    AB_FOCUS_NONE = 0,
    AB_FOCUS_SEARCH,
    AB_FOCUS_EDIT_NAME,
    AB_FOCUS_EDIT_TAGS,
} AssetBrowserFocus;

typedef struct {
    uint32_t id;
    char     name[ASSET_NAME_LEN];
    char     path[ASSET_PATH_LEN];
    char     tags[ASSET_TAGS_LEN];   /* comma-separated, as received over the wire */
} AssetSummary;

typedef struct {
    AssetSummary items[ASSET_BROWSER_MAX];
    int          count;
    int          selected;            /* index into items, -1 = none */

    /* Search bar text -- filters by name/tag (server-side, a SQLite LIKE
     * against both, see assets_db.py's list_assets) rather than filtering
     * the local cache, so it reflects the server's actual index, not just
     * whatever happened to already be listed. Submitted on Enter or a
     * Refresh click (sets refresh_requested), NOT live-as-you-type -- one
     * network round trip per keystroke isn't worth it. Empty string = no
     * filter, same as passing NULL to net_send_asset_list_request. */
    char search[ASSET_SEARCH_LEN];
    int  search_len;

    /* Which single text field (if any) owns this frame's keyboard input
     * instead of the Python console -- see ui.c's ui_on_mouse_button for
     * the click-away-blurs convention, mirrored from the type-switcher
     * dropdown/context menu. */
    AssetBrowserFocus focus;

    /* Shared edit buffers for BOTH rename (existing asset) and create (a
     * pending new one) -- only one can be active at a time (single focus,
     * same as everything else here), so there's no need for two separate
     * pairs of buffers. Which mode it is is entirely determined by
     * editing_id: AB_EDITING_NONE = not editing anything (these buffers'
     * contents are stale/ignored), AB_EDITING_NEW = a pending create
     * (edit_name/edit_tags describe an asset that doesn't exist on the
     * server yet), >= 0 = renaming/retagging that existing asset id. */
    int32_t editing_id;
    char    edit_name[ASSET_NAME_LEN];
    int     edit_name_len;
    char    edit_tags[ASSET_TAGS_LEN];
    int     edit_tags_len;

    /* One-shot request flags, drained by main.c each frame (see main.c's
     * asset browser poll block). Cleared by main.c after acting on them,
     * not by asset_browser.c itself -- mirrors InputState's lmb_click/etc.
     * "producer sets, consumer clears" convention. */
    int      refresh_requested;
    int      load_requested;
    uint32_t load_requested_id;
    int      delete_requested;
    uint32_t delete_requested_id;
    /* editing_id >= 0 when this fires: renames/retags that existing
     * asset to edit_name/edit_tags (net_send_asset_update). */
    int      update_requested;
    /* editing_id == AB_EDITING_NEW when this fires: main.c flattens the
     * currently selected MeshObject to a GLB and POSTs it as edit_name/
     * edit_tags (see main.c's "Save as Asset" handling). */
    int      create_requested;
} AssetBrowserState;

void asset_browser_init(AssetBrowserState *ab);

/* Registers ab as the target for asset_browser_ingest_list_reply()/
 * asset_browser_mark_dirty() (mirrors console.c's single-instance s_cs
 * pattern) -- there's only ever one Asset Browser. */
void asset_browser_set_target(AssetBrowserState *ab);

/* Parses a PKT_ASSET_LIST_REPLY payload (opcode byte already stripped by
 * net.c) into the registered AssetBrowserState. Silently truncates to
 * ASSET_BROWSER_MAX items and to each field's *_LEN if the server ever
 * sends more/longer than that -- matches console.c's log_push truncation
 * behavior for the same "don't crash on an oversized message" reason. */
void asset_browser_ingest_list_reply(const uint8_t *data, int len);

/* Called on PKT_ASSET_CHANGED -- just requests a refresh next frame
 * rather than trying to patch the cache in place, so the cache always
 * reflects one real server reply, never a locally-guessed merge. */
void asset_browser_mark_dirty(void);

/* Consumes InputState's typed_chars/backspace_edge/enter_edge into
 * whichever of search/edit_name/edit_tags ab->focus currently points at
 * -- a true no-op, including NOT draining InputState, when focus is
 * AB_FOCUS_NONE, so main.c's caller can fall back to feeding the same
 * frame's input to pyconsole_update instead (see main.c's main_loop).
 * Enter's behavior depends on which field: in search it sets
 * refresh_requested; in edit_name it just moves focus to edit_tags (like
 * Tab); in edit_tags it submits (sets update_requested or
 * create_requested, whichever editing_id implies). */
void asset_browser_update_focused_text(AssetBrowserState *ab, InputState *inp);

/* Starts renaming/retagging an existing item (ui.c calls this from a
 * click on a row's Rename button) -- copies its current name/tags into
 * edit_name/edit_tags, sets editing_id to its id, and focuses edit_name.
 * `index` is bounds-checked against ab->count; out-of-range is a no-op. */
void asset_browser_begin_rename(AssetBrowserState *ab, int index);

/* Starts a pending create (ui.c calls this from the Scene context menu's
 * "Save as Asset" row, via main.c) -- sets editing_id to AB_EDITING_NEW,
 * defaults edit_name to `default_name`, clears edit_tags, and focuses
 * edit_name. The actual upload doesn't happen until create_requested is
 * set (submitting edit_tags) and main.c acts on it. */
void asset_browser_begin_create(AssetBrowserState *ab, const char *default_name);

/* Cancels whatever asset_browser_begin_rename/begin_create started --
 * editing_id back to AB_EDITING_NONE, focus back to AB_FOCUS_NONE. Safe
 * to call when nothing was being edited (no-op). */
void asset_browser_cancel_edit(AssetBrowserState *ab);
