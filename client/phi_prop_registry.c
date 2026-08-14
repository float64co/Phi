#include "phi_prop_registry.h"
#include "meshobject.h"
#include "halfedge.h"
#include "light.h"
#include "render_settings.h"
#include <stddef.h>

static const PhiProp s_mesh_object_props[] = {
    { "position",  "Position",  PHI_PROP_VEC3, {0, 0}, offsetof(MeshObject, position) },
    { "is_static", "Static",    PHI_PROP_BOOL, {0, 0}, offsetof(MeshObject, is_static) },
};
const PhiPropGroup g_phi_prop_mesh_object = {
    "MeshObject", s_mesh_object_props, (int)(sizeof(s_mesh_object_props) / sizeof(s_mesh_object_props[0]))
};

static const PhiProp s_heface_props[] = {
    { "base_color", "Base Color", PHI_PROP_VEC3,  {0, 0}, offsetof(HEFace, base_color) },
    { "metallic",   "Metallic",   PHI_PROP_FLOAT, {0, 1}, offsetof(HEFace, metallic) },
    { "roughness",  "Roughness",  PHI_PROP_FLOAT, {0, 1}, offsetof(HEFace, roughness) },
    { "emission",   "Emission",   PHI_PROP_VEC3,  {0, 0}, offsetof(HEFace, emission) },
};
const PhiPropGroup g_phi_prop_heface = {
    "HEFace", s_heface_props, (int)(sizeof(s_heface_props) / sizeof(s_heface_props[0]))
};

/* PhiLight (light.h) -- every field registered regardless of the light's
 * current type (e.g. spot_size is exposed even for a Point light); the
 * Properties panel decides which subset to actually SHOW based on
 * ->type (ui.c), the registry itself stays generic/offset-based like
 * every other group here. type itself is a real settable int prop too
 * (Python can do phi.prop_set("light:3", "type", 1) to turn a Point into
 * a Sun) even though the C UI shows it as a click-to-cycle widget rather
 * than a raw number field. */
static const PhiProp s_light_props[] = {
    { "type",        "Type",         PHI_PROP_INT,   {0, LIGHT_TYPE_COUNT - 1}, offsetof(PhiLight, type) },
    { "position",    "Position",     PHI_PROP_VEC3,  {0, 0},    offsetof(PhiLight, position) },
    { "direction",   "Direction",    PHI_PROP_VEC3,  {0, 0},    offsetof(PhiLight, direction) },
    { "color",       "Color",        PHI_PROP_VEC3,  {0, 0},    offsetof(PhiLight, color) },
    { "energy",      "Energy",       PHI_PROP_FLOAT, {0, 0},    offsetof(PhiLight, energy) },
    { "radius",      "Radius",       PHI_PROP_FLOAT, {0, 1000}, offsetof(PhiLight, radius) },
    { "spot_size",   "Spot Size",    PHI_PROP_FLOAT, {0, 3.14159f}, offsetof(PhiLight, spot_size) },
    { "spot_blend",  "Spot Blend",   PHI_PROP_FLOAT, {0, 1},    offsetof(PhiLight, spot_blend) },
    { "area_size",   "Area Size",    PHI_PROP_FLOAT, {0.01f, 1000}, offsetof(PhiLight, area_size) },
    { "sun_angle",   "Sun Angle",    PHI_PROP_FLOAT, {0, 0.5f}, offsetof(PhiLight, sun_angle) },
};
const PhiPropGroup g_phi_prop_light = {
    "PhiLight", s_light_props, (int)(sizeof(s_light_props) / sizeof(s_light_props[0]))
};

static const PhiProp s_render_settings_props[] = {
    { "samples", "Samples", PHI_PROP_INT, {1, 1000000}, offsetof(RenderSettings, samples) },
};
const PhiPropGroup g_phi_prop_render_settings = {
    "RenderSettings", s_render_settings_props, (int)(sizeof(s_render_settings_props) / sizeof(s_render_settings_props[0]))
};
