@echo off
setlocal enabledelayedexpansion
cd /D "%~dp0"

rem ---------------------------------------------------------------------------
rem Usage (argument optional, case-insensitive):
rem   build.bat            debug build - the default, unchanged
rem   build.bat release    release build (/MT /O2 /DNDEBUG /GL /LTCG, debug modules off)
rem   build.bat shaders    only recompile shaders (shaders\*.hlsl -> build\shaders\*.cso)
rem Every mode recompiles the shaders, because the runtime reads the precompiled
rem .cso and no longer compiles HLSL by itself.
rem
rem NOTE: this file must stay pure ASCII. cmd.exe reads a .bat with the local code
rem page (936 here), so UTF-8 Chinese comments get mangled and can even be run as
rem bogus commands. Same reason as test\run_tests.bat and editor\build.bat.
rem ---------------------------------------------------------------------------

set mode=debug
set badarg=
if /I "%~1"=="release" set mode=release
if /I "%~1"=="shaders" set mode=onlyshaders
if not "%~1"=="" if /I not "%~1"=="release" if /I not "%~1"=="shaders" set badarg=%~1
if defined badarg goto :usage

set source_files="%CD%\src\main.cc" ^
                 "%CD%\src\scene.cc" ^
                 "%CD%\src\game.cc" ^
                 "%CD%\src\ui.cc" ^
                 "%CD%\src\font.cc" ^
                 "%CD%\src\save.cc" ^
                 "%CD%\src\sprite.cc" ^
                 "%CD%\src\level.cc" ^
                 "%CD%\src\shared\level_asset.cc" ^
                 "%CD%\src\platform_win32.cc" ^
                 "%CD%\src\collision.cc" ^
                 "%CD%\src\camera.cc" ^
                 "%CD%\src\debug\debug_vis.cc" ^
                 "%CD%\src\debug\trace.cc" ^
                 "%CD%\src\debug\replay.cc" ^
                 "%CD%\src\input.cc" ^
                 "%CD%\src\win32_input.cc" ^
                 "%CD%\src\gameinput_input.cc" ^
                 "%CD%\src\debug\input_script.cc" ^
                 "%CD%\src\d3d12_renderer.cc" ^
                 "%CD%\src\shared\logger.cc" ^
                 "%CD%\src\audio.cc" ^
                 "%CD%\src\game_audio.cc"

rem Flags shared by both profiles.
set cl_shared=/std:c++20 /nologo /GR- /EHs- /EHc- /MP /GS- /Zc:preprocessor ^
    /utf-8 /fp:fast /arch:AVX2 /Gv /Oi ^
    /W4 /I"%CD%\include" /DUNICODE /D_UNICODE

rem Debug (default) = what every regression in test\ runs against: /Od, debug CRT,
rem file:line in log lines, all four debug modules on.
set cl_debug=/MTd /Od /FC /Z7 /JMC- ^
    /DMONO_DEBUG_TMP=1 /DMONO_DEBUG_VIS=1 /DMONO_DEBUG_BUILD=1 /DMONO_DEBUG_INPUT=1

rem Release: optimized CRT + /O2 + whole-program optimization (/GL here, /LTCG
rem at link) + /DNDEBUG. This profile exists to cut a build for shipping or to
rem measure real performance, so it is allowed to drop the safety net that the
rem debug profile keeps:
rem   - /DNDEBUG removes assert(): the conventions this project enforces with
rem     assert hold in the DEBUG profile only. static_assert is compile-time and
rem     unaffected. Watch for C4189 / C4505 when a variable or an internal
rem     function is used by asserts only - remove the leftover, do not keep
rem     NDEBUG out for it.
rem   - the four debug modules are off (no --trace / --fast / replay hotkeys /
rem     collision wireframes).
set cl_release=/MT /O2 /DNDEBUG /GL ^
    /DMONO_DEBUG_TMP=0 /DMONO_DEBUG_VIS=0 /DMONO_DEBUG_BUILD=0 /DMONO_DEBUG_INPUT=0

set cl_common=%cl_shared% %cl_debug%
if "%mode%"=="release" set cl_common=%cl_shared% %cl_release%

set cl_link=/SUBSYSTEM:WINDOWS /incremental:no /opt:ref /opt:icf
rem /GL ????????????? /LTCG ????? release ??
if "%mode%"=="release" set cl_link=%cl_link% /LTCG

if not exist build mkdir build
if not exist build\shaders mkdir build\shaders

rem ---------------------------------------------------------------------------
rem Shaders: precompiled so the runtime never compiles HLSL. fxc ships with the
rem Windows SDK and is on PATH in a vcvars prompt. vs_5_1 / ps_5_1 is the
rem D3D12-era shader model: 5.1 adds register spaces, while 6.x would need dxc
rem plus a signed DXIL blob and buys this single-quad shader nothing at all.
rem Entry point "main", no compile flags.
rem ---------------------------------------------------------------------------
fxc /nologo /T vs_5_1 /E main /Fo"build\shaders\triangle_vs.cso" "shaders\triangle_vs.hlsl"
if errorlevel 1 goto :failed
fxc /nologo /T ps_5_1 /E main /Fo"build\shaders\triangle_ps.cso" "shaders\triangle_ps.hlsl"
if errorlevel 1 goto :failed

if "%mode%"=="onlyshaders" (
    echo [build] shaders ok -^> build\shaders\*.cso
    exit /b 0
)

rem ---------------------------------------------------------------------------
pushd build
cl %cl_common% /Fe:main.exe %source_files% /link %cl_link%
set cl_exit=%errorlevel%
popd
if not "%cl_exit%"=="0" goto :failed

echo [build] ok (%mode%) -^> build\main.exe
exit /b 0

:usage
echo [build] unknown argument: %badarg%
echo [build] usage: build.bat [release ^| shaders]
exit /b 1

:failed
echo [build] BUILD FAILED
exit /b 1
