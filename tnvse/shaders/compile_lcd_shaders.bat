@echo off
setlocal EnableDelayedExpansion

if "%FXC%"=="" set "FXC=C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
if not exist "%FXC%" (
  echo fxc.exe was not found. Set the FXC environment variable first.
  exit /b 1
)

if not exist "%~dp0compiled" mkdir "%~dp0compiled"
for %%C in (0 1 2) do (
  if %%C==0 set "NAME=r"
  if %%C==1 set "NAME=g"
  if %%C==2 set "NAME=b"
  call "%FXC%" /nologo /T ps_2_0 /E Main /D LCD_CHANNEL=%%C /Fo "%~dp0compiled\tnvse_freetype_lcd_!NAME!.pso" "%~dp0freetype_lcd.hlsl"
  if errorlevel 1 exit /b 1
)

endlocal
