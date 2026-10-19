#!/usr/bin/env python3
"""Имитатор API провайдеров для интеграционного теста каскада (только стандартная библиотека).

  POST /gemini/models/<model>:generateContent  -> 429 RESOURCE_EXHAUSTED (как бесплатный тир Gemini)
  POST /gemini-ok/models/<model>:generateContent -> 200 в формате Gemini
  POST /openai/chat/completions                  -> 200 в формате OpenAI
  POST /anthropic                                -> 200 в формате Anthropic Messages

Запуск: python3 mock_providers.py 18999
"""
import base64
import json
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

GEMINI_429 = {
    "error": {
        "code": 429,
        "message": "You exceeded your current quota, please check your plan and billing details.",
        "status": "RESOURCE_EXHAUSTED",
        "details": [
            {"@type": "type.googleapis.com/google.rpc.QuotaFailure",
             "violations": [{"quotaId": "GenerateRequestsPerMinutePerProjectPerModel-FreeTier"}]},
            {"@type": "type.googleapis.com/google.rpc.RetryInfo", "retryDelay": "41s"},
        ],
    }
}


PNG = b"\x89PNG\r\n\x1a\n" + b"\x00" * 24
JPEG = b"\xff\xd8\xff\xe0" + b"\x00" * 24
MP4 = b"\x00\x00\x00\x18ftypisom" + b"\x00" * 24
PCM = b"\x01\x00" * 240
VEO_POLLS = {"n": 0}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):  # тихо
        pass

    def reply(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def raw(self, code, data, ctype, extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        if url.path.startswith("/pollinations/prompt/"):  # Pollinations: картинка без ключа
            prompt = urllib.parse.unquote(url.path[len("/pollinations/prompt/"):])
            if not prompt or "nologo=true" not in url.query:
                return self.reply(400, {"error": "bad request"})
            return self.raw(200, PNG, "image/jpeg")  # тип в заголовке неточный — роутер определяет по сигнатуре
        if url.path == "/gemini-media/operations/op1":  # Veo: операция готова со второго опроса
            if self.headers.get("x-goog-api-key") != "AIza-test-1111":
                return self.reply(403, {"error": {"code": 403, "message": "no key"}})
            VEO_POLLS["n"] += 1
            if VEO_POLLS["n"] < 2:
                return self.reply(200, {"name": "operations/op1", "done": False})
            uri = f"http://127.0.0.1:{self.server.server_port}/files/v1:download?alt=media"
            return self.reply(200, {"name": "operations/op1", "done": True, "response": {
                "generateVideoResponse": {"generatedSamples": [{"video": {"uri": uri}}]}}})
        if url.path == "/files/v1:download":  # скачивание видео: редирект, ключ — параметром
            if urllib.parse.parse_qs(url.query).get("key") != ["AIza-test-1111"]:
                return self.reply(403, {"error": {"message": "no key"}})
            return self.raw(302, b"", "text/plain", {"Location": "/blob/v1.mp4"})
        if url.path == "/blob/v1.mp4":
            return self.raw(200, MP4, "video/mp4")
        self.reply(404, {"error": {"message": "not found"}})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        if self.path.startswith("/gemini/"):
            if self.headers.get("x-goog-api-key") != "AIza-test-1111":  # ключ из «Настроек» сайта
                return self.reply(400, {"error": {"code": 400, "message": "API key not valid."}})
            return self.reply(429, GEMINI_429)
        if self.path.startswith("/gemini-ok/"):
            text = body["contents"][-1]["parts"][0]["text"]
            return self.reply(200, {"candidates": [{"content": {"parts": [{"text": f"Gemini: {text}"}]}}],
                                    "usageMetadata": {"promptTokenCount": 5, "candidatesTokenCount": 7}})
        if self.path == "/openai/chat/completions":
            if self.headers.get("Authorization") != "Bearer sk-test-2222":  # ключ из «Настроек» сайта
                return self.reply(401, {"error": {"message": "Incorrect API key provided"}})
            last = body["messages"][-1]["content"]
            return self.reply(200, {"choices": [{"message": {"role": "assistant", "content": f"GPT-4o отвечает: {last}"}}],
                                    "usage": {"prompt_tokens": 11, "completion_tokens": 9}})
        if self.path == "/catalog/chat/completions":
            if self.headers.get("Authorization") != "Bearer cat-key-3333":  # ключ каталожного провайдера из «Настроек»
                return self.reply(401, {"error": {"message": "Incorrect API key provided"}})
            return self.reply(200, {"choices": [{"message": {"role": "assistant", "content": f"Каталог: {len(body['messages'])} сообщ."}}]})
        if self.path == "/cf/accounts/ACC/ai/run/@cf/black-forest-labs/flux-1-schnell":  # Cloudflare: JSON с base64
            if self.headers.get("Authorization") != "Bearer cftok":
                return self.reply(401, {"errors": [{"message": "Authentication error", "code": 10000}], "success": False})
            if not body.get("prompt"):
                return self.reply(400, {"errors": [{"message": "prompt required"}], "success": False})
            return self.reply(200, {"result": {"image": base64.b64encode(JPEG).decode()}, "success": True})
        if self.path == "/gemini-media/models/tts:generateContent":  # Gemini TTS: «сырой» PCM
            if self.headers.get("x-goog-api-key") != "AIza-test-1111":
                return self.reply(400, {"error": {"code": 400, "message": "API key not valid."}})
            assert body["generationConfig"]["responseModalities"] == ["AUDIO"]
            return self.reply(200, {"candidates": [{"content": {"parts": [{"inlineData": {
                "mimeType": "audio/L16;codec=pcm;rate=24000", "data": base64.b64encode(PCM).decode()}}]}}]})
        if self.path == "/gemini-media/models/veo:predictLongRunning":
            if self.headers.get("x-goog-api-key") != "AIza-test-1111":
                return self.reply(403, {"error": {"code": 403, "message": "no key"}})
            VEO_POLLS["n"] = 0
            return self.reply(200, {"name": "operations/op1"})
        if self.path == "/anthropic":
            if self.headers.get("x-api-key") != "a" or self.headers.get("anthropic-version") != "2023-06-01":  # ключ из .env
                return self.reply(401, {"type": "error", "error": {"type": "authentication_error", "message": "invalid x-api-key"}})
            return self.reply(200, {"content": [{"type": "text", "text": "Claude: ok"}],
                                    "usage": {"input_tokens": 3, "output_tokens": 2}})
        self.reply(404, {"error": {"message": "not found"}})


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18999
    ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
