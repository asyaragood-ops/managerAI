import { ChangeDetectionStrategy, Component, computed, inject, signal } from '@angular/core';
import { FormsModule } from '@angular/forms';
import { BackendService, ServerKey } from '../../core/backend.service';
import { ModelsService } from '../../core/models.service';
import { DEFAULT_SETTINGS, SettingsService } from '../../core/settings.service';
import { RouterSettings, RouterStrategy } from '../../core/types';
import { maskKey, providerColor, usagePct } from '../../core/utils';
import { AddModelDialog } from '../../shared/add-model-dialog';
import { Icon } from '../../shared/icon';

type BoolKey = { [K in keyof RouterSettings]: RouterSettings[K] extends boolean ? K : never }[keyof RouterSettings];

@Component({
  selector: 'app-settings-page',
  imports: [FormsModule, Icon, AddModelDialog],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './settings-page.html',
  styleUrl: './settings-page.css',
})
export class SettingsPage {
  protected readonly models = inject(ModelsService);
  private readonly settingsSvc = inject(SettingsService);
  protected readonly backend = inject(BackendService);
  protected readonly saving = signal<string | null>(null);

  protected readonly maskKey = maskKey;

  /** Черновик настроек: применяется по кнопке «Сохранить». */
  protected readonly draft = signal<RouterSettings>({ ...this.settingsSvc.settings() });
  protected readonly dirty = computed(() => JSON.stringify(this.draft()) !== JSON.stringify(this.settingsSvc.settings()));

  protected readonly revealed = signal<Set<string>>(new Set());
  protected readonly editing = signal<string | null>(null);
  protected readonly keyDraft = signal('');
  protected readonly dialogOpen = signal(false);
  protected readonly toast = signal<string | null>(null);

  protected readonly strategies: { id: RouterStrategy; label: string; hint: string }[] = [
    { id: 'cheap', label: 'Дешевле', hint: 'Бесплатные модели по порядку каскада, платные — в последнюю очередь' },
    { id: 'free', label: 'Свободнее', hint: 'Бесплатная модель с наибольшим запасом лимита, платные — в конце' },
    { id: 'quality', label: 'Качественнее', hint: 'Сначала самые сильные модели, даже платные' },
  ];

  protected readonly strategyHint = computed(() => this.strategies.find((s) => s.id === this.draft().strategy)?.hint ?? '');

  protected readonly toggles: { key: BoolKey; title: string; hint: string }[] = [
    { key: 'keepContext', title: 'Сохранять контекст чата', hint: 'Передавать резервной модели всю переписку; выключено — только последнее сообщение' },
    { key: 'notifySwitch', title: 'Уведомлять о переключении', hint: 'Показывать плашку в чате, когда роутер сменил модель' },
    { key: 'encryptKeys', title: 'Шифровать ключи на сервере', hint: '' },
  ];

  /** Подсказка к переключателю шифрования: зависит от состояния бэкенда. */
  protected readonly encryptHint = computed(() => {
    if (this.backend.online() !== true) return 'Выполняется бэкендом — запустите его, чтобы включить';
    if (this.backend.encryptionAvailable() === false) return 'Недоступно: сервер не смог создать мастер-ключ (см. лог бэкенда)';
    return 'AES-256-GCM в базе бэкенда; мастер-ключ в backend/data/master.key' + (navigator.userAgent.includes('Windows') ? ', защищён Windows (DPAPI)' : '');
  });

  /** Подсказка формата ключа для провайдеров с особым форматом. */
  protected keyPlaceholder(provider: string): string {
    return provider === 'Cloudflare Workers AI' ? 'ACCOUNT_ID:API_TOKEN' : 'Вставьте API-ключ';
  }

  protected readonly pq = signal('');
  protected readonly onlyKeyed = signal(false);

  /** Провайдеры с доступом по API (из каталога и из каскада бэкенда): один ключ на провайдера. */
  private readonly allProviders = computed(() => {
    const keys = this.models.providerKeys();
    const server = this.backend.keys();
    const online = this.backend.online() === true;
    type Row = { name: string; color: string; count: number; key: string; url: string; out: boolean; cascade: string[] };
    const map = new Map<string, Row>();
    for (const m of this.models.models()) {
      if (m.access === 'web') continue;
      const row = map.get(m.provider) ?? { name: m.provider, color: m.color, count: 0, key: keys[m.provider] ?? '', url: m.url ?? '', out: true, cascade: [] };
      row.count++;
      if (!row.key && m.apiKey) row.key = m.apiKey;
      if (usagePct(m) < 100) row.out = false;
      map.set(m.provider, row);
    }
    // Провайдеры каскада бэкенда, которых нет в каталоге (например, платные OpenAI и Anthropic)
    for (const sk of Object.values(server)) {
      const row = map.get(sk.provider) ?? { name: sk.provider, color: providerColor(sk.provider), count: 0, key: keys[sk.provider] ?? '', url: '', out: false, cascade: [] };
      row.cascade = sk.models;
      map.set(sk.provider, row);
    }
    return [...map.values()]
      .map((r) => ({ ...r, ...this.keyStatus(r, server[r.name], online) }))
      .sort(
        (a, b) =>
          Number(!a.key && !a.onServer) - Number(!b.key && !b.onServer) ||
          b.cascade.length - a.cascade.length || b.count - a.count || a.name.localeCompare(b.name),
      );
  });

