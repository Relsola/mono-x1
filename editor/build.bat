@echo off
setlocal enabledelayedexpansion

rem NOTE: this file must stay pure ASCII. cmd.exe reads a .bat with the local
rem code page (936 here), so UTF-8 Chinese comments get mangled and can even be
rem executed as bogus commands. Same reason as test\run_tests.bat.

rem The editor is a separate program: output is build\editor.exe, it does not
rem touch the game's build\main.exe. Works from any cwd (cd's to repo root).
cd /D "%~dp0.."
set root=%CD%

rem ---------------------------------------------------------------------------
rem our sources
set editor_sources="%root%\editor\src\editor_main.cc" ^
                   "%root%\editor\src\editor_doc.cc" ^
                   "%root%\editor\src\editor_edit.cc" ^
                   "%root%\editor\src\editor_ui.cc"

rem sources shared with the game (compiled twice on purpose; a static library
rem for a handful of translation units is not worth the extra moving part)
set shared_sources="%root%\src\shared\level_asset.cc" ^
                   "%root%\src\level.cc" ^
                   "%root%\src\collision.cc" ^
                   "%root%\src\platform_win32.cc" ^
                   "%root%\src\shared\logger.cc"

rem ---------------------------------------------------------------------------
rem third party (Dear ImGui): separate cl invocation with /W0.
rem warning(push,0) cannot wrap another translation unit, so this is the only way.
set imgui_sources="%root%\third_party\imgui\imgui.cpp" ^
                  "%root%\third_party\imgui\imgui_draw.cpp" ^
                  "%root%\third_party\imgui\imgui_tables.cpp" ^
                  "%root%\third_party\imgui\imgui_widgets.cpp" ^
                  "%root%\third_party\imgui\imgui_demo.cpp" ^
                  "%root%\third_party\backends\imgui_impl_win32.cpp" ^
                  "%root%\third_party\backends\imgui_impl_dx11.cpp"

set imgui_objs=imgui.obj imgui_draw.obj imgui_tables.obj imgui_widgets.obj imgui_demo.obj ^
               imgui_impl_win32.obj imgui_impl_dx11.obj

rem ---------------------------------------------------------------------------
rem editor\include holds this program's own headers; src\ holds only .cc
set includes=/I"%root%\include" /I"%root%\editor\include" /I"%root%\third_party\imgui" /I"%root%\third_party\backends"

rem NOTE: source files are UTF-8 without BOM, so /utf-8 is required - without it MSVC
rem reads them as the local code page (936) and wide string literals (window title, message
rem boxes) come out as mojibake. Narrow literals only survived by an accidental round-trip.
set cl_base=/std:c++20 /MTd /nologo /GR- /EHs- /EHc- /MP /Od /Zc:preprocessor ^
    /utf-8 /fp:fast /arch:AVX2 /Gv /Oi /FC /Z7 /JMC- %includes% /DUNICODE /D_UNICODE

rem our code /W4, third party /W0 (never lower our own bar for someone else's warnings)
set cl_our=%cl_base% /W4
set cl_third=%cl_base% /W0

set cl_link=/SUBSYSTEM:WINDOWS /incremental:no /opt:ref /opt:icf

if not exist build mkdir build
if not exist build\editor mkdir build\editor

rem objects go to build\editor\ because the shared sources have the same .obj
rem names as the game's copy - keeping them apart avoids clobbering each other
pushd build\editor

cl %cl_third% /c %imgui_sources%
if errorlevel 1 goto :failed

cl %cl_our% /Fe:..\editor.exe %editor_sources% %shared_sources% %imgui_objs% /link %cl_link%
if errorlevel 1 goto :failed

popd
echo [editor] ok -^> build\editor.exe
exit /b 0

:failed
popd
echo [editor] BUILD FAILED
exit /b 1
