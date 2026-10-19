# Генератор каталога бесплатных моделей для infinityAI -> src/app/core/catalog.ts
# Источники: github.com/cheahjs/free-llm-api-resources (окт. 2026), developers.cloudflare.com/workers-ai/models,
# build.nvidia.com/models, docs.mistral.ai, docs.github.com (GitHub Models rate limits), costbench.com (профили сервисов).
import json, re, sys

E = []  # entries

def add(name, provider, skills, access, tier, limit, unit, period, note, url):
    E.append(dict(name=name, provider=provider, skills=skills, access=access, tier=tier,
                  limit=limit, unit=unit, period=period, note=note, url=url))

GEN = ['text', 'docs', 'scripts', 'marketing']
BIG = GEN + ['code', 'finance', 'sheets']
CODE = ['code']

def llm_skills(n):
    l = n.lower()
    if any(k in l for k in ['coder', 'codestral', 'devstral', 'laguna', 'code']):
        return CODE + (['text'] if 'kimi' in l else [])
    if any(k in l for k in ['translate', 'm2m100', 'indictrans', 'aya-expanse', 'riva-translate']):
        return ['text', 'docs']
    big = ['120b', '70b', '235b', '397b', '480b', '550b', 'ultra', 'super', 'deepseek', 'kimi', 'glm-5', 'glm 5', 'gemini',
           'command-a', 'r1', 'qwq', 'minimax', 'step', 'large', 'medium', 'gpt-oss-120b', 'gemma-4-31b', 'gemma 4 31b',
           'qwen3.6', 'qwen3.5', 'qwen3.8', 'compound', 'grok', 'mimo', 'big pickle', 'jamba', 'solar pro']
    return BIG if any(k in l for k in big) else GEN

def llm(name, provider, *a, skills=None):
    add(name, provider, skills or llm_skills(name), *a)

# ---------- OpenRouter: 20 rpm, 50 req/day (общий) ----------
U = 'https://openrouter.ai/models?max_price=0'
N = 'Общий лимит на все бесплатные модели OpenRouter: 20 запросов/мин, 50/день (1000/день после разового пополнения на $10)'
for n in ['Cohere North Mini Code', 'Ling 3.0 Flash', 'NVIDIA Nemotron 3 Nano Omni 30B A3B', 'NVIDIA Nemotron 3 Super 120B A12B',
          'NVIDIA Nemotron 3 Ultra 550B A55B', 'Poolside Laguna S 2.1', 'Poolside Laguna XS 2.1', 'Gemma 4 26B A4B', 'Gemma 4 31B',
          'Nemotron 3 Nano 30B A3B', 'Nemotron Nano 12B v2 VL', 'Nemotron Nano 9B v2', 'gpt-oss-20b',
          'Nemotron 3.5 Super VL', 'Xing4.0 29B A4B']:
    llm(n, 'OpenRouter', 'api', 'free', 50, 'requests', 'day', N, U)

# ---------- Google AI Studio ----------
U = 'https://aistudio.google.com'
for n, rpd, rpm, tpm in [('Gemini 3.6 Flash', 20, 5, '250K'), ('Gemini 3.5 Flash', 20, 5, '250K'), ('Gemini 3 Flash', 20, 5, '250K'),
                         ('Gemini 3.5 Flash-Lite', 500, 15, '250K'), ('Gemini 3.1 Flash-Lite', 500, 15, '250K'),
                         ('Gemini 2.5 Flash', 20, 5, '250K'), ('Gemini 2.5 Flash-Lite', 20, 10, '250K'),
                         ('Gemma 4 31B Instruct', 14400, 30, '16K'), ('Gemma 4 26B A4B Instruct', 14400, 30, '16K'),
                         ('Gemma 3 27B Instruct', 14400, 30, '15K'), ('Gemma 3 12B Instruct', 14400, 30, '15K'),
                         ('Gemma 3 4B Instruct', 14400, 30, '15K'), ('Gemma 3 1B Instruct', 14400, 30, '15K')]:
    llm(n, 'Google AI Studio', 'api', 'free', rpd, 'requests', 'day',
        f'{rpd} запросов/день, {rpm} в минуту, {tpm} токенов/мин. Вне ЕС данные могут использоваться для обучения', U,
        skills=BIG if 'Gemini' in n or '31B' in n or '27B' in n else GEN)
for n in ['Gemini 3.1 Flash TTS', 'Gemini 2.5 Flash TTS']:
    add(n, 'Google AI Studio', ['audio'], 'api', 'free', 10, 'requests', 'day', '10 запросов/день, 3 в минуту, 10K токенов/мин', U)

# ---------- Groq ----------
U = 'https://console.groq.com'
for n, rpd, tpm in [('Allam 2 7B', 7000, '6K'), ('Llama 3.1 8B', 14400, '6K'), ('Llama 3.3 70B', 1000, '12K'),
                    ('Groq Compound', 250, '70K'), ('Groq Compound Mini', 250, '70K'), ('gpt-oss-120b', 1000, '8K'),
                    ('gpt-oss-20b', 1000, '8K'), ('Qwen3.6 27B', 1000, '8K')]:
    llm(n, 'Groq', 'api', 'free', rpd, 'requests', 'day', f'{rpd} запросов/день, {tpm} токенов/мин', U)
for n in ['Whisper Large v3', 'Whisper Large v3 Turbo']:
    add(n, 'Groq', ['audio', 'video'], 'api', 'free', 2000, 'requests', 'day', '2000 запросов/день (расшифровка аудио)', U)
for n in ['Orpheus v1 English', 'Orpheus Arabic Saudi']:
    add(n, 'Groq', ['audio'], 'api', 'free', 0, 'requests', 'day', 'Бесплатно, лимит не опубликован (синтез речи)', U)

# ---------- Cohere: 1000 req/month общий ----------
U = 'https://dashboard.cohere.com'
N = 'Общий лимит: 1000 запросов/месяц, 20 в минуту'
for n in ['Command A', 'Command A Plus', 'Command A Reasoning', 'Command A Translate', 'Command A Vision', 'Command R',
          'Command R+', 'Command R7B', 'Command R7B Arabic', 'Aya Expanse 32B', 'Aya Vision 32B']:
    llm(n, 'Cohere', 'api', 'free', 1000, 'requests', 'month', N, U)

