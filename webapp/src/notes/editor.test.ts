import type { JSONContent } from '@tiptap/core';
import { GapCursor } from '@tiptap/pm/gapcursor';
import { redoDepth, undoDepth } from '@tiptap/pm/history';
import { NodeSelection, TextSelection } from '@tiptap/pm/state';
import { afterEach, describe, expect, it } from 'vitest';
import {
  createNotesEditor,
  documentUtf8Bytes,
  insertPromptParagraph,
  isAllowedNoteLink,
  noteByteLimit,
  withIsolatedHistory,
  type NotesEditorOptions,
} from './editor';
import type { Editor } from '@tiptap/core';

const open: Editor[] = [];

function editor(options?: NotesEditorOptions): Editor {
  const created = createNotesEditor(options);
  const element = created.options.element;
  if (element instanceof HTMLElement && !element.isConnected) document.body.appendChild(element);
  open.push(created);
  return created;
}

afterEach(() => {
  for (const item of open) item.destroy();
  open.length = 0;
  document.body.replaceChildren();
});

function simplify(node: JSONContent): unknown {
  if (node.type === 'text') {
    const marks = (node.marks ?? []).map((mark) => (
      mark.type === 'link' ? `link:${String(mark.attrs?.href)}` : mark.type
    ));
    return marks.length > 0 ? { text: node.text, marks } : node.text;
  }
  if (node.type === 'image') return { type: 'image', src: node.attrs?.src };
  const result: Record<string, unknown> = { type: node.type };
  if (typeof node.attrs?.level === 'number') result.level = node.attrs.level;
  if (typeof node.attrs?.start === 'number') result.start = node.attrs.start;
  if (node.content) result.content = node.content.map(simplify);
  return result;
}

function docOf(created: Editor): unknown {
  return (created.getJSON().content ?? []).map(simplify);
}

function selectText(created: Editor, text: string, fromOffset: number, toOffset = fromOffset): void {
  let found = -1;
  created.state.doc.descendants((node, pos) => {
    if (found >= 0) return false;
    if (!node.isText || !node.text) return;
    const at = node.text.indexOf(text);
    if (at < 0) return;
    found = pos + at;
    return false;
  });
  expect(found).toBeGreaterThanOrEqual(0);
  created.view.dispatch(created.state.tr.setSelection(TextSelection.create(
    created.state.doc,
    found + fromOffset,
    found + toOffset,
  )));
}

function insertText(created: Editor, text: string): void {
  created.view.dispatch(created.state.tr.insertText(text));
}

describe('note link predicate', () => {
  it('accepts only absolute http and https urls', () => {
    expect(isAllowedNoteLink('https://example.com/a')).toBe(true);
    expect(isAllowedNoteLink('http://example.com')).toBe(true);
    expect(isAllowedNoteLink('HTTP://Example.COM/a')).toBe(true);
    expect(isAllowedNoteLink('example.com')).toBe(false);
    expect(isAllowedNoteLink('mailto:user@example.com')).toBe(false);
    expect(isAllowedNoteLink('javascript:alert(1)')).toBe(false);
    expect(isAllowedNoteLink('file:///tmp/a')).toBe(false);
    expect(isAllowedNoteLink('data:text/plain,hi')).toBe(false);
    expect(isAllowedNoteLink('cha://app')).toBe(false);
  });
});

