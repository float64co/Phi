#pragma once
/* Minimal MSVC compatibility shim for <unistd.h> -- MSVC's CRT doesn't
 * ship this header at all (it's POSIX-only), unlike mingw-w64, which
 * provides a real one -- that's why this project's existing win32
 * build (via mingw-w64 gcc, see the Makefile's win32 target) has never
 * needed this: it's only relevant when compiling with genuine MSVC
 * (build.bat), which has no such compatibility layer of its own.
 *
 * Every file in the vendored MicroPython embed tree that includes
 * <unistd.h> only actually needs it for the ssize_t typedef -- verified
 * by reading each one directly, not assumed: the real POSIX functions
 * those same files also reference (read/write/close/open, for loading/
 * saving compiled .mpy bytecode from/to a raw file descriptor) are all
 * inside `#if MICROPY_READER_POSIX` / `#if MICROPY_PERSISTENT_CODE_SAVE_FILE`
 * blocks, and both of those are 0 in this project's config
 * (client/micropython_embed/py/mpconfig.h's own defaults, not overridden
 * in client/mpconfigport.h) -- so those function bodies never actually
 * compile, on any platform, mingw or MSVC. The only thing genuinely
 * needed for this project's own build is the type below.
 *
 * build.bat adds this directory to the include path LAST (after every
 * real include dir), so if a real unistd.h ever legitimately existed
 * somewhere on the include path first, it would still win over this. */
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
