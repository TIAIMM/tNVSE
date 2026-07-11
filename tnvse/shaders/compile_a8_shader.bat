@echo off
setlocal

set "FXC=%WindowsSdkDir%bin\%WindowsSDKVersion%x64\fxc.exe"
if not exist "%FXC%" set "FXC=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"

if not exist "%FXC%" (
	echo Could not find fxc.exe.
	exit /b 1
)

if not exist "%~dp0compiled" mkdir "%~dp0compiled"
"%FXC%" /nologo /T ps_2_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_a8.pso" "%~dp0freetype_a8.hlsl"
exit /b %errorlevel%
