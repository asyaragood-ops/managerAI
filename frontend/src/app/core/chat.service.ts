import { Injectable, computed, effect, inject, signal } from '@angular/core';
import { AiRouterService } from './ai-router.service';
import { AuthService } from './auth.service';
import { BackendService } from './backend.service';
import { ModelsService } from './models.service';
import { SettingsService } from './settings.service';
import { Chat, ChatMessage, SkillId, skillLabel, MediaKind } from './types';
import { loadJson, saveJson, uid, usagePct } from './utils';

const KEY = 'iai.chats.v1';
const MIN = 60_000;

function seedChats(modelIdByName: (n: string) => string | null): Chat[] {
  const now = Date.now();
  const msg = (role: ChatMessage['role'], text: string, ago: number, model?: string, from?: string): ChatMessage => ({
    id: uid(), role, text, at: now - ago,
    modelId: model ? modelIdByName(model) ?? undefined : undefined,
    fromModelId: from ? modelIdByName(from) ?? undefined : undefined,
  });
  const chat = (title: string, skill: SkillId | null, model: string, ago: number, messages: ChatMessage[]): Chat => ({
    id: uid(), title, skill, modelId: modelIdByName(model), mode: 'auto', messages, updatedAt: now - ago,
  });
  return [
    chat('Сценарий рекламного ролика', 'scripts', 'Command A', 2 * MIN, [
      msg('user', 'Напиши сценарий 30-секундного ролика для кофейни «Зерно». Тон — тёплый, утренний, без пафоса.', 9 * MIN),
      msg('assistant', '**Кадр 1 (0–5 с).** Рассвет за окном, пар над чашкой. Голос за кадром: «Утро начинается не с будильника…»\n\n**Кадр 2 (5–14 с).** Руки бариста, звук помола крупным планом…', 8 * MIN, 'Gemini 2.5 Flash'),
      msg('switch', '', 5 * MIN, 'Command A', 'Gemini 2.5 Flash'),
      msg('user', 'Сделай финал короче и добавь слоган.', 5 * MIN),
      msg('assistant', '**Кадр 4 (24–30 с).** Девушка выходит на улицу с чашкой, улыбается солнцу. Логотип «Зерно».\n\n> Слоган: «Зерно. Утро, которое хочется продлить».', 4 * MIN, 'Command A'),
    ]),
    chat('Квартальный отчёт Q3', 'docs', 'Gemini 2.5 Flash', 62 * MIN, [
      msg('user', 'Собери структуру квартального отчёта: выручка, расходы, ключевые метрики.', 64 * MIN),
      msg('assistant', 'Предлагаю 5 разделов: 1) резюме, 2) выручка по каналам, 3) расходы, 4) метрики роста, 5) планы на Q4.', 62 * MIN, 'Gemini 2.5 Flash'),
    ]),
    chat('Рефакторинг API на TS', 'code', 'Codestral', 190 * MIN, [
      msg('user', 'Как лучше разделить сервис запросов на слои?', 192 * MIN),
      msg('assistant', 'Выделите транспорт (HTTP-клиент), слой репозиториев и доменные сервисы — так их проще тестировать.', 190 * MIN, 'Codestral'),
    ]),
  ];
}

@Injectable({ providedIn: 'root' })
export class ChatService {
  private readonly models = inject(ModelsService);
  private readonly router = inject(AiRouterService);
  private readonly settings = inject(SettingsService);

  readonly chats = signal<Chat[]>(this.load());
  readonly activeId = signal<string | null>(this.chats()[0]?.id ?? null);
  readonly typing = signal(false);
  /** Когда ушёл текущий запрос — чтобы подсказать, что картинка или видео генерируются дольше текста. */
  readonly typingSince = signal(0);

  readonly sorted = computed(() => [...this.chats()].sort((a, b) => b.updatedAt - a.updatedAt));
  readonly active = computed(() => this.chats().find((c) => c.id === this.activeId()) ?? null);

  constructor() {
    effect(() => saveJson(KEY, this.chats()));
  }

