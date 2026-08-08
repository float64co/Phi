#pragma once
/* Native-only escape hatch, deliberately NOT part of phi_platform.h —
 * mirrors phi_platform_native.h's role for X11 (see that file's comment).
 * input.c's Win32 branch needs the raw HWND for cursor clip/hide. */

#include <windows.h>

HWND phi_platform_win32_window(void);
