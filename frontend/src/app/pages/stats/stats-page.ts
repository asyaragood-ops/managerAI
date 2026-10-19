import { ChangeDetectionStrategy, Component, ElementRef, computed, effect, inject, signal, viewChild } from '@angular/core';
import { RouterLink } from '@angular/router';
import { AuthService } from '../../core/auth.service';
import { BackendService, StatsDay, StatsModel, StatsReply } from '../../core/backend.service';
import { Icon } from '../../shared/icon';

/** Клетка календаря: один день. */
interface Cell {
  key: string;      // YYYY-MM-DD
  col: number;      // неделя
  row: number;      // 0 — понедельник … 6 — воскресенье
  level: number;    // 0 — не заходил; 1–4 — заходил, больше токенов → темнее
  data: StatsDay | null;
}

const MONTHS = ['янв', 'фев', 'мар', 'апр', 'май', 'июн', 'июл', 'авг', 'сен', 'окт', 'ноя', 'дек'];
const nf = new Intl.NumberFormat('ru-RU');

/** Дата YYYY-MM-DD ↔ дни от эпохи (UTC — только арифметика, без часовых поясов). */
const toDays = (key: string): number => Date.UTC(+key.slice(0, 4), +key.slice(5, 7) - 1, +key.slice(8, 10)) / 86_400_000;
const toKey = (days: number): string => new Date(days * 86_400_000).toISOString().slice(0, 10);
const tokensOf = (d: StatsDay | null): number => (d ? d.tokens_in + d.tokens_out : 0);

@Component({
  selector: 'app-stats-page',
  imports: [Icon, RouterLink],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './stats-page.html',
  styleUrl: './stats-page.css',
})
export class StatsPage {
  private readonly backend = inject(BackendService);
  protected readonly auth = inject(AuthService);

  protected readonly data = signal<StatsReply | null>(null);
  protected readonly loading = signal(true);
  protected readonly failed = signal(false);
  protected readonly selected = signal<string | null>(null);
  protected readonly hover = signal<{ cell: Cell; x: number; y: number } | null>(null);

  private readonly scroller = viewChild<ElementRef<HTMLElement>>('scroller');
  protected readonly fmt = (n: number): string => nf.format(n);

  constructor() {
    void this.load();
    // календарь длинный — на узком экране сразу показываем последние недели
    effect(() => {
      const el = this.scroller()?.nativeElement;
      if (el && this.data()) queueMicrotask(() => (el.scrollLeft = el.scrollWidth));
    });
  }

  async load(): Promise<void> {
    this.loading.set(true);
    const res = await this.backend.stats();
    this.loading.set(false);
    this.failed.set(!res?.ok);
    if (res?.ok) {
      this.data.set(res);
      if (!this.selected()) this.selected.set(res.today);
    }
  }

  private readonly byDay = computed(() => new Map((this.data()?.days ?? []).map((d) => [d.day, d])));

  /** Сетка за последний год: колонки — недели (с понедельника), строки — дни недели. */
  protected readonly grid = computed(() => {
    const d = this.data();
    if (!d) return { cells: [] as Cell[], weeks: 0, months: [] as { col: number; label: string }[] };
    const today = toDays(d.today);
    const weekday = (n: number) => (new Date(n * 86_400_000).getUTCDay() + 6) % 7; // пн = 0
    const start = today - 364 - weekday(today - 364);

    // уровни цвета — по квартилям расхода токенов в дни с активностью
    const used = [...this.byDay().values()].map(tokensOf).filter((t) => t > 0).sort((a, b) => a - b);
    const q = (p: number) => used[Math.min(used.length - 1, Math.floor(p * used.length))] ?? 0;
    const [q1, q2, q3] = [q(0.25), q(0.5), q(0.75)];
    const level = (day: StatsDay | null): number => {
      if (!day || (!day.visited && !day.requests)) return 0;
      const t = tokensOf(day);
      if (t === 0 || used.length === 0) return 1;
      return t <= q1 ? 1 : t <= q2 ? 2 : t <= q3 ? 3 : 4;
    };

    const cells: Cell[] = [];
    const months: { col: number; label: string }[] = [];
    let lastMonth = -1;
    for (let n = start; n <= today; n++) {
      const key = toKey(n);
      const col = Math.floor((n - start) / 7);
      const row = (n - start) % 7;
      if (row === 0) {
        const m = +key.slice(5, 7) - 1;
        if (m !== lastMonth) {
          // подпись месяца — над первой неделей, где он начинается (и не впритык к предыдущей)
          if (!months.length || col - months[months.length - 1].col >= 3) months.push({ col, label: MONTHS[m] });
          lastMonth = m;
        }
      }
      const data = this.byDay().get(key) ?? null;
      cells.push({ key, col, row, level: level(data), data });
    }
    return { cells, weeks: Math.floor((today - start) / 7) + 1, months };
  });

