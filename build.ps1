# -*- coding: utf-8 -*-
$OutputEncoding = [Console]::OutputEncoding = [System.Text.Encoding]::UTF8

Write-Host "=== 32-bit Build Script ===" -ForegroundColor Green

$vsPath = "D:\Microsoft Visual Studio\2019\Community"
# JadeView 运行时 DLL 的来源包目录（可选）。
# 优先取环境变量 JADEVIEW_PACKAGE；未设置时直接使用仓库根目录已附带的 JadeView_x86.dll。
$jadePackage = $env:JADEVIEW_PACKAGE

# Ensure the output file is not locked by a previous test process.
& taskkill.exe /F /IM findimg_jade_x86.exe 2>$null | Out-Null
Start-Sleep -Milliseconds 500
$outputName = "findimg_jade_x86_v9.exe"
$output = Join-Path $PWD $outputName
Remove-Item $output -Force -ErrorAction SilentlyContinue

# Keep the runtime DLL matched with the x86 import/static library package.
if ($jadePackage) {
    $jadeDll = Join-Path $jadePackage "JadeView_x86.dll"
    if (!(Test-Path $jadeDll)) { Write-Host "JADEVIEW_PACKAGE 下缺少 JadeView_x86.dll" -ForegroundColor Red; exit 1 }
    Copy-Item $jadeDll (Join-Path $PWD "JadeView_x86.dll") -Force
} elseif (!(Test-Path (Join-Path $PWD "JadeView_x86.dll"))) {
    Write-Host "缺少 JadeView_x86.dll：请设置环境变量 JADEVIEW_PACKAGE，或把该 DLL 放到项目根目录" -ForegroundColor Red
    exit 1
}

# Step: Compile with C++17, UTF-8 and all required libraries
Write-Host "Compiling with C++17 and UTF-8..." -ForegroundColor Cyan

$batContent = "call `"$vsPath\VC\Auxiliary\Build\vcvarsall.bat`" x86`r`ncd src`r`ncl.exe /std:c++17 /utf-8 /EHsc /O2 /MT /D UNICODE /D _UNICODE /D JADE_FRONTEND /I. jade_main.cpp ..\lib\JadeView_x86.lib gdiplus.lib comctl32.lib shell32.lib user32.lib gdi32.lib uxtheme.lib ole32.lib oleaut32.lib ntdll.lib ws2_32.lib winspool.lib bcrypt.lib advapi32.lib /Fe:..\$outputName /link /SUBSYSTEM:WINDOWS /MACHINE:X86 /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:findimg_x86.manifest"

[System.IO.File]::WriteAllText("$PWD\compile.bat", $batContent, [System.Text.Encoding]::ASCII)

& cmd /c compile.bat
$buildCode = $LASTEXITCODE

Remove-Item "compile.bat" -ErrorAction SilentlyContinue
Remove-Item "src\*.obj" -ErrorAction SilentlyContinue

if ($buildCode -eq 0 -and (Test-Path $outputName)) {
    Write-Host "`nBuild completed successfully!" -ForegroundColor Green
    Write-Host "Output: $outputName" -ForegroundColor Green
    Get-Item $outputName | Select-Object Name,Length,LastWriteTime
} else {
    Write-Host "`nBuild failed! Check errors above." -ForegroundColor Red
}