# ---------- Cloudflare Workers AI: 10 000 нейронов/день общий ----------
U = 'https://developers.cloudflare.com/workers-ai/models/'
N = 'Общий лимит: 10 000 нейронов/день на все модели Workers AI'
cf_text = ['apertus-v1.5-8b', 'clef', 'clef-flash', 'clef-omni', 'deepseek-r1-distill-qwen-32b', 'deepseek-v4-flash-0731',
           'deepseek-v4-pro-0813', 'eurollm-9b-it', 'gemma-2b-it-lora', 'gemma-4-26b-a4b-it', 'gemma-7b-it-lora',
           'gemma-sea-lion-v4-27b-it', 'glm-4.7-flash', 'glm-5.2', 'glm-5.3', 'glm-5.3-flash', 'gpt-oss-120b', 'gpt-oss-20b',
           'granite-4.0-h-micro', 'kimi-k2.6', 'kimi-k2.7-code', 'llama-2-7b-chat-hf-lora', 'llama-3.1-8b-instruct-fp8',
           'llama-3.2-11b-vision-instruct', 'llama-3.2-1b-instruct', 'llama-3.2-3b-instruct', 'llama-3.3-70b-instruct-fp8-fast',
           'llama-4-scout-17b-16e-instruct', 'mistral-7b-instruct-v0.2-lora', 'mistral-small-3.1-24b-instruct',
           'nemotron-3-120b-a12b', 'phonellm-mini-alpha-2', 'qwen2.5-coder-32b-instruct', 'qwen3-30b-a3b-fp8', 'qwen3.8-27b', 'qwq-32b']
for n in cf_text:
    llm(n, 'Cloudflare Workers AI', 'api', 'free', 10000, 'neurons', 'day', N, U)
for n in ['aura-1', 'aura-2-en', 'aura-2-es', 'melotts']:
    add(n, 'Cloudflare Workers AI', ['audio'], 'api', 'free', 10000, 'neurons', 'day', N + ' (синтез речи)', U)
for n in ['flux (ASR)', 'nova-3', 'whisper', 'whisper-large-v3-turbo', 'whisper-tiny-en']:
    add(n, 'Cloudflare Workers AI', ['audio', 'video'], 'api', 'free', 10000, 'neurons', 'day', N + ' (распознавание речи)', U)
for n in ['dreamshaper-8-lcm', 'flux-1-schnell', 'flux-2-dev', 'flux-2-klein-4b', 'flux-2-klein-9b', 'lucid-origin', 'phoenix-1.0',
          'stable-diffusion-v1-5-inpainting', 'stable-diffusion-xl-base-1.0', 'stable-diffusion-xl-lightning']:
    add(n, 'Cloudflare Workers AI', ['images', 'design'], 'api', 'free', 10000, 'neurons', 'day', N + ' (генерация изображений)', U)
for n in ['indictrans2-en-indic-1B', 'm2m100-1.2b']:
    add(n, 'Cloudflare Workers AI', ['text', 'docs'], 'api', 'free', 10000, 'neurons', 'day', N + ' (перевод)', U)
for n in ['llava-1.5-7b-hf', 'moondream3.1-9B-A2B']:
    add(n, 'Cloudflare Workers AI', ['images', 'docs'], 'api', 'free', 10000, 'neurons', 'day', N + ' (описание изображений)', U)

# ---------- Cerebras ----------
U = 'https://cloud.cerebras.ai'
for n in ['gpt-oss-120b', 'GLM 4.7', 'Gemma 4 31B']:
    llm(n, 'Cerebras', 'api', 'free', 1_000_000, 'tokens', 'day', '1 млн токенов/день, 30K токенов/мин, 5 запросов/мин', U)

# ---------- Kilo Gateway: 200 req/hour по IP ----------
U = 'https://kilo.ai'
N = 'Общий лимит: 200 запросов/час на IP; бесплатные модели могут использовать запросы для обучения'
for n in ['Cohere North Mini Code', 'Kilo Auto Free (роутер)', 'Ling 3.0 Flash', 'Nemotron 3 Nano Omni 30B A3B',
          'Nemotron 3 Super 120B A12B', 'Nemotron 3 Ultra 550B A55B', 'OpenRouter Free (роутер)', 'Poolside Laguna S 2.1',
          'Poolside Laguna XS 2.1', 'StepFun Step 3.7 Flash']:
    llm(n, 'Kilo Gateway', 'api', 'free', 200, 'requests', 'hour', N, U)

# ---------- OpenCode Zen ----------
U = 'https://opencode.ai/zen'
for n in ['Big Pickle', 'DeepSeek V4 Flash Free', 'MiMo-V2.5 Free', 'Laguna S 2.1 Free', 'Ling-3.0-flash Free',
          'North Mini Code Free', 'Nemotron 3 Ultra Free']:
    llm(n, 'OpenCode Zen', 'api', 'free', 0, 'requests', 'day', 'Бесплатно, лимит не опубликован; данные могут использоваться для улучшения моделей', U)

# ---------- Mistral ----------
U = 'https://console.mistral.ai'
N = 'Бесплатный тариф: лимиты зависят от модели (от 25K токенов/мин); нужно согласие на обучение и подтверждение телефона'
for n in ['GLM 5.3', 'Mistral Medium 3.5', 'Mistral Small 4', 'Mistral Large 4', 'Mistral Large 3', 'Ministral 3 14B',
          'Ministral 3 8B', 'Ministral 3 3B']:
    llm(n, 'Mistral', 'api', 'free', 0, 'tokens', 'day', N, U)
for n in ['Mistral OCR 4.1', 'Mistral OCR 3']:
    add(n, 'Mistral', ['docs', 'sheets'], 'api', 'free', 0, 'tokens', 'day', N + ' (распознавание документов)', U)
add('Voxtral TTS', 'Mistral', ['audio'], 'api', 'free', 0, 'tokens', 'day', N + ' (синтез речи)', U)
for n in ['Voxtral Mini Transcribe 2', 'Voxtral Mini Transcribe Realtime', 'Voxtral Small']:
    add(n, 'Mistral', ['audio', 'video'], 'api', 'free', 0, 'tokens', 'day', N + ' (аудио)', U)
for n in ['Timestral 1.0 209M', 'Timestral 1.0 123M']:
    add(n, 'Mistral', ['finance', 'sheets'], 'api', 'free', 0, 'tokens', 'day', N + ' (прогноз временных рядов)', U)
add('Codestral', 'Mistral Codestral', CODE, 'api', 'free', 2000, 'requests', 'day', '2000 запросов/день, 30 в минуту; нужен телефон', U)

# ---------- NVIDIA Build (NIM): 1000 кредитов при регистрации ----------
U = 'https://build.nvidia.com/models'
N = '1000 кредитов при регистрации (до 5000 по запросу), 40 запросов/мин на модель'
for n in ['nemotron-3.5-super-vl-120b-a12b', 'Step 5 Preview', 'deepseek-v4.1-flash', 'glm-5-3-flash',
          'nemotron-3.5-lightning-30b-a3b', 'muse-glimmer-30b', 'laguna-xs-2.1', 'diffusiongemma-26b-a4b-it',
          'nemotron-3-ultra-550b-a55b', 'nemotron-3-nano-omni-30b-a3b-reasoning', 'gemma-4-31b-it', 'nemotron-3-super-120b-a12b',
          'gpt-oss-20b', 'llama-3.2-11b-vision-instruct', 'llama-3.2-90b-vision-instruct', 'paligemma', 'cosmos3-nano',
          'cosmos3-nano-reasoner', 'nemotron-voicechat']:
    llm(n, 'NVIDIA NIM', 'api', 'free', 1000, 'credits', 'once', N, U)
