@echo off
setlocal enabledelayedexpansion

rem ----------------------------------------------------------------------------
rem Android Build Script (Windows Batch)

rem Default values
set BUILD_TYPE=Release
set PACKAGE=0
set CMAKE_GENERATOR=Ninja
set BUILD_JOBS=8

set BUILD_DIR_64=build_arm64
set BUILD_DIR_PACK=build_package

rem ----------------------------------------------------------------------------
rem Process command line options
rem %~1 removes quotes from the first argument.

if not "%~1"=="" (
    rem Check if BuildType is Release or Debug
    if /i "%~1" == "Release" (
        set BUILD_TYPE=%~1
    ) else if /i "%~1" == "Debug" (
        set BUILD_TYPE=%~1
    ) else (
        echo ERROR: BUILD_TYPE must be "Release" or "Debug".
        goto :EOF
    )
)

rem Check second argument (PACKAGE). Any second argument enables packaging.
if not "%~2"=="" (
    set PACKAGE=1
)

rem Check third argument (BUILD_JOBS). Overrides default parallel jobs count.
if not "%~3"=="" (
    set BUILD_JOBS=%~3
)

rem Output build information
if "%PACKAGE%"=="1" (
    echo Building a %BUILD_TYPE% build with packaging enabled. [Jobs: %BUILD_JOBS%]
) else (
    echo Building a %BUILD_TYPE% build without packaging. [Jobs: %BUILD_JOBS%]
)

rem ----------------------------------------------------------------------------
rem Build 64-bit layer

echo.
echo --- Starting 64-bit Build (%BUILD_DIR_64%) ---

pushd %~dp0

rem Create directory (MD does not fail if it already exists)
md %BUILD_DIR_64%

rem ----------------------------------------------------------------------------
rem CMake configure command

echo Starting CMake configuration...

rem Use ^ for line continuation
cmake -S . -B %BUILD_DIR_64% ^
    -G "%CMAKE_GENERATOR%" ^
    -DCMAKE_SYSTEM_NAME=Android ^
    -DANDROID_PLATFORM=29 ^
    -DANDROID_ABI=arm64-v8a ^
    -DANDROID_TOOLCHAIN=clang ^
    -DANDROID_STL=c++_static ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK_HOME%/build/cmake/android.toolchain.cmake" ^
    -DCMAKE_WARN_DEPRECATED=OFF

if errorlevel 1 (
    echo.
    echo FATAL ERROR: CMake configuration failed.
    echo Error code: %errorlevel%
    echo Ensure ANDROID_NDK_HOME environment variable is set correctly and %CMAKE_GENERATOR% is available.
    goto :error
)

rem Execute build
echo Starting CMake build...
cmake --build %BUILD_DIR_64% -j%BUILD_JOBS%

if errorlevel 1 (
    echo.
    echo FATAL ERROR: CMake build failed.
    echo Error code: %errorlevel%
    goto :error
)

rem ----------------------------------------------------------------------------
rem Build release package

if "%PACKAGE%"=="1" (
    echo.
    echo --- Starting packaging ---

    rem Set up package directory
    set "TARGET_ARM64_DIR=%BUILD_DIR_PACK%\bin\android\arm64"
    md "!TARGET_ARM64_DIR!"

    rem Install 64-bit layer
    set SOURCE_PATH=%BUILD_DIR_64%\source\*.so
    set DEST_PATH=%BUILD_DIR_PACK%\bin\android\arm64

    echo Copying library files from !SOURCE_PATH! to !DEST_PATH!
    copy !SOURCE_PATH! !DEST_PATH! >nul

    if errorlevel 1 (
        echo.
        echo WARNING: File copy failed. Check if source files exist.
    )
)

echo.
echo --- Script finished successfully. ---
popd
goto :EOF

:error
echo.
echo Script terminated due to an error.
endlocal
popd