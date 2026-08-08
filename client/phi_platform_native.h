#pragma once
/* Native-only escape hatch, deliberately NOT part of phi_platform.h.
 *
 * phi_platform.h stays cross-platform (no Xlib types visible to code that
 * might compile on wasm). input.c's native branch needs the raw Display/
 * Window handles to grab the pointer and hide the cursor for FPS-style
 * mouse look, so phi_platform_native.c exposes them here instead — this
 * header is only ever included by native-only translation units. */

#include <X11/Xlib.h>

Display *phi_platform_native_display(void);
Window   phi_platform_native_window(void);
