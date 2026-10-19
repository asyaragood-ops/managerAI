import { ChangeDetectionStrategy, Component, ElementRef, afterNextRender, computed, inject, output, signal, viewChild } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { ModelsService } from '../core/models.service';
import { AiModel, LIMIT_UNITS, LimitUnit, RESET_PERIODS, SKILLS, SkillId } from '../core/types';
import { PROVIDERS } from '../core/utils';
import { Icon } from './icon';

@Component({
  selector: 'app-add-model-dialog',
  imports: [FormsModule, Icon],
  changeDetection: ChangeDetectionStrategy.OnPush,
  host: { '(document:keydown.escape)': 'closed.emit()' },
  template: `
    <div class="overlay" (click)="closed.emit()">
      <form class="modal" role="dialog" aria-modal="true" aria-labelledby="am-title" (click)="$event.stopPropagation()" (ngSubmit)="submit()">
        <div class="md-h">
          <div>
            <h2 id="am-title">Новая модель</h2>
            <p class="muted">Роутер начнёт использовать её сразу после добавления</p>
          </div>
          <button type="button" class="ib" (click)="closed.emit()" aria-label="Закрыть"><app-icon name="x" /></button>
        </div>

        <div class="field">
          <label for="am-name">Название модели</label>
          <input #nameInput id="am-name" class="input" name="name" placeholder="Например, Claude Sonnet" autocomplete="off"
                 [class.invalid]="touched() && !name().trim()" [ngModel]="name()" (ngModelChange)="name.set($event)" />
          @if (touched() && !name().trim()) { <span class="err">Введите название модели</span> }
        </div>

        <div class="field">
          <label for="am-provider">Провайдер</label>
          <input id="am-provider" class="input" name="provider" list="am-providers" placeholder="OpenAI, Anthropic, Google…" autocomplete="off"
                 [ngModel]="provider()" (ngModelChange)="provider.set($event)" />
          <datalist id="am-providers">
            @for (p of providers; track p.name) { <option [value]="p.name"></option> }
          </datalist>
        </div>

        <div class="field">
          <label for="am-key">API-ключ</label>
          <div class="input-wrap">
            <input id="am-key" class="input mono" name="apiKey" autocomplete="off" spellcheck="false" placeholder="sk-…"
                   [type]="showKey() ? 'text' : 'password'" [class.invalid]="touched() && !apiKey().trim()"
                   [ngModel]="apiKey()" (ngModelChange)="apiKey.set($event)" />
            <button type="button" class="ib" (click)="showKey.set(!showKey())" [attr.aria-label]="showKey() ? 'Скрыть ключ' : 'Показать ключ'">
              <app-icon [name]="showKey() ? 'eyeOff' : 'eye'" />
            </button>
          </div>
          @if (touched() && !apiKey().trim()) { <span class="err">Вставьте API-ключ</span> }
        </div>

        <div class="field">
          <span class="label">Специализации</span>
          <div class="chips">
            @for (s of skillList; track s.id) {
              <button type="button" class="chip sm" [class.on]="skills().includes(s.id)" (click)="toggleSkill(s.id)" [attr.aria-pressed]="skills().includes(s.id)">{{ s.label }}</button>
            }
          </div>
        </div>

        <div class="two">
          <div class="field">
            <label for="am-limit">Лимит</label>
            <div class="limit">
              <input id="am-limit" class="input" name="limit" type="number" min="1" [ngModel]="limit()" (ngModelChange)="limit.set(+$event)" />
              <select class="input" name="unit" aria-label="Единица лимита" [ngModel]="unit()" (ngModelChange)="unit.set($event)">
                @for (u of units; track u.id) { <option [ngValue]="u.id">{{ u.label }}</option> }
              </select>
            </div>
          </div>
          <div class="field">
            <label for="am-period">Период сброса</label>
            <select id="am-period" class="input" name="period" [ngModel]="period()" (ngModelChange)="period.set(+$event)">
              @for (p of periods; track p.ms) { <option [ngValue]="p.ms">{{ p.label }}</option> }
            </select>
          </div>
        </div>

        <div class="md-f">
          <button type="button" class="btn" (click)="closed.emit()">Отмена</button>
          <button type="submit" class="btn primary"><app-icon name="plus" />Добавить модель</button>
        </div>
      </form>
    </div>
  `,
  styles: `
    .modal { display: flex; flex-direction: column; gap: 14px; }
    .md-h { display: flex; justify-content: space-between; gap: 12px; }
    h2 { font-size: 20px; font-weight: 700; letter-spacing: -.01em; }
    .two { display: grid; grid-template-columns: 1.3fr 1fr; gap: 12px; }
    .limit { display: grid; grid-template-columns: 1fr 120px; gap: 6px; }
    .md-f { display: flex; justify-content: flex-end; gap: 10px; margin-top: 6px; }
    @media (max-width: 520px) { .two { grid-template-columns: 1fr; } }
  `,
})
export class AddModelDialog {
  private readonly models = inject(ModelsService);
  readonly closed = output<void>();
  readonly added = output<AiModel>();

  protected readonly providers = PROVIDERS;
  protected readonly skillList = SKILLS;
  protected readonly units = LIMIT_UNITS;
  protected readonly periods = RESET_PERIODS;

  protected readonly name = signal('');
  protected readonly provider = signal('');
  protected readonly apiKey = signal('');
  protected readonly showKey = signal(false);
  protected readonly skills = signal<SkillId[]>(['text']);
  protected readonly limit = signal(1_000_000);
  protected readonly unit = signal<LimitUnit>('tokens');
  protected readonly period = signal(RESET_PERIODS[1].ms);
  protected readonly touched = signal(false);
  protected readonly valid = computed(() => !!this.name().trim() && !!this.apiKey().trim() && this.limit() > 0);

  private readonly nameInput = viewChild<ElementRef<HTMLInputElement>>('nameInput');

  constructor() {
    afterNextRender(() => this.nameInput()?.nativeElement.focus());
  }

  protected toggleSkill(id: SkillId): void {
    this.skills.update((l) => (l.includes(id) ? l.filter((x) => x !== id) : [...l, id]));
  }

  protected submit(): void {
    this.touched.set(true);
    if (!this.valid()) return;
    const model = this.models.add({
      name: this.name(),
      provider: this.provider(),
      apiKey: this.apiKey().trim(),
      skills: this.skills(),
      limit: this.limit(),
      unit: this.unit(),
      resetPeriodMs: this.period(),
    });
    this.added.emit(model);
    this.closed.emit();
  }
}
