#!/usr/bin/env bash
# Интеграционный тест каскада по настоящему HTTP:
#   ИИ №1 (Gemini) отвечает 429 RESOURCE_EXHAUSTED -> роутер переключается на ИИ №2 (OpenAI-формат).
# Использование: tests/integration_test.sh path/to/infinity-router
set -euo pipefail
BIN=${1:-./build/infinity-router}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
MOCK_PORT=18999
PORT=18080
PY=$(command -v python3 || command -v python)

cleanup() { kill ${MOCK_PID:-} ${SRV_PID:-} 2>/dev/null || true; rm -rf "$TMP"; }
trap cleanup EXIT

cat > "$TMP/config.json" <<EOF
{
  "port": $PORT, "db_path": "$TMP/test.db", "timeout_ms": 5000,
  "cascade": [
    { "id": "gemini", "label": "Gemini", "provider": "gemini", "model": "gemini-2.5-flash",
      "base_url": "http://127.0.0.1:$MOCK_PORT/gemini", "key_name": "Google AI Studio", "tier": "free" },
    { "id": "gpt-4o", "label": "GPT-4o", "provider": "openai", "model": "gpt-4o",
      "base_url": "http://127.0.0.1:$MOCK_PORT/openai", "key_name": "OpenAI", "tier": "paid" },
    { "id": "claude", "label": "Claude", "provider": "anthropic", "model": "claude-sonnet-5-5",
      "base_url": "http://127.0.0.1:$MOCK_PORT/anthropic", "api_key_env": "IT_ANTHROPIC_KEY", "tier": "paid" },
    { "id": "poll", "label": "FLUX (Pollinations)", "provider": "pollinations", "kind": "image", "model": "flux",
      "base_url": "http://127.0.0.1:$MOCK_PORT/pollinations/prompt", "priority": 3, "skills": ["images"] },
    { "id": "cf-flux", "label": "CF Flux", "provider": "cloudflare", "kind": "image", "model": "@cf/black-forest-labs/flux-1-schnell",
      "base_url": "http://127.0.0.1:$MOCK_PORT/cf/accounts/{account}/ai/run", "priority": 1, "skills": ["images"] },
    { "id": "g-tts", "label": "Gemini TTS", "provider": "gemini", "kind": "audio", "model": "tts",
      "base_url": "http://127.0.0.1:$MOCK_PORT/gemini-media" },
    { "id": "veo", "label": "Veo", "provider": "gemini", "kind": "video", "model": "veo", "tier": "paid",
      "base_url": "http://127.0.0.1:$MOCK_PORT/gemini-media" }
  ]
}
EOF
# Модель из каталога: попадает в каскад автоматически, как только для её провайдера введён ключ
cat > "$TMP/catalog.json" <<EOF
{ "models": [ { "id": "cat-mock", "label": "Каталог", "provider": "openai", "model": "m",
  "base_url": "http://127.0.0.1:$MOCK_PORT/catalog", "key_name": "MockCat", "tier": "free", "priority": 2, "limits": { "rpd": 1000 } } ] }
EOF

"$PY" "$HERE/mock_providers.py" $MOCK_PORT & MOCK_PID=$!
for i in $(seq 1 50); do curl -s -o /dev/null -X POST "http://127.0.0.1:$MOCK_PORT/ping" -d "{}" && break; sleep 0.1; done
IT_ANTHROPIC_KEY=a "$BIN" --config "$TMP/config.json" --env /dev/null 2> "$TMP/router.log" & SRV_PID=$!
for i in $(seq 1 50); do curl -s "http://127.0.0.1:$PORT/api/health" >/dev/null && break; sleep 0.1; done

fail() { echo "FAIL: $*"; echo "--- лог роутера:"; cat "$TMP/router.log"; exit 1; }
JAR="$TMP/cookies.txt"
JAR2="$TMP/cookies2.txt"
auth() { # auth <jar> <register|login> <login> <password>
  curl -s -b "$1" -c "$1" -X POST "http://127.0.0.1:$PORT/api/auth/$2" -H 'Content-Type: application/json' \
       -H 'Origin: http://localhost:4200' -d "{\"login\":\"$3\",\"password\":\"$4\",\"tz\":180}"; }

