@echo off
setlocal

echo ============================================
echo  VDBRender (OpenVDB) Nuke Plugin Installer
echo ============================================
echo.

:: ── Paths ─────────────────────────────────────
set BUILD_DIR=C:\dev\VDBmarcher\build
set PLUGIN_SRC=%BUILD_DIR%\VDBRender.dll
set VCPKG_BIN=C:\vcpkg\installed\x64-windows\bin
set NUKE_USER=%USERPROFILE%\.nuke
set PLUGIN_DIR=%NUKE_USER%\plugins

:: ── Create plugin dir ─────────────────────────
echo [1/3] Creating plugin directory...
if not exist "%PLUGIN_DIR%" (
    mkdir "%PLUGIN_DIR%"
    echo       Created: %PLUGIN_DIR%
) else (
    echo       Already exists: %PLUGIN_DIR%
)

:: ── Copy VDBRender.dll ────────────────────────
echo [2/3] Copying VDBRender.dll...
if exist "%PLUGIN_SRC%" (
    copy /Y "%PLUGIN_SRC%" "%PLUGIN_DIR%\" >nul
    echo       OK: VDBRender.dll
) else (
    echo       ERROR: Cannot find %PLUGIN_SRC%
    echo       Make sure you have built the plugin first.
    pause
    exit /b 1
)

:: ── Copy runtime DLLs ─────────────────────────
echo [3/3] Copying runtime dependencies...
for %%D in (openvdb.dll tbb12.dll zlib1.dll blosc.dll Imath-3_2.dll) do (
    if exist "%VCPKG_BIN%\%%D" (
        copy /Y "%VCPKG_BIN%\%%D" "%PLUGIN_DIR%\" >nul
        echo       OK: %%D
    ) else (
        echo       SKIP: %%D not found
    )
)

echo.
echo ============================================
echo  Install complete.
echo  Copy menu.py to %NUKE_USER%\
echo  Launch Nuke: Nodes ^> VDB ^> VDBRender
echo ============================================
echo.
pause
endlocal
