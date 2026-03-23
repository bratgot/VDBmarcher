@echo off
REM  VDBRender — Build for all installed Nuke versions
REM  Nuke 14-15: static link — no dependency DLLs
REM  Nuke 16+:   dynamic link — bundles dependency DLLs
setlocal enabledelayedexpansion

set "SRC=%CD%"
set "VCPKG=C:/vcpkg/scripts/buildsystems/vcpkg.cmake"
set "VCPKG_BIN=C:\vcpkg\installed\x64-windows\bin"
set "DEST=%USERPROFILE%\.nuke\plugins\VDBRender"
set /a N=0

echo.
echo  VDBRender Multi-Version Builder
echo.

for /d %%D in ("C:\Program Files\Nuke*") do (
    set "ND=%%~D"
    set "NN=%%~nxD"
    for /f "tokens=1 delims=." %%V in ("!NN:Nuke=!") do set "MJ=%%V"

    if exist "!ND!\include\DDImage\Iop.h" (
        echo  ---- !NN! ----

        set STD=20
        set "TRIPLET=x64-windows"
        set "STATIC=0"
        if !MJ! LEQ 15 (
            set STD=17
            set "TRIPLET=x64-windows-static-md"
            set "STATIC=1"
        )

        set "BD=!SRC!\build_nuke!MJ!"
        if exist "!BD!" rmdir /s /q "!BD!"
        mkdir "!BD!"
        pushd "!BD!"

        echo    C++!STD!, triplet=!TRIPLET!

        if "!STATIC!"=="1" (
            cmake "!SRC!" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_TOOLCHAIN_FILE="!VCPKG!" -DVCPKG_TARGET_TRIPLET=!TRIPLET! -DNUKE_ROOT="!ND!" -DCMAKE_CXX_STANDARD=!STD! -DOPENVDB_STATIC=ON
        )
        if "!STATIC!"=="0" (
            cmake "!SRC!" -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_TOOLCHAIN_FILE="!VCPKG!" -DVCPKG_TARGET_TRIPLET=!TRIPLET! -DNUKE_ROOT="!ND!" -DCMAKE_CXX_STANDARD=!STD!
        )

        nmake

        if exist "VDBRender.dll" (
            if not exist "!DEST!\nuke!MJ!" mkdir "!DEST!\nuke!MJ!"
            copy /Y VDBRender.dll "!DEST!\nuke!MJ!\" >nul

            if "!STATIC!"=="0" (
                for %%F in (openvdb.dll tbb12.dll tbbmalloc.dll Imath-3_2.dll blosc.dll zlib1.dll zstd.dll lz4.dll) do (
                    if exist "!VCPKG_BIN!\%%F" copy /Y "!VCPKG_BIN!\%%F" "!DEST!\nuke!MJ!\" >nul
                )
                echo  [OK] !NN! dynamic + DLLs
            )
            if "!STATIC!"=="1" (
                echo  [OK] !NN! static
            )
            set /a N+=1
        )
        if not exist "VDBRender.dll" (
            echo  [FAIL] !NN!
        )
        popd
        echo.
    )
)

set "MENUPY=%USERPROFILE%\.nuke\menu.py"
if !N! GTR 0 (
    findstr /c:"_load_vdbrender" "!MENUPY!" >nul 2>&1
    if errorlevel 1 (
        echo.>> "!MENUPY!"
        type "!SRC!\VDBRender_menu.py" >> "!MENUPY!"
        echo  VDBRender block appended to !MENUPY!
    )
)

echo.
echo  Done: !N! version(s) built to %DEST%
echo.
endlocal
