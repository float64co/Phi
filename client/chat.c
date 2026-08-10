#include "chat.h"
#include <string.h>
#include <stdio.h>

static ChatState *s_cs = NULL;

static void log_push(ChatState *cs, const char *line, int is_msg_start) {
    ChatLogLine *slot;
    if (cs->log_count < CHAT_LOG_LINES) {
        slot = &cs->log[cs->log_count++];
    } else {
        for (int i = 1; i < CHAT_LOG_LINES; i++) cs->log[i-1] = cs->log[i];
        slot = &cs->log[CHAT_LOG_LINES - 1];
    }
    strncpy(slot->text, line, CHAT_LINE_LEN - 1);
    slot->text[CHAT_LINE_LEN - 1] = 0;
    slot->is_msg_start = is_msg_start;
}

/* Shared by chat_append_multiline/chat_append_system_text -- splits on
 * real '\n' only (word-wrap is a separate, later, draw-time concern, see
 * ChatLogLine's own comment); `mark_first` controls whether the very
 * first resulting line gets is_msg_start = 1. */
static void append_lines(const char *text, int mark_first) {
    if (!s_cs || !text || !text[0]) return;
    const char *start = text;
    const char *nl;
    int first = 1;
    while ((nl = strchr(start, '\n')) != NULL) {
        char line[CHAT_LINE_LEN];
        size_t len = (size_t)(nl - start);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, start, len);
        line[len] = 0;
        log_push(s_cs, line, first && mark_first);
        first = 0;
        start = nl + 1;
    }
    if (*start) log_push(s_cs, start, first && mark_first);   /* trailing partial line, no final newline */
}

void chat_append_multiline(const char *text) {
    append_lines(text, 1);
}

void chat_append_system_text(const char *text) {
    append_lines(text, 0);
}

void chat_on_reply(const char *text) {
    if (!s_cs) return;
    chat_append_multiline(text);
    s_cs->waiting_for_reply = 0;
}

void chat_init(ChatState *cs) {
    memset(cs, 0, sizeof(*cs));
    s_cs = cs;
    /* One logical paragraph -- draw-time word-wrap breaks it into real
     * screen-width rows (see draw_panel_chat), so this isn't manually
     * pre-broken the way the old three-fixed-line version was. Explains
     * the "@llm" addressing convention (server.py only invokes the model
     * when a message contains it, and tells the model the same thing in
     * its own system prompt -- see server/server.py's _handle_chat) so
     * the human reading this panel knows the syntax too, not just the
     * model. Two blank lines after it (not one) per an explicit request
     * to visually separate this preamble from the user's own first
     * message below it. */
    chat_append_system_text(
        "Mention @llm anywhere in your message to talk to Claude -- a real "
        "Anthropic tool-use loop running server-side (see phi.md's \"Where "
        "AI fits\"). It can call back into this running instance "
        "(get_scene_state) to answer questions about what's happening in "
        "the scene right now.");
    log_push(cs, "", 0);
    log_push(cs, "", 0);
}

/* Same drain-and-clear shape asset_browser.c's consume_text_input uses --
 * see that file's own comment for why histup/histdown are dropped rather
 * than left to leak into whichever field is focused next frame. No
 * history browsing for chat input (unlike the Python console) -- a known,
 * deliberate simplification, not an oversight. */
void chat_update_focused_text(ChatState *cs, InputState *inp) {
    if (cs->focus != CHAT_FOCUS_INPUT) return;

    int enter  = inp->enter_edge;     inp->enter_edge     = 0;
    int backsp = inp->backspace_edge; inp->backspace_edge = 0;
    inp->histup_edge = inp->histdown_edge = 0;

    char chars[TYPED_CHAR_QUEUE_SIZE];
    int nchars = inp->typed_count;
    memcpy(chars, inp->typed_chars, (size_t)nchars);
    inp->typed_count = 0;

    for (int i = 0; i < nchars; i++) {
        char c = chars[i];
        if (cs->input_len < CHAT_INPUT_LEN - 1) {
            cs->input[cs->input_len++] = c;
            cs->input[cs->input_len]   = 0;
        }
    }
    if (backsp && cs->input_len > 0) {
        cs->input[--cs->input_len] = 0;
    }
    if (enter && cs->input_len > 0 && !cs->waiting_for_reply) {
        strncpy(cs->pending_send, cs->input, CHAT_INPUT_LEN - 1);
        cs->pending_send[CHAT_INPUT_LEN - 1] = 0;
        cs->send_requested = 1;
        cs->input[0] = 0;
        cs->input_len = 0;
    }
}
