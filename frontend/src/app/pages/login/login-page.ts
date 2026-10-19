import { ChangeDetectionStrategy, Component, computed, inject, signal } from '@angular/core';
import { AuthService } from '../../core/auth.service';
import { ThemeService } from '../../core/theme.service';
import { Icon } from '../../shared/icon';

type Mode = 'login' | 'register';

/** Вход и регистрация. У каждого пользователя свои API-ключи, настройки, чаты и статистика. */
@Component({
  selector: 'app-login-page',
  imports: [Icon],
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <div class="center">
      <section class="card glass" aria-labelledby="auth-title">
        <div class="brand">
          <span class="logo-xl">∞</span>
          <h1 id="auth-title">infinity<b class="grad-text">AI</b></h1>
          <p class="muted">Один чат — сотни моделей. Ваши ключи и лимиты остаются вашими.</p>
        </div>

        @if (auth.state() === 'offline') {
          <div class="note warn" role="alert">
            <app-icon name="alert" />
            <div>
              <b>Бэкенд не отвечает</b>
              <span>Запустите его (<code>./build/infinity-router</code> в папке backend) и обновите страницу.</span>
            </div>
          </div>
          <button type="button" class="btn primary block" (click)="reload()"><app-icon name="refresh" />Проверить ещё раз</button>
        } @else {
          @if (auth.registration()) {
            <div class="seg full" role="tablist" aria-label="Вход или регистрация">
              <button type="button" role="tab" [class.on]="mode() === 'login'" [attr.aria-selected]="mode() === 'login'" (click)="switchMode('login')">Вход</button>
              <button type="button" role="tab" [class.on]="mode() === 'register'" [attr.aria-selected]="mode() === 'register'" (click)="switchMode('register')">Регистрация</button>
            </div>
          }

          <form class="form" (submit)="$event.preventDefault(); submit()" novalidate>
            <label class="field">
              <span>Логин</span>
              <input class="input" name="username" autocomplete="username" autocapitalize="off" spellcheck="false" required
                     [value]="login()" (input)="login.set($any($event.target).value)" [class.invalid]="!!loginError()" placeholder="например, anna" />
              @if (loginError()) { <small class="err">{{ loginError() }}</small> }
            </label>
            <label class="field">
              <span>Пароль</span>
              <div class="input-wrap">
                <input class="input" name="password" required [type]="showPw() ? 'text' : 'password'"
                       [attr.autocomplete]="mode() === 'register' ? 'new-password' : 'current-password'"
                       [value]="password()" (input)="password.set($any($event.target).value)" [class.invalid]="!!pwError()" />
                <button type="button" class="ib" (click)="showPw.set(!showPw())" [attr.aria-label]="showPw() ? 'Скрыть пароль' : 'Показать пароль'">
                  <app-icon [name]="showPw() ? 'eyeOff' : 'eye'" />
                </button>
              </div>
              @if (pwError()) { <small class="err">{{ pwError() }}</small> }
              @else if (mode() === 'register') { <small class="muted">Не короче 8 символов</small> }
            </label>
            @if (mode() === 'register') {
              <label class="field">
                <span>Пароль ещё раз</span>
                <input class="input" name="password2" required autocomplete="new-password" [type]="showPw() ? 'text' : 'password'"
                       [value]="password2()" (input)="password2.set($any($event.target).value)" [class.invalid]="!!pw2Error()" />
                @if (pw2Error()) { <small class="err">{{ pw2Error() }}</small> }
              </label>
            }

            @if (error()) { <p class="note bad" role="alert"><app-icon name="alert" /><span>{{ error() }}</span></p> }

            <button type="submit" class="btn primary block" [disabled]="busy()">
              <app-icon [name]="mode() === 'login' ? 'lock' : 'user'" />
              {{ busy() ? 'Подождите…' : mode() === 'login' ? 'Войти' : 'Создать аккаунт' }}
            </button>
          </form>

          @if (mode() === 'register') {
            <p class="fine muted">Пароль хранится только в виде хеша. API-ключи, чаты и статистика каждого пользователя видны только ему.</p>
          } @else if (!auth.registration()) {
            <p class="fine muted">Регистрация на этом сервере закрыта — аккаунт выдаёт администратор.</p>
          }
        }
      </section>

      <div class="seg theme" role="group" aria-label="Тема оформления">
        <button type="button" [class.on]="theme.theme() === 'light'" (click)="theme.set('light')" aria-label="Светлая тема"><app-icon name="sun" /></button>
        <button type="button" [class.on]="theme.theme() === 'dark'" (click)="theme.set('dark')" aria-label="Тёмная тема"><app-icon name="moon" /></button>
      </div>
    </div>
  `,
  styles: `
    .center { min-height: 100vh; display: grid; place-items: center; align-content: center; gap: 18px; padding: 24px 16px; }
    .card { width: min(420px, 100%); padding: 32px 28px 26px; border-radius: 28px; box-shadow: var(--sh-lg); display: flex; flex-direction: column; gap: 20px; animation: pop .3s var(--ease); }
    .brand { text-align: center; display: flex; flex-direction: column; align-items: center; gap: 8px; }
    .logo-xl { width: 60px; height: 60px; border-radius: 20px; display: grid; place-items: center; font-size: 34px; font-weight: 700; color: #fff; background: var(--grad); box-shadow: var(--sh-acc); }
    h1 { font-size: 26px; font-weight: 700; letter-spacing: -.02em; }
    .brand p { font-size: 13.5px; max-width: 300px; }
    .form { display: flex; flex-direction: column; gap: 14px; }
    .field > span { font-size: 13px; font-weight: 600; }
    .note { display: flex; gap: 10px; align-items: flex-start; padding: 12px 14px; border-radius: 14px; font-size: 13px; }
    .note div { display: flex; flex-direction: column; gap: 2px; }
    .note.warn { background: color-mix(in srgb, var(--warn) 14%, transparent); }
    .note.warn .i { color: var(--warn); }
    .note.bad { background: color-mix(in srgb, var(--bad) 12%, transparent); color: var(--tx); }
    .note.bad .i { color: var(--bad); }
    .note code { font-family: var(--mono); font-size: 12px; }
    .btn.block { height: 46px; font-size: 15px; }
    .fine { font-size: 12px; text-align: center; line-height: 1.5; }
    .theme button { padding: 7px 10px; }
  `,
})
export class LoginPage {
  protected readonly auth = inject(AuthService);
  protected readonly theme = inject(ThemeService);

  protected readonly mode = signal<Mode>('login');
  protected readonly login = signal('');
  protected readonly password = signal('');
  protected readonly password2 = signal('');
  protected readonly showPw = signal(false);
  protected readonly busy = signal(false);
  protected readonly error = signal<string | null>(null);
  /** Подсвечивать ошибки полей только после первой попытки отправки. */
  private readonly tried = signal(false);

  protected readonly loginError = computed(() => {
    if (!this.tried()) return null;
    const v = this.login().trim();
    if (v.length < 3) return 'Не короче 3 символов';
    if (v.length > 48) return 'Не длиннее 48 символов';
    if (!/^[A-Za-z0-9._@+-]+$/.test(v)) return 'Латинские буквы, цифры и . _ - @ +';
    return null;
  });
  protected readonly pwError = computed(() => {
    if (!this.tried()) return null;
    const v = this.password();
    if (!v) return 'Введите пароль';
    if (this.mode() === 'register' && v.length < 8) return 'Не короче 8 символов';
    return null;
  });
  protected readonly pw2Error = computed(() =>
    this.tried() && this.mode() === 'register' && this.password2() !== this.password() ? 'Пароли не совпадают' : null,
  );

  protected switchMode(m: Mode): void {
    this.mode.set(m);
    this.error.set(null);
    this.tried.set(false);
  }

  protected async submit(): Promise<void> {
    this.tried.set(true);
    this.error.set(null);
    if (this.loginError() || this.pwError() || this.pw2Error()) return;
    this.busy.set(true);
    const res = this.mode() === 'login'
      ? await this.auth.login(this.login(), this.password())
      : await this.auth.register(this.login(), this.password());
    this.busy.set(false); // при успехе страница перезагружается сама
    if (res.error) this.error.set(res.error.charAt(0).toUpperCase() + res.error.slice(1));
  }

  protected reload(): void {
    window.location.reload();
  }
}
