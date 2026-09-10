@echo off
setlocal enabledelayedexpansion
cd /D "%~dp0"

set source_files="%CD%\src\main.cc" "%CD%\src\game.cc" "%CD%\src\collision.cc" "%CD%\src\logger.cc"
set cl_common=/std:c++20 /MTd /nologo /GR- /EHs- /EHc- /MP /Od /Zc:preprocessor ^
    /fp:fast /arch:AVX2 /Gv /Oi ^
    /FC /Z7 /JMC- /W4 /I"%CD%\include" ^
    /DUNICODE /D_UNICODE /DMONO_DEBUG_TMP=1 /DMONO_DEBUG_VIS=1 /DMONO_DEBUG_BUILD=1

set cl_link=/SUBSYSTEM:WINDOWS /incremental:no /opt:ref /opt:icf

if not exist build mkdir build
if not exist build\shaders mkdir build\shaders
copy /Y shaders\*.hlsl build\shaders\ >nul
pushd build
cl %cl_common% /Fe:main.exe %source_files% /link %cl_link%
popd
