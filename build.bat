@echo off
setlocal enabledelayedexpansion
REM ============================================================
REM  Phi -- Windows build via real MSVC (cl.exe), no CMake.
REM
REM  Mirrors the Makefile's `native`/`player` targets as closely as
REM  possible (see Makefile's ENGINE_CORE_SRCS/NATIVE_*/PLAYER_*),
REM  translated to MSVC's own flag syntax and the win32 platform/audio/
REM  gamepad backends (phi_platform_win32.c, ws_client_win32.c,
REM  audio_win32_stub.c, input_gamepad_win32_stub.c) in place of the
REM  Makefile's own Xlib/GLX-based native ones. Run from the repo root --
REM  either from a "Developer Command Prompt for VS" (cl.exe already on
REM  PATH) or a plain cmd.exe (this script will locate and invoke
REM  vcvarsall.bat itself via vswhere.exe if cl.exe isn't already
REM  available).
REM
REM  Usage:
REM    build.bat            -- builds the editor (build\phi_win32.exe)
REM    build.bat editor      -- same as above, explicit
REM    build.bat player      -- builds the shipped-game player
REM                             (build\phi_player_win32.exe), linking
REM                             player_main.c + whatever's in game\src\
REM                             instead of editor_main.c -- same
REM                             Editor/Player split as the Makefile's
REM                             own `native` vs `player` targets (see
REM                             phi.md's "Editor/Player split").
REM
REM  REAL PORTABILITY GAPS BETWEEN MINGW-W64/GCC AND PLAIN MSVC, found by
REM  reading the vendored/engine source directly (not guessed):
REM
REM  1. FIXED: several vendored MicroPython files unconditionally
REM     `#include <unistd.h>` (a POSIX header mingw-w64 ships a
REM     compatibility version of, but plain MSVC's CRT does not ship at
REM     all). Confirmed by direct inspection that the only thing any of
REM     them actually need from it is the ssize_t typedef -- the real
REM     POSIX read()/write()/close() calls those same files also
REM     reference are all inside `#if MICROPY_READER_POSIX` / `#if
REM     MICROPY_PERSISTENT_CODE_SAVE_FILE` blocks, both 0 in this
REM     project's own mpconfigport.h, so those bodies never actually
REM     compile on any toolchain. Fixed with a 3-line compatibility
REM     header (client\msvc_compat\unistd.h) added to the include path
REM     LAST, so a real unistd.h would still win if one ever existed.
REM
REM  2. FIXED: MicroPython's own endianness autodetection
REM     (micropython_embed\py\mpconfig.h) checks the GCC/clang builtin
REM     __BYTE_ORDER__ and falls back to `#include <endian.h>` (another
REM     POSIX header MSVC doesn't ship) when that's undefined -- MSVC
REM     defines neither. Fixed at the port-config level instead of with
REM     an endian.h shim: client\micropython_embed\port\
REM     mpconfigport_common.h now defines MP_ENDIANNESS_LITTLE directly
REM     under `#if defined(_WIN32)`, which short-circuits mpconfig.h's
REM     own autodetection entirely (x86/x64 Windows is always
REM     little-endian).
REM
REM  3. FIXED: phi_platform_win32.c includes <GL/wglext.h> for
REM     wglCreateContextAttribsARB (needed to create a modern OpenGL 3.3
REM     core-profile context), and gl_native.c/.h include <GL/glext.h> for
REM     ~40 core-profile function-pointer typedefs (glCreateShader,
REM     glBindBuffer, etc.) -- both real Khronos OpenGL-registry headers,
REM     not part of the Windows SDK/MSVC and not vendored anywhere in this
REM     repo. Fixed with minimal compatibility headers
REM     (client\msvc_compat\GL\wglext.h, client\msvc_compat\GL\glext.h)
REM     defining just the symbols this codebase actually uses (real,
REM     stable, publicly-documented extension-spec values/signatures, not
REM     placeholders), added to the include path LAST so real ones would
REM     still win if they ever existed.
REM
REM  4. FIXED: ~13 cross-platform files (octree_render.c, renderer.c,
REM     texture_cache.c, gl_native.c/.h, editor_main.c, player_main.c,
REM     ...) `#include <GL/gl.h>` directly. mingw-w64's own GL/gl.h
REM     defensively includes <windows.h> itself under WIN32, but real
REM     MSVC's Windows-SDK-shipped GL/gl.h assumes WINGDIAPI/APIENTRY are
REM     ALREADY defined by windows.h having been included first --
REM     without it, cl.exe fails with syntax errors on every declaration
REM     in the header. Fixed by force-including a small prefix header
REM     (client\msvc_compat\win32_gl_prefix.h, via the /FI flag below)
REM     into every translation unit, rather than touching 13 otherwise-
REM     platform-agnostic files -- it #includes <windows.h> itself (with
REM     WIN32_LEAN_AND_MEAN, so it doesn't drag in the legacy <winsock.h>
REM     that would conflict with ws_client_win32.c's own, correctly-
REM     ordered <winsock2.h>) under `#if defined(_MSC_VER)` only, so
REM     mingw-w64 builds are untouched.
REM
REM  5. FIXED: this script's own client source list had silently drifted
REM     from the Makefile's ENGINE_CORE_SRCS -- `main.c` (a file that no
REM     longer exists; the real entry points are editor_main.c/
REM     player_main.c) instead of the real editor/player split, and
REM     missing texture_cache.c/node_graph.c/audio_wav.c/render_hooks.c/
REM     skinned_scene_objects.c entirely (5 real engine-core files that
REM     would otherwise mean undefined-symbol link errors, or in
REM     texture_cache.c's case -- since other engine-core files call its
REM     functions -- simply never compiling at all). Confirmed by diffing
REM     this list against the Makefile's own ENGINE_CORE_SRCS directly,
REM     not assumed in sync.
REM
REM  6. FIXED: MicroPython's MP_NORETURN/MP_NOINLINE/MP_ALWAYSINLINE
REM     (`#ifndef`-guarded fallbacks in mpconfig.h) expand to
REM     __attribute__((noreturn))/((noinline))/((always_inline)), and
REM     MP_LIKELY/MP_UNLIKELY wrap __builtin_expect -- all GCC/clang-only,
REM     none understood by real MSVC (__builtin_expect in particular
REM     doesn't even fail the compile, just warns "assuming extern
REM     returning int" and fails at LINK time instead, since nothing
REM     defines that symbol). Fixed at the port-config level, same file/
REM     pattern as gap #2: client\micropython_embed\port\
REM     mpconfigport_common.h overrides all four under `#if defined(_MSC_VER)`
REM     with their real MSVC equivalents (__declspec(noreturn),
REM     __declspec(noinline), __forceinline) or, for the branch-prediction
REM     hints, a plain passthrough (correct since they're optimizer hints
REM     only, never required for correctness).
REM
REM  7. FIXED: renderer.c/transform_op.c use M_PI directly -- MSVC's
REM     math.h only defines M_PI (and friends) when _USE_MATH_DEFINES is
REM     set before math.h is first included; glibc/mingw's math.h defines
REM     it unconditionally. Fixed with a plain /D_USE_MATH_DEFINES
REM     compiler flag below.
REM
REM  8. FIXED: forcing <windows.h> into every translation unit for gap #4
REM     surfaced a second-order issue: windows.h's own min/max function-
REM     like macros collided with plain min/max identifiers in the
REM     vendored Bullet headers (e.g. btHeightfieldTerrainShape.h), which
REM     never saw windows.h at all before gap #4's fix. NOMINMAX (in
REM     win32_gl_prefix.h, alongside WIN32_LEAN_AND_MEAN) suppresses those
REM     macros -- the standard fix for mixing windows.h with C++ code.
REM
REM  9. FIXED: this script's own CL environment variable (`set CL=cl.exe`)
REM     collided with cl.exe's OWN reserved use of an env var literally
REM     named CL -- cl.exe silently prepends %CL%'s contents as extra
REM     command-line arguments to every invocation of itself, so naming
REM     this script's variable CL fed cl.exe's own name back to it as a
REM     bogus source file on every single compile. Harmless at compile
REM     time (just the "unrecognized source file type 'cl.exe', object
REM     file assumed" D9024 warning seen on every file), but fatal at
REM     link time, where the linker inherited the same bogus "cl.exe"
REM     input and failed with LNK1181 ("cannot open input file 'cl.exe'")
REM     after otherwise successfully compiling the ENTIRE source list.
REM     Traced from that final link error back to its root cause, not
REM     guessed -- fixed by renaming the variable to CLEXE throughout.
REM
REM  Two real BUGS also found and fixed along the way, unrelated to MSVC
REM  portability (would break on any compiler that actually tried to
REM  build these files, which apparently nothing had before this pass):
REM    - client\audio_win32_stub.c's own top comment contained a literal
REM      `*/` mid-sentence ("phi_audio_play*/stop call"), which closed the
REM      real /* ... */ block comment early and left the rest of the
REM      comment's prose to be parsed as real C tokens.
REM    - client\skinned_scene_objects.c calls glDeleteBuffers (GL 1.5)
REM      without including gl_native.h, unlike every sibling GL file in
REM      this codebase -- opengl32.dll's export table is frozen at GL 1.1,
REM      so anything newer needs gl_native.h's wglGetProcAddress-based
REM      fetching or it's an unresolved external symbol at link time on
REM      Windows specifically (an implicit-declaration warning, not an
REM      error, is all this produces on Linux, where it happens to
REM      resolve against libGL.so directly).
REM
REM  REMAINING UNVERIFIED GAP, FLAGGED HONESTLY: this codebase uses C99
REM  compound-literal syntax extensively (`(Vec3f){1,2,3}`-shaped
REM  expressions, in ~38 client/ files) and, in a couple of spots
REM  (client\editor_main.c and client\player_main.c, both building a
REM  PhiPlatformConfig), a C99 designated initializer
REM  (`{ .title = "Phi", ... }`). GCC/mingw-w64/clang all accept both as
REM  standard C99 (or as GNU extensions in C++ mode); real MSVC's C
REM  front end has a long, well-documented history of NOT supporting
REM  compound literals in particular, even under /std:c11 or /std:c17
REM  (designated initializers fare somewhat better but are not
REM  guaranteed either). This script cannot verify whether that's still
REM  true of whatever MSVC version you have installed. If compilation
REM  fails with syntax errors pointing at lines shaped like
REM  `(SomeType){...}`, the most likely real fix is switching CL below
REM  to clang-cl.exe instead (accepts the exact same command-line flags
REM  cl.exe does; installable via the Visual Studio Installer's optional
REM  "C++ Clang tools for Windows" component, or standalone LLVM) --
REM  clang-cl supports compound literals as the real GNU extension they
REM  are. Not pre-applied here since it changes which compiler actually
REM  runs; flagged so it's a five-second fix instead of a confusing dead
REM  end.
REM ============================================================

set TARGET=editor
if /i "%~1"=="player" set TARGET=player
if /i "%~1"=="editor" set TARGET=editor

REM NOT named CL -- cl.exe treats an environment variable literally named
REM CL (and _CL_) as extra command-line options it silently prepends to
REM EVERY invocation of itself. Naming this variable CL means cl.exe's own
REM name (the string "cl.exe") gets fed back to it as a bogus extra
REM argument on every single compile -- harmless at compile time (just the
REM "unrecognized source file type 'cl.exe', object file assumed" warning
REM seen throughout this build), but fatal at link time, where the linker
REM inherits that same injected "cl.exe" as a bogus input file it can't
REM open (LINK1181). Found by tracing D9024 all the way to the final
REM LNK1181 -- not guessed.
set CLEXE=cl.exe

where %CLEXE% >nul 2>nul
if errorlevel 1 (
    echo [build.bat] %CLEXE% not found on PATH -- looking for a Visual Studio installation via vswhere...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo [build.bat] vswhere.exe not found. Either run this from a "Developer Command Prompt for VS", or install Visual Studio / Build Tools with the "Desktop development with C++" workload.
        exit /b 1
    )
    set "VSINSTALL="
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
    if not defined VSINSTALL (
        echo [build.bat] No Visual Studio installation with the C++ ^(VC.Tools.x86.x64^) workload was found.
        exit /b 1
    )
    call "!VSINSTALL!\VC\Auxiliary\Build\vcvarsall.bat" x64
    if errorlevel 1 exit /b 1
    echo [build.bat] found and set up: !VSINSTALL!
)

