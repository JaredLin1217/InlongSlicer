@REM Inlong Slicer build script for Windows with VS auto-detect
@echo off
set WP=%CD%
set _START_TIME=%TIME%
set "_BUILD_EXIT_CODE="

@REM Default target architecture to the host CPU arch; override by passing
@REM "x64" or "arm64" as an argument. PROCESSOR_ARCHITEW6432 covers a 32-bit
@REM shell running on a 64-bit OS, where PROCESSOR_ARCHITECTURE reads "x86".
set arch=x64
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" set arch=ARM64
if /I "%PROCESSOR_ARCHITEW6432%"=="ARM64" set arch=ARM64
if /I "%1"=="arm64" set arch=ARM64
if /I "%2"=="arm64" set arch=ARM64
if /I "%1"=="x64" set arch=x64
if /I "%2"=="x64" set arch=x64

@REM Check for Ninja Multi-Config option (-x)
set USE_NINJA=0
set FAST_BUILD=0
set FULL_FAST_BUILD=0
for %%a in (%*) do (
    if "%%a"=="-x" set USE_NINJA=1
    if "%%a"=="fast" set FAST_BUILD=1
    if "%%a"=="allfast" (
        set FAST_BUILD=1
        set FULL_FAST_BUILD=1
    )
)

@REM Check for clang-cl option (-l). Combined with -x it also builds the deps with
@REM clang-cl; on the Visual Studio generator it applies to the slicer only, because
@REM the dependency sub-builds have no toolset to inherit and stay on MSVC.
set CLANG_ARG=
set TOOLSET_ARG=
for %%a in (%*) do (
    if "%%a"=="-l" (
        set CLANG_ARG=-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
        set TOOLSET_ARG=-T ClangCL
    )
)

@REM Check for unit-tests option ("tests")
set BUILD_TESTS=OFF
for %%a in (%*) do (
    if /I "%%a"=="tests" set BUILD_TESTS=ON
)

if "%USE_NINJA%"=="1" (
    echo Using Ninja Multi-Config generator
    set CMAKE_GENERATOR="Ninja Multi-Config"
    set VS_VERSION=Ninja
    goto :generator_ready
)

@REM Detect Visual Studio version using msbuild
echo Detecting Visual Studio version using msbuild...

@REM Try to get MSBuild version - the output format varies by VS version
set VS_MAJOR=
for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
    for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
    set MSBUILD_OUTPUT=%%i
    goto :version_found
)

@REM Alternative method for newer MSBuild versions
if "%VS_MAJOR%"=="" (
    for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
        for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
        set MSBUILD_OUTPUT=%%i
        goto :version_found
    )
)

:version_found
echo MSBuild version detected: %MSBUILD_OUTPUT%
echo Major version: %VS_MAJOR%

if "%VS_MAJOR%"=="" (
    echo Error: Could not determine Visual Studio version from msbuild
    echo Please ensure Visual Studio and MSBuild are properly installed
    exit /b 1
)

if "%VS_MAJOR%"=="16" (
    set VS_VERSION=2019
    set CMAKE_GENERATOR="Visual Studio 16 2019"
) else if "%VS_MAJOR%"=="17" (
    set VS_VERSION=2022
    set CMAKE_GENERATOR="Visual Studio 17 2022"
) else if "%VS_MAJOR%"=="18" (
    set VS_VERSION=2026
    set CMAKE_GENERATOR="Visual Studio 18 2026"
) else (
    echo Error: Unsupported Visual Studio version: %VS_MAJOR%
    echo Supported versions: VS2019 (16.8+^), VS2022 (17.x^), VS2026 (18.x^)
    exit /b 1
)

echo Detected Visual Studio %VS_VERSION% (version %VS_MAJOR%)
echo Using CMake generator: %CMAKE_GENERATOR%

:generator_ready

@REM Pack deps
if "%1"=="pack" (
    setlocal ENABLEDELAYEDEXPANSION
    cd %WP%/deps/build
    if "%arch%"=="ARM64" cd %WP%/deps/build-arm64
    for /f "tokens=2-4 delims=/ " %%a in ('date /t') do set build_date=%%c%%b%%a
    set DEPS_FOLDER=InlongSlicer_dep
    echo packing deps: InlongSlicer_dep_win-!arch!_!build_date!_vs!VS_VERSION!.zip

    %WP%/tools/7z.exe a InlongSlicer_dep_win-!arch!_!build_date!_vs!VS_VERSION!.zip !DEPS_FOLDER!
    goto :done
)

set debug=OFF
set debuginfo=OFF
if "%1"=="debug" set debug=ON
if "%2"=="debug" set debug=ON
if "%1"=="debuginfo" set debuginfo=ON
if "%2"=="debuginfo" set debuginfo=ON
if "%debug%"=="ON" (
    set build_type=Debug
    set build_dir=build-dbg
) else (
    if "%debuginfo%"=="ON" (
        set build_type=RelWithDebInfo
        set build_dir=build-dbginfo
    ) else (
        set build_type=Release
        set build_dir=build
    )
)
if "%arch%"=="ARM64" set build_dir=%build_dir%-arm64
echo build type set to %build_type%
set BUILD_TARGET=ALL_BUILD
if "%FAST_BUILD%"=="1" set BUILD_TARGET=InlongSlicer_app_gui
if "%FULL_FAST_BUILD%"=="1" set BUILD_TARGET=ALL_BUILD
echo target arch set to %arch%

setlocal DISABLEDELAYEDEXPANSION
cd deps
if not exist %build_dir% mkdir %build_dir%
cd %build_dir%
set "SIG_FLAG="
if defined INLONG_UPDATER_SIG_KEY set "SIG_FLAG=-DINLONG_UPDATER_SIG_KEY=%INLONG_UPDATER_SIG_KEY%"