# --- Вход ---
ANON=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/api/chat" -d '{"messages":[{"role":"user","content":"hi"}]}')
[ "$ANON" = "401" ] || fail "без входа чат должен отвечать 401, получено $ANON"
R=$(auth "$JAR" register owner owner-pass-1)
echo "$R" | grep -q '"login":"owner"' || fail "регистрация: $R"
grep -q "iai_session" "$JAR" || fail "cookie сессии не выставлена"
grep -q "#HttpOnly_" "$JAR" || fail "cookie сессии должна быть HttpOnly"
ME=$(curl -s -b "$JAR" "http://127.0.0.1:$PORT/api/auth/me?tz=180")
echo "$ME" | grep -q '"login":"owner"' || fail "/api/auth/me: $ME"
BADPW=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/api/auth/login" -H 'Origin: http://localhost:4200' -d '{"login":"owner","password":"wrong-wrong"}')
[ "$BADPW" = "401" ] || fail "неверный пароль должен давать 401, получено $BADPW"
post() { curl -s -b "$JAR" -c "$JAR" -X POST "http://127.0.0.1:$PORT/api/chat" -H 'Content-Type: application/json' -d "$1"; }

# Ключи Gemini и OpenAI — как из «Настроек» сайта; Claude — из окружения (.env)
K=$(curl -s -b "$JAR" -c "$JAR" -X POST "http://127.0.0.1:$PORT/api/keys/bulk" -H 'Content-Type: application/json' -H 'Origin: http://localhost:4200' \
  -d '{"keys":{"Google AI Studio":"AIza-test-1111","OpenAI":"sk-test-2222"}}')
