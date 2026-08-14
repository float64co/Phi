#include "scene_target.h"
#include "light.h"
#include <stdlib.h>
#include <string.h>

static MeshObject     *(*s_get_selected_object)(void) = NULL;
static const int      *s_edit_face = NULL;
static RenderSettings *s_render_settings = NULL;

void scene_target_register(MeshObject *(*get_selected_object)(void), const int *edit_face,
                            RenderSettings *render_settings) {
    s_get_selected_object = get_selected_object;
    s_edit_face = edit_face;
    s_render_settings = render_settings;
}

int scene_resolve_target(const char *target, const PhiPropGroup **out_group, void **out_owner) {
    if (strcmp(target, "object") == 0) {
        MeshObject *obj = s_get_selected_object ? s_get_selected_object() : NULL;
        if (!obj) return 0;
        *out_group = &g_phi_prop_mesh_object;
        *out_owner = obj;
        return 1;
    }
    if (strcmp(target, "face") == 0) {
        MeshObject *obj = s_get_selected_object ? s_get_selected_object() : NULL;
        if (!obj || !obj->hem ||
            !s_edit_face || *s_edit_face < 0 ||
            *s_edit_face >= obj->hem->face_count ||
            obj->hem->faces[*s_edit_face].deleted) {
            return 0;
        }
        *out_group = &g_phi_prop_heface;
        *out_owner = &obj->hem->faces[*s_edit_face];
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
