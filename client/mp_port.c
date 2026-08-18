/* Phi's own MicroPython "port" glue — NOT part of the generated
 * client/micropython_embed/ tree (that's regenerated from upstream, see
 * client/mpconfigport.h's header comment; this file is hand-written and
 * stays put across regenerations). Two unrelated jobs live here:
 *
 * 1. mp_lexer_new_from_file below: this embedding has no real filesystem
 *    -- every script Phi runs arrives as in-memory text
 *    (mp_embed_exec_str), never loaded via a file path. Config in
 *    mpconfigport.h turns off the code paths that assume file-backed
 *    import/open/stdio exist, EXCEPT exec()/eval()'s file-path overload
 *    (builtinevex.c's eval_exec_helper), which still references
 *    mp_lexer_new_from_file unconditionally regardless of that config.
 *    Phi never calls exec()/eval() with a filename argument, so this
 *    only needs to link, not actually succeed — it raises OSError if
 *    ever reached.
 *
 * 2. phi_mp_init/phi_mp_exec/phi_mp_capture_output further down (see
 *    mp_port.h): the real interpreter lifecycle + a Console-facing exec
 *    call that captures print()/traceback output instead of letting it
 *    go to the process's real stdout, invisible from inside the game
 *    window -- this is Phase 1's "Console panel becomes a real Python
 *    REPL" piece (see phi.md), NOT Phase 5's phi.emit/ctx.prop/
 *    @phi.panel/@phi.node decorator API surface, which stays out of
 *    scope here on purpose. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "py/lexer.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "port/micropython_embed.h"
#include "mp_port.h"
#include "phi_prop_registry.h"
#include "scene_target.h"
#include "light.h"
#include "halfedge.h"
#include "halfedge_gltf.h"
#include "scene_objects.h"
#include "mesh_edit.h"
#include "node_graph.h"
#include "input.h"
#include "input_gamepad.h"
#include "phi_audio.h"

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    mp_raise_OSError(ENOENT);
}

/* ---- Phi's own init/exec/output-capture glue (see mp_port.h) ---- */

/* Same 64KB size client/mp_test_main.c/mp_stress_test_main.c already
 * proved sufficient for the @phi.panel/@phi.node decorator-pattern
 * validation scripts -- this pass's Console-typed one-liners are no
 * heavier than those. */
static char s_mp_heap[64 * 1024];

static char  *s_capture_buf = NULL;
static size_t s_capture_len = 0, s_capture_cap = 0;

/* Routed here by mpconfigport.h's MP_PLAT_PRINT_STRN override -- this is
 * every byte print()/an exception traceback would otherwise send to the
 * real process stdout (mphalport.c's default mp_hal_stdout_tx_strn_cooked,
 * invisible from inside the game window), captured instead so the Console
 * panel can show it. */
void phi_mp_capture_output(const char *str, size_t len) {
    if (s_capture_len + len + 1 > s_capture_cap) {
        size_t new_cap = s_capture_cap ? s_capture_cap * 2 : 256;
        while (new_cap < s_capture_len + len + 1) new_cap *= 2;
        s_capture_buf = (char *)realloc(s_capture_buf, new_cap);
        s_capture_cap = new_cap;
    }
    memcpy(s_capture_buf + s_capture_len, str, len);
    s_capture_len += len;
    s_capture_buf[s_capture_len] = 0;
}

/* ---- DNA/RNA property system + @phi.panel, exposed to Python (see
 * mp_port.h's own comment on this section) ---- */

#define PHI_MP_MAX_PANELS 8

/* Real multi-object scene graph (Phase 5, see scene_objects.h) --
 * "the object" phi.enable_physics/apply_impulse/get_velocity/set_velocity
 * operate on is now WHICHEVER MeshObject is currently selected, resolved
 * fresh through this callback every call rather than a fixed pointer +
 * loaded bool (main.c's selected_mesh_object, registered once at
 * startup) -- same function-pointer-handoff shape phi_mp_register_
 * render_callback/_ragdoll_callback already use for cross-file state
 * main.c owns. */
static MeshObject      *(*s_get_selected_object)(void) = NULL;
static const int       *s_target_edit_face = NULL;
static PhiPhysicsWorld *s_phys_world = NULL;

void phi_mp_register_targets(MeshObject *(*get_selected_object)(void), const int *edit_face,
                              PhiPhysicsWorld *phys_world) {
    s_get_selected_object = get_selected_object;
    s_target_edit_face = edit_face;
    s_phys_world = phys_world;
}

/* Target resolution ("object"/"face"/"light:<id>"/"render") now lives in
 * scene_target.c, shared with chat-driven scene mutation (main.c's
 * PKT_PROP_SET_REQUEST handler) rather than being duplicated here -- see
 * its own comment for why. phi_mp_register_targets below still registers
 * this file's OWN physics-specific resolver (used directly by enable_
 * physics/apply_impulse/get_velocity/set_velocity, which need more than
 * just prop get/set); main.c calls scene_target_register alongside it
 * with the same underlying resolver. */

static mp_obj_t native_prop_get(mp_obj_t target_obj, mp_obj_t identifier_obj) {
    const char *target = mp_obj_str_get_str(target_obj);
    const char *identifier = mp_obj_str_get_str(identifier_obj);
    const PhiPropGroup *group; void *owner;
    if (!scene_resolve_target(target, &group, &owner)) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.prop_get: target not currently available ('object' needs a loaded MeshObject, 'face' needs a selected face, 'light:<id>' needs that light to exist, 'render' is always available)"));
    }
    const PhiProp *prop = phi_prop_find(group, identifier);
    if (!prop) mp_raise_ValueError(MP_ERROR_TEXT("phi.prop_get: unknown property identifier"));
    if (prop->type == PHI_PROP_VEC3) {
        float v[3];
        phi_prop_get_vec3(owner, prop, v);
        mp_obj_t items[3] = { mp_obj_new_float(v[0]), mp_obj_new_float(v[1]), mp_obj_new_float(v[2]) };
        return mp_obj_new_tuple(3, items);
    }
    float v;
    phi_prop_get_float(owner, prop, &v);
    return mp_obj_new_float((mp_float_t)v);
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_prop_get_obj, native_prop_get);

static mp_obj_t native_prop_set(mp_obj_t target_obj, mp_obj_t identifier_obj, mp_obj_t value_obj) {
    const char *target = mp_obj_str_get_str(target_obj);
    const char *identifier = mp_obj_str_get_str(identifier_obj);
    const PhiPropGroup *group; void *owner;
    if (!scene_resolve_target(target, &group, &owner)) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.prop_set: target not currently available ('object' needs a loaded MeshObject, 'face' needs a selected face, 'light:<id>' needs that light to exist, 'render' is always available)"));
    }
    const PhiProp *prop = phi_prop_find(group, identifier);
    if (!prop) mp_raise_ValueError(MP_ERROR_TEXT("phi.prop_set: unknown property identifier"));
    if (prop->type == PHI_PROP_VEC3) {
        size_t n; mp_obj_t *items;
        mp_obj_get_array(value_obj, &n, &items);
        if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.prop_set: expected a 3-element sequence for a VEC3 property"));
        float v[3] = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
        phi_prop_set_vec3(owner, prop, v);
    } else {
        phi_prop_set_float(owner, prop, (float)mp_obj_get_float(value_obj));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(native_prop_set_obj, native_prop_set);

/* ---- Physics, exposed to Python (see phi.md's "Bullet Physics via
 * Emscripten" -- Python API surface) -- operates only on the "object"
 * target's phys_body (there's no per-face or arbitrary-object physics
 * concept), a narrower version of phi.prop_get/set's own "object"/"face"
 * simplification, since only whole objects have rigid bodies at all. */

static mp_obj_t native_enable_physics(mp_obj_t mass_obj, mp_obj_t restitution_obj) {
    MeshObject *obj = s_get_selected_object ? s_get_selected_object() : NULL;
    if (!obj) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.enable_physics: no MeshObject selected"));
    }
    if (obj->phys_body) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.enable_physics: already has a physics body"));
    }
    /* Convex hull from the mesh's own real vertices -- same shape choice
     * as main.c's Enable Physics context-menu action now makes (see its
     * own comment for why, and Blender's rigidbody.cc default for
     * dynamic bodies), kept consistent here so triggering physics from a
     * script produces the identical physical result as triggering it
     * from the menu. */
    if (!obj->hem || obj->hem->vert_count < 4) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.enable_physics: not enough vertices for a hull (need a real 3D mesh)"));
    }
    HalfEdgeMesh *hem = obj->hem;
    float *flat = (float *)malloc((size_t)hem->vert_count * 3 * sizeof(float));
    for (int i = 0; i < hem->vert_count; i++) {
        flat[i*3+0] = hem->verts[i].pos[0];
        flat[i*3+1] = hem->verts[i].pos[1];
        flat[i*3+2] = hem->verts[i].pos[2];
    }
    float orientation[4] = {
        obj->orientation.x, obj->orientation.y,
        obj->orientation.z, obj->orientation.w
    };
    float mass = (float)mp_obj_get_float(mass_obj);
    float restitution = (float)mp_obj_get_float(restitution_obj);
    obj->phys_body = phi_physics_add_convex_hull_body(s_phys_world, flat, hem->vert_count,
                                                        obj->position, orientation,
                                                        mass, restitution);
    free(flat);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_enable_physics_obj, native_enable_physics);

/* Shared guard for the three functions below -- all three are meaningless
 * (and would otherwise segfault on a NULL phys_body) before phi.
 * enable_physics has actually created one. Returns the selected object's
 * phys_body (NULL if there's no valid target or it has none yet), so
 * callers get both the guard result and the pointer they need from one
 * call instead of re-resolving s_get_selected_object() themselves. */
static PhiRigidBody *require_phys_body(void) {
    MeshObject *obj = s_get_selected_object ? s_get_selected_object() : NULL;
    return obj ? obj->phys_body : NULL;
}

static mp_obj_t native_apply_impulse(mp_obj_t impulse_obj, mp_obj_t rel_pos_obj) {
    PhiRigidBody *body = require_phys_body();
    if (!body) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.apply_impulse: object has no physics body (call phi.enable_physics first)"));
    }
    size_t n; mp_obj_t *items;
    mp_obj_get_array(impulse_obj, &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.apply_impulse: expected a 3-element impulse vector"));
    Vec3f impulse = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    mp_obj_get_array(rel_pos_obj, &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.apply_impulse: expected a 3-element rel_pos vector"));
    Vec3f rel_pos = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    phi_physics_apply_impulse(body, impulse, rel_pos);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_apply_impulse_obj, native_apply_impulse);

static mp_obj_t native_get_velocity(void) {
    PhiRigidBody *body = require_phys_body();
    if (!body) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.get_velocity: object has no physics body (call phi.enable_physics first)"));
    }
    Vec3f v = phi_physics_get_linear_velocity(body);
    mp_obj_t items[3] = { mp_obj_new_float(v.x), mp_obj_new_float(v.y), mp_obj_new_float(v.z) };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_get_velocity_obj, native_get_velocity);

static mp_obj_t native_set_velocity(mp_obj_t v_obj) {
    PhiRigidBody *body = require_phys_body();
    if (!body) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.set_velocity: object has no physics body (call phi.enable_physics first)"));
    }
    size_t n; mp_obj_t *items;
    mp_obj_get_array(v_obj, &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.set_velocity: expected a 3-element velocity vector"));
    Vec3f v = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    phi_physics_set_linear_velocity(body, v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_set_velocity_obj, native_set_velocity);

/* ---- Camera control (Phase 9 gap-closing, 2026-08-18 -- see phi.md's
 * Phase 9 "Known gaps": the player build had no way to move the camera
 * at all, from C or Python). Function-pointer handoff, NOT a raw
 * Renderer* -- see mp_port.h's phi_mp_register_camera_callback comment
 * for why (keeps mp_port.c from ever needing renderer.c/GL linked into
 * the lightweight mp_geometry_test/mp_node_test Makefile targets).
 * Registered only by player_main.c (the editor drives its camera from
 * mouse orbit/pan via its own cam_recompute_pos, see editor_main.c; a
 * Python override there would fight that every frame, so this stays
 * player-only on purpose), same shape phi_mp_register_render_callback/
 * _ragdoll_callback below already use for main.c-owned logic. */
static void (*s_set_camera_cb)(Vec3f eye, float yaw, float pitch) = NULL;

void phi_mp_register_camera_callback(void (*set_camera)(Vec3f eye, float yaw, float pitch)) {
    s_set_camera_cb = set_camera;
}

static mp_obj_t native_set_camera(size_t n_args, const mp_obj_t *args) {
    if (!s_set_camera_cb) mp_raise_ValueError(MP_ERROR_TEXT("phi.set_camera: not available (no camera callback registered)"));
    Vec3f eye = {
        (float)mp_obj_get_float(args[0]), (float)mp_obj_get_float(args[1]), (float)mp_obj_get_float(args[2])
    };
    float yaw   = (float)mp_obj_get_float(args[3]);
    float pitch = (float)mp_obj_get_float(args[4]);
    s_set_camera_cb(eye, yaw, pitch);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_set_camera_obj, 5, 5, native_set_camera);

/* ---- Whole-object transforms, exposed to Python and keyed by object id
 * (Phase 9 gap-closing, 2026-08-18 -- see phi.md's Phase 9 "Known gaps":
 * phi.mesh_object() placed an object once, at creation, with no way for
 * a script to move/rotate/scale it afterward -- the only lever was
 * mutating raw vertex positions directly. Object-id-keyed like phi.
 * get_vertices/set_vertices etc. above, NOT selection-keyed like phi.
 * enable_physics/prop_get('object', ...) -- these need to work from a
 * shipped game, which has no selection concept at all (see player_
 * main.c's own top comment). */

static mp_obj_t native_get_object_position(mp_obj_t id_obj) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(id_obj));
    if (!obj) mp_raise_ValueError(MP_ERROR_TEXT("phi.get_object_position: no such object"));
    mp_obj_t items[3] = { mp_obj_new_float(obj->position.x), mp_obj_new_float(obj->position.y), mp_obj_new_float(obj->position.z) };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_get_object_position_obj, native_get_object_position);

