@echo off
rem Двойной клик по файлу = запуск всего проекта (бэкенд + фронтенд).
rem Нужен MSYS2 в C:\msys64 (или задайте MSYS2_ROOT).
chcp 65001 >nul
if "%MSYS2_ROOT%"=="" set MSYS2_ROOT=C:\msys64
if not exist "%MSYS2_ROOT%\usr\bin\bash.exe" (
  echo Не найден MSYS2 в %MSYS2_ROOT%. Установите MSYS2 или задайте переменную MSYS2_ROOT.
  pause
  exit /b 1
)
set MSYSTEM=UCRT64
set CHERE_INVOKING=1
set MSYS2_PATH_TYPE=inherit
"%MSYS2_ROOT%\usr\bin\bash.exe" -lc "./start.sh"
pause
