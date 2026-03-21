@echo off
setlocal

echo ============================================
echo  VDBRender (OpenVDB) Nuke Plugin Installer
echo ============================================
echo.

:: ── Paths ─────────────────────────────────────
set BUILD_DIR=C:\dev\VDBRender\build\Release
set PLUGIN_SRC=%BUILD_DIR%\VDBRender.dll
set VCPKG_BIN=C:\vcpkg\installed\x64-windows\bin
set NUKE_USER=%USERPROFILE%\.nuke
set PLUGIN_DIR=%NUKE_USER%\plugins
set MENU_PY=%NUKE_USER%\menu.py

:: ── Create plugin dir ─────────────────────────
echo [1/4] Creating plugin directory...
if not exist "%PLUGIN_DIR%" (
    mkdir "%PLUGIN_DIR%"
    echo       Created: %PLUGIN_DIR%
) else (
    echo       Already exists: %PLUGIN_DIR%
)

:: ── Copy VDBRender.dll ────────────────────────
echo [2/4] Copying VDBRender.dll...
if exist "%PLUGIN_SRC%" (
    copy /Y "%PLUGIN_SRC%" "%PLUGIN_DIR%\" >nul
    echo       OK: VDBRender.dll
) else (
    echo       ERROR: Cannot find %PLUGIN_SRC%
    echo       Make sure you have built the plugin first.
    pause
    exit /b 1
)

:: ── Copy OpenVDB runtime DLLs ─────────────────
echo [3/4] Copying runtime dependencies from vcpkg...
set COPY_OK=1
for %%D in (openvdb.dll tbb12.dll zlib1.dll blosc.dll Half-2_5.dll Iex-2_5.dll) do (
    if exist "%VCPKG_BIN%\%%D" (
        copy /Y "%VCPKG_BIN%\%%D" "%PLUGIN_DIR%\" >nul
        echo       OK: %%D
    ) else (
        echo       SKIP: %%D not found ^(may not be needed^)
    )
)

:: Also check for newer Imath/Half naming (OpenEXR 3.x)
for %%D in (Imath-3_1.dll Imath.dll) do (
    if exist "%VCPKG_BIN%\%%D" (
        copy /Y "%VCPKG_BIN%\%%D" "%PLUGIN_DIR%\" >nul
        echo       OK: %%D
    )
)

:: ── Append to menu.py ────────────────────────
echo [4/4] Updating menu.py...

:: Check if menu.py already contains VDBRender
if exist "%MENU_PY%" (
    findstr /C:"VDBRender" "%MENU_PY%" >nul 2>&1
    if not errorlevel 1 (
        echo       VDBRender already present in menu.py - skipping.
        goto done
    )
)

:: Append the menu entry
(
echo.
echo # -- VDBRender ^(OpenVDB^) -----------------------------------------------
echo try:
echo     import nuke, os
echo     nuke.pluginAddPath^(os.path.expanduser^("~/.nuke/plugins"^)^)
echo     nuke.load^("VDBRender"^)
echo     toolbar = nuke.menu^("Nodes"^)
echo     vdb_menu = toolbar.addMenu^("VDB"^)
echo     vdb_menu.addCommand^("VDBRender", "nuke.createNode^('VDBRender'^)"^)
echo except Exception as e:
echo     nuke.warning^("VDBRender failed to load: " + str^(e^)^)
) >> "%MENU_PY%"

echo       Appended to: %MENU_PY%

:done
echo.
echo ============================================
echo  Install complete.
echo  Launch Nuke and look for VDB ^> VDBRender
echo  in the Nodes menu.
echo  Reads .vdb files directly — no conversion.
echo ============================================
echo.
pause
endlocal
