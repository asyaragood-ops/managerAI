import { ChangeDetectionStrategy, Component, inject, signal } from '@angular/core';
import { RouterLink, RouterLinkActive, RouterOutlet } from '@angular/router';
import { AuthService } from './core/auth.service';
import { BackendService } from './core/backend.service';
import { ModelsService } from './core/models.service';
import { ThemeService } from './core/theme.service';
import { Icon, IconName } from './shared/icon';

@Component({
  selector: 'app-root',
  imports: [RouterOutlet, RouterLink, RouterLinkActive, Icon],
  changeDetection: ChangeDetectionStrategy.OnPush,
  host: { '(document:click)': 'menuOpen.set(false)', '(document:keydown.escape)': 'menuOpen.set(false)' },
  templateUrl: './app.html',
  styleUrl: './app.css',
})
export class App {
  protected readonly theme = inject(ThemeService);
  protected readonly auth = inject(AuthService);
  /** Данные пользователя загружаем только после входа — до него в браузере не должно появиться чужих или общих записей. */
  protected readonly models = this.auth.state() === 'auth' ? inject(ModelsService) : null;
  /** Связь с бэкендом (ключи, настройки, каскад) — тоже только для вошедшего. */
  protected readonly backend = this.auth.state() === 'auth' ? inject(BackendService) : null;
  protected readonly menuOpen = signal(false);

  protected toggleMenu(e: Event): void {
    e.stopPropagation();
    this.menuOpen.update((v) => !v);
  }

  protected readonly nav: { path: string; label: string; icon: IconName }[] = [
    { path: '/chat', label: 'Чаты', icon: 'chat' },
    { path: '/models', label: 'Модели', icon: 'models' },
    { path: '/stats', label: 'Статистика', icon: 'stats' },
    { path: '/settings', label: 'Настройки', icon: 'settings' },
  ];
}
