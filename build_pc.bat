@echo off
python gen.py || exit /b 1
where gcc >nul 2>nul && (gcc -O3 -Wall -Wextra -I. -Igame game/game.c runtime.c main_win32.c -lgdi32 -lwininet -luser32 -lkernel32 -lm -o Game.exe -mwindows && goto ok)
where cl >nul 2>nul && (cl /O2 /utf-8 /I. /Igame game/game.c runtime.c main_win32.c /Fe:Game.exe /link gdi32.lib wininet.lib user32.lib kernel32.lib shell32.lib /SUBSYSTEM:WINDOWS && goto ok)
echo No C compiler found.
exit /b 1
:ok
if not exist "assets" mkdir "assets"
xcopy /E /I /Y "game\assets" "assets" >nul
echo Build complete: Game.exe
