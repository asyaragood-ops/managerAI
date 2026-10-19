import { Injectable, effect, signal } from '@angular/core';

export type Theme = 'light' | 'dark';

@Injectable({ providedIn: 'root' })
export class ThemeService {
  /** Начальное значение уже выставлено скриптом в index.html. */
  readonly theme = signal<Theme>(document.documentElement.dataset['theme'] === 'light' ? 'light' : 'dark');

  constructor() {
    effect(() => {
      const t = this.theme();
      document.documentElement.dataset['theme'] = t;
      try {
        localStorage.setItem('iai.theme', t);
      } catch {
        /* ignore */
      }
    });
  }

  set(t: Theme): void {
    this.theme.set(t);
  }
}
