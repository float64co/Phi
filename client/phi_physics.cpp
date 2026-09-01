#include "phi_physics.h"
#include <btBulletDynamicsCommon.h>
#include <LinearMath/btConvexHullComputer.h>

struct PhiPhysicsWorld {
    btDefaultCollisionConfiguration     *collision_config;
    btCollisionDispatcher               *dispatcher;
    btBroadphaseInterface                *broadphase;
    btSequentialImpulseConstraintSolver *solver;
    btDiscreteDynamicsWorld             *world;
};

struct PhiRigidBody {
    btRigidBody      *body;
    btCollisionShape *shape;         /* owned */
    btMotionState    *motion_state;  /* owned */
};

struct PhiConstraint {
    /* btTypedConstraint, not the specific btFixedConstraint -- widened
     * (was btFixedConstraint*) so this same wrapper struct also covers
     * phi_physics_add_point2point_constraint's btPoint2PointConstraint
     * below, since every operation this file actually performs on it
     * (addConstraint/removeConstraint/setBreakingImpulseThreshold/
     * isEnabled) is a btTypedConstraint base-class method -- a real
     * generalization, not a behavior change for the existing fixed-
     * constraint call sites. */
    btTypedConstraint *constraint;   /* owned */
};

extern "C" {

PhiPhysicsWorld *phi_physics_world_create(void) {
    PhiPhysicsWorld *w = new PhiPhysicsWorld();
    w->collision_config = new btDefaultCollisionConfiguration();
    w->dispatcher        = new btCollisionDispatcher(w->collision_config);
    w->broadphase         = new btDbvtBroadphase();
    w->solver             = new btSequentialImpulseConstraintSolver();
    w->world = new btDiscreteDynamicsWorld(w->dispatcher, w->broadphase, w->solver, w->collision_config);
    /* Z-up (2026-08-19, see vec3.h's coordinate-convention note): gravity
     * pulls toward -Z now, not -Y. */
    w->world->setGravity(btVector3(0.0f, 0.0f, -9.81f));
    return w;
}

void phi_physics_world_destroy(PhiPhysicsWorld *world) {
    if (!world) return;
    /* Bodies still in the world are the caller's responsibility to have
     * removed via phi_physics_remove_body first -- matches the same
     * "caller owns cleanup order" convention this codebase already uses
     * for MeshObject.hem/render_mesh (see meshobject.h). We don't walk
     * the world's remaining bodies and free them here, since that would
     * silently free memory a caller might still hold a PhiRigidBody* to. */
    delete world->world;
    delete world->solver;
    delete world->broadphase;
    delete world->dispatcher;
    delete world->collision_config;
    delete world;
}

void phi_physics_world_set_gravity(PhiPhysicsWorld *world, Vec3f gravity) {
    world->world->setGravity(btVector3(gravity.x, gravity.y, gravity.z));
}

void phi_physics_world_step(PhiPhysicsWorld *world, float dt) {
    world->world->stepSimulation(dt, 10, btScalar(1.0) / btScalar(60.0));
}

PhiRigidBody *phi_physics_add_box_body(PhiPhysicsWorld *world, Vec3f half_extents,
                                        Vec3f position, float orientation[4],
                                        float mass, float restitution) {
    PhiRigidBody *rb = new PhiRigidBody();
    rb->shape = new btBoxShape(btVector3(half_extents.x, half_extents.y, half_extents.z));

    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(btVector3(position.x, position.y, position.z));
    transform.setRotation(btQuaternion(orientation[0], orientation[1], orientation[2], orientation[3]));
    rb->motion_state = new btDefaultMotionState(transform);

    btVector3 local_inertia(0.0f, 0.0f, 0.0f);
    if (mass != 0.0f) rb->shape->calculateLocalInertia(mass, local_inertia);

    btRigidBody::btRigidBodyConstructionInfo rb_info(mass, rb->motion_state, rb->shape, local_inertia);
    rb_info.m_restitution = restitution;
    rb->body = new btRigidBody(rb_info);
    world->world->addRigidBody(rb->body);
    return rb;
}

PhiRigidBody *phi_physics_add_convex_hull_body(PhiPhysicsWorld *world,
                                                const float *positions, int vert_count,
                                                Vec3f position, float orientation[4],
                                                float mass, float restitution) {
    PhiRigidBody *rb = new PhiRigidBody();

    /* Same technique as Blender's RB_shape_new_convex_hull (intern/
     * rigidbody/rb_bullet_api.cpp): try to embed Bullet's usual 0.04
     * collision margin by shrinking the hull inward; if that fails for a
     * thin/degenerate point cloud, fall back to an exact zero-margin
     * hull rather than refusing to create a shape. Stride is 3 floats
     * (tightly-packed xyz) -- every caller so far (main.c's Enable
     * Physics) flattens into exactly that shape before calling. */
    const float margin = 0.04f;
    btConvexHullComputer hull_computer;
    if (hull_computer.compute(positions, 3 * (int)sizeof(float), vert_count, margin, 0.0f) < 0.0f) {
        hull_computer.compute(positions, 3 * (int)sizeof(float), vert_count, 0.0f, 0.0f);
    }
    rb->shape = new btConvexHullShape(&(hull_computer.vertices[0].getX()),
                                       hull_computer.vertices.size());

    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(btVector3(position.x, position.y, position.z));
    transform.setRotation(btQuaternion(orientation[0], orientation[1], orientation[2], orientation[3]));
    rb->motion_state = new btDefaultMotionState(transform);

    btVector3 local_inertia(0.0f, 0.0f, 0.0f);
    if (mass != 0.0f) rb->shape->calculateLocalInertia(mass, local_inertia);

    btRigidBody::btRigidBodyConstructionInfo rb_info(mass, rb->motion_state, rb->shape, local_inertia);
    rb_info.m_restitution = restitution;
    rb->body = new btRigidBody(rb_info);
    world->world->addRigidBody(rb->body);
    return rb;
}

PhiRigidBody *phi_physics_add_capsule_body(PhiPhysicsWorld *world, float radius, float half_height,
                                            Vec3f position, float orientation[4],
                                            float mass, float restitution) {
    PhiRigidBody *rb = new PhiRigidBody();
    rb->shape = new btCapsuleShape(radius, 2.0f * half_height);   /* btCapsuleShape's 2nd param is full height, not half */

    btTransform transform;
    transform.setIdentity();
    transform.setOrigin(btVector3(position.x, position.y, position.z));
    transform.setRotation(btQuaternion(orientation[0], orientation[1], orientation[2], orientation[3]));
    rb->motion_state = new btDefaultMotionState(transform);

    btVector3 local_inertia(0.0f, 0.0f, 0.0f);
    if (mass != 0.0f) rb->shape->calculateLocalInertia(mass, local_inertia);

    btRigidBody::btRigidBodyConstructionInfo rb_info(mass, rb->motion_state, rb->shape, local_inertia);
    rb_info.m_restitution = restitution;
    rb->body = new btRigidBody(rb_info);
    world->world->addRigidBody(rb->body);
    return rb;
}

void phi_physics_remove_body(PhiPhysicsWorld *world, PhiRigidBody *body) {
    if (!body) return;
    world->world->removeRigidBody(body->body);
    delete body->body;
    delete body->motion_state;
    delete body->shape;
    delete body;
}

void phi_physics_get_transform(PhiRigidBody *body, Vec3f *out_position, float out_orientation[4]) {
    btTransform t;
    body->motion_state->getWorldTransform(t);
    if (out_position) {
        const btVector3 &p = t.getOrigin();
        out_position->x = p.x(); out_position->y = p.y(); out_position->z = p.z();
    }
    if (out_orientation) {
        btQuaternion q = t.getRotation();
        out_orientation[0] = q.x(); out_orientation[1] = q.y();
        out_orientation[2] = q.z(); out_orientation[3] = q.w();
    }
}

void phi_physics_set_transform(PhiRigidBody *body, Vec3f position, const float orientation[4]) {
    btTransform t;
    t.setIdentity();
    t.setOrigin(btVector3(position.x, position.y, position.z));
    t.setRotation(btQuaternion(orientation[0], orientation[1], orientation[2], orientation[3]));
    body->body->setWorldTransform(t);
    body->motion_state->setWorldTransform(t);
    body->body->activate(true);
}

void phi_physics_apply_impulse(PhiRigidBody *body, Vec3f impulse, Vec3f rel_pos) {
    body->body->activate(true);
    body->body->applyImpulse(btVector3(impulse.x, impulse.y, impulse.z),
                              btVector3(rel_pos.x, rel_pos.y, rel_pos.z));
}

void phi_physics_set_linear_velocity(PhiRigidBody *body, Vec3f v) {
    body->body->activate(true);
    body->body->setLinearVelocity(btVector3(v.x, v.y, v.z));
}

Vec3f phi_physics_get_linear_velocity(PhiRigidBody *body) {
    const btVector3 &v = body->body->getLinearVelocity();
    Vec3f out = { v.x(), v.y(), v.z() };
    return out;
}

/* Same construction Blender's own make_constraint_transforms/
 * RB_constraint_new_fixed use: a world-space pivot frame, expressed
 * relative to each body's CURRENT world transform (frame_N = body_N's
 * inverse world transform * pivot_transform) -- a btFixedConstraint is
 * rigid regardless of the frame's own orientation, so an identity
 * rotation at the pivot point is a legitimate, simple default (Blender's
 * own default constraint-empty orientation is usually identity too). */
PhiConstraint *phi_physics_add_fixed_constraint(PhiPhysicsWorld *world,
                                                 PhiRigidBody *a, PhiRigidBody *b,
                                                 Vec3f pivot_world, float breaking_threshold) {
    btTransform pivot_transform;
    pivot_transform.setIdentity();
    pivot_transform.setOrigin(btVector3(pivot_world.x, pivot_world.y, pivot_world.z));

    btTransform frame_a = a->body->getWorldTransform().inverse() * pivot_transform;
    btTransform frame_b = b->body->getWorldTransform().inverse() * pivot_transform;

    PhiConstraint *c = new PhiConstraint();
    c->constraint = new btFixedConstraint(*a->body, *b->body, frame_a, frame_b);
    c->constraint->setBreakingImpulseThreshold(breaking_threshold);
    /* disableCollisionsBetweenLinkedBodies=true -- matches Blender's own
     * rigid body constraint default, so two glued fragments don't also
     * jitter against each other's collision shapes while intact. */
    world->world->addConstraint(c->constraint, true);
    return c;
}

PhiConstraint *phi_physics_add_point2point_constraint(PhiPhysicsWorld *world,
                                                        PhiRigidBody *a, Vec3f pivot_a,
                                                        PhiRigidBody *b, Vec3f pivot_b) {
    PhiConstraint *c = new PhiConstraint();
    c->constraint = new btPoint2PointConstraint(*a->body, *b->body,
                                                 btVector3(pivot_a.x, pivot_a.y, pivot_a.z),
                                                 btVector3(pivot_b.x, pivot_b.y, pivot_b.z));
    /* disableCollisionsBetweenLinkedBodies=true -- same reasoning as the
     * fixed constraint above: adjacent bone capsules shouldn't also
     * collide against each other at the joint they're pinned at. */
    world->world->addConstraint(c->constraint, true);
    return c;
}

void phi_physics_remove_constraint(PhiPhysicsWorld *world, PhiConstraint *c) {
    if (!c) return;
    world->world->removeConstraint(c->constraint);
    delete c->constraint;
    delete c;
}

int phi_physics_constraint_is_broken(const PhiConstraint *c) {
    return c->constraint->isEnabled() ? 0 : 1;
}

} /* extern "C" */