describe('atomic document acceptance', () => {
  it('rejects a root change and its autolink together', () => {
    const saves: string[] = [];
    const high = editor({
      byteLimit: 100_000,
      onAcceptedDocument: () => saves.push('high'),
    });
    const sample = ' https://example.com ';
    insertText(high, sample);
    expect(saves).toEqual(['high']);
    const linked = documentUtf8Bytes(high.state.doc);
    const plain = high.schema.node(
      'doc',
      null,
      high.schema.node('paragraph', null, high.schema.text(sample)),
    );
    expect(documentUtf8Bytes(plain)).toBeLessThan(linked);
    expect(JSON.stringify(high.getJSON())).toContain('https://example.com');

    const limited = editor({
      byteLimit: linked - 1,
      onAcceptedDocument: () => saves.push('limited'),
    });
    const before = limited.getJSON();
    const selection = limited.state.selection.toJSON();
    const depth = undoDepth(limited.state);
    insertText(limited, sample);
    expect(limited.getJSON()).toEqual(before);
    expect(limited.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(limited.state)).toBe(depth);
    expect(redoDepth(limited.state)).toBe(0);
    expect(saves).toEqual(['high']);
  });

  it('counts a split link href in the candidate json', () => {
    const href = `https://example.com/${'a'.repeat(80)}`;
    const content = {
      type: 'doc',
      content: [{
        type: 'paragraph',
        content: [{ type: 'text', text: 'xy', marks: [{ type: 'link', attrs: { href } }] }],
      }],
    };
    const high = editor({ byteLimit: 100_000 });
    high.commands.setContent(content);
    high.view.dispatch(high.state.tr.replaceWith(2, 2, high.schema.text('Z')));
    expect(JSON.stringify(high.getJSON()).split(href).length - 1).toBe(2);
    const grown = documentUtf8Bytes(high.state.doc);

    const saves: string[] = [];
    const limited = editor({
      byteLimit: grown - 1,
      content,
      onAcceptedDocument: () => saves.push('save'),
    });
    const before = limited.getJSON();
    const depth = undoDepth(limited.state);
    limited.view.dispatch(limited.state.tr.replaceWith(2, 2, limited.schema.text('Z')));
    expect(limited.getJSON()).toEqual(before);
    expect(undoDepth(limited.state)).toBe(depth);
    expect(saves).toEqual([]);
  });

  it('measures escaped unicode with the same utf-8 count', () => {
    const created = editor();
    insertText(created, '"\\\u4f60');
    const json = JSON.stringify(created.getJSON());
    expect(documentUtf8Bytes(created.state.doc)).toBe(new TextEncoder().encode(json).byteLength);
    expect(json).toContain('\\"');
    expect(json).toContain('\\\\');
    expect(new TextEncoder().encode('\u4f60')).toHaveLength(3);
    expect(json).toContain('\u4f60');
  });

  it('accepts a document at the 12 MiB limit and rejects one more byte', () => {
    const probe = editor();
    const one = documentUtf8Bytes(probe.schema.node(
      'doc',
      null,
      probe.schema.node('paragraph', null, probe.schema.text('a')),
    ));
    const two = documentUtf8Bytes(probe.schema.node(
      'doc',
      null,
      probe.schema.node('paragraph', null, probe.schema.text('aa')),
    ));
    expect(two - one).toBe(1);
    const length = noteByteLimit - (one - 1);
    const text = 'a'.repeat(length);
    expect(documentUtf8Bytes(probe.schema.node(
      'doc',
      null,
      probe.schema.node('paragraph', null, probe.schema.text(text)),
    ))).toBe(noteByteLimit);

    const created = editor({ byteLimit: noteByteLimit });
    created.commands.setContent({
      type: 'doc',
      content: [{ type: 'paragraph', content: [{ type: 'text', text }] }],
    });
    expect(documentUtf8Bytes(created.state.doc)).toBe(noteByteLimit);
    const depth = undoDepth(created.state);
    insertText(created, 'b');
    expect(documentUtf8Bytes(created.state.doc)).toBe(noteByteLimit);
    expect(created.state.doc.textContent).toBe(text);
    expect(undoDepth(created.state)).toBe(depth);
  }, 60_000);

  it('redraws a rejected composition over a dirty contenteditable', () => {
    const saves: string[] = [];
    const created = editor({
      byteLimit: 40,
      content: 'hello',
      onAcceptedDocument: () => saves.push('save'),
    });
    const dom = created.view.dom;
    dom.textContent = 'hello COMPOSITION';
    const selection = created.state.selection.toJSON();
    const depth = undoDepth(created.state);
    const redo = redoDepth(created.state);
    const notified = saves.length;
    insertText(created, 'x'.repeat(80));
    expect(created.getText()).toBe('hello');
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(depth);
    expect(redoDepth(created.state)).toBe(redo);
    expect(saves).toHaveLength(notified);
    expect(dom.textContent).toContain('hello');
    expect(dom.textContent).not.toContain('COMPOSITION');
  });

  it('keeps typing grouped and isolates a tagged action', () => {
    const saves: string[] = [];
    const created = editor({ onAcceptedDocument: () => saves.push('save') });
    insertText(created, 'a');
    insertText(created, 'b');
    expect(undoDepth(created.state)).toBe(1);
    expect(saves).toHaveLength(2);
    selectText(created, 'ab', 0, 2);
    expect(withIsolatedHistory(created, () => created.chain().toggleBold().run())).toBe(true);
    expect(saves).toHaveLength(3);
    const end = created.state.doc.content.size - 1;
    created.view.dispatch(created.state.tr.setSelection(TextSelection.create(created.state.doc, end)));
    insertText(created, 'c');
    expect(created.getText()).toBe('abc');
    expect(undoDepth(created.state)).toBe(3);
    expect(saves).toHaveLength(4);

    created.commands.undo();
    expect(created.getText()).toBe('ab');
    expect(created.getJSON().content?.[0].content?.[0].marks?.[0].type).toBe('bold');
    created.commands.undo();
    expect(created.getText()).toBe('ab');
    expect(created.getJSON().content?.[0].content?.[0].marks).toBeUndefined();
    created.commands.undo();
    expect(created.getText()).toBe('');
    expect(saves.length).toBe(7);
  });

  it('leaves redo in place when saving dispatches nothing', () => {
    const saves: string[] = [];
    const created = editor({ onAcceptedDocument: () => saves.push('save') });
    insertText(created, 'a');
    insertText(created, 'b');
    created.commands.undo();
    expect(created.getText()).toBe('');
    expect(redoDepth(created.state)).toBe(1);
    const notified = saves.length;
    created.emit('update', {
      editor: created,
      transaction: created.state.tr,
      appendedTransactions: [],
    });
    expect(saves).toHaveLength(notified + 1);
    expect(redoDepth(created.state)).toBe(1);
    created.commands.redo();
    expect(created.getText()).toBe('ab');
  });

  it('isolates a paste from the surrounding typing', () => {
    const created = editor();
    insertText(created, 'a');
    const pasted = created.state.tr.insertText('Z');
    pasted.setMeta('uiEvent', 'paste');
    created.view.dispatch(pasted);
    insertText(created, 'b');
    expect(created.getText()).toBe('aZb');
    created.commands.undo();
    expect(created.getText()).toBe('aZ');
    created.commands.undo();
    expect(created.getText()).toBe('a');
    created.commands.undo();
    expect(created.getText()).toBe('');
  });

  it('blocks editing and save notification when content is invalid', () => {
    const saves: string[] = [];
    const created = editor({
      content: {
        type: 'doc',
        content: [{ type: 'codeBlock', content: [{ type: 'text', text: 'x' }] }],
      },
      onAcceptedDocument: () => saves.push('save'),
    });
    expect(created.isEditable).toBe(false);
    insertText(created, 'z');
    expect(saves).toEqual([]);
    expect(insertPromptParagraph(created, 'Hello')).toBe(false);
  });

  it('renders a stored javascript link with an empty href', () => {
    const created = editor();
    created.commands.setContent({
      type: 'doc',
      content: [{
        type: 'paragraph',
        content: [{
          type: 'text',
          text: 'click',
          marks: [{ type: 'link', attrs: { href: 'javascript:alert(1)' } }],
        }],
      }],
    });
    expect(created.view.dom.querySelector('a')?.getAttribute('href')).toBe('');
    expect(() => {
      created.commands.setContent('<p><a href="javascript:alert(1)">click</a></p>');
    }).toThrow(/Invalid HTML content/);
    expect(created.getText()).toBe('click');
    expect(created.view.dom.querySelector('a')?.getAttribute('href')).toBe('');
  });
});

