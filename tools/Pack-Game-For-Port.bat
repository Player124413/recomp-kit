@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0.."

if "%~1"=="" (
    echo ========================================================
    echo   Smart Game Packer for Recomp-Kit
    echo   - Up to 5 GB : Packs FULL game
    echo   - 6 GB+      : Packs ONLY .exe, .dll, .ini ^(skips heavy files^)
    echo ========================================================
    echo.
    echo Drag and drop a game folder onto this .bat file, or enter path below:
    set /p "GAME_DIR=Game folder path: "
) else (
    set "GAME_DIR=%~1"
)

python tools\pack_game_for_port.py "!GAME_DIR!"
echo.
pause
