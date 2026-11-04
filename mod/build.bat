@echo off
rem Builds the mod as bass.dll (x86) into mod\build\. Pass "install" to also copy it into the game folder.
setlocal
cd /d "%~dp0"
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
if not exist build mkdir build
cl /nologo /std:c++17 /O2 /MT /EHsc /W3 /GS /Zi /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
   /Fobuild\ /Fdbuild\ src\*.cpp ^
   /link /DLL /OUT:build\bass.dll /PDB:build\bass.pdb /DEBUG /OPT:REF ^
   user32.lib gdi32.lib ws2_32.lib iphlpapi.lib dxguid.lib advapi32.lib hid.lib setupapi.lib
if errorlevel 1 exit /b 1
rem A float kept in an xmm register across a call into the game comes back as garbage: see tools\xmmcheck.py.
where python >nul 2>nul
if not errorlevel 1 (
    python tools\xmmcheck.py build\bass.dll
    if errorlevel 1 exit /b 1
)
if /i not "%1"=="install" exit /b 0
rem Keep the genuine BASS (92,216 bytes) as bass_orig.dll the first time, never a copy of the mod.
if not exist "..\bass_orig.dll" (
    for %%F in ("..\bass.dll") do if not "%%~zF"=="92216" (echo ..\bass.dll is not the original BASS & exit /b 1)
    copy /y "..\bass.dll" "..\bass_orig.dll" >nul || exit /b 1
)
copy /y build\bass.dll "..\bass.dll" >nul || exit /b 1
echo installed to game folder
