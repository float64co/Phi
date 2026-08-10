#include "phi_physics.h"
#include <btBulletDynamicsCommon.h>

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

extern "C" {

PhiPhysicsWorld *phi_physics_world_create(void) {
    PhiPhysicsWorld *w = new PhiPhysicsWorld();
    w->collision_config = new btDefaultCollisionConfiguration();
    w->dispatcher        = new btCollisionDispatcher(w->collision_config);
    w->broadphase         = new btDbvtBroadphase();
    w->solver             = new btSequentialImpulseConstraintSolver();
    w->world = new btDiscreteDynamicsWorld(w->dispatcher, w->broadphase, w->solver, w->collision_config);
    w->world->setGravity(btVector3(0.0f, -9.81f, 0.0f));
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

} /* extern "C" */
