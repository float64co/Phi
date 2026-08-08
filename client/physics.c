#include "physics.h"
#include "octree.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

float g_move_speed = MOVE_SPEED;
float g_gravity    = GRAVITY;

/* ---- Math helpers ---- */
static inline float vec3_dot(Vec3f a, Vec3f b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}
static inline Vec3f vec3_add(Vec3f a, Vec3f b) {
    return (Vec3f){a.x+b.x, a.y+b.y, a.z+b.z};
}
static inline Vec3f vec3_scale(Vec3f a, float s) {
    return (Vec3f){a.x*s, a.y*s, a.z*s};
}
static inline float vec3_len(Vec3f v) {
    return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
}
static inline Vec3f vec3_norm(Vec3f v) {
    float l = vec3_len(v);
    if (l < 1e-6f) return (Vec3f){0,0,0};
    return vec3_scale(v, 1.0f/l);
}
static inline Vec3f vec3_sub(Vec3f a, Vec3f b) {
    return (Vec3f){a.x-b.x, a.y-b.y, a.z-b.z};
}

/* ---- Player AABB ---- */
static inline void player_bounds(Vec3f pos,
                                  float *x0, float *y0, float *z0,
                                  float *x1, float *y1, float *z1) {
    *x0 = pos.x - PLAYER_HALFWIDTH;
    *x1 = pos.x + PLAYER_HALFWIDTH;
    *y0 = pos.y;
    *y1 = pos.y + PLAYER_HEIGHT;
    *z0 = pos.z - PLAYER_HALFWIDTH;
    *z1 = pos.z + PLAYER_HALFWIDTH;
}

int physics_check_ground(const Octree *world, Vec3f pos) {
    /* physics_move_player() hard-clamps pos.y to 16 regardless of octree
     * content (the "ground plane" — see renderer_draw_ground_plane), so
     * resting there must count as on_ground even where the octree itself
     * has no real solid geometry (anywhere outside the original arena, or
     * anywhere carved empty). Otherwise this falls through to the octree
     * check below, which reads solid=false there, on_ground never becomes
     * true, and movement runs through the air-strafe branch forever —
     * which accelerates but never applies friction, i.e. permanent sliding.
     * Matches the server's equivalent check (on_ground = pos.y <= 16.1) in
     * server.py exactly, for the same reason. */
    if (pos.y <= 16.1f) return 1;

    Vec3f test = pos;
    test.y -= 2.0f;
    float x0, y0, z0, x1, y1, z1;
    player_bounds(test, &x0,&y0,&z0,&x1,&y1,&z1);
    return octree_aabb_solid(world, x0, y0-1, z0, x1, y0+1, z1);
}

float player_eye_h(const Player *p) {
    return p->crouching ? PLAYER_EYE_H_CROUCH : PLAYER_EYE_H;
}

/* ---- Quake-style accelerate ---- */
static void pm_accelerate(Vec3f *vel, Vec3f wish_dir, float wish_speed,
                            float accel, float dt) {
    float current_speed = vec3_dot(*vel, wish_dir);
    float add_speed = wish_speed - current_speed;
    if (add_speed <= 0) return;
    float accel_speed = accel * dt * wish_speed;
    if (accel_speed > add_speed) accel_speed = add_speed;
    vel->x += accel_speed * wish_dir.x;
    vel->y += accel_speed * wish_dir.y;
    vel->z += accel_speed * wish_dir.z;
}

/* ---- Quake-style friction ---- */
static void pm_friction(Vec3f *vel, float dt) {
    float speed = vec3_len(*vel);
    if (speed < 1.0f) { vel->x = vel->y = vel->z = 0; return; }
    float control = speed < STOP_SPEED ? STOP_SPEED : speed;
    float newspeed = speed - dt * control * FRICTION;
    if (newspeed < 0) newspeed = 0;
    float scale = newspeed / speed;
    vel->x *= scale; vel->y *= scale; vel->z *= scale;
}

