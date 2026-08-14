#pragma once
#include "phi_prop.h"

/* This project's actual PhiProp registrations -- phi_prop.h/.c themselves
 * know nothing about MeshObject/HEFace, this is where that knowledge
 * lives (offsetof(MeshObject, position) etc.), kept separate so the
 * generic property engine doesn't need to include meshobject.h/
 * halfedge.h at all. */

extern const PhiPropGroup g_phi_prop_mesh_object;      /* MeshObject: position, is_static */
extern const PhiPropGroup g_phi_prop_heface;           /* HEFace: base_color, metallic, roughness, emission */
extern const PhiPropGroup g_phi_prop_light;            /* PhiLight: position, direction, color, energy, radius, spot_size, spot_blend, area_size, sun_angle */
extern const PhiPropGroup g_phi_prop_render_settings;  /* RenderSettings: samples */
