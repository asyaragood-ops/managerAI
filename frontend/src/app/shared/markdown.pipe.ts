import { Pipe, PipeTransform } from '@angular/core';

const esc = (s: string) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

/**
 * Минимальная разметка ответов: абзацы, **жирный**, `код`, цитаты «> ».
 * Текст сначала экранируется, поэтому HTML от модели не исполняется.
 */
@Pipe({ name: 'md' })
export class MarkdownPipe implements PipeTransform {
  transform(text: string): string {
    return text
      .split(/\n{2,}/)
      .map((block) => {
        const quote = block.startsWith('> ');
        const html = esc(quote ? block.slice(2) : block)
          .replace(/\*\*(.+?)\*\*/g, '<b>$1</b>')
          .replace(/`([^`]+)`/g, '<code>$1</code>')
          .replace(/\n/g, '<br>');
        return quote ? `<blockquote>${html}</blockquote>` : `<p>${html}</p>`;
      })
      .join('');
  }
}
