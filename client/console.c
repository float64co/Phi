#include "console.h"
#include "mp_port.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static ConsoleState *s_cs = NULL;

static void log_push(ConsoleState *cs, const char *line) {
    if (cs->log_count < CONSOLE_LOG_LINES) {
        strncpy(cs->log[cs->log_count], line, CONSOLE_LINE_LEN - 1);
        cs->log[cs->log_count][CONSOLE_LINE_LEN - 1] = 0;
        cs->log_count++;
    } else {
        for (int i = 1; i < CONSOLE_LOG_LINES; i++)
            memcpy(cs->log[i-1], cs->log[i], CONSOLE_LINE_LEN);
        strncpy(cs->log[CONSOLE_LOG_LINES - 1], line, CONSOLE_LINE_LEN - 1);
        cs->log[CONSOLE_LOG_LINES - 1][CONSOLE_LINE_LEN - 1] = 0;
    }
}

/* Splits Python output (real print() output, potentially several lines,
 * or a multi-line exception traceback -- both come back from phi_mp_exec
 * as one string, see mp_port.h) on '\n' and log_push()es each line
 * separately, so the scrollback shows real line breaks instead of one
 * giant run-on entry. Any single line longer than CONSOLE_LINE_LEN gets
 * truncated by log_push itself, same as every other console message. */
static void log_push_multiline(ConsoleState *cs, const char *text) {
    if (!text || !text[0]) return;
    const char *start = text;
    const char *nl;
    while ((nl = strchr(start, '\n')) != NULL) {
        char line[CONSOLE_LINE_LEN];
        size_t len = (size_t)(nl - start);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, start, len);
        line[len] = 0;
        log_push(cs, line);
        start = nl + 1;
    }
    if (*start) log_push(cs, start);   /* trailing partial line, no final newline */
}

static void history_push(ConsoleState *cs, const char *line) {
    if (line[0] == 0) return;
    if (cs->history_count < CONSOLE_HISTORY) {
        strncpy(cs->history[cs->history_count], line, CONSOLE_INPUT_LEN - 1);
        cs->history[cs->history_count][CONSOLE_INPUT_LEN - 1] = 0;
        cs->history_count++;
    } else {
        for (int i = 1; i < CONSOLE_HISTORY; i++)
            memcpy(cs->history[i-1], cs->history[i], CONSOLE_INPUT_LEN);
        strncpy(cs->history[CONSOLE_HISTORY - 1], line, CONSOLE_INPUT_LEN - 1);
        cs->history[CONSOLE_HISTORY - 1][CONSOLE_INPUT_LEN - 1] = 0;
    }
}

void console_init(ConsoleState *cs) {
    memset(cs, 0, sizeof(*cs));
    cs->history_pos = -1;
    s_cs = cs;
    log_push(cs, "Phi Python console. Type Python; it runs through a real embedded");
    log_push(cs, "interpreter (no special sandboxing beyond MicroPython's own build).");
}

void console_append(const char *line) {
    if (s_cs) log_push(s_cs, line);
}

/* Runs typed input through the embedded interpreter and shows whatever it
 * printed (or its traceback, if it raised) in the scrollback -- see
 * mp_port.h's phi_mp_exec. This console has no command layer of its own
 * anymore (see phi.md's note on why): it's a real Python REPL, full stop. */
static void console_submit(ConsoleState *cs) {
    char line[CONSOLE_INPUT_LEN];
    strncpy(line, cs->input, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;

    char echo[CONSOLE_LINE_LEN];
    snprintf(echo, sizeof(echo), ">>> %s", line);
    log_push(cs, echo);
    history_push(cs, line);
    cs->history_pos = -1;
    cs->input[0] = 0;
    cs->input_len = 0;

    char *output = phi_mp_exec(line);
    log_push_multiline(cs, output);
    free(output);
}

void console_update(ConsoleState *cs, InputState *inp) {
    /* Consume-on-read: always clear these edges so an unopened/just-closed
     * console never leaves them stuck for next frame. */
    int toggle = inp->console_toggle; inp->console_toggle = 0;
    int escape = inp->escape_edge;    inp->escape_edge    = 0;
    int enter  = inp->enter_edge;     inp->enter_edge      = 0;
    int backsp = inp->backspace_edge; inp->backspace_edge  = 0;
    int hup    = inp->histup_edge;    inp->histup_edge     = 0;
    int hdown  = inp->histdown_edge;  inp->histdown_edge   = 0;
    char chars[TYPED_CHAR_QUEUE_SIZE];
    int nchars = inp->typed_count;
    memcpy(chars, inp->typed_chars, (size_t)nchars);
    inp->typed_count = 0;

    if (toggle) {
        cs->open = !cs->open;
        input_set_console_open(inp, cs->open);
        cs->history_pos = -1;
    }

    /* Escape is browser-reserved to exit pointer lock and that can't be
     * blocked — Chrome in particular won't even deliver the Escape keydown
     * to us when it's what triggered the unlock, so our own escape_edge
     * handling below can't be relied on to close the console in that case.
     * Watching pointer_locked directly catches it regardless of whether we
     * ever see the keydown.
     *
     * Only fires on the *transition* from locked to unlocked (tracked via
     * s_was_pointer_locked below), not "currently unlocked" as a standing
     * condition — pointer lock now defaults off and has no click-to-engage
     * gesture on any platform (see this session's "stop stealing the
     * mouse" fix), so treating "not locked" as reason enough to force-close
     * meant the console closed itself one frame after every open and could
     * never actually be used. The original scenario this guards against
     * (Escape drops lock, console should close too) is still handled: that
     * IS a locked->unlocked transition. */
    static int s_was_pointer_locked = 0;
    if (cs->open && s_was_pointer_locked && !inp->pointer_locked) {
        cs->open = 0;
        input_set_console_open(inp, 0);
    }
    s_was_pointer_locked = inp->pointer_locked;

    if (!cs->open) return;

    if (escape) {
        cs->open = 0;
        input_set_console_open(inp, 0);
        return;
    }

    for (int i = 0; i < nchars; i++) {
        char c = chars[i];
        if (c == '`') continue;   /* the key that opened the console */
        if (cs->input_len < CONSOLE_INPUT_LEN - 1) {
            cs->input[cs->input_len++] = c;
            cs->input[cs->input_len]   = 0;
        }
    }
    if (backsp && cs->input_len > 0) {
        cs->input[--cs->input_len] = 0;
    }
    if (hup && cs->history_count > 0) {
        if (cs->history_pos < cs->history_count - 1) cs->history_pos++;
        int idx = cs->history_count - 1 - cs->history_pos;
        strncpy(cs->input, cs->history[idx], CONSOLE_INPUT_LEN - 1);
        cs->input[CONSOLE_INPUT_LEN - 1] = 0;
        cs->input_len = (int)strlen(cs->input);
    }
    if (hdown) {
        if (cs->history_pos > 0) {
            cs->history_pos--;
            int idx = cs->history_count - 1 - cs->history_pos;
            strncpy(cs->input, cs->history[idx], CONSOLE_INPUT_LEN - 1);
            cs->input[CONSOLE_INPUT_LEN - 1] = 0;
            cs->input_len = (int)strlen(cs->input);
        } else {
            cs->history_pos = -1;
            cs->input[0] = 0;
            cs->input_len = 0;
        }
    }
    if (enter) {
        console_submit(cs);
    }
}