set SRCDIR=client
set BUILDDIR=build
set RSPFILE=%BUILDDIR%\build.rsp
if "%TARGET%"=="player" (
    set OUT=%BUILDDIR%\phi_player_win32.exe
) else (
    set OUT=%BUILDDIR%\phi_win32.exe
)

if not exist %BUILDDIR% mkdir %BUILDDIR%
if exist %RSPFILE% del %RSPFILE%

REM ---- Compiler flags + includes (matches Makefile's NATIVE_CFLAGS) ----
REM /EHsc: standard C++ exception model, needed for the vendored Bullet
REM C++ sources and client\phi_physics.cpp. msvc_compat is last on
REM purpose (see gaps #1-3 above).
echo /nologo>> %RSPFILE%
echo /O2>> %RSPFILE%
echo /W3>> %RSPFILE%
echo /EHsc>> %RSPFILE%
REM MSVC's math.h only defines M_PI (and friends) when _USE_MATH_DEFINES
REM is set before it's first included -- glibc/mingw's math.h defines
REM M_PI unconditionally, which is why this was never needed for the
REM Makefile's own win32 (mingw-w64) target. renderer.c/transform_op.c
REM (at least) use M_PI directly.
echo /D_USE_MATH_DEFINES>> %RSPFILE%
echo /I%SRCDIR%>> %RSPFILE%
echo /I%SRCDIR%\micropython_embed>> %RSPFILE%
echo /I%SRCDIR%\micropython_embed\port>> %RSPFILE%
echo /I%SRCDIR%\vendor\bullet3\src>> %RSPFILE%
echo /I%SRCDIR%\msvc_compat>> %RSPFILE%
REM /FI forces win32_gl_prefix.h into every translation unit (see that
REM file's own comment for why: real MSVC's Windows-SDK GL/gl.h needs
REM windows.h included first, unlike mingw-w64's own gl.h). Bare filename
REM only, NOT a client\msvc_compat\-prefixed path: /FI's search order
REM checks the directory of the file being compiled FIRST (which differs
REM per translation unit -- client\, the vendored bullet3 tree, game\src\,
REM ...), only falling back to /I dirs second, so a baked-in relative
REM path resolves wrong for every TU not sitting directly in the repo
REM root. The bare name resolves correctly for every TU via the
REM /I%SRCDIR%\msvc_compat entry already added above.
echo /FIwin32_gl_prefix.h>> %RSPFILE%

REM ---- Engine-core sources (matches Makefile's ENGINE_CORE_SRCS, in the
REM same order) -- hand-listed since this is a short, stable list the
REM Makefile itself also hand-lists; Bullet/MicroPython below are NOT
REM hand-listed since those are 100s of vendored files best discovered
REM the same way the Makefile's own wildcard does it, not hand-copied
REM into a second list that could silently drift out of sync with what's
REM actually vendored. Shared by both the editor and player targets --
REM same as the Makefile's NATIVE_SRCS/PLAYER_SRCS both starting from
REM ENGINE_CORE_SRCS. ----
for %%f in (
    octree_render.c renderer.c net.c input.c console.c
    asset_browser.c chat.c halfedge.c halfedge_gltf.c texture_cache.c meshobject.c
    mesh_edit.c node_graph.c audio_wav.c render_hooks.c fracture.c armature.c animation.c skinned_mesh.c
    gizmo.c transform_op.c light.c scene_target.c scene_objects.c fracture_body.c path_tracer.c skinned_mesh_object.c skinned_scene_objects.c ragdoll.c font.c svg_icon.c ui.c area_tree.c phi_prop.c
    phi_prop_registry.c mp_port.c phi_physics.cpp
) do echo "%SRCDIR%\%%f">> %RSPFILE%

REM ---- Win32 platform backend, shared by editor and player (matches
REM audio_win32_stub.c/input_gamepad_win32_stub.c standing in for the
REM Makefile's SDL2-backed audio_native.c/input_gamepad_native.c --
REM real, honest "reports nothing" stubs, not silently broken ones, see
REM each file's own top comment). http_client_native.c is deliberately
REM NOT built here: it's POSIX-sockets-only (see its own header comment),
REM and editor_main.c already guards every call to it behind
REM `#if !defined(__EMSCRIPTEN__) && !defined(_WIN32)` / PHI_HAVE_HTTP_CLIENT,
REM so the Asset Browser's Create flow just reports itself unavailable on
REM this target instead of failing to link. ----
for %%f in (
    phi_platform_win32.c gl_native.c gbuffer.c ws_client_win32.c
    audio_win32_stub.c input_gamepad_win32_stub.c
) do echo "%SRCDIR%\%%f">> %RSPFILE%

if "%TARGET%"=="player" (
    echo "%SRCDIR%\player_main.c">> %RSPFILE%

    REM ---- Whatever the user has actually placed in game\src\ -- a real
    REM directory walk, evaluated at build.bat invocation time, not a
    REM fixed list (matches the Makefile's own GAME_SRC_FILES :=
    REM $(wildcard game/src/*.c)) so a game with no game\src\ at all
    REM (a pure game\main.py game) still builds cleanly. ----
    if exist game\src\*.c (
        for %%f in (game\src\*.c) do echo "%%f">> %RSPFILE%
    )

    REM ---- PHI_GAME_HAS_C_ENTRY, defined iff game\src\main.c
    REM specifically exists -- matches the Makefile's own
    REM PHI_GAME_HAS_C_ENTRY, see player_main.c's own top comment for why
    REM that one file's presence (not just "some game\src\ files exist")
    REM is what selects the C-entry-point code path over the game\main.py
    REM one. ----
    if exist game\src\main.c echo /DPHI_GAME_HAS_C_ENTRY>> %RSPFILE%
) else (
    echo "%SRCDIR%\editor_main.c">> %RSPFILE%
)

REM ---- Bullet (LinearMath+BulletCollision+BulletDynamics only -- see
REM phi.md's Phase 2 status for why only those three subtrees are
REM vendored) -- same two-directory-level shape as the Makefile's own
REM $(wildcard $(BULLET_DIR)/*/*.cpp) $(wildcard $(BULLET_DIR)/*/*/*.cpp),
REM just walked recursively here since nothing is vendored deeper than
REM that (verified directly, not assumed) so a full recursive walk finds
REM exactly the same file set. ----
for /r %SRCDIR%\vendor\bullet3\src %%f in (*.cpp) do echo "%%f">> %RSPFILE%

REM ---- MicroPython embed tree -- same reasoning as Bullet above. ----
for /r %SRCDIR%\micropython_embed %%f in (*.c) do echo "%%f">> %RSPFILE%

REM ---- Output + link (matches Makefile's NATIVE_LDFLAGS, minus
REM -lX11/-lpthread/-ldl/-lrt/ALSA (Linux-only) and -lstdc++ (MSVC links
REM its own C++ runtime automatically), plus the real Win32 import libs
REM phi_platform_win32.c/gl_native.c/ws_client_win32.c need). ----
echo /Fe:%OUT%>> %RSPFILE%
echo /link>> %RSPFILE%
echo opengl32.lib>> %RSPFILE%
echo gdi32.lib>> %RSPFILE%
echo user32.lib>> %RSPFILE%
echo kernel32.lib>> %RSPFILE%
echo ws2_32.lib>> %RSPFILE%
echo bcrypt.lib>> %RSPFILE%

echo [build.bat] building target: %TARGET%
echo [build.bat] compiling (pulls in the full vendored Bullet + MicroPython trees -- expect this to take a while, same reason the Makefile's own native/player targets are the slowest builds in this project)...
%CLEXE% @%RSPFILE%
if errorlevel 1 (
    echo.
    echo [build.bat] build FAILED -- see the portability gaps documented at the top of this script before debugging further.
    exit /b 1
)
echo [build.bat] build complete -^> %OUT%