if "%1"=="slicer" (
    GOTO :slicer
)
if "%FAST_BUILD%"=="1" (
    GOTO :slicer
)
echo "building deps.."
if defined CLANG_ARG if "%USE_NINJA%"=="0" echo Note: -l needs -x for the dependencies; building them with MSVC.

echo on
REM Set minimum CMake policy to avoid <3.5 errors
set CMAKE_POLICY_VERSION_MINIMUM=3.5
if "%USE_NINJA%"=="1" (
    cmake ../ -G %CMAKE_GENERATOR% %CLANG_ARG% -Wno-dev -DCMAKE_BUILD_TYPE=%build_type%
    cmake --build . --config %build_type% --target deps
) else (
    cmake ../ -G %CMAKE_GENERATOR% -A %arch% -Wno-dev -DCMAKE_BUILD_TYPE=%build_type%
    cmake --build . --config %build_type% --target deps -- /m /nr:false
)
@echo off

if "%1"=="deps" goto :done

:slicer
echo "building Inlong Slicer..."
cd %WP%
if not exist %build_dir% mkdir %build_dir%
cd %build_dir%
echo Build target set to %BUILD_TARGET%

echo on
set CMAKE_POLICY_VERSION_MINIMUM=3.5
if "%USE_NINJA%"=="1" (
    cmake .. -G %CMAKE_GENERATOR% %CLANG_ARG% -Wno-dev -DINLONG_TOOLS=ON %SIG_FLAG% -DBUILD_TESTS=%BUILD_TESTS% -DCMAKE_BUILD_TYPE=%build_type%
    cmake --build . --config %build_type% --target %BUILD_TARGET%
) else (
    cmake .. -G %CMAKE_GENERATOR% -A %arch% %TOOLSET_ARG% -Wno-dev -DINLONG_TOOLS=ON %SIG_FLAG% -DBUILD_TESTS=%BUILD_TESTS% -DCMAKE_BUILD_TYPE=%build_type%
    cmake --build . --config %build_type% --target %BUILD_TARGET% -- /m /nr:false
)
@echo off
if "%FAST_BUILD%"=="1" (
    echo Fast build completed. Skipping gettext and install.
    echo Output directory: %WP%\%build_dir%\src\%build_type%
    goto :done
)
cd ..
call scripts/run_gettext.bat
cd %build_dir%
cmake --build . --target install --config %build_type%
if errorlevel 1 goto :build_failed

if not defined INLONG_WINDOWS_CODESIGN_STORE set "INLONG_WINDOWS_CODESIGN_STORE=CurrentUser"
if not defined INLONG_WINDOWS_TIMESTAMP_URL set "INLONG_WINDOWS_TIMESTAMP_URL=http://timestamp.digicert.com"

if defined INLONG_WINDOWS_CODESIGN_THUMBPRINT (
    powershell -NoProfile -File "%WP%\scripts\windows\sign_windows_release.ps1" -Path "%WP%\%build_dir%\src\%build_type%" -CertificateThumbprint "%INLONG_WINDOWS_CODESIGN_THUMBPRINT%" -CertificateStoreLocation "%INLONG_WINDOWS_CODESIGN_STORE%" -TimestampUrl "%INLONG_WINDOWS_TIMESTAMP_URL%"
    if errorlevel 1 goto :build_failed

    powershell -NoProfile -File "%WP%\scripts\windows\sign_windows_release.ps1" -Path "%WP%\%build_dir%\InlongSlicer" -CertificateThumbprint "%INLONG_WINDOWS_CODESIGN_THUMBPRINT%" -CertificateStoreLocation "%INLONG_WINDOWS_CODESIGN_STORE%" -TimestampUrl "%INLONG_WINDOWS_TIMESTAMP_URL%"
    if errorlevel 1 goto :build_failed

    if exist "%WP%\%build_dir%\InlongSlicer_InlongOnly" (
        powershell -NoProfile -File "%WP%\scripts\windows\sign_windows_release.ps1" -Path "%WP%\%build_dir%\InlongSlicer_InlongOnly" -CertificateThumbprint "%INLONG_WINDOWS_CODESIGN_THUMBPRINT%" -CertificateStoreLocation "%INLONG_WINDOWS_CODESIGN_STORE%" -TimestampUrl "%INLONG_WINDOWS_TIMESTAMP_URL%"
        if errorlevel 1 goto :build_failed
    )
) else (
    echo Windows signing skipped: INLONG_WINDOWS_CODESIGN_THUMBPRINT is not set.
)

goto :done

:build_failed
set "_BUILD_EXIT_CODE=%ERRORLEVEL%"
if "%_BUILD_EXIT_CODE%"=="0" set "_BUILD_EXIT_CODE=1"
echo Build failed with exit code %_BUILD_EXIT_CODE%.

:done
@echo off
for /f "tokens=1-3 delims=:.," %%a in ("%_START_TIME: =0%") do set /a "_start_s=(1%%a-100)*3600+(1%%b-100)*60+(1%%c-100)"
for /f "tokens=1-3 delims=:.," %%a in ("%TIME: =0%") do set /a "_end_s=(1%%a-100)*3600+(1%%b-100)*60+(1%%c-100)"
set /a "_elapsed=_end_s - _start_s"
if %_elapsed% lss 0 set /a "_elapsed+=86400"
set /a "_hours=_elapsed / 3600"
set /a "_remainder=_elapsed - _hours * 3600"
set /a "_mins=_remainder / 60"
set /a "_secs=_remainder - _mins * 60"
echo.
if defined _BUILD_EXIT_CODE (
    echo Build failed after %_hours%h %_mins%m %_secs%s
    exit /b %_BUILD_EXIT_CODE%
)
echo Build completed in %_hours%h %_mins%m %_secs%s
