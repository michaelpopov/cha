import { getSchema } from '@tiptap/core';
import { zipSync, strToU8 } from 'fflate';
import { describe, expect, it } from 'vitest';

import type { NoteItem } from '../api/client';
import { prepareEpubImport } from './epubImport';
import { createNotesEditor, notesExtensions } from './editor';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import { pictureByteLimit, pictureFromBytes } from './pictures';

const container = (path: string) => `<?xml version="1.0"?><container xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="${path}" media-type="application/oebps-package+xml"/></rootfiles></container>`;
const xhtml = (body: string) => `<html xmlns="http://www.w3.org/1999/xhtml"><head><title/></head><body>${body}</body></html>`;
const pkg = (manifest: string, spine: string, metadata = '<dc:title>Test Book</dc:title>', attrs = '') => `<package xmlns="http://www.idpf.org/2007/opf" xmlns:dc="http://purl.org/dc/elements/1.1/" version="3.0"><metadata>${metadata}</metadata><manifest>${manifest}</manifest><spine ${attrs}>${spine}</spine></package>`;
const item = (id: string, href: string, extra = '') => `<item id="${id}" href="${href}" media-type="application/xhtml+xml" ${extra}/>`;
const ref = (id: string, extra = '') => `<itemref idref="${id}" ${extra}/>`;

function archive(files: Record<string, string | Uint8Array>): Uint8Array {
  return zipSync(Object.fromEntries(Object.entries(files).map(([path, content]) => [
    path, typeof content === 'string' ? strToU8(content) : content,
  ])));
}

function utf16(text: string, littleEndian: boolean, bom = true): Uint8Array {
  const source = (bom ? '\uFEFF' : '') + text;
  const bytes = new Uint8Array(source.length * 2);
  const view = new DataView(bytes.buffer);
  for (let i = 0; i < source.length; i++) view.setUint16(i * 2, source.charCodeAt(i), littleEndian);
  return bytes;
}

function book(body: string, metadata?: string): Record<string, string> {
  return {
    'META-INF/container.xml': container('Book/package.opf'),
    'Book/package.opf': pkg(item('text', 'text.xhtml'), ref('text'), metadata),
    'Book/text.xhtml': xhtml(body),
  };
}

function text(document: Record<string, unknown>): string {
  return getSchema(notesExtensions()).nodeFromJSON(document).textContent;
}

