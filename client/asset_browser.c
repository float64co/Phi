#include "asset_browser.h"
#include <string.h>
#include <stdio.h>

static AssetBrowserState *s_ab = NULL;

void asset_browser_init(AssetBrowserState *ab) {
    memset(ab, 0, sizeof(*ab));
    ab->selected = -1;
}

void asset_browser_set_target(AssetBrowserState *ab) {
    s_ab = ab;
}

/* Reads a [len:u8 bytes] field, NUL-terminating into `out` (capacity
 * `out_cap`, truncated if the field is longer). Returns the new read
 * cursor, or NULL if the length prefix itself or its bytes run past
 * `end` -- caller treats NULL as "malformed packet, stop parsing". */
static const uint8_t *read_lenprefixed(const uint8_t *p, const uint8_t *end,
                                        char *out, int out_cap) {
    if (p >= end) return NULL;
    uint8_t flen = *p++;
    if (p + flen > end) return NULL;
    int n = flen < out_cap - 1 ? flen : out_cap - 1;
    memcpy(out, p, (size_t)n);
    out[n] = 0;
    return p + flen;
}

void asset_browser_ingest_list_reply(const uint8_t *data, int len) {
    if (!s_ab) return;
    if (len < 2) { s_ab->count = 0; return; }

    const uint8_t *p = data;
    const uint8_t *end = data + len;
    uint16_t count; memcpy(&count, p, 2); p += 2;

    int n = 0;
    for (uint16_t i = 0; i < count && n < ASSET_BROWSER_MAX; i++) {
        if (p + 4 > end) break;
        AssetSummary *a = &s_ab->items[n];
        memcpy(&a->id, p, 4); p += 4;
        p = read_lenprefixed(p, end, a->name, ASSET_NAME_LEN);
        if (!p) break;
        p = read_lenprefixed(p, end, a->path, ASSET_PATH_LEN);
        if (!p) break;
        p = read_lenprefixed(p, end, a->tags, ASSET_TAGS_LEN);
        if (!p) break;
        n++;
    }
    s_ab->count = n;
    if (s_ab->selected >= n) s_ab->selected = -1;
    printf("[asset_browser] list reply: %d asset(s)\n", n);
}

void asset_browser_mark_dirty(void) {
    if (s_ab) s_ab->refresh_requested = 1;
}
