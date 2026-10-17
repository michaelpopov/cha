import type { Editor } from '@tiptap/core';
import { afterEach, describe, expect, it } from 'vitest';

import { appendNoteHtml, createNotesEditor } from './editor';
import { notePageHtml, notePageName, promptPageUrl } from './pageImport';

let editor: Editor | undefined;
afterEach(() => { editor?.destroy(); });

describe('URL imports', () => {
  it.each([
    ['  https://example.org/article?q=1#part\n', 'https://example.org/article?q=1#part'],
    ['HTTP://example.org', 'http://example.org/'],
    ['https://example.org/a%20b', 'https://example.org/a%20b'],
    ['Read https://example.org', null],
    ['https://example.org read this', null],
    ['https://example.org\nhttps://example.net', null],
    ['example.org', null],
    ['README.md', null],
    ['main.py', null],
    ['  www.example.org/article?q=1#part\n', 'http://www.example.org/article?q=1#part'],
    ['WWW.example.org', 'http://www.example.org/'],
    ['example.org/a%20b', null],
    ['example.org read this', null],
    ['example.org\nexample.net', null],
    ['reader@example.org', null],
    ['ordinary text', null],
    ['ftp://example.org', null],
    ['https://', null],
  ])('detects only a whole web address: %s', (prompt, expected) => {
    expect(promptPageUrl(prompt)).toBe(expected);
  });

  it('makes a valid note name from a page title', () => {
    expect(notePageName('  Breaking\n\tnews \u0085 today\r\n')).toBe('Breaking news today');
    expect(notePageName('\u0000\u001f')).toBe('');
    const long = notePageName(`${'😀'.repeat(189)} tail`);
    expect(Array.from(long)).toHaveLength(189);
    expect(long.endsWith(' ')).toBe(false);
  });

  it('appends a title and formatted article, resolves links, removes images, and undoes once', () => {
    editor = createNotesEditor({ content: '<p>Existing text</p>' });
    editor.commands.setTextSelection({ from: 1, to: 5 });
    const before = editor.getJSON();
    const html = notePageHtml({
      url: 'https://example.org/news/article',
      title: 'Article title',
      markdown: '# Article title\n\nSome **bold** and *italic* text. [Details](../details)\n\n'
        + '- First\n- Second\n\n![Photo](https://example.org/image.png)'
        + '<img src="https://example.org/tracker" onerror="alert(1)">'
        + '<script>alert(1)</script><p><a href="javascript:alert(1)">Unsafe</a></p>',
    });
    expect(appendNoteHtml(editor, html)).toBe(true);
    expect(editor.getJSON().content?.[0]).toEqual(before.content?.[0]);
    expect(editor.view.dom.querySelectorAll('h1')).toHaveLength(1);
    expect(editor.view.dom.querySelector('h1')).toHaveTextContent('Article title');
    expect(editor.view.dom.querySelector('strong')).toHaveTextContent('bold');
    expect(editor.view.dom.querySelector('em')).toHaveTextContent('italic');
    expect(editor.view.dom.querySelectorAll('li')).toHaveLength(2);
    expect(editor.view.dom.querySelector('a')).toHaveAttribute('href', 'https://example.org/details');
    expect(editor.view.dom.querySelector('img, script, [onerror]')).toBeNull();
    expect(editor.getHTML()).not.toContain('javascript:');
    expect(editor.getText()).not.toContain('https://example.org/news/article');
    expect(editor.commands.undo()).toBe(true);
    expect(editor.getJSON()).toEqual(before);
    expect(editor.commands.redo()).toBe(true);
    expect(editor.getText()).toContain('Article title');
  });

  it('replaces the initial empty paragraph and restores it with undo', () => {
    editor = createNotesEditor();
    const before = editor.getJSON();
    const html = notePageHtml({ url: 'https://example.org', title: 'Title', markdown: 'Article' });
    expect(appendNoteHtml(editor, html)).toBe(true);
    expect(editor.getJSON().content?.map((node) => node.type)).toEqual(['heading', 'paragraph']);
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Title\nArticle');
    expect(editor.commands.undo()).toBe(true);
    expect(editor.getJSON()).toEqual(before);
    expect(editor.commands.redo()).toBe(true);
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Title\nArticle');
  });

  it('imports fenced code as an editable code block with its whitespace intact', () => {
    editor = createNotesEditor();
    const html = notePageHtml({
      url: 'https://example.org', title: 'Example',
      markdown: '```python\ndef embed(text):\n    return model.encode(text)\n```',
    });
    expect(appendNoteHtml(editor, html)).toBe(true);
    expect(editor.isEditable).toBe(true);
    expect(editor.view.dom.querySelector('pre code')?.textContent)
      .toBe('def embed(text):\n    return model.encode(text)\n');
    expect(editor.getJSON().content?.[1]).toMatchObject({
      type: 'codeBlock', attrs: { language: 'python' },
    });
    expect(editor.commands.undo()).toBe(true);
    expect(editor.isEmpty).toBe(true);
  });

  it.each(['<p></p><p>Keep</p>', '<p></p><p></p>'])(
    'preserves existing blank paragraphs in %s', (content) => {
      editor = createNotesEditor({ content });
      const before = editor.getJSON().content ?? [];
      expect(appendNoteHtml(editor, '<h1>Title</h1><p>Article</p>')).toBe(true);
      expect(editor.getJSON().content?.slice(0, before.length)).toEqual(before);
    },
  );

  it('preserves an empty paragraph from the imported content', () => {
    editor = createNotesEditor();
    expect(appendNoteHtml(editor, '<p></p><h1>Title</h1><p>Article</p>')).toBe(true);
    expect(editor.getJSON().content?.map((node) => node.type)).toEqual(['paragraph', 'heading', 'paragraph']);
  });

  it('treats the title as text and rejects empty articles', () => {
    const html = notePageHtml({ url: 'https://example.org', title: '<img src=x>', markdown: 'Text' });
    expect(html).toContain('&lt;img src=x&gt;');
    expect(() => notePageHtml({
      url: 'https://example.org', title: 'Title', markdown: '![Only an image](https://example.org/p.png)',
    })).toThrow();
  });

  it('rejects an oversized import without changing the document or undo history', () => {
    editor = createNotesEditor({ content: '<p>Keep</p>', byteLimit: 300 });
    const before = editor.getJSON();
    expect(appendNoteHtml(editor, `<p>${'x'.repeat(400)}</p>`)).toBe(false);
    expect(editor.getJSON()).toEqual(before);
    expect(editor.can().undo()).toBe(false);
    editor.setEditable(false);
    expect(appendNoteHtml(editor, '<p>Late</p>')).toBe(false);
  });
});
