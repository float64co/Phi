#include "scene_objects.h"
#include <string.h>

typedef struct {
    MeshObject obj;
    int in_use;
} SceneObjectSlot;

static SceneObjectSlot s_slots[SCENE_MAX_OBJECTS];
static int             s_next_id = 1;

void scene_objects_init(void) {
    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1;
}

MeshObject *scene_object_add(void) {
    for (int i = 0; i < SCENE_MAX_OBJECTS; i++) {
        if (s_slots[i].in_use) continue;
        memset(&s_slots[i], 0, sizeof(SceneObjectSlot));
        s_slots[i].in_use = 1;
        s_slots[i].obj.id = s_next_id++;
        return &s_slots[i].obj;
    }
    return NULL;
}

void scene_object_delete(MeshObject *obj, PhiPhysicsWorld *phys_world) {
    if (!obj) return;
    mesh_destroy(obj->render_mesh);
    halfedge_destroy(obj->hem);
    if (obj->phys_body && phys_world) {
        phi_physics_remove_body(phys_world, obj->phys_body);
    }
    for (int i = 0; i < SCENE_MAX_OBJECTS; i++) {
        if (&s_slots[i].obj == obj) {
            memset(&s_slots[i], 0, sizeof(SceneObjectSlot));
            return;
        }
    }
}

MeshObject *scene_object_find(int id) {
    for (int i = 0; i < SCENE_MAX_OBJECTS; i++) {
        if (s_slots[i].in_use && s_slots[i].obj.id == id) return &s_slots[i].obj;
    }
    return NULL;
}

int scene_object_get_all(MeshObject *out_objects[SCENE_MAX_OBJECTS]) {
    int n = 0;
    for (int i = 0; i < SCENE_MAX_OBJECTS; i++) {
        if (s_slots[i].in_use) out_objects[n++] = &s_slots[i].obj;
    }
    return n;
}

int scene_object_count(void) {
    int n = 0;
    for (int i = 0; i < SCENE_MAX_OBJECTS; i++) if (s_slots[i].in_use) n++;
    return n;
}
