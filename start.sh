#!/usr/bin/env bash
# Запуск всего проекта одной командой: ./start.sh
# Собирает бэкенд (если нужно), запускает его и фронтенд, по Ctrl+C останавливает оба.
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

# --- бэкенд: сборка при необходимости ---
cd backend
EXE=build/infinity-router
[ -f "$EXE.exe" ] && EXE="$EXE.exe"
NEED_BUILD=0
[ -f "$EXE" ] || NEED_BUILD=1
if [ "$NEED_BUILD" = 0 ] && [ -n "$(find src CMakeLists.txt config.json -newer "$EXE" -print -quit 2>/dev/null)" ]; then
  NEED_BUILD=1   # исходники новее exe
fi
if [ "$NEED_BUILD" = 1 ]; then
  echo ">>> Сборка бэкенда..."
  if command -v ninja >/dev/null 2>&1; then GEN=Ninja; MARK=build/build.ninja
  else GEN="MSYS Makefiles"; MARK=build/Makefile; fi
  if [ ! -f "$MARK" ]; then
    echo ">>> Конфигурация cmake ($GEN)..."
    rm -rf build   # старый каталог мог быть создан другим генератором
    cmake -B build -G "$GEN" || { echo "Ошибка конфигурации cmake (нужны: pacman -S mingw-w64-ucrt-x86_64-{cmake,ninja,gcc,curl,sqlite3,openssl})"; exit 1; }
  fi
  cmake --build build || { echo "Ошибка сборки бэкенда"; exit 1; }
  EXE=build/infinity-router
  [ -f "$EXE.exe" ] && EXE="$EXE.exe"
fi

# --- фронтенд: зависимости при первом запуске ---
cd ../frontend
if [ ! -d node_modules ]; then
  echo ">>> npm install (первый запуск)..."
  npm install || { echo "Ошибка npm install"; exit 1; }
fi
cd ../backend

echo ">>> Запуск бэкенда на http://127.0.0.1:8080"
"./$EXE" &
BACK=$!
cleanup() { echo; echo ">>> Остановка..."; kill "$BACK" 2>/dev/null; wait "$BACK" 2>/dev/null; }
trap cleanup EXIT INT TERM

sleep 1
if ! kill -0 "$BACK" 2>/dev/null; then echo "Бэкенд не запустился"; exit 1; fi

echo ">>> Запуск фронтенда (откроется браузер, http://localhost:4200)"
cd ../frontend
npm start
