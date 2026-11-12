import { strFromU8, unzipSync } from 'fflate';
import { expect, it, vi } from 'vitest';

import type { NoteItem, NoteTextMode } from '../api/client';
import { noteDocument, noteItem, paragraph, textNode } from '../test/notes';
import { buildFolderEpub } from './epubExport';
import { prepareEpubImport } from './epubImport';

const book = noteItem({ id: 10, kind: 'folder', name: 'History <&> 史 🏛\u0001' });
const folder = (id: number, parent_id: number, name: string) => noteItem({ id, parent_id, name, kind: 'folder' });
const chapter = (id: number, parent_id: number, name: string, flags: Partial<NoteItem> = {}) =>
  noteItem({ id, parent_id, name, ...flags });
const items = [
  book, folder(11, 10, 'Rome & Greece'), folder(12, 11, 'Archives'), folder(13, 10, 'Empty'),
  chapter(14, 13, 'ANNOTATIONS', { kind: 'annotation' }),
  chapter(15, 10, 'Summary (2)', { folder_summary: true }),
  chapter(16, 10, 'Summary'),
  chapter(17, 10, 'Session summary', { is_summary: true, read_only: true }),
  chapter(18, 11, 'Summary', { folder_summary: true, has_l1: true }),
  chapter(19, 12, 'Republic', { read_only: true, summary_excluded: true }),
  chapter(20, 11, 'Republic', { has_l2: true }),
  chapter(21, 10, 'Blank'),
];

function parseXml(files: Record<string, Uint8Array>, path: string): XMLDocument {
  expect(files[path], path).toBeDefined();
  const doc = new DOMParser().parseFromString(strFromU8(files[path]), 'application/xml');
  expect(doc.querySelector('parsererror'), path).toBeNull();
  return doc;
}

it('writes a valid container and keeps navigation, spine and reads in tree order', async () => {
  const read = vi.fn(async (note: NoteItem) => noteDocument(note.id === 21 ? { type: 'paragraph' } : paragraph('Body')));
  const { title, bytes } = await buildFolderEpub(book.id, items, 'full_text', read);
  expect(title).toBe(book.name);
  expect(read.mock.calls.map(([note]) => note.id)).toEqual([15, 18, 19, 20, 21, 17, 16]);

  const header = new DataView(bytes.buffer, bytes.byteOffset);
  expect(header.getUint32(0, true)).toBe(0x04034b50);
  expect(header.getUint16(6, true) & 8).toBe(0);
  expect(header.getUint16(8, true)).toBe(0);
  expect(header.getUint16(26, true)).toBe(8);
  expect(header.getUint16(28, true)).toBe(0);
  expect(strFromU8(bytes.subarray(30, 58))).toBe('mimetypeapplication/epub+zip');

  const files = unzipSync(bytes);
  const container = parseXml(files, 'META-INF/container.xml');
  expect(container.documentElement.namespaceURI).toBe('urn:oasis:names:tc:opendocument:xmlns:container');
  const opfPath = container.querySelector('rootfile')!.getAttribute('full-path')!;
  const opf = parseXml(files, opfPath);
  expect(opf.documentElement.namespaceURI).toBe('http://www.idpf.org/2007/opf');
  expect(opf.documentElement.getAttribute('version')).toBe('3.0');
  const metadata = opf.getElementsByTagNameNS('http://purl.org/dc/elements/1.1/', '*');
  expect([...metadata].map(el => el.textContent)).toEqual([
    expect.stringMatching(/^urn:uuid:[0-9a-f-]{36}$/), 'History <&> 史 🏛�', 'und',
  ]);
  expect(metadata[0].id).toBe(opf.documentElement.getAttribute('unique-identifier'));
  expect(opf.querySelector('meta[property="dcterms:modified"]')?.textContent)
    .toMatch(/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$/);
  const manifest = [...opf.querySelectorAll('manifest > item')];
  const paths = new Map(manifest.map(el => [el.id, el.getAttribute('href')]));
  expect(paths.size).toBe(manifest.length);
  expect([...paths.values()].map(path => 'EPUB/' + path).sort())
    .toEqual(Object.keys(files).filter(path => path.startsWith('EPUB/') && path !== opfPath).sort());

  const navItem = opf.querySelector('item[properties="nav"]')!;
  const navigation = parseXml(files, 'EPUB/' + navItem.getAttribute('href'));
  expect(navigation.documentElement.getAttribute('lang')).toBe('und');
  expect(navigation.querySelector('nav')?.getAttributeNS('http://www.idpf.org/2007/ops', 'type')).toBe('toc');
  expect(strFromU8(files['EPUB/' + navItem.getAttribute('href')])).toContain('<nav epub:type="toc">');
  expect(navigation.querySelector('h1')).toBeNull();
  expect([...navigation.querySelectorAll('li > span')].map(el => el.textContent)).toEqual(['Rome & Greece', 'Archives']);
  expect(navigation.querySelector('ol > li > ol > li > ol > li > a')?.textContent).toBe('Republic');
  const links = [...navigation.querySelectorAll('nav a')];
  expect(links.map(el => el.getAttribute('href')))
    .toEqual([...opf.querySelectorAll('spine > itemref')].map(el => paths.get(el.getAttribute('idref')!)));
  expect(links.map(el => el.textContent)).toEqual(['Summary (2)', 'Summary', 'Republic', 'Republic', 'Blank', 'Session summary', 'Summary']);
  for (const link of links) {
    const doc = parseXml(files, 'EPUB/' + link.getAttribute('href'));
    expect(doc.documentElement.namespaceURI).toBe('http://www.w3.org/1999/xhtml');
    expect(doc.documentElement.getAttribute('xml:lang')).toBe('und');
    expect(doc.querySelector('title')?.textContent).toBe(link.textContent);
    expect(doc.querySelector('h1')?.textContent).toBe(link.textContent);
    expect(files['EPUB/' + doc.querySelector('link')?.getAttribute('href')]).toBeDefined();
    if (link.textContent === 'Blank') expect(doc.querySelector('body')?.textContent).toBe('Blank');
  }
});

