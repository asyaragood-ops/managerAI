import { AiModel, LIMIT_UNITS, UsageLevel } from './types';

export const uid = (): string => Math.random().toString(36).slice(2, 10) + Date.now().toString(36).slice(-4);

/** Безопасное чтение/запись localStorage (в приватном режиме может бросать исключения). */
/**
 * Данные в браузере (чаты, модели, настройки) у каждого пользователя свои:
 * после входа ключи «iai.x» превращаются в «iai.u<id>.x». Тема оформления — общая.
 */
let storageScope = '';

export function setStorageScope(uid: number | null): void {
  storageScope = uid ? `u${uid}.` : '';
}

const GLOBAL_KEYS = new Set(['iai.theme']);

function scoped(key: string): string {
  return storageScope && key.startsWith('iai.') && !GLOBAL_KEYS.has(key) ? `iai.${storageScope}${key.slice(4)}` : key;
}

/** Старые ключи API из браузера (до появления аккаунтов) — их перенесёт на сервер первый вошедший. */
export const LEGACY_KEYS_KEY = 'iai.providerKeys.v1';

/**
 * Данные, сохранённые в этом браузере до появления аккаунтов, достаются первому вошедшему пользователю:
 * копируются в его раздел и удаляются из общего. Возвращает старые API-ключи (их нужно отправить на сервер).
 */
export function adoptLegacyStorage(uid: number): Record<string, string> {
  let keys: Record<string, string> = {};
  try {
    const legacy: string[] = [];
    for (let i = 0; i < localStorage.length; i++) {
      const k = localStorage.key(i);
      if (k && k.startsWith('iai.') && !GLOBAL_KEYS.has(k) && !/^iai\.u\d+\./.test(k)) legacy.push(k);
    }
    for (const k of legacy) {
      const v = localStorage.getItem(k);
      if (k === LEGACY_KEYS_KEY) {
        keys = v ? (JSON.parse(v) as Record<string, string>) : {};
      } else {
        const target = `iai.u${uid}.${k.slice(4)}`;
        if (v !== null && localStorage.getItem(target) === null) localStorage.setItem(target, v);
      }
      localStorage.removeItem(k);
    }
  } catch {
    /* хранилище недоступно — переносить нечего */
  }
  return keys;
}

export function loadJson<T>(key: string, fallback: T): T {
  try {
    const raw = localStorage.getItem(scoped(key));
    return raw ? (JSON.parse(raw) as T) : fallback;
  } catch {
    return fallback;
  }
}

export function saveJson(key: string, value: unknown): void {
  try {
    localStorage.setItem(scoped(key), JSON.stringify(value));
  } catch {
    /* хранилище недоступно — просто не сохраняем */
  }
}

export const usagePct = (m: AiModel): number => (m.limit > 0 ? Math.min(100, Math.round((m.used / m.limit) * 100)) : 0);

export function usageLevel(pct: number): UsageLevel {
  if (pct >= 100) return 'out';
  if (pct >= 85) return 'hi';
  if (pct >= 60) return 'mid';
  return 'lo';
}

export function statusLabel(m: AiModel, pct: number, hasKey = true): string {
  if (!m.enabled) return 'Выключена';
  if (m.access === 'web') return 'Только сайт';
  if (!hasKey) return 'Нужен ключ';
  if (pct >= 100) return 'Лимит исчерпан';
  if (pct >= 85) return 'Почти исчерпан';
  return 'Активна';
}

/** 1 234 567 → «1.2M», 54 000 → «54K». */
export function compact(n: number): string {
  if (n >= 1_000_000) return trim(n / 1_000_000) + 'M';
  if (n >= 10_000) return trim(n / 1_000) + 'K';
  return n.toLocaleString('ru-RU');
}
const trim = (x: number): string => (Math.round(x * 10) / 10).toString().replace(/\.0$/, '');

export const unitLabel = (m: AiModel): string => LIMIT_UNITS.find((u) => u.id === m.unit)?.label ?? '';

