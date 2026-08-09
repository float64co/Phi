#include "asset_browser.h"
#include <string.h>
#include <stdio.h>

static AssetBrowserState *s_ab = NULL;

void asset_browser_init(AssetBrowserState *ab) {
    memset(ab, 0, sizeof(*ab));
    ab->selected = -1;
    ab->editing_id = AB_EDITING_NONE;
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

/* Feeds InputState's typed_chars/backspace into (buf, *len_ptr) up to
 * cap-1, draining InputState the same way every text field here always
 * has. No history concept for any of these fields -- histup/histdown are
 * dropped rather than left to leak into whichever field is focused next
 * frame. Returns whether Enter was pressed this frame; callers decide
 * what that means per field. */
static int consume_text_input(char *buf, int *len_ptr, int cap, InputState *inp) {
    int enter  = inp->enter_edge;     inp->enter_edge     = 0;
    int backsp = inp->backspace_edge; inp->backspace_edge = 0;
    inp->histup_edge = inp->histdown_edge = 0;

    char chars[TYPED_CHAR_QUEUE_SIZE];
    int nchars = inp->typed_count;
    memcpy(chars, inp->typed_chars, (size_t)nchars);
    inp->typed_count = 0;

    for (int i = 0; i < nchars; i++) {
        char c = chars[i];
        if (*len_ptr < cap - 1) {
            buf[(*len_ptr)++] = c;
            buf[*len_ptr] = 0;
        }
    }
    if (backsp && *len_ptr > 0) {
        buf[--(*len_ptr)] = 0;
    }
    return enter;
}

void asset_browser_update_focused_text(AssetBrowserState *ab, InputState *inp) {
    switch (ab->focus) {
    case AB_FOCUS_SEARCH: {
        int enter = consume_text_input(ab->search, &ab->search_len, ASSET_SEARCH_LEN, inp);
        if (enter) ab->refresh_requested = 1;   /* same as clicking Refresh, using whatever's currently typed */
        break;
    }
    case AB_FOCUS_EDIT_NAME: {
        int enter = consume_text_input(ab->edit_name, &ab->edit_name_len, ASSET_NAME_LEN, inp);
        if (enter) ab->focus = AB_FOCUS_EDIT_TAGS;   /* Tab-like: Enter in Name moves to Tags, doesn't submit yet */
        break;
    }
    case AB_FOCUS_EDIT_TAGS: {
        int enter = consume_text_input(ab->edit_tags, &ab->edit_tags_len, ASSET_TAGS_LEN, inp);
        if (enter) {
            if (ab->editing_id == AB_EDITING_NEW) ab->create_requested = 1;
            else if (ab->editing_id >= 0) ab->update_requested = 1;
        }
        break;
    }
    case AB_FOCUS_NONE:
    default:
        return;   /* no-op -- leave InputState untouched so main.c falls back to pyconsole_update this frame */
    }
}

void asset_browser_begin_rename(AssetBrowserState *ab, int index) {
    if (index < 0 || index >= ab->count) return;
    const AssetSummary *a = &ab->items[index];
    ab->editing_id = (int32_t)a->id;
    strncpy(ab->edit_name, a->name, ASSET_NAME_LEN - 1);
    ab->edit_name[ASSET_NAME_LEN - 1] = 0;
    ab->edit_name_len = (int)strlen(ab->edit_name);
    strncpy(ab->edit_tags, a->tags, ASSET_TAGS_LEN - 1);
    ab->edit_tags[ASSET_TAGS_LEN - 1] = 0;
    ab->edit_tags_len = (int)strlen(ab->edit_tags);
    ab->focus = AB_FOCUS_EDIT_NAME;
}

void asset_browser_begin_create(AssetBrowserState *ab, const char *default_name) {
    ab->editing_id = AB_EDITING_NEW;
    strncpy(ab->edit_name, default_name, ASSET_NAME_LEN - 1);
    ab->edit_name[ASSET_NAME_LEN - 1] = 0;
    ab->edit_name_len = (int)strlen(ab->edit_name);
    ab->edit_tags[0] = 0;
    ab->edit_tags_len = 0;
    ab->focus = AB_FOCUS_EDIT_NAME;
}

void asset_browser_cancel_edit(AssetBrowserState *ab) {
    ab->editing_id = AB_EDITING_NONE;
    ab->focus = AB_FOCUS_NONE;
}
