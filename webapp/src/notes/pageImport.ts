import DOMPurify from 'dompurify';
import { Marked } from 'marked';

import type { NotePage } from '../api/client';
import { canonicalNoteLink } from './editor';

export function promptPageUrl(text: string): string | null {
  return canonicalNoteLink(text);
}

const markdown = new Marked({
  async: false,
  renderer: { image: () => '' },
});

export function notePageHtml(page: NotePage): string {
  const rendered = markdown.parse(page.markdown);
  if (typeof rendered !== 'string') throw new TypeError('Expected synchronous Markdown.');
  const body = document.createElement('div');
  body.innerHTML = DOMPurify.sanitize(rendered, {
    ALLOWED_TAGS: ['h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'p', 'strong', 'b', 'em', 'i',
      'u', 'ul', 'ol', 'li', 'br', 'a', 'pre', 'code', 'blockquote'],
    ALLOWED_ATTR: ['href', 'start'],
    ALLOW_DATA_ATTR: false,
  });
  for (const link of body.querySelectorAll('a')) {
    const destination = link.getAttribute('href');
    if (!destination) continue;
    try {
      const href = canonicalNoteLink(new URL(destination, page.url).href);
      if (href) link.setAttribute('href', href);
      else link.removeAttribute('href');
    } catch {
      link.removeAttribute('href');
    }
  }
  if (!body.textContent?.trim()) throw new Error('The page has no readable text.');
  const title = page.title.trim();
  if (title) {
    const first = body.firstElementChild;
    if (first?.matches('h1, h2, h3, h4, h5, h6') && first.textContent?.trim() === title) {
      first.remove();
    }
    const heading = document.createElement('h1');
    heading.textContent = title;
    body.prepend(heading);
  }
  return body.innerHTML;
}
