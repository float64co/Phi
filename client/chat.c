#include "chat.h"
#include <string.h>
#include <stdio.h>

static ChatState *s_cs = NULL;

static void log_push(ChatState *cs, const char *line) {
    if (cs->log_count < CHAT_LOG_LINES) {
        strncpy(cs->log[cs->log_count], line, CHAT_LINE_LEN - 1);
        cs->log[cs->log_count][CHAT_LINE_LEN - 1] = 0;
        cs->log_count++;
    } else {
        for (int i = 1; i < CHAT_LOG_LINES; i++)
            memcpy(cs->log[i-1], cs->log[i], CHAT_LINE_LEN);
        strncpy(cs->log[CHAT_LOG_LINES - 1], line, CHAT_LINE_LEN - 1);
        cs->log[CHAT_LOG_LINES - 1][CHAT_LINE_LEN - 1] = 0;
    }
}

void chat_append_multiline(const char *text) {
    if (!s_cs || !text || !text[0]) return;
    const char *start = text;
    const char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        char line[CHAT_LINE_LEN];
        size_t len = (size_t)(nl - start);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, start, len);
        line[len] = 0;
        log_push(s_cs, line);
        start = nl + 1;
    }
    if (*start) log_push(s_cs, start);   /* trailing partial line, no final newline */
}

void chat_on_reply(const char *text) {
    if (!s_cs) return;
    chat_append_multiline(text);
    s_cs->waiting_for_reply = 0;
}

void chat_init(ChatState *cs) {
    memset(cs, 0, sizeof(*cs));
    s_cs = cs;
    log_push(cs, "Click below to talk to Claude -- a real Anthropic tool-use loop");
    log_push(cs, "running server-side (see phi.md's \"Where AI fits\"). It can call");
    log_push(cs, "back into this running instance (get_scene_state) to answer.");
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
