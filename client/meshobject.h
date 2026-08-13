#pragma once
#include "vec3.h"            /* Vec3f */
#include "octree_render.h"  /* RenderMesh */
#include "halfedge.h"
#include "phi_physics.h"    /* PhiRigidBody (opaque) -- see phys_body below */

/* Phase 1 foundation: the MeshObject entity type sketched in phi.md's
 * "Mesh Editor" section — kept minimal for this phase's non-interactive
 * foundation slice (glTF I/O + half-edge structure only, see phi.md). */
typedef struct { float x, y, z, w; } Quat;

typedef struct {
    int        id;
    Vec3f      position;
    Quat       orientation;
    /* Non-uniform scale, {1,1,1} = untouched -- driven by main.c's S
     * transform tool (see transform_op.h), applied in
     * renderer_draw_mesh_object's model matrix. Every existing spawn site
     * must set this explicitly (no implicit zero-init default the way
     * position/orientation get away with, since a zeroed scale would
     * collapse the object to nothing rather than leaving it unchanged). */
    Vec3f      scale;
    RenderMesh *render_mesh;
    int        is_static;
    /* NULL = no physics simulation for this object (the common case --
     * most MeshObjects are just static scene geometry). Non-NULL once
     * something calls phi_physics_add_box_body and stores the result
     * here (see main.c's CTX_ACTION_ENABLE_PHYSICS handler) -- main.c's
     * frame loop then syncs position/orientation FROM this body's
     * simulated transform every frame instead of leaving them alone.
     * Owned by this MeshObject once set, same convention as hem/
     * render_mesh -- freed via phi_physics_remove_body by whatever
     * deletes the object or disables its physics. Phase 2's real
     * landing of the ConvexHull-shaped placeholder phi.md's original
     * sketch had here -- box-shape-from-AABB only this pass, see
     * phi_physics.h's own note on why convex hulls are deferred. */
    PhiRigidBody *phys_body;
    /* The live, editable half-edge representation this object's
     * render_mesh was last flattened from — NULL for objects that don't
     * (yet) carry one (kept optional rather than required so existing
     * non-editable callers aren't forced to populate it). Owned by this
     * MeshObject once set (freed alongside render_mesh by whatever deletes
     * the object). Editing operations (extrude/inset/loop-cut, see
     * mesh_edit.h) mutate this in place and the caller re-flattens via
     * meshobject_build_render_mesh_from_halfedge afterward — this field is
     * what lets an already-spawned object be edited more than once instead
     * of only ever being built-then-discarded (see main.c's earlier
     * spawn_test_mesh_object, which used to halfedge_destroy() right after
     * the first flatten). */
    HalfEdgeMesh *hem;
} MeshObject;

/* MeshObject's own vertex layout (interleaved, non-indexed/vertex-
 * duplicated-per-triangle, same shape as octree_render.h's generic
 * VERTEX_STRIDE=7 format but richer -- a real per-face PBR material
 * replaces the old single unused mat_id float, see meshobject_build_
 * render_mesh_from_halfedge). Deliberately a SEPARATE format/shader path
 * from the shared world/ground/players/rockets one (renderer.c's
 * VERTEX_STRIDE=7 + its shared program) rather than growing that shared
 * format for everyone: those draw calls have no per-face material concept
 * at all (their color is one glUniform3f per whole draw call) and don't
 * need one, so giving every vertex in the game 8 extra floats it'd never
 * use would be pure waste for no benefit outside the one editable entity
 * type that actually has faces worth coloring individually.
 *   pos:        3 x float (12 bytes)
 *   normal:     3 x float (12 bytes)
 *   base_color: 3 x float (12 bytes)
 *   metallic:   1 x float ( 4 bytes)
 *   roughness:  1 x float ( 4 bytes)
 *   emission:   3 x float (12 bytes)
 */
#define MESHOBJ_VERTEX_STRIDE 14

Quat quat_identity(void);

/* Normalizes q -- returns quat_identity() for a near-zero-length input
 * rather than dividing by ~0, since that's a real, reachable case
 * (quat_slerp's own linear-interpolation fallback path normalizes an
 * interpolated result that could in principle be degenerate). */
Quat quat_normalize(Quat q);