static mp_obj_t native_set_object_position(size_t n_args, const mp_obj_t *args) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(args[0]));
    if (!obj) mp_raise_ValueError(MP_ERROR_TEXT("phi.set_object_position: no such object"));
    obj->position.x = (float)mp_obj_get_float(args[1]);
    obj->position.y = (float)mp_obj_get_float(args[2]);
    obj->position.z = (float)mp_obj_get_float(args[3]);
    /* A kinematic/static sync, not a physics push -- matches gizmo drag's
     * own "the transform IS the new ground truth" behavior on a static
     * object (see transform_op.c). A DYNAMIC body would just have its
     * own simulated transform overwrite this again next physics step;
     * real "move a physics object by script" goes through phi.object_
     * apply_impulse/set_velocity instead, same division of labor
     * phi.apply_impulse vs. a gizmo drag already has today. */
    if (obj->phys_body) {
        float orientation[4] = { obj->orientation.x, obj->orientation.y, obj->orientation.z, obj->orientation.w };
        phi_physics_set_transform(obj->phys_body, obj->position, orientation);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_set_object_position_obj, 4, 4, native_set_object_position);

static mp_obj_t native_get_object_rotation(mp_obj_t id_obj) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(id_obj));
    if (!obj) mp_raise_ValueError(MP_ERROR_TEXT("phi.get_object_rotation: no such object"));
    mp_obj_t items[4] = {
        mp_obj_new_float(obj->orientation.x), mp_obj_new_float(obj->orientation.y),
        mp_obj_new_float(obj->orientation.z), mp_obj_new_float(obj->orientation.w)
    };
    return mp_obj_new_tuple(4, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_get_object_rotation_obj, native_get_object_rotation);

static mp_obj_t native_set_object_rotation(size_t n_args, const mp_obj_t *args) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(args[0]));
    if (!obj) mp_raise_ValueError(MP_ERROR_TEXT("phi.set_object_rotation: no such object"));
    obj->orientation.x = (float)mp_obj_get_float(args[1]);
    obj->orientation.y = (float)mp_obj_get_float(args[2]);
    obj->orientation.z = (float)mp_obj_get_float(args[3]);
    obj->orientation.w = (float)mp_obj_get_float(args[4]);
    if (obj->phys_body) {
        float orientation[4] = { obj->orientation.x, obj->orientation.y, obj->orientation.z, obj->orientation.w };
        phi_physics_set_transform(obj->phys_body, obj->position, orientation);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_set_object_rotation_obj, 5, 5, native_set_object_rotation);

static mp_obj_t native_get_object_scale(mp_obj_t id_obj) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(id_obj));
    if (!obj) mp_raise_ValueError(MP_ERROR_TEXT("phi.get_object_scale: no such object"));
    mp_obj_t items[3] = { mp_obj_new_float(obj->scale.x), mp_obj_new_float(obj->scale.y), mp_obj_new_float(obj->scale.z) };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_get_object_scale_obj, native_get_object_scale);

static mp_obj_t native_set_object_scale(size_t n_args, const mp_obj_t *args) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(args[0]));
    if (!obj) mp_raise_ValueError(MP_ERROR_TEXT("phi.set_object_scale: no such object"));
    /* Zeroed scale is a real, previously-hit degenerate case elsewhere in
     * this codebase (see meshobject.h's own scale comment) -- guarded the
     * same way here rather than silently producing a collapsed object. */
    float sx = (float)mp_obj_get_float(args[1]), sy = (float)mp_obj_get_float(args[2]), sz = (float)mp_obj_get_float(args[3]);
    if (sx == 0.0f || sy == 0.0f || sz == 0.0f) mp_raise_ValueError(MP_ERROR_TEXT("phi.set_object_scale: scale components must be non-zero"));
    obj->scale.x = sx; obj->scale.y = sy; obj->scale.z = sz;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_set_object_scale_obj, 4, 4, native_set_object_scale);

/* ---- Object-id-keyed physics, exposed to Python (Phase 9 gap-closing,
 * 2026-08-18 -- see phi.md's Phase 9 "Known gaps": phi.enable_physics/
 * apply_impulse/get_velocity/set_velocity above all resolve through the
 * editor's "selected object" concept, which a shipped game has no
 * equivalent of at all -- so game/main.py could never touch physics.
 * These do the identical real work (same phi_physics.h calls, same
 * convex-hull-from-mesh-vertices shape choice) but take an object id
 * directly, same "id into scene_objects.c's registry" idiom phi.
 * get_vertices/set_vertices/the transform functions above already use.
 * phi_mp_register_physics_world below reuses s_phys_world (declared with
 * phi_mp_register_targets, at the top of this physics section) rather
 * than adding a second physics-world pointer to keep in sync -- the
 * editor already sets it via phi_mp_register_targets; player_main.c,
 * which has no selection to register, calls this instead. */

void phi_mp_register_physics_world(PhiPhysicsWorld *phys_world) {
    s_phys_world = phys_world;
}

static mp_obj_t native_object_enable_physics(size_t n_args, const mp_obj_t *args) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(args[0]));
    if (!obj) mp_raise_ValueError(MP_ERROR_TEXT("phi.object_enable_physics: no such object"));
    if (obj->phys_body) mp_raise_ValueError(MP_ERROR_TEXT("phi.object_enable_physics: already has a physics body"));
    if (!obj->hem || obj->hem->vert_count < 4) mp_raise_ValueError(MP_ERROR_TEXT("phi.object_enable_physics: not enough vertices for a hull (need a real 3D mesh)"));
    HalfEdgeMesh *hem = obj->hem;
    float *flat = (float *)malloc((size_t)hem->vert_count * 3 * sizeof(float));
    for (int i = 0; i < hem->vert_count; i++) {
        flat[i*3+0] = hem->verts[i].pos[0];
        flat[i*3+1] = hem->verts[i].pos[1];
        flat[i*3+2] = hem->verts[i].pos[2];
    }
    float orientation[4] = { obj->orientation.x, obj->orientation.y, obj->orientation.z, obj->orientation.w };
    float mass = (float)mp_obj_get_float(args[1]);
    float restitution = (float)mp_obj_get_float(args[2]);
    obj->phys_body = phi_physics_add_convex_hull_body(s_phys_world, flat, hem->vert_count,
                                                        obj->position, orientation, mass, restitution);
    free(flat);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_object_enable_physics_obj, 3, 3, native_object_enable_physics);

static mp_obj_t native_object_apply_impulse(size_t n_args, const mp_obj_t *args) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(args[0]));
    if (!obj || !obj->phys_body) mp_raise_ValueError(MP_ERROR_TEXT("phi.object_apply_impulse: no such object, or it has no physics body (call phi.object_enable_physics first)"));
    size_t n; mp_obj_t *items;
    mp_obj_get_array(args[1], &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.object_apply_impulse: expected a 3-element impulse vector"));
    Vec3f impulse = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    mp_obj_get_array(args[2], &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.object_apply_impulse: expected a 3-element rel_pos vector"));
    Vec3f rel_pos = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    phi_physics_apply_impulse(obj->phys_body, impulse, rel_pos);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_object_apply_impulse_obj, 3, 3, native_object_apply_impulse);

static mp_obj_t native_object_get_velocity(mp_obj_t id_obj) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(id_obj));
    if (!obj || !obj->phys_body) mp_raise_ValueError(MP_ERROR_TEXT("phi.object_get_velocity: no such object, or it has no physics body (call phi.object_enable_physics first)"));
    Vec3f v = phi_physics_get_linear_velocity(obj->phys_body);
    mp_obj_t items[3] = { mp_obj_new_float(v.x), mp_obj_new_float(v.y), mp_obj_new_float(v.z) };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_object_get_velocity_obj, native_object_get_velocity);

