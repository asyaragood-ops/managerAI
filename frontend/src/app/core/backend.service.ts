import { Injectable, effect, inject, signal } from '@angular/core';
import { AuthService } from './auth.service';
import { ModelsService } from './models.service';
import { SettingsService } from './settings.service';
import { RouterSettings } from './types';
import { loadJson, saveJson } from './utils';

/** Настройки сохранены, пока бэкенд был выключен, — отправить при подключении. */
const PENDING_KEY = 'iai.settingsPending.v1';

/** Настройки роутера на сервере: те же поля + id выключенных моделей. */
export type ServerSettings = RouterSettings & { disabled: string[] };

/** Модель из каскада бэкенда (GET /api/models). */
export interface ServerModel {
  id: string;
  label: string;
  auto: boolean;
  enabled: boolean;
  configured: boolean;
  key_name: string;
}

/** Состояние ключа провайдера на бэкенде (сам ключ сервер никогда не возвращает). */
export interface ServerKey {
  provider: string;
  set: boolean;
  /** settings — сохранён из «Настроек» сайта, env — взят из backend/.env */
  source: 'settings' | 'env' | null;
  /** «…a1B2» */
  hint: string | null;
  /** Модели каскада бэкенда, которые используют этот ключ. */
  models: string[];
}

/** Статистика пользователя (GET /api/stats). */
export interface StatsDay {
  day: string;
  visited: boolean;
  requests: number;
  tokens_in: number;
  tokens_out: number;
  media: number;
  switches: number;
}

export interface StatsModel {
  id: string;
  label: string;
  provider: string;
  kind: string;
  rpd: number;
  req_day: number;
  tpd: number;
  tokens_day: number;
  rpmo: number;
  req_month: number;
  cooldown_until: number;
}

export interface StatsReply {
  ok: boolean;
  today: string;
  tz: number;
  days: StatsDay[];
  /** Личный дневной лимит токенов (0 — без лимита) и расход за сегодня. */
  daily_limit: number;
  used_today: number;
  /** Когда сбрасываются суточные лимиты провайдеров (unix, полночь UTC). */
  limits_reset_at: number;
  models: StatsModel[];
  /** Остатки на сегодня по лимитам провайдеров: null — у подключённых моделей такого лимита нет. */
  left: { tokens: number | null; requests: number | null; token_models: number; request_models: number };
}

/**
 * Связь с бэкендом-роутером на C: доступность, API-ключи, настройки и статистика текущего пользователя.
 * Ключи хранятся только на сервере (у каждого пользователя свои); в браузер приходит лишь подсказка «…a1B2».
 */
@Injectable({ providedIn: 'root' })
export class BackendService {
  private readonly models = inject(ModelsService);
  private readonly settings = inject(SettingsService);
  private readonly auth = inject(AuthService);

  /** null — ещё не проверяли, true — бэкенд отвечает, false — недоступен (демо-режим). */
  readonly online = signal<boolean | null>(null);
  readonly keys = signal<Record<string, ServerKey>>({});
  /** Модели, которые сейчас участвуют в каскаде бэкенда (с ключом и не выключены). */
  readonly cascadeIds = signal<Set<string>>(new Set());
  /** Можно ли включить шифрование ключей (у сервера есть мастер-ключ). null — неизвестно. */
  readonly encryptionAvailable = signal<boolean | null>(null);

  /** Настройки и список выключенных моделей уже сверены с текущим запуском бэкенда. */
  private synced = false;

  constructor() {
    void this.connect();
    // Пока бэкенд не найден — проверяем раз в 15 секунд и сверяем настройки, когда он появится.
    setInterval(() => {
      if (!this.synced || this.online() !== true) void this.connect();
    }, 15_000);
    // Выключили/включили модель на странице «Модели» — сразу сообщаем роутеру.
    effect(() => {
      const disabled = this.disabledIds();
      if (this.synced && this.online() === true) void this.pushDisabled(disabled);
    });
  }

  /** id выключенных моделей каталога (совпадают с id в backend/catalog.json). */
  private disabledIds(): string[] {
    return this.models.models().filter((m) => !m.enabled).map((m) => m.catalogId ?? m.id).sort();
  }

  private lastDisabled = '';

  private async pushDisabled(disabled: string[]): Promise<void> {
    const sig = JSON.stringify(disabled);
    if (sig === this.lastDisabled) return;
    this.lastDisabled = sig;
    const res = await this.post<{ ok: boolean }>('/api/settings', { disabled });
    if (res?.ok) void this.refreshModels();
    else this.lastDisabled = '';
  }

