#include "fracture_body.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PHI_MAX_FRACTURE_CONSTRAINTS 256

typedef struct {
    MeshObject obj;
    int        in_use;
} FragmentSlot;

static FragmentSlot   s_fragments[PHI_MAX_FRACTURE_BODIES];
static int            s_fragment_count = 0;
static PhiConstraint *s_constraints[PHI_MAX_FRACTURE_CONSTRAINTS];
static int            s_constraint_count = 0;
static int            s_next_id = 1;

void fracture_body_system_init(void) {
    memset(s_fragments, 0, sizeof(s_fragments));
    s_fragment_count = 0;
    memset(s_constraints, 0, sizeof(s_constraints));
    s_constraint_count = 0;
    s_next_id = 1;
}

/* Same flat-face-normal + default-material flattening
 * meshobject_build_render_mesh_from_halfedge uses (see its own comment
 * for the winding convention), rewritten against a FractureFragment's
 * plain flat position/index arrays instead of a HalfEdgeMesh -- fragments
 * carry no per-face material of their own (fracture.h's FractureFragment
 * is position/index only), so every fragment gets the same reasonable
 * default halfedge_set_face_material's own doc comment already
 * establishes for this codebase (0.7 gray, non-metallic, roughness 0.8,
 * no emission) rather than inventing a different fallback here. Sized to
 * the fragment's OWN real vertex count, not mesh_create()'s generous
 * ~262k-vertex default capacity -- that default is fine for the one
 * MeshObject test-object slot, but multiplied across up to
 * PHI_MAX_FRACTURE_BODIES fragments it would waste a genuinely large
 * amount of memory for what are typically small pieces. */
static RenderMesh *make_fragment_render_mesh(const FractureFragment *frag) {
    RenderMesh *m = (RenderMesh *)calloc(1, sizeof(RenderMesh));
    int n_verts = frag->index_count;   /* non-indexed, 3 verts/triangle, same convention MESHOBJ layout already uses */
    if (n_verts < 3) n_verts = 3;
    m->capacity = n_verts;
    m->data = (float *)malloc((size_t)m->capacity * MESHOBJ_VERTEX_STRIDE * sizeof(float));
    m->count = 0;

    for (int t = 0; t < frag->index_count / 3; t++) {
        unsigned short i0 = frag->indices[t*3+0], i1 = frag->indices[t*3+1], i2 = frag->indices[t*3+2];
        const float *a = &frag->positions[i0*3];
        const float *b = &frag->positions[i1*3];
        const float *c = &frag->positions[i2*3];
        float ux = b[0]-a[0], uy = b[1]-a[1], uz = b[2]-a[2];
        float vx = c[0]-a[0], vy = c[1]-a[1], vz = c[2]-a[2];
        float n[3] = { uy*vz - uz*vy, uz*vx - ux*vz, ux*vy - uy*vx };
        float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        if (len > 1e-8f) { n[0] /= len; n[1] /= len; n[2] /= len; }

        const float *tri[3] = { a, b, c };
        for (int i = 0; i < 3; i++) {
            float *v = m->data + m->count * MESHOBJ_VERTEX_STRIDE;
            v[0] = tri[i][0]; v[1] = tri[i][1]; v[2] = tri[i][2];
            v[3] = n[0]; v[4] = n[1]; v[5] = n[2];
            v[6] = 0.7f; v[7] = 0.7f; v[8] = 0.7f;   /* default base_color, matches halfedge.h's own documented default */
            v[9] = 0.0f;    /* metallic */
            v[10] = 0.8f;   /* roughness */
            v[11] = 0.0f; v[12] = 0.0f; v[13] = 0.0f;  /* emission */
            m->count++;
        }
    }
    m->dirty = 1;
    return m;
}

