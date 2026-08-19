#pragma once
/* Forced include (see build.bat's /FI flag -- applied to every translation
 * unit in that one `%CL% @%RSPFILE%` invocation, Bullet/MicroPython
 * included, harmless there since it's just windows.h) ensuring
 * <windows.h> is always included before any file's own <GL/gl.h>.
 *
 * Real MSVC's Windows-SDK-shipped GL/gl.h assumes WINGDIAPI/APIENTRY are
 * already defined by windows.h -- without it, cl.exe fails with syntax
 * errors on every declaration in the header (e.g. "expected '(' to follow
 * WINGDIAPI"). mingw-w64's own GL/gl.h defensively #includes windows.h
 * itself when WIN32 is defined, which is why the Makefile's win32 target
 * (mingw-w64 via WSL) never needed this. This codebase's ~13 cross-
 * platform files that #include <GL/gl.h> directly (octree_render.c,
 * renderer.c, texture_cache.c, gl_native.c/.h, editor_main.c,
 * player_main.c, ...) have no reason to know or care about that
 * MSVC-specific ordering requirement themselves, so it's forced in here
 * instead of touching each one.
 *
 * WIN32_LEAN_AND_MEAN matters for correctness, not just size: without it,
 * windows.h pulls in the old <winsock.h>, which then conflicts with
 * ws_client_win32.c's own (correctly-ordered, winsock2-first) <winsock2.h>
 * -- WIN32_LEAN_AND_MEAN suppresses windows.h's winsock.h include, leaving
 * winsock2.h as the only Winsock definition anywhere in the program. GDI
 * (HDC/HGLRC/wgl*, what phi_platform_win32.c actually needs from
 * windows.h) is unaffected by WIN32_LEAN_AND_MEAN -- it only excludes
 * things like Cryptography/DDE/RPC/Shell/Winsock, not GDI/USER32.
 *
 * NOMINMAX matters for the same "every TU, including ones that never
 * asked for windows.h" reason: without it, windows.h's own min/max
 * function-like macros shadow every other min/max in the program,
 * including plain C++ identifiers named min/max in the vendored Bullet
 * headers (e.g. BulletCollision/CollisionShapes/btHeightfieldTerrainShape.h)
 * that have nothing to do with Windows at all -- this only surfaces once
 * windows.h is forced into their translation units too. */
#if defined(_MSC_VER)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
