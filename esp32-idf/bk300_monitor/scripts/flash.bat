@echo off
REM Прошивка bk300_monitor через esptool (Windows).
REM Запуск: flash.bat [COMPORT] [BAUD]
REM   COMPORT — например COM5 (по умолчанию COM5).
REM   BAUD — скорость (по умолчанию 460800).
REM Требуется: Python 3.8+ + `pip install esptool`.

setlocal ENABLEDELAYEDEXPANSION

cd /d "%~dp0"

if exist flash.env (
    for /F "usebackq tokens=1,2 delims==" %%A in (`findstr /B /V "#" flash.env`) do (
        if not "%%A"=="" set "%%A=%%B"
    )
)

if "%CHIP%"=="" set CHIP=esp32s3
if "%PROJECT_NAME%"=="" set PROJECT_NAME=bk300_monitor

set PORT=%~1
if "%PORT%"=="" set PORT=%PORT_DEFAULT%
if "%PORT%"=="" set PORT=COM5

set BAUD=%~2
if "%BAUD%"=="" set BAUD=460800

set PYTHON=python
where %PYTHON% >NUL 2>NUL
if errorlevel 1 (
    echo ERROR: Python не найден в PATH. Поставь Python 3.8+ и перезапусти.
    exit /b 1
)

%PYTHON% -m esptool version >NUL 2>NUL
if errorlevel 1 (
    echo ^>^> esptool не установлен, ставлю...
    %PYTHON% -m pip install --user --upgrade esptool || (
        echo ERROR: не удалось поставить esptool. Запусти вручную: %PYTHON% -m pip install esptool
        exit /b 1
    )
)

echo ^>^> CHIP=%CHIP% PORT=%PORT% BAUD=%BAUD%
echo ^>^> esptool write_flash @flash_args

%PYTHON% -m esptool ^
    --chip %CHIP% ^
    -p %PORT% ^
    -b %BAUD% ^
    --before default_reset ^
    --after hard_reset ^
    write_flash ^
    @flash_args

endlocal
