@echo off
rem Builds the ATLAS of common objects and copies it to Azahar.
set GAME=%RR_GAME_DIR%
set DATA=%RR_AZAHAR%\user\sdmc\3ds\RingRacers3ds\data
cd /d "%~dp0"
py convert_atlas.py "%GAME%\bios.pk3" "%DATA%\ringracers_3ds_atlas.bin" atlas_list.txt "%DATA%\gfx.pk3" "%DATA%\maps.pk3" "%DATA%\followers.pk3"
pause
