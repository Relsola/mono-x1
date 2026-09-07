@echo off
setlocal enabledelayedexpansion
cd /D "%~dp0"

set source_file=%CD%\main.cc %CD%\game.cc %CD%\collision.cc
set cl_common=/std:c++20 /MTd /nologo /GR- /EHs- /EHc- /MP /Od ^
    /fp:fast /arch:AVX2 /Gv /Oi ^
    /FC /Z7 /JMC- /W4 ^
    /DUNICODE /D_UNICODE /D_DEBUG_TMP=1 /D_DEBUG_VIS=1 /D_DEBUG_BUILD=1

set cl_link=/SUBSYSTEM:WINDOWS /incremental:no /opt:ref /opt:icf

if not exist build mkdir build
if not exist build\shaders mkdir build\shaders
copy /Y shaders\*.hlsl build\shaders\ >nul
pushd build
cl %cl_common% /Fe:main.exe %source_file% /link %cl_link%
popd