describe('prompt insertion', () => {
  it('fills an empty paragraph and rejects whitespace', () => {
    const created = editor();
    expect(created.commands.insertPromptParagraph('  \n\t')).toBe(false);
    expect(docOf(created)).toEqual([{ type: 'paragraph' }]);
    expect(created.commands.insertPromptParagraph('X')).toBe(true);
    expect(docOf(created)).toEqual([{ type: 'paragraph', content: ['X'] }]);
  });

  it('splits a paragraph, a heading, and a selected span', () => {
    const created = editor();
    created.commands.setContent('<p>abcd</p>');
    selectText(created, 'abcd', 2);
    expect(created.commands.insertPromptParagraph('X\nY')).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['ab'] },
      { type: 'paragraph', content: ['X', { type: 'hardBreak' }, 'Y'] },
      { type: 'paragraph', content: ['cd'] },
    ]);

    created.commands.setContent('<h2>abcd</h2>');
    selectText(created, 'abcd', 2);
    expect(created.commands.insertPromptParagraph('X')).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'heading', level: 2, content: ['ab'] },
      { type: 'paragraph', content: ['X'] },
      { type: 'heading', level: 2, content: ['cd'] },
    ]);

    created.commands.setContent('<p>abcd</p>');
    selectText(created, 'abcd', 1, 3);
    expect(created.commands.insertPromptParagraph('X')).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['a'] },
      { type: 'paragraph', content: ['X'] },
      { type: 'paragraph', content: ['d'] },
    ]);
  });

  it('inserts at the start or end of a block without an empty fragment', () => {
    const created = editor();
    created.commands.setContent('<p>abcd</p>');
    selectText(created, 'abcd', 0);
    created.commands.insertPromptParagraph('X');
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['X'] },
      { type: 'paragraph', content: ['abcd'] },
    ]);
    created.commands.setContent('<p>abcd</p>');
    selectText(created, 'abcd', 4);
    created.commands.insertPromptParagraph('X');
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['abcd'] },
      { type: 'paragraph', content: ['X'] },
    ]);
  });

  it('keeps a nested list in the same item', () => {
    const created = editor();
    created.commands.setContent('<ul><li><p>abcd</p><ul><li><p>nested</p></li></ul></li></ul>');
    selectText(created, 'abcd', 2);
    created.commands.insertPromptParagraph('X');
    expect(docOf(created)).toEqual([{
      type: 'bulletList',
      content: [{
        type: 'listItem',
        content: [
          { type: 'paragraph', content: ['ab'] },
          { type: 'paragraph', content: ['X'] },
          { type: 'paragraph', content: ['cd'] },
          {
            type: 'bulletList',
            content: [{ type: 'listItem', content: [{ type: 'paragraph', content: ['nested'] }] }],
          },
        ],
      }],
    }]);
  });

  it('keeps marks on the surviving fragments and leaves the new paragraph plain', () => {
    const created = editor();
    created.commands.setContent('<p><strong>abcd</strong></p>');
    selectText(created, 'abcd', 2);
    created.commands.insertPromptParagraph('X');
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: [{ text: 'ab', marks: ['bold'] }] },
      { type: 'paragraph', content: ['X'] },
      { type: 'paragraph', content: [{ text: 'cd', marks: ['bold'] }] },
    ]);
  });

  it('replaces a selected image and inserts beside a gap cursor', () => {
    const created = editor();
    created.commands.setContent({
      type: 'doc',
      content: [
        { type: 'paragraph', content: [{ type: 'text', text: 'keep' }] },
        { type: 'image', attrs: { src: 'data:image/png;base64,aaaa' } },
        { type: 'paragraph', content: [{ type: 'text', text: 'tail' }] },
      ],
    });
    const imagePos = created.state.doc.firstChild!.nodeSize;
    created.view.dispatch(created.state.tr.setSelection(NodeSelection.create(created.state.doc, imagePos)));
    expect(created.commands.insertPromptParagraph('X')).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['keep'] },
      { type: 'paragraph', content: ['X'] },
      { type: 'paragraph', content: ['tail'] },
    ]);

    created.commands.setContent({
      type: 'doc',
      content: [{ type: 'image', attrs: { src: 'data:image/png;base64,aaaa' } }],
    });
    const after = created.state.doc.firstChild!.nodeSize;
    const gapValid = (GapCursor as unknown as {
      valid: ($pos: ReturnType<Editor['state']['doc']['resolve']>) => boolean;
    }).valid;
    const gap = created.state.doc.resolve(gapValid(created.state.doc.resolve(0)) ? 0 : after);
    expect(gapValid(gap)).toBe(true);
    created.view.dispatch(created.state.tr.setSelection(new GapCursor(gap)));
    expect(created.commands.insertPromptParagraph('Y')).toBe(true);
    const blocks = docOf(created) as { type: string }[];
    expect(blocks.map((block) => block.type).sort()).toEqual(['image', 'paragraph']);
  });

  it('replaces across sibling blocks and across list items', () => {
    const created = editor();
    created.commands.setContent('<p>abcd</p><p>efgh</p>');
    selectText(created, 'abcd', 2, 2);
    const from = created.state.selection.from;
    let to = -1;
    created.state.doc.descendants((node, pos) => {
      if (to >= 0) return false;
      if (!node.isText || node.text !== 'efgh') return;
      to = pos + 2;
      return false;
    });
    created.view.dispatch(created.state.tr.setSelection(TextSelection.create(created.state.doc, from, to)));
    created.commands.insertPromptParagraph('X');
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['ab'] },
      { type: 'paragraph', content: ['X'] },
      { type: 'paragraph', content: ['gh'] },
    ]);

    created.commands.setContent('<ul><li><p>abcd</p></li><li><p>mid</p></li><li><p>efgh</p></li></ul>');
    selectText(created, 'abcd', 2);
    const start = created.state.selection.from;
    let end = -1;
    created.state.doc.descendants((node, pos) => {
      if (end >= 0) return false;
      if (!node.isText || node.text !== 'efgh') return;
      end = pos + 2;
      return false;
    });
    created.view.dispatch(created.state.tr.setSelection(TextSelection.create(created.state.doc, start, end)));
    created.commands.insertPromptParagraph('X');
    expect(docOf(created)).toEqual([{
      type: 'bulletList',
      content: [
        {
          type: 'listItem',
          content: [
            { type: 'paragraph', content: ['ab'] },
            { type: 'paragraph', content: ['X'] },
          ],
        },
        { type: 'listItem', content: [{ type: 'paragraph', content: ['gh'] }] },
      ],
    }]);
  });

  it('fits a removed start item and keeps a leading paragraph before a nested list', () => {
    const created = editor();
    created.commands.setContent(
      '<ul><li><p>abcd</p></li><li><p>ef</p><ul><li><p>nested</p></li></ul></li></ul>',
    );
    selectText(created, 'abcd', 0);
    const start = created.state.selection.from;
    let end = -1;
    created.state.doc.descendants((node, pos) => {
      if (end >= 0) return false;
      if (!node.isText || node.text !== 'ef') return;
      end = pos + 2;
      return false;
    });
    created.view.dispatch(created.state.tr.setSelection(TextSelection.create(created.state.doc, start, end)));
    created.commands.insertPromptParagraph('X');
    expect(docOf(created)).toEqual([{
      type: 'bulletList',
      content: [
        { type: 'listItem', content: [{ type: 'paragraph', content: ['X'] }] },
        {
          type: 'listItem',
          content: [
            { type: 'paragraph' },
            {
              type: 'bulletList',
              content: [{ type: 'listItem', content: [{ type: 'paragraph', content: ['nested'] }] }],
            },
          ],
        },
      ],
    }]);
  });

  it('keeps literal text, links the final url, and continues before the old suffix', () => {
    const created = editor();
    created.commands.setContent('<p>abcd</p>');
    selectText(created, 'abcd', 2);
    const original = created.state.selection.toJSON();
    expect(created.commands.insertPromptParagraph('**bold**\r\nsee https://example.com/a')).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['ab'] },
      {
        type: 'paragraph',
        content: [
          '**bold**',
          { type: 'hardBreak' },
          'see ',
          { text: 'https://example.com/a', marks: ['link:https://example.com/a'] },
        ],
      },
      { type: 'paragraph', content: ['cd'] },
    ]);
    expect(created.commands.insertPromptParagraph('Z')).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['ab'] },
      {
        type: 'paragraph',
        content: [
          '**bold**',
          { type: 'hardBreak' },
          'see ',
          { text: 'https://example.com/a', marks: ['link:https://example.com/a'] },
        ],
      },
      { type: 'paragraph', content: ['Z'] },
      { type: 'paragraph', content: ['cd'] },
    ]);
    created.commands.undo();
    expect(docOf(created)).toHaveLength(3);
    created.commands.undo();
    expect(docOf(created)).toEqual([{ type: 'paragraph', content: ['abcd'] }]);
    expect(created.state.selection.toJSON()).toEqual(original);
  });

  it('links http, bare domains, and normalized urls without repairing other schemes', () => {
    const created = editor();
    created.commands.insertPromptParagraph(
      'http://example.com example.com HTTP://Example.COM/a mailto:user@example.com javascript:alert(1) file:///tmp/a data:text/plain,hi cha://app https://example.com/a.',
    );
    expect(docOf(created)).toEqual([{
      type: 'paragraph',
      content: [
        { text: 'http://example.com', marks: ['link:http://example.com/'] },
        ' ',
        { text: 'example.com', marks: ['link:http://example.com/'] },
        ' ',
        { text: 'HTTP://Example.COM/a', marks: ['link:http://example.com/a'] },
        ' mailto:user@example.com javascript:alert(1) file:///tmp/a data:text/plain,hi cha://app ',
        { text: 'https://example.com/a', marks: ['link:https://example.com/a'] },
        '.',
      ],
    }]);
  });

  it('preserves an ordered list start and restores the old selection in one undo', () => {
    const created = editor();
    created.commands.setContent('<ol start="3"><li><p>abcd</p></li></ol>');
    selectText(created, 'abcd', 2);
    const selection = created.state.selection.toJSON();
    created.commands.insertPromptParagraph('X');
    expect(docOf(created)).toEqual([{
      type: 'orderedList',
      start: 3,
      content: [{
        type: 'listItem',
        content: [
          { type: 'paragraph', content: ['ab'] },
          { type: 'paragraph', content: ['X'] },
          { type: 'paragraph', content: ['cd'] },
        ],
      }],
    }]);
    created.commands.undo();
    expect(docOf(created)).toEqual([{
      type: 'orderedList',
      start: 3,
      content: [{ type: 'listItem', content: [{ type: 'paragraph', content: ['abcd'] }] }],
    }]);
    expect(created.state.selection.toJSON()).toEqual(selection);
  });

  it('rejects an over-limit insertion without clearing the submitted result', () => {
    const created = editor({ byteLimit: 80 });
    expect(created.commands.insertPromptParagraph('x'.repeat(200))).toBe(false);
    expect(docOf(created)).toEqual([{ type: 'paragraph' }]);
    expect(undoDepth(created.state)).toBe(0);
  });

  it('does not autolink a final url unless the insertion command adds the mark', () => {
    const created = editor();
    insertText(created, 'https://example.com/a');
    expect(JSON.stringify(created.getJSON())).not.toContain('"type":"link"');
    created.commands.setContent('<p></p>');
    created.commands.insertPromptParagraph('https://example.com/a');
    expect(docOf(created)).toEqual([{
      type: 'paragraph',
      content: [{ text: 'https://example.com/a', marks: ['link:https://example.com/a'] }],
    }]);
  });
});

