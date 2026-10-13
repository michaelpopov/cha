import type { JSONContent } from '@tiptap/core';
import { GapCursor } from '@tiptap/pm/gapcursor';
import { redoDepth, undoDepth } from '@tiptap/pm/history';
import { Fragment, Slice } from '@tiptap/pm/model';
import { NodeSelection, TextSelection } from '@tiptap/pm/state';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import {
  canonicalNoteLink,
  createNotesEditor,
  importPictureFile,
  insertPromptParagraph,
  insertTrackedPicture,
  isAllowedNoteLink,
  linkRangeAt,
  removeNoteLink,
  replaceLinkDestination,
  selectNoteEnd,
  storedWebDestination,
  trackSelectionBookmark,
  transformClipboardHTML,
  withIsolatedHistory,
  type NotesEditorOptions,
} from './editor';
import {
  noteTooLargeMessage,
  pictureFromBytes,
  pictureTooLargeMessage,
  pictureTypeMessage,
} from './pictures';
import type { Editor } from '@tiptap/core';

if (typeof ClipboardEvent === 'undefined') {
  class ClipboardEventPolyfill extends Event {
    clipboardData: DataTransfer | null = null;
  }
  globalThis.ClipboardEvent = ClipboardEventPolyfill as unknown as typeof ClipboardEvent;
}

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
  vi.restoreAllMocks();
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

function selectBetween(
  created: Editor,
  fromText: string,
  fromOffset: number,
  toText: string,
  toOffset: number,
): void {
  selectText(created, fromText, fromOffset);
  const from = created.state.selection.from;
  selectText(created, toText, toOffset);
  const to = created.state.selection.from;
  created.view.dispatch(created.state.tr.setSelection(TextSelection.create(created.state.doc, from, to)));
}

function insertText(created: Editor, text: string): void {
  created.view.dispatch(created.state.tr.insertText(text));
}

function pasteSlice(created: Editor, slice: Slice): boolean {
  const event = new Event('paste') as ClipboardEvent;
  let handled = false;
  created.view.someProp('handlePaste', (fn) => {
    handled = !!fn(created.view, event, slice);
    return handled;
  });
  return handled;
}

function dropSlice(
  created: Editor,
  slice: Slice,
  transfer: { files?: { length: number }; types?: string[] },
): boolean {
  const event = { dataTransfer: transfer } as unknown as DragEvent;
  let handled = false;
  created.view.someProp('handleDrop', (fn) => {
    handled = !!fn(created.view, event, slice, false);
    return handled;
  });
  return handled;
}

function imageSlice(created: Editor): Slice {
  const image = created.schema.nodes.image.create({ src: 'data:image/png;base64,aa' });
  return new Slice(Fragment.from(image), 0, 0);
}

const pngHeader = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x01];

function pngSrc(): string {
  const picture = pictureFromBytes(Uint8Array.from(pngHeader));
  if ('error' in picture) throw new Error(picture.error);
  return picture.src;
}

function pasteWith(created: Editor, slice: Slice, data: { files?: File[]; text?: string }): boolean {
  const event = {
    clipboardData: {
      files: data.files ?? [],
      getData: (type: string) => (type === 'text/plain' ? data.text ?? '' : ''),
    },
  } as unknown as ClipboardEvent;
  let handled = false;
  created.view.someProp('handlePaste', (fn) => {
    handled = !!fn(created.view, event, slice);
    return handled;
  });
  return handled;
}

function clickAt(created: Editor, target: Element, button: number): boolean {
  const event = new MouseEvent('click', { button, bubbles: true, cancelable: true });
  let handled = false;
  target.addEventListener('click', () => {
    created.view.someProp('handleClick', (fn) => {
      handled = !!fn(created.view, 0, event);
      return handled;
    });
  }, { once: true });
  target.dispatchEvent(event);
  return handled;
}

function anchorOf(created: Editor): Element {
  const anchor = created.view.dom.querySelector('a');
  if (!anchor) throw new Error('Link is missing.');
  return anchor;
}

