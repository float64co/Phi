#pragma once
#include "octree.h"         /* Vec3f */
#include "octree_render.h"  /* RenderMesh */
#include "halfedge.h"

/* Phase 1 foundation: the MeshObject entity type sketched in phi.md's
 * "Mesh Editor" section — kept minimal for this phase's non-interactive
 * foundation slice (glTF I/O + half-edge structure only, see phi.md).
 * Deliberately NOT included yet: the ConvexHull physics-hull field from
 * phi.md's sketch — Bullet isn't integrated (that's Phase 2's job), so a
 * placeholder field for it would just be dead weight; it'll be added when
 * physics actually lands rather than faked now. */
typedef struct { float x, y, z, w; } Quat;

typedef struct {
    int        id;
    Vec3f      position;
    Quat       orientation;
    RenderMesh *render_mesh;
    int        is_static;
} MeshObject;

Quat quat_identity(void);

/* Column-major 4x4 rotation matrix from a unit quaternion, same layout
 * convention as renderer.c's mat4_* helpers (m[col*4+row]). Standard
 * formula, not derived here — verified numerically (identity in, identity
 * out; known 90-degree axis rotations checked against expected basis
 * vectors) before use, matching this codebase's established bar for
 * matrix/rotation code (see renderer.c's mat4_inverse comment). */
void quat_to_mat4(const Quat *q, float *out16);

/* Builds render_mesh's flat (pos,normal,mat_id) triangle list (RenderMesh's
 * existing VERTEX_STRIDE=7 format, non-indexed/vertex-duplicated-per-
 * triangle — same shape octree_render.c's mesh_rebuild already produces)
 * from a HalfEdgeMesh. Computes flat per-face normals since this phase's
 * test asset carries no NORMAL attribute (see halfedge_gltf.c's load
 * path) — every corner of a face gets that face's flat normal, so
 * lighting is faceted rather than smooth-shaded. mat_id is a fixed value
 * for the whole object (no per-face material assignment yet, that's an
 * editor feature out of scope for this foundation slice). */
void meshobject_build_render_mesh_from_halfedge(RenderMesh *out, const HalfEdgeMesh *hem, float mat_id);