it('keeps formatting, packages raster bytes and imports the result through CHA', async () => {
  const first = chapter(31, 10, 'Chapter <&> 😀\u0001');
  const second = chapter(32, 10, 'Next');
  const pictures = {
    png: 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jC1sAAAAASUVORK5CYII=',
    jpeg: '/9j/',
    gif: 'R0lGODlhAQABAIAAAP///wAAACH5BAEAAAAALAAAAAABAAEAAAICRAEAOw==',
    webp: 'UklGRgAAAABXRUJQ',
  };
  const formatted = noteDocument(
    { type: 'heading', attrs: { level: 1 }, content: [textNode(first.name)] },
    { type: 'heading', attrs: { level: 1 }, content: [textNode('Section')] },
    { type: 'paragraph', content: [
      textNode('Book & 章\u0001'), { type: 'hardBreak' },
      { type: 'text', text: 'Comment', marks: [{ type: 'highlight', attrs: { color: '#b3d9ff' } }] },
      { type: 'text', text: 'Web', marks: [{ type: 'link', attrs: { href: 'https://example.com/book?a=1&b=2' } }] },
      { type: 'text', text: 'Unsafe', marks: [{ type: 'link', attrs: { href: 'javascript:alert(1)' } }] },
      { type: 'text', text: 'Underlined', marks: [{ type: 'underline' }] },
    ] },
    { type: 'orderedList', attrs: { start: 7 }, content: [{ type: 'listItem', content: [paragraph('Seventh')] }] },
    { type: 'codeBlock', content: [textNode('<script> & code\n  indented')] },
    ...Object.entries(pictures).map(([ext, base64]) => ({
      type: 'image', attrs: { src: `data:image/${ext};base64,${base64}`, ...(ext === 'png' ? { alt: 'A <picture>\u0001' } : {}) },
    })),
    { type: 'image', attrs: { src: 'https://example.com/pixel.gif', alt: 'Remote picture' } },
    { type: 'image', attrs: { src: 'data:image/png;base64,bm90IGFuIGltYWdl' } },
  );
  const { bytes } = await buildFolderEpub(book.id, [book, first, second], 'full_text', async note =>
    note.id === first.id ? formatted : noteDocument(paragraph('Second chapter')));
  const files = unzipSync(bytes);
  const doc = parseXml(files, 'EPUB/chapter-0001.xhtml');
  expect(doc.querySelectorAll('h1')).toHaveLength(1);
  expect(doc.querySelector('h1')?.textContent).toBe('Chapter <&> 😀�');
  expect(doc.querySelector('h2')?.textContent).toBe('Section');
  expect(doc.querySelector('p')?.textContent).toContain('Book & 章�');
  expect(doc.querySelector('br')).not.toBeNull();
  expect(doc.querySelector('u')?.textContent).toBe('Underlined');
  expect(doc.querySelector('mark[data-color]')?.textContent).toBe('Comment');
  expect(doc.querySelector('ol')?.getAttribute('start')).toBe('7');
  expect(doc.querySelector('pre > code')?.textContent).toBe('<script> & code\n  indented');
  expect([...doc.querySelectorAll('a')].map(a => a.getAttribute('href'))).toEqual(['https://example.com/book?a=1&b=2']);
  expect(doc.documentElement.textContent).toContain('Unsafe');
  expect(doc.querySelector('script, [onload], [style]')).toBeNull();

  const opf = parseXml(files, 'EPUB/book.opf');
  const images = [...doc.querySelectorAll('img')];
  expect(images).toHaveLength(4);
  for (const [index, [ext, base64]] of Object.entries(pictures).entries()) {
    const src = images[index].getAttribute('src')!;
    expect(src).toBe(`images/image-000${index + 1}.${ext}`);
    expect(images[index].getAttribute('alt')).toBe(ext === 'png' ? 'A <picture>�' : '');
    expect(opf.querySelector(`item[href="${src}"]`)?.getAttribute('media-type')).toBe(`image/${ext}`);
    expect(files['EPUB/' + src]).toEqual(Uint8Array.from(atob(base64), char => char.charCodeAt(0)));
  }
  expect(doc.documentElement.textContent).toContain('Remote picture');
  expect(doc.documentElement.textContent).toContain('Image unavailable');
  const imported = prepareEpubImport(bytes, 'book.epub', []);
  expect(imported.notes.map(note => note.name)).toEqual(['001. Chapter <&> 😀�', '002. Next']);
  expect(JSON.stringify(imported.notes[0].document)).toContain(`data:image/png;base64,${pictures.png}`);
  expect(JSON.stringify(imported.notes[1].document)).toContain('Second chapter');
});