/* ---- Slide move (one axis at a time, 3 iterations) ---- */
Vec3f physics_move_player(const Octree *world, Vec3f pos, Vec3f *vel, float dt) {
    float x0, y0, z0, x1, y1, z1;
    Vec3f move = vec3_scale(*vel, dt);

    /* Try X */
    Vec3f np = (Vec3f){pos.x + move.x, pos.y, pos.z};
    player_bounds(np, &x0,&y0,&z0,&x1,&y1,&z1);
    if (octree_aabb_solid(world, x0,y0,z0,x1,y1,z1)) {
        move.x = 0; vel->x = 0; np.x = pos.x;
    }

    /* Try Y */
    Vec3f np2 = (Vec3f){np.x, np.y + move.y, np.z};
    player_bounds(np2, &x0,&y0,&z0,&x1,&y1,&z1);
    if (octree_aabb_solid(world, x0,y0,z0,x1,y1,z1)) {
        move.y = 0; vel->y = 0; np2.y = np.y;
    }

    /* Try Z */
    Vec3f np3 = (Vec3f){np2.x, np2.y, np2.z + move.z};
    player_bounds(np3, &x0,&y0,&z0,&x1,&y1,&z1);
    if (octree_aabb_solid(world, x0,y0,z0,x1,y1,z1)) {
        move.z = 0; vel->z = 0; np3.z = np2.z;
    }

    /* Hard clamp to interior bounds — prevents escaping through thin walls
       or falling off the edge if the octree collision misses anything.
       Interior is [16..WORLD_SIZE-16]; player AABB extends PLAYER_HALFWIDTH
       on x/z. Kept WORLD_SIZE-relative (not hardcoded to the old 256) so
       this doesn't trap players at the old boundary once they've carved a
       way into space the editor opened up beyond it. */
    float bmin = 16.0f + PLAYER_HALFWIDTH + 1.0f;
    float bmax = (float)WORLD_SIZE - 16.0f - PLAYER_HALFWIDTH - 1.0f;
    if (np3.x < bmin) { np3.x = bmin; vel->x = 0; }
    if (np3.x > bmax) { np3.x = bmax; vel->x = 0; }
    if (np3.z < bmin) { np3.z = bmin; vel->z = 0; }
    if (np3.z > bmax) { np3.z = bmax; vel->z = 0; }
    /* Floor at y=16, ceiling at y=(WORLD_SIZE-16)-PLAYER_HEIGHT */
    float ymax = (float)WORLD_SIZE - 16.0f - PLAYER_HEIGHT;
    if (np3.y < 16.0f)  { np3.y = 16.0f;  vel->y = 0; }
    if (np3.y > ymax)   { np3.y = ymax;   vel->y = 0; }

    return np3;
}

void physics_apply_input(Player *p,
                          int forward, int back, int left, int right,
                          int jump,
                          float yaw, float pitch,
                          float dt, int on_ground) {
    p->yaw   = yaw;
    p->pitch = pitch;

    /* Build wish direction in world space */
    float sy = sinf(yaw), cy = cosf(yaw);
    Vec3f fwd  = {-sy, 0,  -cy};
    Vec3f right_v = { cy, 0, -sy};

    Vec3f wish = {0,0,0};
    if (forward) { wish.x += fwd.x;   wish.z += fwd.z; }
    if (back)    { wish.x -= fwd.x;   wish.z -= fwd.z; }
    if (left)    { wish.x -= right_v.x; wish.z -= right_v.z; }
    if (right)   { wish.x += right_v.x; wish.z += right_v.z; }

    float wish_len = sqrtf(wish.x*wish.x + wish.z*wish.z);
    if (wish_len > 0) {
        wish.x /= wish_len;
        wish.z /= wish_len;
    }

    float move_speed = p->crouching ? g_move_speed * CROUCH_SPEED_MULT : g_move_speed;

    if (on_ground) {
        /* Ground movement */
        p->vel.y = 0;
        if (wish_len > 0) {
            /* Still-standard Quake accel/friction while a key is actually
             * held, so strafing/turning keeps some feel. */
            pm_friction(&p->vel, dt);
            pm_accelerate(&p->vel, wish, move_speed, GROUND_ACCEL, dt);
        } else {
            /* No movement key held: stop almost instantly instead of
             * coasting on a friction decay curve. */
            p->vel.x = 0;
            p->vel.z = 0;
        }
        if (jump) {
            p->vel.y = JUMP_SPEED;
        }
    } else {
        /* Air movement: Quake air strafing */
        pm_accelerate(&p->vel, wish, g_move_speed, AIR_ACCEL, dt);
    }
}