static mp_obj_t native_object_set_velocity(mp_obj_t id_obj, mp_obj_t v_obj) {
    MeshObject *obj = scene_object_find(mp_obj_get_int(id_obj));
    if (!obj || !obj->phys_body) mp_raise_ValueError(MP_ERROR_TEXT("phi.object_set_velocity: no such object, or it has no physics body (call phi.object_enable_physics first)"));
    size_t n; mp_obj_t *items;
    mp_obj_get_array(v_obj, &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.object_set_velocity: expected a 3-element velocity vector"));
    Vec3f v = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    phi_physics_set_linear_velocity(obj->phys_body, v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_object_set_velocity_obj, native_object_set_velocity);

/* ---- Keyboard/mouse input, exposed to Python (Phase 9 gap-closing,
 * 2026-08-18 -- see phi.md's Phase 9 "Known gaps": the player build had
 * NO input source at all before this -- input.c's own real, portable
 * PhiKey/keys_down state (see input.h) now exists specifically to close
 * this gap. Registered only by player_main.c, same "player-only, editor
 * drives its own UI directly off InputState already" reasoning phi_mp_
 * register_renderer above documents. */
static const InputState *s_input = NULL;

void phi_mp_register_input(const InputState *inp) {
    s_input = inp;
}

/* Name -> PhiKey, so Python scripts write phi.key_down('w') rather than
 * needing to know this enum's integer values -- same "string identifier,
 * not a raw enum int" ergonomics phi.add_light('sun', ...)/phi.prop_get
 * already establish. Single ASCII letters/digits map directly; anything
 * else is spelled out. Returns -1 for an unrecognized name. */
static int key_name_to_phikey(const char *name) {
    size_t len = strlen(name);
    if (len == 1) {
        if (name[0] >= 'a' && name[0] <= 'z') return PHI_KEY_A + (name[0] - 'a');
        if (name[0] >= 'A' && name[0] <= 'Z') return PHI_KEY_A + (name[0] - 'A');
        if (name[0] >= '0' && name[0] <= '9') return PHI_KEY_0 + (name[0] - '0');
        return -1;
    }
    if (strcmp(name, "space") == 0)  return PHI_KEY_SPACE;
    if (strcmp(name, "shift") == 0)  return PHI_KEY_SHIFT;
    if (strcmp(name, "ctrl") == 0)   return PHI_KEY_CTRL;
    if (strcmp(name, "up") == 0)     return PHI_KEY_UP;
    if (strcmp(name, "down") == 0)   return PHI_KEY_DOWN;
    if (strcmp(name, "left") == 0)   return PHI_KEY_LEFT;
    if (strcmp(name, "right") == 0)  return PHI_KEY_RIGHT;
    if (strcmp(name, "enter") == 0)  return PHI_KEY_ENTER;
    if (strcmp(name, "escape") == 0) return PHI_KEY_ESCAPE;
    if (strcmp(name, "tab") == 0)    return PHI_KEY_TAB;
    return -1;
}

static mp_obj_t native_key_down(mp_obj_t key_obj) {
    if (!s_input) mp_raise_ValueError(MP_ERROR_TEXT("phi.key_down: not available (no input registered)"));
    int pk = key_name_to_phikey(mp_obj_str_get_str(key_obj));
    if (pk < 0) mp_raise_ValueError(MP_ERROR_TEXT("phi.key_down: unrecognized key name"));
    return mp_obj_new_bool(s_input->keys_down[pk]);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_key_down_obj, native_key_down);

static mp_obj_t native_mouse_pos(void) {
    if (!s_input) mp_raise_ValueError(MP_ERROR_TEXT("phi.mouse_pos: not available (no input registered)"));
    mp_obj_t items[2] = { mp_obj_new_int(s_input->mouse_x), mp_obj_new_int(s_input->mouse_y) };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_mouse_pos_obj, native_mouse_pos);

static mp_obj_t native_mouse_button_down(mp_obj_t name_obj) {
    if (!s_input) mp_raise_ValueError(MP_ERROR_TEXT("phi.mouse_button_down: not available (no input registered)"));
    const char *name = mp_obj_str_get_str(name_obj);
    if (strcmp(name, "left") == 0)   return mp_obj_new_bool(s_input->lmb_down);
    if (strcmp(name, "right") == 0)  return mp_obj_new_bool(s_input->rmb_down);
    if (strcmp(name, "middle") == 0) return mp_obj_new_bool(s_input->mmb_down);
    mp_raise_ValueError(MP_ERROR_TEXT("phi.mouse_button_down: name must be 'left', 'right', or 'middle'"));
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_mouse_button_down_obj, native_mouse_button_down);

/* ---- Gamepad, exposed to Python (Phase 9 gap-closing, 2026-08-18 -- see
 * phi.md's Phase 9 "Known gaps": input_gamepad.h was real and wired for
 * C (game/src/*.c via phi.h) but had no Python binding at all).
 * Function-pointer handoff, NOT calling phi_gamepad_count/phi_gamepad_
 * get_state directly -- see mp_port.h's phi_mp_register_gamepad_
 * callbacks comment for why (input_gamepad_native.c pulls in the entire
 * vendored SDL2 tree; mp_geometry_test/mp_node_test must never need that
 * linked in just to test unrelated bindings). player_main.c hands over
 * those exact two real functions by pointer. */
static int                     (*s_gamepad_count_cb)(void) = NULL;
static const PhiGamepadState  *(*s_gamepad_get_state_cb)(int index) = NULL;

void phi_mp_register_gamepad_callbacks(int (*count)(void), const PhiGamepadState *(*get_state)(int index)) {
    s_gamepad_count_cb = count;
    s_gamepad_get_state_cb = get_state;
}

static int gamepad_button_name_to_enum(const char *name) {
    if (strcmp(name, "a") == 0) return PHI_GAMEPAD_BUTTON_A;
    if (strcmp(name, "b") == 0) return PHI_GAMEPAD_BUTTON_B;
    if (strcmp(name, "x") == 0) return PHI_GAMEPAD_BUTTON_X;
    if (strcmp(name, "y") == 0) return PHI_GAMEPAD_BUTTON_Y;
    if (strcmp(name, "back") == 0)  return PHI_GAMEPAD_BUTTON_BACK;
    if (strcmp(name, "guide") == 0) return PHI_GAMEPAD_BUTTON_GUIDE;
    if (strcmp(name, "start") == 0) return PHI_GAMEPAD_BUTTON_START;
    if (strcmp(name, "leftstick") == 0)  return PHI_GAMEPAD_BUTTON_LEFTSTICK;
    if (strcmp(name, "rightstick") == 0) return PHI_GAMEPAD_BUTTON_RIGHTSTICK;
    if (strcmp(name, "leftshoulder") == 0)  return PHI_GAMEPAD_BUTTON_LEFTSHOULDER;
    if (strcmp(name, "rightshoulder") == 0) return PHI_GAMEPAD_BUTTON_RIGHTSHOULDER;
    if (strcmp(name, "dpad_up") == 0)    return PHI_GAMEPAD_BUTTON_DPAD_UP;
    if (strcmp(name, "dpad_down") == 0)  return PHI_GAMEPAD_BUTTON_DPAD_DOWN;
    if (strcmp(name, "dpad_left") == 0)  return PHI_GAMEPAD_BUTTON_DPAD_LEFT;
    if (strcmp(name, "dpad_right") == 0) return PHI_GAMEPAD_BUTTON_DPAD_RIGHT;
    return -1;
}

static int gamepad_axis_name_to_enum(const char *name) {
    if (strcmp(name, "leftx") == 0)  return PHI_GAMEPAD_AXIS_LEFTX;
    if (strcmp(name, "lefty") == 0)  return PHI_GAMEPAD_AXIS_LEFTY;
    if (strcmp(name, "rightx") == 0) return PHI_GAMEPAD_AXIS_RIGHTX;
    if (strcmp(name, "righty") == 0) return PHI_GAMEPAD_AXIS_RIGHTY;
    if (strcmp(name, "lefttrigger") == 0)  return PHI_GAMEPAD_AXIS_LEFT_TRIGGER;
    if (strcmp(name, "righttrigger") == 0) return PHI_GAMEPAD_AXIS_RIGHT_TRIGGER;
    return -1;
}

static mp_obj_t native_gamepad_count(void) {
    return mp_obj_new_int(s_gamepad_count_cb ? s_gamepad_count_cb() : 0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_gamepad_count_obj, native_gamepad_count);

static mp_obj_t native_gamepad_connected(mp_obj_t index_obj) {
    const PhiGamepadState *s = s_gamepad_get_state_cb ? s_gamepad_get_state_cb(mp_obj_get_int(index_obj)) : NULL;
    return mp_obj_new_bool(s && s->connected);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_gamepad_connected_obj, native_gamepad_connected);

static mp_obj_t native_gamepad_button(mp_obj_t index_obj, mp_obj_t button_obj) {
    const PhiGamepadState *s = s_gamepad_get_state_cb ? s_gamepad_get_state_cb(mp_obj_get_int(index_obj)) : NULL;
    if (!s || !s->connected) return mp_obj_new_bool(false);
    int btn = gamepad_button_name_to_enum(mp_obj_str_get_str(button_obj));
    if (btn < 0) mp_raise_ValueError(MP_ERROR_TEXT("phi.gamepad_button: unrecognized button name"));
    return mp_obj_new_bool(s->buttons[btn]);
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_gamepad_button_obj, native_gamepad_button);

static mp_obj_t native_gamepad_axis(mp_obj_t index_obj, mp_obj_t axis_obj) {
    const PhiGamepadState *s = s_gamepad_get_state_cb ? s_gamepad_get_state_cb(mp_obj_get_int(index_obj)) : NULL;
    if (!s || !s->connected) return mp_obj_new_float(0.0f);
    int axis = gamepad_axis_name_to_enum(mp_obj_str_get_str(axis_obj));
    if (axis < 0) mp_raise_ValueError(MP_ERROR_TEXT("phi.gamepad_axis: unrecognized axis name"));
    return mp_obj_new_float((mp_float_t)s->axes[axis]);
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_gamepad_axis_obj, native_gamepad_axis);

/* ---- Audio, exposed to Python (Phase 10, see phi.md's "Phase 10 --
 * Audio" -- a genuinely unaddressed system before this, not a gap in an
 * existing phase). Function-pointer handoff for the same reason camera/
 * gamepad above use one: the real backend (audio_native.c, which may
 * pull in ALSA; audio_wasm.c; audio_win32_stub.c) must never need to be
 * linked into mp_geometry_test/mp_node_test/mp_phase9_gap_test just to
 * exercise unrelated bindings. Loaded sounds are tracked by a small
 * int-handle registry here (s_loaded_sounds), the same "id, not a raw
 * pointer, crosses into Python" idiom every other phi.* binding in this
 * file already uses (scene object ids, light ids, graph ids) -- a
 * PhiSound* is never itself visible to a Python script. */
static PhiSound       *(*s_audio_load_cb)(const char *path) = NULL;
static PhiAudioVoice    (*s_audio_play_cb)(PhiSound *sound, float volume, int loop) = NULL;
static PhiAudioVoice    (*s_audio_play_3d_cb)(PhiSound *sound, Vec3f position, float volume, int loop) = NULL;
static void             (*s_audio_stop_cb)(PhiAudioVoice voice) = NULL;

void phi_mp_register_audio_callbacks(
    PhiSound *(*load_sound)(const char *path),
    PhiAudioVoice (*play)(PhiSound *sound, float volume, int loop),
    PhiAudioVoice (*play_3d)(PhiSound *sound, Vec3f position, float volume, int loop),
    void (*stop)(PhiAudioVoice voice)
) {
    s_audio_load_cb = load_sound;
    s_audio_play_cb = play;
    s_audio_play_3d_cb = play_3d;
    s_audio_stop_cb = stop;
}

#define PHI_MP_MAX_LOADED_SOUNDS 64
static PhiSound *s_loaded_sounds[PHI_MP_MAX_LOADED_SOUNDS];
static int       s_loaded_sound_count = 0;

static mp_obj_t native_load_sound(mp_obj_t path_obj) {
    if (!s_audio_load_cb) mp_raise_ValueError(MP_ERROR_TEXT("phi.load_sound: not available (no audio backend registered)"));
    if (s_loaded_sound_count >= PHI_MP_MAX_LOADED_SOUNDS) mp_raise_ValueError(MP_ERROR_TEXT("phi.load_sound: too many sounds already loaded (PHI_MP_MAX_LOADED_SOUNDS)"));
    PhiSound *s = s_audio_load_cb(mp_obj_str_get_str(path_obj));
    if (!s) mp_raise_ValueError(MP_ERROR_TEXT("phi.load_sound: failed to load (missing file, not a WAV, or an unsupported PCM layout)"));
    int handle = s_loaded_sound_count++;
    s_loaded_sounds[handle] = s;
    return mp_obj_new_int(handle);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_load_sound_obj, native_load_sound);

static PhiSound *resolve_sound(int handle) {
    if (handle < 0 || handle >= s_loaded_sound_count) return NULL;
    return s_loaded_sounds[handle];
}

static mp_obj_t native_play_sound(size_t n_args, const mp_obj_t *args) {
    PhiSound *s = resolve_sound(mp_obj_get_int(args[0]));
    if (!s) mp_raise_ValueError(MP_ERROR_TEXT("phi.play_sound: no such loaded sound handle"));
    if (!s_audio_play_cb) mp_raise_ValueError(MP_ERROR_TEXT("phi.play_sound: not available (no audio backend registered)"));
    float volume = n_args > 1 ? (float)mp_obj_get_float(args[1]) : 1.0f;
    int loop = n_args > 2 ? mp_obj_is_true(args[2]) : 0;
    return mp_obj_new_int(s_audio_play_cb(s, volume, loop));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_play_sound_obj, 1, 3, native_play_sound);

static mp_obj_t native_play_sound_3d(size_t n_args, const mp_obj_t *args) {
    PhiSound *s = resolve_sound(mp_obj_get_int(args[0]));
    if (!s) mp_raise_ValueError(MP_ERROR_TEXT("phi.play_sound_3d: no such loaded sound handle"));
    if (!s_audio_play_3d_cb) mp_raise_ValueError(MP_ERROR_TEXT("phi.play_sound_3d: not available (no audio backend registered)"));
    Vec3f pos = { (float)mp_obj_get_float(args[1]), (float)mp_obj_get_float(args[2]), (float)mp_obj_get_float(args[3]) };
    float volume = n_args > 4 ? (float)mp_obj_get_float(args[4]) : 1.0f;
    int loop = n_args > 5 ? mp_obj_is_true(args[5]) : 0;
    return mp_obj_new_int(s_audio_play_3d_cb(s, pos, volume, loop));
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_play_sound_3d_obj, 4, 6, native_play_sound_3d);

static mp_obj_t native_stop_sound(mp_obj_t voice_obj) {
    if (s_audio_stop_cb) s_audio_stop_cb(mp_obj_get_int(voice_obj));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_stop_sound_obj, native_stop_sound);

/* ---- Light objects, exposed to Python (light.h) -- spawn/delete/list
 * live outside the generic phi.prop_get/set surface since they add or
 * remove a whole light rather than reading/writing one of an existing
 * light's fields (phi.prop_get/set("light:<id>", ...) still covers that
 * part, via scene_target.c's resolver). ---- */

static mp_obj_t native_add_light(size_t n_args, const mp_obj_t *args) {
    const char *type_str = mp_obj_str_get_str(args[0]);
    LightType type;
    if (strcmp(type_str, "point") == 0) type = LIGHT_TYPE_POINT;
    else if (strcmp(type_str, "sun") == 0) type = LIGHT_TYPE_SUN;
    else if (strcmp(type_str, "spot") == 0) type = LIGHT_TYPE_SPOT;
    else if (strcmp(type_str, "area") == 0) type = LIGHT_TYPE_AREA;
    else mp_raise_ValueError(MP_ERROR_TEXT("phi.add_light: type must be 'point', 'sun', 'spot', or 'area'"));

    Vec3f pos = {
        (float)mp_obj_get_float(args[1]), (float)mp_obj_get_float(args[2]), (float)mp_obj_get_float(args[3])
    };
    PhiLight *l = light_spawn(type, pos);
    if (!l) mp_raise_ValueError(MP_ERROR_TEXT("phi.add_light: light registry is full"));
    return mp_obj_new_int(l->id);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_add_light_obj, 4, 4, native_add_light);

static mp_obj_t native_delete_light(mp_obj_t id_obj) {
    int id = mp_obj_get_int(id_obj);
    if (!light_delete(id)) mp_raise_ValueError(MP_ERROR_TEXT("phi.delete_light: no light with that id"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_delete_light_obj, native_delete_light);

static mp_obj_t native_list_lights(void) {
    PhiLight *lights[PHI_MAX_LIGHTS];
    int n = light_get_all(lights);
    mp_obj_t items[PHI_MAX_LIGHTS];
    for (int i = 0; i < n; i++) items[i] = mp_obj_new_int(lights[i]->id);
    return mp_obj_new_tuple((size_t)n, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_list_lights_obj, native_list_lights);

/* ---- Phase 5's real multi-object scene graph, exposed to Python (see
 * scene_objects.h) -- geometry CREATION and vertex EDITING, the actual
 * load-bearing ask this phase was built for ("make it so Claude can
 * create geometry and redefine the verts of existing scene geometry via
 * the Python API"). Calls scene_objects.c/halfedge.c/halfedge_gltf.c
 * directly, no main.c indirection needed -- same shape phi.add_light/
 * delete_light/list_lights above already established for light.c's own
 * self-contained registry (scene_objects.c is exactly that kind of
 * module too, unlike phi.render()/activate_ragdoll(), which genuinely
 * need a callback into main.c-only state like g_cam_pos/g_skinned_test_
 * obj).
 *
 * A RenderMesh is allocated directly (`calloc`) rather than via octree_
 * render.c's mesh_create() -- deliberately, so this file's translation
 * unit never needs to link that GL-touching file (mesh_create/mesh_
 * upload_stride/mesh_draw all live together there, see fracture_body.c's
 * own make_fragment_render_mesh for the identical reasoning) -- every
 * existing no-GL mp_* standalone test would otherwise start needing a
 * real GL context just because mp_port.c got recompiled, a real
 * regression this avoids by construction, not by luck. meshobject_
 * build_render_mesh_from_halfedge itself is already GL-free (plain
 * malloc/realloc, verified by this project's own earlier sessions using
 * it from no-GL test harnesses directly). ---- */

static mp_obj_t native_mesh_object(size_t n_args, const mp_obj_t *args) {
    const char *path = mp_obj_str_get_str(args[0]);
    float x = n_args > 1 ? (float)mp_obj_get_float(args[1]) : 0.0f;
    float y = n_args > 2 ? (float)mp_obj_get_float(args[2]) : 0.0f;
    float z = n_args > 3 ? (float)mp_obj_get_float(args[3]) : 0.0f;

    HalfEdgeMesh *hem = halfedge_load_gltf(path);
    if (!hem) mp_raise_ValueError(MP_ERROR_TEXT("phi.mesh_object: failed to load the file (bad path, or not a valid glTF/GLB)"));
    MeshObject *obj = scene_object_add();
    if (!obj) {
        halfedge_destroy(hem);
        mp_raise_ValueError(MP_ERROR_TEXT("phi.mesh_object: scene is full (SCENE_MAX_OBJECTS)"));
    }
    obj->position = (Vec3f){x, y, z};
    obj->orientation = quat_identity();
    obj->scale = (Vec3f){1.0f, 1.0f, 1.0f};
    obj->is_static = 1;
    obj->render_mesh = (RenderMesh *)calloc(1, sizeof(RenderMesh));
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, hem);
    obj->hem = hem;
    return mp_obj_new_int(obj->id);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_mesh_object_obj, 1, 4, native_mesh_object);

/* Builds a real MeshObject from scratch -- no file, no pre-authored
 * asset -- out of a flat (x,y,z,x,y,z,...) position sequence and a flat
 * (triangle-list) index sequence, via halfedge_build_from_triangles
 * (already existed, this is its first Python-reachable caller). Indices
 * are validated against the actual vertex count before being narrowed to
 * uint16_t (this codebase's existing index-width convention throughout
 * -- RenderMesh/SkinnedMesh both already use it), so a bad index raises
 * a real Python exception instead of silently wrapping or reading past
 * the position array. */
static mp_obj_t native_create_mesh(size_t n_args, const mp_obj_t *args) {
    size_t n_pos; mp_obj_t *pos_items;
    mp_obj_get_array(args[0], &n_pos, &pos_items);
    if (n_pos == 0 || n_pos % 3 != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.create_mesh: positions must be a flat sequence of x,y,z triples"));
    }
    size_t n_idx; mp_obj_t *idx_items;
    mp_obj_get_array(args[1], &n_idx, &idx_items);
    if (n_idx == 0 || n_idx % 3 != 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.create_mesh: indices must be a flat sequence of triangle triples"));
    }

    int pos_count = (int)(n_pos / 3);
    float *positions = (float *)malloc(n_pos * sizeof(float));
    for (size_t i = 0; i < n_pos; i++) positions[i] = (float)mp_obj_get_float(pos_items[i]);

    unsigned short *indices = (unsigned short *)malloc(n_idx * sizeof(unsigned short));
    for (size_t i = 0; i < n_idx; i++) {
        int v = mp_obj_get_int(idx_items[i]);
        if (v < 0 || v >= pos_count) {
            free(positions); free(indices);
            mp_raise_ValueError(MP_ERROR_TEXT("phi.create_mesh: an index is out of range for the given positions"));
        }
        indices[i] = (unsigned short)v;
    }

    HalfEdgeMesh *hem = halfedge_build_from_triangles(positions, pos_count, indices, (int)n_idx);
    free(positions);
    free(indices);
    if (!hem) mp_raise_ValueError(MP_ERROR_TEXT("phi.create_mesh: failed to build mesh topology"));

    MeshObject *obj = scene_object_add();
    if (!obj) {
        halfedge_destroy(hem);
        mp_raise_ValueError(MP_ERROR_TEXT("phi.create_mesh: scene is full (SCENE_MAX_OBJECTS)"));
    }
    obj->position = (Vec3f){
        n_args > 2 ? (float)mp_obj_get_float(args[2]) : 0.0f,
        n_args > 3 ? (float)mp_obj_get_float(args[3]) : 0.0f,
        n_args > 4 ? (float)mp_obj_get_float(args[4]) : 0.0f
    };
    obj->orientation = quat_identity();
    obj->scale = (Vec3f){1.0f, 1.0f, 1.0f};
    obj->is_static = 1;
    obj->render_mesh = (RenderMesh *)calloc(1, sizeof(RenderMesh));
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, hem);
    obj->hem = hem;
    return mp_obj_new_int(obj->id);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_create_mesh_obj, 2, 5, native_create_mesh);

/* Redefines EVERY vertex position of an existing object's live geometry
 * in one call -- "redefine the verts of existing scene geometry", the
 * explicit second half of this phase's ask. Same vertex COUNT as
 * whatever's already there (this rewrites positions, not topology --
 * phi.create_mesh is the way to build genuinely different topology); a
 * mismatched count raises rather than silently truncating/leaving
 * trailing vertices untouched. Re-flattens the render mesh afterward
 * (same rebuild step main.c's own mesh_edit context-menu actions already
 * do after extrude/inset/loop-cut) so the visible geometry updates
 * immediately, not just the underlying half-edge data. Does NOT update
 * phys_body's collision shape if the object has one -- a real, honest
 * scope limit (regenerating a convex hull after an arbitrary vertex edit
 * is real future work, not attempted this pass); the shape simply goes
 * stale relative to the new visual geometry until re-enabled. */
static mp_obj_t native_set_vertices(mp_obj_t id_obj, mp_obj_t positions_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.set_vertices: no such object (or it has no editable geometry)"));
    }
    size_t n; mp_obj_t *items;
    mp_obj_get_array(positions_obj, &n, &items);
    if (n % 3 != 0 || (int)(n / 3) != obj->hem->vert_count) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.set_vertices: expected exactly 3*vert_count floats, same vertex count as the existing mesh (use phi.create_mesh to change topology)"));
    }
    for (int i = 0; i < obj->hem->vert_count; i++) {
        obj->hem->verts[i].pos[0] = (float)mp_obj_get_float(items[i*3+0]);
        obj->hem->verts[i].pos[1] = (float)mp_obj_get_float(items[i*3+1]);
        obj->hem->verts[i].pos[2] = (float)mp_obj_get_float(items[i*3+2]);
    }
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_set_vertices_obj, native_set_vertices);

/* Single-vertex variant of the above -- cheaper for a small, targeted
 * edit than rebuilding the whole positions list in Python first just to
 * change one vertex. */
static mp_obj_t native_set_vertex(size_t n_args, const mp_obj_t *args) {
    int id = mp_obj_get_int(args[0]);
    int index = mp_obj_get_int(args[1]);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.set_vertex: no such object (or it has no editable geometry)"));
    }
    if (index < 0 || index >= obj->hem->vert_count) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.set_vertex: vertex index out of range"));
    }
    obj->hem->verts[index].pos[0] = (float)mp_obj_get_float(args[2]);
    obj->hem->verts[index].pos[1] = (float)mp_obj_get_float(args[3]);
    obj->hem->verts[index].pos[2] = (float)mp_obj_get_float(args[4]);
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_set_vertex_obj, 5, 5, native_set_vertex);

