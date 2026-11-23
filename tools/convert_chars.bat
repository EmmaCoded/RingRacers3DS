@echo off
rem Ring Racers 3DS: converts the game's characters (chars.pk3) for the 3DS.
rem Drag chars.pk3 (the game's data folder) onto this .bat.
rem Output: chars_3ds folder next to this .bat. Copy its data folder to SD:\3ds\RingRacers3ds\data
if "%~1"=="" (py "%~dp0convert_chars.py") else (py "%~dp0convert_chars.py" "%~1")
pause