Rocket *physics_fire_rocket(GameState *gs, Player *p) {
    /* Find free rocket slot */
    Rocket *r = NULL;
    for (int i = 0; i < MAX_ROCKETS; i++) {
        if (!gs->rockets[i].active) { r = &gs->rockets[i]; break; }
    }
    if (!r) return NULL;

    r->active   = 1;
    r->owner_id = p->id;
    r->lifetime = 8.0f;

    /* Spawn from eye position */
    float sy = sinf(p->yaw), cy = cosf(p->yaw);
    float sp = sinf(p->pitch), cp = cosf(p->pitch);
    Vec3f eye = {p->pos.x, p->pos.y + player_eye_h(p), p->pos.z};

    /* View-aligned direction — matches crosshair exactly */
    Vec3f dir = vec3_norm((Vec3f){-sy*cp, sp, -cy*cp});

    /* Spawn slightly forward so the rocket clears the player AABB */
    r->pos = (Vec3f){
        eye.x + dir.x * (PLAYER_HALFWIDTH + 4.0f),
        eye.y + dir.y * (PLAYER_HALFWIDTH + 4.0f),
        eye.z + dir.z * (PLAYER_HALFWIDTH + 4.0f)
    };
    r->vel = vec3_scale(dir, ROCKET_SPEED);
    return r;
}

static void rocket_explode(GameState *gs, Rocket *r) {
    r->active = 0;

    /* Apply impulse + damage to all players in radius */
    for (int i = 0; i < gs->num_players; i++) {
        Player *p = &gs->players[i];
        if (!p->alive) continue;
        if (p->noclip || p->god) continue;   /* editing or god mode: immune to damage/knockback */

        Vec3f eye = {p->pos.x, p->pos.y + player_eye_h(p), p->pos.z};
        Vec3f delta = vec3_sub(eye, r->pos);
        float dist = vec3_len(delta);
        if (dist >= ROCKET_RADIUS) continue;

        /* Occlusion check: skip if solid geometry blocks the straight line
         * from the explosion to this player — e.g. a floor/wall between
         * them. Without this, splash damage/knockback reaches straight
         * through built geometry even though the rocket itself correctly
         * can't (it already stops on octree_ray_cast hits in the movement
         * loop above). Skipped for near-point-blank hits (self rocket-jumps,
         * direct hits) so standing on your own floor and firing at your
         * feet still works normally. */
        if (dist > 2.0f) {
            Vec3f dir = vec3_scale(delta, 1.0f / dist);
            /* Nudge the cast origin 1 unit toward the target first — the
             * explosion point is usually exactly ON the surface it just
             * hit (rocket impact), and casting from exactly on a boundary
             * is numerically ambiguous: it can register that same surface
             * as "blocking" even though the ray is heading away from it
             * into open space, which would wrongly negate self
             * rocket-jumps (explosion at your feet, straight up to your
             * own eye, on the very floor you're standing on). */
            Vec3f origin = { r->pos.x + dir.x, r->pos.y + dir.y, r->pos.z + dir.z };
            Vec3f hit, norm;
            float blocked_t = octree_ray_cast(gs->world, origin, dir, dist - 2.0f, &hit, &norm);
            if (blocked_t >= 0) continue;
        }

        float frac = 1.0f - dist / ROCKET_RADIUS;

        /* Impulse: away from explosion centre */
        Vec3f impulse = vec3_scale(vec3_norm(delta), frac * ROCKET_FORCE);
        p->vel = vec3_add(p->vel, impulse);

        /* Damage */
        int dmg = (int)(ROCKET_SPLASH_DMG * frac);
        if (p->id == r->owner_id) dmg = (int)(dmg * ROCKET_SELF_DMG);
        p->hp -= dmg;

        /* Rocket jumping: never kill player from self-damage below 1hp */
        if (p->id == r->owner_id && p->hp < 1) p->hp = 1;
        else if (p->hp <= 0) {
            p->alive = 0;
            p->hp = 0;
            p->respawn_timer = 3.0f;
            printf("Player %d killed by player %d's rocket\n", p->id, r->owner_id);
        }
    }
}

