import { Routes } from '@angular/router';
import { authGuard, guestGuard } from './core/auth.service';

export const routes: Routes = [
  { path: '', pathMatch: 'full', redirectTo: 'chat' },
  { path: 'login', title: 'Вход · infinityAI', canActivate: [guestGuard], loadComponent: () => import('./pages/login/login-page').then((m) => m.LoginPage) },
  { path: 'chat', title: 'Чаты · infinityAI', canActivate: [authGuard], loadComponent: () => import('./pages/chat/chat-page').then((m) => m.ChatPage) },
  { path: 'models', title: 'Модели · infinityAI', canActivate: [authGuard], loadComponent: () => import('./pages/models/models-page').then((m) => m.ModelsPage) },
  { path: 'stats', title: 'Статистика · infinityAI', canActivate: [authGuard], loadComponent: () => import('./pages/stats/stats-page').then((m) => m.StatsPage) },
  { path: 'settings', title: 'Настройки · infinityAI', canActivate: [authGuard], loadComponent: () => import('./pages/settings/settings-page').then((m) => m.SettingsPage) },
  { path: '**', redirectTo: 'chat' },
];
