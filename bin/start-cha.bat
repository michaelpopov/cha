@echo off
setlocal

set "CONFIG=..\cha.toml"
set "IMPORT_SEED=import-seed"

rem Starts CHA from this directory. The real config lives outside it.
for %%I in ("%~dp0.") do set "HERE=%%~fI"
set "EXECUTABLE=%HERE%\chaweb.exe"
for %%I in ("%HERE%\%CONFIG%") do set "CONFIG_PATH=%%~fI"
for %%I in ("%HERE%\%IMPORT_SEED%") do set "IMPORT_SEED_PATH=%%~fI"

if not exist "%EXECUTABLE%" (
    echo start-cha: no executable at %EXECUTABLE% 1>&2
    exit /b 1
)
if not exist "%CONFIG_PATH%" (
    echo start-cha: no configuration file at %CONFIG_PATH% 1>&2
    echo start-cha: copy and edit %HERE%\cha.toml.example 1>&2
    echo start-cha: then initialize its database explicitly: 1>&2
    echo   "%EXECUTABLE%" --config="%CONFIG_PATH%" --import "%IMPORT_SEED_PATH%" 1>&2
    exit /b 1
)

echo Press Ctrl+C to stop.
"%EXECUTABLE%" --root "%HERE%" --config="%CONFIG_PATH%"
exit /b %ERRORLEVEL%
