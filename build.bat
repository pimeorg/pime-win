@echo off
setlocal

rem Builds pime-win with MSVC + Ninja and stages everything into build\dist.
rem Set VCVARS to override the Visual Studio environment script.

set "ROOT=%~dp0"

if not defined VCVARS (
    for %%d in (
        "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
        "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        "D:\vs\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) do if exist "%%~d" set "VCVARS=%%~d"
)

if defined VCVARS (
    echo Using %VCVARS%
    call "%VCVARS%" >nul
)

where cl.exe >nul 2>nul || (
    echo cl.exe not found. Install MSVC x64 or set VCVARS to vcvars64.bat.
    exit /b 1
)

cmake -S "%ROOT%." -B "%ROOT%build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%ROOT%build" --parallel || exit /b 1

echo.
echo Built: %ROOT%build\dist