static mp_obj_t native_list_objects(void) {
    MeshObject *objects[SCENE_MAX_OBJECTS];
    int n = scene_object_get_all(objects);
    mp_obj_t items[SCENE_MAX_OBJECTS];
    for (int i = 0; i < n; i++) items[i] = mp_obj_new_int(objects[i]->id);
    return mp_obj_new_tuple((size_t)n, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_list_objects_obj, native_list_objects);

static mp_obj_t native_delete_object(mp_obj_t id_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj) mp_raise_ValueError(MP_ERROR_TEXT("phi.delete_object: no object with that id"));
    /* s_phys_world (already registered by phi_mp_register_targets, used
     * by enable_physics/etc. above) -- passed through so a live phys_
     * body (if this object ever got one, e.g. via the selected-object
     * phi.enable_physics path) is actually removed from the simulation,
     * not just leaked/left dangling in Bullet's world while this
     * MeshObject's own slot gets reused (see scene_object_delete's own
     * contract: NULL here would silently skip that cleanup). */
    scene_object_delete(obj, s_phys_world);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_delete_object_obj, native_delete_object);

/* ---- Incremental mesh editing, exposed to Python -- adding vertices/
 * faces to EXISTING geometry (create_mesh/set_vertices above only cover
 * from-scratch build and same-topology-rewrite), reading back current
 * geometry (nothing above lets a script see what's already there),
 * flipping winding, and mesh_edit.c's own extrude/inset/loop-cut
 * operations -- already used by the right-click context menu, but not
 * Python-reachable until now. Together these are what let a script (or
 * Claude driving one) reshape a mesh into new topology one edit at a
 * time, not just replace it wholesale. ---- */

static mp_obj_t native_get_vertices(mp_obj_t id_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.get_vertices: no such object (or it has no editable geometry)"));
    }
    HalfEdgeMesh *hem = obj->hem;
    mp_obj_t *items = (mp_obj_t *)malloc((size_t)hem->vert_count * 3 * sizeof(mp_obj_t));
    for (int i = 0; i < hem->vert_count; i++) {
        items[i*3+0] = mp_obj_new_float(hem->verts[i].pos[0]);
        items[i*3+1] = mp_obj_new_float(hem->verts[i].pos[1]);
        items[i*3+2] = mp_obj_new_float(hem->verts[i].pos[2]);
    }
    mp_obj_t result = mp_obj_new_tuple((size_t)hem->vert_count * 3, items);
    free(items);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_get_vertices_obj, native_get_vertices);

/* One (face_index, (v0,v1,...)) pair per LIVE face (deleted/tombstoned
 * faces are skipped, same as halfedge_flatten_triangles does for
 * rendering). face_index is included explicitly -- NOT just this face's
 * position in the returned tuple -- because tombstoning leaves gaps in
 * hem->faces; position-in-list only equals real face index when nothing
 * has ever been deleted. delete_face/extrude_face/inset_face all need the
 * real index, so a script enumerating faces has to be able to get it back
 * out of get_faces() rather than reconstruct it. n-gon aware via
 * HEFace.count even though every live face is a triangle in this codebase
 * today (see halfedge.h). */
static mp_obj_t native_get_faces(mp_obj_t id_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.get_faces: no such object (or it has no editable geometry)"));
    }
    HalfEdgeMesh *hem = obj->hem;
    mp_obj_t *faces = (mp_obj_t *)malloc((size_t)(hem->face_count > 0 ? hem->face_count : 1) * sizeof(mp_obj_t));
    int live_count = 0;
    for (int f = 0; f < hem->face_count; f++) {
        if (hem->faces[f].deleted) continue;
        int n = hem->faces[f].count;
        int verts[64];
        if (n > 64) n = 64;   /* defensive -- triangles only today, well under this */
        halfedge_face_verts(hem, f, verts);
        mp_obj_t v_items[64];
        for (int i = 0; i < n; i++) v_items[i] = mp_obj_new_int(verts[i]);
        mp_obj_t pair[2] = { mp_obj_new_int(f), mp_obj_new_tuple((size_t)n, v_items) };
        faces[live_count++] = mp_obj_new_tuple(2, pair);
    }
    mp_obj_t result = mp_obj_new_tuple((size_t)live_count, faces);
    free(faces);
    return result;
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_get_faces_obj, native_get_faces);

