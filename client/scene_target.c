#include "scene_target.h"
#include "light.h"
#include <stdlib.h>
#include <string.h>

static MeshObject     *s_obj = NULL;
static const int      *s_obj_loaded = NULL;
static const int      *s_edit_face = NULL;
static RenderSettings *s_render_settings = NULL;

void scene_target_register(MeshObject *test_obj, const int *test_obj_loaded, const int *edit_face,
                            RenderSettings *render_settings) {
    s_obj = test_obj;
    s_obj_loaded = test_obj_loaded;
    s_edit_face = edit_face;
    s_render_settings = render_settings;
}

int scene_resolve_target(const char *target, const PhiPropGroup **out_group, void **out_owner) {
    if (strcmp(target, "object") == 0) {
        if (!s_obj || !s_obj_loaded || !*s_obj_loaded) return 0;
        *out_group = &g_phi_prop_mesh_object;
        *out_owner = s_obj;
        return 1;
    }
    if (strcmp(target, "face") == 0) {
        if (!s_obj || !s_obj_loaded || !*s_obj_loaded || !s_obj->hem ||
            !s_edit_face || *s_edit_face < 0 ||
            *s_edit_face >= s_obj->hem->face_count ||
            s_obj->hem->faces[*s_edit_face].deleted) {
            return 0;
        }
        *out_group = &g_phi_prop_heface;
        *out_owner = &s_obj->hem->faces[*s_edit_face];
        return 1;
    }
    if (strcmp(target, "render") == 0) {
        if (!s_render_settings) return 0;
        *out_group = &g_phi_prop_render_settings;
        *out_owner = s_render_settings;
        return 1;
    }
    if (strncmp(target, "light:", 6) == 0) {
        int id = atoi(target + 6);
        PhiLight *l = light_find(id);
        if (!l) return 0;
        *out_group = &g_phi_prop_light;
        *out_owner = l;
        return 1;
    }
    return 0;
}
