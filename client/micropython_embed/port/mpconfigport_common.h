/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2022-2023 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>

// Type definitions for the specific machine

typedef long mp_off_t;

// Need to provide a declaration/definition of alloca()
#if defined(__FreeBSD__) || defined(__NetBSD__)
// BSD
#include <stdlib.h>
#elif defined(_WIN32)
// Windows
#include <malloc.h>
#else
// Other OS
#include <alloca.h>
#endif

// mpconfig.h's own endianness autodetection checks __BYTE_ORDER__ (a GCC/
// clang builtin) and falls back to #include <endian.h> if that's undefined
// -- MSVC defines neither, and doesn't ship <endian.h> at all (it's POSIX-
// only). x86/x64 Windows is always little-endian, so short-circuit that
// whole autodetection by defining MP_ENDIANNESS_LITTLE directly (mpconfig.h
// skips its own detection entirely once either MP_ENDIANNESS_LITTLE or
// MP_ENDIANNESS_BIG is already defined by the port).
#if defined(_WIN32)
#define MP_ENDIANNESS_LITTLE (1)
#endif

// mpconfig.h's own MP_NORETURN fallback (`#ifndef`-guarded, so this port
// override takes priority) is `__attribute__((noreturn))`, GCC/clang-only
// syntax real MSVC's C front end doesn't parse at all -- MSVC's equivalent
// is __declspec(noreturn), placed the same way (before the return type),
// which is exactly how misc.h's own `MP_NORETURN void m_malloc_fail(...)`
// already uses this macro.
#if defined(_MSC_VER)
#define MP_NORETURN __declspec(noreturn)
#endif

// Same story for MP_LIKELY/MP_UNLIKELY -- mpconfig.h's own (`#ifndef`-
// guarded) fallback wraps __builtin_expect, a GCC/clang compiler builtin
// with no MSVC equivalent. cl.exe doesn't error on the undeclared-function
// call (just warns and assumes an extern returning int), but nothing
// actually defines a linkable __builtin_expect symbol anywhere, so it
// fails at LINK time instead. These are optimizer hints only, never
// required for correctness, so a plain passthrough is a correct (if
// slower) MSVC substitute.
#if defined(_MSC_VER)
#define MP_LIKELY(x) (x)
#define MP_UNLIKELY(x) (x)
#endif

// Same story again for MP_NOINLINE/MP_ALWAYSINLINE -- mpconfig.h's own
// (`#ifndef`-guarded) fallbacks are __attribute__((noinline)) and
// __attribute__((always_inline)), neither understood by real MSVC.
// __declspec(noinline)/__forceinline are the direct MSVC equivalents,
// used the same way (as a modifier on the function declaration).
// MP_WEAK (__attribute__((weak))) is deliberately NOT overridden here --
// grepped: nothing in this project's vendored MicroPython tree actually
// uses it, only mpconfig.h's own definition, so it's dead code on every
// platform and MSVC has no simple drop-in for real weak-symbol linkage
// anyway (would need /alternatename linker tricks, not a macro).
#if defined(_MSC_VER)
#define MP_NOINLINE __declspec(noinline)
#define MP_ALWAYSINLINE __forceinline
#endif

#define MICROPY_MPHALPORT_H "port/mphalport.h"