/* Adding a lone vertex doesn't change anything a render-mesh rebuild would
 * pick up (halfedge_flatten_triangles/meshobject_build_render_mesh_from_
 * halfedge only ever walk LIVE FACES, never unreferenced vertices) --
 * so unlike add_face/delete_face/flip_normals below, this one skips the
 * rebuild; there's nothing to redraw until the new vertex is actually
 * used in a face. */
static mp_obj_t native_add_vertex(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int id = mp_obj_get_int(args[0]);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.add_vertex: no such object (or it has no editable geometry)"));
    }
    float x = (float)mp_obj_get_float(args[1]);
    float y = (float)mp_obj_get_float(args[2]);
    float z = (float)mp_obj_get_float(args[3]);
    int idx = halfedge_add_vertex(obj->hem, x, y, z);
    return mp_obj_new_int(idx);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_add_vertex_obj, 4, 4, native_add_vertex);

/* Exactly 3 indices -- this codebase's editable meshes are triangles-only
 * for now (see halfedge.h/mesh_edit.h); halfedge_flatten_triangles assumes
 * every LIVE face already is one, so accepting an n-gon here would corrupt
 * the next render-mesh rebuild rather than fail loudly. Indices are
 * bounds-checked here because halfedge_add_face itself does not -- an
 * out-of-range index writes straight past hem->verts (see its own
 * implementation), the same reason native_create_mesh above validates
 * before calling halfedge_build_from_triangles. */
static mp_obj_t native_add_face(mp_obj_t id_obj, mp_obj_t indices_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.add_face: no such object (or it has no editable geometry)"));
    }
    size_t n; mp_obj_t *items;
    mp_obj_get_array(indices_obj, &n, &items);
    if (n != 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.add_face: must be exactly 3 vertex indices (this engine's editable meshes are triangles-only for now)"));
    }
    int verts[3];
    for (size_t i = 0; i < n; i++) {
        int v = mp_obj_get_int(items[i]);
        if (v < 0 || v >= obj->hem->vert_count) {
            mp_raise_ValueError(MP_ERROR_TEXT("phi.add_face: a vertex index is out of range"));
        }
        verts[i] = v;
    }
    int f = halfedge_add_face(obj->hem, verts, 3);
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    return mp_obj_new_int(f);
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_add_face_obj, native_add_face);

static mp_obj_t native_delete_face(mp_obj_t id_obj, mp_obj_t face_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.delete_face: no such object (or it has no editable geometry)"));
    }
    int f = mp_obj_get_int(face_obj);
    if (f < 0 || f >= obj->hem->face_count || obj->hem->faces[f].deleted) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.delete_face: no such face (out of range, or already deleted)"));
    }
    halfedge_delete_face(obj->hem, f);
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_delete_face_obj, native_delete_face);

/* Sets an ARBITRARY face's real PBR material (halfedge_set_face_material,
 * see halfedge.h) -- distinct from phi.prop_set('face', ...), which only
 * ever targets the currently UI-selected face (see scene_target.c) and
 * can't be driven by a script pointing at a specific object_id+face_index
 * it already knows. Built for Phase 6's shader nodes (principled_bsdf/
 * emission, see PHI_BOOTSTRAP below) to have something real to apply
 * their computed material to even with no node-graph editor panel yet --
 * without this, a shader graph's result could be computed but never
 * actually reach real geometry. halfedge_set_face_material itself
 * silently no-ops on a bad face index; validated here instead so a bad
 * call raises a real ValueError rather than doing nothing observably. */
static mp_obj_t native_set_face_material(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int id = mp_obj_get_int(args[0]);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.set_face_material: no such object (or it has no editable geometry)"));
    }
    int f = mp_obj_get_int(args[1]);
    if (f < 0 || f >= obj->hem->face_count || obj->hem->faces[f].deleted) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.set_face_material: no such face (out of range, or already deleted)"));
    }
    size_t n; mp_obj_t *items;
    mp_obj_get_array(args[2], &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.set_face_material: base_color must be a 3-element [r,g,b] sequence"));
    float base_color[3] = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    float metallic = (float)mp_obj_get_float(args[3]);
    float roughness = (float)mp_obj_get_float(args[4]);
    mp_obj_get_array(args[5], &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.set_face_material: emission must be a 3-element [r,g,b] sequence"));
    float emission[3] = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    halfedge_set_face_material(obj->hem, f, base_color, metallic, roughness, emission);
    /* Material is baked per-vertex into MESHOBJ_VERTEX_STRIDE at flatten
     * time, not sampled from HEFace at draw time -- rebuild so the change
     * is actually visible, same reasoning every other geometry-mutating
     * binding in this file already follows. */
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_set_face_material_obj, 6, 6, native_set_face_material);

/* Whole-object winding flip (mesh_edit_flip_normals, see mesh_edit.h for
 * why this is the safe granularity -- flipping a single face's winding
 * without its neighbors breaks half-edge twin consistency). Returns the
 * number of faces flipped, so a script can tell the call actually did
 * something. */
static mp_obj_t native_flip_normals(mp_obj_t id_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.flip_normals: no such object (or it has no editable geometry)"));
    }
    int n = mesh_edit_flip_normals(obj->hem);
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    return mp_obj_new_int(n);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_flip_normals_obj, native_flip_normals);

static mp_obj_t native_extrude_face(mp_obj_t id_obj, mp_obj_t face_obj, mp_obj_t dist_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.extrude_face: no such object (or it has no editable geometry)"));
    }
    int f = mp_obj_get_int(face_obj);
    float dist = (float)mp_obj_get_float(dist_obj);
    int newf = mesh_edit_extrude_face(obj->hem, f, dist);
    if (newf < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.extrude_face: failed (out-of-range/deleted face, or not a triangle)"));
    }
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    return mp_obj_new_int(newf);
}
static MP_DEFINE_CONST_FUN_OBJ_3(native_extrude_face_obj, native_extrude_face);

static mp_obj_t native_inset_face(mp_obj_t id_obj, mp_obj_t face_obj, mp_obj_t factor_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.inset_face: no such object (or it has no editable geometry)"));
    }
    int f = mp_obj_get_int(face_obj);
    float factor = (float)mp_obj_get_float(factor_obj);
    int newf = mesh_edit_inset_face(obj->hem, f, factor);
    if (newf < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.inset_face: failed (out-of-range/deleted face, or not a triangle)"));
    }
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    return mp_obj_new_int(newf);
}
static MP_DEFINE_CONST_FUN_OBJ_3(native_inset_face_obj, native_inset_face);

static mp_obj_t native_loop_cut(mp_obj_t id_obj, mp_obj_t edge_obj) {
    int id = mp_obj_get_int(id_obj);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.loop_cut: no such object (or it has no editable geometry)"));
    }
    int e = mp_obj_get_int(edge_obj);
    int mv = mesh_edit_loop_cut_edge(obj->hem, e);
    if (mv < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.loop_cut: failed (out-of-range edge, or its face isn't a live triangle)"));
    }
    meshobject_build_render_mesh_from_halfedge(obj->render_mesh, obj->hem);
    return mp_obj_new_int(mv);
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_loop_cut_obj, native_loop_cut);

/* Companion to loop_cut above -- loop_cut takes an EDGE index, but nothing
 * exposed to Python enumerates edges at all (get_faces only gives vertex
 * indices per face). Without this, loop_cut would be uncallable from a
 * script except by guessing indices. Mirrors exactly how the interactive
 * right-click loop-cut tool itself resolves a pick: a face + an
 * approximate 3D point (e.g. the midpoint of the edge a script wants) maps
 * to the real edge index whose own midpoint is nearest. */
static mp_obj_t native_nearest_edge_of_face(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int id = mp_obj_get_int(args[0]);
    MeshObject *obj = scene_object_find(id);
    if (!obj || !obj->hem) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.nearest_edge_of_face: no such object (or it has no editable geometry)"));
    }
    int f = mp_obj_get_int(args[1]);
    float x = (float)mp_obj_get_float(args[2]);
    float y = (float)mp_obj_get_float(args[3]);
    float z = (float)mp_obj_get_float(args[4]);
    int e = mesh_edit_nearest_edge_of_face(obj->hem, f, x, y, z);
    if (e < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.nearest_edge_of_face: no such face (out of range or deleted)"));
    }
    return mp_obj_new_int(e);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_nearest_edge_of_face_obj, 5, 5, native_nearest_edge_of_face);

/* Phase 3's real offline path tracer (path_tracer.h), triggered from
 * Python -- main.c owns the actual scene-collection/pt_render/pt_write_
 * png logic (render_still_frame_to_disk), registered here as a plain
 * function pointer rather than duplicated, same shape phi_mp_register_
 * targets already established for cross-file state main.c owns. */
static int (*s_render_still_frame)(char *out_path, size_t cap) = NULL;

void phi_mp_register_render_callback(int (*cb)(char *out_path, size_t cap)) {
    s_render_still_frame = cb;
}

static mp_obj_t native_render(void) {
    if (!s_render_still_frame) mp_raise_ValueError(MP_ERROR_TEXT("phi.render: not available yet"));
    char path[256];
    if (!s_render_still_frame(path, sizeof(path))) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.render: render failed (empty scene, or the PNG write failed)"));
    }
    return mp_obj_new_str(path, strlen(path));
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_render_obj, native_render);

/* Phase 4's Armature -> Bullet ragdoll handoff (ragdoll.h), same function-
 * pointer-registration shape as phi.render() just above. */
static int (*s_activate_ragdoll)(void) = NULL;

void phi_mp_register_ragdoll_callback(int (*cb)(void)) {
    s_activate_ragdoll = cb;
}

static mp_obj_t native_activate_ragdoll(void) {
    if (!s_activate_ragdoll) mp_raise_ValueError(MP_ERROR_TEXT("phi.activate_ragdoll: not available yet"));
    return mp_obj_new_int(s_activate_ragdoll());
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_activate_ragdoll_obj, native_activate_ragdoll);

/* ---- Animation playback control, exposed to Python (see mp_port.h's
 * phi_mp_register_animation_callbacks) -- operates on main.c's single
 * g_skinned_test_obj slot, same function-pointer-handoff shape phi.
 * render()/phi.activate_ragdoll() above already use. Real gap this
 * closes: real C-side Armature/AnimClip/Playback/GPU skinning existed
 * with zero Python bindings before this. ---- */
static int   (*s_anim_play)(const char *clip_name, int loop) = NULL;
static void  (*s_anim_set_playing)(int playing) = NULL;
static int   (*s_anim_get_playing)(void) = NULL;
static float (*s_anim_get_time)(void) = NULL;
static int   (*s_anim_set_time)(float t) = NULL;
static int   (*s_anim_list_clips)(char out_names[][64], float *out_durations, int max_clips) = NULL;

void phi_mp_register_animation_callbacks(
    int   (*play)(const char *clip_name, int loop),
    void  (*set_playing)(int playing),
    int   (*get_playing)(void),
    float (*get_time)(void),
    int   (*set_time)(float t),
    int   (*list_clips)(char out_names[][64], float *out_durations, int max_clips)
) {
    s_anim_play = play;
    s_anim_set_playing = set_playing;
    s_anim_get_playing = get_playing;
    s_anim_get_time = get_time;
    s_anim_set_time = set_time;
    s_anim_list_clips = list_clips;
}

/* clip_name defaults to None (picks clip 0 if any exist), loop defaults
 * to True -- native, not a Python-level default-arg wrapper, matching
 * native_mesh_object/native_create_mesh's own optional-trailing-args
 * convention elsewhere in this file. */
