import { Injectable, computed, effect, signal } from '@angular/core';
import { CATALOG, CatalogEntry } from './catalog';
import { AiModel, NewModelInput, PERIOD_MS } from './types';
import { loadJson, providerColor, saveJson, uid, usagePct } from './utils';

const KEY = 'iai.models.v1';
const SEED_VERSION_KEY = 'iai.seedVersion';
const SEED_VERSION = 4;
/** id моделей каталога, которые пользователь удалил, — чтобы они не возвращались. */
const HIDDEN_KEY = 'iai.hiddenCatalog.v1';
const STATS_KEY = 'iai.stats.v1';

interface DayStats {
  day: string;
  requests: number;
  switches: number;
}

const today = () => new Date().toDateString();

/** Модель из записи каталога бесплатных моделей. */
function fromCatalog(e: CatalogEntry, now: number): AiModel {
  const period = PERIOD_MS[e.period];
  return {
    id: e.id, catalogId: e.id, name: e.name, provider: e.provider, color: providerColor(e.provider), apiKey: '',
    skills: e.skills, limit: e.limit, used: 0, unit: e.unit, resetPeriodMs: period, resetAt: period ? now + period : 0,
    enabled: true, priority: e.skills.includes('finance') ? 1 : e.tier === 'trial' ? 3 : 2,
    access: e.access, tier: e.tier, note: e.note, url: e.url,
  };
}

/** Добавить в список записи каталога, которых в нём ещё нет (по id). */
function withCatalog(list: AiModel[]): AiModel[] {
  const now = Date.now();
  const have = new Set([...list.map((m) => m.catalogId ?? m.id), ...loadJson<string[]>(HIDDEN_KEY, [])]);
  return [...list, ...CATALOG.filter((e) => !have.has(e.id)).map((e) => fromCatalog(e, now))];
}

/**
 * Подготовка сохранённых данных: удаляем старые демо-модели (с ключами «demo-…»)
 * и добавляем записи каталога, которых ещё нет.
 */
function migrate(saved: AiModel[]): AiModel[] {
  const version = loadJson<number>(SEED_VERSION_KEY, saved.length ? 1 : SEED_VERSION);
  saveJson(SEED_VERSION_KEY, SEED_VERSION);
  const list = version < 4 ? saved.filter((m) => m.catalogId || !m.apiKey.startsWith('demo-')) : saved;
  return withCatalog(list);
}

@Injectable({ providedIn: 'root' })
export class ModelsService {
  readonly models = signal<AiModel[]>(migrate(loadJson<AiModel[]>(KEY, [])));
  /** Текущее время — обновляется по таймеру, чтобы «до сброса» и сброс лимитов были живыми. */
  readonly now = signal(Date.now());
  readonly stats = signal<DayStats>(this.freshStats(loadJson<DayStats>(STATS_KEY, { day: today(), requests: 0, switches: 0 })));

  /**
   * Провайдеры, для которых у пользователя есть API-ключ на сервере: провайдер → подсказка «…a1B2».
   * Один ключ включает все модели провайдера. Сами ключи в браузере не хранятся — список приходит с бэкенда.
   */
  readonly providerKeys = signal<Record<string, string>>({});

  readonly byId = computed(() => new Map(this.models().map((m) => [m.id, m])));
  /** Модели, которые роутер может использовать прямо сейчас: API, есть ключ, лимит не исчерпан. */
  readonly activeCount = computed(
    () => this.models().filter((m) => m.enabled && m.access !== 'web' && !!this.keyFor(m) && usagePct(m) < 100).length,
  );
  readonly withKeyCount = computed(() => this.models().filter((m) => m.access !== 'web' && !!this.keyFor(m)).length);
  readonly nearestReset = computed(() => {
    const now = this.now();
    const times = this.models().filter((m) => m.used > 0 && m.resetPeriodMs > 0).map((m) => m.resetAt - now);
    return times.length ? Math.min(...times) : null;
  });
  readonly avgLoad = computed(() => {
    const list = this.models().filter((m) => m.enabled);
    return list.length ? Math.round(list.reduce((s, m) => s + usagePct(m), 0) / list.length) : 0;
  });

  constructor() {
    effect(() => saveJson(KEY, this.models()));
    effect(() => saveJson(STATS_KEY, this.stats()));
    this.tick();
    setInterval(() => this.tick(), 15_000);
  }

  /** Есть ли у модели ключ: собственный (модель, добавленная вручную) или ключ её провайдера на сервере. */
  keyFor(m: AiModel): string {
    return m.apiKey || this.providerKeys()[m.provider] || '';
  }

  get(id: string | null | undefined): AiModel | undefined {
    return id ? this.byId().get(id) : undefined;
  }

  add(input: NewModelInput): AiModel {
    const model: AiModel = {
      ...input,
      id: uid(),
      name: input.name.trim(),
      provider: input.provider.trim() || 'Другой',
      color: providerColor(input.provider),
      used: 0,
      resetAt: Date.now() + input.resetPeriodMs,
      enabled: true,
      priority: 2,
      access: 'api',
    };
    this.models.update((list) => [...list, model]);
    return model;
  }

  update(id: string, patch: Partial<AiModel>): void {
    this.models.update((list) => list.map((m) => (m.id === id ? { ...m, ...patch } : m)));
  }

  remove(id: string): void {
    const cat = this.get(id)?.catalogId;
    if (cat) saveJson(HIDDEN_KEY, [...new Set([...loadJson<string[]>(HIDDEN_KEY, []), cat])]);
    this.models.update((list) => list.filter((m) => m.id !== id));
  }

  resetUsage(id: string): void {
    const m = this.get(id);
    if (m) this.update(id, { used: 0, resetAt: m.resetPeriodMs ? Date.now() + m.resetPeriodMs : 0 });
  }

  /** Списать расход с лимита модели (на бэкенде это будет делать роутер). */
  consume(id: string, amount: number): void {
    this.models.update((list) =>
      list.map((m) => (m.id === id ? { ...m, used: m.limit ? Math.min(m.limit, m.used + amount) : m.used + amount } : m)),
    );
  }

  countRequest(switched: boolean): void {
    this.stats.update((s) => {
      const base = this.freshStats(s);
      return { ...base, requests: base.requests + 1, switches: base.switches + (switched ? 1 : 0) };
    });
  }

  private freshStats(s: DayStats): DayStats {
    return s.day === today() ? s : { day: today(), requests: 0, switches: 0 };
  }

  private tick(): void {
    const now = Date.now();
    this.now.set(now);
    // Демо-сброс лимитов по расписанию (в проде это делает бэкенд).
    const due = (m: AiModel) => m.resetPeriodMs > 0 && m.resetAt <= now;
    if (this.models().some(due)) {
      this.models.update((list) =>
        list.map((m) => {
          if (!due(m)) return m;
          const periods = Math.ceil((now - m.resetAt) / m.resetPeriodMs) || 1;
          return { ...m, used: 0, resetAt: m.resetAt + periods * m.resetPeriodMs };
        }),
      );
    }
  }
}
