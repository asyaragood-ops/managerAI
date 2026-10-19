import { ChangeDetectionStrategy, Component, ElementRef, computed, inject, input, output, signal, viewChild } from '@angular/core';
import { CATALOG } from '../core/catalog';
import { ModelsService } from '../core/models.service';
import { AiModel, SkillId } from '../core/types';
import { usagePct } from '../core/utils';
import { Icon } from './icon';

/** Модели каталога, которые умеет вызывать бэкенд-роутер (чат-модели с известным API id). */
const ROUTABLE = new Set(CATALOG.filter((e) => e.routable).map((e) => e.id));

interface Option {
  m: AiModel;
  pct: number;
  ready: boolean;
  matches: boolean;
}

/**
 * Выбор модели для чата: «Авто-роутинг» или конкретная модель, с поиском.
 * Показывает модели, которые роутер умеет вызывать (чат-модели по API и добавленные вручную);
 * модели «только сайт», генераторы картинок/аудио и т.п. закрепить в чате нельзя.
 */
@Component({
  selector: 'app-model-picker',
  imports: [Icon],
  changeDetection: ChangeDetectionStrategy.OnPush,
  host: { '(document:click)': 'open.set(false)', '(document:keydown.escape)': 'open.set(false)' },
  template: `
    <button type="button" class="trigger glass" (click)="toggle($event)" [attr.aria-expanded]="open()" aria-haspopup="listbox" title="Какая модель отвечает в этом чате">
      <app-icon name="route" />
      <span class="lbl">{{ current()?.name ?? 'Авто-роутинг' }}</span>
      @if (!current() && autoModel()) { <em>{{ autoModel() }}</em> }
      @if (current() && !currentReady()) { <em class="warn">нужен ключ</em> }
      <svg class="chev" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="m6 9 6 6 6-6"/></svg>
    </button>

    @if (open()) {
      <div class="panel" (click)="$event.stopPropagation()" role="dialog" aria-label="Выбор модели">
        <label class="search">
          <app-icon name="search" />
          <input #q type="search" placeholder="Поиск модели или провайдера…" [value]="query()" (input)="query.set($any($event.target).value)"
                 (keydown.enter)="pickFirst()" aria-label="Поиск модели" />
        </label>

        <div class="list" role="listbox">
          @if (!query()) {
            <button type="button" class="opt auto" role="option" [class.on]="value() === 'auto'" [attr.aria-selected]="value() === 'auto'" (click)="choose('auto')">
              <span class="av sm grad">∞</span>
              <span class="nm"><b>Авто-роутинг</b><small>Роутер сам выберет модель под задачу и переключится при исчерпании лимита</small></span>
              @if (value() === 'auto') { <app-icon name="check" /> }
            </button>
          }

          @for (g of groups(); track g.title) {
            <div class="gh">{{ g.title }} · {{ g.items.length }}</div>
            @for (o of g.items; track o.m.id) {
              <button type="button" class="opt" role="option" [class.on]="o.m.id === value()" [attr.aria-selected]="o.m.id === value()" (click)="choose(o.m.id)">
                <span class="av sm" [style.--c]="o.m.color">{{ o.m.name[0] }}</span>
                <span class="nm"><b>{{ o.m.name }}</b><small>{{ o.m.provider }}{{ kindHint(o.m) }}{{ o.matches ? ' · подходит для задачи' : '' }}</small></span>
                @if (o.ready) {
                  <span class="pct">{{ o.m.limit ? o.pct + '%' : '' }}</span>
                } @else {
                  <span class="nk">нет ключа</span>
                }
                @if (o.m.id === value()) { <app-icon name="check" /> }
              </button>
            }
          } @empty {
            <p class="none">Ничего не найдено</p>
          }
        </div>

        @if (current() && !currentReady()) {
          <p class="hint">Для {{ current()!.provider }} не указан API-ключ — добавьте его в «Настройках», иначе ответит другая модель.</p>
        }
      </div>
    }
  `,
  styles: `
    :host { position: relative; display: inline-block; }
    .trigger { display: flex; align-items: center; gap: 8px; height: 40px; padding: 0 12px 0 14px; border-radius: 999px; font-weight: 600; max-width: 340px; }
    .trigger .i { color: var(--accent); }
    .lbl { white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .trigger em { font-style: normal; font-weight: 500; color: var(--mut); padding-left: 10px; border-left: 1px solid var(--bd2); white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .trigger em.warn { color: var(--warn); }
    .chev { width: 16px; height: 16px; color: var(--mut); flex: none; }
    .panel { position: absolute; right: 0; top: calc(100% + 8px); z-index: 30; width: min(420px, calc(100vw - 24px)); padding: 10px; border-radius: 20px;
      background: var(--modal); border: 1px solid var(--bd2); box-shadow: var(--sh-lg); backdrop-filter: blur(20px); animation: pop .15s var(--ease); }
    .search { display: flex; align-items: center; gap: 8px; height: 40px; padding: 0 12px; border-radius: 12px; background: var(--surf2); color: var(--mut); margin-bottom: 8px; }
    .search input { flex: 1; border: 0; background: none; outline: none; color: var(--tx); }
    .list { max-height: min(420px, 60vh); overflow-y: auto; display: flex; flex-direction: column; gap: 2px; }
    .gh { font-size: 11.5px; font-weight: 600; text-transform: uppercase; letter-spacing: .05em; color: var(--mut); padding: 10px 10px 4px; }
    .opt { display: flex; align-items: center; gap: 10px; width: 100%; padding: 8px 10px; border-radius: 12px; text-align: left; }
    .opt:hover { background: var(--hover); }
    .opt.on { background: color-mix(in srgb, var(--accent) 14%, transparent); }
    .opt > .i { color: var(--accent); }
    .grad { background: var(--grad); }
    .nm { flex: 1; min-width: 0; display: flex; flex-direction: column; }
    .nm b { font-weight: 600; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .nm small { color: var(--mut); font-size: 12px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
    .auto .nm small { white-space: normal; }
    .pct { font-size: 12px; font-weight: 600; color: var(--mut); }
    .nk { font-size: 11px; font-weight: 600; color: var(--warn); white-space: nowrap; }
    .none { padding: 16px; text-align: center; color: var(--mut); }
    .hint { font-size: 12px; color: var(--warn); padding: 8px 10px 2px; }
    @media (max-width: 900px) { .trigger em { display: none; } .trigger { max-width: 180px; } }
  `,
})
export class ModelPicker {
  private readonly models = inject(ModelsService);