add('riva-translate-4b-instruct-v2', 'NVIDIA NIM', ['text', 'docs'], 'api', 'free', 1000, 'credits', 'once', N + ' (перевод)', U)
add('nemotron-ocr-v1', 'NVIDIA NIM', ['docs', 'sheets'], 'api', 'free', 1000, 'credits', 'once', N + ' (OCR)', U)
for n in ['Kumo Tabular', 'Kumo Relational']:
    add(n, 'NVIDIA NIM', ['sheets', 'finance'], 'api', 'free', 1000, 'credits', 'once', N + ' (прогнозы по таблицам)', U)
for n in ['magpie-tts-zeroshot']:
    add(n, 'NVIDIA NIM', ['audio'], 'api', 'free', 1000, 'credits', 'once', N + ' (синтез речи)', U)
for n in ['Studio Voice', 'Background Noise Removal']:
    add(n, 'NVIDIA NIM', ['audio', 'video'], 'api', 'free', 1000, 'credits', 'once', N + ' (обработка звука)', U)
for n in ['Video Super Resolution', 'cosmos-transfer2.5-2b']:
    add(n, 'NVIDIA NIM', ['video'], 'api', 'free', 1000, 'credits', 'once', N + ' (видео)', U)

# ---------- GitHub Models (Copilot Free) ----------
U = 'https://github.com/marketplace/models'
for n, rpd in [('DeepSeek-R1', 8), ('DeepSeek-R1-0528', 8), ('MAI-DS-R1', 8), ('Grok 3', 15), ('Grok 3 Mini', 30)]:
    llm(n, 'GitHub Models', 'api', 'free', rpd, 'requests', 'day', f'{rpd} запросов/день по тарифу Copilot Free', U)

# ---------- Scaleway: 1 млн токенов + 60 мин аудио (разово) ----------
U = 'https://www.scaleway.com/en/generative-apis/'
N = 'Разово: 1 млн бесплатных токенов на старте'
for n in ['Gemma 3 27B Instruct', 'Llama 3.3 70B Instruct', 'Pixtral 12B', 'Devstral 2 123B', 'Gemma 4 26B A4B', 'GLM 5.2',
          'gpt-oss-120b', 'Holo2 30B A3B', 'Mistral Medium 3.5', 'Mistral Small 3.2 24B', 'Qwen3 235B A22B',
          'Qwen3 Coder 30B A3B', 'Qwen3.5 397B A17B', 'Qwen3.6 35B A3B']:
    llm(n, 'Scaleway', 'api', 'trial', 1_000_000, 'tokens', 'once', N, U)
for n in ['Whisper Large v3', 'Voxtral Small 24B']:
    add(n, 'Scaleway', ['audio', 'video'], 'api', 'trial', 60, 'minutes', 'once', 'Разово: 60 минут расшифровки аудио', U)

# ---------- Пробные кредиты ----------
U = 'https://cloud.sambanova.ai'
for n in ['DeepSeek V3.1', 'DeepSeek V3.2', 'Gemma 4 31B', 'gpt-oss-120b', 'Llama 3.3 70B Instruct', 'MiniMax M2.7']:
    llm(n, 'SambaNova', 'api', 'trial', 5, 'usd', 'once', 'Разово $5 кредитов на 3 месяца', U)
U = 'https://app.hyperbolic.xyz'
for n in ['DeepSeek V3 0324', 'Llama 3.3 70B Instruct', 'DeepSeek R1 0528', 'Qwen3 Coder 480B A35B']:
    llm(n, 'Hyperbolic', 'api', 'trial', 1, 'usd', 'once', 'Разово $1 кредитов', U)
for n, s in [('Jamba Large', BIG), ('Jamba Mini', GEN)]:
    add(n, 'AI21', s, 'api', 'trial', 10, 'usd', 'once', 'Разово $10 кредитов на 3 месяца', 'https://studio.ai21.com')
for n, s in [('Solar Pro', BIG), ('Solar Mini', GEN)]:
    add(n, 'Upstage', s, 'api', 'trial', 10, 'usd', 'once', 'Разово $10 кредитов на 3 месяца', 'https://console.upstage.ai')
for name, prov, amt, per, note, url in [
    ('Открытые модели (каталог)', 'Fireworks', 1, 'once', 'Разово $1 кредитов', 'https://fireworks.ai/models'),
    ('Любая модель (оплата за GPU)', 'Baseten', 30, 'once', 'Разово $30 кредитов', 'https://www.baseten.co'),
    ('Открытые модели (каталог)', 'Nebius', 1, 'once', 'Разово $1 кредитов', 'https://nebius.com'),
    ('Открытые модели (каталог)', 'Novita', 0.5, 'once', 'Разово $0.5 кредитов на 1 год', 'https://novita.ai'),
    ('Открытые модели (каталог)', 'NLP Cloud', 15, 'once', 'Разово $15 кредитов; нужен телефон', 'https://nlpcloud.com'),
    ('Любая модель (оплата за GPU)', 'Modal', 30, 'month', '$30/месяц на тарифе Starter', 'https://modal.com'),
    ('Открытые модели (каталог)', 'Inference.net', 1, 'once', 'Разово $1 (+$25 за опрос)', 'https://inference.net'),
    ('Inference Providers (роутер)', 'Hugging Face', 0.1, 'month', '$0.10 кредитов в месяц', 'https://huggingface.co/inference-providers'),
    ('AI Gateway (роутер)', 'Vercel', 5, 'month', '$5 в месяц на часть каталога', 'https://vercel.com/ai-gateway')]:
    add(name, prov, GEN + ['code'], 'api', 'trial', amt, 'usd', per, note, url)
U = 'https://www.alibabacloud.com/en/product/modelstudio'
for n in ['Qwen Max', 'Qwen Plus', 'Qwen Flash', 'Qwen Coder']:
    llm(n, 'Alibaba Model Studio', 'api', 'trial', 1_000_000, 'tokens', 'once', '1 млн токенов на каждую модель на 90 дней (Сингапур)', U)

