import { NgTemplateOutlet } from '@angular/common';
import { ChangeDetectionStrategy, Component, computed, effect, inject, signal } from '@angular/core';
import { BackendService } from '../../core/backend.service';
import { CATALOG_DATE } from '../../core/catalog';
import { ModelsService } from '../../core/models.service';
import { AiModel, SKILLS, SkillId, UsageLevel, skillLabel } from '../../core/types';
import { duration, loadJson, resetText, saveJson, statusLabel, usageLevel, usagePct, usageText } from '../../core/utils';
import { AddModelDialog } from '../../shared/add-model-dialog';
import { Icon } from '../../shared/icon';

interface ModelView {
  m: AiModel;
  pct: number;
  level: UsageLevel;
  hasKey: boolean;
  /** Есть числовой лимит — показываем % и прогресс-бар. */
  measurable: boolean;
  status: string;
  statusClass: string;
  usage: string;
  resetIn: string;
}

type ViewMode = 'grid' | 'list';
type AccessFilter = 'all' | 'api' | 'web' | 'keyed';

const PAGE = 48;

@Component({
  selector: 'app-models-page',
  imports: [Icon, AddModelDialog, NgTemplateOutlet],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './models-page.html',
  styleUrl: './models-page.css',
  host: { '(document:click)': 'menuFor.set(null)' },
})
export class ModelsPage {
  protected readonly svc = inject(ModelsService);
  private readonly backend = inject(BackendService);

  /** Модель сейчас участвует в каскаде бэкенда (ключ введён, модель включена). */
  protected inCascade(m: AiModel): boolean {
    return this.backend.cascadeIds().has(m.catalogId ?? m.id);
  }

  protected readonly skills = SKILLS;
  protected readonly skillLabel = skillLabel;
  protected readonly duration = duration;
  protected readonly catalogDate = new Date(CATALOG_DATE).toLocaleDateString('ru-RU');

  protected readonly view = signal<ViewMode>(loadJson<ViewMode>('iai.modelsView', 'grid'));
  protected readonly filter = signal<SkillId | 'all'>('all');
  protected readonly access = signal<AccessFilter>('all');
  protected readonly provider = signal('');
  protected readonly query = signal('');
  protected readonly limit = signal(PAGE);
  protected readonly dialogOpen = signal(false);
  protected readonly menuFor = signal<string | null>(null);
  protected readonly toast = signal<string | null>(null);

  protected readonly items = computed<ModelView[]>(() => {
    const now = this.svc.now();
    this.svc.providerKeys(); // пересчитать при изменении ключей
    return this.svc.models().map((m) => {
      const pct = usagePct(m);
      const level = usageLevel(pct);
      const hasKey = m.access !== 'web' && !!this.svc.keyFor(m);
      const ready = m.enabled && m.access !== 'web' && hasKey;
      return {
        m, pct, level, hasKey,
        measurable: m.limit > 0,
        status: statusLabel(m, pct, hasKey),
        statusClass: !ready ? (m.access === 'web' ? 'off' : m.enabled ? 'hi' : 'off') : level === 'mid' ? 'lo' : level,
        usage: usageText(m),
        resetIn: resetText(m, now),
      };
    });
  });

  protected readonly providers = computed(() =>
    [...new Set(this.svc.models().map((m) => m.provider))].sort((a, b) => a.localeCompare(b, 'ru')),
  );

  /** Модели после фильтров по доступу, провайдеру и поиску — от них считаются счётчики навыков. */
  private readonly base = computed(() => {
    const a = this.access();
    const p = this.provider();
    const q = this.query().trim().toLowerCase();
    return this.items().filter(
      (it) =>
        (a === 'all' || (a === 'web' ? it.m.access === 'web' : a === 'api' ? it.m.access !== 'web' : it.hasKey)) &&
        (!p || it.m.provider === p) &&
        (!q || it.m.name.toLowerCase().includes(q) || it.m.provider.toLowerCase().includes(q)),
    );
  });

  protected readonly total = computed(() => this.base().length);

  protected readonly counts = computed(() => {
    const map = new Map<SkillId, number>();
    for (const { m } of this.base()) for (const s of m.skills) map.set(s, (map.get(s) ?? 0) + 1);
    return map;
  });

  protected readonly filtered = computed(() => {
    const f = this.filter();
    // Сначала то, что роутер может использовать, затем API без ключа, затем «только сайт».
    const rank = (it: ModelView) => (it.hasKey ? 0 : it.m.access === 'web' ? 2 : 1);
    return this.base()
      .filter(({ m }) => f === 'all' || m.skills.includes(f))
      .sort((a, b) => rank(a) - rank(b));
  });

  protected readonly visible = computed(() => this.filtered().slice(0, this.limit()));

  constructor() {
    effect(() => saveJson('iai.modelsView', this.view()));
    // При смене фильтров начинаем показ с первой страницы
    effect(() => {
      this.filter();
      this.access();
      this.provider();
      this.query();
      this.limit.set(PAGE);
    });
  }

  protected more(): void {
    this.limit.update((n) => n + PAGE);
  }

  protected openMenu(id: string, e: Event): void {
    e.stopPropagation();
    this.menuFor.set(this.menuFor() === id ? null : id);
  }

  protected toggleEnabled(m: AiModel): void {
    this.svc.update(m.id, { enabled: !m.enabled });
    this.menuFor.set(null);
  }

  protected reset(m: AiModel): void {
    this.svc.resetUsage(m.id);
    this.menuFor.set(null);
    this.flash(`Счётчик ${m.name} сброшен`);
  }

  protected remove(m: AiModel): void {
    this.svc.remove(m.id);
    this.menuFor.set(null);
    this.flash(`${m.name} удалена`);
  }

  protected onAdded(m: AiModel): void {
    this.flash(`${m.name} добавлена`);
  }

  private flash(text: string): void {
    this.toast.set(text);
    setTimeout(() => this.toast() === text && this.toast.set(null), 2200);
  }
}