describe('EPUB import', () => {
  it.each([
    { encoding: 'UTF-16LE', littleEndian: true, bom: true },
    { encoding: 'UTF-16BE', littleEndian: false, bom: true },
    { encoding: 'UTF-16LE', littleEndian: true, bom: false },
    { encoding: 'UTF-16BE', littleEndian: false, bom: false },
  ])('reads $encoding XML files (BOM: $bom)', ({ encoding, littleEndian, bom }) => {
    const files = book('<h1>Résumé 章 😀</h1><p>Содержание</p>');
    files['Book/package.opf'] = pkg(item('text', 'text.xhtml') + item('nav', 'nav.xhtml', 'properties="nav"'),
      ref('text'), '<dc:title>书籍 😀</dc:title>');
    files['Book/nav.xhtml'] = xhtml('<nav xmlns:epub="http://www.idpf.org/2007/ops" epub:type="toc"><a href="text.xhtml">Introducción 章</a></nav>');
    const bytes = zipSync(Object.fromEntries(Object.entries(files).map(([path, source]) => [
      path, utf16(`<?xml version="1.0" encoding="${encoding}"?>` + source.replace(/^<\?xml[^?]*\?>/u, ''), littleEndian, bom),
    ])));
    const result = prepareEpubImport(bytes, 'file.epub', []);
    expect(result.name).toBe('书籍 😀');
    expect(result.notes[0].name).toBe('001. Introducción 章');
    expect(text(result.notes[0].document)).toBe('Résumé 章 😀Содержание');
  });

  it.each([true, false])('rejects truncated UTF-16 text (little endian: %s)', (littleEndian) => {
    const files = Object.fromEntries(Object.entries(book('')).map(([path, source]) => [path, strToU8(source)]));
    files['Book/text.xhtml'] = utf16(xhtml('<p>Text</p>'), littleEndian).slice(0, -1);
    expect(() => prepareEpubImport(zipSync(files), 'file.epub', [])).toThrow('Could not read “Book/text.xhtml”');
  });

  it('uses EPUB 3 spine order and TOC labels, includes non-linear entries, and removes repeats', () => {
    const result = prepareEpubImport(archive({
      'META-INF/container.xml': container('Book/package.opf'),
      'Book/a.xhtml': xhtml('<h1>A</h1>'),
      'Book/b.xhtml': xhtml('<h1>B</h1>'),
      'Book/unused.xhtml': 'broken XML',
      'Book/package.opf': pkg(item('a', 'a.xhtml') + item('b', 'b.xhtml')
        + item('nav', 'Navigation/nav.xhtml', 'properties="nav"'), ref('b', 'linear="no"') + ref('a') + ref('b') + ref('nav')),
      'Book/Navigation/nav.xhtml': xhtml(`<nav xmlns:epub="http://www.idpf.org/2007/ops" epub:type="page-list"><a href="../b.xhtml">17</a></nav>
        <nav xmlns:epub="http://www.idpf.org/2007/ops" epub:type="landmarks"><a href="../b.xhtml">Body</a></nav>
        <nav xmlns:epub="http://www.idpf.org/2007/ops" epub:type="toc"><ol><li><a href="../b.xhtml#first">Title B</a></li>
        <li><a href="../b.xhtml#second">Ignored duplicate</a></li><li><a href="../a.xhtml">Title A</a></li></ol></nav>`),
    }), 'file.epub', []);
    expect(result.name).toBe('Test Book');
    expect(result.notes.map((note) => note.name)).toEqual(['001. Title B', '002. Title A', '003. nav']);
    expect(result.notes.slice(0, 2).map((note) => text(note.document))).toEqual(['B', 'A']);
  });

  it('uses only the EPUB 2 NCX navMap and keeps the first usable label', () => {
    const files = book('<h1>Heading</h1>');
    files['Book/package.opf'] = pkg(item('text', 'text.xhtml') + '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>', ref('text'), undefined, 'toc="ncx"');
    files['Book/toc.ncx'] = `<ncx><pageList><navPoint><navLabel><text>iv</text></navLabel><content src="text.xhtml"/></navPoint></pageList>
      <navMap><navPoint><navLabel><text> </text></navLabel><content src="text.xhtml"/></navPoint>
      <navPoint><navLabel><text>Introduction</text></navLabel><content src="text.xhtml#top"/></navPoint></navMap></ncx>`;
    expect(prepareEpubImport(archive(files), 'file.epub', []).notes[0].name).toBe('001. Introduction');
    files['Book/package.opf'] = files['Book/package.opf'].replace('</manifest>', item('nav', 'missing.xhtml', 'properties="nav"') + '</manifest>');
    expect(prepareEpubImport(archive(files), 'file.epub', []).notes[0].name).toBe('001. Heading');
  });

  it('uses fallback titles when the navigation file cannot be parsed', () => {
    const files = book('<h1>Heading</h1>');
    files['Book/package.opf'] = pkg(item('text', 'text.xhtml') + '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>', ref('text'), undefined, 'toc="ncx"');
    files['Book/toc.ncx'] = '<ncx><navMap>broken';
    expect(prepareEpubImport(archive(files), 'file.epub', []).notes[0].name).toBe('001. Heading');
  });

  it.each(['Book#1', 'Book%20Title', '书籍'])('resolves literal archive paths and encoded links in %s', (directory) => {
    const files = {
      'META-INF/container.xml': container(`${encodeURIComponent(directory)}/package.opf`),
      [`${directory}/package.opf`]: pkg(item('a', 'Text/%E7%AB%A0%23%25.xhtml') + item('nav', 'Navigation/nav.xhtml', 'properties="nav"'), ref('a')),
      [`${directory}/Text/章#%.xhtml`]: xhtml('<h1>Fallback</h1>'),
      [`${directory}/Navigation/nav.xhtml`]: xhtml('<nav xmlns:epub="http://www.idpf.org/2007/ops" epub:type="toc"><a href="../Text/%E7%AB%A0%23%25.xhtml#heading">Resolved</a></nav>'),
    };
    expect(prepareEpubImport(archive(files), 'file.epub', []).notes[0].name).toBe('001. Resolved');
  });

  it('resolves ../ at the archive root', () => {
    const files = book('<p>Root</p>');
    files['Book/package.opf'] = pkg(item('text', '../../text.xhtml?revision=1#top'), ref('text'));
    files['text.xhtml'] = files['Book/text.xhtml'];
    expect(text(prepareEpubImport(archive(files), 'file.epub', []).notes[0].document)).toBe('Root');
  });

  it('preserves supported formatting, XML self-closing anchors, and one file per note', () => {
    const result = prepareEpubImport(archive(book(`<a id="p5"/><h1>First</h1><p>Text <strong>bold</strong> <em>italic</em> <u>underlined</u><br/>Next</p>
      <h2>Second</h2><ul><li>Bullet</li></ul><ol start="4" type="a"><li>Numbered</li></ol><pre><code>  code\nline</code></pre>
      <section>Section<p>Paragraph</p></section><table><tr><td>Left</td><td>Right</td></tr></table>`)), 'file.epub', []);
    expect(result.notes).toHaveLength(1);
    expect(result.notes[0].name).toBe('001. First');
    const doc = result.notes[0].document;
    const json = JSON.stringify(doc);
    for (const type of ['heading', 'paragraph', 'bold', 'italic', 'underline', 'bulletList', 'orderedList', 'codeBlock', 'hardBreak']) {
      expect(json).toContain(`"${type}"`);
    }
    expect(text(doc)).toContain('Left\tRight');
    expect(json).toContain('"type":"orderedList","attrs":{"start":4,"type":"a"}');
    expect(json).toContain('  code\\nline');
    const editor = createNotesEditor({ content: doc, readPicture: async () => new Uint8Array() });
    try {
      expect(editor.isEditable).toBe(true);
      editor.commands.insertContent('Edited');
      expect(editor.getText()).toContain('Edited');
    } finally {
      editor.destroy();
    }
  });

  it('removes unsupported media, scripts, styles, interactive controls and internal link destinations', () => {
    const result = prepareEpubImport(archive(book(`<p><a href="book.io/chapter.xhtml">Internal</a> <a href="#top">Fragment</a>
      <a href=" HTTPS://example.com/page ">External</a><a href="mailto:a@example.com">Mail</a></p>
      <script>bad()</script><style>bad{}</style><form>Form</form><button>Button</button><textarea>Input</textarea>
      <img src="https://example.com/image.png"/><img src="data:image/png;base64,AA=="/>
      <svg xmlns="http://www.w3.org/2000/svg"><text>SVG text</text></svg><video>Video</video>`)), 'file.epub', []);
    const json = JSON.stringify(result.notes[0].document);
    expect(text(result.notes[0].document)).toMatch(/Internal Fragment ExternalMail/u);
    expect(json).toContain('https://example.com/page');
    for (const removed of ['book.io', '#top', 'mailto:', 'bad()', 'Form', 'Button', 'Input', 'image.png', 'data:image', 'SVG text', 'Video']) expect(json).not.toContain(removed);
    expect(json.match(/"type":"link"/gu)).toHaveLength(1);
  });

  it('keeps MathML text in document order', () => {
    const result = prepareEpubImport(archive(book(`<p>E = <math xmlns="http://www.w3.org/1998/Math/MathML">
      <mi>m</mi><msup><mi>c</mi><mn>2</mn></msup></math>.</p>`)), 'file.epub', []);
    expect(text(result.notes[0].document)).toBe('E = mc2.');
  });

  it('keeps supported inline styles and canonical links through cleanup', () => {
    const result = prepareEpubImport(archive(book(`<p style="color: red; background-color: yellow">
      <span style="font-weight: bold">Bold</span><span style="font-style: italic">Italic</span>
      <span style="text-decoration: underline">Underlined</span>
      <a href=" HTTPS://EXAMPLE.COM/path " target="book-frame" rel="opener">Link</a></p>`)), 'file.epub', []);
    const json = JSON.stringify(result.notes[0].document);
    for (const type of ['bold', 'italic', 'underline']) expect(json).toContain(`"type":"${type}"`);
    expect(json).toContain('https://example.com/path');
    for (const removed of ['color', 'yellow', 'book-frame', '"rel":"opener"']) expect(json).not.toContain(removed);
  });

  it('cleans prefixed XHTML while preserving headings, formatting, table text and web links', () => {
    const files = book('');
    files['Book/text.xhtml'] = `<h:html xmlns:h="http://www.w3.org/1999/xhtml"><h:body>
      <h:h1>Heading</h:h1><h:p>Readable <h:strong>bold</h:strong></h:p>
      <h:script>script text</h:script><h:style>style text</h:style><h:form>form text</h:form>
      <h:video>video text</h:video><h:button>button text</h:button><h:textarea>input text</h:textarea>
      <h:img src="https://example.com/image.png"/><h:img src="data:image/png;base64,AA=="/>
      <s:svg xmlns:s="http://www.w3.org/2000/svg"><s:text>SVG text</s:text></s:svg>
      <h:table><h:tbody><h:tr><h:td><h:p>Left</h:p><h:div>Again</h:div></h:td><h:td>Right</h:td></h:tr></h:tbody></h:table>
      <h:p><h:a href="book.io/chapter.xhtml">Internal</h:a><h:a href=" HTTPS://example.com/page ">External</h:a></h:p>
      </h:body></h:html>`;
    const result = prepareEpubImport(archive(files), 'file.epub', []);
    const doc = result.notes[0].document;
    expect(result.notes[0].name).toBe('001. Heading');
    expect(text(doc)).toBe('HeadingReadable boldLeftAgain\tRightInternalExternal');
    const json = JSON.stringify(doc);
    for (const type of ['bold', 'hardBreak']) expect(json).toContain(`"type":"${type}"`);
    expect(json).toContain('https://example.com/page');
    expect(json.match(/"type":"link"/gu)).toHaveLength(1);
    for (const removed of ['script text', 'style text', 'form text', 'video text', 'button text', 'input text',
      'SVG text', 'image.png', 'data:image', 'book.io']) expect(json).not.toContain(removed);
  });

  it.each(['', 'h:'])('preserves table captions before rows with tag prefix "%s"', (prefix) => {
    const files = book('');
    files['Book/text.xhtml'] = `<${prefix}html xmlns="http://www.w3.org/1999/xhtml" xmlns:h="http://www.w3.org/1999/xhtml"><${prefix}body>
      <${prefix}table><${prefix}caption>Annual <${prefix}strong>totals</${prefix}strong></${prefix}caption>
        <${prefix}tr><${prefix}td>2025</${prefix}td><${prefix}td>42</${prefix}td></${prefix}tr></${prefix}table>
      <${prefix}table><${prefix}caption>Caption only</${prefix}caption></${prefix}table>
      </${prefix}body></${prefix}html>`;
    const doc = prepareEpubImport(archive(files), 'file.epub', []).notes[0].document;
    expect(doc).toEqual({ type: 'doc', content: [
      { type: 'paragraph', content: [
        { type: 'text', text: 'Annual ' },
        { type: 'text', text: 'totals', marks: [{ type: 'bold' }] },
      ] },
      { type: 'paragraph', content: [{ type: 'text', text: '2025\t42' }] },
      { type: 'paragraph', content: [{ type: 'text', text: 'Caption only' }] },
    ] });
  });

  it.each(['div', 'section'])('preserves prefixed %s block boundaries and inline formatting', (tag) => {
    const files = book('');
    files['Book/text.xhtml'] = `<h:html xmlns:h="http://www.w3.org/1999/xhtml"><h:body>
      <h:${tag}>First <h:span>paragraph.</h:span></h:${tag}><h:${tag}>Second <h:strong>paragraph.</h:strong></h:${tag}>
      </h:body></h:html>`;
    const doc = prepareEpubImport(archive(files), 'file.epub', []).notes[0].document;
    expect(doc).toEqual({ type: 'doc', content: [
      { type: 'paragraph', content: [{ type: 'text', text: 'First paragraph.' }] },
      { type: 'paragraph', content: [
        { type: 'text', text: 'Second ' },
        { type: 'text', text: 'paragraph.', marks: [{ type: 'bold' }] },
      ] },
    ] });
  });

  it('flattens nested tables before their parent table', () => {
    const result = prepareEpubImport(archive(book(`<table><tr><td>Before<table>
      <caption>Nested caption</caption><tr><td>Inner left</td><td>Inner right</td></tr>
      </table></td><td>Outer right</td></tr></table>`)), 'file.epub', []);
    expect(result.notes[0].document).toEqual({ type: 'doc', content: [
      { type: 'paragraph', content: [
        { type: 'text', text: 'Before' }, { type: 'hardBreak' },
        { type: 'text', text: 'Nested caption' }, { type: 'hardBreak' },
        { type: 'text', text: 'Inner left\tInner right\tOuter right' },
      ] },
    ] });
  });

  it.each([
    ['PNG', [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]],
    ['JPEG', [0xff, 0xd8, 0xff]],
    ['GIF', [0x47, 0x49, 0x46, 0x38, 0x39, 0x61]],
    ['WebP', [0x52, 0x49, 0x46, 0x46, 0, 0, 0, 0, 0x57, 0x45, 0x42, 0x50]],
  ])('embeds %s pictures from encoded chapter-relative paths in reading order', (_format, header) => {
    const bytes = new Uint8Array(header);
    const files = book('');
    files['Book/package.opf'] = pkg(item('text', 'Text/chapter.xhtml'), ref('text'));
    files['Book/Text/chapter.xhtml'] = `<h:html xmlns:h="http://www.w3.org/1999/xhtml"><h:body>
      <h:p>Before</h:p><h:p><h:img src="../Pictures/%E7%AB%A0%20%23%25.bin" alt="Illustration" title="Caption" width="1"/></h:p>
      <h:p>After</h:p></h:body></h:html>`;
    const doc = prepareEpubImport(archive({ ...files, 'Book/Pictures/章 #%.bin': bytes }), 'file.epub', []).notes[0].document;
    const picture = pictureFromBytes(bytes);
    if (!('src' in picture)) throw new Error('Expected a picture');
    expect(doc).toMatchObject({ type: 'doc', content: [
      { type: 'paragraph', content: [{ type: 'text', text: 'Before' }] },
      { type: 'image', attrs: { src: picture.src, alt: 'Illustration', title: 'Caption', width: null } },
      { type: 'paragraph', content: [{ type: 'text', text: 'After' }] },
    ] });
    const editor = createNotesEditor({ content: doc, readPicture: async () => { throw new Error('Unexpected download'); } });
    try {
      expect(editor.view.dom.querySelector('img')).toHaveAttribute('src', picture.src);
      expect(editor.getJSON()).toEqual(doc);
    } finally {
      editor.destroy();
    }
  });

  it('imports image-only chapters and valid data pictures', () => {
    const bytes = new Uint8Array([0xff, 0xd8, 0xff]);
    const picture = pictureFromBytes(bytes);
    if (!('src' in picture)) throw new Error('Expected a picture');
    const doc = prepareEpubImport(archive({
      ...book(`<p><img src="cover.jpg"/></p><img src="${picture.src}"/>`),
      'Book/cover.jpg': bytes,
    }), 'file.epub', []).notes[0].document;
    expect(doc).toMatchObject({ type: 'doc', content: [
      { type: 'image', attrs: { src: picture.src } },
      { type: 'image', attrs: { src: picture.src } },
    ] });
  });

  it('keeps chapter text when pictures are missing, oversized, unsupported or external', () => {
    const oversized = new Uint8Array(pictureByteLimit + 1);
    oversized.set([0xff, 0xd8, 0xff]);
    const doc = prepareEpubImport(archive({
      ...book(`<p>Before<img src="missing.png"/><img src="large.jpg"/><img src="bad.png"/>
        <img src="drawing.svg"/><img src="https://example.com/remote.jpg"/><img src="//example.com/remote.jpg"/>
        <img src="file:///tmp/local.jpg"/><img src="data:image/png;base64,AA=="/>After</p>`),
      'Book/large.jpg': oversized,
      'Book/bad.png': 'Not an image',
      'Book/drawing.svg': '<svg xmlns="http://www.w3.org/2000/svg"/>',
    }), 'file.epub', []).notes[0].document;
    expect(text(doc)).toBe('Before After');
    expect(JSON.stringify(doc)).not.toContain('"type":"image"');
  });

  it('imports empty, missing-image-only and non-XHTML spine files as empty notes', () => {
    const files = book('<img src="cover.png"/>');
    files['Book/package.opf'] = pkg(item('text', 'text.xhtml') + item('empty', 'empty.xhtml') + '<item id="svg" href="cover.svg" media-type="image/svg+xml"/>', ref('text') + ref('empty') + ref('svg'));
    files['Book/empty.xhtml'] = xhtml('');
    files['Book/cover.svg'] = 'not XML; this media must not be parsed';
    const result = prepareEpubImport(archive(files), 'file.epub', []);
    expect(result.notes).toHaveLength(3);
    for (const note of result.notes) expect(note.document).toEqual({ type: 'doc', content: [{ type: 'paragraph' }] });
  });

  it('cleans Unicode names, uses fallbacks and avoids existing root names', () => {
    const result = prepareEpubImport(archive(book('<h1> </h1><h2>' + '😀'.repeat(220) + '</h2>', '<dc:title> </dc:title><dc:title> Book\n Title </dc:title>')), 'file.epub', [
      { id: 1, parent_id: null, kind: 'folder', name: 'Book Title' },
    ]);
    expect(result.name).toBe('Book Title 1');
    expect(Array.from(result.notes[0].name)).toHaveLength(195);
    expect(prepareEpubImport(archive(book('<p>Text</p>', '')), 'Fallback.epub', []).name).toBe('Fallback');
    expect(prepareEpubImport(archive(book('<p>Text</p>', '')), '.epub', []).name).toBe('Book');
    expect(prepareEpubImport(archive(book('<p>Text</p>')), 'file.epub', []).notes[0].name).toBe('001. text');
  });

  it('checks book names only in the selected destination', () => {
    const bytes = archive(book('<p>Text</p>', '<dc:title>Book</dc:title>'));
    const items: NoteItem[] = [
      { id: 1, parent_id: null, kind: 'folder', name: 'Book' },
      { id: 2, parent_id: null, kind: 'folder', name: 'Research' },
      { id: 3, parent_id: 2, kind: 'folder', name: 'Book' },
      { id: 4, parent_id: null, kind: 'folder', name: 'Other' },
    ];
    expect(prepareEpubImport(bytes, 'file.epub', items, 2)).toMatchObject({ parent_id: 2, name: 'Book 1' });
    expect(prepareEpubImport(bytes, 'file.epub', items, 4)).toMatchObject({ parent_id: 4, name: 'Book' });
  });

  // The 1000-file book takes about 1.5 s alone and can exceed the 5 s default on a busy machine.
  it.each([10, 100, 1000])('numbers %i files with equal-width prefixes and permits duplicate titles', (count) => {
    const files = book('');
    files['Book/package.opf'] = pkg(Array.from({ length: count }, (_, i) => item(`f${i}`, `${i}.xhtml`)).join(''), Array.from({ length: count }, (_, i) => ref(`f${i}`)).join(''));
    for (let i = 0; i < count; i++) files[`Book/${i}.xhtml`] = xhtml('<h1>Same</h1>');
    const names = prepareEpubImport(archive(files), 'file.epub', []).notes.map((note) => note.name);
    expect(names).toHaveLength(count);
    expect([...names].sort()).toEqual(names);
    expect(names[0]).toBe(`${count === 1000 ? '0001' : '001'}. Same`);
    expect(names.at(-1)).toBe(`${String(count).padStart(3, '0')}. Same`);
  }, 30_000);

  it.each([
    ['missing text', (files: Record<string, string>) => { delete files['Book/text.xhtml']; }, 'Book/text.xhtml'],
    ['broken text XML', (files: Record<string, string>) => { files['Book/text.xhtml'] = xhtml('<p>unclosed'); }, 'Book/text.xhtml'],
    ['encrypted text', (files: Record<string, string>) => { files['Book/text.xhtml'] = 'encrypted data'; }, 'Book/text.xhtml'],
    ['text outside the XHTML namespace', (files: Record<string, string>) => { files['Book/text.xhtml'] = '<html><body><p>Text</p></body></html>'; }, 'No XHTML body in “Book/text.xhtml”'],
    ['broken package', (files: Record<string, string>) => { files['Book/package.opf'] = '<package/>'; }, 'Book/package.opf'],
    ['missing package', (files: Record<string, string>) => { delete files['Book/package.opf']; }, 'Book/package.opf'],
    ['undecodable path', (files: Record<string, string>) => { files['Book/package.opf'] = pkg(item('text', 'bad%ZZ'), ref('text')); }, 'spine'],
  ])('rejects %s with a source filename', (_name, change, message) => {
    const files = book('<p>Text</p>');
    change(files);
    expect(() => prepareEpubImport(archive(files), 'file.epub', [])).toThrow(message);
  });

  it('ignores unused huge or broken resources, but checks sizes before inflating text', () => {
    const files = book('<p>Text</p>');
    files['Book/font.ttf'] = 'font';
    files['META-INF/encryption.xml'] = 'broken, unused encryption metadata';
    const bytes = archive(files);
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const setDeclaredSize = (path: string, size: number) => {
      for (let i = 0; i < bytes.length - 46; i++) {
        if (view.getUint32(i, true) !== 0x02014b50) continue;
        const nameLength = view.getUint16(i + 28, true);
        if (new TextDecoder().decode(bytes.subarray(i + 46, i + 46 + nameLength)) === path) view.setUint32(i + 24, size, true);
      }
    };
    setDeclaredSize('Book/font.ttf', 17 * 1024 * 1024);
    expect(prepareEpubImport(bytes, 'file.epub', []).notes).toHaveLength(1);
    setDeclaredSize('Book/text.xhtml', 17 * 1024 * 1024);
    expect(() => prepareEpubImport(bytes, 'file.epub', [])).toThrow('Book/text.xhtml');
  });

  it('checks both the per-note document limit and complete request limit', () => {
    expect(() => prepareEpubImport(archive(book(`<p>${'x'.repeat(noteDocumentByteLimit)}</p>`)), 'file.epub', [])).toThrow('12 MiB');
    const files = book(`<p>${'x'.repeat(9 * 1024 * 1024)}</p>`);
    files['Book/second.xhtml'] = files['Book/text.xhtml'];
    files['Book/package.opf'] = pkg(item('text', 'text.xhtml') + item('second', 'second.xhtml'), ref('text') + ref('second'));
    expect(() => prepareEpubImport(archive(files), 'file.epub', [])).toThrow('request limit');
  });

  it('skips pictures that do not fit the note and book size limits', () => {
    const bytes = new Uint8Array(pictureByteLimit);
    bytes.set([0xff, 0xd8, 0xff]);
    const pictures = (doc: Record<string, unknown>) => JSON.stringify(doc).match(/"type":"image"/gu)?.length ?? 0;
    const note = prepareEpubImport(archive({
      ...book('<img src="picture.jpg"/>'.repeat(9) + '<p>After</p>'),
      'Book/picture.jpg': bytes,
    }), 'file.epub', []).notes[0].document;
    expect(pictures(note)).toBe(8);
    expect(text(note)).toBe('After');
    expect(noteDocumentByteLength(note)).toBeLessThanOrEqual(noteDocumentByteLimit);
    const files = book('<img src="picture.jpg"/>'.repeat(6) + '<p>First</p>');
    files['Book/second.xhtml'] = xhtml('<img src="picture.jpg"/>'.repeat(6) + '<p>Second</p>');
    files['Book/package.opf'] = pkg(item('text', 'text.xhtml') + item('second', 'second.xhtml'), ref('text') + ref('second'));
    const result = prepareEpubImport(archive({ ...files, 'Book/picture.jpg': bytes }), 'file.epub', []);
    expect(result.notes.map((item) => pictures(item.document))).toEqual([6, 5]);
    expect(result.notes.map((item) => text(item.document))).toEqual(['First', 'Second']);
    expect(noteDocumentByteLength(result)).toBeLessThanOrEqual(16 * 1024 * 1024 - 64 * 1024);
  });
});