  /** 'auto' или id модели. */
  readonly value = input.required<string>();
  readonly skill = input<SkillId | null>(null);
  /** Имя модели, которую сейчас выбрал бы авто-роутинг (для подписи). */
  readonly autoModel = input<string | null>(null);
  readonly changed = output<string>();

  protected readonly open = signal(false);
  protected readonly query = signal('');
  private readonly q = viewChild<ElementRef<HTMLInputElement>>('q');

  /** Медиа-модели отвечают файлом, а не текстом — подсказываем это в списке. */
  protected kindHint(m: AiModel): string {
    if (m.skills.includes('text')) return '';
    if (m.skills.includes('images')) return ' · рисует картинки';
    if (m.skills.includes('audio')) return ' · озвучивает текст';
    return '';
  }

  protected readonly current = computed(() => (this.value() === 'auto' ? undefined : this.models.get(this.value())));
  protected readonly currentReady = computed(() => {
    const m = this.current();
    return !!m && !!this.models.keyFor(m);
  });

  private readonly options = computed<Option[]>(() => {
    this.models.providerKeys();
    const skill = this.skill();
    return this.models
      .models()
      .filter((m) => m.access !== 'web' && m.enabled && (!m.catalogId || ROUTABLE.has(m.catalogId)))
      .map((m) => ({ m, pct: usagePct(m), ready: !!this.models.keyFor(m), matches: !!skill && m.skills.includes(skill) }));
  });

  protected readonly groups = computed(() => {
    const q = this.query().trim().toLowerCase();
    const list = this.options()
      .filter((o) => !q || o.m.name.toLowerCase().includes(q) || o.m.provider.toLowerCase().includes(q))
      .sort((a, b) => Number(b.matches) - Number(a.matches) || a.pct - b.pct || a.m.name.localeCompare(b.m.name));
    return [
      { title: 'Готовы к работе', items: list.filter((o) => o.ready) },
      { title: 'Нужен API-ключ', items: list.filter((o) => !o.ready) },
    ].filter((g) => g.items.length);
  });

  protected toggle(e: Event): void {
    e.stopPropagation();
    this.open.update((v) => !v);
    if (this.open()) {
      this.query.set('');
      setTimeout(() => this.q()?.nativeElement.focus());
    }
  }

  protected choose(id: string): void {
    this.changed.emit(id);
    this.open.set(false);
  }

  protected pickFirst(): void {
    const first = this.groups()[0]?.items[0];
    if (first) this.choose(first.m.id);
  }
}
