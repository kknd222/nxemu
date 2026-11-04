@echo off
setlocal
call "D:\VSBuildTools2022\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
for %%I in ("%~dp0..") do set "R=%%~fI"
set "OUT=%~1"
if not defined OUT set "OUT=%R%\build\native-probe"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /std:c++20 /utf-8 /EHsc /MT /O1 /I"%R%\src" /Fo"%OUT%\nx2-native-probe.obj" /Fe"%OUT%\nx2-native-probe.exe" "%R%\tools\nx2_native_probe.cpp"
exit /b %errorlevel%
