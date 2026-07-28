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
if exist "%~dp0compiled\tnvse_freetype_native_argb.pso" del /q "%~dp0compiled\tnvse_freetype_native_argb.pso"
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
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_native_coverage.pso" "%~dp0freetype_native_coverage.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_native_argb.pso" "%~dp0freetype_native_argb.hlsl"
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
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D COMPOSITE_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_composite_fast.pso" "%~dp0freetype_native_mtsdf_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D COMPOSITE_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_composite_balanced.pso" "%~dp0freetype_native_mtsdf_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D COMPOSITE_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_composite_high.pso" "%~dp0freetype_native_mtsdf_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D COMPOSITE_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_composite_fast.pso" "%~dp0freetype_native_mtsdf_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D COMPOSITE_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_composite_balanced.pso" "%~dp0freetype_native_mtsdf_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D COMPOSITE_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_composite_high.pso" "%~dp0freetype_native_mtsdf_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
call :compile_mtsdf_profiles 0 fast
if errorlevel 1 exit /b %errorlevel%
call :compile_mtsdf_profiles 1 balanced
if errorlevel 1 exit /b %errorlevel%
call :compile_mtsdf_profiles 2 high
if errorlevel 1 exit /b %errorlevel%
exit /b 0

:compile_mtsdf_profiles
for %%M in (8 9 10 11 12 13 14 15) do (
	call :compile_mtsdf_profile %1 %2 %%M 0
	if errorlevel 1 exit /b 1
)
for %%M in (9 11 13 15) do (
	call :compile_mtsdf_profile %1 %2 %%M 1
	if errorlevel 1 exit /b 1
)
exit /b 0

:compile_mtsdf_profile
set "PROFILE_SUFFIX="
if "%4"=="1" set "PROFILE_SUFFIX=_shift"
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D COMPOSITE_QUALITY=%1 /D COMPOSITE_STATIC_LAYER_MASK=%3 /D COMPOSITE_STATIC_SHIFTED_SHADOW=%4 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_composite_%2_m%3%PROFILE_SUFFIX%.pso" "%~dp0freetype_native_mtsdf_composite.hlsl"
exit /b %errorlevel%
