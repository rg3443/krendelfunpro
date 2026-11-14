@echo off
rem Сборка rentbot тем же MSVC (VS 18 Build Tools), которым vcpkg собирает зависимости.
rem VSLANG=1033: вывод компилятора на английском. С русским выводом CMake не распознаёт /showIncludes,
rem не отслеживает изменения заголовков и оставляет устаревшие .obj — бинарник падает при запуске.
rem Использование: build.bat        — сборка
rem                build.bat clean  — сборка с нуля
setlocal
set VSLANG=1033
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if /i "%1"=="clean" if exist build rmdir /s /q build
if not exist build\CMakeCache.txt (
    cmake -S . -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ^
        -DCMAKE_TOOLCHAIN_FILE=C:/prog/supprogs/vcpkg/scripts/buildsystems/vcpkg.cmake || exit /b 1
)
cmake --build build || exit /b 1
echo.
echo OK: build\rentbot.exe
