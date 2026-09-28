@echo off
setlocal
cd /d "%~dp0"

where cl >nul 2>nul
if %errorlevel%==0 (
    echo [MSVC] building...
    cl /nologo /O2 /EHsc /utf-8 /MT DiskWidget.cpp /link /SUBSYSTEM:WINDOWS /OUT:DiskWidget.exe
    if %errorlevel%==0 goto done
    goto fail
)

where g++ >nul 2>nul
if %errorlevel%==0 (
    echo [MinGW] building...
    g++ -O2 -s -static -mwindows DiskWidget.cpp -o DiskWidget.exe -lgdiplus -lgdi32 -luser32 -lshell32 -ladvapi32 -lole32
    if %errorlevel%==0 goto done
    goto fail
)

echo.
echo Compiler not found.
echo Option A: open "x64 Native Tools Command Prompt for VS" and run build.bat there.
echo Option B: install w64devkit or WinLibs (MinGW-w64), add it to PATH, run build.bat again.
echo Option C: use the GitHub Actions workflow (see README in the chat).
pause
exit /b 1

:fail
echo.
echo Build failed - copy the error text and send it to me.
pause
exit /b 1

:done
echo.
echo Done: %cd%\DiskWidget.exe
pause
