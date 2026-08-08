@echo off
setlocal EnableExtensions

set "FXC=%WindowsSdkDir%bin\%WindowsSDKVersion%x64\fxc.exe"
if not exist "%FXC%" set "FXC=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"

if not exist "%FXC%" (
	echo Could not find fxc.exe.
	exit /b 1
)

if not exist "%~dp0compiled" mkdir "%~dp0compiled"
del /q "%~dp0compiled\tnvse_freetype_native_*.pso" 2>nul
del /q "%~dp0compiled\tnvse_freetype_native_*.vso" 2>nul
for %%F in ("%~dp0compiled\tnvse_freetype_native_*.pso" "%~dp0compiled\tnvse_freetype_native_*.vso") do (
	if exist "%%~fF" (
		echo Could not remove stale shader output %%~fF.
		exit /b 1
	)
)
"%FXC%" /nologo /T vs_3_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_native_vs.vso" "%~dp0freetype_native_vs.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T vs_3_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_native_vanilla_layout_vs.vso" "%~dp0freetype_native_vanilla_layout_vs.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T vs_3_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_native_vanilla_parametric_vs.vso" "%~dp0freetype_native_vanilla_parametric_vs.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_native_coverage.pso" "%~dp0freetype_native_coverage.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /Fo "%~dp0compiled\tnvse_freetype_native_argb.pso" "%~dp0freetype_native_argb.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D FILL_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_fast.pso" "%~dp0freetype_native_distance_field_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D FILL_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_balanced.pso" "%~dp0freetype_native_distance_field_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D FILL_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_fill_high.pso" "%~dp0freetype_native_distance_field_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D EFFECT_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_effects_fast.pso" "%~dp0freetype_native_distance_field_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D EFFECT_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_effects_balanced.pso" "%~dp0freetype_native_distance_field_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D EFFECT_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_effects_high.pso" "%~dp0freetype_native_distance_field_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D FILL_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_fill_fast.pso" "%~dp0freetype_native_distance_field_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D FILL_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_fill_balanced.pso" "%~dp0freetype_native_distance_field_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D FILL_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_fill_high.pso" "%~dp0freetype_native_distance_field_fill.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D EFFECT_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_effects_fast.pso" "%~dp0freetype_native_distance_field_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D EFFECT_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_effects_balanced.pso" "%~dp0freetype_native_distance_field_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D EFFECT_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_effects_high.pso" "%~dp0freetype_native_distance_field_effects.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D COMPOSITE_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_composite_fast.pso" "%~dp0freetype_native_distance_field_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D COMPOSITE_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_composite_balanced.pso" "%~dp0freetype_native_distance_field_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D COMPOSITE_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_composite_high.pso" "%~dp0freetype_native_distance_field_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D COMPOSITE_QUALITY=0 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_composite_fast.pso" "%~dp0freetype_native_distance_field_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D COMPOSITE_QUALITY=1 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_composite_balanced.pso" "%~dp0freetype_native_distance_field_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D COMPOSITE_QUALITY=2 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_composite_high.pso" "%~dp0freetype_native_distance_field_composite.hlsl"
if errorlevel 1 exit /b %errorlevel%
call :compile_mtsdf_profiles 0 fast
if errorlevel 1 exit /b %errorlevel%
call :compile_mtsdf_profiles 1 balanced
if errorlevel 1 exit /b %errorlevel%
call :compile_mtsdf_profiles 2 high
if errorlevel 1 exit /b %errorlevel%
call :compile_vanilla_layout_profiles 0 fast
if errorlevel 1 exit /b %errorlevel%
call :compile_vanilla_layout_profiles 1 balanced
if errorlevel 1 exit /b %errorlevel%
call :compile_vanilla_layout_profiles 2 high
if errorlevel 1 exit /b %errorlevel%
call :compile_true_sdf_vanilla_layout_profiles 0 fast
if errorlevel 1 exit /b %errorlevel%
call :compile_true_sdf_vanilla_layout_profiles 1 balanced
if errorlevel 1 exit /b %errorlevel%
call :compile_true_sdf_vanilla_layout_profiles 2 high
if errorlevel 1 exit /b %errorlevel%
for /f %%C in ('dir /b /a-d "%~dp0compiled\tnvse_freetype_native_*.pso" "%~dp0compiled\tnvse_freetype_native_*.vso" 2^>nul ^| find /c /v ""') do set "OUTPUT_COUNT=%%C"
if not "%OUTPUT_COUNT%"=="131" (
	echo Expected 131 native shader outputs, found %OUTPUT_COUNT%.
	exit /b 1
)
for %%F in ("%~dp0compiled\tnvse_freetype_native_*.pso" "%~dp0compiled\tnvse_freetype_native_*.vso") do (
	if %%~zF LEQ 0 (
		echo Shader output is empty: %%~fF.
		exit /b 1
	)
)
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
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D COMPOSITE_QUALITY=%1 /D COMPOSITE_STATIC_LAYER_MASK=%3 /D COMPOSITE_STATIC_SHIFTED_SHADOW=%4 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_composite_%2_m%3%PROFILE_SUFFIX%.pso" "%~dp0freetype_native_distance_field_composite.hlsl"
exit /b %errorlevel%

:compile_vanilla_layout_profiles
for %%M in (8 9 10 11 12 13 14 15) do (
	call :compile_vanilla_layout_profile %1 %2 %%M 0
	if errorlevel 1 exit /b 1
)
for %%M in (9 11 13 15) do (
	call :compile_vanilla_layout_profile %1 %2 %%M 1
	if errorlevel 1 exit /b 1
)
exit /b 0

:compile_vanilla_layout_profile
set "PROFILE_SUFFIX="
if "%4"=="1" set "PROFILE_SUFFIX=_shift"
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D COMPOSITE_QUALITY=%1 /D COMPOSITE_STATIC_LAYER_MASK=%3 /D COMPOSITE_STATIC_SHIFTED_SHADOW=%4 /Fo "%~dp0compiled\tnvse_freetype_native_mtsdf_vanilla_layout_%2_m%3%PROFILE_SUFFIX%.pso" "%~dp0freetype_native_distance_field_composite.hlsl"
exit /b %errorlevel%

:compile_true_sdf_vanilla_layout_profiles
for %%M in (8 9 10 11 12 13 14 15) do (
	call :compile_true_sdf_vanilla_layout_profile %1 %2 %%M 0
	if errorlevel 1 exit /b 1
)
for %%M in (9 11 13 15) do (
	call :compile_true_sdf_vanilla_layout_profile %1 %2 %%M 1
	if errorlevel 1 exit /b 1
)
exit /b 0

:compile_true_sdf_vanilla_layout_profile
set "PROFILE_SUFFIX="
if "%4"=="1" set "PROFILE_SUFFIX=_shift"
"%FXC%" /nologo /T ps_3_0 /E Main /O3 /D DISTANCE_FIELD_TRUE_SDF=1 /D COMPOSITE_QUALITY=%1 /D COMPOSITE_STATIC_LAYER_MASK=%3 /D COMPOSITE_STATIC_SHIFTED_SHADOW=%4 /Fo "%~dp0compiled\tnvse_freetype_native_sdf_vanilla_layout_%2_m%3%PROFILE_SUFFIX%.pso" "%~dp0freetype_native_distance_field_composite.hlsl"
exit /b %errorlevel%