echo "$K" | grep -q '"saved":2' || fail "ключи не сохранились: $K"
KL=$(curl -s -b "$JAR" -c "$JAR" "http://127.0.0.1:$PORT/api/keys")
echo "$KL" | grep -q '"provider":"OpenAI","set":true,"source":"settings","hint":"…2222"' || fail "GET /api/keys: $KL"
echo "$KL" | grep -q '"provider":"Anthropic","set":true,"source":"env"' || fail "Anthropic должен браться из .env: $KL"
echo "$KL" | grep -q 'sk-test-2222' && fail "ключ целиком не должен возвращаться"
EVIL=$(curl -s -b "$JAR" -c "$JAR" -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/api/keys" -H 'Origin: https://evil.example' -d '{"provider":"OpenAI","key":"stolen"}')
[ "$EVIL" = "403" ] || fail "запрос с чужого сайта должен быть отклонён, получено $EVIL"

R1=$(post '{"messages":[{"role":"user","content":"Привет, кто ответит?"}]}')
echo "Ответ 1: $R1"
echo "$R1" | grep -q '"badge":"Ответил: GPT-4o"' || fail "ожидался бэйдж GPT-4o"
echo "$R1" | grep -q '"switched":true' || fail "ожидалось switched=true"
echo "$R1" | grep -q '"status":"quota","limit":true,"http_status":429' || fail "ожидалась попытка Gemini с 429/quota"
grep -q "Основной лимит исчерпан" "$TMP/router.log" || fail "нет записи о переключении в логе"
grep -q "Gemini на паузе 41 с" "$TMP/router.log" || fail "пауза должна браться из retryDelay (41s)"

R2=$(post '{"messages":[{"role":"user","content":"Ещё раз"}]}')
echo "$R2" | grep -q '"status":"cooldown"' || fail "Gemini должен быть на паузе и не вызываться"

R3=$(post '{"messages":[{"role":"user","content":"Закреплённая модель"}],"model":"claude"}')
echo "$R3" | grep -q '"badge":"Ответил: Claude"' || fail "закреплённая модель Claude должна ответить первой"

M=$(curl -s -b "$JAR" -c "$JAR" "http://127.0.0.1:$PORT/api/models")
echo "$M" | grep -q '"id":"gemini"' || fail "/api/models без gemini"

echo "$M" | grep -q '"id":"cat-mock"' && fail "модель каталога без ключа не должна попадать в каскад"

# --- Настройки ---
SET=$(curl -s -b "$JAR" -c "$JAR" "http://127.0.0.1:$PORT/api/settings")
echo "$SET" | grep -q '"threshold":90' || fail "GET /api/settings: $SET"
BAD=$(curl -s -b "$JAR" -c "$JAR" -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/api/settings" -H 'Origin: https://evil.example' -d '{"autoSwitch":false}')
[ "$BAD" = "403" ] || fail "настройки с чужого сайта должны отклоняться, получено $BAD"
setp() { curl -s -b "$JAR" -c "$JAR" -X POST "http://127.0.0.1:$PORT/api/settings" -H 'Content-Type: application/json' -H 'Origin: http://localhost:4200' -d "$1"; }

# Ввели ключ каталожного провайдера — модель сразу в каскаде, без перезапуска
curl -s -b "$JAR" -c "$JAR" -X POST "http://127.0.0.1:$PORT/api/keys" -H 'Origin: http://localhost:4200' -d '{"provider":"MockCat","key":"cat-key-3333"}' >/dev/null
M=$(curl -s -b "$JAR" -c "$JAR" "http://127.0.0.1:$PORT/api/models")
echo "$M" | grep -q '"id":"cat-mock"[^}]*"auto":true' || fail "модель каталога с ключом должна появиться: $M"
R4=$(post '{"messages":[{"role":"user","content":"a"},{"role":"assistant","content":"b"},{"role":"user","content":"c"}]}')
echo "$R4" | grep -q '"badge":"Ответил: Каталог"' || fail "ожидался ответ модели каталога: $R4"
echo "$R4" | grep -q 'Каталог: 3 сообщ' || fail "контекст должен передаваться целиком: $R4"

# Выключили модель на странице «Модели» — каскад её обходит
setp '{"disabled":["cat-mock"]}' | grep -q '"ok":true' || fail "disabled не сохранился"
R5=$(post '{"messages":[{"role":"user","content":"d"}]}')
echo "$R5" | grep -q '"badge":"Ответил: GPT-4o"' || fail "выключенная модель не должна отвечать: $R5"
setp '{"disabled":[]}' >/dev/null

# Шифрование ключей: в базе — только шифротекст, роутер по-прежнему работает
setp '{"encryptKeys":true}' | grep -q '"encryptKeys":true' || fail "encryptKeys не включилось"
RAW=$("$PY" -c "import sqlite3,sys; print([r[0] for r in sqlite3.connect(sys.argv[1]).execute('select api_key from user_keys')])" "$TMP/test.db")
echo "$RAW" | grep -q 'cat-key-3333\|sk-test-2222' && fail "ключи в базе должны быть зашифрованы: $RAW"
echo "$RAW" | grep -q 'enc:v1:' || fail "нет шифротекста: $RAW"
[ -s "$TMP/master.key" ] || fail "мастер-ключ не создан"
R6=$(post '{"messages":[{"role":"user","content":"e"}]}')
echo "$R6" | grep -q '"badge":"Ответил: Каталог"' || fail "после шифрования ключи должны расшифровываться: $R6"

# --- Картинки, аудио, видео ---
setp '{"encryptKeys":false}' >/dev/null
media_get() { curl -s -b "$JAR" -c "$JAR" -o "$TMP/media.bin" -w '%{http_code} %{content_type}' "http://127.0.0.1:$PORT$1"; }
url_of() { echo "$1" | sed -n 's/.*"url":"\([^"]*\)".*/\1/p'; }

# 1. Ключа Cloudflare нет → рисует Pollinations (без ключа); русский запрос переводит текстовая модель
I1=$(post '{"messages":[{"role":"user","content":"Нарисуй кота в шляпе"}]}')
echo "Картинка 1: $I1"
echo "$I1" | grep -q '"kind":"image"' || fail "ожидался ответ-картинка: $I1"
echo "$I1" | grep -q '"badge":"Нарисовал: FLUX (Pollinations)"' || fail "рисовать должен Pollinations: $I1"
echo "$I1" | grep -q '"prompt_model":"Каталог"\|"prompt_model":"GPT-4o"' || fail "промпт должна готовить текстовая модель: $I1"
U1=$(url_of "$I1")
[ "$(media_get "$U1")" = "200 image/png" ] || fail "картинка не отдаётся по $U1"
head -c 4 "$TMP/media.bin" | grep -q PNG || fail "это не PNG"

# 2. Ввели ключ Cloudflare → в чате «Изображения» рисует FLUX на Cloudflare (качественнее Pollinations)
curl -s -b "$JAR" -c "$JAR" -X POST "http://127.0.0.1:$PORT/api/keys" -H 'Origin: http://localhost:4200' -d '{"provider":"Cloudflare Workers AI","key":"ACC:cftok"}' >/dev/null
I2=$(post '{"messages":[{"role":"user","content":"cat astronaut, watercolor"}],"skill":"images"}')
echo "$I2" | grep -q '"badge":"Нарисовал: CF Flux"' || fail "ожидалась картинка от Cloudflare: $I2"
[ "$(media_get "$(url_of "$I2")")" = "200 image/jpeg" ] || fail "картинка Cloudflare не отдаётся"

# 3. Озвучка: русский текст — Gemini TTS, PCM упакован в WAV
A1=$(post '{"messages":[{"role":"user","content":"Озвучь: Привет, мир!"}]}')
echo "$A1" | grep -q '"badge":"Озвучил: Gemini TTS"' || fail "ожидалась озвучка: $A1"
echo "$A1" | grep -q '"prompt":"Привет, мир!"' || fail "озвучить нужно текст после двоеточия: $A1"
[ "$(media_get "$(url_of "$A1")")" = "200 audio/wav" ] || fail "аудио не отдаётся"
head -c 12 "$TMP/media.bin" | grep -q WAVE || fail "это не WAV"

# 4. Видео: долгая операция Veo → опрос → скачивание через редирект
V1=$(post '{"messages":[{"role":"user","content":"Сгенерируй видео: волны на рассвете"}]}')
echo "$V1" | grep -q '"badge":"Снял видео: Veo"' || fail "ожидалось видео: $V1"
[ "$(media_get "$(url_of "$V1")")" = "200 video/mp4" ] || fail "видео не отдаётся"
grep -q "AIza-test-1111" "$TMP/router.log" && fail "ключ не должен попадать в лог"

# 5. Чужие файлы через /api/media не отдаются
for bad in "/api/media/../config.json" "/api/media/abc.exe" "/api/media/XYZ.png" "/api/media/%2e%2e%2fconfig.json"; do
  [ "$(curl -s -b "$JAR" -c "$JAR" -o /dev/null -w '%{http_code}' --path-as-is "http://127.0.0.1:$PORT$bad")" = "404" ] || fail "$bad должен давать 404"
done

# 6. Второй пользователь: своих ключей нет, ключ из .env (Claude) — только владельцу, чужие файлы не видны
auth "$JAR2" register second second-pass-1 | grep -q '"login":"second"' || fail "регистрация второго пользователя"
K2=$(curl -s -b "$JAR2" "http://127.0.0.1:$PORT/api/keys")
echo "$K2" | grep -q '"set":true' && fail "у нового пользователя не должно быть ключей: $K2"
[ "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR2" "http://127.0.0.1:$PORT$U1")" = "404" ] || fail "чужая картинка должна давать 404"
[ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT$U1")" = "401" ] || fail "файл без входа должен давать 401"
S2=$(curl -s -b "$JAR2" "http://127.0.0.1:$PORT/api/stats")
echo "$S2" | grep -q '"requests":0,"tokens_in":0' || fail "статистика второго пользователя должна быть пустой: $S2"

# 7. Статистика владельца: сегодня — посещение и расход
ST=$(curl -s -b "$JAR" "http://127.0.0.1:$PORT/api/stats")
echo "Статистика: $(echo "$ST" | head -c 300)"
echo "$ST" | grep -q '"visited":true' || fail "сегодня должен быть отмечен как день посещения: $ST"
echo "$ST" | grep -q '"daily_limit":1000000' || fail "дневной лимит по умолчанию — 1 млн токенов: $ST"

# 8. Выход
curl -s -b "$JAR" -c "$JAR" -X POST "http://127.0.0.1:$PORT/api/auth/logout" -H 'Origin: http://localhost:4200' >/dev/null
[ "$(curl -s -o /dev/null -w '%{http_code}' -b "$JAR" "http://127.0.0.1:$PORT/api/models")" = "401" ] || fail "после выхода доступ должен закрыться"
python3 - "$TMP/test.db" <<'PY' || fail "в базе не должно быть паролей и токенов в открытом виде"
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
rows = db.execute("select pass from users").fetchall()
assert rows and all(r[0].startswith("pbkdf2_sha256$") and "pass-1" not in r[0] for r in rows)
assert all(len(r[0]) == 64 for r in db.execute("select token_hash from sessions"))
PY

echo "--- лог роутера:"; cat "$TMP/router.log"
echo "OK: интеграционный тест каскада пройден"
