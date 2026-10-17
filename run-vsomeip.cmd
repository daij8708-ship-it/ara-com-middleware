@echo off
rem ===========================================================================
rem  ara-com vsomeip dual-process launcher (Windows -> two WSL terminals)
rem
rem  Opens one terminal for the SERVER and one for the CLIENT, so both sides
rem  can be watched separately, exactly like the manual steps in
rem  docs/run-all-tests.md.
rem
rem  Usage:  run-vsomeip.cmd [case] [server|client|both] [client args...]
rem
rem    run-vsomeip.cmd                        demo case, both terminals
rem    run-vsomeip.cmd completion             async RPC completion / timeout
rem    run-vsomeip.cmd concurrency            multiple outstanding (100 4)
rem    run-vsomeip.cmd concurrency both 1000 16
rem    run-vsomeip.cmd demo server            server terminal only
rem
rem  Set ARA_WSL_DISTRO to pick a specific distro (default: the WSL default).
rem  For unattended runs use run-tests.cmd instead.
rem ===========================================================================
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"

for /f "tokens=2 delims=:" %%C in ('chcp') do set "OLD_CP=%%C"
chcp 65001 >nul 2>nul

set "DO_PAUSE=0"
echo %cmdcmdline% | find /i "%~nx0" >nul
if not errorlevel 1 set "DO_PAUSE=1"

set "CASE=%~1"
if "%CASE%"=="" set "CASE=demo"
set "ROLE=%~2"
if "%ROLE%"=="" set "ROLE=both"

rem client extra args (up to three), e.g. 1000 16
set "CARGS="
if not "%~3"=="" set "CARGS=%~3"
if not "%~4"=="" set "CARGS=%CARGS% %~4"
if not "%~5"=="" set "CARGS=%CARGS% %~5"

if /i "%CASE%"=="demo" goto case_ok
if /i "%CASE%"=="completion" goto case_ok
if /i "%CASE%"=="concurrency" goto case_ok
echo [ara-com] unknown case: %CASE%
echo [ara-com] case must be demo, completion or concurrency
set "RC=2"
goto finish
:case_ok

if /i "%ROLE%"=="server" goto role_ok
if /i "%ROLE%"=="client" goto role_ok
if /i "%ROLE%"=="both" goto role_ok
echo [ara-com] unknown role: %ROLE%
echo [ara-com] role must be server, client or both
set "RC=2"
goto finish
:role_ok

if not exist "%PROJECT_DIR%\scripts\vsomeip_pair.sh" (
    echo [ara-com] scripts\vsomeip_pair.sh not found in %PROJECT_DIR%
    set "RC=2"
    goto finish
)

where wsl.exe >nul 2>nul
if errorlevel 1 goto no_wsl

set "DISTRO_ARG="
if not "%ARA_WSL_DISTRO%"=="" set "DISTRO_ARG=-d %ARA_WSL_DISTRO%"

echo [ara-com] case   : %CASE%
echo [ara-com] role   : %ROLE%
if not "%CARGS%"=="" echo [ara-com] client : %CARGS%
echo.

if /i "%ROLE%"=="client" goto only_client

echo [ara-com] opening SERVER terminal ...
start "ara-com vsomeip SERVER (%CASE%)" wsl.exe %DISTRO_ARG% --cd "%PROJECT_DIR%." -- bash ./scripts/vsomeip_pair.sh server %CASE%
if /i "%ROLE%"=="server" goto launched

echo [ara-com] opening CLIENT terminal ...
start "ara-com vsomeip CLIENT (%CASE%)" wsl.exe %DISTRO_ARG% --cd "%PROJECT_DIR%." -- bash ./scripts/vsomeip_pair.sh client %CASE% %CARGS%
goto launched

:only_client
echo [ara-com] opening CLIENT terminal ...
start "ara-com vsomeip CLIENT (%CASE%)" wsl.exe %DISTRO_ARG% --cd "%PROJECT_DIR%." -- bash ./scripts/vsomeip_pair.sh client %CASE% %CARGS%

:launched
echo.
echo [ara-com] server and client talk through a ready-marker, so the order
echo [ara-com] of the two windows does not matter (client waits up to 45s).
echo [ara-com] logs : %PROJECT_DIR%\artifacts\vsomeip-pair\%CASE%\
echo [ara-com] stop : press Ctrl+C in the SERVER terminal when done.
set "RC=0"
goto finish

:no_wsl
echo [ara-com] WSL was not found. Install WSL2 first:  wsl --install -d Ubuntu-22.04
set "RC=2"
goto finish

:finish
echo.
if defined OLD_CP chcp %OLD_CP% >nul 2>nul
if "%DO_PAUSE%"=="1" pause
endlocal & exit /b %RC%
