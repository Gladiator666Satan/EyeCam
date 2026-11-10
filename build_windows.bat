@echo off
setlocal enabledelayedexpansion

:: ==============================================================================
:: NexusCam / EyeCam - Master Windows Build Script
:: Compiles DirectShow Virtual Camera Filter DLL, Test Tools, and Desktop Studio
:: ==============================================================================

echo ======================================================================
echo  NexusCam: Building Desktop Engine and DirectShow Filter for Windows
echo ======================================================================

set "BUILD_TYPE=Release"
if not "%~1"=="" set "BUILD_TYPE=%~1"

set "PROJECT_ROOT=%~dp0"
set "BUILD_DIR=%PROJECT_ROOT%build_windows"

:: 1. Locate MSVC Environment
where cl.exe >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo [+] MSVC compiler found in PATH.
) else (
    echo [*] Searching for Visual Studio installation...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo [-] Error: vswhere.exe not found. Please run this script from 'Developer Command Prompt for VS'.
        exit /b 1
    )

    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        set "VS_INSTALL_DIR=%%i"
    )

    if not defined VS_INSTALL_DIR (
        echo [-] Error: Visual Studio C++ toolset not found.
        exit /b 1
    )

    echo [+] Found Visual Studio at: "!VS_INSTALL_DIR!"
    call "!VS_INSTALL_DIR!\VC\Auxiliary\Build\vcvars64.bat"
)

:: 2. Check CMake
where cmake.exe >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [-] Error: cmake.exe is required but not found in PATH.
    exit /b 1
)

:: 3. Configure CMake
echo.
echo [*] Configuring CMake project in: %BUILD_DIR%
if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
cd /d "%BUILD_DIR%"

cmake "%PROJECT_ROOT%" ^
    -G "Visual Studio 17 2022" -A x64 ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DBUILD_EYECAM_APP=ON
if %ERRORLEVEL% neq 0 (
    echo [-] CMake configuration failed.
    exit /b %ERRORLEVEL%
)

:: 4. Build Targets
echo.
echo [*] Compiling targets (%BUILD_TYPE%)...
cmake --build . --config %BUILD_TYPE% --parallel %NUMBER_OF_PROCESSORS%
if %ERRORLEVEL% neq 0 (
    echo [-] Compilation failed.
    exit /b %ERRORLEVEL%
)

echo.
echo ======================================================================
echo  Build Successful!
echo  Binaries generated in: %BUILD_DIR%\bin\%BUILD_TYPE% (or %BUILD_DIR%\bin)
echo ======================================================================
echo.
echo  Key Output Files:
echo   - EyeCamFilter.dll      : DirectShow Virtual Camera Source Filter
echo   - windows_test.exe      : Shared Memory IPC producer test tool
echo   - EyeCam.exe            : Desktop Studio UI (if Qt6 was found)
echo.
echo  Registration Instructions:
echo   To register the DirectShow filter for Zoom/OBS/Teams, run as Administrator:
echo   regsvr32.exe "%BUILD_DIR%\bin\%BUILD_TYPE%\EyeCamFilter.dll"
echo ======================================================================

cd /d "%PROJECT_ROOT%"
endlocal
exit /b 0
