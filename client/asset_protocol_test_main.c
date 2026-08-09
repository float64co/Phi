/* Standalone, no-GL/no-X11 test harness for the asset CRUD wire protocol's
 * CLIENT side (net.c/asset_browser.c) against a REAL, already-running
 * server.py -- server/test_asset_protocol.py already validates the
 * server's own protocol correctness with an independent Python WS client,
 * but never touches the actual C encoder/decoder main.c relies on. This
 * harness does: it's the real net_send_asset_update/delete C code being
 * exercised, and the real asset_browser_ingest_list_reply C parser
 * decoding the server's real reply bytes -- not a reimplementation.
 *
 * Same "prove the subsystem works in isolation, no GL/window dependency"
 * precedent as mesh_edit_test_main.c/fracture_test_main.c/
 * mp_console_test_main.c, applied to net.c instead. Native-only (uses
 * ws_client_native.c/net_poll_native, and POSIX usleep) -- there's no
 * wasm/win32 equivalent of "run a standalone binary against a live
 * server" the way this environment can for native.
 *
 * Usage: build/asset_protocol_test [ws://host:port/ws]
 * Expects the 3 test assets (see tools/gen_test_assets.py) already
 * uploaded via HTTP POST /assets -- this harness lists/loads/updates/
 * deletes whatever's actually indexed, it doesn't seed the DB itself. */
#include "net.h"
#include "asset_browser.h"
#include "halfedge_gltf.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* net.c's PKT_CONSOLE_MSG handler calls this unconditionally; linking the
 * real console.c would pull in the whole MicroPython embed tree for
 * nothing this harness needs (mp_console_test_main.c already covers that
 * path in isolation) -- a local stub is lighter and just as correct here. */
void pyconsole_append(const char *line) {
    printf("[asset_protocol_test] (console message, ignored by this harness): %s\n", line);
}