  protected readonly keyedCount = computed(() => this.allProviders().filter((p) => p.key || p.onServer).length);

  protected readonly providerRows = computed(() => {
    const q = this.pq().trim().toLowerCase();
    return this.allProviders().filter((p) => (!this.onlyKeyed() || p.key || p.onServer) && (!q || p.name.toLowerCase().includes(q)));
  });

  protected plural(n: number): string {
    const d = n % 10, h = n % 100;
    if (d === 1 && h !== 11) return 'модель';
    if (d >= 2 && d <= 4 && (h < 12 || h > 14)) return 'модели';
    return 'моделей';
  }

  /** Статус ключа: есть ли он на сервере и откуда. Сам ключ браузер не видит — только подсказку «…a1B2». */
  private keyStatus(
    p: { key: string; out: boolean },
    sk: ServerKey | undefined,
    online: boolean,
  ): { cls: string; label: string; onServer: boolean; hint: string } {
    const onServer = !!sk?.set;
    const hint = sk?.set ? (sk.source === 'env' ? `общий ключ сервера из backend/.env (${sk.hint})` : `ваш ключ на сервере (${sk.hint})`) : '';
    if (!online && !onServer) return { cls: 'off', label: 'Нет связи', onServer, hint };
    if (!onServer) return { cls: 'off', label: 'Нет ключа', onServer, hint };
    if (p.out) return { cls: 'out', label: 'Лимит', onServer, hint };
    return { cls: '', label: sk?.source === 'env' ? 'Из .env' : 'На сервере', onServer, hint };
  }

  protected set<K extends keyof RouterSettings>(key: K, value: RouterSettings[K]): void {
    this.draft.update((d) => ({ ...d, [key]: value }));
  }

  protected flip(key: BoolKey): void {
    this.set(key, !this.draft()[key]);
  }

  protected reveal(id: string): void {
    this.revealed.update((s) => {
      const n = new Set(s);
      if (n.has(id)) n.delete(id);
      else n.add(id);
      return n;
    });
  }

  protected startEdit(provider: string): void {
    this.editing.set(provider);
    this.keyDraft.set(''); // сохранённый ключ сервер не отдаёт — вводится заново целиком
  }

  /** Ключ уходит на бэкенд и хранится там (только для вашего аккаунта); в браузере его копии нет. */
  protected async saveKey(provider: string, remove = false): Promise<void> {
    const key = remove ? '' : this.keyDraft().trim();
    if (!key && !remove) {
      this.editing.set(null); // пустое поле — просто отмена; удаляет ключ только кнопка с корзиной
      return;
    }
    this.saving.set(provider);
    const res = await this.backend.setKey(provider, key);
    this.saving.set(null);
    if (res.error) {
      this.flash(`Ключ не принят: ${res.error}`, 4000);
      return;
    }
    if (!res.saved) {
      this.flash('Бэкенд не отвечает — ключ не сохранён. Запустите бэкенд и повторите', 4000);
      return;
    }
    this.keyDraft.set('');
    this.editing.set(null);
    this.flash(key ? `Ключ ${provider} сохранён на сервере` : `Ключ ${provider} удалён`);
  }

  /** Удалить ключ провайдера с сервера. */
  protected async removeKey(provider: string): Promise<void> {
    await this.saveKey(provider, true);
  }

  /** Дневной лимит токенов: пресеты и произвольное число (0 — без лимита). */
  protected readonly limitPresets = [100_000, 500_000, 1_000_000, 5_000_000, 0];

  protected limitLabel(n: number): string {
    return n === 0 ? 'Без лимита' : n >= 1_000_000 ? `${n / 1_000_000} млн` : `${n / 1000} тыс.`;
  }

  protected setLimit(raw: string | number): void {
    const n = Math.max(0, Math.min(10_000_000_000, Math.round(Number(raw) || 0)));
    this.set('dailyTokenLimit', n);
  }

  /** Настройки применяет бэкенд-роутер; без бэкенда они сохраняются в браузере и отправятся при запуске. */
  protected async save(): Promise<void> {
    this.saving.set('settings');
    const res = await this.backend.saveSettings(this.draft());
    this.saving.set(null);
    if (res.error) {
      this.flash(`Не сохранено: ${res.error}`, 4000);
      return;
    }
    this.draft.set({ ...this.settingsSvc.settings() });
    this.flash(res.saved ? 'Настройки сохранены и применены роутером' : 'Бэкенд не запущен — настройки сохранены в браузере и применятся при запуске');
  }

  protected cancel(): void {
    this.draft.set({ ...this.settingsSvc.settings() });
  }

  protected resetDefaults(): void {
    this.draft.set({ ...DEFAULT_SETTINGS });
  }

  private flash(text: string, ms = 2200): void {
    this.toast.set(text);
    setTimeout(() => this.toast() === text && this.toast.set(null), ms);
  }
}
