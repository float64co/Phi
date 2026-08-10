#pragma once
#include "input.h"

/* Chat panel -- a real text field talking to a server-side Anthropic
 * tool-use loop (see phi.md's "Where AI fits" / server/anthropic_client.py),
 * NOT the always-focused-no-toggle console.c pattern: a chat box is
 * something you click into to type, the same way a real chat app works,
 * so it needs its own explicit focus state (ChatFocus) rather than eating
 * every keystroke by default. Click-to-focus / click-away-blur is handled
 * in ui.c, mirroring AssetBrowserFocus's convention exactly (see
 * asset_browser.h's own comment on why that pattern exists) -- this makes
 * THREE candidate text-input owners now competing for the keyboard each
 * frame (Asset Browser fields, Chat input, Python Console), arbitrated by
 * main.c's single per-frame if/else-if/else chain. */

#define CHAT_LOG_LINES   40
#define CHAT_LINE_LEN    200
#define CHAT_INPUT_LEN   240

typedef enum {
    CHAT_FOCUS_NONE = 0,
    CHAT_FOCUS_INPUT,
} ChatFocus;

typedef struct {
    char input[CHAT_INPUT_LEN];
    int  input_len;

    char log[CHAT_LOG_LINES][CHAT_LINE_LEN];
    int  log_count;

    ChatFocus focus;

    /* Set by chat_update_focused_text on Enter; main.c drains this once
     * per frame into net_send_chat_msg and clears it -- the same "UI
     * raises intent, main.c executes" shape asset_browser.h's
     * create_requested/update_requested/etc already establish, so chat.c
     * has no reason to touch net.c directly. */
    int  send_requested;
    char pending_send[CHAT_INPUT_LEN];

    /* True from the moment send_requested is drained until a real
     * PKT_CHAT_REPLY (chat_on_reply) arrives -- draw_panel_chat shows a
     * "thinking..." row while this is set, since a real tool-use round
     * trip to Anthropic (which can itself round-trip back to THIS same
     * client for a get_scene_state tool call before it's done) is not
     * instant the way a local UI action is. */
    int  waiting_for_reply;
} ChatState;

/* Zeroes cs, registers it as the target for chat_append_multiline()/
 * chat_on_reply() (mirrors console.c's single-instance s_cs pattern --
 * there's only ever one Chat panel), and seeds one welcome log line. */
void chat_init(ChatState *cs);

/* Consumes InputState's typed_chars/backspace_edge/enter_edge into the
 * input buffer -- a true no-op, including NOT draining InputState, when
 * cs->focus is CHAT_FOCUS_NONE, so main.c's caller can fall back to
 * feeding the same frame's input to whatever's next in the chain (same
 * contract as asset_browser_update_focused_text). Enter sets
 * send_requested + pending_send and clears the input box, but leaves
 * focus alone -- unlike the Asset Browser's transient edit fields, a chat
 * box stays focused after you hit Enter, so you can keep typing. */
void chat_update_focused_text(ChatState *cs, InputState *inp);

/* Splits `text` on '\n' and pushes each line into the registered
 * ChatState's scrollback ring buffer, same truncation/wrap behavior as
 * console.c's log_push/log_push_multiline. Used both for the local "You:
 * ..." echo (main.c, right after draining send_requested) and for
 * whatever the assistant said. */
void chat_append_multiline(const char *text);

/* Called by net.c on PKT_CHAT_REPLY: chat_append_multiline(text), then
 * clears waiting_for_reply. */
void chat_on_reply(const char *text);
