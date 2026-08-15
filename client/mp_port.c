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
    "phi.set_velocity = _native_set_velocity\n"
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
    "phi.flip_normals = _native_flip_normals\n"
    "phi.extrude_face = _native_extrude_face\n"
    "phi.inset_face = _native_inset_face\n"
    "phi.loop_cut = _native_loop_cut\n"
    "phi.nearest_edge_of_face = _native_nearest_edge_of_face\n"
    "phi.render = _native_render\n"
    "phi.activate_ragdoll = _native_activate_ragdoll\n";

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
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_flip_normals")), MP_OBJ_FROM_PTR(&native_flip_normals_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_extrude_face")), MP_OBJ_FROM_PTR(&native_extrude_face_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_inset_face")), MP_OBJ_FROM_PTR(&native_inset_face_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_loop_cut")), MP_OBJ_FROM_PTR(&native_loop_cut_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_nearest_edge_of_face")), MP_OBJ_FROM_PTR(&native_nearest_edge_of_face_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_render")), MP_OBJ_FROM_PTR(&native_render_obj));
    mp_obj_dict_store(MP_OBJ_FROM_PTR(globals), MP_OBJ_NEW_QSTR(qstr_from_str("_native_activate_ragdoll")), MP_OBJ_FROM_PTR(&native_activate_ragdoll_obj));

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