void physics_update(GameState *gs, float dt) {
    gs->time += dt;

    /* Update rockets */
    for (int i = 0; i < MAX_ROCKETS; i++) {
        Rocket *r = &gs->rockets[i];
        if (!r->active) continue;

        r->lifetime -= dt;
        if (r->lifetime <= 0) { r->active = 0; continue; }

        /* Move rocket */
        Vec3f new_pos = {
            r->pos.x + r->vel.x * dt,
            r->pos.y + r->vel.y * dt,
            r->pos.z + r->vel.z * dt
        };

        /* Rocket vs world */
        Vec3f hit, norm;
        float t = octree_ray_cast(gs->world, r->pos,
                                   vec3_norm(r->vel),
                                   vec3_len(r->vel) * dt,
                                   &hit, &norm);
        if (t >= 0) {
            r->pos = hit;
            rocket_explode(gs, r);
            continue;
        }
        r->pos = new_pos;
    }

    /* Update players */
    for (int i = 0; i < gs->num_players; i++) {
        Player *p = &gs->players[i];
        if (!p->alive) {
            p->respawn_timer -= dt;
            if (p->respawn_timer <= 0) {
                /* Respawn at centre of map, on top of the central platform
                 * (y=32 sat inside its solid base — stuck-in-wall bug) */
                p->pos = (Vec3f){128, 48, 128};
                p->vel = (Vec3f){0,0,0};
                p->hp  = 100;
                p->alive = 1;
            }
            continue;
        }

        if (p->noclip) continue;   /* editor mode owns this player's position */

        int on_ground = physics_check_ground(gs->world, p->pos);
        p->on_ground  = on_ground;

        /* Gravity */
        if (!on_ground) p->vel.y -= g_gravity * dt;

        /* Slide move */
        p->pos = physics_move_player(gs->world, p->pos, &p->vel, dt);

        /* World bounds clamp */
        if (p->pos.y < -32) {
            p->alive = 0;
            p->respawn_timer = 2.0f;
        }
    }

    /* Update bot AI */
    physics_update_bots(gs, dt);
}

/* ---- Bot respawn positions ---- */
static Vec3f s_bot_spawns[MAX_BOTS] = {
    { 32, 32,  32},   /* NW corner, ground level */
    {224, 32,  32},   /* NE corner */
    { 32, 32, 224},   /* SW corner */
    {224, 32, 224},   /* SE corner */
    {128, 32,  32},   /* N-mid */
    {128, 32, 224},   /* S-mid */
    { 32, 32, 128},   /* W-mid */
    {224, 32, 128},   /* E-mid */
};
static const char *s_bot_names[MAX_BOTS] = {
    "Ranger","Keel","Anarki","Slash","Sarge","Doom","Visor","Major"
};

static void init_bot(GameState *gs, Player *b, int slot) {
    memset(b, 0, sizeof(*b));
    b->id       = 200 + slot;   /* bot IDs start at 200 */
    b->is_bot   = 1;
    b->alive    = 1;
    b->hp       = 100;
    b->pos      = s_bot_spawns[slot];
    b->vel      = (Vec3f){0,0,0};
    b->bot_think_timer = (float)slot * 0.1f;  /* stagger first think */
    b->bot_fire_timer  = 1.0f + slot * 0.3f;
    strncpy(b->name, s_bot_names[slot], 15);
    (void)gs;
}

void physics_spawn_bots(GameState *gs, int count) {
    if (count > MAX_BOTS) count = MAX_BOTS;
    for (int i = 0; i < count; i++) {
        if (gs->num_players >= 16) break;
        init_bot(gs, &gs->players[gs->num_players++], i);
    }
}

/* Console addbot: fills the next unused slot in s_bot_spawns/s_bot_names,
 * counting bots already present (so it composes with physics_spawn_bots()
 * having already filled some at startup, and with prior addbot/delbot). */
int physics_add_bot(GameState *gs) {
    if (gs->num_players >= 16) return 0;
    int bot_count = 0;
    for (int i = 0; i < gs->num_players; i++)
        if (gs->players[i].is_bot) bot_count++;
    if (bot_count >= MAX_BOTS) return 0;
    init_bot(gs, &gs->players[gs->num_players++], bot_count);
    return 1;
}

/* Removes the most-recently-added bot (highest bot id), compacting the
 * players array so num_players/indices stay contiguous. */
int physics_remove_bot(GameState *gs) {
    int found = -1;
    uint8_t best_id = 0;
    for (int i = 0; i < gs->num_players; i++) {
        if (gs->players[i].is_bot && gs->players[i].id >= best_id) {
            best_id = gs->players[i].id;
            found = i;
        }
    }
    if (found < 0) return 0;
    for (int i = found; i < gs->num_players - 1; i++)
        gs->players[i] = gs->players[i + 1];
    gs->num_players--;
    return 1;
}