static mp_obj_t native_play_animation(size_t n_args, const mp_obj_t *args) {
    const char *name = NULL;
    int loop = 1;
    if (n_args > 0 && args[0] != mp_const_none) name = mp_obj_str_get_str(args[0]);
    if (n_args > 1) loop = mp_obj_is_true(args[1]);
    if (!s_anim_play) mp_raise_ValueError(MP_ERROR_TEXT("phi.play_animation: not available yet"));
    if (!s_anim_play(name, loop)) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.play_animation: no skinned object loaded, or no clip with that name (see phi.list_animation_clips)"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_play_animation_obj, 0, 2, native_play_animation);

static mp_obj_t native_pause_animation(void) {
    if (s_anim_set_playing) s_anim_set_playing(0);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_pause_animation_obj, native_pause_animation);

static mp_obj_t native_resume_animation(void) {
    if (s_anim_set_playing) s_anim_set_playing(1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_resume_animation_obj, native_resume_animation);

static mp_obj_t native_is_animation_playing(void) {
    return (s_anim_get_playing && s_anim_get_playing()) ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_is_animation_playing_obj, native_is_animation_playing);

static mp_obj_t native_get_animation_time(void) {
    return mp_obj_new_float(s_anim_get_time ? (mp_float_t)s_anim_get_time() : (mp_float_t)0.0);
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_get_animation_time_obj, native_get_animation_time);

static mp_obj_t native_set_animation_time(mp_obj_t t_obj) {
    float t = (float)mp_obj_get_float(t_obj);
    if (!s_anim_set_time || !s_anim_set_time(t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.set_animation_time: nothing is currently playing to scrub (call phi.play_animation first)"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_set_animation_time_obj, native_set_animation_time);

/* One (name, duration) pair per real loaded clip -- so a script (or
 * Claude) can discover real clip names instead of guessing them, same
 * reasoning phi.md's Phase 6 section gives for phi.node_types() existing. */
static mp_obj_t native_list_animation_clips(void) {
    char names[8][64];
    float durations[8];
    int n = s_anim_list_clips ? s_anim_list_clips(names, durations, 8) : 0;
    mp_obj_t items[8];
    for (int i = 0; i < n; i++) {
        mp_obj_t pair[2] = { mp_obj_new_str(names[i], strlen(names[i])), mp_obj_new_float((mp_float_t)durations[i]) };
        items[i] = mp_obj_new_tuple(2, pair);
    }
    return mp_obj_new_tuple((size_t)n, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_list_animation_clips_obj, native_list_animation_clips);

/* ---- @phi.panel registry (C side) --
 * Captured eagerly the moment a panel's decorator runs (see
 * native_panel_registered below, called from PHI_BOOTSTRAP's @panel
 * decorator) rather than introspected from Python's _panel_registry dict
 * on demand -- simpler than walking a MicroPython dict's internal map
 * from C, and it's the natural point to instantiate+cache the instance
 * anyway (see phi_mp_draw_panel's own comment on why instances are
 * cached, not rebuilt every call). */
typedef struct {
    char     name[64];
    mp_obj_t instance;   /* MP_OBJ_NULL if instantiation raised */
} PhiMpPanel;

static PhiMpPanel s_panels[PHI_MP_MAX_PANELS];
static int        s_panel_count = 0;
static mp_obj_t   s_phi_namespace = MP_OBJ_NULL;   /* cached `phi`, looked up once, passed as every panel's ctx */

static mp_obj_t native_panel_registered(mp_obj_t name_obj) {
    const char *name = mp_obj_str_get_str(name_obj);
    int slot = -1;
    for (int i = 0; i < s_panel_count; i++) {
        if (strcmp(s_panels[i].name, name) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_panel_count >= PHI_MP_MAX_PANELS) {
            printf("[mp_port] @phi.panel('%s'): registry full (max %d), ignored\n", name, PHI_MP_MAX_PANELS);
            return mp_const_none;
        }
        slot = s_panel_count++;
    }
    strncpy(s_panels[slot].name, name, sizeof(s_panels[slot].name) - 1);
    s_panels[slot].name[sizeof(s_panels[slot].name) - 1] = 0;
    s_panels[slot].instance = MP_OBJ_NULL;

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t registry = mp_obj_dict_get(MP_OBJ_FROM_PTR(mp_globals_get()), MP_OBJ_NEW_QSTR(qstr_from_str("_panel_registry")));
        mp_obj_t cls = mp_obj_dict_get(registry, mp_obj_new_str(name, strlen(name)));
        mp_obj_t instance = mp_call_function_n_kw(cls, 0, 0, NULL);
        s_panels[slot].instance = instance;
        nlr_pop();
    } else {
        printf("[mp_port] @phi.panel('%s'): instantiation raised, panel left unusable:\n", name);
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_panel_registered_obj, native_panel_registered);

/* ---- Phase 6's node graphs, exposed to Python (node_graph.h) -- see
 * phi.md's "Geometry and Animation Nodes". @phi.node registers a node
 * TYPE (name/inputs/outputs/category/fn) into the registry below, the
 * same "capture everything at decoration time" shape @phi.panel/
 * native_panel_registered already established just above. phi.Graph (a
 * plain Python class in PHI_BOOTSTRAP, see its own comment further down)
 * wraps an integer graph id into node_graph.c's own C-owned PhiGraph
 * registry, the identical "opaque id into a C array" idiom MeshObject/
 * PhiLight ids already use throughout this codebase -- NOT a native
 * MicroPython type written in C (no make_new/locals-dict machinery),
 * which this embedding has never needed before and doesn't need here
 * either: a handful of free functions plus a thin Python-side wrapper
 * class is simpler and lower-risk than hand-writing a real mp_obj_type_t.
 *
 * native_graph_evaluate is the one place in this whole feature that
 * legitimately touches BOTH node_graph.c's topology and live mp_obj_t
 * Python values -- every value it produces (each node's real return
 * value, resolved link inputs) lives only in that function's own C-
 * stack-local arrays for the duration of one evaluate() call, never
 * stored back into node_graph.c itself (see node_graph.h's file comment
 * on why that split exists). ---- */

#define PHI_MP_MAX_NODE_TYPES 32

typedef struct {
    char     name[64];
    mp_obj_t inputs;    /* the original @phi.node(inputs=...) list, verbatim -- see phi.node_types() below */
    mp_obj_t outputs;   /* same, outputs= */
    mp_obj_t category;  /* a Python str */
    mp_obj_t fn;        /* the wrapped function itself, called by native_graph_evaluate */
} PhiMpNodeType;

static PhiMpNodeType s_node_types[PHI_MP_MAX_NODE_TYPES];
static int           s_node_type_count = 0;

static PhiMpNodeType *find_node_type(const char *name) {
    for (int i = 0; i < s_node_type_count; i++) {
        if (strcmp(s_node_types[i].name, name) == 0) return &s_node_types[i];
    }
    return NULL;
}

static mp_obj_t native_node_registered(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    const char *name = mp_obj_str_get_str(args[0]);
    PhiMpNodeType *nt = find_node_type(name);
    if (!nt) {
        if (s_node_type_count >= PHI_MP_MAX_NODE_TYPES) {
            printf("[mp_port] @phi.node('%s'): registry full (max %d), ignored\n", name, PHI_MP_MAX_NODE_TYPES);
            return mp_const_none;
        }
        nt = &s_node_types[s_node_type_count++];
    }
    strncpy(nt->name, name, sizeof(nt->name) - 1);
    nt->name[sizeof(nt->name) - 1] = 0;
    nt->inputs = args[1];
    nt->outputs = args[2];
    nt->category = args[3];
    nt->fn = args[4];
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_node_registered_obj, 5, 5, native_node_registered);

/* phi.node_types() -- introspection so a script (or an LLM working in a
 * running instance) can discover what's registered before building a
 * graph, per phi.md's own stated reason for this call existing. Rebuilds
 * a fresh dict from the registry every call rather than caching one --
 * this registry only grows a handful of times at startup, not a hot path. */
static mp_obj_t native_node_types(void) {
    mp_obj_t dict = mp_obj_new_dict((size_t)s_node_type_count);
    for (int i = 0; i < s_node_type_count; i++) {
        mp_obj_t entry = mp_obj_new_dict(3);
        mp_obj_dict_store(entry, mp_obj_new_str("inputs", 6), s_node_types[i].inputs);
        mp_obj_dict_store(entry, mp_obj_new_str("outputs", 7), s_node_types[i].outputs);
        mp_obj_dict_store(entry, mp_obj_new_str("category", 8), s_node_types[i].category);
        mp_obj_dict_store(dict, mp_obj_new_str(s_node_types[i].name, strlen(s_node_types[i].name)), entry);
    }
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_node_types_obj, native_node_types);

static mp_obj_t native_graph_create(mp_obj_t kind_obj) {
    const char *kind_str = mp_obj_str_get_str(kind_obj);
    PhiGraphKind kind = (strcmp(kind_str, "animation") == 0) ? PHI_GRAPH_KIND_ANIMATION : PHI_GRAPH_KIND_GEOMETRY;
    int gid = phi_graph_create(kind);
    if (gid < 0) mp_raise_ValueError(MP_ERROR_TEXT("phi.Graph: graph registry is full (PHI_GRAPH_MAX_GRAPHS)"));
    return mp_obj_new_int(gid);
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_graph_create_obj, native_graph_create);

static mp_obj_t native_graph_add_node(mp_obj_t gid_obj, mp_obj_t type_name_obj) {
    int gid = mp_obj_get_int(gid_obj);
    const char *type_name = mp_obj_str_get_str(type_name_obj);
    int idx = phi_graph_add_node(gid, type_name);
    if (idx < 0) mp_raise_ValueError(MP_ERROR_TEXT("phi.Graph.add_node: no such graph, or it's full (PHI_GRAPH_MAX_NODES)"));
    return mp_obj_new_int(idx);
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_graph_add_node_obj, native_graph_add_node);

static mp_obj_t native_graph_set_param_float(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int gid = mp_obj_get_int(args[0]);
    int idx = mp_obj_get_int(args[1]);
    const char *name = mp_obj_str_get_str(args[2]);
    float value = (float)mp_obj_get_float(args[3]);
    if (!phi_graph_set_param_float(gid, idx, name, value))
        mp_raise_ValueError(MP_ERROR_TEXT("phi.Graph: no such graph/node for a param set"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_graph_set_param_float_obj, 4, 4, native_graph_set_param_float);

static mp_obj_t native_graph_set_param_string(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int gid = mp_obj_get_int(args[0]);
    int idx = mp_obj_get_int(args[1]);
    const char *name = mp_obj_str_get_str(args[2]);
    const char *value = mp_obj_str_get_str(args[3]);
    if (!phi_graph_set_param_string(gid, idx, name, value))
        mp_raise_ValueError(MP_ERROR_TEXT("phi.Graph: no such graph/node for a param set"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_graph_set_param_string_obj, 4, 4, native_graph_set_param_string);

/* See node_graph.h's own comment on why PhiGraphParam grew a real third
 * type -- shader node params (base_color, emission's color) are 3-tuples,
 * which neither set_param_float nor set_param_string above could hold. */
static mp_obj_t native_graph_set_param_vec3(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int gid = mp_obj_get_int(args[0]);
    int idx = mp_obj_get_int(args[1]);
    const char *name = mp_obj_str_get_str(args[2]);
    size_t n; mp_obj_t *items;
    mp_obj_get_array(args[3], &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.Graph: a vec3 param must be a 3-element sequence"));
    float value[3] = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    if (!phi_graph_set_param_vec3(gid, idx, name, value))
        mp_raise_ValueError(MP_ERROR_TEXT("phi.Graph: no such graph/node for a param set"));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_graph_set_param_vec3_obj, 4, 4, native_graph_set_param_vec3);

static mp_obj_t native_graph_connect(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int gid = mp_obj_get_int(args[0]);
    int src = mp_obj_get_int(args[1]);
    const char *src_socket = mp_obj_str_get_str(args[2]);
    int dst = mp_obj_get_int(args[3]);
    const char *dst_socket = mp_obj_str_get_str(args[4]);
    return phi_graph_connect(gid, src, src_socket, dst, dst_socket) ? mp_const_true : mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_graph_connect_obj, 5, 5, native_graph_connect);

static mp_obj_t native_graph_set_position(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int gid = mp_obj_get_int(args[0]);
    int idx = mp_obj_get_int(args[1]);
    float x = (float)mp_obj_get_float(args[2]);
    float y = (float)mp_obj_get_float(args[3]);
    phi_graph_set_position(gid, idx, x, y);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_graph_set_position_obj, 4, 4, native_graph_set_position);

/* Finds src_socket's position within a node type's declared `outputs`
 * list (same (name, type[, ...]) tuple shape as `inputs`) -- -1 if not
 * found. Only needed to unpack a MULTI-output node's returned tuple at
 * the right index; a single-output type's raw return value IS the
 * output already (ordinary Python return semantics, not a 1-tuple), see
 * native_graph_evaluate below. */
static int find_output_index(mp_obj_t outputs, const char *socket_name) {
    size_t n; mp_obj_t *items;
    mp_obj_get_array(outputs, &n, &items);
    for (size_t i = 0; i < n; i++) {
        size_t tn; mp_obj_t *titems;
        mp_obj_get_array(items[i], &tn, &titems);
        if (tn >= 1 && strcmp(mp_obj_str_get_str(titems[0]), socket_name) == 0) return (int)i;
    }
    return -1;
}

/* The evaluator: walks node_graph.c's topological order, and for each
 * node resolves every declared input socket (an upstream link's cached
 * output wins over the node's own literal param, matching every node
 * editor's convention; neither present just omits that keyword and lets
 * the Python function's own default argument -- or a real TypeError if
 * it has none -- handle it) and calls the node type's registered Python
 * function by KEYWORD, not position -- robust to inputs= being listed in
 * a different order than the function's own parameters, unlike a
 * positional call would be.
 *
 * Honest scope note: `context` (args[1]) is accepted (matching phi.md's
 * `graph.evaluate(context)` signature) but not yet threaded into any
 * node call as an implicit extra input -- animation graphs' "time is an
 * implicit input" behavior needs real per-frame main-loop integration
 * this pass doesn't attempt; a node function simply won't receive it
 * yet. The overall graph result is the LAST node in topological order's
 * output -- a real, stated scope decision (phi.md's own Graph sketch
 * doesn't specify how a graph's single overall result is chosen), not an
 * accident of iteration order. */
static mp_obj_t native_graph_evaluate(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    int gid = mp_obj_get_int(args[0]);
    PhiGraph *g = phi_graph_find(gid);
    if (!g) mp_raise_ValueError(MP_ERROR_TEXT("phi.Graph.evaluate: no such graph"));

    int order[PHI_GRAPH_MAX_NODES];
    int n = phi_graph_topological_order(g, order);
    if (n < 0) mp_raise_ValueError(MP_ERROR_TEXT("phi.Graph.evaluate: the graph contains a cycle"));

    mp_obj_t node_output[PHI_GRAPH_MAX_NODES];   /* stack-local, NOT static -- see this function's own comment on why (reentrancy) */
    for (int i = 0; i < n; i++) {
        int node_idx = order[i];
        PhiGraphNode *node = &g->nodes[node_idx];
        PhiMpNodeType *nt = find_node_type(node->type_name);
        if (!nt) mp_raise_ValueError(MP_ERROR_TEXT("phi.Graph.evaluate: a node's type was never registered via @phi.node"));

        size_t n_inputs; mp_obj_t *input_items;
        mp_obj_get_array(nt->inputs, &n_inputs, &input_items);

        mp_obj_t kw_args[PHI_GRAPH_MAX_PARAMS * 2];
        size_t n_kw = 0;
        for (size_t s = 0; s < n_inputs && n_kw < PHI_GRAPH_MAX_PARAMS; s++) {
            size_t tn; mp_obj_t *titems;
            mp_obj_get_array(input_items[s], &tn, &titems);
            if (tn < 1) continue;
            const char *socket_name = mp_obj_str_get_str(titems[0]);

            mp_obj_t resolved = MP_OBJ_NULL;
            for (int l = 0; l < g->link_count; l++) {
                if (!g->links[l].used || g->links[l].dst_node != node_idx) continue;
                if (strcmp(g->links[l].dst_socket, socket_name) != 0) continue;
                int src_node = g->links[l].src_node;
                PhiMpNodeType *src_nt = find_node_type(g->nodes[src_node].type_name);
                mp_obj_t raw = node_output[src_node];
                if (src_nt) {
                    size_t out_n; mp_obj_t *out_items;
                    mp_obj_get_array(src_nt->outputs, &out_n, &out_items);
                    if (out_n > 1) {
                        int oi = find_output_index(src_nt->outputs, g->links[l].src_socket);
                        if (oi >= 0) {
                            size_t rn; mp_obj_t *ritems;
                            mp_obj_get_array(raw, &rn, &ritems);
                            if ((size_t)oi < rn) raw = ritems[oi];
                        }
                    }
                }
                resolved = raw;
                break;   /* last-link-wins isn't reachable here (break on first match) -- deliberate: first match in link-array order, same simple "first wins" this pass commits to rather than defining a real precedence rule for genuinely ambiguous multi-link-into-one-socket authoring, which node_graph.c's own connect() comment already flags as a UI/authoring concern, not a topology one */
            }
            if (resolved == MP_OBJ_NULL) {
                for (int p = 0; p < node->param_count; p++) {
                    if (strcmp(node->params[p].name, socket_name) != 0) continue;
                    if (node->params[p].type == PHI_GRAPH_PARAM_STRING) {
                        resolved = mp_obj_new_str(node->params[p].value_s, strlen(node->params[p].value_s));
                    } else if (node->params[p].type == PHI_GRAPH_PARAM_VEC3) {
                        mp_obj_t v3[3] = {
                            mp_obj_new_float((mp_float_t)node->params[p].value_vec3[0]),
                            mp_obj_new_float((mp_float_t)node->params[p].value_vec3[1]),
                            mp_obj_new_float((mp_float_t)node->params[p].value_vec3[2]),
                        };
                        resolved = mp_obj_new_tuple(3, v3);
                    } else {
                        resolved = mp_obj_new_float((mp_float_t)node->params[p].value_f);
                    }
                    break;
                }
            }
            if (resolved == MP_OBJ_NULL) continue;

            kw_args[n_kw*2 + 0] = MP_OBJ_NEW_QSTR(qstr_from_str(socket_name));
            kw_args[n_kw*2 + 1] = resolved;
            n_kw++;
        }

        node_output[node_idx] = mp_call_function_n_kw(nt->fn, 0, n_kw, kw_args);
    }

    return n > 0 ? node_output[order[n-1]] : mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(native_graph_evaluate_obj, 2, 2, native_graph_evaluate);

/* Bootstrap script, run once at the end of phi_mp_init after the native
 * functions above are already in globals (it references them by name).
 * The @phi.panel class-decorator shape is copied verbatim from
 * mp_test_main.c step 4 -- already prototyped and confirmed working
 * against real MicroPython there, not reinvented here. */
static const char *PHI_BOOTSTRAP =
    "_panel_registry = {}\n"
    "class Panel:\n"
    "    pass\n"
    "def _panel_decorator(name):\n"
    "    def wrap(cls):\n"
    "        _panel_registry[name] = cls\n"
    "        _native_panel_registered(name)\n"
    "        return cls\n"
    "    return wrap\n"
    "class _PhiNamespace:\n"
    "    pass\n"
    "phi = _PhiNamespace()\n"
    "phi.Panel = Panel\n"
    "phi.panel = _panel_decorator\n"
    "phi.prop_get = _native_prop_get\n"
    "phi.prop_set = _native_prop_set\n"
    "phi.enable_physics = _native_enable_physics\n"
    "phi.apply_impulse = _native_apply_impulse\n"
    "phi.get_velocity = _native_get_velocity\n"
    "phi.set_velocity = _native_set_velocity\n"
    /* Phase 9 gap-closing (2026-08-18) -- camera, whole-object transforms,
     * object-id-keyed physics, keyboard/mouse, and gamepad. See phi.md's
     * Phase 9 "Known gaps" for what each of these closes and why. */
    "phi.set_camera = _native_set_camera\n"
    "phi.get_object_position = _native_get_object_position\n"
    "phi.set_object_position = _native_set_object_position\n"
    "phi.get_object_rotation = _native_get_object_rotation\n"
    "phi.set_object_rotation = _native_set_object_rotation\n"
    "phi.get_object_scale = _native_get_object_scale\n"
    "phi.set_object_scale = _native_set_object_scale\n"
    "phi.object_enable_physics = _native_object_enable_physics\n"
    "phi.object_apply_impulse = _native_object_apply_impulse\n"
    "phi.object_get_velocity = _native_object_get_velocity\n"
    "phi.object_set_velocity = _native_object_set_velocity\n"
    "phi.key_down = _native_key_down\n"
    "phi.mouse_pos = _native_mouse_pos\n"
    "phi.mouse_button_down = _native_mouse_button_down\n"
    "phi.gamepad_count = _native_gamepad_count\n"
    "phi.gamepad_connected = _native_gamepad_connected\n"
    "phi.gamepad_button = _native_gamepad_button\n"
    "phi.gamepad_axis = _native_gamepad_axis\n"
    /* Phase 10 (2026-08-18) -- audio, see phi.md's "Phase 10 -- Audio". */
    "phi.load_sound = _native_load_sound\n"
    "phi.play_sound = _native_play_sound\n"
    "phi.play_sound_3d = _native_play_sound_3d\n"
    "phi.stop_sound = _native_stop_sound\n"
    "phi.add_light = _native_add_light\n"
    "phi.delete_light = _native_delete_light\n"
    "phi.list_lights = _native_list_lights\n"
    "phi.mesh_object = _native_mesh_object\n"
    "phi.create_mesh = _native_create_mesh\n"
    "phi.set_vertices = _native_set_vertices\n"
    "phi.set_vertex = _native_set_vertex\n"
    "phi.list_objects = _native_list_objects\n"
    "phi.delete_object = _native_delete_object\n"
    "phi.get_vertices = _native_get_vertices\n"
    "phi.get_faces = _native_get_faces\n"
    "phi.add_vertex = _native_add_vertex\n"
    "phi.add_face = _native_add_face\n"
    "phi.delete_face = _native_delete_face\n"
    "phi.set_face_material = _native_set_face_material\n"
    "phi.flip_normals = _native_flip_normals\n"
    "phi.extrude_face = _native_extrude_face\n"
    "phi.inset_face = _native_inset_face\n"
    "phi.loop_cut = _native_loop_cut\n"
    "phi.nearest_edge_of_face = _native_nearest_edge_of_face\n"
    "phi.render = _native_render\n"
    "phi.activate_ragdoll = _native_activate_ragdoll\n"
    /* Animation playback (see mp_port.h's phi_mp_register_animation_
     * callbacks) -- operates on main.c's single skinned-test-object slot. */
    "phi.play_animation = _native_play_animation\n"
    "phi.pause_animation = _native_pause_animation\n"
    "phi.resume_animation = _native_resume_animation\n"
    "phi.is_animation_playing = _native_is_animation_playing\n"
    "phi.get_animation_time = _native_get_animation_time\n"
    "phi.set_animation_time = _native_set_animation_time\n"
    "phi.list_animation_clips = _native_list_animation_clips\n"
    /* Phase 6's node graphs (see phi.md) -- @phi.node captures a node
     * TYPE at decoration time, the identical shape @phi.panel/_panel_
     * decorator above already established for panels. Deliberately
     * mirrors that decorator's own structure rather than inventing a new
     * pattern. */
    "_node_registry = {}\n"
    "def _node_decorator(inputs=None, outputs=None, category='geometry'):\n"
    "    def wrap(fn):\n"
    "        name = fn.__name__\n"
    "        real_inputs = inputs if inputs is not None else []\n"
    "        real_outputs = outputs if outputs is not None else []\n"
    "        _node_registry[name] = fn\n"
    "        _native_node_registered(name, real_inputs, real_outputs, category, fn)\n"
    "        return fn\n"
    "    return wrap\n"
    "phi.node = _node_decorator\n"
    "phi.node_types = _native_node_types\n"
    /* Marker classes for socket type annotations (@phi.node(inputs=
     * [('mesh', phi.Mesh)], ...)) -- metadata for introspection/future
     * UI only, NOT enforced against a node function's real arguments at
     * call time this pass (a real, stated scope limit, not an oversight
     * -- see native_graph_evaluate's own comment). */
    "class Mesh:\n"
    "    pass\n"
    "class Texture:\n"
    "    pass\n"
    "phi.Mesh = Mesh\n"
    "phi.Texture = Texture\n"
    /* phi.Graph -- a plain Python wrapper around an opaque C-owned graph
     * id (node_graph.c), the same "id into a C array" idiom phi.add_
     * light/phi.mesh_object already use. add_node's **params collects
     * literal (unconnected-input) values by ordinary Python kwarg syntax
     * rather than needing a native dict-parsing function on the C side. */
    "class Graph:\n"
    "    def __init__(self, kind='geometry'):\n"
    "        self._id = _native_graph_create(kind)\n"
    "    def add_node(self, type_name, **params):\n"
    "        idx = _native_graph_add_node(self._id, type_name)\n"
    "        for k in params:\n"
    "            v = params[k]\n"
    "            if isinstance(v, str):\n"
    "                _native_graph_set_param_string(self._id, idx, k, v)\n"
    "            elif isinstance(v, (tuple, list)):\n"
    "                _native_graph_set_param_vec3(self._id, idx, k, v)\n"
    "            else:\n"
    "                _native_graph_set_param_float(self._id, idx, k, float(v))\n"
    "        return idx\n"
    "    def connect(self, src, src_socket, dst, dst_socket):\n"
    "        return _native_graph_connect(self._id, src, src_socket, dst, dst_socket)\n"
    "    def set_position(self, node_id, x, y):\n"
    "        _native_graph_set_position(self._id, node_id, x, y)\n"
    "    def evaluate(self, context=None):\n"
    "        return _native_graph_evaluate(self._id, context)\n"
    "phi.Graph = Graph\n"
    /* Built-in node types -- one MVP-real example per node domain
     * (shader/geometry/animation), each backed by an already-proven
     * phi.* call, not a new invented subsystem. Registered the same way
     * any user's own @phi.node function would be (this bootstrap script
     * has no privileged path -- phi.node is already fully defined by the
     * time execution reaches here). */
    /* Shader nodes (see phi.md's "screen-space effects... same @phi.node
     * decorator" note -- these are the material-shading half of that,
     * not screen-space post-process). No node-graph editor panel exists
     * yet (see phi.md's Phase 6 status) -- these are real, evaluable node
     * types today via phi.Graph, just not yet visually wireable. */
    "class Color:\n"
    "    pass\n"
    "phi.Color = Color\n"
    "class Shader:\n"
    "    pass\n"
    "phi.Shader = Shader\n"
    "@phi.node(inputs=[('base_color', Color, (0.8, 0.8, 0.8)), ('metallic', float, 0.0), ('roughness', float, 0.5)], outputs=[('shader', Shader)], category='shader')\n"
    "def principled_bsdf(base_color=(0.8, 0.8, 0.8), metallic=0.0, roughness=0.5):\n"
    "    return {'base_color': tuple(base_color), 'metallic': float(metallic), 'roughness': float(roughness), 'emission': (0.0, 0.0, 0.0)}\n"
    "@phi.node(inputs=[('color', Color, (1.0, 1.0, 1.0)), ('strength', float, 1.0)], outputs=[('shader', Shader)], category='shader')\n"
    "def emission(color=(1.0, 1.0, 1.0), strength=1.0):\n"
    "    r = color[0] * strength\n"
    "    g = color[1] * strength\n"
    "    b = color[2] * strength\n"
    "    return {'base_color': (0.0, 0.0, 0.0), 'metallic': 0.0, 'roughness': 1.0, 'emission': (r, g, b)}\n"
    /* apply_material is the bridge that makes the two node types above
     * genuinely actionable without a panel: it writes a graph-computed
     * shader dict onto real geometry via phi.set_face_material. */
    "@phi.node(inputs=[('object_id', float, 0.0), ('face_index', float, 0.0), ('shader', Shader, None)], outputs=[], category='shader')\n"
    "def apply_material(object_id=0.0, face_index=0.0, shader=None):\n"
    "    if shader is None:\n"
    "        return None\n"
    "    phi.set_face_material(int(object_id), int(face_index), shader['base_color'], shader['metallic'], shader['roughness'], shader['emission'])\n"
    "    return None\n"
    /* Geometry nodes -- matches the doc's own input_mesh/noise_displace
     * vocabulary (phi.md's Phase 6 Graph example), using only already-
     * proven phi.* geometry calls rather than inventing a new noise
     * function to get a first example landed. */
    "@phi.node(inputs=[('asset', str, ''), ('x', float, 0.0), ('y', float, 0.0), ('z', float, 0.0)], outputs=[('object_id', float)], category='geometry')\n"
    "def input_mesh(asset='', x=0.0, y=0.0, z=0.0):\n"
    "    return float(phi.mesh_object(asset, x, y, z))\n"
    "@phi.node(inputs=[('object_id', float, 0.0), ('dx', float, 0.0), ('dy', float, 0.0), ('dz', float, 0.0)], outputs=[('object_id', float)], category='geometry')\n"
    "def translate_mesh(object_id=0.0, dx=0.0, dy=0.0, dz=0.0):\n"
    "    oid = int(object_id)\n"
    "    verts = phi.get_vertices(oid)\n"
    "    offsets = (dx, dy, dz)\n"
    "    new_verts = [verts[i] + offsets[i % 3] for i in range(len(verts))]\n"
    "    phi.set_vertices(oid, new_verts)\n"
    "    return float(oid)\n"
    /* Animation node -- a real trigger over the new phi.play_animation
     * binding. NOT per-frame time-driven yet (native_graph_evaluate
     * doesn't thread `context`/time into node calls this pass, a stated
     * scope limit -- see its own comment), so this is scoped honestly as
     * a one-shot "start this clip" node, not a live scrubber. */
    "@phi.node(inputs=[('clip_name', str, ''), ('loop', float, 1.0)], outputs=[('playing', float)], category='animation')\n"
    "def play_animation_node(clip_name='', loop=1.0):\n"
    "    ok = phi.play_animation(clip_name if clip_name else None, bool(loop))\n"
    "    return 1.0 if ok else 0.0\n";

int phi_mp_panel_count(void) { return s_panel_count; }

const char *phi_mp_panel_name(int index) {
    if (index < 0 || index >= s_panel_count) return "";
    return s_panels[index].name;
}

int phi_mp_draw_panel(int index, char out_lines[][256], int max_lines) {
    if (index < 0 || index >= s_panel_count || s_panels[index].instance == MP_OBJ_NULL) return -1;
    s_capture_len = 0;
    if (s_capture_buf) s_capture_buf[0] = 0;

    int n_written;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_obj_t draw_bound = mp_load_attr(s_panels[index].instance, qstr_from_str("draw"));
        mp_obj_t args[1] = { s_phi_namespace };
        mp_obj_t result = mp_call_function_n_kw(draw_bound, 1, 0, args);
        if (mp_obj_is_type(result, &mp_type_list)) {
            size_t n; mp_obj_t *items;
            mp_obj_list_get(result, &n, &items);
            n_written = 0;
            for (size_t i = 0; i < n && (int)i < max_lines; i++) {
                const char *s = mp_obj_str_get_str(items[i]);
                strncpy(out_lines[n_written], s, 255);
                out_lines[n_written][255] = 0;
                n_written++;
            }
        } else {
            n_written = 0;   /* didn't return a list -- "nothing to draw", not an error */
        }
        nlr_pop();
    } else {
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
        n_written = -1;
    }
    return n_written;
}

const char *phi_mp_last_captured_output(void) {
    return s_capture_buf ? s_capture_buf : "";
}

static void phi_mp_install_bindings(void) {
    mp_obj_dict_t *globals = MP_OBJ_TO_PTR(mp_globals_get());
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_prop_get")), MP_OBJ_FROM_PTR(&native_prop_get_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_prop_set")), MP_OBJ_FROM_PTR(&native_prop_set_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_panel_registered")), MP_OBJ_FROM_PTR(&native_panel_registered_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_enable_physics")), MP_OBJ_FROM_PTR(&native_enable_physics_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_apply_impulse")), MP_OBJ_FROM_PTR(&native_apply_impulse_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_get_velocity")), MP_OBJ_FROM_PTR(&native_get_velocity_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_set_velocity")), MP_OBJ_FROM_PTR(&native_set_velocity_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_set_camera")), MP_OBJ_FROM_PTR(&native_set_camera_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_get_object_position")), MP_OBJ_FROM_PTR(&native_get_object_position_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_set_object_position")), MP_OBJ_FROM_PTR(&native_set_object_position_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_get_object_rotation")), MP_OBJ_FROM_PTR(&native_get_object_rotation_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_set_object_rotation")), MP_OBJ_FROM_PTR(&native_set_object_rotation_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_get_object_scale")), MP_OBJ_FROM_PTR(&native_get_object_scale_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_set_object_scale")), MP_OBJ_FROM_PTR(&native_set_object_scale_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_object_enable_physics")), MP_OBJ_FROM_PTR(&native_object_enable_physics_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_object_apply_impulse")), MP_OBJ_FROM_PTR(&native_object_apply_impulse_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_object_get_velocity")), MP_OBJ_FROM_PTR(&native_object_get_velocity_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_object_set_velocity")), MP_OBJ_FROM_PTR(&native_object_set_velocity_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_key_down")), MP_OBJ_FROM_PTR(&native_key_down_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_mouse_pos")), MP_OBJ_FROM_PTR(&native_mouse_pos_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_mouse_button_down")), MP_OBJ_FROM_PTR(&native_mouse_button_down_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_gamepad_count")), MP_OBJ_FROM_PTR(&native_gamepad_count_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_gamepad_connected")), MP_OBJ_FROM_PTR(&native_gamepad_connected_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_gamepad_button")), MP_OBJ_FROM_PTR(&native_gamepad_button_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_gamepad_axis")), MP_OBJ_FROM_PTR(&native_gamepad_axis_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_load_sound")), MP_OBJ_FROM_PTR(&native_load_sound_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_play_sound")), MP_OBJ_FROM_PTR(&native_play_sound_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_play_sound_3d")), MP_OBJ_FROM_PTR(&native_play_sound_3d_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_stop_sound")), MP_OBJ_FROM_PTR(&native_stop_sound_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_add_light")), MP_OBJ_FROM_PTR(&native_add_light_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_delete_light")), MP_OBJ_FROM_PTR(&native_delete_light_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_list_lights")), MP_OBJ_FROM_PTR(&native_list_lights_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_mesh_object")), MP_OBJ_FROM_PTR(&native_mesh_object_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_create_mesh")), MP_OBJ_FROM_PTR(&native_create_mesh_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_set_vertices")), MP_OBJ_FROM_PTR(&native_set_vertices_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_set_vertex")), MP_OBJ_FROM_PTR(&native_set_vertex_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_list_objects")), MP_OBJ_FROM_PTR(&native_list_objects_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_delete_object")), MP_OBJ_FROM_PTR(&native_delete_object_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_get_vertices")), MP_OBJ_FROM_PTR(&native_get_vertices_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_get_faces")), MP_OBJ_FROM_PTR(&native_get_faces_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_add_vertex")), MP_OBJ_FROM_PTR(&native_add_vertex_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_add_face")), MP_OBJ_FROM_PTR(&native_add_face_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_delete_face")), MP_OBJ_FROM_PTR(&native_delete_face_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_set_face_material")), MP_OBJ_FROM_PTR(&native_set_face_material_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_flip_normals")), MP_OBJ_FROM_PTR(&native_flip_normals_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_extrude_face")), MP_OBJ_FROM_PTR(&native_extrude_face_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_inset_face")), MP_OBJ_FROM_PTR(&native_inset_face_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_loop_cut")), MP_OBJ_FROM_PTR(&native_loop_cut_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_nearest_edge_of_face")), MP_OBJ_FROM_PTR(&native_nearest_edge_of_face_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_render")), MP_OBJ_FROM_PTR(&native_render_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_activate_ragdoll")), MP_OBJ_FROM_PTR(&native_activate_ragdoll_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_play_animation")), MP_OBJ_FROM_PTR(&native_play_animation_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_pause_animation")), MP_OBJ_FROM_PTR(&native_pause_animation_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_resume_animation")), MP_OBJ_FROM_PTR(&native_resume_animation_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_is_animation_playing")), MP_OBJ_FROM_PTR(&native_is_animation_playing_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_get_animation_time")), MP_OBJ_FROM_PTR(&native_get_animation_time_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_set_animation_time")), MP_OBJ_FROM_PTR(&native_set_animation_time_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_list_animation_clips")), MP_OBJ_FROM_PTR(&native_list_animation_clips_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_node_registered")), MP_OBJ_FROM_PTR(&native_node_registered_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_node_types")), MP_OBJ_FROM_PTR(&native_node_types_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_graph_create")), MP_OBJ_FROM_PTR(&native_graph_create_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_graph_add_node")), MP_OBJ_FROM_PTR(&native_graph_add_node_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_graph_set_param_float")), MP_OBJ_FROM_PTR(&native_graph_set_param_float_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_graph_set_param_string")), MP_OBJ_FROM_PTR(&native_graph_set_param_string_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_graph_set_param_vec3")), MP_OBJ_FROM_PTR(&native_graph_set_param_vec3_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_graph_connect")), MP_OBJ_FROM_PTR(&native_graph_connect_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_graph_set_position")), MP_OBJ_FROM_PTR(&native_graph_set_position_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_graph_evaluate")), MP_OBJ_FROM_PTR(&native_graph_evaluate_obj));

    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_embed_exec_str(PHI_BOOTSTRAP);
        s_phi_namespace = mp_obj_dict_get(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("phi")));
        nlr_pop();
    } else {
        printf("[mp_port] PHI_BOOTSTRAP raised during phi_mp_init -- phi.panel/phi.prop_get/set will be unavailable:\n");
        mp_obj_print_exception(&mp_plat_print, (mp_obj_t)nlr.ret_val);
    }
}

void phi_mp_init(void *stack_top) {
    mp_embed_init(&s_mp_heap[0], sizeof(s_mp_heap), stack_top);
    /* mp_embed_init only calls the deprecated mp_stack_set_top(), which
     * never sets stack_limit -- py/cstack.c's mp_cstack_check() (what's
     * actually compiled in at this ROM level) reads that same state and
     * treats the default-zero limit as "already over budget", raising a
     * false RecursionError on the very first check, caught nowhere, which
     * hangs the process in nlr_jump_fail's infinite loop. Root-caused via
     * gdb backtrace during this session's mp_test_main.c development
     * (see its own comment) -- the exact same workaround applies here,
     * for the exact same reason. mp_stack_set_limit() is the old (but
     * still functional) stackctrl.c API and writes the same
     * MP_STATE_THREAD(stack_limit) cstack.c reads. */
    mp_stack_set_limit(32 * 1024);
    phi_mp_install_bindings();
}

char *phi_mp_exec(const char *code) {
    s_capture_len = 0;
    if (s_capture_buf) s_capture_buf[0] = 0;
    mp_embed_exec_str(code);
    return strdup(s_capture_buf ? s_capture_buf : "");
}