describe('find and replace', () => {
  function search(created: Editor): { text: string; from: number; to: number }[] {
    return created.storage.findAndReplace.results.map((result) => ({
      text: created.state.doc.textBetween(result.from, result.to),
      from: result.from,
      to: result.to,
    }));
  }

  it('replaces literally, including $1 and an empty deletion', () => {
    const created = editor({ content: '<p>cat</p>' });
    created.commands.setSearchTerm('cat');
    created.commands.setReplaceTerm('$1');
    expect(withIsolatedHistory(created, () => created.commands.replace())).toBe(true);
    expect(created.getText()).toBe('$1');
    created.commands.setContent('<p>cat</p>');
    created.commands.setSearchTerm('cat');
    created.commands.setReplaceTerm('');
    expect(withIsolatedHistory(created, () => created.commands.replace())).toBe(true);
    expect(created.getText()).toBe('');
  });

  it('applies case and whole word, and ignores whole word in regex mode', () => {
    const created = editor({ content: '<p>cat catalog Cat</p>' });
    created.commands.setCaseSensitive(true);
    created.commands.setWholeWord(true);
    created.commands.setSearchTerm('cat');
    expect(search(created).map((item) => item.text)).toEqual(['cat']);
    created.commands.setCaseSensitive(false);
    created.commands.setSearchTerm('cat');
    expect(search(created).map((item) => item.text)).toEqual(['cat', 'Cat']);
    created.commands.setWholeWord(false);
    created.commands.setSearchTerm('cat');
    expect(search(created)).toHaveLength(3);
    created.commands.setUseRegex(true);
    created.commands.setWholeWord(true);
    created.commands.setSearchTerm('cat');
    expect(search(created)).toHaveLength(3);
  });

  it('produces no matches for an unsupported regular expression', () => {
    const created = editor({ content: '<p>cat</p>' });
    const before = created.getJSON();
    const depth = undoDepth(created.state);
    for (const term of ['(?=cat)', '\\1', '(?<=c)at']) {
      created.commands.setUseRegex(true);
      created.commands.setSearchTerm(term);
      expect(created.storage.findAndReplace.results).toEqual([]);
      expect(created.commands.replaceAll()).toBe(false);
      expect(created.getJSON()).toEqual(before);
      expect(undoDepth(created.state)).toBe(depth);
    }
  });

  it('does not match across a hard break or a paragraph', () => {
    const created = editor();
    created.commands.setContent({
      type: 'doc',
      content: [
        { type: 'paragraph', content: [{ type: 'text', text: 'ca' }, { type: 'hardBreak' }, { type: 'text', text: 't' }] },
        { type: 'paragraph', content: [{ type: 'text', text: 't' }] },
      ],
    });
    created.commands.setSearchTerm('cat');
    expect(created.storage.findAndReplace.results).toEqual([]);
    created.commands.setContent('<p>ca</p><p>t</p>');
    created.commands.setSearchTerm('cat');
    expect(created.storage.findAndReplace.results).toEqual([]);
  });

  it('replaces the original matches in one history event', () => {
    const saves: string[] = [];
    const created = editor({
      content: '<p>a a</p>',
      onAcceptedDocument: () => saves.push('save'),
    });
    const depth = undoDepth(created.state);
    created.commands.setSearchTerm('a');
    created.commands.setReplaceTerm('aa');
    expect(saves).toEqual([]);
    expect(undoDepth(created.state)).toBe(depth);
    expect(withIsolatedHistory(created, () => created.commands.replaceAll())).toBe(true);
    expect(created.getText()).toBe('aa aa');
    expect(undoDepth(created.state)).toBe(depth + 1);
    expect(saves).toEqual(['save']);
    created.commands.undo();
    expect(created.getText()).toBe('a a');
  });

  it('serializes a highlighted selection for the clipboard', () => {
    const created = editor({ content: '<p>keep <mark>this</mark> text</p>' });
    selectText(created, 'this', 0, 4);
    const copied = created.view.serializeForClipboard(created.state.selection.content());
    expect(copied.text).toBe('this');
    expect(copied.dom.querySelector('mark')?.textContent).toBe('this');
    expect(created.view.dom.querySelector('mark')).not.toBeNull();
  });
});