  private load(): Chat[] {
    const saved = loadJson<Chat[]>(KEY, []);
    if (saved.length) return saved;
    const byName = (n: string) => this.models.models().find((m) => m.name === n)?.id ?? null;
    return seedChats(byName);
  }

  create(): Chat {
    const c: Chat = { id: uid(), title: 'Новый чат', skill: null, modelId: null, mode: 'auto', messages: [], updatedAt: Date.now() };
    this.chats.update((l) => [c, ...l]);
    this.activeId.set(c.id);
    return c;
  }

  remove(id: string): void {
    this.chats.update((l) => l.filter((c) => c.id !== id));
    if (this.activeId() === id) this.activeId.set(this.sorted()[0]?.id ?? null);
  }

  patch(id: string, p: Partial<Chat>): void {
    this.chats.update((l) => l.map((c) => (c.id === id ? { ...c, ...p } : c)));
  }

  private push(id: string, m: ChatMessage, extra: Partial<Chat> = {}): void {
    this.chats.update((l) =>
      l.map((c) => (c.id === id ? { ...c, ...extra, messages: [...c.messages, m], updatedAt: m.at } : c)),
    );
  }

  /** null — ещё не знаем, true — бэкенд отвечает, false — работаем в демо-режиме. */
  readonly backendOnline = inject(BackendService).online;
  private readonly auth = inject(AuthService);

  /**
   * Отправка сообщения: POST /api/chat на бэкенд-роутер (каскад моделей на C).
   * Если бэкенд не запущен — ответ в демо-режиме, чтобы интерфейс оставался рабочим.
   */
  send(text: string): void {
    const body = text.trim();
    if (!body || this.typing()) return;
    const chat = this.active() ?? this.create();
    const title = chat.messages.length === 0 ? body.slice(0, 48) : chat.title;
    this.push(chat.id, { id: uid(), role: 'user', text: body, at: Date.now() }, { title });
    this.typing.set(true);
    this.typingSince.set(Date.now());

    const history = (this.chats().find((c) => c.id === chat.id)?.messages ?? [])
      .filter((m) => (m.role === 'user' || (m.role === 'assistant' && !m.demo)) && (m.text.trim() || m.media))
      .slice(-40)
      // медиа-ответ модели видят как подпись — так «а теперь сделай его синим» понятно без самой картинки
      .map((m) => ({ role: m.role, content: m.media ? `[${m.media.kind}: ${m.media.prompt ?? ''}]` : m.text }));

    fetch('/api/chat', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ messages: history, model: chat.mode, skill: chat.skill }),
    })
      .then(async (res) => {
        if (res.status === 401) {
          this.auth.onUnauthorized(); // сессия истекла — на страницу входа
          return;
        }
        const data = (await res.json().catch(() => null)) as BackendReply | null;
        // Ответ не от нашего бэкенда (например, прокси dev-сервера вернул 504) — считаем, что бэкенд не запущен
        if (!data || typeof data.ok !== 'boolean') throw new Error('backend offline');
        this.backendOnline.set(true);
        this.applyBackendReply(chat.id, data);
      })
      .catch(() => {
        this.backendOnline.set(false);
        this.demoReply(chat);
      })
      .finally(() => this.typing.set(false));
  }

  private applyBackendReply(chatId: string, data: BackendReply): void {
    const s = this.settings.settings();
    const failed = data.attempts.filter((a) => a.status !== 'ok' && a.status !== 'no_api_key' && a.status !== 'disabled');
    if (data.ok && data.model) {
      const from = failed.at(-1);
      if (data.switched && from && s.notifySwitch) {
        this.push(chatId, {
          id: uid(), role: 'switch', text: '', at: Date.now(),
          fromLabel: from.model.label, modelLabel: data.model.label, limit: from.limit,
          fromModelId: this.models.get(from.model.id)?.id, modelId: this.models.get(data.model.id)?.id,
        });
      }
      const local = this.models.get(data.model.id); // если id из конфига совпадает с каталогом — обновим карточку
      if (local) {
        const used = data.usage.input_tokens + data.usage.output_tokens;
        this.models.consume(local.id, local.unit === 'tokens' ? used : 1);
      }
      this.models.countRequest(data.switched);
      this.push(
        chatId,
        {
          id: uid(), role: 'assistant', at: Date.now(), text: data.text ?? '',
          modelId: local?.id, modelLabel: data.model.label, badge: data.badge,
          media: data.media && data.kind !== 'chat'
            ? { kind: data.kind, url: data.media.url, mime: data.media.mime, prompt: data.prompt, promptBy: data.prompt_model ?? null }
            : undefined,
        },
        { modelId: local?.id ?? null },
      );
      return;
    }
    const lines = data.attempts.map((a) => `• **${a.model.label}** — ${STATUS_TEXT[a.status] ?? a.status}${a.http_status ? ` (HTTP ${a.http_status})` : ''}`);
    this.push(chatId, {
      id: uid(), role: 'assistant', at: Date.now(),
      text: `Не удалось получить ответ: ${data.error ?? 'все ИИ недоступны'}.` + (lines.length ? `\n\n${lines.join('\n')}` : '') +
        '\n\nПроверьте API-ключи в **Настройках** и порядок моделей в **backend/config.json**.',
    });
  }

  /** Демо-ответ без бэкенда: модель выбирает клиентская копия роутера. */
  private demoReply(chat: Chat): void {
    const s = this.settings.settings();
    const { model, switchedFrom } = this.router.resolve(chat.mode, chat.skill, chat.modelId);
    if (switchedFrom && model && s.notifySwitch) {
      this.push(chat.id, { id: uid(), role: 'switch', text: '', at: Date.now(), modelId: model.id, fromModelId: switchedFrom.id, limit: true });
    }
    if (!model) {
      this.push(chat.id, {
        id: uid(), role: 'assistant', at: Date.now(), demo: true,
        text: 'Бэкенд не запущен, а в демо-режиме нет доступных моделей. Запустите **backend** (см. backend/README.md) или добавьте ключ в **Настройках**.',
      });
      return;
    }
    this.models.consume(model.id, Math.max(1, Math.round(model.limit * (0.03 + Math.random() * 0.05))));
    this.models.countRequest(!!switchedFrom);
    const skill = chat.skill ? ` (навык: ${skillLabel(chat.skill)})` : '';
    this.push(
      chat.id,
      {
        id: uid(), role: 'assistant', at: Date.now(), modelId: model.id, demo: true, badge: `Демо: ${model.name}`,
        text: `Это демо-ответ: бэкенд-роутер не запущен.\n\nЗапрос получила бы модель **${model.name}**${skill}. Сейчас у неё использовано ${usagePct(this.models.get(model.id) ?? model)}% лимита.`,
      },
      { modelId: model.id },
    );
  }
}

