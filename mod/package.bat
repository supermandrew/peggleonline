@echo off
rem Builds the mod and packs what players need into mod\dist\PeggleNightsOnline.zip.
setlocal
cd /d "%~dp0"
call "%~dp0build.bat" || exit /b 1
rem (only what this makes: dist\ keeps other things too, such as the trailer)
if exist dist\PeggleNightsOnline rmdir /s /q dist\PeggleNightsOnline
if exist dist\PeggleNightsOnline.zip del dist\PeggleNightsOnline.zip
mkdir dist\PeggleNightsOnline
copy /y build\bass.dll dist\PeggleNightsOnline\ >nul
copy /y package\*.* dist\PeggleNightsOnline\ >nul
powershell -NoProfile -Command "Compress-Archive -Path 'dist\PeggleNightsOnline\*' -DestinationPath 'dist\PeggleNightsOnline.zip' -Force" || exit /b 1
echo created dist\PeggleNightsOnline.zip
