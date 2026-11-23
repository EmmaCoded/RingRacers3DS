@echo off
rem GDB over Wi-Fi to the 3DS (Luma: Debugger options). Usage:
rem   gdb3ds.bat save          saves the .elf of the last build to %RR_WORK%\gdb  (do it whenever a new .3dsx is copied to the SD)
rem   gdb3ds.bat IP [PORT]      connects (PORT defaults to 4000; shown in Luma > Process list)
set ELF=%RR_WORK%\gdb\ringracers.elf
set GDB=C:\devkitPro\devkitARM\bin\arm-none-eabi-gdb.exe
if /i "%1"=="save" (
  if not exist "%RR_WORK%\gdb" mkdir "%RR_WORK%\gdb"
  copy /y "%~dp0..\build\bin\ringracers.elf" "%ELF%"
  goto :eof
)
if "%1"=="" (
  echo Usage: gdb3ds.bat IP [PORT]   or   gdb3ds.bat save
  goto :eof
)
set PORT=%2
if "%PORT%"=="" set PORT=4000
echo Connecting to %1:%PORT% ... on the 3DS leave the Luma menu with B, then type here: c
"%GDB%" "%ELF%" -ex "set pagination off" -ex "set confirm off" -ex "target remote %1:%PORT%"