/** Ответ POST /api/chat (см. backend/src/router.c → router_reply_json). */
interface BackendModel {
  id: string;
  label: string;
  provider: string;
  model: string;
  tier: string;
}

interface BackendReply {
  ok: boolean;
  request_id: string;
  /** chat — текстовый ответ; image/audio/video — файл в media */
  kind: 'chat' | MediaKind;
  text?: string;
  media?: { url: string; mime: string; bytes: number };
  /** Промпт для медиа-модели и текстовая модель, которая его подготовила. */
  prompt?: string;
  prompt_model?: string | null;
  badge?: string;
  error?: string;
  model?: BackendModel;
  switched: boolean;
  usage: { input_tokens: number; output_tokens: number };
  attempts: { model: BackendModel; status: string; limit: boolean; http_status: number; latency_ms: number; message: string }[];
  latency_ms: number;
}

const STATUS_TEXT: Record<string, string> = {
  rate_limit: 'превышен лимит запросов',
  quota: 'исчерпана квота',
  local_limit: 'исчерпан локальный лимит',
  cooldown: 'на паузе после ошибок',
  auth: 'неверный API-ключ',
  bad_request: 'запрос отклонён',
  server_error: 'ошибка сервера провайдера',
  network: 'нет связи',
  bad_response: 'пустой ответ',
  no_api_key: 'не задан API-ключ',
  disabled: 'выключена',
};
