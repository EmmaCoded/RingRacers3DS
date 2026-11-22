@echo off
rem PUBLIC PATCH: drag one or more character mods (.pk3) onto this .bat.
rem Output is ready for the 3DS (uncompressed + RRS2 sounds). Copy to SD:\3ds\RingRacers3ds\mods\ (max 16).
rem Needs: Python (py), pip install soundfile soxr numpy, and dspenc next to this script.
cd /d "%~dp0"
if "%~1"=="" (
  echo Drag a .pk3 onto this file.
  pause
  goto :eof
)
py convert_mod.py %*
pause
