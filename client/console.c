#include "console.h"
#include <string.h>
#include <stdio.h>

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

/* Shared by console_init() (so the first time a player opens the console
 * it reads as if they'd already typed 'help') and the 'help' command
 * itself, so the two can never drift out of sync. */
static void print_help(ConsoleState *cs) {
    log_push(cs, "help | pos | tp x y z | grid <2..512> | mat <0..7>");
    log_push(cs, "noclip | god | hp <n> | give | speed <n> | gravity <n>");
    log_push(cs, "fov <n> | sensitivity <n> | name <name> | players | kill | clear");
    log_push(cs, "save [name] | load [name] | newmap | maps | addbot | delbot");
    log_push(cs, "bind <key> <cmd> | unbind <key>");
}

void console_init(ConsoleState *cs) {
    memset(cs, 0, sizeof(*cs));
    cs->history_pos = -1;
    s_cs = cs;
    log_push(cs, "Qek console.");
    print_help(cs);
}

void console_append(const char *line) {
    if (s_cs) log_push(s_cs, line);
}

/* Parses and runs a single command line. Shared by console_submit() (Enter
 * on typed input) and the bind dispatcher in console_update() (a bound key
 * press), so a bind behaves exactly as if its command had been typed. */
static void console_dispatch(ConsoleState *cs, EditorState *ed, NetState *ns,
                             GameState *gs, Renderer *r, InputState *inp,
                             Player *local, const char *line) {
    char cmd[32] = {0}, rest[CONSOLE_INPUT_LEN] = {0}, out[CONSOLE_LINE_LEN];
    int n = sscanf(line, "%31s %95[^\n]", cmd, rest);
    if (n < 1) return;

    if (strcmp(cmd, "help") == 0) {
        print_help(cs);
    } else if (strcmp(cmd, "clear") == 0) {
        cs->log_count = 0;
    } else if (strcmp(cmd, "pos") == 0) {
        snprintf(out, sizeof(out), "pos: %.1f %.1f %.1f", local->pos.x, local->pos.y, local->pos.z);
        log_push(cs, out);
    } else if (strcmp(cmd, "name") == 0) {
        if (rest[0]) {
            strncpy(local->name, rest, 15); local->name[15] = 0;
            net_send_hello(ns, local->name);
            snprintf(out, sizeof(out), "name = %s", local->name);
            log_push(cs, out);
        } else {
            log_push(cs, "usage: name <name>");
        }
    } else if (strcmp(cmd, "players") == 0) {
        if (gs->num_players == 0) {
            log_push(cs, "no players");
        }
        for (int i = 0; i < gs->num_players; i++) {
            Player *p = &gs->players[i];
            snprintf(out, sizeof(out), "#%-3d %-15s hp=%-4d%s%s",
                     p->id, p->name[0] ? p->name : (p->is_bot ? "(bot)" : "(player)"),
                     p->hp, p->is_bot ? " [bot]" : "", p->alive ? "" : " [dead]");
            log_push(cs, out);
        }
    } else if (strcmp(cmd, "kill") == 0) {
        local->alive = 0;
        local->hp    = 0;
        local->respawn_timer = 3.0f;
        log_push(cs, "you died");
    } else if (strcmp(cmd, "tp") == 0) {
        float x, y, z;
        if (sscanf(rest, "%f %f %f", &x, &y, &z) == 3) {
            local->pos = (Vec3f){x, y, z};
            local->vel = (Vec3f){0, 0, 0};
            log_push(cs, "teleported");
        } else {
            log_push(cs, "usage: tp x y z");
        }
    } else if (strcmp(cmd, "grid") == 0) {
        int g;
        if (sscanf(rest, "%d", &g) == 1 && g >= 2 && g <= 512) {
            int pw = 0; while ((1 << pw) < g) pw++;
            ed->grid_pow = pw < 1 ? 1 : (pw > 9 ? 9 : pw);
            snprintf(out, sizeof(out), "grid = %d", 1 << ed->grid_pow);
            log_push(cs, out);
        } else {
            log_push(cs, "usage: grid <2..512>");
        }
    } else if (strcmp(cmd, "mat") == 0) {
        int m;
        if (sscanf(rest, "%d", &m) == 1 && m >= 0) {
            ed->cur_mat = m % 8;
            snprintf(out, sizeof(out), "material = %d", ed->cur_mat);
            log_push(cs, out);
        } else {
            log_push(cs, "usage: mat <0..7>");
        }
    } else if (strcmp(cmd, "noclip") == 0) {
        ed->active = !ed->active;
        log_push(cs, ed->active ? "editor mode on" : "editor mode off");
    } else if (strcmp(cmd, "god") == 0) {
        local->god = !local->god;
        log_push(cs, local->god ? "god mode on" : "god mode off");
    } else if (strcmp(cmd, "hp") == 0) {
        int v;
        if (sscanf(rest, "%d", &v) == 1) {
            local->hp = v < 0 ? 0 : (v > 500 ? 500 : v);
            snprintf(out, sizeof(out), "hp = %d", local->hp);
            log_push(cs, out);
        } else {
            log_push(cs, "usage: hp <0..500>");
        }
    } else if (strcmp(cmd, "give") == 0) {
        local->hp = 100;
        log_push(cs, "hp restored to 100");
    } else if (strcmp(cmd, "speed") == 0) {
        float v;
        if (sscanf(rest, "%f", &v) == 1 && v > 0) {
            g_move_speed = v;
            snprintf(out, sizeof(out), "move speed = %.0f", g_move_speed);
            log_push(cs, out);
        } else {
            log_push(cs, "usage: speed <positive number> (default 280)");
        }
    } else if (strcmp(cmd, "gravity") == 0) {
        float v;
        if (sscanf(rest, "%f", &v) == 1) {
            g_gravity = v;
            snprintf(out, sizeof(out), "gravity = %.0f", g_gravity);
            log_push(cs, out);
        } else {
            log_push(cs, "usage: gravity <number> (default 600, 0 = none)");
        }
    } else if (strcmp(cmd, "fov") == 0) {
        float v;
        if (sscanf(rest, "%f", &v) == 1) {
            renderer_set_fov(r, v);
            snprintf(out, sizeof(out), "fov = %.0f", v < 30.0f ? 30.0f : (v > 150.0f ? 150.0f : v));
            log_push(cs, out);
        } else {
            log_push(cs, "usage: fov <30..150> (default 75)");
        }
    } else if (strcmp(cmd, "sensitivity") == 0) {
        float v;
        if (sscanf(rest, "%f", &v) == 1 && v > 0) {
            inp->sensitivity = v;
            snprintf(out, sizeof(out), "sensitivity = %.4f", v);
            log_push(cs, out);
        } else {
            log_push(cs, "usage: sensitivity <positive number> (default 0.002)");
        }
    } else if (strcmp(cmd, "skybox") == 0) {
        float rr, gg, bb;
        if (sscanf(rest, "%f %f %f", &rr, &gg, &bb) == 3) {
            renderer_set_sky_color(rr, gg, bb);
            log_push(cs, "sky color set");
        } else {
            log_push(cs, "usage: skybox r g b (each 0..1, default 0.3 0.5 0.8)");
        }
    } else if (strcmp(cmd, "save") == 0) {
        const char *name = rest[0] ? rest : "default";
        net_send_save_map(ns, name);
        snprintf(out, sizeof(out), "saving '%s'...", name);
        log_push(cs, out);
    } else if (strcmp(cmd, "load") == 0) {
        const char *name = rest[0] ? rest : "default";
        net_send_load_map(ns, name);
        snprintf(out, sizeof(out), "loading '%s'...", name);
        log_push(cs, out);
    } else if (strcmp(cmd, "newmap") == 0) {
        net_send_new_map(ns);
        log_push(cs, "resetting map to default...");
    } else if (strcmp(cmd, "maps") == 0) {
        net_send_list_maps(ns);
    } else if (strcmp(cmd, "addbot") == 0) {
        log_push(cs, physics_add_bot(gs) ? "bot added" : "bot limit reached (max 8)");
    } else if (strcmp(cmd, "delbot") == 0) {
        log_push(cs, physics_remove_bot(gs) ? "bot removed" : "no bots to remove");
    } else if (strcmp(cmd, "bind") == 0) {
        char key[KEY_CODE_LEN] = {0}, bindcmd[CONSOLE_INPUT_LEN] = {0};
        if (sscanf(rest, "%15s %95[^\n]", key, bindcmd) == 2) {
            int slot = -1;
            for (int i = 0; i < cs->bind_count; i++)
                if (strcmp(cs->bind_keys[i], key) == 0) { slot = i; break; }
            if (slot < 0 && cs->bind_count < CONSOLE_MAX_BINDS) slot = cs->bind_count++;
            if (slot >= 0) {
                strncpy(cs->bind_keys[slot], key, KEY_CODE_LEN - 1);
                cs->bind_keys[slot][KEY_CODE_LEN - 1] = 0;
                strncpy(cs->bind_cmds[slot], bindcmd, CONSOLE_INPUT_LEN - 1);
                cs->bind_cmds[slot][CONSOLE_INPUT_LEN - 1] = 0;
                snprintf(out, sizeof(out), "bound %s -> %s", key, bindcmd);
                log_push(cs, out);
            } else {
                log_push(cs, "bind table full (32 max)");
            }
        } else {
            log_push(cs, "usage: bind <key> <command>  (key = browser code, e.g. KeyG, Digit1, F5)");
        }
    } else if (strcmp(cmd, "unbind") == 0) {
        char key[KEY_CODE_LEN] = {0};
        if (sscanf(rest, "%15s", key) == 1) {
            int found = -1;
            for (int i = 0; i < cs->bind_count; i++)
                if (strcmp(cs->bind_keys[i], key) == 0) { found = i; break; }
            if (found >= 0) {
                for (int i = found; i < cs->bind_count - 1; i++) {
                    memcpy(cs->bind_keys[i], cs->bind_keys[i+1], KEY_CODE_LEN);
                    memcpy(cs->bind_cmds[i], cs->bind_cmds[i+1], CONSOLE_INPUT_LEN);
                }
                cs->bind_count--;
                log_push(cs, "unbound");
            } else {
                log_push(cs, "no such bind");
            }
        } else {
            log_push(cs, "usage: unbind <key>");
        }
    } else {
        snprintf(out, sizeof(out), "unknown command: %s (try 'help')", cmd);
        log_push(cs, out);
    }
}

