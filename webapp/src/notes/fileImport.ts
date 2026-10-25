import { ChaError, type TextFileRequest } from '../api/client';
import { noteFileNoText } from './editor';
import { notePageHtml, notePageName } from './pageImport';

export const noteFileTooLarge = 'This file is too large to load into a note. The limit is 256 KiB of text.';

export function promptFilePath(text: string): string | null {
  let path = text.trim();
  if (path.startsWith('"') && path.endsWith('"')) path = path.slice(1, -1);
  if (/[\r\n\0]/u.test(path)) return null;
  // Match desktop chat paths, including Windows "Copy as path" and UNC paths.
  // As in chat, "/word" without a second "/" is text.
  return /^(?:\/[^/]*\/|[a-z]:[\\/]|\\\\)/iu.test(path) ? path : null;
}

export function noteFileHtml(file: TextFileRequest): { html: string; title: string; preserveWhitespace: boolean } {
  const body = document.createElement('div');
  if (/\.(?:md|markdown|mdown|mkd)$/iu.test(file.name)) {
    try {
      body.innerHTML = notePageHtml({ url: 'file:///', title: '', markdown: file.text });
    } catch {
      throw new ChaError('invalid_argument', noteFileNoText);
    }
    return {
      html: body.innerHTML,
      title: notePageName(body.querySelector('h1, h2, h3, h4, h5, h6')?.textContent ?? ''),
      preserveWhitespace: false,
    };
  }
  let paragraph: HTMLParagraphElement | null = null;
  for (const line of file.text.replace(/\r\n?/gu, '\n').split('\n')) {
    if (!line.trim()) {
      paragraph = null;
      continue;
    }
    if (!paragraph) {
      paragraph = document.createElement('p');
      body.append(paragraph);
    } else {
      paragraph.append(document.createElement('br'));
    }
    paragraph.append(document.createTextNode(line));
  }
  return { html: body.innerHTML, title: '', preserveWhitespace: true };
}