/** Текст лимита: «1.2M / 1.7M токенов», «$3 / $5», или пометка, если лимит не опубликован. */
export function usageText(m: AiModel): string {
  if (!m.limit) return 'лимит не опубликован';
  if (m.unit === 'usd') return `$${trim(m.used)} / $${trim(m.limit)}`;
  return `${compact(m.used)} / ${compact(m.limit)} ${unitLabel(m)}`;
}

/** Время до сброса или пометка для разовых лимитов. */
export function resetText(m: AiModel, now: number): string {
  if (!m.resetPeriodMs) return m.tier === 'trial' ? 'Разовые кредиты' : 'Разовый лимит';
  // Для сервисов «только сайт» и неопубликованных лимитов момент сброса неизвестен — показываем период.
  if (m.access === 'web' || !m.limit) return periodText(m.resetPeriodMs);
  return 'Сброс через ' + duration(m.resetAt - now);
}

function periodText(ms: number): string {
  const H = 3_600_000;
  if (ms <= H) return 'Обновляется каждые несколько часов';
  if (ms <= 24 * H) return 'Обновляется ежедневно';
  if (ms <= 7 * 24 * H) return 'Обновляется еженедельно';
  return 'Обновляется ежемесячно';
}

/** Миллисекунды → «3 ч 12 мин», «2 д 4 ч», «47 мин». */
export function duration(ms: number): string {
  if (ms <= 0) return 'сейчас';
  const min = Math.ceil(ms / 60_000);
  const d = Math.floor(min / 1440);
  const h = Math.floor((min % 1440) / 60);
  const m = min % 60;
  if (d > 0) return h ? `${d} д ${h} ч` : `${d} д`;
  if (h > 0) return m ? `${h} ч ${m.toString().padStart(2, '0')} мин` : `${h} ч`;
  return `${m} мин`;
}

export function timeLabel(at: number): string {
  return new Date(at).toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' });
}

export function relativeDay(at: number, now: number): string {
  const diff = now - at;
  if (diff < 60_000) return 'сейчас';
  if (diff < 3_600_000) return `${Math.floor(diff / 60_000)} мин`;
  const a = new Date(at), b = new Date(now);
  if (a.toDateString() === b.toDateString()) return `${Math.floor(diff / 3_600_000)} ч`;
  const y = new Date(now - 86_400_000);
  if (a.toDateString() === y.toDateString()) return 'вчера';
  return a.toLocaleDateString('ru-RU', { day: 'numeric', month: 'short' });
}

export function maskKey(key: string): string {
  if (!key) return '—';
  if (key.length <= 10) return '•'.repeat(key.length);
  return key.slice(0, 6) + '•'.repeat(14) + key.slice(-4);
}

export const PROVIDERS: { name: string; color: string }[] = [
  { name: 'OpenAI', color: '#10a37f' },
  { name: 'Anthropic', color: '#d97757' },
  { name: 'Google', color: '#4285f4' },
  { name: 'DeepSeek', color: '#4d6bfe' },
  { name: 'Mistral', color: '#fa520f' },
  { name: 'xAI', color: '#6b7280' },
  { name: 'Groq', color: '#f55036' },
  { name: 'Midjourney', color: '#a855f7' },
  { name: 'Runway', color: '#e11d48' },
  { name: 'ElevenLabs', color: '#f59e0b' },
  { name: 'Jasper', color: '#fa4d8c' },
];

const FALLBACK_COLORS = ['#8b5cf6', '#06b6d4', '#ec4899', '#14b8a6', '#f97316', '#6366f1'];

export function providerColor(provider: string): string {
  const p = PROVIDERS.find((x) => x.name.toLowerCase() === provider.trim().toLowerCase());
  if (p) return p.color;
  let h = 0;
  for (const ch of provider) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
  return FALLBACK_COLORS[h % FALLBACK_COLORS.length];
}
