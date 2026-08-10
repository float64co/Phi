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
#include "halfedge.h"

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

static MeshObject      *s_target_obj = NULL;
static const int       *s_target_obj_loaded = NULL;
static const int       *s_target_edit_face = NULL;
static PhiPhysicsWorld *s_phys_world = NULL;

void phi_mp_register_targets(MeshObject *test_obj, const int *test_obj_loaded, const int *edit_face,
                              PhiPhysicsWorld *phys_world) {
    s_target_obj = test_obj;
    s_target_obj_loaded = test_obj_loaded;
    s_target_edit_face = edit_face;
    s_phys_world = phys_world;
}

/* Resolves target="object"/"face" to the live PhiPropGroup+owner pointer
 * to read/write through -- returns 0 (out params untouched) if that
 * target isn't currently available (nothing loaded / no face selected),
 * which native_prop_get/set turn into a real raised Python exception
 * rather than a silent wrong read. */
static int resolve_target(const char *target, const PhiPropGroup **out_group, void **out_owner) {
    if (strcmp(target, "object") == 0) {
        if (!s_target_obj || !s_target_obj_loaded || !*s_target_obj_loaded) return 0;
        *out_group = &g_phi_prop_mesh_object;
        *out_owner = s_target_obj;
        return 1;
    }
    if (strcmp(target, "face") == 0) {
        if (!s_target_obj || !s_target_obj_loaded || !*s_target_obj_loaded || !s_target_obj->hem ||
            !s_target_edit_face || *s_target_edit_face < 0 ||
            *s_target_edit_face >= s_target_obj->hem->face_count ||
            s_target_obj->hem->faces[*s_target_edit_face].deleted) {
            return 0;
        }
        *out_group = &g_phi_prop_heface;
        *out_owner = &s_target_obj->hem->faces[*s_target_edit_face];
        return 1;
    }
    return 0;
}

static mp_obj_t native_prop_get(mp_obj_t target_obj, mp_obj_t identifier_obj) {
    const char *target = mp_obj_str_get_str(target_obj);
    const char *identifier = mp_obj_str_get_str(identifier_obj);
    const PhiPropGroup *group; void *owner;
    if (!resolve_target(target, &group, &owner)) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.prop_get: target not currently available ('object' needs a loaded MeshObject, 'face' needs a selected face)"));
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
    if (!resolve_target(target, &group, &owner)) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.prop_set: target not currently available ('object' needs a loaded MeshObject, 'face' needs a selected face)"));
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
    if (!s_target_obj || !s_target_obj_loaded || !*s_target_obj_loaded) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.enable_physics: no MeshObject loaded"));
    }
    if (s_target_obj->phys_body) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.enable_physics: already has a physics body"));
    }
    Vec3f half_extents;
    if (!meshobject_local_aabb_half_extents(s_target_obj->hem, &half_extents)) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.enable_physics: couldn't compute an AABB (empty mesh?)"));
    }
    float orientation[4] = {
        s_target_obj->orientation.x, s_target_obj->orientation.y,
        s_target_obj->orientation.z, s_target_obj->orientation.w
    };
    float mass = (float)mp_obj_get_float(mass_obj);
    float restitution = (float)mp_obj_get_float(restitution_obj);
    s_target_obj->phys_body = phi_physics_add_box_body(s_phys_world, half_extents, s_target_obj->position,
                                                         orientation, mass, restitution);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_enable_physics_obj, native_enable_physics);

/* Shared guard for the three functions below -- all three are meaningless
 * (and would otherwise segfault on a NULL phys_body) before phi.
 * enable_physics has actually created one. */
static int require_phys_body(void) {
    return s_target_obj && s_target_obj_loaded && *s_target_obj_loaded && s_target_obj->phys_body != NULL;
}

static mp_obj_t native_apply_impulse(mp_obj_t impulse_obj, mp_obj_t rel_pos_obj) {
    if (!require_phys_body()) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.apply_impulse: object has no physics body (call phi.enable_physics first)"));
    }
    size_t n; mp_obj_t *items;
    mp_obj_get_array(impulse_obj, &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.apply_impulse: expected a 3-element impulse vector"));
    Vec3f impulse = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    mp_obj_get_array(rel_pos_obj, &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.apply_impulse: expected a 3-element rel_pos vector"));
    Vec3f rel_pos = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    phi_physics_apply_impulse(s_target_obj->phys_body, impulse, rel_pos);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(native_apply_impulse_obj, native_apply_impulse);

static mp_obj_t native_get_velocity(void) {
    if (!require_phys_body()) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.get_velocity: object has no physics body (call phi.enable_physics first)"));
    }
    Vec3f v = phi_physics_get_linear_velocity(s_target_obj->phys_body);
    mp_obj_t items[3] = { mp_obj_new_float(v.x), mp_obj_new_float(v.y), mp_obj_new_float(v.z) };
    return mp_obj_new_tuple(3, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(native_get_velocity_obj, native_get_velocity);

static mp_obj_t native_set_velocity(mp_obj_t v_obj) {
    if (!require_phys_body()) {
        mp_raise_ValueError(MP_ERROR_TEXT("phi.set_velocity: object has no physics body (call phi.enable_physics first)"));
    }
    size_t n; mp_obj_t *items;
    mp_obj_get_array(v_obj, &n, &items);
    if (n != 3) mp_raise_ValueError(MP_ERROR_TEXT("phi.set_velocity: expected a 3-element velocity vector"));
    Vec3f v = { (float)mp_obj_get_float(items[0]), (float)mp_obj_get_float(items[1]), (float)mp_obj_get_float(items[2]) };
    phi_physics_set_linear_velocity(s_target_obj->phys_body, v);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(native_set_velocity_obj, native_set_velocity);

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
    "phi.set_velocity = _native_set_velocity\n";

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
