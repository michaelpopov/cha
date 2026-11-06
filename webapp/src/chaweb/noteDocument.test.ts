import { expect, it } from 'vitest';
import type { JSONContent } from '@tiptap/core';
import { noteDocument, noteItem, paragraph, textNode } from '../test/notes';
import { renderNoteDocument } from './noteDocument';

function body(content: JSONContent[], name = 'Article') {
  const rendered = renderNoteDocument(noteDocument(...content), noteItem({ name }), []);
  expect(rendered).not.toBeNull();
  const dom = new DOMParser().parseFromString(rendered!.html, 'text/html');
  return { ...rendered!, body: dom.body };
}

it('serializes the shared schema without an editor and preserves rich content', () => {
  const { body: dom } = body([
    { type: 'heading', attrs: { level: 1 }, content: [textNode(' Article  ')] },
    { type: 'heading', attrs: { level: 1 }, content: [textNode('Section')] },
    { type: 'heading', attrs: { level: 6 }, content: [textNode('Deep heading')] },
    { type: 'paragraph', content: [...['bold', 'italic', 'underline', 'code', 'highlight'].map((type) => ({
      type: 'text', text: type, marks: [{ type }],
    })), { type: 'hardBreak' }, textNode('after break')] },
    { type: 'orderedList', attrs: { start: 7 }, content: [{ type: 'listItem', content: [paragraph('Seventh')] }] },
    { type: 'bulletList', content: [{ type: 'listItem', content: [paragraph('Bullet')] }] },
    { type: 'codeBlock', attrs: { language: 'js' }, content: [textNode('<script> & code\nline two')] },
  ]);
  expect(dom.querySelector('h1')).toBeNull();
  expect(dom.querySelector('h2')?.textContent).toBe('Section');
  expect(dom.querySelector('h6')?.textContent).toBe('Deep heading');
  for (const tag of ['strong', 'em', 'u', 'code', 'mark', 'br', 'ul']) expect(dom.querySelector(tag)).not.toBeNull();
  expect(dom.querySelector('ol')?.getAttribute('start')).toBe('7');
  expect(dom.querySelector('pre')?.textContent).toBe('<script> & code\nline two');
  expect(dom.querySelector('[contenteditable],.tiptap,script,[style]')).toBeNull();
});

it('keeps stored web links and removes unsafe destinations without losing their text', () => {
  const hrefs = ['https://example.com/a', 'http://example.com', 'javascript:alert(1)',
    'cha://notes/1', 'data:text/html,test', 'file:///tmp/a', '//example.com', 'example.com'];
  const { body: dom } = body([{ type: 'paragraph', content: hrefs.map((href) => ({
    type: 'text', text: href, marks: [{ type: 'link', attrs: { href, target: '_blank', onclick: 'alert(1)' } }],
  })) }]);
  expect([...dom.querySelectorAll('a[href]')].map((a) => a.getAttribute('href')))
    .toEqual(['https://example.com/a', 'http://example.com/']);
  for (const href of hrefs) expect(dom.textContent).toContain(href);
  expect(dom.querySelector('[onclick],[target],[rel]')).toBeNull();
});

it('allows embedded raster data only, retaining alt text and safe highlight colors', () => {
  const png = 'data:image/png;base64,iVBORw0KGgo=';
  const { body: dom } = body([
    ...[png, 'https://tracking.test/image.png', 'data:image/svg+xml;base64,PHN2Zz4=',
      'data:image/png;base64,bm90IGEgcGljdHVyZQ=='].map((src, index) => ({
      type: 'image', attrs: { src, alt: index === 0 ? 'Picture' : 'Unavailable picture' },
    })),
    { type: 'image', attrs: { src: 'bad' } },
    { type: 'paragraph', content: ['#b3d9ff', '#f00'].map((color) => ({
      type: 'text', text: 'Comment', marks: [{ type: 'highlight', attrs: { color } }],
    })) },
  ]);
  expect(dom.querySelectorAll('img')).toHaveLength(1);
  expect(dom.querySelector('img')?.getAttribute('src')).toBe(png);
  expect(dom.querySelector('img')?.alt).toBe('Picture');
  expect(dom.textContent).toContain('Unavailable picture');
  expect(dom.textContent).toContain('Image unavailable');
  expect(dom.querySelectorAll('mark[data-color]')).toHaveLength(1);
  expect(dom.querySelector('[style]')).toBeNull();
});

it('sorts annotation groups like native Notes and omits source jump controls', () => {
  const group = (name: string, id: number) => [
    { type: 'heading', attrs: { level: 1 }, content: [textNode(name)] },
    { type: 'paragraph', content: [{ type: 'annotationSource', attrs: { sourceId: id } }, textNode(`${name} comment`)] },
  ];
  const rendered = renderNoteDocument(noteDocument(...group('Zebra', 2), ...group('Alpha', 3)),
    noteItem({ kind: 'annotation', name: 'ANNOTATIONS' }),
    [noteItem({ id: 2, name: 'Zebra' }), noteItem({ id: 3, name: 'Alpha' })]);
  expect(rendered?.html.indexOf('Alpha')).toBeLessThan(rendered!.html.indexOf('Zebra'));
  expect(rendered?.html).toContain('Alpha comment');
  expect(rendered?.html).not.toContain('button');
  expect(rendered?.html).not.toContain('sourceId');
});

it('reports empty notes and rejects unsupported or invalid structure', () => {
  expect(renderNoteDocument(noteDocument({ type: 'paragraph' }), noteItem(), [])?.empty).toBe(true);
  for (const document of [
    { type: 'paragraph' }, noteDocument({ type: 'unknown' }),
    noteDocument({ type: 'paragraph', content: [{ type: 'image' }] }),
    noteDocument({ type: 'text', text: 'Outside a block' }),
    noteDocument({ type: 'paragraph', content: [{ type: 'text', text: 'x', marks: [{ type: 'unknown' }] }] }),
    { type: 'doc', content: 'invalid' },
  ]) expect(renderNoteDocument(document, noteItem(), [])).toBeNull();
});