# ---------- Голос и транскрибация (API) ----------
add('ElevenLabs TTS', 'ElevenLabs', ['audio', 'video'], 'api', 'free', 10000, 'credits', 'month', '10 000 кредитов/месяц (≈10 000 символов). Доступ к API на бесплатном тарифе источники описывают по-разному', 'https://costbench.com/software/ai-voice-tools/elevenlabs/')
add('ElevenLabs Scribe', 'ElevenLabs', ['audio', 'video'], 'api', 'free', 10000, 'credits', 'month', 'Из общего лимита 10 000 кредитов/месяц', 'https://costbench.com/software/ai-transcription-apis/elevenlabs-scribe/')
add('Deepgram Nova (STT)', 'Deepgram', ['audio', 'video'], 'api', 'trial', 200, 'usd', 'once', 'Разово $200 кредитов, без карты', 'https://costbench.com/software/ai-transcription-apis/deepgram/')
add('Deepgram Aura (TTS)', 'Deepgram', ['audio'], 'api', 'trial', 200, 'usd', 'once', 'Из тех же $200 разовых кредитов', 'https://costbench.com/software/ai-transcription-apis/deepgram/')
add('AssemblyAI Universal', 'AssemblyAI', ['audio', 'video'], 'api', 'trial', 50, 'usd', 'once', 'Разово $50 (≈185 часов расшифровки)', 'https://costbench.com/software/ai-transcription-apis/assemblyai/')
add('Speechmatics STT', 'Speechmatics', ['audio', 'video'], 'api', 'free', 480, 'minutes', 'month', 'От 480 минут/месяц (источник указывает до 1200–1800)', 'https://costbench.com/software/ai-transcription-apis/speechmatics/')
add('Speechmatics TTS', 'Speechmatics', ['audio'], 'api', 'free', 1_000_000, 'chars', 'month', '1 млн символов/месяц', 'https://costbench.com/software/ai-transcription-apis/speechmatics/')
add('Google Speech-to-Text', 'Google Cloud', ['audio', 'video'], 'api', 'free', 60, 'minutes', 'month', '60 минут/месяц + $300 новым клиентам на 90 дней', 'https://costbench.com/software/ai-transcription-apis/google-speech-to-text/')
add('Amazon Transcribe', 'AWS', ['audio', 'video'], 'api', 'free', 60, 'minutes', 'month', '60 минут/месяц первые 12 месяцев', 'https://costbench.com/software/ai-transcription-apis/aws-transcribe/')
add('Azure Speech TTS', 'Microsoft Azure', ['audio'], 'api', 'free', 500_000, 'chars', 'month', '0,5 млн символов/месяц (тариф F0)', 'https://costbench.com/software/ai-voice-tools/microsoft-speech/')
add('Azure Speech STT', 'Microsoft Azure', ['audio', 'video'], 'api', 'free', 300, 'minutes', 'month', '5 часов/месяц (тариф F0)', 'https://costbench.com/software/ai-voice-tools/microsoft-speech/')
add('Tavus Conversational Video', 'Tavus', ['video', 'marketing'], 'api', 'free', 20, 'minutes', 'month', '20 минут/месяц, API включён', 'https://costbench.com/software/ai-video-generators/tavus/')

# ---------- Сервисы с бесплатным тарифом только на сайте ----------
CB = 'https://costbench.com/software/'
def web(name, skills, limit, unit, period, note, path):
    add(name, name, skills, 'web', 'free', limit, unit, period, note, CB + path)

