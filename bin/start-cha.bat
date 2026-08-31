@echo off
setlocal

set "HOST=0.0.0.0"
set "PORT=8086"
set "DATABASE=..\cha.sqlite3"
set "IMPORT_SEED=import-seed"

rem Starts CHA from this directory. The build stages chaweb.exe and web/ here.
for %%I in ("%~dp0.") do set "HERE=%%~fI"
set "EXECUTABLE=%HERE%\chaweb.exe"

if not exist "%EXECUTABLE%" (
    echo start-cha: no executable at %EXECUTABLE% 1>&2
    echo start-cha: build the project; the build copies chaweb.exe here 1>&2
    exit /b 1
)

for %%I in ("%HERE%\%DATABASE%") do set "DATABASE_PATH=%%~fI"
for %%I in ("%HERE%\%IMPORT_SEED%") do set "IMPORT_SEED_PATH=%%~fI"
if not exist "%DATABASE_PATH%" (
    echo start-cha: no database at %DATABASE_PATH% 1>&2
    echo start-cha: initialize it explicitly, then run this script again: 1>&2
    echo   "%EXECUTABLE%" --data "%DATABASE_PATH%" --import "%IMPORT_SEED_PATH%" 1>&2
    exit /b 1
)

set "URL_HOST=%HOST%"
if not "%URL_HOST::=%"=="%URL_HOST%" set "URL_HOST=[%URL_HOST%]"
echo CHA: http://%URL_HOST%:%PORT%/
echo Press Ctrl+C to stop.

"%EXECUTABLE%" --root "%HERE%" --data "%DATABASE_PATH%" --host "%HOST%" --port "%PORT%"
exit /b %ERRORLEVEL%
