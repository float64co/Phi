#include "phi_prop_registry.h"
#include "meshobject.h"
#include "halfedge.h"
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
