@echo off
rem ============================================================
rem EDITOR regression runner. No arguments.
rem
rem   editor\test\run_tests.bat
rem
rem Where things live (this split is deliberate - do not mix them up):
rem   game cases + runner   ->  test\run_tests.bat      (baseline / save_roundtrip / smoke)
rem   editor runner         ->  editor\test\run_tests.bat (this file)
rem   editor cases          ->  built into build\editor.exe (--selftest)
rem
rem It runs the editor's two headless entry points:
rem
rem   1. --selftest
rem      editor_edit's interaction cases (hit test / drag / snap),
rem      editor_doc's undo cases, and the asset save->load->compare case.
rem      No window, no D3D, no ImGui. Per-case lines go to editor.log
rem      ("[selftest] ok ..." / "[selftest] FAILED ...").
rem
rem   2. --check --smoke
rem      loads every data\map\*.bin, runs the semantic validation and the
rem      compile diagnostics, writes the report to build\editor_check.txt,
rem      then launches build\main.exe --fast input_script test\smoke.txt and
rem      folds its exit code in.
rem
rem So one invocation answers: are the editing rules right (no window),
rem are the assets valid and compilable, and can the game still load and
rem run them. It does NOT build (run build.bat and editor\build.bat first)
rem and it does NOT cover the UI translation layer (screen -> world):
rem that needs a real window and a real mouse click, see editor\README.md.
rem
rem Exit code: 0 = both steps passed, 1 = a step failed or the editor
rem binary is missing. On failure the relevant excerpt is printed (the
rem "[selftest] FAILED" lines from editor.log, the "RESULT:" / "[ERROR]"
rem lines from build\editor_check.txt) and the full files are left in
rem place for a closer look.
rem
rem ASCII-only on purpose: cmd reads a .bat with the local code page (936
rem here), so UTF-8 Chinese text would be mis-decoded (AGENTS.md 4.5).
rem Chinese notes live in editor\README.md.
rem ============================================================
setlocal
cd /D "%~dp0..\.."

if not exist "build\editor.exe" (
    echo [error] build\editor.exe not found - run editor\build.bat first
    exit /b 1
)

if not exist build mkdir build
set PASSED=0
set FAILED=0

rem ---- 1. editing rules / undo / asset round trip (no window) ----
echo [editor-test] selftest
"build\editor.exe" --selftest
if errorlevel 1 goto :selftest_failed
echo [editor-test] selftest PASS
set /A PASSED+=1
goto :check

:selftest_failed
echo [editor-test] selftest FAILED
if exist editor.log findstr /C:"[selftest] FAILED" editor.log
set /A FAILED+=1

rem ---- 2. assets + validation + compile diagnostics, then smoke ----
:check
echo.
echo [editor-test] check --smoke
"build\editor.exe" --check --smoke
if errorlevel 1 goto :check_failed
echo [editor-test] check --smoke PASS
set /A PASSED+=1
goto :summary

:check_failed
echo [editor-test] check --smoke FAILED
if exist build\editor_check.txt findstr /C:"RESULT:" /C:"[ERROR]" build\editor_check.txt
set /A FAILED+=1

:summary
echo.
echo ==== editor checks: %PASSED% passed, %FAILED% failed ====
if %FAILED% NEQ 0 exit /b 1
exit /b 0