int fracture_body_activate(PhiPhysicsWorld *world, const HalfEdgeMesh *source_hem,
                            Vec3f source_position, Quat source_orientation,
                            int n_fragments, unsigned int seed, float breaking_threshold) {
    fracture_body_clear(world);
    if (!source_hem || n_fragments < 1) return 0;

    MeshObject src = {0};
    src.hem = (HalfEdgeMesh *)source_hem;   /* fracture_voronoi only reads through this, never mutates */
    FractureFragment *frags = fracture_voronoi(&src, n_fragments, seed);
    if (!frags) return 0;

    unsigned char adjacent[PHI_MAX_FRACTURE_BODIES * PHI_MAX_FRACTURE_BODIES];
    int n_for_adjacency = n_fragments <= PHI_MAX_FRACTURE_BODIES ? n_fragments : PHI_MAX_FRACTURE_BODIES;
    fracture_compute_adjacency(frags, n_for_adjacency, adjacent);

    float orientation4[4] = { source_orientation.x, source_orientation.y, source_orientation.z, source_orientation.w };
    /* fragment index (into frags[]) -> slot index in s_fragments[], or -1
     * if that fragment was empty/skipped/didn't fit -- needed to look up
     * the right PhiRigidBody* when wiring adjacency constraints below. */
    int slot_of[PHI_MAX_FRACTURE_BODIES];
    for (int i = 0; i < PHI_MAX_FRACTURE_BODIES; i++) slot_of[i] = -1;

    int spawned = 0;
    for (int i = 0; i < n_fragments && spawned < PHI_MAX_FRACTURE_BODIES; i++) {
        if (frags[i].pos_count == 0) continue;   /* real, expected outcome for some seed placements, see fracture.h */

        FragmentSlot *slot = &s_fragments[spawned];
        slot->obj.id = s_next_id++;
        slot->obj.position = source_position;
        slot->obj.orientation = source_orientation;
        slot->obj.scale = (Vec3f){1.0f, 1.0f, 1.0f};
        slot->obj.is_static = 0;
        slot->obj.hem = NULL;
        slot->obj.render_mesh = make_fragment_render_mesh(&frags[i]);

        slot->obj.phys_body = phi_physics_add_convex_hull_body(
            world, frags[i].positions, frags[i].pos_count, source_position, orientation4, 1.0f, 0.2f);
        slot->in_use = 1;
        if (i < PHI_MAX_FRACTURE_BODIES) slot_of[i] = spawned;
        spawned++;
    }
    s_fragment_count = spawned;

    /* Glue every adjacent pair (see fracture_compute_adjacency's own
     * comment: a shared vertex means they were cut from the same
     * bisector plane, i.e. they physically touch) with a real breaking-
     * threshold fixed constraint at the shared origin -- every fragment
     * was spawned at the identical source_position/orientation (each
     * one's OWN local-space vertex offsets are what actually place it
     * within that shared frame, see this file's header comment), so
     * that shared point is a simple, correct, always-valid pivot rather
     * than needing to compute a per-pair midpoint. */
    for (int i = 0; i < n_for_adjacency && s_constraint_count < PHI_MAX_FRACTURE_CONSTRAINTS; i++) {
        if (slot_of[i] < 0) continue;
        for (int j = i + 1; j < n_for_adjacency && s_constraint_count < PHI_MAX_FRACTURE_CONSTRAINTS; j++) {
            if (slot_of[j] < 0) continue;
            if (!adjacent[i * n_for_adjacency + j]) continue;
            PhiRigidBody *a = s_fragments[slot_of[i]].obj.phys_body;
            PhiRigidBody *b = s_fragments[slot_of[j]].obj.phys_body;
            if (!a || !b) continue;
            s_constraints[s_constraint_count++] =
                phi_physics_add_fixed_constraint(world, a, b, source_position, breaking_threshold);
        }
    }

    fracture_free_fragments(frags, n_fragments);
    return spawned;
}

void fracture_body_clear(PhiPhysicsWorld *world) {
    for (int i = 0; i < s_constraint_count; i++) {
        phi_physics_remove_constraint(world, s_constraints[i]);
    }
    s_constraint_count = 0;
    for (int i = 0; i < s_fragment_count; i++) {
        if (!s_fragments[i].in_use) continue;
        MeshObject *obj = &s_fragments[i].obj;
        if (obj->phys_body) phi_physics_remove_body(world, obj->phys_body);
        mesh_destroy(obj->render_mesh);
    }
    memset(s_fragments, 0, sizeof(s_fragments));
    s_fragment_count = 0;
}

int fracture_body_count(void) { return s_fragment_count; }

int fracture_body_get(int index, Vec3f *out_position, Quat *out_orientation) {
    if (index < 0 || index >= s_fragment_count || !s_fragments[index].in_use) return 0;
    if (out_position) *out_position = s_fragments[index].obj.position;
    if (out_orientation) *out_orientation = s_fragments[index].obj.orientation;
    return 1;
}

void fracture_body_apply_impulse(int index, Vec3f impulse, Vec3f rel_pos) {
    if (index < 0 || index >= s_fragment_count || !s_fragments[index].in_use) return;
    if (!s_fragments[index].obj.phys_body) return;
    phi_physics_apply_impulse(s_fragments[index].obj.phys_body, impulse, rel_pos);
}

void fracture_body_sync_and_draw_all(Renderer *r) {
    for (int i = 0; i < s_fragment_count; i++) {
        if (!s_fragments[i].in_use) continue;
        MeshObject *obj = &s_fragments[i].obj;
        if (obj->phys_body) {
            float orientation[4];
            phi_physics_get_transform(obj->phys_body, &obj->position, orientation);
            obj->orientation.x = orientation[0];
            obj->orientation.y = orientation[1];
            obj->orientation.z = orientation[2];
            obj->orientation.w = orientation[3];
        }
        renderer_draw_mesh_object(r, obj);
    }
}
