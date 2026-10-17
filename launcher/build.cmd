@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"

echo 正在构建 Windows 启动器...
dotnet publish MonitorLauncher.csproj -c Release -r win-x64 ^
    --self-contained false ^
    -p:PublishSingleFile=true ^
    -p:DebugType=None ^
    -p:DebugSymbols=false ^
    -o dist
if errorlevel 1 (
    echo.
    echo 构建失败，请确认已安装 .NET 8 SDK。
    pause
    exit /b 1
)

copy /Y "dist\ara-com-monitor.exe" "..\ara-com-monitor.exe" >nul
if errorlevel 1 (
    echo.
    echo 复制 exe 失败。
    pause
    exit /b 1
)

echo.
echo 已生成: %~dp0..\ara-com-monitor.exe
echo 双击即可启动；运行需要 .NET 8 运行时。
endlocal
