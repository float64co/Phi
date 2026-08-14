#pragma once
#include "vec3.h"

/* Light objects -- Phase 3's "Both like Blender does it" light-source
 * model (see phi.md's Phase 3 status): mesh-face emission (already real,
 * see HEFace.emission/phi_prop_registry.c) covers area-light-from-
 * geometry; this file covers the OTHER half, dedicated Light objects
 * (Point/Sun/Spot/Area), mirroring the core fields of Blender's own
 * `Light` DNA struct (blender/source/blender/makesdna/DNA_light_types.h,
 * read directly rather than guessed) -- deliberately a real subset, not
 * every EEVEE-specific/deprecated field Blender itself carries (shadow
 * filter radius, cascade shadow-map tuning, etc. -- meaningless without
 * a shadow system for arbitrary lights, which this pass doesn't build).
 *
 * A fixed-capacity registry (PHI_MAX_LIGHTS), not a real dynamic scene
 * graph -- same "small, bounded, real" scope this project's other
 * fixed-capacity systems use (see e.g. console.h's CONSOLE_LOG_LINES).
 * Deliberately NOT wired into the live deferred rasterizer's own
 * lighting pass in this pass -- that pass still runs off main.c's one
 * hardcoded directional light, unrelated to these. Spawned Lights are
 * real, persistent, selectable, editable scene data, feeding the not-
 * yet-built Phase 3 path tracer once it exists; flagged honestly rather
 * than half-wiring a multi-light deferred-shading pass this feature
 * wasn't asked for. */

typedef enum {
    LIGHT_TYPE_POINT = 0,
    LIGHT_TYPE_SUN,
    LIGHT_TYPE_SPOT,
    LIGHT_TYPE_AREA,
    LIGHT_TYPE_COUNT
} LightType;

typedef struct {
    int       id;          /* stable identity, 1-based -- 0 means "invalid/unused slot", never a real light's id */
    LightType type;
    Vec3f     position;
    /* Sun/Spot only -- the direction the light points, e.g. {0,-1,0} for
     * straight down (Blender's own default light orientation). Point/
     * Area lights ignore this (Area's own facing isn't modeled this
     * pass -- see the struct's own scope note above). Not required to
     * arrive unit-length; light_spawn/light-prop-set normalize it. */
    Vec3f     direction;
    Vec3f     color;        /* default {1,1,1} */
    float     energy;       /* power/strength, Watts-ish, same rough order of magnitude Blender's own defaults use */
    float     radius;       /* Point only -- soft-shadow source radius */
    float     spot_size;    /* Spot only, radians, full cone angle */
    float     spot_blend;   /* Spot only, 0..1, edge softness */
    float     area_size;    /* Area only -- square side length (LA_AREA_SQUARE only this pass, not Blender's rect/disk/ellipse variants) */
    float     sun_angle;    /* Sun only, radians, angular diameter (soft-shadow penumbra size) */
} PhiLight;

#define PHI_MAX_LIGHTS 16

/* Selection-id range for lights, shared by main.c (picking/context menu)
 * and ui.c (Properties panel/Outliner) -- see ui.h's own comment on the
 * full id-range convention (wire-box=3, MeshObjects=4000+id, Lights=
 * 5000+id). A public constant here rather than two independently-typed
 * "5000u" literals drifting apart. */
#define LIGHT_ID_BASE 5000u

/* World-space radius used for BOTH the ray-vs-light pick test below and
 * the icon rendering (renderer.c's renderer_draw_lights) -- one public
 * constant rather than two independently-tuned numbers that could drift
 * apart and make the clickable region not match what's actually drawn. */
#define LIGHT_ICON_RADIUS 2.0f

/* Clears the registry -- call once at startup, same role meshobject.c's
 * spawn functions play for the (separate) MeshObject slot. */
void light_system_init(void);

/* Adds a new light, direction defaulted to {0,-1,0} and every other
 * field to a reasonable per-type default (see light.c). Returns NULL
 * (no id consumed) if the registry is already at PHI_MAX_LIGHTS. */
PhiLight *light_spawn(LightType type, Vec3f position);

/* Removes the light with this id. Returns 1 on success, 0 if no light
 * with that id exists (already deleted / never existed). */
int light_delete(int id);

/* Returns NULL if no live light has this id. */
PhiLight *light_find(int id);

/* Fills out_lights (caller-owned, PHI_MAX_LIGHTS capacity) with pointers
 * to every live light and returns the count -- for iterating to draw/
 * pick/list (Outliner, scene-state JSON) without exposing the registry's
 * own internal array/slot layout to callers. */
int light_get_all(PhiLight *out_lights[PHI_MAX_LIGHTS]);

/* Ray-vs-light picking -- lights have no mesh geometry of their own, so
 * this tests a fixed-radius sphere at each light's position (see
 * light.c's LIGHT_ICON_RADIUS) and returns whichever is nearest along
 * the ray (smallest hit t) -- the same "nearest hit wins" contract
 * meshobject_ray_pick_face already uses, so main.c's try_pick_object can
 * compare a mesh hit and a light hit directly by t. Returns NULL
 * (*out_t untouched) if the ray misses every light. */
PhiLight *light_ray_pick(Vec3f ray_origin, Vec3f ray_dir, float *out_t);

/* Icon rendering (renderer_draw_lights) deliberately lives in renderer.c,
 * not here -- same split renderer_draw_mesh_object already has from
 * meshobject.c (entity data/logic in its own file, drawing it is the
 * renderer's job), which also keeps this file (and everything that only
 * needs the data model -- mp_port.c's Python bindings, scene_target.c's
 * resolver, standalone test harnesses) free of any GL/Renderer
 * dependency at all. */