  /**
   * Сохранить настройки роутера на сервере. Сервер — источник истины: при ошибке (например, шифрование
   * недоступно) настройки не применяются. Если бэкенд не запущен — сохраняем в браузере и досылаем позже.
   */
  async saveSettings(next: RouterSettings): Promise<{ saved: boolean; error?: string }> {
    const res = await this.post<{ ok: boolean; error?: string; settings?: ServerSettings; encryption_available?: boolean }>(
      '/api/settings', { ...next, disabled: this.disabledIds() },
    );
    if (res === null) {
      if (next.encryptKeys && !this.settings.settings().encryptKeys) {
        return { saved: false, error: 'шифрование ключей выполняется на сервере — сначала запустите бэкенд' };
      }
      this.online.set(false);
      this.settings.save(next);
      saveJson(PENDING_KEY, true);
      return { saved: false };
    }
    if (!res.ok || !res.settings) return { saved: false, error: res.error ?? 'сервер отклонил настройки' };
    this.applyServer(res.settings, res.encryption_available);
    saveJson(PENDING_KEY, false);
    return { saved: true };
  }

  /** Синхронизация настроек при подключении: несохранённые на сервере — отправить, иначе взять с сервера. */
  private async syncSettings(): Promise<boolean> {
    if (loadJson<boolean>(PENDING_KEY, false)) {
      const r = await this.saveSettings(this.settings.settings());
      if (r.error) saveJson(PENDING_KEY, false); // сервер не принял (например, нет шифрования) — берём его вариант
      else return r.saved;
    }
    const data = await this.request<{ ok: boolean; settings: ServerSettings; encryption_available: boolean }>('/api/settings');
    if (!data?.ok) return false;
    this.applyServer(data.settings, data.encryption_available);
    // Список выключенных моделей ведёт страница «Модели» в браузере.
    await this.pushDisabled(this.disabledIds());
    return true;
  }

  private applyServer(s: ServerSettings, encryption?: boolean): void {
    const { disabled, ...rest } = s;
    this.lastDisabled = JSON.stringify([...disabled].sort());
    this.settings.save({ ...this.settings.settings(), ...rest });
    if (encryption !== undefined) this.encryptionAvailable.set(encryption);
  }

  /** Какие модели сейчас в каскаде бэкенда — для отметки «В каскаде» на странице «Модели». */
  async refreshModels(): Promise<void> {
    const data = await this.request<{ ok: boolean; models: ServerModel[] }>('/api/models');
    if (data?.ok) this.cascadeIds.set(new Set(data.models.filter((m) => m.enabled && m.configured).map((m) => m.id)));
  }

  /** Проверить бэкенд, загрузить состояние ключей и сверить настройки. */
  async connect(): Promise<void> {
    if (!(await this.refresh())) {
      this.synced = false; // бэкенд перезапустят — сверим настройки снова
      return;
    }
    if (this.synced) {
      void this.refreshModels();
      return;
    }
    if (!(await this.syncSettings())) return;
    await this.refreshModels();
    this.synced = true;
  }

  /** Загрузить состояние ключей с сервера. true — бэкенд доступен. */
  async refresh(): Promise<boolean> {
    const data = await this.request<{ ok: boolean; keys: ServerKey[] }>('/api/keys');
    if (!data?.ok) {
      this.online.set(false);
      return false;
    }
    this.online.set(true);
    this.keys.set(Object.fromEntries(data.keys.map((k) => [k.provider, k])));
    // в браузере — только отметка «ключ есть» (подсказка …a1B2), сам ключ не покидает сервер
    this.models.providerKeys.set(Object.fromEntries(data.keys.filter((k) => k.set).map((k) => [k.provider, k.hint ?? '•'])));
    return true;
  }

  /** Сохранить (или удалить пустой строкой) ключ провайдера на сервере. */
  async setKey(provider: string, key: string): Promise<{ saved: boolean; error?: string }> {
    const res = await this.post<{ ok: boolean; error?: string }>('/api/keys', { provider, key: key.trim() });
    if (res === null) {
      this.online.set(false);
      return { saved: false };
    }
    if (!res.ok) return { saved: false, error: res.error };
    await this.refresh();
    void this.refreshModels(); // модели каталога этого провайдера сразу попадают в каскад
    return { saved: true };
  }

  /** Статистика за последние days дней. null — бэкенд недоступен. */
  stats(days = 371): Promise<StatsReply | null> {
    return this.request<StatsReply>(`/api/stats?days=${days}`);
  }

  private post<T>(url: string, body: unknown): Promise<T | null> {
    return this.request<T>(url, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  }

  /** null — бэкенд недоступен или ответил не JSON (например, прокси dev-сервера вернул 504). */
  private async request<T>(url: string, init?: RequestInit): Promise<T | null> {
    try {
      const res = await fetch(url, init);
      if (res.status === 401) {
        this.auth.onUnauthorized(); // сессия истекла — на страницу входа
        return null;
      }
      const data = (await res.json().catch(() => null)) as T | null;
      return data && typeof data === 'object' && 'ok' in data ? data : null;
    } catch {
      return null;
    }
  }
}
