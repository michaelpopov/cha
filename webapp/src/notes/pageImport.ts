import DOMPurify from 'dompurify';
import { Marked } from 'marked';

import type { NotePage } from '../api/client';
import { canonicalNoteLink } from './editor';

// Linkify reads file names such as README.md as domains, so require a scheme or www.
export function promptPageUrl(text: string): string | null {
  return /^(?:https?:\/\/|www\.)/iu.test(text.trim()) ? canonicalNoteLink(text) : null;
}

// Note names have at most 200 characters and no line breaks; keep room for a " N" suffix.
export function notePageName(title: string): string {
  const name = title.replace(/[\s\p{Cc}]+/gu, ' ').trim();
  return Array.from(name).slice(0, 190).join('').trimEnd();
}

const markdown = new Marked({ async: false });

export function notePageHtml(page: NotePage): string {
  const rendered = markdown.parse(page.markdown);
  if (typeof rendered !== 'string') throw new TypeError('Expected synchronous Markdown.');
  const body = document.createElement('div');
  body.innerHTML = DOMPurify.sanitize(rendered, {
    ALLOWED_TAGS: ['h1', 'h2', 'h3', 'h4', 'h5', 'h6', 'p', 'strong', 'b', 'em', 'i',
      'u', 'ul', 'ol', 'li', 'br', 'a', 'pre', 'code', 'blockquote', 'img'],
    // The code block reads its language from the class of <code>.
    ALLOWED_ATTR: ['href', 'start', 'class', 'src', 'alt', 'title'],
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
  for (const image of body.querySelectorAll('img')) {
    try {
      const src = new URL(image.getAttribute('src') ?? '', page.url);
      if (['http:', 'https:', 'data:'].includes(src.protocol)) image.setAttribute('src', src.href);
      else image.remove();
    } catch {
      image.remove();
    }
  }
  // Images are block nodes in the editor; an image-only paragraph would add a blank line.
  for (const paragraph of body.querySelectorAll('p')) {
    if (paragraph.querySelector('img') && !paragraph.textContent?.trim()) {
      paragraph.replaceWith(...paragraph.childNodes);
    }
  }
  if (!body.textContent?.trim() && !body.querySelector('img')) throw new Error('The page has no readable text.');
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
