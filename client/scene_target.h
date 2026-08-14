#pragma once
#include "phi_prop_registry.h"
#include "meshobject.h"
#include "render_settings.h"

/* Resolves a string target identifier to a live PhiPropGroup + owner
 * pointer for the DNA/RNA property system (phi_prop.h) -- the ONE place
 * this resolution rule lives, shared by both the Python bindings
 * (mp_port.c's phi.prop_get/set) and chat-driven scene mutation (main.c's
 * PKT_PROP_SET_REQUEST wire handler), rather than two independently-
 * maintained copies drifting apart -- the same risk ui.c's context-menu
 * items[]/actions[] duplication already taught this project a real
 * lesson about.
 *
 * Recognizes:
 *   "object"      -- the live MeshObject (see scene_target_register)
 *   "face"        -- the currently-selected face of that MeshObject
 *   "light:<id>"  -- the light with this id (light.h's own self-
 *                    contained registry, no registration needed here)
 *   "render"      -- the single global RenderSettings instance
 *
 * Returns 0 (out params untouched) if the target isn't currently
 * available (nothing loaded, no face selected, no light with that id,
 * render settings never registered) -- callers turn that into a real
 * error (a raised Python exception, or an error JSON reply over the
 * wire), never a silent wrong read/write. */

/* Registers the live pointers this resolver needs for "object"/"face"/
 * "render" (not "light:<id>", which reads light.h's own registry
 * directly) -- same "explicit pointers handed in, no hidden globals"
 * convention mp_port.c's own phi_mp_register_targets already uses for
 * its physics-specific bindings; call once at startup alongside it. */
void scene_target_register(MeshObject *test_obj, const int *test_obj_loaded, const int *edit_face,
                            RenderSettings *render_settings);

int scene_resolve_target(const char *target, const PhiPropGroup **out_group, void **out_owner);
