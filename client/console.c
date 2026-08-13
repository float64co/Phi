#include "console.h"
#include "mp_port.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static PyConsoleState *s_cs = NULL;

static void log_push(PyConsoleState *cs, const char *line) {
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
static void log_push_multiline(PyConsoleState *cs, const char *text) {
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

static void history_push(PyConsoleState *cs, const char *line) {
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

void pyconsole_init(PyConsoleState *cs) {
    memset(cs, 0, sizeof(*cs));
    cs->history_pos = -1;
    s_cs = cs;
    log_push(cs, "Phi Python console -- a real embedded interpreter (no special");
    log_push(cs, "sandboxing beyond MicroPython's own build).");
}

void pyconsole_append(const char *line) {
    if (s_cs) log_push(s_cs, line);
}

/* Runs typed input through the embedded interpreter and shows whatever it
 * printed (or its traceback, if it raised) in the scrollback -- see
 * mp_port.h's phi_mp_exec. This console has no command layer of its own
 * anymore (see phi.md's note on why): it's a real Python REPL, full stop. */
static void pyconsole_submit(PyConsoleState *cs) {
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

void pyconsole_update(PyConsoleState *cs, InputState *inp) {
    /* The Python panel is an always-focused text input -- there is no
     * open/close focus toggle (the old backquote-toggled modal drop-down
     * was Qek's console paradigm, deliberately not carried forward).
     * Keyboard input flows here every frame, apart from the few reserved
     * shortcuts input.c keeps as function keys. Consume-on-read: clear
     * every edge as it's read so nothing sticks across frames. */
    int enter  = inp->enter_edge;     inp->enter_edge      = 0;
    int backsp = inp->backspace_edge; inp->backspace_edge  = 0;
    int hup    = inp->histup_edge;    inp->histup_edge     = 0;
    int hdown  = inp->histdown_edge;  inp->histdown_edge   = 0;
    char chars[TYPED_CHAR_QUEUE_SIZE];
    int nchars = inp->typed_count;
    memcpy(chars, inp->typed_chars, (size_t)nchars);
    inp->typed_count = 0;

    for (int i = 0; i < nchars; i++) {
        char c = chars[i];
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
        pyconsole_submit(cs);
    }
}
