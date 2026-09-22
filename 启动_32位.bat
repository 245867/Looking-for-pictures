@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~dp0findimg_jade_x86_v9.exe' -WorkingDirectory '%~dp0' -Verb RunAs"
exit /b
