@echo off
rem ===========================================================================
rem  ara-com-middleware one-click test launcher (Windows wrapper for WSL)
rem
rem  Double-click this file, or run in a terminal:
rem      run-tests.cmd                     (default: unit + guards)
rem      run-tests.cmd all
rem      run-tests.cmd vsomeip --keep-going
rem      run-tests.cmd capacity
rem
rem  Every argument is forwarded to run-tests.sh inside WSL.
rem  Set ARA_WSL_DISTRO to pick a specific distro (default: the WSL default).
rem ===========================================================================
setlocal EnableExtensions

set "PROJECT_DIR=%~dp0"
if "%PROJECT_DIR:~-1%"=="\" set "PROJECT_DIR=%PROJECT_DIR:~0,-1%"

rem Use UTF-8 so the Chinese output of run-tests.sh renders correctly.
for /f "tokens=2 delims=:" %%C in ('chcp') do set "OLD_CP=%%C"
chcp 65001 >nul 2>nul

rem Pause only when the file was double-clicked (fresh console window).
set "DO_PAUSE=0"
echo %cmdcmdline% | find /i "%~nx0" >nul
if not errorlevel 1 set "DO_PAUSE=1"

if not exist "%PROJECT_DIR%\run-tests.sh" (
    echo [ara-com] run-tests.sh not found in %PROJECT_DIR%
    set "RC=2"
    goto finish
)

where wsl.exe >nul 2>nul
if errorlevel 1 goto no_wsl

set "DISTRO_ARG="
if not "%ARA_WSL_DISTRO%"=="" set "DISTRO_ARG=-d %ARA_WSL_DISTRO%"

echo [ara-com] project : %PROJECT_DIR%
echo [ara-com] running : bash ./run-tests.sh %*
echo.
wsl.exe %DISTRO_ARG% --cd "%PROJECT_DIR%." -- bash ./run-tests.sh %*
set "RC=%ERRORLEVEL%"
goto finish

:no_wsl
echo [ara-com] WSL was not found. Install WSL2 first:  wsl --install -d Ubuntu-22.04
echo [ara-com] Or run the script from any Linux shell:  bash run-tests.sh
set "RC=2"
goto finish

:finish
set "LAST="
if exist "%PROJECT_DIR%\artifacts\last-run.txt" set /p LAST=<"%PROJECT_DIR%\artifacts\last-run.txt"
if defined LAST echo [ara-com] logs   : %LAST%
echo.
if "%RC%"=="0" (echo [ara-com] result : OK) else (echo [ara-com] result : FAILED ^(exit code %RC%^))
if defined OLD_CP chcp %OLD_CP% >nul 2>nul
if "%DO_PAUSE%"=="1" pause
endlocal & exit /b %RC%
