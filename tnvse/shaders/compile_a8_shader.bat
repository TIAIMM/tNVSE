@echo off
setlocal

set "FXC=%WindowsSdkDir%bin\%WindowsSDKVersion%x64\fxc.exe"
if not exist "%FXC%" set "FXC=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"

if not exist "%FXC%" (
	echo Could not find fxc.exe.
	exit /b 1
)

if not exist "%~dp0compiled" mkdir "%~dp0compiled"
if exist "%~dp0compiled\tnvse_freetype_native_original.pso" del /q "%~dp0compiled\tnvse_freetype_native_original.pso"
if exist "%~dp0compiled\tnvse_freetype_native_coverage.pso" del /q "%~dp0compiled\tnvse_freetype_native_coverage.pso"
if exist "%~dp0compiled\tnvse_freetype_native_sdf.pso" del /q "%~dp0compiled\tnvse_freetype_native_sdf.pso"
if exist "%~dp0compiled\tnvse_freetype_native_effects_fast.pso" del /q "%~dp0compiled\tnvse_freetype_native_effects_fast.pso"
if exist "%~dp0compiled\tnvse_freetype_native_effects_balanced.pso" del /q "%~dp0compiled\tnvse_freetype_native_effects_balanced.pso"
if exist "%~dp0compiled\tnvse_freetype_native_effects_high.pso" del /q "%~dp0compiled\tnvse_freetype_native_effects_high.pso"
if exist "%~dp0compiled\tnvse_freetype_native_mtsdf_fill.pso" del /q "%~dp0compiled\tnvse_freetype_native_mtsdf_fill.pso"
if exist "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_subpixel_fast.pso" del /q "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_subpixel_fast.pso"
if exist "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_subpixel_balanced.pso" del /q "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_subpixel_balanced.pso"
if exist "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_subpixel_high.pso" del /q "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_subpixel_high.pso"
if exist "%~dp0compiled\tnvse_freetype_native_sdf_fill_subpixel_fast.pso" del /q "%~dp0compiled\tnvse_freetype_native_sdf_fill_subpixel_fast.pso"
if exist "%~dp0compiled\tnvse_freetype_native_sdf_fill_subpixel_balanced.pso" del /q "%~dp0compiled\tnvse_freetype_native_sdf_fill_subpixel_balanced.pso"
if exist "%~dp0compiled\tnvse_freetype_native_sdf_fill_subpixel_high.pso" del /q "%~dp0compiled\tnvse_freetype_native_sdf_fill_subpixel_high.pso"
"%FXC%" /nologo /T vs_3_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_native_vs.vso" "%~dp0freetype_native_vs.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D FILL_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_fast.pso" "%~dp0freetype_native_mtsdf_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D FILL_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_balanced.pso" "%~dp0freetype_native_mtsdf_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D FILL_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_high.pso" "%~dp0freetype_native_mtsdf_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D EFFECT_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_effects_fast.pso" "%~dp0freetype_native_mtsdf_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D EFFECT_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_effects_balanced.pso" "%~dp0freetype_native_mtsdf_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D EFFECT_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_effects_high.pso" "%~dp0freetype_native_mtsdf_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D FILL_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_fill_fast.pso" "%~dp0freetype_native_mtsdf_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D FILL_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_fill_balanced.pso" "%~dp0freetype_native_mtsdf_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D FILL_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_fill_high.pso" "%~dp0freetype_native_mtsdf_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D EFFECT_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_effects_fast.pso" "%~dp0freetype_native_mtsdf_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D EFFECT_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_effects_balanced.pso" "%~dp0freetype_native_mtsdf_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D EFFECT_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_effects_high.pso" "%~dp0freetype_native_mtsdf_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
exit /b 0
