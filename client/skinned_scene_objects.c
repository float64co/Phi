#include "skinned_scene_objects.h"
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <GL/gl.h>
#endif

typedef struct {
    SkinnedMeshObject obj;
    int in_use;
    int id;
} SkinnedSceneObjectSlot;

static SkinnedSceneObjectSlot s_slots[SKINNED_SCENE_MAX_OBJECTS];
static int                    s_next_id = 1;

void skinned_scene_objects_init(void) {
    memset(s_slots, 0, sizeof(s_slots));
    s_next_id = 1;
}

SkinnedMeshObject *skinned_scene_object_add(void) {
    for (int i = 0; i < SKINNED_SCENE_MAX_OBJECTS; i++) {
        if (s_slots[i].in_use) continue;
        memset(&s_slots[i], 0, sizeof(SkinnedSceneObjectSlot));
        s_slots[i].in_use = 1;
        s_slots[i].id = s_next_id++;
        return &s_slots[i].obj;
    }
    return NULL;
}

void skinned_scene_object_delete(SkinnedMeshObject *obj) {
    if (!obj) return;
    skinned_mesh_object_free(obj);
    /* renderer_draw_skinned_mesh (not this file) is the only thing that
     * ever glGenBuffers these -- mirrors scene_object_delete's own
     * mesh_destroy call freeing MeshObject::render_mesh's vbo alongside
     * its CPU data, same "whoever owns the slot cleans up everything
     * about to go out of scope" convention. */
    if (obj->gpu_uploaded) {
        glDeleteBuffers(1, &obj->vbo);
        glDeleteBuffers(1, &obj->ebo);
    }
    for (int i = 0; i < SKINNED_SCENE_MAX_OBJECTS; i++) {
        if (&s_slots[i].obj == obj) {
            memset(&s_slots[i], 0, sizeof(SkinnedSceneObjectSlot));
            return;
        }
    }
}

int skinned_scene_object_get_all(SkinnedMeshObject *out_objects[SKINNED_SCENE_MAX_OBJECTS]) {
    int n = 0;
    for (int i = 0; i < SKINNED_SCENE_MAX_OBJECTS; i++) {
        if (s_slots[i].in_use) out_objects[n++] = &s_slots[i].obj;
    }
    return n;
}

int skinned_scene_object_count(void) {
    int n = 0;
    for (int i = 0; i < SKINNED_SCENE_MAX_OBJECTS; i++) if (s_slots[i].in_use) n++;
    return n;
}

int skinned_scene_object_id(const SkinnedMeshObject *obj) {
    for (int i = 0; i < SKINNED_SCENE_MAX_OBJECTS; i++) {
        if (&s_slots[i].obj == obj) return s_slots[i].id;
    }
    return -1;
}
