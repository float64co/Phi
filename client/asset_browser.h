#pragma once
#include <stdint.h>
#include "input.h"

/* Client-side cache + intent-queue for the Asset Browser panel (see
 * phi.md's "Asset tracking and the Asset Browser panel" / "Wire protocol:
 * CRUD over a hybrid HTTP + WS split"). Owns exactly two things:
 *
 *   1. The last list of assets the server sent us (a cache of the
 *      server's SQLite index, never the source of truth itself).
 *   2. One-shot request flags that ui.c sets when the user clicks a
 *      button, and that main.c polls once per frame and turns into real
 *      net_send_asset_*() calls — the same "UI raises intent, main.c
 *      executes it" shape ui_poll_context_menu_action() already
 *      established, rather than ui.c reaching into net.c directly (it
 *      doesn't include net.h today and this doesn't change that).
 *
 * Parsing PKT_ASSET_LIST_REPLY's bytes lives here too (not net.c), same
 * division of labor console.c/net.c already have for PKT_CONSOLE_MSG:
 * net.c strips the opcode byte and hands the rest off. */

#define ASSET_BROWSER_MAX  64
#define ASSET_NAME_LEN     64
#define ASSET_PATH_LEN     128
#define ASSET_TAGS_LEN     96
#define ASSET_SEARCH_LEN   64

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
     * network round trip per keystroke isn't worth it, and nothing else
     * in this codebase's text entry (console.c's own input line) live-
     * submits either. Empty string = no filter, same as passing NULL to
     * net_send_asset_list_request. */
    char search[ASSET_SEARCH_LEN];
    int  search_len;
    /* Whether the search bar currently owns keyboard input instead of the
     * Python console -- this is the ONE piece of text-field focus this
     * codebase has, since console.c was deliberately built as "the only
     * text field, always focused, no toggle" before this. Set on a click
     * inside the search bar rect, cleared by clicking anywhere else (see
     * ui.c's ui_on_mouse_button, same click-away-dismisses convention the
     * type-switcher dropdown/context menu already use) or by switching
     * the panel away from PANEL_ASSET_BROWSER entirely. */
    int  search_focused;

    /* One-shot request flags, drained by main.c each frame (see main.c's
     * asset browser poll block). Cleared by main.c after acting on them,
     * not by asset_browser.c itself -- mirrors InputState's lmb_click/etc.
     * "producer sets, consumer clears" convention. */
    int      refresh_requested;
    int      load_requested;
    uint32_t load_requested_id;
    int      delete_requested;
    uint32_t delete_requested_id;
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
 * ab->search IF (and only if) ab->search_focused -- a no-op, including
 * NOT draining InputState, when unfocused, so main.c's caller can fall
 * back to feeding the same frame's input to pyconsole_update instead (see
 * main.c's main_loop). Enter sets ab->refresh_requested rather than
 * sending anything itself -- same "raise intent, main.c executes it"
 * shape every other request flag here already uses. */
void asset_browser_update_search(AssetBrowserState *ab, InputState *inp);
