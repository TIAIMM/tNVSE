@echo off
setlocal

set "FXC=%WindowsSdkDir%bin\%WindowsSDKVersion%x64\fxc.exe"
if not exist "%FXC%" set "FXC=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"

if not exist "%FXC%" (
	echo Could not find fxc.exe.
	exit /b 1
)

if not exist "%~dp0compiled" mkdir "%~dp0compiled"
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_a8.pso" "%~dp0freetype_a8.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D EFFECT_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_effects_fast.pso" "%~dp0freetype_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D EFFECT_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_effects_balanced.pso" "%~dp0freetype_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D EFFECT_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_effects_high.pso" "%~dp0freetype_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify_shader_contract.ps1" -Fxc "%FXC%" -ShaderDirectory "%~dp0."
exit /b %errorlevel%