V, VM = ['video'], ['video', 'marketing']
web('Runway', V, 125, 'credits', 'once', 'Разово 125 кредитов', 'ai-video-generators/runway/')
web('Kling AI', V, 66, 'credits', 'day', '66 кредитов/день, ролики до 5 с с водяным знаком', 'ai-video-generators/kling/')
web('Hailuo AI', V, 3, 'generations', 'day', '≈3–5 видео/день + 200 приветственных кредитов', 'ai-video-generators/hailuo-ai/')
web('PixVerse', V, 30, 'credits', 'day', '30 кредитов/день + 60 на старте, до 540p', 'ai-video-generators/pixverse/')
web('Vidu', V, 40, 'credits', 'month', '40 кредитов/месяц', 'ai-video-generators/vidu/')
web('Pika', V, 80, 'credits', 'month', '80 кредитов/месяц, 480p (источник также упоминает 250)', 'ai-video-generators/pika/')
web('Haiper', V, 10, 'generations', 'day', '10 генераций/день + 300 кредитов разово', 'ai-video-generators/haiper/')
web('Google Veo (Gemini)', V, 5, 'generations', 'day', '≈5–10 роликов/день до 8 с, 720p; в API бесплатного тарифа нет', 'ai-video-generators/google-veo/')
web('Higgsfield AI', V, 2, 'generations', 'month', '≈2 ролика/месяц по 5 с, 720p', 'ai-video-generators/higgsfield/')
web('LTX Video', V, 0, 'credits', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-video-generators/ltx-video/')
web('Wan Video', V, 0, 'credits', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-video-generators/wan-video/')
web('MakeThisVid', V, 1, 'generations', 'week', '1 ролик/неделю (8 с, превью с водяным знаком)', 'ai-video-generators/makethisvid/')
web('HeyGen', VM, 3, 'generations', 'month', '3 видео/месяц до 1 минуты', 'ai-video-generators/heygen/')
web('Synthesia', VM, 10, 'minutes', 'month', '10 минут видео/месяц', 'ai-video-generators/synthesia/')
web('Colossyan', VM, 3, 'minutes', 'month', '≈3 минуты видео/месяц', 'ai-video-generators/colossyan/')
web('D-ID', VM, 3, 'minutes', 'once', 'Пробно: 3 минуты видео на 14 дней', 'ai-video-generators/d-id/')
web('InVideo AI', ['video', 'marketing', 'scripts'], 10, 'generations', 'week', '10 видео/неделю', 'ai-video-generators/invideo-ai/')
web('Descript', ['video', 'audio'], 60, 'minutes', 'month', '1 час медиа/месяц + 100 ИИ-кредитов разово', 'ai-video-generators/descript/')
web('Captions', ['video', 'audio'], 0, 'credits', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-video-generators/captions/')

A = ['audio']
web('LOVO AI', A, 20, 'minutes', 'month', '20 минут озвучки, 10 скачиваний/месяц', 'ai-voice-tools/lovo-ai/')
web('Murf AI', A, 10, 'minutes', 'month', '10 минут озвучки/месяц', 'ai-voice-tools/murf-ai/')
web('Play.ht', A, 12500, 'chars', 'month', '12 500 символов/месяц', 'ai-voice-tools/play-ht/')
web('Speechify', A, 150000, 'chars', 'once', '150 000 символов качественными голосами', 'ai-voice-tools/speechify/')
web('Podcastle', A, 0, 'credits', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-voice-tools/podcastle/')
web('Resemble AI', A, 0, 'credits', 'month', 'Тариф Flex за $0 (оплата по использованию), есть API', 'ai-voice-tools/resemble-ai/')
web('TTSMaker', A, 0, 'chars', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-voice-tools/ttsmaker/')
web('Voice.ai', A, 0, 'credits', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-voice-tools/voice-ai/')
web('WellSaid Labs', A, 3, 'minutes', 'month', 'Пробно: 3 минуты скачивания/месяц', 'ai-voice-tools/wellsaid-labs/')
web('Udio', A, 10, 'credits', 'day', '10 кредитов/день (100/месяц), до 3 песен/день', 'ai-music-generators/udio/')
web('AIVA', A, 3, 'generations', 'month', '3 скачивания/месяц, треки до 3 минут', 'ai-music-generators/aiva/')
web('Boomy', A, 25, 'generations', 'once', '25 сохранённых песен', 'ai-music-generators/boomy/')
web('Mubert', A, 25, 'generations', 'month', '25 треков и 5 MP3/месяц', 'ai-music-generators/mubert/')
web('Loudly', A, 25, 'generations', 'month', '25 песен/месяц по 30 с', 'ai-music-generators/loudly/')
web('Soundful', A, 0, 'generations', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-music-generators/soundful/')
web('Ecrett Music', A, 0, 'generations', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-music-generators/ecrett-music/')

I, ID = ['images', 'design'], ['images', 'design', 'marketing']
web('Leonardo.ai', I, 150, 'credits', 'day', '150 токенов/день', 'ai-image-generators/leonardo-ai/')
web('Recraft', I, 30, 'credits', 'day', '30 кредитов/день (источник также упоминает 50)', 'ai-image-generators/recraft/')
web('Playground AI', I, 10, 'generations', 'hour', 'до 10 изображений каждые 3 часа', 'ai-image-generators/playground-ai/')
web('NightCafe', I, 2, 'credits', 'day', '2 кредита/день + безлимит базовой модели', 'ai-image-generators/nightcafe/')
web('Adobe Firefly', ID, 0, 'generations', 'day', 'Ежедневные бесплатные генерации, точное число не опубликовано', 'ai-image-generators/adobe-firefly/')
web('Bing Image Creator', I, 25, 'generations', 'week', '25 быстрых генераций/неделю, обычные — без ограничений', 'ai-image-generators/bing-image-creator/')
web('Canva AI', ID, 50, 'generations', 'month', '50 использований Magic Studio/месяц', 'ai-image-generators/canva-ai/')
web('Craiyon', I, 2, 'generations', 'day', '2 Pro-генерации/день, Lite — без ограничений', 'ai-image-generators/craiyon/')
web('Imagine Art', I, 100, 'credits', 'day', '100 кредитов/день', 'ai-image-generators/imagine-art/')
web('Grok Aurora', I, 0, 'generations', 'day', 'Бесплатно, «щедрые лимиты» без точных цифр', 'ai-image-generators/grok-aurora/')
web('Mage.space', I, 0, 'generations', 'day', 'Бесплатный тариф есть, лимит не опубликован', 'ai-image-generators/mage-space/')
web('Krea AI', I, 0, 'credits', 'day', 'Бесплатный тариф есть, лимит не опубликован', 'ai-image-generators/krea-ai/')
web('Clipdrop', ['design', 'images'], 20, 'generations', 'day', '20 использований каждой функции/сутки', 'ai-design-tools/clipdrop/')
web('Figma AI', ['design'], 150, 'credits', 'day', '150 ИИ-кредитов/день, не больше 500/месяц', 'ai-design-tools/figma-ai/')
web('Photoroom', ['design', 'marketing'], 250, 'generations', 'month', '250 экспортов/месяц', 'ai-design-tools/photoroom/')
web('Uizard', ['design'], 3, 'generations', 'month', '3 ИИ-генерации/месяц, 2 проекта', 'ai-design-tools/uizard/')
web('Mokker AI', ['design', 'marketing'], 0, 'generations', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-design-tools/mokker-ai/')
web('Flair AI', ['design', 'marketing'], 0, 'generations', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-design-tools/flair-ai/')

P = ['docs', 'design']
web('Gamma', P, 400, 'credits', 'once', 'Разово 400 ИИ-кредитов', 'ai-presentations/gamma/')
web('Tome', P, 500, 'credits', 'once', '500 ИИ-кредитов', 'ai-presentations/tome/')
web('SlidesAI', P, 12, 'generations', 'month', '12 презентаций/год (≈1/месяц), 120 кредитов/год', 'ai-presentations/slidesai/')
web('Decktopus', P, 0, 'generations', 'month', 'Бесплатный тариф с ограниченными ИИ-функциями', 'ai-presentations/decktopus/')
web('MagicSlides', P, 0, 'generations', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-presentations/magicslides/')

W = ['text', 'marketing']
web('Rytr', W, 10000, 'chars', 'month', '10 000 символов/месяц', 'ai-writing-tools/rytr/')
web('QuillBot', ['text', 'docs'], 6, 'requests', 'day', 'Перефраз до 125 слов, «очеловечивание» 6 раз/день, 20 ИИ-чатов', 'ai-writing-tools/quillbot/')
web('Wordtune', ['text', 'docs'], 10, 'requests', 'day', '10 переписываний/день, 3 саммари/месяц', 'ai-writing-tools/wordtune/')
web('HyperWrite', ['text', 'docs'], 15, 'generations', 'month', '15 ИИ-генераций/месяц', 'ai-writing-tools/hyperwrite/')
web('Koala AI', W, 0, 'generations', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-writing-tools/koala-ai/')
web('Narrato', W, 0, 'generations', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-writing-tools/narrato/')
web('Grammarly', ['text', 'docs'], 0, 'requests', 'month', 'Бесплатный тариф есть, лимит ИИ-запросов не опубликован', 'ai-writing-tools/grammarly/')
web('Semrush', ['marketing'], 10, 'requests', 'day', '10 отчётов/день, 1 проект', 'ai-seo-tools/semrush-ai/')
web('Ahrefs AI', ['marketing'], 0, 'requests', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-seo-tools/ahrefs-ai/')
web('MarketMuse', ['marketing'], 0, 'requests', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-seo-tools/marketmuse/')
web('Diib', ['marketing'], 0, 'requests', 'month', 'Бесплатный тариф есть, лимит не опубликован', 'ai-seo-tools/diib/')

R = ['docs']
web('Elicit', R, 2, 'generations', 'month', '2 автоматических отчёта/месяц', 'ai-research-tools/elicit/')
web('Consensus', R, 10, 'requests', 'month', '10 Pro-сообщений и 3 Deep-обзора/месяц', 'ai-research-tools/consensus/')
web('Perplexity', ['text', 'docs'], 5, 'requests', 'day', '≈5–10 Pro-поисков/день (точно не публикуется)', 'ai-chatbots/perplexity/')
web('Notion AI', ['docs', 'sheets'], 20, 'requests', 'once', 'Разово 20 ИИ-ответов', 'ai-productivity/notion-ai/')
for n, path in [('Humata AI', 'ai-research-tools/humata-ai/'), ('Jenni AI', 'ai-research-tools/jenni-ai/'),
                ('Powerdrill AI', 'ai-research-tools/powerdrill-ai/'), ('Explainpaper', 'ai-research-tools/explainpaper/'),
                ('Semantic Scholar', 'ai-research-tools/semantic-scholar/')]:
    web(n, R, 0, 'requests', 'month', 'Бесплатный тариф есть, лимит не опубликован', path)

add('Codex', 'OpenAI Codex', ['code'], 'web', 'free', 0, 'requests', 'hour',
    'Ограниченный доступ на бесплатном ChatGPT (приложение, CLI, IDE через вход ChatGPT); лимит обновляется окнами по 5 часов, число не публикуется. API — платный',
    'https://chatgpt.com/codex')

C = ['text', 'docs', 'scripts', 'marketing', 'code']
for n, path in [('ChatGPT', 'ai-chatbots/chatgpt/'), ('Claude', 'ai-chatbots/claude/'), ('Google Gemini', 'ai-chatbots/gemini/'),
                ('DeepSeek Chat', 'ai-chatbots/deepseek/'), ('Le Chat (Mistral)', 'ai-chatbots/le-chat-mistral/'),
                ('Meta AI', 'ai-chatbots/meta-ai/'), ('HuggingChat', 'ai-chatbots/huggingchat/'), ('Poe', 'ai-chatbots/poe/'),
                ('Pi', 'ai-chatbots/pi/')]:
    web(n, C, 0, 'requests', 'day', 'Бесплатный тариф в приложении; лимит сообщений не опубликован', path)
web('Character.ai', ['text', 'scripts'], 0, 'requests', 'day', 'Бесплатный тариф в приложении; лимит не опубликован', 'ai-chatbots/character-ai/')

# ---------- Как вызывать модели через API (для автоматического каскада бэкенда) ----------
# provider -> (адаптер бэкенда, base_url). Модель попадает в каскад, только если для неё известен API-id.
# Для Cloudflare ключ вводится в виде «ACCOUNT_ID:API_TOKEN» — account подставляется в URL.
PROVIDER_API = {
    'Google AI Studio': ('gemini', ''),
    'Groq': ('openai', 'https://api.groq.com/openai/v1'),
    'OpenRouter': ('openai', 'https://openrouter.ai/api/v1'),
    'Cohere': ('openai', 'https://api.cohere.ai/compatibility/v1'),
    'Cloudflare Workers AI': ('openai', 'https://api.cloudflare.com/client/v4/accounts/{account}/ai/v1'),
    'Cerebras': ('openai', 'https://api.cerebras.ai/v1'),
    'Mistral': ('openai', 'https://api.mistral.ai/v1'),
    'Mistral Codestral': ('openai', 'https://codestral.mistral.ai/v1'),
    'NVIDIA NIM': ('openai', 'https://integrate.api.nvidia.com/v1'),
    'GitHub Models': ('openai', 'https://models.github.ai/inference'),
    'Scaleway': ('openai', 'https://api.scaleway.ai/v1'),
    'SambaNova': ('openai', 'https://api.sambanova.ai/v1'),
    'Hyperbolic': ('openai', 'https://api.hyperbolic.xyz/v1'),
    'AI21': ('openai', 'https://api.ai21.com/studio/v1'),
    'Upstage': ('openai', 'https://api.upstage.ai/v1'),
    'Alibaba Model Studio': ('openai', 'https://dashscope-intl.aliyuncs.com/compatible-mode/v1'),
}

def gemini_id(n):
    n = n.replace(' Instruct', '-it') if n.startswith('Gemma') else n
    return n.lower().replace(' ', '-')

API_IDS = {
    'Google AI Studio': {n: gemini_id(n) for n in ['Gemini 3.6 Flash', 'Gemini 3.5 Flash', 'Gemini 3 Flash', 'Gemini 3.5 Flash-Lite',
        'Gemini 3.1 Flash-Lite', 'Gemini 2.5 Flash', 'Gemini 2.5 Flash-Lite', 'Gemma 4 31B Instruct', 'Gemma 4 26B A4B Instruct',
        'Gemma 3 27B Instruct', 'Gemma 3 12B Instruct', 'Gemma 3 4B Instruct', 'Gemma 3 1B Instruct']},
    'Groq': {'Allam 2 7B': 'allam-2-7b', 'Llama 3.1 8B': 'llama-3.1-8b-instant', 'Llama 3.3 70B': 'llama-3.3-70b-versatile',
             'Groq Compound': 'groq/compound', 'Groq Compound Mini': 'groq/compound-mini', 'gpt-oss-120b': 'openai/gpt-oss-120b',
             'gpt-oss-20b': 'openai/gpt-oss-20b', 'Qwen3.6 27B': 'qwen/qwen3.6-27b'},
    'OpenRouter': {'Gemma 4 26B A4B': 'google/gemma-4-26b-a4b-it:free', 'Gemma 4 31B': 'google/gemma-4-31b-it:free',
                   'Nemotron 3 Nano 30B A3B': 'nvidia/nemotron-3-nano-30b-a3b:free', 'Nemotron Nano 12B v2 VL': 'nvidia/nemotron-nano-12b-v2-vl:free',
                   'Nemotron Nano 9B v2': 'nvidia/nemotron-nano-9b-v2:free', 'gpt-oss-20b': 'openai/gpt-oss-20b:free',
                   'Nemotron 3.5 Super VL': 'nvidia/nemotron-3.5-super-vl:free', 'Xing4.0 29B A4B': 'xingchen-agi/xing4.0-29b-a4b:free'},
    'Cohere': {'Command A': 'command-a-03-2025', 'Command A Plus': 'command-a-plus-05-2026', 'Command A Reasoning': 'command-a-reasoning-08-2025',
               'Command A Translate': 'command-a-translate-08-2025', 'Command A Vision': 'command-a-vision-07-2025',
               'Command R': 'command-r-08-2024', 'Command R+': 'command-r-plus-08-2024', 'Command R7B': 'command-r7b-12-2024',
               'Command R7B Arabic': 'command-r7b-arabic-02-2025', 'Aya Expanse 32B': 'c4ai-aya-expanse-32b', 'Aya Vision 32B': 'c4ai-aya-vision-32b'},
    'Cloudflare Workers AI': {k: '@cf/' + v for k, v in {
        'deepseek-r1-distill-qwen-32b': 'deepseek-ai/deepseek-r1-distill-qwen-32b', 'gemma-4-26b-a4b-it': 'google/gemma-4-26b-a4b-it',
        'gemma-sea-lion-v4-27b-it': 'aisingapore/gemma-sea-lion-v4-27b-it', 'glm-4.7-flash': 'zai-org/glm-4.7-flash', 'glm-5.2': 'zai-org/glm-5.2',
        'gpt-oss-120b': 'openai/gpt-oss-120b', 'gpt-oss-20b': 'openai/gpt-oss-20b', 'granite-4.0-h-micro': 'ibm-granite/granite-4.0-h-micro',
        'kimi-k2.6': 'moonshotai/kimi-k2.6', 'kimi-k2.7-code': 'moonshotai/kimi-k2.7-code', 'llama-3.1-8b-instruct-fp8': 'meta/llama-3.1-8b-instruct-fp8',
        'llama-3.2-11b-vision-instruct': 'meta/llama-3.2-11b-vision-instruct', 'llama-3.2-1b-instruct': 'meta/llama-3.2-1b-instruct',
        'llama-3.2-3b-instruct': 'meta/llama-3.2-3b-instruct', 'llama-3.3-70b-instruct-fp8-fast': 'meta/llama-3.3-70b-instruct-fp8-fast',
        'llama-4-scout-17b-16e-instruct': 'meta/llama-4-scout-17b-16e-instruct', 'mistral-small-3.1-24b-instruct': 'mistralai/mistral-small-3.1-24b-instruct',
        'nemotron-3-120b-a12b': 'nvidia/nemotron-3-120b-a12b', 'qwen2.5-coder-32b-instruct': 'qwen/qwen2.5-coder-32b-instruct',
        'qwen3-30b-a3b-fp8': 'qwen/qwen3-30b-a3b-fp8', 'qwq-32b': 'qwen/qwq-32b'}.items()},
    'Cerebras': {'gpt-oss-120b': 'gpt-oss-120b', 'GLM 4.7': 'zai-glm-4.7', 'Gemma 4 31B': 'gemma-4-31b'},
    'Mistral': {'Mistral Medium 3.5': 'mistral-medium-latest', 'Mistral Small 4': 'mistral-small-latest', 'Mistral Large 4': 'mistral-large-latest'},
    'Mistral Codestral': {'Codestral': 'codestral-latest'},
    'NVIDIA NIM': {'nemotron-3.5-super-vl-120b-a12b': 'nvidia/nemotron-3.5-super-vl-120b-a12b', 'deepseek-v4.1-flash': 'deepseek-ai/deepseek-v4.1-flash',
                   'nemotron-3.5-lightning-30b-a3b': 'nvidia/nemotron-3.5-lightning-30b-a3b', 'muse-glimmer-30b': 'meta/muse-glimmer-30b',
                   'laguna-xs-2.1': 'poolside/laguna-xs-2.1', 'nemotron-3-ultra-550b-a55b': 'nvidia/nemotron-3-ultra-550b-a55b',
                   'nemotron-3-nano-omni-30b-a3b-reasoning': 'nvidia/nemotron-3-nano-omni-30b-a3b-reasoning', 'gemma-4-31b-it': 'google/gemma-4-31b-it',
                   'nemotron-3-super-120b-a12b': 'nvidia/nemotron-3-super-120b-a12b', 'gpt-oss-20b': 'openai/gpt-oss-20b',
                   'llama-3.2-11b-vision-instruct': 'meta/llama-3.2-11b-vision-instruct', 'llama-3.2-90b-vision-instruct': 'meta/llama-3.2-90b-vision-instruct'},
    'GitHub Models': {'DeepSeek-R1': 'deepseek/deepseek-r1', 'DeepSeek-R1-0528': 'deepseek/deepseek-r1-0528', 'MAI-DS-R1': 'microsoft/mai-ds-r1',
                      'Grok 3': 'xai/grok-3', 'Grok 3 Mini': 'xai/grok-3-mini'},
    'Scaleway': {'Gemma 3 27B Instruct': 'gemma-3-27b-it', 'Llama 3.3 70B Instruct': 'llama-3.3-70b-instruct', 'Pixtral 12B': 'pixtral-12b-2409',
                 'Devstral 2 123B': 'devstral-2-123b-instruct-2512', 'Gemma 4 26B A4B': 'gemma-4-26b-a4b-it', 'GLM 5.2': 'glm-5.2',
                 'gpt-oss-120b': 'gpt-oss-120b', 'Holo2 30B A3B': 'holo2-30b-a3b', 'Mistral Medium 3.5': 'mistral-medium-3.5-128b',
                 'Mistral Small 3.2 24B': 'mistral-small-3.2-24b-instruct-2506', 'Qwen3 235B A22B': 'qwen3-235b-a22b-instruct-2507',
                 'Qwen3 Coder 30B A3B': 'qwen3-coder-30b-a3b-instruct', 'Qwen3.5 397B A17B': 'qwen3.5-397b-a17b', 'Qwen3.6 35B A3B': 'qwen3.6-35b-a3b'},
    'SambaNova': {'DeepSeek V3.1': 'DeepSeek-V3.1', 'DeepSeek V3.2': 'DeepSeek-V3.2', 'Gemma 4 31B': 'gemma-4-31b-it', 'gpt-oss-120b': 'gpt-oss-120b',
                  'Llama 3.3 70B Instruct': 'Meta-Llama-3.3-70B-Instruct', 'MiniMax M2.7': 'MiniMax-M2.7'},
    'Hyperbolic': {'DeepSeek V3 0324': 'deepseek-ai/DeepSeek-V3-0324', 'Llama 3.3 70B Instruct': 'meta-llama/Llama-3.3-70B-Instruct',
                   'DeepSeek R1 0528': 'deepseek-ai/DeepSeek-R1-0528', 'Qwen3 Coder 480B A35B': 'Qwen/Qwen3-Coder-480B-A35B-Instruct'},
    'AI21': {'Jamba Large': 'jamba-large', 'Jamba Mini': 'jamba-mini'},
    'Upstage': {'Solar Pro': 'solar-pro', 'Solar Mini': 'solar-mini'},
    'Alibaba Model Studio': {'Qwen Max': 'qwen-max', 'Qwen Plus': 'qwen-plus', 'Qwen Flash': 'qwen-flash', 'Qwen Coder': 'qwen3-coder-plus'},
}

# Дополнительные локальные лимиты (запросов в минуту), известные из источников
RPM = {('Google AI Studio', n): v for n, v in [('Gemini 3.6 Flash', 5), ('Gemini 3.5 Flash', 5), ('Gemini 3 Flash', 5),
       ('Gemini 3.5 Flash-Lite', 15), ('Gemini 3.1 Flash-Lite', 15), ('Gemini 2.5 Flash', 5), ('Gemini 2.5 Flash-Lite', 10)]}
RPM.update({('Google AI Studio', n): 30 for n in ['Gemma 4 31B Instruct', 'Gemma 4 26B A4B Instruct', 'Gemma 3 27B Instruct',
            'Gemma 3 12B Instruct', 'Gemma 3 4B Instruct', 'Gemma 3 1B Instruct']})
RPM.update({('OpenRouter', n): 20 for n in API_IDS['OpenRouter']})
RPM.update({('Google AI Studio', 'Gemini 2.5 Flash TTS'): 3, ('Google AI Studio', 'Gemini 3.1 Flash TTS'): 3})
RPM.update({('Cohere', n): 20 for n in API_IDS['Cohere']})
RPM.update({('Cerebras', n): 5 for n in API_IDS['Cerebras']})
RPM.update({('Mistral Codestral', 'Codestral'): 30, **{('NVIDIA NIM', n): 40 for n in API_IDS['NVIDIA NIM']}})

def quality_rank(name, skills):
    l = name.lower()
    if any(k in l for k in ['1b', '3b', '4b', '7b', '8b', '9b', 'micro', 'mini', 'nano', 'lite', 'small', 'flash-lite']): return 3
    return 1 if 'finance' in skills else 2

# Медиа-модели, которые умеет вызывать роутер: имя в каталоге → (что генерирует, id в API, приоритет качества)
MEDIA_IDS = {
    'Cloudflare Workers AI': {
        'flux-1-schnell': ('image', '@cf/black-forest-labs/flux-1-schnell', 1),
        'lucid-origin': ('image', '@cf/leonardo/lucid-origin', 2),
        'phoenix-1.0': ('image', '@cf/leonardo/phoenix-1.0', 2),
        'stable-diffusion-xl-base-1.0': ('image', '@cf/stabilityai/stable-diffusion-xl-base-1.0', 3),
        'stable-diffusion-xl-lightning': ('image', '@cf/bytedance/stable-diffusion-xl-lightning', 3),
        'dreamshaper-8-lcm': ('image', '@cf/lykon/dreamshaper-8-lcm', 3),
        'aura-2-en': ('audio', '@cf/deepgram/aura-2-en', 2),
        'aura-2-es': ('audio', '@cf/deepgram/aura-2-es', 3),
        'aura-1': ('audio', '@cf/deepgram/aura-1', 3),
        'melotts': ('audio', '@cf/myshell-ai/melotts', 3),
    },
    'Google AI Studio': {
        'Gemini 3.1 Flash TTS': ('audio', 'gemini-3.1-flash-tts-preview', 1),
        'Gemini 2.5 Flash TTS': ('audio', 'gemini-2.5-flash-preview-tts', 2),
    },
}
MEDIA_API = {'Cloudflare Workers AI': ('cloudflare', 'https://api.cloudflare.com/client/v4/accounts/{account}/ai/run'),
             'Google AI Studio': ('gemini', '')}

def attach_api(e):
    ids = API_IDS.get(e['provider'], {})
    chat = e['access'] == 'api' and any(s in e['skills'] for s in ('text', 'code', 'docs'))
    media = MEDIA_IDS.get(e['provider'], {}).get(e['name']) if e['access'] == 'api' else None
    e['kind'], e['api_model'] = ('chat', ids.get(e['name'])) if chat and ids.get(e['name']) else (media[0], media[1]) if media else ('chat', None)
    e['media_prio'] = media[2] if media else None
    e['routable'] = bool(e['api_model'])

for e in E:
    attach_api(e)

def backend_entry(e):
    adapter, base = (MEDIA_API if e['kind'] != 'chat' else PROVIDER_API)[e['provider']]
    lim = {}
    if e['limit'] and e['unit'] == 'requests' and e['period'] == 'day': lim['rpd'] = e['limit']
    if e['limit'] and e['unit'] == 'requests' and e['period'] == 'month': lim['rpmo'] = e['limit']
    if e['limit'] and e['unit'] == 'tokens' and e['period'] == 'day': lim['tpd'] = e['limit']
    if (e['provider'], e['name']) in RPM: lim['rpm'] = RPM[(e['provider'], e['name'])]
    return {'id': e['id'], 'label': e['name'], 'provider': adapter, 'base_url': base, 'model': e['api_model'],
            'key_name': e['provider'], 'tier': e['tier'], 'skills': e['skills'], 'limits': lim, 'kind': e['kind'],
            'priority': e['media_prio'] or quality_rank(e['name'], e['skills'])}

# ---------- проверка и вывод ----------
SK = {'text', 'code', 'design', 'video', 'scripts', 'docs', 'sheets', 'finance', 'marketing', 'images', 'audio'}
seen = set()
for e in E:
    assert set(e['skills']) <= SK, e
    slug = re.sub(r'[^a-z0-9]+', '-', (e['provider'] + '-' + e['name'].replace('+', ' plus')).lower()).strip('-')
    assert slug not in seen, slug
    seen.add(slug)
    e['id'] = 'cat-' + slug

if __name__ == '__main__':
    from collections import Counter
    print('total', len(E), Counter(e['access'] for e in E), Counter(e['tier'] for e in E), file=sys.stderr)
    c = Counter(s for e in E for s in e['skills']); print(dict(c), file=sys.stderr)
    print(len({e['provider'] for e in E}), 'providers', file=sys.stderr)
    if '--backend-json' in sys.argv:
        rows = [backend_entry(e) for e in E if e['routable']]
        print(len(rows), 'моделей для автокаскада бэкенда', file=sys.stderr)
        sys.stdout.write(json.dumps({'generated': '2026-10-19', 'models': rows}, ensure_ascii=False, indent=1) + '\n')
        sys.exit(0)
    keys = ['id', 'name', 'provider', 'skills', 'access', 'tier', 'limit', 'unit', 'period', 'note', 'url', 'routable']
    rows = ',\n'.join('  ' + json.dumps([e[k] for k in keys], ensure_ascii=False) for e in E)
    out = f'''// Автоматически сгенерировано (scripts/gen_catalog.py) — не редактируйте вручную.
// Бэкенд получает тот же каталог: python scripts/gen_catalog.py --backend-json > ../backend/catalog.json
// Каталог моделей и сервисов с бесплатным лимитом или стартовыми кредитами.
// Данные собраны 19.10.2026. Лимиты бесплатных тарифов часто меняются — проверяйте по ссылке.
// Источники: github.com/cheahjs/free-llm-api-resources, developers.cloudflare.com/workers-ai/models,
// build.nvidia.com/models, docs.mistral.ai, docs.github.com/github-models, costbench.com.
import {{ LimitPeriod, LimitUnit, SkillId }} from './types';

export const CATALOG_DATE = '2026-10-19';

export interface CatalogEntry {{
  id: string;
  name: string;
  provider: string;
  skills: SkillId[];
  /** api — роутер может вызывать модель; web — бесплатный лимит есть только на сайте сервиса. */
  access: 'api' | 'web';
  /** free — постоянный бесплатный лимит; trial — разовые/пробные кредиты. */
  tier: 'free' | 'trial';
  /** 0 — лимит не опубликован. */
  limit: number;
  unit: LimitUnit;
  period: LimitPeriod;
  note: string;
  url: string;
  /** Чат-модель с известным API: при наличии ключа автоматически попадает в каскад бэкенда. */
  routable: boolean;
}}

type Row = [string, string, string, SkillId[], 'api' | 'web', 'free' | 'trial', number, LimitUnit, LimitPeriod, string, string, boolean];

const ROWS: Row[] = [
{rows},
];

export const CATALOG: CatalogEntry[] = ROWS.map(([id, name, provider, skills, access, tier, limit, unit, period, note, url, routable]) => ({{
  id, name, provider, skills, access, tier, limit, unit, period, note, url, routable,
}}));
'''
    sys.stdout.write(out)