static int g_fail = 0;
static void check(int cond, const char *msg) {
    printf("  %s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) g_fail = 1;
}

static NetState          g_ns = {0};
static AssetBrowserState g_ab;

static void pump(double seconds) {
    int iters = (int)(seconds * 100.0);
    for (int i = 0; i < iters; i++) {
        net_poll_native();
        usleep(10000);
    }
}

/* Mirrors main.c's own "if refresh_requested, send the request and clear
 * the flag" polling -- this harness has no main_loop, so it has to do
 * that one step itself to react to a PKT_ASSET_CHANGED broadcast. */
static void service_refresh(double seconds) {
    pump(seconds);
    if (g_ab.refresh_requested) {
        g_ab.refresh_requested = 0;
        net_send_asset_list_request(&g_ns, NULL);
        pump(seconds);
    }
}

int main(int argc, char **argv) {
    const char *url = argc > 1 ? argv[1] : "ws://localhost:8765/ws";
    asset_browser_init(&g_ab);
    asset_browser_set_target(&g_ab);

    printf("[asset_protocol_test] connecting to %s ...\n", url);
    net_connect(&g_ns, url);
    check(g_ns.connected, "net_connect succeeded");
    if (!g_ns.connected) { printf("\n[asset_protocol_test] RESULT: FAIL\n"); return 1; }

    printf("[asset_protocol_test] === 1: initial list arrives with no explicit request "
           "(net.c auto-requests it right after HELLO) ===\n");
    pump(1.0);
    for (int i = 0; i < g_ab.count; i++) {
        printf("    #%u  %-20s path=%-40s tags=%s\n",
               g_ab.items[i].id, g_ab.items[i].name, g_ab.items[i].path, g_ab.items[i].tags);
    }
    check(g_ab.count >= 3, "at least 3 assets indexed (expects pyramid/spheroid/cuboid already uploaded via HTTP)");
    if (g_ab.count < 3) {
        printf("\n[asset_protocol_test] RESULT: FAIL (seed the 3 test assets via HTTP POST first)\n");
        return 1;
    }

    printf("[asset_protocol_test] === 1b: a real C-encoded LIST_REQUEST with a query string actually filters (Asset Browser search bar's own request path) ===\n");
    int full_count = g_ab.count;
    char full_first_name[ASSET_NAME_LEN];
    strncpy(full_first_name, g_ab.items[0].name, sizeof(full_first_name) - 1);
    full_first_name[sizeof(full_first_name) - 1] = 0;
    net_send_asset_list_request(&g_ns, full_first_name);
    pump(1.0);
    int all_match = g_ab.count > 0;
    for (int i = 0; i < g_ab.count; i++) {
        if (strstr(g_ab.items[i].name, full_first_name) == NULL &&
            strstr(g_ab.items[i].tags, full_first_name) == NULL) {
            all_match = 0;
        }
    }
    check(g_ab.count > 0 && g_ab.count <= full_count, "query for a real asset's own name returns a non-empty, no-larger result set");
    check(all_match, "every result actually matches the query string (server-side LIKE against name/tag, not an unfiltered dump)");
    net_send_asset_list_request(&g_ns, "definitely_not_a_real_asset_name_xyz");
    pump(1.0);
    check(g_ab.count == 0, "a query matching nothing returns an empty list, not a stale/unfiltered one");
    net_send_asset_list_request(&g_ns, NULL);   /* restore the full list for the rest of this test */
    pump(1.0);
    check(g_ab.count == full_count, "NULL query (Refresh with an empty search bar) restores the full list");

    printf("[asset_protocol_test] === 2: each listed asset's path really loads (real cgltf pipeline, no GL) ===\n");
    int all_loaded = 1;
    for (int i = 0; i < g_ab.count; i++) {
        HalfEdgeMesh *hem = halfedge_load_gltf(g_ab.items[i].path);
        if (hem) {
            printf("    OK: %s (%s) -> %d verts, %d faces\n",
                   g_ab.items[i].name, g_ab.items[i].path, hem->vert_count, hem->face_count);
            halfedge_destroy(hem);
        } else {
            printf("    FAILED to load %s at %s\n", g_ab.items[i].name, g_ab.items[i].path);
            all_loaded = 0;
        }
    }
    check(all_loaded, "every listed asset's path loads via halfedge_load_gltf");

    printf("[asset_protocol_test] === 3: real C-encoded UPDATE round-trips through the live server ===\n");
    uint32_t target_id = g_ab.items[0].id;
    char new_name[ASSET_NAME_LEN];
    snprintf(new_name, sizeof(new_name), "%.40s_ctest", g_ab.items[0].name);
    net_send_asset_update(&g_ns, target_id, new_name, "ctest,updated");
    service_refresh(1.0);
    int found_updated = 0;
    for (int i = 0; i < g_ab.count; i++) {
        if (g_ab.items[i].id == target_id && strcmp(g_ab.items[i].name, new_name) == 0 &&
            strcmp(g_ab.items[i].tags, "ctest,updated") == 0) {
            found_updated = 1;
        }
    }
    check(found_updated, "server applied the C-encoded UPDATE; re-list (triggered by PKT_ASSET_CHANGED) reflects it");

    printf("[asset_protocol_test] === 4: real C-encoded DELETE round-trips through the live server ===\n");
    int count_before = g_ab.count;
    net_send_asset_delete(&g_ns, target_id);
    service_refresh(1.0);
    int still_present = 0;
    for (int i = 0; i < g_ab.count; i++) if (g_ab.items[i].id == target_id) still_present = 1;
    check(!still_present, "deleted asset no longer appears in the re-listed result");
    check(g_ab.count == count_before - 1, "count dropped by exactly one");

    if (g_fail) printf("\n[asset_protocol_test] RESULT: FAIL\n");
    else printf("\n[asset_protocol_test] RESULT: PASS (all checks passed)\n");
    return g_fail;
}
