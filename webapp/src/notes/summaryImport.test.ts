import { describe, expect, it, vi } from 'vitest';
import { summaryDocument } from './summaryImport';
import { noteDocumentByteLimit } from './documentBytes';

function texts(node: Record<string, unknown>): string {
  return typeof node.text === 'string' ? node.text
    : Array.isArray(node.content) ? node.content.map(child => texts(child)).join(node.type === 'doc' ? '\n' : '') : '';
}

describe('summaryDocument', () => {
  it('preserves headings, nested lists, marks, explicit links, quotes and fenced code', () => {
    const doc = summaryDocument('# Goal\n\n**Bold** and *emphasis* [source](https://example.org/a).\n\n- One\n  - Nested\n\n> Quote\n\n```typescript\nconst value = 1;\n```');
    const json = JSON.stringify(doc);
    for (const type of ['heading', 'bulletList', 'bold', 'italic', 'link', 'codeBlock']) expect(json).toContain(type);
    expect(json).toContain('https://example.org/a');
    expect(json).toContain('typescript');
    expect(texts(doc)).toContain('Nested');
    // Notes have no block quote node; the quoted text stays as a paragraph.
    expect(texts(doc)).toContain('Quote');
  });

  it('keeps relative paths as text and strips image elements before DOM assignment', () => {
    const assignment = vi.spyOn(Element.prototype, 'innerHTML', 'set');
    const doc = summaryDocument('[relative](../notes.md) [web](https://example.org) ![external](https://example.org/image.png)');
    expect(JSON.stringify(doc)).not.toContain('image');
    expect(JSON.stringify(doc)).not.toContain('file:');
    expect(texts(doc)).toContain('relative');
    expect(JSON.stringify(doc)).toContain('https://example.org');
    // The only HTML assigned to a DOM element has already excluded external images.
    expect(assignment.mock.calls.every(([html]) => !String(html).includes('<img'))).toBe(true);
    assignment.mockRestore();
  });

  it('flattens Markdown and simple HTML tables preserving caption, rows, marks and tabs', () => {
    const markdown = summaryDocument('| Key | Value |\n| --- | --- |\n| **One** | Two |');
    expect(texts(markdown)).toContain('Key\tValue');
    expect(texts(markdown)).toContain('One\tTwo');
    expect(JSON.stringify(markdown)).toContain('bold');
    const html = summaryDocument('<table><caption>Results</caption><tbody><tr><td>A</td><td><em>B</em></td></tr><tr><td>C</td><td>D</td></tr></tbody></table>');
    expect(texts(html)).toBe('Results\nA\tB\nC\tD');
    expect(JSON.stringify(html)).toContain('italic');
  });

  it('rejects merged cells but accepts table text in fenced code', () => {
    expect(() => summaryDocument('<table><tr><td colspan="2">Merged</td></tr></table>')).toThrow();
    expect(() => summaryDocument('<table><tr><td rowspan="0">Merged</td></tr></table>')).toThrow();
    expect(texts(summaryDocument('```html\n<table><tr><td colspan="2">Example</td></tr></table>\n```'))).toContain('colspan="2"');
  });

  it.each(['', ' \n\t', '![](https://example.org/image.png)', '<script>alert(1)</script>'])('rejects unreadable content %s', (markdown) => {
    expect(() => summaryDocument(markdown)).toThrow();
  });

  it('checks both source and JSON encoded byte limits', () => {
    expect(() => summaryDocument('a'.repeat(noteDocumentByteLimit + 1))).toThrow();
    // Backslashes and quotes require JSON escaping and can cross the limit after parsing.
    expect(() => summaryDocument('```\n' + '"'.repeat(noteDocumentByteLimit / 2) + '\n```')).toThrow();
  });
});
