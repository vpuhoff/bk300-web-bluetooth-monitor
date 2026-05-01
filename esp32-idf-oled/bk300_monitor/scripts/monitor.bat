@echo off
REM Serial-монитор через pyserial miniterm (без IDF).
REM Запуск: monitor.bat [COMPORT] [BAUD]
setlocal

cd /d "%~dp0"

set PORT=%~1
if "%PORT%"=="" set PORT=COM5

set BAUD=%~2
if "%BAUD%"=="" set BAUD=115200

set PYTHON=python
where %PYTHON% >NUL 2>NUL
if errorlevel 1 (
    echo ERROR: Python не найден.
    exit /b 1
)

%PYTHON% -c "import serial.tools.miniterm" >NUL 2>NUL
if errorlevel 1 (
    echo ^>^> ставлю pyserial...
    %PYTHON% -m pip install --user --upgrade pyserial
)

echo ^>^> miniterm %PORT% @ %BAUD% (Ctrl+] для выхода)
%PYTHON% -m serial.tools.miniterm --raw %PORT% %BAUD%

endlocal
