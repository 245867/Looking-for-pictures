@echo off
setlocal
cd /d "%~dp0"
net session >nul 2>&1
if not "%errorlevel%"=="0" (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -Verb RunAs -FilePath '%~f0'"
  exit /b 0
)
set "DLL=%~dp0runtime\dm.dll"
if not exist "%DLL%" (
  echo Missing runtime\dm.dll
  pause
  exit /b 2
)
%SystemRoot%\SysWOW64\regsvr32.exe /s "%DLL%"
if errorlevel 1 (
  echo COM registration failed. code=%errorlevel%
) else (
  echo DM COM registration succeeded.
)
pause