  protected readonly todayStats = computed(() => {
    const d = this.data();
    return d ? this.byDay().get(d.today) ?? null : null;
  });

  /** Дней подряд с посещением, заканчивая сегодня (или вчера, если сегодня ещё не отмечено). */
  protected readonly streak = computed(() => {
    const d = this.data();
    if (!d) return 0;
    const visited = (n: number) => !!this.byDay().get(toKey(n))?.visited;
    let n = toDays(d.today);
    if (!visited(n)) n--;
    let s = 0;
    while (visited(n)) {
      s++;
      n--;
    }
    return s;
  });

  protected readonly visitsYear = computed(() => [...this.byDay().values()].filter((d) => d.visited).length);
  protected readonly tokensYear = computed(() => [...this.byDay().values()].reduce((s, d) => s + tokensOf(d), 0));

  /** Остаток на сегодня: по личному лимиту, если он задан. */
  protected readonly budget = computed(() => {
    const d = this.data();
    if (!d || d.daily_limit <= 0) return null;
    const left = Math.max(0, d.daily_limit - d.used_today);
    return { left, limit: d.daily_limit, pct: Math.min(100, Math.round((d.used_today / d.daily_limit) * 100)) };
  });

  protected readonly selectedDay = computed(() => {
    const key = this.selected();
    return key ? { key, data: this.byDay().get(key) ?? null } : null;
  });

  protected readonly activeDays = computed(() =>
    [...this.byDay().values()].filter((d) => d.visited || d.requests).sort((a, b) => b.day.localeCompare(a.day)),
  );

  protected readonly resetTime = computed(() => {
    const d = this.data();
    return d ? new Date(d.limits_reset_at * 1000).toLocaleTimeString('ru-RU', { hour: '2-digit', minute: '2-digit' }) : '';
  });

  protected dateLabel(key: string, withWeekday = true): string {
    return new Date(toDays(key) * 86_400_000).toLocaleDateString('ru-RU', {
      day: 'numeric', month: 'long', year: 'numeric', timeZone: 'UTC', ...(withWeekday ? { weekday: 'long' } : {}),
    });
  }

  protected cellLabel(c: Cell): string {
    const base = this.dateLabel(c.key);
    if (!c.level) return `${base}: не заходил`;
    return `${base}: заходил, ${nf.format(tokensOf(c.data))} токенов, запросов: ${c.data?.requests ?? 0}`;
  }

  protected tokens(d: StatsDay | null): number {
    return tokensOf(d);
  }

  protected modelRows = computed(() => (this.data()?.models ?? []).slice().sort((a, b) => this.modelPct(b) - this.modelPct(a)));

  protected modelPct(m: StatsModel): number {
    const r = m.rpd ? m.req_day / m.rpd : 0;
    const t = m.tpd ? m.tokens_day / m.tpd : 0;
    return Math.min(100, Math.round(Math.max(r, t) * 100));
  }

  protected kindWord(k: string): string {
    return k === 'image' ? 'картинки' : k === 'audio' ? 'озвучка' : k === 'video' ? 'видео' : 'текст';
  }

  // ---- взаимодействие с календарём ----

  protected pick(c: Cell): void {
    this.selected.set(c.key);
  }

  protected showTip(c: Cell, e: MouseEvent | FocusEvent): void {
    const el = e.target as HTMLElement;
    const box = el.parentElement!.getBoundingClientRect();
    const r = el.getBoundingClientRect();
    this.hover.set({ cell: c, x: r.left - box.left + r.width / 2, y: r.top - box.top });
  }

  /** Стрелки двигают выбранный день: ←/→ — неделя, ↑/↓ — день. */
  protected onKey(e: KeyboardEvent): void {
    const d = this.data();
    const cur = this.selected();
    if (!d || !cur) return;
    const step = { ArrowLeft: -7, ArrowRight: 7, ArrowUp: -1, ArrowDown: 1, Home: -364, End: 364 }[e.key];
    if (step === undefined) return;
    e.preventDefault();
    const first = this.grid().cells[0]?.key ?? d.today;
    const n = Math.min(toDays(d.today), Math.max(toDays(first), toDays(cur) + step));
    const key = toKey(n);
    this.selected.set(key);
    queueMicrotask(() => (document.querySelector(`[data-day="${key}"]`) as HTMLElement | null)?.focus());
  }
}