static void console_submit(ConsoleState *cs, EditorState *ed, NetState *ns,
                           GameState *gs, Renderer *r, InputState *inp, Player *local) {
    char line[CONSOLE_INPUT_LEN];
    strncpy(line, cs->input, sizeof(line) - 1);
    line[sizeof(line) - 1] = 0;

    char echo[CONSOLE_LINE_LEN];
    snprintf(echo, sizeof(echo), "> %s", line);
    log_push(cs, echo);
    history_push(cs, line);
    cs->history_pos = -1;
    cs->input[0] = 0;
    cs->input_len = 0;

    console_dispatch(cs, ed, ns, gs, r, inp, local, line);
}

void console_update(ConsoleState *cs, InputState *inp, EditorState *ed,
                    NetState *ns, GameState *gs, Renderer *r, Player *local) {
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

    char codes[PRESSED_CODE_QUEUE_SIZE][KEY_CODE_LEN];
    int ncodes = inp->pressed_code_count;
    memcpy(codes, inp->pressed_codes, (size_t)ncodes * KEY_CODE_LEN);
    inp->pressed_code_count = 0;

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
     * ever see the keydown. */
    if (cs->open && !inp->pointer_locked) {
        cs->open = 0;
        input_set_console_open(inp, 0);
    }

    /* Dispatch any bound keys pressed this frame. input.c only captures
     * pressed_codes while the console isn't open, so this is naturally
     * gated the same way — checked again here defensively. */
    if (!cs->open) {
        for (int i = 0; i < ncodes; i++) {
            for (int b = 0; b < cs->bind_count; b++) {
                if (strcmp(cs->bind_keys[b], codes[i]) == 0) {
                    char msg[CONSOLE_LINE_LEN];
                    snprintf(msg, sizeof(msg), "[bind %s] %s", codes[i], cs->bind_cmds[b]);
                    log_push(cs, msg);
                    console_dispatch(cs, ed, ns, gs, r, inp, local, cs->bind_cmds[b]);
                    break;
                }
            }
        }
    }

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
        console_submit(cs, ed, ns, gs, r, inp, local);
    }
}
