@echo off
rem ============================================================
rem Regression runner.
rem
rem   test\run_tests.bat ^<case^>          one case, real time (watch it)
rem   test\run_tests.bat ^<case^> fast     one case, fast + determinism guard
rem   test\run_tests.bat all             every test\*.txt, real time
rem   test\run_tests.bat all fast        every case, fast + guard  ^<- AI regression
rem
rem ^<case^> is a file in test\ (e.g. baseline.txt; the ".txt" may be omitted).
rem
rem `fast` does two things:
rem   1. passes --fast to the game: one logic step per frame (not paced by real
rem      time) and one Present every 64 frames -> a 1961 frame case drops from
rem      33s to ~1.0s;
rem   2. runs the case TWICE and byte-compares the two --trace files. That
rem      comparison is the DETERMINISM GUARD: two runs of the same tape must be
rem      identical, so any field that is not in the snapshot but still affects
rem      the logic shows up here. It is NOT a performance test -- it is the
rem      reason fast mode costs two runs.
rem
rem Without `fast` the case runs exactly once, in real time, so a human can
rem watch the rendering; no trace files are written in that mode.
rem
rem Exit code: 0 = every requested case passed, 1 = a failure or bad arguments.
rem
rem Deliberately serial (one case at a time) and deliberately sharing one pair of
rem artifact paths (build\_trace_a.csv / _trace_b.csv): parallel cases / CI /
rem per-case artifacts are NOT a goal here.
rem
rem This file is ASCII-only on purpose: cmd reads a .bat with the local code page
rem (936 here), so UTF-8 Chinese text would be mis-decoded (already bitten us -
rem see AGENTS.md section 4.5). Chinese notes live in docs/input-script.md.
rem
rem Arguments are passed WITHOUT quotes: main.exe splits the command line on
rem whitespace and does not strip quotes, so "test\x.txt" would become a file
rem name that contains quote characters. Case names therefore must not contain
rem spaces.
rem ============================================================
setlocal enabledelayedexpansion
cd /D "%~dp0.."

set CASE=%~1
set MODE=%~2
set FAST=0

if "%CASE%"=="" goto :usage

if /I "%MODE%"=="fast" set FAST=1
if "%MODE%"=="" goto :mode_ok
if /I "%MODE%"=="fast" goto :mode_ok
echo [error] unknown second argument "%MODE%" ^(expected: fast^)
goto :usage

:mode_ok
if /I "%CASE%"=="all" goto :run

if not exist "test\%CASE%" if exist "test\%CASE%.txt" set CASE=%CASE%.txt
if not exist "test\%CASE%" (
    echo [error] no such case: test\%CASE%
    goto :usage
)

:run
if not exist build mkdir build
set TRACE_A=build\_trace_a.csv
set TRACE_B=build\_trace_b.csv
set PASSED=0
set FAILED=0

if "%FAST%"=="1" (echo [mode] fast + determinism guard) else (echo [mode] real time)

if /I "%CASE%"=="all" (
    for %%f in (test\*.txt) do call :run_case "%%f"
) else (
    call :run_case "test\%CASE%"
)

echo.
echo ==== regression: %PASSED% passed, %FAILED% failed ====
if %FAILED% neq 0 exit /b 1
exit /b 0

:usage
echo.
echo usage:
echo   test\run_tests.bat ^<case^>          one case, real time (watch it)
echo   test\run_tests.bat ^<case^> fast     one case, fast + determinism guard
echo   test\run_tests.bat all             all cases, real time
echo   test\run_tests.bat all fast        all cases, fast + guard (AI regression)
echo.
echo ^<case^> = file in test\ (e.g. baseline.txt)
exit /b 1

:run_case
set CASE_NAME=%~nx1
set CASE_PATH=%~1

rem game.log is truncated by every game start (see logger.cc), so it only ever
rem contains THIS case's output -- no need to skip stale lines from earlier ones.

rem Drop stale traces first: otherwise a leftover file from an earlier run could
rem make the "no trace file" check below pass and silently skip the guard.
del /q "%TRACE_A%" "%TRACE_B%" 2>nul

if "%FAST%"=="1" goto :run_case_fast

build\main.exe input_script %CASE_PATH%
set RC=!errorlevel!
call :report
goto :eof

:run_case_fast
build\main.exe input_script %CASE_PATH% --fast --trace %TRACE_A%
set RC=!errorlevel!
call :report

rem The guard only says something about a case that PASSED: if the first run
rem already failed (bad script text, failed load, failed asserts) we stop here
rem -- running it twice would just double-count the failure and print a
rem misleading "nondeterministic" message for what is really a load error.
if !RC! neq 0 (
    echo         skip determinism guard: the first run already failed
    goto :eof
)

if not exist "%TRACE_A%" (
    echo         WARN no trace file, determinism guard skipped ^(MONO_DEBUG_BUILD off?^)
    goto :eof
)

build\main.exe input_script %CASE_PATH% --fast --trace %TRACE_B%
set RC=!errorlevel!
if !RC! neq 0 (
    echo         FAIL second run exited with !RC! ^(the first run exited 0^)
    set /a FAILED+=1
    goto :eof
)
if not exist "%TRACE_B%" (
    echo         WARN no trace file on the second run, determinism guard skipped
    goto :eof
)

fc.exe /b "%TRACE_A%" "%TRACE_B%" >nul
if errorlevel 1 (
    echo         FAIL nondeterministic: two runs of the same tape differ
    call :show_trace_diff
    set /a FAILED+=1
)
goto :eof

rem Print the first few differing lines so the frame where the two runs diverge
rem is visible without opening anything. /l compares line-by-line (the trace is
rem one line per logic step, frame in the first column).
:show_trace_diff
fc.exe /l /n "%TRACE_A%" "%TRACE_B%" > "build\_trace_diff.txt"
set /a SHOWN=0
for /f "usebackq delims=" %%l in ("build\_trace_diff.txt") do (
    set /a SHOWN+=1
    if !SHOWN! leq 8 echo         %%l
)
if !SHOWN! gtr 8 echo         ^(only the first 8 lines; full diff: build\_trace_diff.txt^)
goto :eof

:report
if !RC! equ 0 (
    echo [case] !CASE_NAME!  PASS
    set /a PASSED+=1
    goto :eof
)
echo [case] !CASE_NAME!  FAIL exit=!RC!
echo         reproduce: build\main.exe input_script !CASE_PATH! --fast --trace %TRACE_A%
echo         log lines from this case ^(build\main.exe writes them to game.log^):
call :show_log_lines
set /a FAILED+=1
goto :eof

rem A broken tape can produce dozens of failure lines; the first few (they carry
rem the script line number, the mismatch and a state context line) plus the
rem TAPE: summary list at the end are what actually explains the cause.
:show_log_lines
set /a SHOWN=0
set /a TOTAL=0
for /f "usebackq delims=" %%l in (`findstr /C:"[ERROR]" /C:"FAIL" /C:"TAPE:" game.log`) do (
    set /a TOTAL+=1
    if !TOTAL! leq 12 echo         %%l
)
if !TOTAL! gtr 12 echo         ^(... and !TOTAL! lines total, all in game.log^)
goto :eof
