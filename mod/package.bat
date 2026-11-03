@echo off
rem Builds the mod and packs what players need into mod\dist\PeggleNightsOnline.zip.
setlocal
cd /d "%~dp0"
call "%~dp0build.bat" || exit /b 1
if exist dist rmdir /s /q dist
mkdir dist\PeggleNightsOnline
copy /y build\bass.dll dist\PeggleNightsOnline\ >nul
copy /y package\*.* dist\PeggleNightsOnline\ >nul
powershell -NoProfile -Command "Compress-Archive -Path 'dist\PeggleNightsOnline\*' -DestinationPath 'dist\PeggleNightsOnline.zip' -Force" || exit /b 1
echo created dist\PeggleNightsOnline.zip
