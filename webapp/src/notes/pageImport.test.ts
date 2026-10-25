import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import type { Editor } from '@tiptap/core';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { createNativeChaClient } from '../api/nativeClient';
import { createFakeNativeBridge } from '../api/nativeBridge';
import { appendNoteHtml as appendPictures, createNotesEditor } from './editor';
import { pictureFromBytes } from './pictures';
import { notePageHtml, notePageName, promptPageUrl } from './pageImport';
import { noteAudioBlocks } from './useNoteAudio';

let editor: Editor | undefined;
afterEach(() => { editor?.destroy(); });

const readPicture = async () => new Uint8Array();
const appendNoteHtml = (editor: Editor, html: string) => appendPictures(editor, html, readPicture, () => true);

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

  it('appends a title and formatted article, resolves links, skips failed images, and undoes once', async () => {
    editor = createNotesEditor({ content: '<p>Existing text</p>', readPicture });
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
    expect(await appendNoteHtml(editor, html)).toBe(true);
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

  it('replaces the initial empty paragraph and restores it with undo', async () => {
    editor = createNotesEditor({ readPicture });
    const before = editor.getJSON();
    const html = notePageHtml({ url: 'https://example.org', title: 'Title', markdown: 'Article' });
    expect(await appendNoteHtml(editor, html)).toBe(true);
    expect(editor.getJSON().content?.map((node) => node.type)).toEqual(['heading', 'paragraph']);
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Title\nArticle');
    expect(editor.commands.undo()).toBe(true);
    expect(editor.getJSON()).toEqual(before);
    expect(editor.commands.redo()).toBe(true);
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Title\nArticle');
  });

  it('keeps formatted page paragraphs as separate narration blocks', async () => {
    editor = createNotesEditor({ readPicture });
    const first = 'First sentence. '.repeat(80).trim();
    const second = 'Second sentence. '.repeat(80).trim();
    const html = notePageHtml({ url: 'https://example.org/article', title: 'Article',
      markdown: `# Article\n\n${first}\n\n${second} [Details](./details)` });
    expect(await appendNoteHtml(editor, html)).toBe(true);
    expect(editor.getJSON().content?.map(node => node.type)).toEqual(['heading', 'paragraph', 'paragraph']);
    expect(noteAudioBlocks(editor.state.doc).map(block => block.text))
      .toEqual(['Article', first, `${second} Details`]);
    expect(editor.view.dom.querySelector('a')).toHaveAttribute('href', 'https://example.org/details');
  });

  it('imports fenced code as an editable code block with its whitespace intact', async () => {
    editor = createNotesEditor({ readPicture });
    const html = notePageHtml({
      url: 'https://example.org', title: 'Example',
      markdown: '```python\ndef embed(text):\n    return model.encode(text)\n```',
    });
    expect(await appendNoteHtml(editor, html)).toBe(true);
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
    'preserves existing blank paragraphs in %s', async (content) => {
      editor = createNotesEditor({ content, readPicture });
      const before = editor.getJSON().content ?? [];
      expect(await appendNoteHtml(editor, '<h1>Title</h1><p>Article</p>')).toBe(true);
      expect(editor.getJSON().content?.slice(0, before.length)).toEqual(before);
    },
  );

  it('preserves an empty paragraph from the imported content', async () => {
    editor = createNotesEditor({ readPicture });
    expect(await appendNoteHtml(editor, '<p></p><h1>Title</h1><p>Article</p>')).toBe(true);
    expect(editor.getJSON().content?.map((node) => node.type)).toEqual(['paragraph', 'heading', 'paragraph']);
  });

  it('treats the title as text and rejects empty articles', () => {
    const html = notePageHtml({ url: 'https://example.org', title: '<img src=x>', markdown: 'Text' });
    expect(html).toContain('&lt;img src=x&gt;');
    expect(() => notePageHtml({
      url: 'https://example.org', title: 'Title', markdown: '   ',
    })).toThrow();
  });

  it('rejects an oversized import without changing the document or undo history', async () => {
    editor = createNotesEditor({ content: '<p>Keep</p>', byteLimit: 300, readPicture });
    const before = editor.getJSON();
    expect(await appendNoteHtml(editor, `<p>${'x'.repeat(400)}</p>`)).toBe(false);
    expect(editor.getJSON()).toEqual(before);
    expect(editor.can().undo()).toBe(false);
    editor.setEditable(false);
    expect(await appendNoteHtml(editor, '<p>Late</p>')).toBe(false);
  });

  it('embeds page pictures in place, resolves their URLs, and skips failures', async () => {
    editor = createNotesEditor({ readPicture });
    const before = editor.getJSON();
    const bytes = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const reader = vi.fn(async (url: string) => {
      if (url.endsWith('missing.png')) throw new Error('not found');
      return bytes;
    });
    const html = notePageHtml({
      url: 'https://example.org/news/article', title: 'Article',
      markdown: 'Before\n\n![Photo](../photo.png)\n\nAfter\n\n![Missing](missing.png)',
    });
    expect(await appendPictures(editor, html, reader, () => true)).toBe(true);
    expect(reader.mock.calls.map(([url]) => url)).toEqual([
      'https://example.org/photo.png', 'https://example.org/news/missing.png',
    ]);
    expect(editor.getJSON().content?.map((node) => node.type))
      .toEqual(['heading', 'paragraph', 'image', 'paragraph']);
    expect(editor.view.dom.querySelector('img[src]')).toHaveAttribute('src', (pictureFromBytes(bytes) as { src: string }).src);
    const after = editor.getJSON();
    editor.commands.undo();
    expect(editor.getJSON()).toEqual(before);
    editor.commands.redo();
    expect(editor.getJSON()).toEqual(after);
  });

  it('keeps imported text when pictures cannot fit in the note', async () => {
    editor = createNotesEditor({ byteLimit: 500, readPicture });
    const bytes = new Uint8Array(400);
    bytes.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const html = notePageHtml({ url: 'https://example.org', title: 'Title',
      markdown: 'Before\n\n![Photo](/p.png)\n\nAfter' });
    expect(await appendPictures(editor, html, async () => bytes, () => true, 500)).toBe(true);
    expect(editor.getText()).toContain('Before');
    expect(editor.getText()).toContain('After');
    expect(editor.view.dom.querySelector('img[src]')).toBeNull();
  });

  it('reports a page whose only pictures cannot be loaded', async () => {
    editor = createNotesEditor({ readPicture });
    const html = notePageHtml({ url: 'https://example.org', title: '', markdown: '![Photo](/p.png)' });
    await expect(appendNoteHtml(editor, html)).rejects.toThrow('The page has no readable text.');
    expect(editor.isEmpty).toBe(true);
  });

  it('does not insert a page when its editor context changes during a picture download', async () => {
    editor = createNotesEditor({ readPicture });
    let finish!: (bytes: Uint8Array) => void;
    let current = true;
    const pending = appendPictures(editor,
      '<p>Article</p><img src="https://example.org/p.png">',
      () => new Promise((resolve) => { finish = resolve; }), () => current);
    await vi.waitFor(() => expect(finish).toBeDefined());
    current = false;
    finish(new Uint8Array());
    expect(await pending).toBe(false);
    expect(editor.isEmpty).toBe(true);
  });

  it('imports an actual web_reader API response through the CHA client and picture importer', async () => {
    // Generated by web_reader /extract with its pictures.html fixture and real worker.
    const response = JSON.parse(readFileSync(join(dirname(fileURLToPath(import.meta.url)),
      '../../../tests/fixtures/notes/web-reader-pictures.json'), 'utf8'));
    const readPicture = vi.fn(({ url }: { url: string }) => ({
      base64: url.endsWith('/missing.png') ? ''
        : 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/9kcAAAAASUVORK5CYII=',
    }));
    const client = createNativeChaClient(createFakeNativeBridge({
      'notes.readPage': () => response,
      'notes.readPicture': (params) => readPicture(params as { url: string }),
    }));
    const page = await client.readNotePage('https://example.org/news/article');
    editor = createNotesEditor({ readPicture: client.readNotePicture });
    const before = editor.getJSON();
    expect(await appendPictures(editor, notePageHtml(page), client.readNotePicture, () => true)).toBe(true);
    expect(readPicture.mock.calls.map(([{ url }]) => url)).toEqual([
      'https://example.org/assets/first.png', 'https://example.org/assets/lazy.webp',
      'https://example.org/assets/linked.jpg', 'https://example.org/assets/missing.png',
    ]);
    const sequence: string[] = [];
    editor.state.doc.descendants((node) => {
      if (node.isText) sequence.push(node.text ?? '');
      if (node.type.name === 'image') {
        expect(node.attrs.src).toMatch(/^data:image\/png;base64,/);
        sequence.push(`[${node.attrs.alt}]`);
      }
    });
    const ordered = sequence.join(' ');
    const expected = ['Before the first picture.', '[First picture]', 'The first picture caption.',
      'Between the first and lazy pictures.', '[Lazy picture]', 'Linked text before',
      '[Linked picture]', 'linked text after.', 'Before the missing picture.', 'After all pictures.'];
    const positions = expected.map((part) => ordered.indexOf(part));
    expect(positions.every((position) => position >= 0)).toBe(true);
    expect(positions).toEqual([...positions].sort((a, b) => a - b));
    expect(editor.view.dom.querySelectorAll('img[src]')).toHaveLength(3);
    const imported = editor.getJSON();
    editor.commands.undo();
    expect(editor.getJSON()).toEqual(before);
    editor.commands.redo();
    expect(editor.getJSON()).toEqual(imported);
  });

});
