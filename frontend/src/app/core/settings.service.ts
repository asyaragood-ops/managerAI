import { Injectable, signal } from '@angular/core';
import { RouterSettings } from './types';
import { loadJson, saveJson } from './utils';

const KEY = 'iai.settings';

export const DEFAULT_SETTINGS: RouterSettings = {
  autoSwitch: true,
  threshold: 90,
  strategy: 'free',
  keepContext: true,
  notifySwitch: true,
  encryptKeys: false,
  dailyTokenLimit: 1_000_000,
};

@Injectable({ providedIn: 'root' })
export class SettingsService {
  readonly settings = signal<RouterSettings>({ ...DEFAULT_SETTINGS, ...loadJson<Partial<RouterSettings>>(KEY, {}) });

  save(next: RouterSettings): void {
    this.settings.set({ ...next });
    saveJson(KEY, next);
  }
}
