#pragma once

/* Global offline-raytracer render settings (Phase 3, see phi.md) -- just
 * sample count for now, the one parameter explicitly asked for; real
 * enough to grow (max bounces, resolution, etc.) as Phase 3 itself gets
 * built, not a placeholder struct guessing at fields nothing reads yet.
 * A single instance, owned by main.c (g_render_settings, same "plain
 * global, explicit pointer handed to whoever needs it" convention
 * g_test_mesh_object/g_edit_face already use) -- registered with the
 * DNA/RNA property system (phi_prop_registry.c's g_phi_prop_render_
 * settings) so both the Properties panel and phi.prop_get/set("render",
 * ...) read/write the exact same struct, no separate code path. */
typedef struct {
    int samples;
} RenderSettings;
