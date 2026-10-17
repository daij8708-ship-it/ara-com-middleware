@echo off
setlocal
chcp 65001 >nul

rem 双击启动 ara-com 运行监测：由 Windows 调用 WSL 执行同一套脚本。
rem 可通过环境变量 ARA_COM_WSL_DISTRO 覆盖发行版名称。
set "DISTRO=%ARA_COM_WSL_DISTRO%"
if not defined DISTRO set "DISTRO=Ubuntu-22.04"

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

echo 正在启动 ara-com 运行监测...
echo 发行版: %DISTRO%
echo 目录:   %HERE%
echo.

wsl.exe -d "%DISTRO%" --cd "%HERE%" -- bash scripts/monitor.sh %*
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
    echo.
    echo 启动失败，退出码 %RC%。
    echo 请确认 WSL 发行版 %DISTRO% 可用，并已安装 Fast DDS、CMake、Python 3。
    pause
)

endlocal
