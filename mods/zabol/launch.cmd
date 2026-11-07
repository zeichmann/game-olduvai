@echo off
setlocal
cd /d "%~dp0..\.."
set "OLDUVAI_PLAYER_SHEET=%CD%\mods\zabol\stone-age-sprites-v2.png"
set "ZABOL_EXE=%CD%\build\release\Release\olduvai.exe"
if not exist "%ZABOL_EXE%" set "ZABOL_EXE=%CD%\build\release\olduvai.exe"
if not exist "%ZABOL_EXE%" (
    echo Build the game first. See docs\BUILDING.md.
    pause
    exit /b 1
)
start "Zabol - Stone Age" "%ZABOL_EXE%" --game-dir "%CD%\game_data" --profile hd --hd-profile xbrz --render-scale 6 --output-filter sharp --window 1280x800 --play --level 1 --music-device opl
