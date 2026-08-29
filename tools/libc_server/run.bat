@echo off
REM Fast loop: prebuilt USB fpdoom + doom1.wad over USB. NO SD card.
REM Then run:  madctl-test.bat 0xNN  (fptest pattern)  or edit --mac below.
.\spd_dump ^
  fdl nor_fdl1.bin 0x40004000 ^
  fdl fpdoom.bin ram

if %ERRORLEVEL% neq 0 goto end

cd workdir && ..\libc_server -- --bright 50 --rotate 2 --mac 0x10 doom

:end
pause