it.each([
  ['full_text', 'full_text', ''], ['compressed', 'full_text', ''], ['l1', 'l1', ' (L1)'], ['l2', 'l2', ' (L2)'],
] as const)('exports %s using %s and the reader’s fallback', async (mode, readMode, suffix) => {
  const first = chapter(31, 10, 'First', { has_l1: true, has_l2: true });
  const second = chapter(32, 10, 'Second');
  const read = vi.fn(async (note: NoteItem, selected: NoteTextMode) =>
    noteDocument(paragraph(note.id === first.id ? selected : 'Fallback body')));
  const { title, bytes } = await buildFolderEpub(book.id, [book, first, second], mode, read);
  expect(title).toBe(book.name + suffix);
  expect(read.mock.calls).toEqual([[first, readMode], [second, readMode]]);
  const files = unzipSync(bytes);
  expect(parseXml(files, 'EPUB/chapter-0001.xhtml').querySelector('p')?.textContent).toBe(readMode);
  expect(parseXml(files, 'EPUB/chapter-0002.xhtml').querySelector('p')?.textContent).toBe('Fallback body');
  expect(parseXml(files, 'EPUB/book.opf').getElementsByTagNameNS('http://purl.org/dc/elements/1.1/', 'title')[0].textContent)
    .toBe('History <&> 史 🏛�' + suffix);
});

it('validates folder membership and level availability before reading, and accepts a summary-only folder', async () => {
  const summary = chapter(15, 10, 'Summary', { folder_summary: true });
  const read = vi.fn(async () => noteDocument(paragraph('Summary body')));
  await expect(buildFolderEpub(11, [book], 'full_text', read)).rejects.toThrow('This folder is no longer available.');
  await expect(buildFolderEpub(10, [book], 'full_text', read)).rejects.toThrow('This folder has no notes to export.');
  for (const mode of ['l1', 'l2'] as const) {
    await expect(buildFolderEpub(10, [book, summary], mode, read))
      .rejects.toThrow(`This folder has no ${mode.toUpperCase()} text to export.`);
  }
  expect(read).not.toHaveBeenCalled();
  const { bytes } = await buildFolderEpub(10, [book, summary], 'full_text', read);
  expect(prepareEpubImport(bytes, 'book.epub', []).notes).toHaveLength(1);
});

it.each(['missing', 'corrupt'])('names a %s note and stops before the next read', async (failure) => {
  const bad = chapter(15, 10, 'First');
  const read = vi.fn(async () => {
    if (failure === 'missing') throw new Error('not found');
    return { type: 'doc', content: [{ type: 'unknown' }] };
  });
  await expect(buildFolderEpub(10, [book, bad, chapter(16, 10, 'Next')], 'full_text', read))
    .rejects.toThrow('Could not export the note “First”.');
  expect(read).toHaveBeenCalledExactlyOnceWith(bad, 'full_text');
});
