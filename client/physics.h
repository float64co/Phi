#pragma once
#include "octree.h"
#include <stdint.h>

#define GRAVITY          600.0f   /* units/s^2 — initial value of g_gravity, below */
#define JUMP_SPEED       220.0f   /* units/s */
#define MOVE_SPEED       280.0f   /* units/s ground — initial value of g_move_speed, below */
#define AIR_ACCEL        12.0f    /* Quake sv_airaccelerate */
#define GROUND_ACCEL     14.0f
#define FRICTION         12.0f    /* higher = stops sooner after releasing WASD (was 6) */
#define STOP_SPEED       100.0f
#define PLAYER_HALFWIDTH 14.0f
#define PLAYER_HEIGHT    56.0f
#define PLAYER_EYE_H     48.0f
#define PLAYER_EYE_H_CROUCH (PLAYER_EYE_H * 0.5f)   /* lowers aim/rocket-spawn origin while crouching */
#define CROUCH_SPEED_MULT   0.5f

#define ROCKET_SPEED     1400.0f
#define ROCKET_RADIUS    120.0f
#define ROCKET_FORCE     900.0f
#define ROCKET_SELF_DMG  0.7f     /* fraction of blast damage applied to self */
#define ROCKET_SPLASH_DMG 100.0f  /* max direct damage */
#define MAX_ROCKETS      32

#define MAX_BOTS         8   /* hard cap; addbot/delbot manage bots within this */
#define DEFAULT_BOTS     4   /* how many spawn at game start */

/* Runtime-tunable copies of MOVE_SPEED/GRAVITY (console 'speed'/'gravity'
 * commands) — movement code reads these instead of the #define defaults
 * above, which just seed their initial values. */
extern float g_move_speed;
extern float g_gravity;
#define BOT_THINK_RATE   0.25f   /* seconds between AI decisions */
#define BOT_FIRE_RANGE   180.0f  /* max distance to fire */
#define BOT_MOVE_SPEED   160.0f

typedef struct {
    Vec3f pos;
    Vec3f vel;
    float yaw;    /* radians */
    float pitch;  /* radians */
    int   on_ground;
    int   hp;
    uint8_t id;
    char  name[16];
    int   alive;
    float respawn_timer;
    int   noclip;    /* editor mode: position is driven by editor.c's fly_move, skip gravity/collision */
    int   god;       /* console 'god' command: damage/knockback immunity without forcing noclip fly */
    int   crouching; /* Ctrl held: lowers eye/rocket-spawn height, halves ground speed */
    /* Bot AI state */
    int   is_bot;
    float bot_think_timer;
    float bot_fire_timer;
    Vec3f bot_target_pos;   /* where the bot wants to move */
    int   bot_jump;
} Player;

typedef struct {
    uint8_t id;
    uint8_t owner_id;
    Vec3f   pos;
    Vec3f   vel;
    int     active;
    float   lifetime;
} Rocket;

typedef struct {
    Player  players[16];
    Rocket  rockets[MAX_ROCKETS];
    int     num_players;
    Octree *world;
    float   time;
} GameState;

/* Called once per frame */
void physics_update(GameState *gs, float dt);

/* Apply player input (called before physics_update) */
void physics_apply_input(Player *p,
                          int forward, int back, int left, int right,
                          int jump,
                          float yaw, float pitch,
                          float dt, int on_ground);

/* Fire a rocket from player p in the direction they're looking */
Rocket *physics_fire_rocket(GameState *gs, Player *p);

/* Quake-style AABB move with octree collision; returns new pos */
Vec3f physics_move_player(const Octree *world, Vec3f pos, Vec3f *vel, float dt);

/* Check if player is standing on ground */
int physics_check_ground(const Octree *world, Vec3f pos);

/* Effective eye/rocket-spawn height, accounting for crouch */
float player_eye_h(const Player *p);

/* Spawn bots into the game state */
void physics_spawn_bots(GameState *gs, int count);

/* Console addbot/delbot: add/remove one bot at a time (up to MAX_BOTS
 * total). Returns 1 on success, 0 if already at the cap / no bot to remove. */
int physics_add_bot(GameState *gs);
int physics_remove_bot(GameState *gs);

/* Update bot AI (called from physics_update) */
void physics_update_bots(GameState *gs, float dt);
