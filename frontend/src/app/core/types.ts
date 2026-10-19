/** Специализации (навыки) моделей — по ним работает фильтр и умный роутер. */
export const SKILLS = [
  { id: 'text', label: 'Тексты' },
  { id: 'code', label: 'Код' },
  { id: 'design', label: 'Дизайн' },
  { id: 'video', label: 'Монтаж видео' },
  { id: 'scripts', label: 'Сценарии' },
  { id: 'docs', label: 'Документы' },
  { id: 'sheets', label: 'Таблицы' },
  { id: 'finance', label: 'Финансы' },
  { id: 'marketing', label: 'Маркетинг' },
  { id: 'images', label: 'Изображения' },
  { id: 'audio', label: 'Аудио' },
] as const;

export type SkillId = (typeof SKILLS)[number]['id'];

export const skillLabel = (id: SkillId): string => SKILLS.find((s) => s.id === id)?.label ?? id;

/** В чём считается лимит модели. */
export type LimitUnit = 'tokens' | 'requests' | 'generations' | 'credits' | 'chars' | 'minutes' | 'usd' | 'neurons';

/** Период сброса лимита из каталога. once — разовый лимит, не сбрасывается. */
export type LimitPeriod = 'minute' | 'hour' | 'day' | 'week' | 'month' | 'once';

export const PERIOD_MS: Record<LimitPeriod, number> = {
  minute: 60_000,
  hour: 3_600_000,
  day: 24 * 3_600_000,
  week: 7 * 24 * 3_600_000,
  month: 30 * 24 * 3_600_000,
  once: 0,
};

export const LIMIT_UNITS: { id: LimitUnit; label: string }[] = [
  { id: 'tokens', label: 'токенов' },
  { id: 'requests', label: 'запросов' },
  { id: 'generations', label: 'генераций' },
  { id: 'credits', label: 'кредитов' },
  { id: 'chars', label: 'символов' },
  { id: 'minutes', label: 'минут' },
  { id: 'usd', label: '$ кредитов' },
  { id: 'neurons', label: 'нейронов' },
];

export const RESET_PERIODS: { ms: number; label: string }[] = [
  { ms: 3_600_000, label: 'Каждый час' },
  { ms: 5 * 3_600_000, label: 'Каждые 5 часов' },
  { ms: 24 * 3_600_000, label: 'Каждые 24 часа' },
  { ms: 7 * 24 * 3_600_000, label: 'Каждую неделю' },
  { ms: 30 * 24 * 3_600_000, label: 'Каждый месяц' },
];

export interface AiModel {
  id: string;
  name: string;
  provider: string;
  /** Цвет аватара модели. */
  color: string;
  apiKey: string;
  skills: SkillId[];
  limit: number;
  used: number;
  unit: LimitUnit;
  resetPeriodMs: number;
  /** Unix-время (мс) следующего сброса лимита. */
  resetAt: number;
  enabled: boolean;
  /** Чем меньше число, тем «качественнее» модель для стратегии «Качественнее». */
  priority: number;
  /** api — модель можно вызывать; web — бесплатно только на сайте сервиса (роутер её пропускает). */
  access?: 'api' | 'web';
  /** trial — разовые/пробные кредиты. */
  tier?: 'free' | 'trial';
  /** Описание бесплатного лимита словами. */
  note?: string;
  /** Ссылка на сервис или источник данных о лимите. */
  url?: string;
  /** id записи каталога (для встроенных моделей). */
  catalogId?: string;
}

export type NewModelInput = Pick<AiModel, 'name' | 'provider' | 'apiKey' | 'skills' | 'limit' | 'unit' | 'resetPeriodMs'>;

export type UsageLevel = 'lo' | 'mid' | 'hi' | 'out';

export type MessageRole = 'user' | 'assistant' | 'switch';

/** Что сгенерировала медиа-модель. Файл хранится на бэкенде (backend/data/media), в чате — только ссылка. */
export type MediaKind = 'image' | 'audio' | 'video';

export interface ChatMedia {
  kind: MediaKind;
  /** /api/media/<id>.png — отдаёт бэкенд */
  url: string;
  mime: string;
  /** Промпт, который ушёл модели (для картинок и видео — на английском). */
  prompt?: string;
  /** Текстовая модель, которая подготовила промпт. */
  promptBy?: string | null;
}

export interface ChatMessage {
  id: string;
  role: MessageRole;
  text: string;
  at: number;
  /** Какая модель ответила (для assistant) или на какую переключились (для switch). */
  modelId?: string;
  /** С какой модели переключились (для switch). */
  fromModelId?: string;
  /** Имена моделей от бэкенда (их может не быть в каталоге фронтенда). */
  modelLabel?: string;
  fromLabel?: string;
  /** Бэйдж от бэкенда: «Ответил: Gemini». */
  badge?: string;
  /** Для switch: true — причина в лимите, false — ошибка/недоступность. */
  limit?: boolean;
  /** Ответ получен в демо-режиме (бэкенд недоступен). */
  demo?: boolean;
  /** Картинка, аудио или видео от медиа-модели. */
  media?: ChatMedia;
}

export interface Chat {
  id: string;
  title: string;
  skill: SkillId | null;
  /** Модель, которая сейчас ведёт этот чат. */
  modelId: string | null;
  /** 'auto' — выбирает роутер, иначе id закреплённой модели. */
  mode: 'auto' | string;
  messages: ChatMessage[];
  updatedAt: number;
}

export type RouterStrategy = 'cheap' | 'free' | 'quality';

export interface RouterSettings {
  autoSwitch: boolean;
  /** Порог в процентах, при котором роутер переключает модель заранее. */
  threshold: number;
  strategy: RouterStrategy;
  keepContext: boolean;
  notifySwitch: boolean;
  encryptKeys: boolean;
  /** Личный дневной лимит токенов (0 — без лимита). Соблюдает бэкенд. */
  dailyTokenLimit: number;
}
