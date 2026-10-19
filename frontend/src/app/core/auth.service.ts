import { Injectable, signal } from '@angular/core';
import { CanActivateFn, Router } from '@angular/router';
import { inject } from '@angular/core';
import { adoptLegacyStorage, setStorageScope } from './utils';

export interface User {
  id: number;
  login: string;
  /** Смещение часового пояса от UTC, минуты. */
  tz: number;
}

/** unknown — ещё проверяем; auth — вошёл; anon — нужно войти; offline — бэкенд не отвечает. */
export type AuthState = 'unknown' | 'auth' | 'anon' | 'offline';

interface AuthReply {
  ok: boolean;
  user?: User;
  registration?: boolean;
  error?: string;
  retry_after?: number;
}

/** Смещение часового пояса браузера: Москва → 180. Нужно, чтобы «сегодня» в статистике совпадало с календарём. */
export const browserTz = (): number => -new Date().getTimezoneOffset();

/**
 * Вход, регистрация и выход. Сессия — cookie HttpOnly, её ставит бэкенд; скрипты сайта токен не видят.
 * После входа страница перезагружается: все сервисы стартуют уже с данными этого пользователя.
 */
@Injectable({ providedIn: 'root' })
export class AuthService {
  readonly state = signal<AuthState>('unknown');
  readonly user = signal<User | null>(null);
  /** Открыта ли регистрация на этом сервере. */
  readonly registration = signal(true);

  /** Вызывается до запуска приложения (provideAppInitializer). */
  async init(): Promise<void> {
    let res: Response;
    try {
      res = await fetch(`/api/auth/me?tz=${browserTz()}`);
    } catch {
      this.state.set('offline');
      return;
    }
    const data = (await res.json().catch(() => null)) as AuthReply | null;
    if (!data || typeof data.ok !== 'boolean') {
      this.state.set('offline'); // ответ не от нашего бэкенда (например, 504 от прокси dev-сервера)
      return;
    }
    this.registration.set(data.registration !== false);
    if (!data.ok || !data.user) {
      this.state.set('anon');
      return;
    }
    setStorageScope(data.user.id);
    this.user.set(data.user);
    this.state.set('auth');
    await this.uploadLegacyKeys(adoptLegacyStorage(data.user.id));
  }

  login(login: string, password: string): Promise<{ error?: string }> {
    return this.submit('login', login, password);
  }

  register(login: string, password: string): Promise<{ error?: string }> {
    return this.submit('register', login, password);
  }

  async logout(): Promise<void> {
    await fetch('/api/auth/logout', { method: 'POST' }).catch(() => null);
    window.location.assign('/login');
  }

  /** Сессия истекла или её удалили — на страницу входа (с перезагрузкой, чтобы не осталось чужих данных в памяти). */
  onUnauthorized(): void {
    if (this.state() === 'anon') return;
    this.state.set('anon');
    window.location.assign('/login');
  }

  private async submit(kind: 'login' | 'register', login: string, password: string): Promise<{ error?: string }> {
    let res: Response;
    try {
      res = await fetch(`/api/auth/${kind}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ login: login.trim(), password, tz: browserTz() }),
      });
    } catch {
      return { error: 'Бэкенд не отвечает — запустите его и попробуйте ещё раз' };
    }
    const data = (await res.json().catch(() => null)) as AuthReply | null;
    if (!data || typeof data.ok !== 'boolean') return { error: 'Бэкенд не отвечает — запустите его и попробуйте ещё раз' };
    if (!data.ok) return { error: data.error ?? 'Не получилось' };
    window.location.assign('/chat');
    return {};
  }

  /** Ключи, которые до появления аккаунтов хранились в этом браузере, переезжают на сервер к этому пользователю. */
  private async uploadLegacyKeys(keys: Record<string, string>): Promise<void> {
    if (!Object.keys(keys).length) return;
    await fetch('/api/keys/bulk', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ keys }),
    }).catch(() => null);
  }
}

/** Разделы сайта — только после входа. */
export const authGuard: CanActivateFn = () => {
  const auth = inject(AuthService);
  return auth.state() === 'auth' ? true : inject(Router).createUrlTree(['/login']);
};

/** Страница входа — только тем, кто ещё не вошёл. */
export const guestGuard: CanActivateFn = () => {
  const auth = inject(AuthService);
  return auth.state() === 'auth' ? inject(Router).createUrlTree(['/chat']) : true;
};
