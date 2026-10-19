import { Injectable, inject } from '@angular/core';
import { ModelsService } from './models.service';
import { SettingsService } from './settings.service';
import { AiModel, SkillId } from './types';
import { usagePct } from './utils';

/**
 * Клиентская копия логики умного роутера — нужна, чтобы интерфейс работал до появления бэкенда.
 * Когда появится сервер на C, выбор модели будет приходить в ответе API, а этот сервис
 * останется только для подсказок в интерфейсе.
 */
@Injectable({ providedIn: 'root' })
export class AiRouterService {
  private readonly models = inject(ModelsService);
  private readonly settings = inject(SettingsService);

  /** Может ли модель ещё принимать запросы с учётом порога переключения. */
  isAvailable(m: AiModel | undefined): boolean {
    if (!m || !m.enabled || m.access === 'web' || !this.models.keyFor(m)) return false;
    if (!m.limit) return true; // лимит не опубликован — считаем доступной, пока провайдер не вернёт отказ
    const s = this.settings.settings();
    const limit = s.autoSwitch ? s.threshold : 100;
    return usagePct(m) < limit;
  }

  /** Выбрать лучшую модель для навыка, исключая указанные. */
  pick(skill: SkillId | null, exclude: string[] = []): AiModel | null {
    const pool = this.models.models().filter((m) => !exclude.includes(m.id) && this.isAvailable(m));
    if (!pool.length) return null;
    const matching = skill ? pool.filter((m) => m.skills.includes(skill)) : pool;
    const list = matching.length ? matching : pool;
    const strategy = this.settings.settings().strategy;
    const sorted = [...list].sort((a, b) => {
      if (strategy === 'quality') return a.priority - b.priority || usagePct(a) - usagePct(b);
      if (strategy === 'cheap') return b.priority - a.priority || usagePct(a) - usagePct(b);
      return usagePct(a) - usagePct(b);
    });
    return sorted[0];
  }

  /**
   * Какая модель ответит в чате.
   * mode = 'auto' — роутер каждый раз заново выбирает лучшую модель под специализацию;
   * mode = id модели — используется она, а при исчерпании лимита (если включено
   * автопереключение) роутер подменяет её другой.
   * switchedFrom — модель, с которой пришлось уйти из-за лимита (для плашки в чате).
   */
  resolve(mode: string, skill: SkillId | null, lastModelId: string | null): { model: AiModel | null; switchedFrom: AiModel | null } {
    const pinned = mode !== 'auto' ? this.models.get(mode) : undefined;
    if (pinned) {
      if (this.isAvailable(pinned)) return { model: pinned, switchedFrom: null };
      if (!this.settings.settings().autoSwitch) return { model: null, switchedFrom: null };
      const next = this.pick(skill, [pinned.id]);
      return { model: next, switchedFrom: next ? pinned : null };
    }
    const model = this.pick(skill);
    const last = this.models.get(lastModelId);
    const switchedFrom = last && model && last.id !== model.id && !this.isAvailable(last) ? last : null;
    return { model, switchedFrom };
  }
}