void physics_update_bots(GameState *gs, float dt) {
    for (int i = 0; i < gs->num_players; i++) {
        Player *b = &gs->players[i];
        if (!b->is_bot || !b->alive) continue;

        b->bot_think_timer -= dt;
        b->bot_fire_timer  -= dt;

        /* ---- Think: pick a target player ---- */
        if (b->bot_think_timer <= 0.0f) {
            b->bot_think_timer = BOT_THINK_RATE;

            /* Find nearest non-bot alive player */
            Player *target = NULL;
            float best_dist = 1e30f;
            for (int j = 0; j < gs->num_players; j++) {
                Player *p = &gs->players[j];
                if (p->is_bot || !p->alive || p->noclip || p->god) continue;
                Vec3f d = vec3_sub(p->pos, b->pos);
                float dist = vec3_len(d);
                if (dist < best_dist) { best_dist = dist; target = p; }
            }

            if (target) {
                /* Move toward target with a bit of randomness */
                b->bot_target_pos = target->pos;

                /* Aim yaw/pitch at target eye */
                Vec3f eye_t = {target->pos.x, target->pos.y + PLAYER_EYE_H, target->pos.z};
                Vec3f eye_b = {b->pos.x,      b->pos.y      + PLAYER_EYE_H, b->pos.z};
                Vec3f diff  = vec3_sub(eye_t, eye_b);
                b->yaw   = atan2f(-diff.x, -diff.z);
                float horiz = sqrtf(diff.x*diff.x + diff.z*diff.z);
                b->pitch = atan2f(diff.y, horiz);

                /* Rocket jump if target is significantly higher */
                b->bot_jump = (target->pos.y > b->pos.y + 32.0f) && b->on_ground;
            } else {
                /* Wander: face centre of map */
                Vec3f centre = {128, 48, 128};
                Vec3f diff   = vec3_sub(centre, b->pos);
                b->yaw  = atan2f(-diff.x, -diff.z);
                b->pitch = 0.0f;
                b->bot_target_pos = centre;
                b->bot_jump = 0;
            }
        }

        /* ---- Move toward target position ---- */
        {
            Vec3f diff = vec3_sub(b->bot_target_pos, b->pos);
            diff.y = 0;
            float dist = sqrtf(diff.x*diff.x + diff.z*diff.z);
            if (dist > 8.0f) {
                float sy = sinf(b->yaw), cy = cosf(b->yaw);
                /* Synthesise forward input along yaw direction */
                Vec3f fwd = {-sy, 0, -cy};
                float accel = b->on_ground ? GROUND_ACCEL : AIR_ACCEL;
                float ws    = BOT_MOVE_SPEED;
                float curr  = b->vel.x*fwd.x + b->vel.z*fwd.z;
                float add   = ws - curr;
                if (add > 0) {
                    float asp = fminf(accel * dt * ws, add);
                    b->vel.x += asp * fwd.x;
                    b->vel.z += asp * fwd.z;
                }
            }
        }

        /* ---- Jump ---- */
        if (b->bot_jump && b->on_ground) {
            b->vel.y = JUMP_SPEED;
            b->bot_jump = 0;
        }

        /* ---- Fire: aim slightly ahead of target, fire rockets ---- */
        if (b->bot_fire_timer <= 0.0f) {
            b->bot_fire_timer = 0.8f + (float)(b->id & 3) * 0.15f;

            /* Find a target to shoot at */
            for (int j = 0; j < gs->num_players; j++) {
                Player *p = &gs->players[j];
                if (p->is_bot || !p->alive || p->noclip || p->god) continue;
                Vec3f eye_b = {b->pos.x, b->pos.y + PLAYER_EYE_H, b->pos.z};
                Vec3f eye_p = {p->pos.x, p->pos.y + PLAYER_EYE_H, p->pos.z};
                Vec3f diff  = vec3_sub(eye_p, eye_b);
                float dist  = vec3_len(diff);
                if (dist > BOT_FIRE_RANGE) continue;

                /* Spawn rocket */
                Rocket *r = physics_fire_rocket(gs, b);
                if (r) {
                    /* Lead the target slightly */
                    float travel = dist / ROCKET_SPEED;
                    Vec3f lead = {
                        eye_p.x + p->vel.x * travel * 0.5f,
                        eye_p.y + p->vel.y * travel * 0.5f,
                        eye_p.z + p->vel.z * travel * 0.5f
                    };
                    Vec3f dir = vec3_norm(vec3_sub(lead, eye_b));
                    r->vel = vec3_scale(dir, ROCKET_SPEED);
                }
                break;
            }
        }
    }
}
