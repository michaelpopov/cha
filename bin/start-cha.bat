@echo off
setlocal

set "HOST=0.0.0.0"
set "PORT=8086"
set "WORKSPACE=..\workspace"

rem Starts CHA from this directory. The build stages chaweb.exe and web/ here.
for %%I in ("%~dp0.") do set "HERE=%%~fI"
set "EXECUTABLE=%HERE%\chaweb.exe"

if not exist "%EXECUTABLE%" (
    echo start-cha: no executable at %EXECUTABLE% 1>&2
    echo start-cha: build the project; the build copies chaweb.exe here 1>&2
    exit /b 1
)

for %%I in ("%HERE%\%WORKSPACE%") do set "WORKSPACE_PATH=%%~fI"
if not exist "%WORKSPACE_PATH%\" (
    echo start-cha: no workspace at %WORKSPACE_PATH% 1>&2
    echo start-cha: set WORKSPACE at the top of this script 1>&2
    exit /b 1
)

set "URL_HOST=%HOST%"
if not "%URL_HOST::=%"=="%URL_HOST%" set "URL_HOST=[%URL_HOST%]"
echo CHA: http://%URL_HOST%:%PORT%/
echo Press Ctrl+C to stop.

"%EXECUTABLE%" --root "%HERE%" --workspace "%WORKSPACE_PATH%" --host "%HOST%" --port "%PORT%"
exit /b %ERRORLEVEL%