describe('note link predicate', () => {
  it('accepts absolute http and https urls and a bare web address', () => {
    expect(isAllowedNoteLink('https://example.com/a')).toBe(true);
    expect(isAllowedNoteLink('http://example.com')).toBe(true);
    expect(canonicalNoteLink('http://example.com')).toBe('http://example.com/');
    expect(isAllowedNoteLink('HTTP://Example.COM/a')).toBe(true);
    expect(storedWebDestination('HTTP://Example.COM/a')).toBe('http://example.com/a');
    expect(isAllowedNoteLink('example.com')).toBe(true);
    expect(canonicalNoteLink('example.com')).toBe('http://example.com/');
    expect(storedWebDestination('example.com')).toBeNull();
    expect(isAllowedNoteLink('mailto:user@example.com')).toBe(false);
    expect(isAllowedNoteLink('user@example.com')).toBe(false);
    expect(isAllowedNoteLink('javascript:alert(1)')).toBe(false);
    expect(storedWebDestination('javascript:alert(1)')).toBeNull();
    expect(isAllowedNoteLink('file:///tmp/a')).toBe(false);
    expect(isAllowedNoteLink('file.txt')).toBe(false);
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
    const linked = noteDocumentByteLength(high.state.doc);
    const plain = high.schema.node(
      'doc',
      null,
      high.schema.node('paragraph', null, high.schema.text(sample)),
    );
    expect(noteDocumentByteLength(plain)).toBeLessThan(linked);
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
    const grown = noteDocumentByteLength(high.state.doc);

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
    expect(noteDocumentByteLength(created.state.doc)).toBe(new TextEncoder().encode(json).byteLength);
    expect(json).toContain('\\"');
    expect(json).toContain('\\\\');
    expect(new TextEncoder().encode('\u4f60')).toHaveLength(3);
    expect(json).toContain('\u4f60');
  });

  it('accepts a document at the 12 MiB limit and rejects one more byte', () => {
    const probe = editor();
    const one = noteDocumentByteLength(probe.schema.node(
      'doc',
      null,
      probe.schema.node('paragraph', null, probe.schema.text('a')),
    ));
    const two = noteDocumentByteLength(probe.schema.node(
      'doc',
      null,
      probe.schema.node('paragraph', null, probe.schema.text('aa')),
    ));
    expect(two - one).toBe(1);
    const length = noteDocumentByteLimit - (one - 1);
    const text = 'a'.repeat(length);
    expect(noteDocumentByteLength(probe.schema.node(
      'doc',
      null,
      probe.schema.node('paragraph', null, probe.schema.text(text)),
    ))).toBe(noteDocumentByteLimit);

    const created = editor({ byteLimit: noteDocumentByteLimit });
    created.commands.setContent({
      type: 'doc',
      content: [{ type: 'paragraph', content: [{ type: 'text', text }] }],
    });
    expect(noteDocumentByteLength(created.state.doc)).toBe(noteDocumentByteLimit);
    const depth = undoDepth(created.state);
    insertText(created, 'b');
    expect(noteDocumentByteLength(created.state.doc)).toBe(noteDocumentByteLimit);
    expect(created.state.doc.textContent).toBe(text);
    expect(undoDepth(created.state)).toBe(depth);
  }, 60_000);

  it('redraws a rejected browser text change through the DOM observer', async () => {
    const changeDomText = async (created: Editor) => {
      const text = created.view.dom.querySelector('p')?.firstChild;
      expect(text).toBeInstanceOf(Text);
      (text as Text).data = `hello${'x'.repeat(80)}`;
      await new Promise((resolve) => setTimeout(resolve, 0));
    };

    const accepted = editor({ content: 'hello' });
    await changeDomText(accepted);
    expect(accepted.getText()).toBe(`hello${'x'.repeat(80)}`);

    const saves: string[] = [];
    const created = editor({
      byteLimit: 120,
      content: 'hello',
      onAcceptedDocument: () => saves.push('save'),
    });
    const selection = created.state.selection.toJSON();
    const depth = undoDepth(created.state);
    const redo = redoDepth(created.state);
    await changeDomText(created);
    expect(created.getText()).toBe('hello');
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(depth);
    expect(redoDepth(created.state)).toBe(redo);
    expect(saves).toEqual([]);
    expect(created.view.dom.textContent).toBe('hello');
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

  it('does not notify a save when opening content', () => {
    const saves: string[] = [];
    const created = editor({
      content: '<p>https://example.com already here</p>',
      onAcceptedDocument: () => saves.push('save'),
    });
    expect(created.getText()).toBe('https://example.com already here');
    expect(JSON.stringify(created.getJSON())).not.toContain('"type":"link"');
    expect(saves).toEqual([]);
  });

  it('rejects a document change while editing is disabled', () => {
    const saves: string[] = [];
    const created = editor({
      content: 'hello',
      onAcceptedDocument: () => saves.push('save'),
    });
    created.setEditable(false, false);
    const before = created.getJSON();
    const selection = created.state.selection.toJSON();
    const depth = undoDepth(created.state);
    insertText(created, 'x');
    expect(created.getJSON()).toEqual(before);
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(depth);
    expect(saves).toEqual([]);
  });

  it('supports headings, underline, and ordered list start values', () => {
    const created = editor();
    created.commands.setContent('<h1>One</h1><h6>Six</h6><p>mark</p><ol start="4"><li><p>item</p></li></ol>');
    selectText(created, 'mark', 0, 4);
    expect(created.chain().toggleUnderline().run()).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'heading', level: 1, content: ['One'] },
      { type: 'heading', level: 6, content: ['Six'] },
      { type: 'paragraph', content: [{ text: 'mark', marks: ['underline'] }] },
      {
        type: 'orderedList',
        start: 4,
        content: [{ type: 'listItem', content: [{ type: 'paragraph', content: ['item'] }] }],
      },
    ]);
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
    expect(insertPromptParagraph(created, '  \n\t')).toBe(false);
    expect(docOf(created)).toEqual([{ type: 'paragraph' }]);
    expect(insertPromptParagraph(created, 'X')).toBe(true);
    expect(docOf(created)).toEqual([{ type: 'paragraph', content: ['X'] }]);
  });

  it('splits a paragraph, a heading, and a selected span', () => {
    const created = editor();
    created.commands.setContent('<p>abcd</p>');
    selectText(created, 'abcd', 2);
    expect(insertPromptParagraph(created, 'X\nY')).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['ab'] },
      { type: 'paragraph', content: ['X', { type: 'hardBreak' }, 'Y'] },
      { type: 'paragraph', content: ['cd'] },
    ]);

    created.commands.setContent('<h2>abcd</h2>');
    selectText(created, 'abcd', 2);
    expect(insertPromptParagraph(created, 'X')).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'heading', level: 2, content: ['ab'] },
      { type: 'paragraph', content: ['X'] },
      { type: 'heading', level: 2, content: ['cd'] },
    ]);

    created.commands.setContent('<p>abcd</p>');
    selectText(created, 'abcd', 1, 3);
    expect(insertPromptParagraph(created, 'X')).toBe(true);
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
    insertPromptParagraph(created, 'X');
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['X'] },
      { type: 'paragraph', content: ['abcd'] },
    ]);
    created.commands.setContent('<p>abcd</p>');
    selectText(created, 'abcd', 4);
    insertPromptParagraph(created, 'X');
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['abcd'] },
      { type: 'paragraph', content: ['X'] },
    ]);
  });

  it('keeps a nested list in the same item', () => {
    const created = editor();
    created.commands.setContent('<ul><li><p>abcd</p><ul><li><p>nested</p></li></ul></li></ul>');
    selectText(created, 'abcd', 2);
    insertPromptParagraph(created, 'X');
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
    insertPromptParagraph(created, 'X');
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: [{ text: 'ab', marks: ['bold'] }] },
      { type: 'paragraph', content: ['X'] },
      { type: 'paragraph', content: [{ text: 'cd', marks: ['bold'] }] },
    ]);
  });

  it('places the opening caret at the end without recording history', () => {
    const created = editor({ content: '<p>Hello</p>' });
    selectNoteEnd(created);
    expect(created.state.doc.textContent).toBe('Hello');
    expect(created.state.selection.empty).toBe(true);
    expect(created.state.selection.from).toBe(created.state.doc.content.size - 1);
    expect(undoDepth(created.state)).toBe(0);
  });

  it('uses a gap cursor after a final image without recording history', () => {
    const created = editor({
      content: {
        type: 'doc',
        content: [{ type: 'image', attrs: { src: 'data:image/png;base64,aaaa' } }],
      },
    });
    selectNoteEnd(created);
    expect(created.state.selection).toBeInstanceOf(GapCursor);
    expect(created.state.selection.from).toBe(created.state.doc.content.size);
    expect(undoDepth(created.state)).toBe(0);
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
    expect(insertPromptParagraph(created, 'X')).toBe(true);
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
    expect(insertPromptParagraph(created, 'Y')).toBe(true);
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
    insertPromptParagraph(created, 'X');
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
    insertPromptParagraph(created, 'X');
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
    insertPromptParagraph(created, 'X');
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

  it('adds a new item when the removed start item leaves the list', () => {
    const created = editor({ content: '<ul><li><p>one</p></li><li><p>two</p></li></ul><p>after</p>' });
    selectBetween(created, 'two', 0, 'after', 2);
    expect(insertPromptParagraph(created, 'X')).toBe(true);
    expect(docOf(created)).toEqual([
      {
        type: 'bulletList',
        content: [
          { type: 'listItem', content: [{ type: 'paragraph', content: ['one'] }] },
          { type: 'listItem', content: [{ type: 'paragraph', content: ['X'] }] },
        ],
      },
      { type: 'paragraph', content: ['ter'] },
    ]);
    expect(created.state.selection.$from.parent.textContent).toBe('X');
    expect(created.state.selection.$from.parentOffset).toBe(1);
  });

  it('replaces the whole note after Select All', () => {
    const created = editor({ content: '<p>abc</p><ul><li><p>def</p></li></ul>' });
    const before = docOf(created);
    created.commands.selectAll();
    expect(insertPromptParagraph(created, 'X')).toBe(true);
    expect(docOf(created)).toEqual([{ type: 'paragraph', content: ['X'] }]);
    expect(created.state.selection.from).toBe(created.state.doc.content.size - 1);
    created.commands.undo();
    expect(docOf(created)).toEqual(before);
  });

  it('keeps the insertion in a nested start item', () => {
    const nestedItem = {
      type: 'bulletList',
      content: [{
        type: 'listItem',
        content: [
          { type: 'paragraph', content: ['b'] },
          { type: 'paragraph', content: ['X'] },
        ],
      }],
    };
    const created = editor();
    created.commands.setContent('<ul><li><p>A</p><ul><li><p>bc</p></li></ul></li></ul><p>de</p>');
    selectBetween(created, 'bc', 1, 'de', 1);
    expect(insertPromptParagraph(created, 'X')).toBe(true);
    expect(docOf(created)).toEqual([
      {
        type: 'bulletList',
        content: [{ type: 'listItem', content: [{ type: 'paragraph', content: ['A'] }, nestedItem] }],
      },
      { type: 'paragraph', content: ['e'] },
    ]);

    created.commands.setContent(
      '<ul><li><p>A</p><ul><li><p>bc</p></li></ul></li><li><p>de</p></li></ul>',
    );
    selectBetween(created, 'bc', 1, 'de', 1);
    expect(insertPromptParagraph(created, 'X')).toBe(true);
    expect(docOf(created)).toEqual([{
      type: 'bulletList',
      content: [
        { type: 'listItem', content: [{ type: 'paragraph', content: ['A'] }, nestedItem] },
        { type: 'listItem', content: [{ type: 'paragraph', content: ['e'] }] },
      ],
    }]);
  });

  it('replaces across two blocks that share one node object', () => {
    const created = editor({ content: '<p>abcd</p>' });
    const first = created.state.doc.firstChild!;
    created.view.dispatch(created.state.tr.insert(created.state.doc.content.size, first));
    expect(created.state.doc.child(1)).toBe(first);
    created.view.dispatch(created.state.tr.setSelection(
      TextSelection.create(created.state.doc, 3, first.nodeSize + 3),
    ));
    expect(insertPromptParagraph(created, 'X')).toBe(true);
    expect(docOf(created)).toEqual([
      { type: 'paragraph', content: ['ab'] },
      { type: 'paragraph', content: ['X'] },
      { type: 'paragraph', content: ['cd'] },
    ]);
  });

  it('keeps literal text, links the final url, and continues before the old suffix', () => {
    const created = editor();
    created.commands.setContent('<p>abcd</p>');
    selectText(created, 'abcd', 2);
    const original = created.state.selection.toJSON();
    expect(insertPromptParagraph(created, '**bold**\r\nsee https://example.com/a')).toBe(true);
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
    expect(insertPromptParagraph(created, 'Z')).toBe(true);
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

  it('links bare domains and explicit http and https urls, like typing and paste', () => {
    const created = editor();
    insertPromptParagraph(
      created,
      'http://example.com example.com file.txt HTTP://Example.COM/a mailto:user.name@example.com javascript:alert(1) file:///tmp/a data:text/plain,hi cha://app https://example.com/a.',
    );
    expect(docOf(created)).toEqual([{
      type: 'paragraph',
      content: [
        { text: 'http://example.com', marks: ['link:http://example.com/'] },
        ' ',
        { text: 'example.com', marks: ['link:http://example.com/'] },
        ' file.txt ',
        { text: 'HTTP://Example.COM/a', marks: ['link:http://example.com/a'] },
        ' mailto:user.name@example.com javascript:alert(1) file:///tmp/a data:text/plain,hi cha://app ',
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
    insertPromptParagraph(created, 'X');
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
    const errors: string[] = [];
    const created = editor({ byteLimit: 80, onCommandError: (message) => errors.push(message) });
    expect(insertPromptParagraph(created, 'x'.repeat(200))).toBe(false);
    expect(docOf(created)).toEqual([{ type: 'paragraph' }]);
    expect(undoDepth(created.state)).toBe(0);
    expect(errors).toEqual([noteTooLargeMessage]);
  });

  it('does not autolink a final url unless the insertion command adds the mark', () => {
    const created = editor();
    insertText(created, 'https://example.com/a');
    expect(JSON.stringify(created.getJSON())).not.toContain('"type":"link"');
    created.commands.setContent('<p></p>');
    insertPromptParagraph(created, 'https://example.com/a');
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

  it('rejects invalid images in a paste or a non-file drop', () => {
    const saves: string[] = [];
    const created = editor({
      content: '<p>keep</p>',
      onAcceptedDocument: () => saves.push('save'),
    });
    selectText(created, 'keep', 0, 4);
    const before = created.getJSON();
    const selection = created.state.selection.toJSON();
    const depth = undoDepth(created.state);
    const slice = imageSlice(created);
    expect(pasteSlice(created, slice)).toBe(true);
    expect(dropSlice(created, slice, { files: { length: 0 }, types: ['text/html'] })).toBe(true);
    expect(created.getJSON()).toEqual(before);
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(depth);
    expect(saves).toEqual([]);
  });

  it('consumes file drops without insertion', () => {
    const created = editor({ content: '<p>keep</p>' });
    const before = created.getJSON();
    expect(dropSlice(created, imageSlice(created), {
      files: { length: 1 }, types: ['Files'],
    })).toBe(true);
    expect(created.getJSON()).toEqual(before);
  });

  it('consumes an empty paste slice over selected text without dispatch', () => {
    const saves: string[] = [];
    const created = editor({
      content: '<p>keep this</p>',
      onAcceptedDocument: () => saves.push('save'),
    });
    selectText(created, 'keep this', 0, 9);
    const before = created.getJSON();
    const selection = created.state.selection.toJSON();
    const depth = undoDepth(created.state);
    expect(pasteSlice(created, Slice.empty)).toBe(true);
    expect(created.getJSON()).toEqual(before);
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(depth);
    expect(saves).toEqual([]);
  });

  it('serializes a highlighted selection for the clipboard', () => {
    const created = editor({ content: '<p>keep <mark>this</mark> text</p>' });
    selectText(created, 'this', 0, 4);
    const copied = created.view.serializeForClipboard(created.state.selection.content());
    expect(copied.text).toBe('this');
    expect(copied.dom.querySelector('mark')?.textContent).toBe('this');
    expect(created.view.dom.querySelector('mark')).not.toBeNull();
  });

  it('marks the current selection while the editor is inactive', async () => {
    const created = editor({ content: '<p>hello</p>' });
    const before = created.getJSON();
    expect(created.view.dom.querySelector('.cha-notes-inactive-caret')).not.toBeNull();
    selectText(created, 'hello', 1, 4);
    expect(created.state.doc.textBetween(created.state.selection.from, created.state.selection.to)).toBe('ell');
    expect(created.view.dom.querySelector('.cha-notes-inactive-selection')).not.toBeNull();
    expect(created.getJSON()).toEqual(before);
    expect(undoDepth(created.state)).toBe(0);

    created.commands.setSearchTerm('ell');
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(created.state.doc.textBetween(created.state.selection.from, created.state.selection.to)).toBe('ell');
    expect(created.view.dom.querySelector('.cha-notes-inactive-selection')).not.toBeNull();
    expect(undoDepth(created.state)).toBe(0);

    created.view.focus();
    await Promise.resolve();
    expect(created.view.dom.querySelector('.cha-notes-inactive-selection')).toBeNull();
    expect(created.view.dom.querySelector('.cha-notes-inactive-caret')).toBeNull();
    created.view.dom.blur();
    await Promise.resolve();
    expect(created.view.dom.querySelector('.cha-notes-inactive-selection')).not.toBeNull();
    expect(created.getJSON()).toEqual(before);
    expect(undoDepth(created.state)).toBe(0);
  });

  it('marks a selected image and a gap cursor without saving the marker', () => {
    const saves: string[] = [];
    const created = editor({
      content: {
        type: 'doc',
        content: [{ type: 'image', attrs: { src: 'data:image/png;base64,aaaa' } }],
      },
      onAcceptedDocument: () => saves.push('save'),
    });
    const imagePos = 0;
    created.view.dispatch(created.state.tr.setSelection(NodeSelection.create(created.state.doc, imagePos)).setMeta('addToHistory', false));
    expect(created.view.dom.querySelector('.cha-notes-inactive-node')).not.toBeNull();
    selectNoteEnd(created);
    expect(created.state.selection).toBeInstanceOf(GapCursor);
    expect(created.view.dom.querySelector('.cha-notes-inactive-caret')).not.toBeNull();
    expect(undoDepth(created.state)).toBe(0);
    expect(saves).toEqual([]);
  });

  it('matches across marks inside one textblock', () => {
    const created = editor({
      content: {
        type: 'doc',
        content: [{
          type: 'paragraph',
          content: [
            { type: 'text', text: 'c', marks: [{ type: 'bold' }] },
            { type: 'text', text: 'at' },
          ],
        }],
      },
    });
    created.commands.setSearchTerm('cat');
    expect(created.storage.findAndReplace.results).toHaveLength(1);
    expect(created.state.doc.textBetween(
      created.storage.findAndReplace.results[0].from,
      created.storage.findAndReplace.results[0].to,
    )).toBe('cat');
  });

  it('rejects a replacement that exceeds the note limit', () => {
    const probe = editor({ content: '<p>cat</p>' });
    const limit = noteDocumentByteLength(probe.state.doc);
    const created = editor({ content: '<p>cat</p>', byteLimit: limit });
    const before = created.getJSON();
    const selection = created.state.selection.toJSON();
    const depth = undoDepth(created.state);
    created.commands.setSearchTerm('cat');
    created.commands.setReplaceTerm('catcat');
    withIsolatedHistory(created, () => created.commands.replace());
    expect(created.getJSON()).toEqual(before);
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(depth);
  });
});

describe('pictures and clipboard', () => {
  it.each([
    ['https://example.com/path', 'https://example.com/path'],
    ['http://example.com/path', 'http://example.com/path'],
    ['example.org', 'http://example.org'],
  ])('links pasted %s and keeps the whole paste in one Undo step', (url, href) => {
    const accepted = vi.fn();
    const created = editor({ onAcceptedDocument: accepted });
    insertText(created, 'Before ');
    const before = created.getJSON();
    const depth = undoDepth(created.state);
    accepted.mockClear();
    const text = `Read ${url} for **details**`;
    created.view.pasteText(text);
    const pasted = created.getJSON();
    expect(created.getText()).toBe(`Before ${text}`);
    expect(created.view.dom.querySelector('a')?.getAttribute('href')).toBe(href);
    expect(created.view.dom.querySelector('strong')).toBeNull();
    expect(accepted).toHaveBeenCalledOnce();
    expect(undoDepth(created.state)).toBe(depth + 1);
    created.commands.undo();
    expect(created.getJSON()).toEqual(before);
    created.commands.redo();
    expect(created.getJSON()).toEqual(pasted);
  });

  it('rejects a URL paste atomically when its added link marks exceed the limit', () => {
    const text = 'Read https://example.com for details';
    const plain = editor({ content: `<p>${text}</p>` });
    const rejected = vi.fn();
    const accepted = vi.fn();
    const created = editor({
      byteLimit: noteDocumentByteLength(plain.state.doc),
      onCommandError: rejected,
      onAcceptedDocument: accepted,
    });
    const before = created.getJSON();
    const selection = created.state.selection.toJSON();
    created.view.pasteText(text);
    expect(rejected).toHaveBeenCalledWith(noteTooLargeMessage);
    expect(accepted).not.toHaveBeenCalled();
    expect(created.getJSON()).toEqual(before);
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(0);
  });

  it('inserts one valid image and undoes it', () => {
    const created = editor({ content: '<p>keep</p>' });
    const src = pngSrc();
    const depth = undoDepth(created.state);
    expect(created.view.pasteHTML(`<img src="${src}">`)).toBe(true);
    expect(JSON.stringify(created.getJSON())).toContain(src);
    expect(undoDepth(created.state)).toBe(depth + 1);
    created.commands.undo();
    expect(created.getText()).toBe('keep');
    created.commands.redo();
    expect(JSON.stringify(created.getJSON())).toContain(src);
  });

  it('rejects a mixed paste that contains an invalid image', () => {
    const errors: string[] = [];
    const created = editor({
      content: '<p>keep</p>',
      onCommandError: (message) => errors.push(message),
    });
    selectText(created, 'keep', 0, 4);
    const before = created.getJSON();
    const depth = undoDepth(created.state);
    expect(created.view.pasteHTML('<p>NEW <img src="data:image/png;base64,aa"> END</p>')).toBe(true);
    expect(created.getJSON()).toEqual(before);
    expect(undoDepth(created.state)).toBe(depth);
    expect(errors).toEqual([pictureTypeMessage]);
  });

  it('uses plain text when the clipboard image type is unsupported', () => {
    const created = editor({ content: '<p>keep</p>' });
    selectText(created, 'keep', 0, 4);
    const file = new File([Uint8Array.from([1])], 'a.svg', { type: 'image/svg+xml' });
    expect(pasteWith(created, Slice.empty, { files: [file], text: 'ALT' })).toBe(true);
    expect(created.getText()).toBe('ALT');
    expect(undoDepth(created.state)).toBe(1);
  });

  it('uses plain text once when filtered HTML is empty', () => {
    const created = editor({ content: '<p>keep</p>' });
    selectText(created, 'keep', 0, 4);
    expect(pasteWith(created, Slice.empty, { text: 'ALT' })).toBe(true);
    expect(created.getText()).toBe('ALT');
    expect(undoDepth(created.state)).toBe(1);
  });

  it('imports one clipboard image and does not also insert its text', async () => {
    const created = editor({ content: '<p>keep</p>' });
    const file = new File([Uint8Array.from(pngHeader)], 'a.png', { type: 'image/png' });
    expect(pasteWith(created, Slice.empty, { files: [file], text: 'ALT' })).toBe(true);
    await vi.waitFor(() => {
      expect(JSON.stringify(created.getJSON())).toContain('data:image/png');
    });
    expect(created.getText()).toContain('keep');
    expect(created.getText()).not.toContain('ALT');
    expect(undoDepth(created.state)).toBe(1);
    created.commands.undo();
    expect(created.getText()).toBe('keep');
    expect(JSON.stringify(created.getJSON())).not.toContain('data:image/png');
  });

  it('does not fall back to text when clipboard image bytes are rejected', async () => {
    const errors: string[] = [];
    const created = editor({
      content: '<p>keep</p>',
      onCommandError: (message) => errors.push(message),
    });
    const file = new File([Uint8Array.from([1, 2, 3, 4])], 'a.png', { type: 'image/png' });
    expect(pasteWith(created, Slice.empty, { files: [file], text: 'ALT' })).toBe(true);
    await vi.waitFor(() => expect(errors).toEqual([pictureTypeMessage]));
    expect(created.getText()).toBe('keep');
    expect(undoDepth(created.state)).toBe(0);
  });

  it('keeps a selection when unsupported paste has no text alternative', () => {
    const created = editor({ content: '<p>keep</p>' });
    selectText(created, 'keep', 0, 4);
    const before = created.getJSON();
    const selection = created.state.selection.toJSON();
    expect(created.view.pasteHTML('<script>no()</script><img src="https://example.com/a.png">')).toBe(true);
    expect(created.getJSON()).toEqual(before);
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(0);
  });

  it('omits a remote image and keeps the surrounding text', () => {
    const created = editor({ content: '<p></p>' });
    expect(created.view.pasteHTML('<p>hello <img src="https://example.com/a.png"> there</p>')).toBe(true);
    expect(created.getText()).toContain('hello');
    expect(created.getText()).toContain('there');
    expect(JSON.stringify(created.getJSON())).not.toContain('https://example.com/a.png');
  });

  it('flattens a table and keeps a bold cell', () => {
    const created = editor({ content: '<p></p>' });
    created.view.pasteHTML('<table><tr><td><b>A</b></td><td>B</td></tr><tr><td>C</td><td>D</td></tr></table>');
    expect(created.getText({ blockSeparator: '\n' })).toContain('A\tB');
    expect(created.getText({ blockSeparator: '\n' })).toContain('C\tD');
    expect(JSON.stringify(created.getJSON())).toContain('"type":"bold"');
  });

  it('separates table cells with tabs and paragraphs within a cell with line breaks', () => {
    const created = editor();
    const before = created.getJSON();
    created.view.pasteHTML(
      '<table><tr><td><p><b>A</b></p></td><td><p>B</p><p>B2</p></td></tr>'
      + '<tr><td><div>C</div></td><td></td><td><div>D</div></td></tr></table>',
    );
    expect(created.getText({ blockSeparator: '\n' })).toBe('A\tB\nB2\nC\t\tD');
    expect(created.view.dom.querySelector('strong')?.textContent).toBe('A');
    const pasted = created.getJSON();
    expect(undoDepth(created.state)).toBe(1);
    created.commands.undo();
    expect(created.getJSON()).toEqual(before);
    created.commands.redo();
    expect(created.getJSON()).toEqual(pasted);
  });

  it('keeps web highlights and Word highlights, and drops web background colors', () => {
    const web = editor({ content: '<p></p>' });
    web.view.pasteHTML('<p>say <mark>hi</mark></p>');
    expect(JSON.stringify(web.getJSON())).toContain('"type":"highlight"');

    const page = editor({ content: '<p></p>' });
    page.view.pasteHTML('<p><span style="background-color:yellow">hi</span></p>');
    expect(JSON.stringify(page.getJSON())).not.toContain('highlight');
    expect(page.getText()).toContain('hi');

    const word = editor({ content: '<p></p>' });
    word.view.pasteHTML(
      '<html xmlns:o="urn:schemas-microsoft-com:office:office"><p><span style="background-color:yellow">hi</span></p></html>',
    );
    expect(JSON.stringify(word.getJSON())).toContain('"type":"highlight"');
    const marked = editor({ content: '<p></p>' });
    marked.view.pasteHTML(
      '<html xmlns:o="urn:schemas-microsoft-com:office:office"><p><span style="mso-highlight:yellow">yo</span></p></html>',
    );
    expect(JSON.stringify(marked.getJSON())).toContain('"type":"highlight"');

    const mentioned = editor({ content: '<p></p>' });
    mentioned.view.pasteHTML('<p>Microsoft Word <span style="background-color:yellow">hi</span></p>');
    expect(JSON.stringify(mentioned.getJSON())).not.toContain('highlight');
    expect(mentioned.getText()).toContain('hi');

    const generated = editor({ content: '<p></p>' });
    generated.view.pasteHTML(
      '<meta name="Generator" content="Microsoft Word 15"><p><span style="background-color:yellow">hi</span></p>',
    );
    expect(JSON.stringify(generated.getJSON())).toContain('"type":"highlight"');

    const program = editor({ content: '<p></p>' });
    program.view.pasteHTML(
      '<meta name=ProgId content=Word.Document><p><span style="background-color:yellow">yo</span></p>',
    );
    expect(JSON.stringify(program.getJSON())).toContain('"type":"highlight"');
  });

  it('preserves an internal slice and rewrites pasted links', () => {
    const created = editor({ content: '<p><strong>Bold</strong> <mark>hi</mark></p>' });
    created.commands.selectAll();
    const copied = created.view.serializeForClipboard(created.state.selection.content());
    expect(copied.dom.innerHTML).toContain('data-pm-slice');
    expect(transformClipboardHTML(copied.dom.innerHTML)).toContain('data-pm-slice');
    const next = editor({ content: '<p></p>' });
    next.view.pasteHTML(copied.dom.innerHTML);
    expect(JSON.stringify(next.getJSON())).toContain('"type":"bold"');
    expect(JSON.stringify(next.getJSON())).toContain('"type":"highlight"');

    const links = editor({ content: '<p></p>' });
    links.view.pasteHTML('<p><a href="https://example.com/docs">Docs</a> <a href="example.com">Bare</a> <a href="javascript:alert(1)">Bad</a></p>');
    expect(links.getText()).toContain('Docs');
    expect(links.getText()).toContain('Bare');
    expect(links.getText()).toContain('Bad');
    expect(JSON.stringify(links.getJSON())).toContain('https://example.com/docs');
    expect(JSON.stringify(links.getJSON())).toContain('http://example.com/');
    expect(JSON.stringify(links.getJSON())).not.toContain('javascript');
    expect(transformClipboardHTML('<p><font color="red">Hi</font><script>bad()</script></p>')).not.toContain('color');
    expect(transformClipboardHTML('<p>Hi</p><iframe src="https://example.com"></iframe>')).not.toContain('iframe');
  });

  it('lets a valid non-file image drop through and consumes every file drop', () => {
    const created = editor({ content: '<p>keep</p>' });
    const before = created.getJSON();
    const selection = created.state.selection.toJSON();
    const depth = undoDepth(created.state);
    const valid = new Slice(
      Fragment.from(created.schema.nodes.image.create({ src: pngSrc() })),
      0,
      0,
    );
    expect(dropSlice(created, valid, { files: { length: 0 }, types: ['text/html'] })).toBe(false);
    expect(dropSlice(created, valid, { files: { length: 1 }, types: ['Files', 'text/html'] })).toBe(true);
    const text = new Slice(
      Fragment.from(created.schema.nodes.paragraph.create(null, created.schema.text('NEW'))),
      0,
      0,
    );
    expect(dropSlice(created, text, { files: { length: 1 }, types: ['Files', 'text/plain'] })).toBe(true);
    expect(dropSlice(created, Slice.empty, { files: { length: 0 }, types: ['text/plain'] })).toBe(true);
    expect(created.getJSON()).toEqual(before);
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(depth);
  });

  it('inserts at the mapped bookmark and ignores a later caret', () => {
    const created = editor({ content: '<p>abcd</p>' });
    const src = pngSrc();
    selectText(created, 'abcd', 1);
    const tracked = trackSelectionBookmark(created);
    selectText(created, 'abcd', 4);
    expect(insertTrackedPicture(created, tracked.current(), src)).toBe('inserted');
    expect(created.getText({ blockSeparator: '|' })).toBe('a||bcd');
    created.commands.undo();

    selectText(created, 'abcd', 1);
    const mapped = trackSelectionBookmark(created);
    insertText(created, 'Z');
    expect(insertTrackedPicture(created, mapped.current(), src)).toBe('inserted');
    expect(created.getText({ blockSeparator: '|' })).toBe('aZ||bcd');
    expect(undoDepth(created.state)).toBe(2);
  });

  it('does not insert into a frozen editor or a stale context', async () => {
    const created = editor({ content: '<p>abcd</p>' });
    const src = pngSrc();
    selectText(created, 'abcd', 1);
    created.setEditable(false, false);
    expect(insertTrackedPicture(created, created.state.selection.getBookmark(), src)).toBe('unavailable');
    expect(created.getText()).toBe('abcd');
    created.setEditable(true, false);

    const file = new File([Uint8Array.from(pngHeader)], 'a.png', { type: 'image/png' });
    await expect(importPictureFile(created, file, () => false)).resolves.toBeNull();
    expect(created.getText()).toBe('abcd');
    expect(undoDepth(created.state)).toBe(0);
  });

  it('rejects an image that makes the note too large and keeps the selection', async () => {
    const probe = editor({ content: '<p>abcd</p>' });
    const limit = noteDocumentByteLength(probe.state.doc);
    const errors: string[] = [];
    const created = editor({
      content: '<p>abcd</p>',
      byteLimit: limit,
      onCommandError: (message) => errors.push(message),
    });
    selectText(created, 'abcd', 1);
    const before = created.getJSON();
    const selection = created.state.selection.toJSON();
    const depth = undoDepth(created.state);
    const file = new File([Uint8Array.from(pngHeader)], 'a.png', { type: 'image/png' });
    await expect(importPictureFile(created, file, () => true)).resolves.toBeNull();
    expect(errors).toEqual([noteTooLargeMessage]);
    expect(insertTrackedPicture(created, created.state.selection.getBookmark(), pngSrc())).toBe('rejected');
    expect(created.getJSON()).toEqual(before);
    expect(created.state.selection.toJSON()).toEqual(selection);
    expect(undoDepth(created.state)).toBe(depth);
    const over = new Uint8Array(1_048_577);
    over.set(pngHeader);
    await expect(importPictureFile(
      created,
      new File([over], 'big.png', { type: 'image/png' }),
      () => true,
    )).resolves.toBe(pictureTooLargeMessage);
  });
});

describe('link activation and edits', () => {
  it('opens the stored http destination and blocks a damaged link', () => {
    const openWindow = vi.spyOn(window, 'open').mockReturnValue(null);
    const created = editor({
      content: {
        type: 'doc',
        content: [{
          type: 'paragraph',
          content: [{
            type: 'text',
            text: 'Go',
            marks: [{ type: 'link', attrs: { href: 'HTTP://Example.COM/a' } }],
          }],
        }],
      },
    });
    expect(clickAt(created, anchorOf(created), 0)).toBe(true);
    expect(openWindow).toHaveBeenCalledWith('http://example.com/a', '_blank', 'noopener,noreferrer');
    expect(clickAt(created, anchorOf(created), 2)).toBe(false);
    expect(openWindow).toHaveBeenCalledTimes(1);

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
    expect(created.getText()).toBe('click');
    expect(clickAt(created, anchorOf(created), 0)).toBe(true);
    expect(openWindow).toHaveBeenCalledTimes(1);
  });

  it('ignores a click beside a link and cancels the native link action', () => {
    const openWindow = vi.spyOn(window, 'open').mockReturnValue(null);
    const created = editor({
      content: {
        type: 'doc',
        content: [{
          type: 'paragraph',
          content: [
            { type: 'text', text: 'see ' },
            { type: 'text', text: 'Go', marks: [{ type: 'link', attrs: { href: 'https://example.com/' } }] },
          ],
        }],
      },
    });
    const paragraph = created.view.dom.querySelector('p');
    if (!paragraph) throw new Error('Paragraph is missing.');
    expect(clickAt(created, paragraph, 0)).toBe(false);
    expect(openWindow).not.toHaveBeenCalled();

    created.setEditable(false, false);
    const click = new MouseEvent('click', { bubbles: true, cancelable: true });
    anchorOf(created).dispatchEvent(click);
    expect(click.defaultPrevented).toBe(true);
  });

  it('autolinks a typed bare domain and does not autolink an unsupported scheme', () => {
    const created = editor();
    insertText(created, 'example.com ');
    expect(docOf(created)).toEqual([{
      type: 'paragraph',
      content: [
        { text: 'example.com', marks: ['link:http://example.com'] },
        ' ',
      ],
    }]);
    created.commands.setContent('<p></p>');
    insertText(created, 'javascript:alert(1) ');
    expect(JSON.stringify(created.getJSON())).not.toContain('"type":"link"');
    created.commands.setContent('<p></p>');
    insertText(created, 'mailto:user@example.com ');
    expect(JSON.stringify(created.getJSON())).not.toContain('"type":"link"');
  });

  it('removes a link without recreating it on reopen or an unrelated edit', () => {
    const created = editor();
    insertText(created, 'example.com ');
    const range = linkRangeAt(created.state.doc, 1);
    expect(range?.href).toBe('http://example.com');
    expect(removeNoteLink(created, range!)).toBe(true);
    expect(JSON.stringify(created.getJSON())).not.toContain('"type":"link"');
    created.commands.setTextSelection(created.state.doc.content.size - 1);
    insertText(created, 'x ');
    expect(JSON.stringify(created.getJSON())).not.toContain('"type":"link"');
    const reopened = editor({ content: created.getJSON() });
    expect(JSON.stringify(reopened.getJSON())).not.toContain('"type":"link"');
    expect(undoDepth(created.state)).toBeGreaterThan(0);
    created.commands.undo();
    created.commands.undo();
    expect(JSON.stringify(created.getJSON())).toContain('"type":"link"');
  });

  it('changes only the destination and keeps the other marks', () => {
    const created = editor({
      content: {
        type: 'doc',
        content: [{
          type: 'paragraph',
          content: [{
            type: 'text',
            text: 'docs',
            marks: [
              { type: 'bold' },
              { type: 'link', attrs: { href: 'https://example.com/a' } },
            ],
          }],
        }],
      },
    });
    const range = linkRangeAt(created.state.doc, 1)!;
    const depth = undoDepth(created.state);
    expect(replaceLinkDestination(created, range, 'mailto:user@example.com')).toBe('invalid');
    expect(undoDepth(created.state)).toBe(depth);
    expect(replaceLinkDestination(created, range, 'http://example.org/b')).toBe('applied');
    expect(created.getText()).toBe('docs');
    expect(JSON.stringify(created.getJSON())).toContain('http://example.org/b');
    expect(JSON.stringify(created.getJSON())).toContain('"type":"bold"');
    expect(JSON.stringify(created.getJSON())).not.toContain('https://example.com/a');
    created.commands.undo();
    expect(JSON.stringify(created.getJSON())).toContain('https://example.com/a');
  });

  it('changes nothing when the captured link moved before Apply', () => {
    const created = editor({
      content: {
        type: 'doc',
        content: [{
          type: 'paragraph',
          content: [
            { type: 'text', text: 'ab ' },
            { type: 'text', text: 'docs', marks: [{ type: 'link', attrs: { href: 'https://example.com/a' } }] },
          ],
        }],
      },
    });
    const range = linkRangeAt(created.state.doc, 4)!;
    created.view.dispatch(created.state.tr.insertText('X', 1));
    const before = created.getJSON();
    const depth = undoDepth(created.state);
    expect(replaceLinkDestination(created, range, 'http://example.org/b')).toBe('missing');
    expect(created.getJSON()).toEqual(before);
    expect(undoDepth(created.state)).toBe(depth);
  });
});
