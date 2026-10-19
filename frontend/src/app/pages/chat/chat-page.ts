import { ChangeDetectionStrategy, Component, ElementRef, computed, effect, inject, signal, viewChild } from '@angular/core';
import { AiRouterService } from '../../core/ai-router.service';
import { ChatService } from '../../core/chat.service';
import { ModelsService } from '../../core/models.service';
import { Chat, SKILLS, SkillId, skillLabel, MediaKind } from '../../core/types';
import { relativeDay, timeLabel, usagePct } from '../../core/utils';
import { Icon } from '../../shared/icon';
import { MarkdownPipe } from '../../shared/markdown.pipe';
import { ModelPicker } from '../../shared/model-picker';

@Component({
  selector: 'app-chat-page',
  imports: [Icon, MarkdownPipe, ModelPicker],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './chat-page.html',
  styleUrl: './chat-page.css',
})
export class ChatPage {
  protected readonly chats = inject(ChatService);
  protected readonly models = inject(ModelsService);
  private readonly router = inject(AiRouterService);

  protected readonly skills = SKILLS;
  protected readonly skillLabel = skillLabel;
  protected readonly timeLabel = timeLabel;
  protected readonly usagePct = usagePct;

  protected readonly draft = signal('');

  /** Подпись под файлом. */
  protected readonly mediaWord: Record<MediaKind, string> = { image: 'Изображение', audio: 'Аудио', video: 'Видео' };
  /** Сообщения, чей файл не загрузился (бэкенд выключен или файл удалён). */
  protected readonly broken = signal<Set<string>>(new Set());

  protected markBroken(id: string): void {
    this.broken.update((s) => new Set(s).add(id));
  }

  protected ext(mime: string): string {
    return ({ 'image/jpeg': 'jpg', 'image/png': 'png', 'image/webp': 'webp', 'audio/mpeg': 'mp3', 'audio/wav': 'wav', 'video/mp4': 'mp4' } as Record<string, string>)[mime] ?? 'bin';
  }
  protected readonly search = signal('');
  protected readonly listOpen = signal(false);

  private readonly scroller = viewChild<ElementRef<HTMLElement>>('scroller');
  private readonly input = viewChild<ElementRef<HTMLTextAreaElement>>('input');

  protected readonly groups = computed(() => {
    const q = this.search().trim().toLowerCase();
    const now = this.models.now();
    const today = new Date(now).toDateString();
    const list = this.chats.sorted().filter((c) => !q || c.title.toLowerCase().includes(q));
    const toItem = (c: Chat) => ({ c, model: this.models.get(c.modelId)?.name ?? 'Авто', when: relativeDay(c.updatedAt, now) });
    return [
      { title: 'Сегодня', items: list.filter((c) => new Date(c.updatedAt).toDateString() === today).map(toItem) },
      { title: 'Ранее', items: list.filter((c) => new Date(c.updatedAt).toDateString() !== today).map(toItem) },
    ].filter((g) => g.items.length);
  });

  /** Модель, которая ответит на следующее сообщение (для подсказки под полем ввода). */
  protected readonly nextModel = computed(() => {
    const c = this.chats.active();
    return c ? this.router.resolve(c.mode, c.skill, c.modelId).model : this.router.pick(null);
  });

  /** Подпись для авто-режима: какую модель выбрал бы роутер. */
  protected readonly autoName = computed(() => {
    const c = this.chats.active();
    return !c || c.mode === 'auto' ? (this.nextModel()?.name ?? null) : null;
  });

  protected readonly suggestions: { text: string; skill: SkillId }[] = [
    { text: 'Напиши сценарий 30-секундного рекламного ролика', skill: 'scripts' },
    { text: 'Сделай таблицу расходов на месяц по категориям', skill: 'sheets' },
    { text: 'Найди ошибку в функции на TypeScript', skill: 'code' },
    { text: 'Придумай 5 концепций логотипа для кофейни', skill: 'design' },
    { text: 'Составь контент-план для Instagram на неделю', skill: 'marketing' },
    { text: 'Напиши 3 варианта рекламного объявления', skill: 'marketing' },
  ];

  constructor() {
    // Прокрутка вниз при новых сообщениях
    effect(() => {
      this.chats.active()?.messages.length;
      this.chats.typing();
      setTimeout(() => {
        const el = this.scroller()?.nativeElement;
        if (el) el.scrollTo({ top: el.scrollHeight, behavior: 'smooth' });
      });
    });
  }

  protected select(id: string): void {
    this.chats.activeId.set(id);
    this.listOpen.set(false);
  }

  protected newChat(): void {
    this.chats.create();
    this.listOpen.set(false);
    setTimeout(() => this.input()?.nativeElement.focus());
  }

  protected setSkill(value: string): void {
    const c = this.chats.active() ?? this.chats.create();
    this.chats.patch(c.id, { skill: value ? (value as SkillId) : null });
  }

  protected setMode(value: string): void {
    const c = this.chats.active() ?? this.chats.create();
    this.chats.patch(c.id, { mode: value });
  }

  protected useSuggestion(s: { text: string; skill: SkillId }): void {
    this.setSkill(s.skill);
    this.draft.set(s.text);
    this.input()?.nativeElement.focus();
  }

  protected onKey(e: KeyboardEvent): void {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
      e.preventDefault();
      this.send();
    }
  }

  protected autosize(el: HTMLTextAreaElement): void {
    el.style.height = 'auto';
    el.style.height = Math.min(el.scrollHeight, 220) + 'px';
  }

  protected send(): void {
    if (!this.draft().trim()) return;
    this.chats.send(this.draft());
    this.draft.set('');
    const el = this.input()?.nativeElement;
    if (el) el.style.height = 'auto';
  }

  protected removeChat(id: string, e: Event): void {
    e.stopPropagation();
    this.chats.remove(id);
  }
}