/* Spherical linear interpolation, t in [0,1] -- takes the shorter of the
 * two paths between a and b (negates b first if the dot product is
 * negative, the standard fix for quaternion double-cover: q and -q
 * represent the same rotation, but naively interpolating toward the
 * "wrong" sign takes the long way around). Falls back to a normalized
 * linear interpolation when a and b are nearly parallel (dot > 0.9995),
 * the standard numerical-stability fix for sin(theta)-in-the-denominator
 * blowing up as theta -> 0. Used by Phase 4's animation.c for rotation-
 * channel keyframe interpolation (see phi.md's "Animation Editor"). */
Quat quat_slerp(Quat a, Quat b, float t);

/* Computes a local-space AABB half-extent (for phi_physics_add_box_body's
 * box shape) from hem's actual vertex bounds -- see meshobject.c for the
 * "assumes roughly centered on local origin" caveat. Returns 0 (out
 * untouched) if hem is NULL or empty. */
int meshobject_local_aabb_half_extents(const HalfEdgeMesh *hem, Vec3f *out_half_extents);

/* Column-major 4x4 rotation matrix from a unit quaternion, same layout
 * convention as renderer.c's mat4_* helpers (m[col*4+row]). Standard
 * formula, not derived here — verified numerically (identity in, identity
 * out; known 90-degree axis rotations checked against expected basis
 * vectors) before use, matching this codebase's established bar for
 * matrix/rotation code (see renderer.c's mat4_inverse comment). */
void quat_to_mat4(const Quat *q, float *out16);

/* Builds render_mesh's flat MESHOBJ_VERTEX_STRIDE-per-vertex triangle list
 * (non-indexed/vertex-duplicated-per-triangle) from a HalfEdgeMesh.
 * Computes flat per-face normals since this phase's test asset carries no
 * NORMAL attribute (see halfedge_gltf.c's load path) — every corner of a
 * face gets that face's flat normal, so lighting is faceted rather than
 * smooth-shaded. Every corner of a face also gets that face's real PBR
 * material (HEFace's own base_color/metallic/roughness/emission fields,
 * see halfedge.h) — a genuine per-face value now, not the old fixed-
 * per-whole-object mat_id float this function used to take as a
 * parameter. */
void meshobject_build_render_mesh_from_halfedge(RenderMesh *out, const HalfEdgeMesh *hem);

/* Real ray/triangle picking against obj's actual world-space geometry
 * (Möller–Trumbore, not a bounding-box approximation) — returns 1 and
 * sets *out_t (ray parameter of the nearest hit) if ray_origin+t*ray_dir
 * hits any triangle, 0 otherwise. ray_dir need not be pre-normalized (t
 * is just a scale factor along whatever direction was passed), but the
 * caller comparing t across multiple objects/calls should keep it
 * consistent (a normalized dir makes t a literal world-space distance,
 * which is what main.c's picking code uses it for). */
int meshobject_ray_pick(const MeshObject *obj, Vec3f ray_origin, Vec3f ray_dir, float *out_t);

/* Face-level picking against obj->hem directly (not render_mesh) — same
 * Möller–Trumbore/world-space-transform technique as meshobject_ray_pick,
 * but walks the live half-edge faces (skipping deleted ones, see
 * halfedge_delete_face) so the returned *out_face index is something
 * mesh_edit.c's operations can act on directly. Returns 0 (obj->hem is
 * NULL, or no face hit) or 1 with *out_t and *out_face set to the nearest hit.
 * Requires every live face to be a triangle (true of every face this
 * codebase's editing ops ever produce, see halfedge.h's own "triangles
 * only for now" note). */
int meshobject_ray_pick_face(const MeshObject *obj, Vec3f ray_origin, Vec3f ray_dir,
                              float *out_t, int *out_face);

/* Un-transforms a world-space point into obj->hem's own local space
 * (inverse of the position+orientation transform meshobject_ray_pick/
 * meshobject_ray_pick_face apply going the other way) — the rotation
 * matrix quat_to_mat4 produces is orthonormal, so its inverse is just its
 * transpose, no separate matrix-inverse routine needed. Used to turn a
 * world-space ray-pick hit point into the local-space point
 * mesh_edit_nearest_edge_of_face expects. */
Vec3f meshobject_world_to_local(const MeshObject *obj, Vec3f world_pos);
