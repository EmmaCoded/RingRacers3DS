@echo off
rem Creates data\thumbnails_3ds.bin (80x50 thumbnails for the menu)
set SD=%RR_AZAHAR%\user\sdmc\3ds\RingRacers3ds
cd /d "%~dp0"
py make_thumbnails.py "%SD%\data\maps.pk3" "%SD%\bios.pk3" "%SD%\data\thumbnails_3ds.bin"
pause
