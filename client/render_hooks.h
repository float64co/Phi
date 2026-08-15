#pragma once
#include "gbuffer.h"   /* GBuffer is an anonymous struct typedef (no tag name), so it must be
                         * included here, not forward-declared -- matches this file's own design
                         * philosophy below (game/src/ *.c callbacks get the real struct, not a
                         * faked opaque handle), so there's nothing to hide by forward-declaring
                         * anyway. */

/* Real C-level render-pass insertion points -- Phase 0's originally-
 * sketched "user-defined passes" mechanism (phi.md's Deferred Renderer
 * section shows a Python `@phi.render_pass(insert="after_gbuffer")`
 * decorator), landed here as the C half only: a game/src/ *.c author
 * (Phase 9's public C API, see phi.h) can register a plain C callback at
 * one of these 4 named points and have it actually invoked mid-pipeline.
 * The Python decorator itself is NOT built here -- that needs MicroPython
 * binding work of its own and wasn't part of what this pass asked for;
 * this is the C-level foundation it would eventually call into.
 *
 * Honest note on the point names: phi.md's original diagram sketched
 * these against an ASSUMED pipeline order (lighting+transparent, then
 * taa+bloom, then tonemap, then fxaa) that the real implementation
 * (gbuffer.c's gbuffer_resolve) evolved past -- the real order is
 * Lighting -> Bloom -> Transparency -> Tonemap -> TAA -> FXAA. Rather
 * than silently pretend the old diagram still matches, each point below
 * is documented against where it ACTUALLY fires in the real pipeline,
 * picking the closest real boundary to each name's original intent:
 *
 *   PHI_HOOK_AFTER_GBUFFER   -- after geometry+shadow passes, before
 *                                Lighting begins. gb's albedo/normal/
 *                                material/emissive/depth/object_id
 *                                textures are all populated; nothing
 *                                has been lit yet.
 *   PHI_HOOK_AFTER_LIGHTING  -- after Lighting, Bloom, AND Transparency
 *                                all complete (they share gb->hdr_fbo),
 *                                before Tonemap. gb->hdr_tex holds the
 *                                full HDR-composited scene.
 *   PHI_HOOK_AFTER_RESOLVE   -- after Tonemap, before TAA. gb's LDR
 *                                target (gb->ldr_fbo/ldr_tex) holds the
 *                                tonemapped-but-not-yet-temporally-
 *                                resolved frame.
 *   PHI_HOOK_AFTER_TONEMAP   -- after FXAA, the very last point before
 *                                the frame is presented (matches the
 *                                original diagram's own placement of
 *                                this one specifically -- "after [fxaa],
 *                                final output").
 *
 * Each callback receives the real, concrete GBuffer* gbuffer.c/gbuffer.h
 * already pass around internally -- not an opaque/sandboxed handle. This
 * is deliberate, not an oversight: game/src/ *.c is compiled and
 * statically linked directly into the same binary as the engine core
 * (see phi.h's own header comment on Phase 9's distribution model), so a
 * callback here is already fully-trusted code in the same process, not
 * an isolated plugin -- there is no real safety boundary to build for
 * that case, only an API-shape one, so handing back the actual struct
 * (raw GL texture/FBO handles included) is honest about what's really
 * happening rather than pretending a thin, less-useful wrapper is a
 * meaningful sandbox. */

typedef enum {
    PHI_HOOK_AFTER_GBUFFER = 0,
    PHI_HOOK_AFTER_LIGHTING,
    PHI_HOOK_AFTER_RESOLVE,
    PHI_HOOK_AFTER_TONEMAP,
    PHI_HOOK_POINT_COUNT,
} PhiRenderHookPoint;

typedef void (*PhiRenderHookFn)(GBuffer *gbuf, void *userdata);

#define PHI_RENDER_HOOKS_MAX_PER_POINT 8

/* Clears every registered hook at every point -- call once at startup,
 * same registry-init convention as scene_objects_init/phi_graph_system_
 * init. */
void render_hooks_init(void);

/* Registers fn to be called (with userdata passed through unchanged)
 * every time `point` fires during gbuffer_resolve. Returns 1 on success,
 * 0 if that point's registry is already at PHI_RENDER_HOOKS_MAX_PER_POINT
 * (a real, checked bound -- not silently dropped, not unbounded growth). */
int render_hooks_register(PhiRenderHookPoint point, PhiRenderHookFn fn, void *userdata);

/* Removes a specific (fn, userdata) registration -- returns 1 if one was
 * found and removed, 0 otherwise. Exact-match on both fn and userdata,
 * so multiple registrations of the same fn with different userdata can
 * be told apart and removed independently. */
int render_hooks_unregister(PhiRenderHookPoint point, PhiRenderHookFn fn, void *userdata);

/* Invokes every hook registered at `point`, in registration order,
 * passing gbuf through to each. gbuffer.c calls this at the 4 real
 * pipeline boundaries documented above -- not meant to be called from
 * anywhere else. A no-op (cheap: one array-length check) when nothing is
 * registered at that point, so this is safe to call unconditionally on
 * every frame even before any game/src/ *.c code has registered anything. */
void render_hooks_invoke(PhiRenderHookPoint point, GBuffer *gbuf);
