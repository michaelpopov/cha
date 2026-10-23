import userEvent from '@testing-library/user-event';
import fixture from '../../../tests/fixtures/notes/annotations.json';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { generateText, type Editor } from '@tiptap/core';
import { TextSelection } from '@tiptap/pm/state';
import { createNotesEditor, notesExtensions, toggleNoteHighlight, addNoteComment } from './editor';
import { noteAnnotations, selectNoteAnnotation } from './highlights';
import { sortAnnotationGroups } from './annotationSource';
import { availableNoteName, compareNoteSiblings } from './tree';


const editors: Editor[] = [];
function create(options: Parameters<typeof createNotesEditor>[0]) {
  const editor = createNotesEditor(options);
  editors.push(editor);
  return editor;
}
afterEach(() => editors.splice(0).forEach((editor) => editor.destroy()));
const readPicture = async () => new Uint8Array();
const generated = { type: 'doc', content: [
  { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text: 'Source' }] },
  { type: 'paragraph', content: [
    { type: 'annotationSource', attrs: { sourceId: 42, entryType: 'comment', ordinal: 1 } },
    { type: 'hardBreak' }, { type: 'text', text: 'Comment' }, { type: 'hardBreak' }, { type: 'hardBreak' },
  ] },
] };

describe('annotations', () => {
  it('matches the shared backend fixture and selects full source ranges without document or history changes', () => {
    const editor = create({ content: fixture.document, readPicture });
    const rejected = vi.fn();
    create({ content: fixture.generated_document, readPicture, onRejectedContent: rejected });
    expect(rejected).not.toHaveBeenCalled();
    const ranges = noteAnnotations(editor.state.doc);
    expect(ranges.map(({ entryType, ordinal, text }) => ({ entryType, ordinal, text }))).toEqual(fixture.entries);
    editor.setEditable(false);
    for (const range of ranges) {
      expect(selectNoteAnnotation(editor, { ...range, prefix: range.text.slice(0, 3) })).toBe(true);
      expect(editor.state.doc.textBetween(editor.state.selection.from, editor.state.selection.to, '', '\n')).toBe(range.text);
    }
    const selection = editor.state.selection;
    expect(selectNoteAnnotation(editor, { entryType: 'highlight', ordinal: 200, prefix: 'Gone' })).toBe(false);
    expect(selectNoteAnnotation(editor, { entryType: 'highlight', ordinal: 1, prefix: 'Changed' })).toBe(false);
    expect(editor.state.selection.eq(selection)).toBe(true);
    expect(editor.can().undo()).toBe(false);
    expect(editor.getJSON()).toEqual(fixture.document);
  });

  it('loads generated content, activates the source button, strips controls on copy and rejects all document changes', () => {
    const open = vi.fn();
    const rejected = vi.fn();
    const editor = create({ content: generated, readPicture, immutable: () => true, onOpenAnnotation: open, onRejectedContent: rejected });
    expect(rejected).not.toHaveBeenCalled();
    const button = editor.view.dom.querySelector('button')!;
    button.click();
    expect(open).toHaveBeenCalledWith(42, { entryType: 'comment', ordinal: 1, prefix: '\nComment\n\n' });
    editor.commands.selectAll();
    const copy = editor.view.serializeForClipboard(editor.state.selection.content());
    expect(copy.text).toContain('Comment');
    expect(copy.dom.innerHTML).not.toContain('annotationSource');
    expect(copy.dom.innerHTML).not.toContain('button');
    expect(generateText(generated, notesExtensions())).not.toContain('42');
    const before = editor.getJSON();
    editor.commands.insertContent('EDIT');
    editor.setEditable(false);
    toggleNoteHighlight(editor);
    addNoteComment(editor, 1, 'New comment');
    expect(editor.getJSON()).toEqual(before);
    editor.view.dispatch(editor.state.tr.setSelection(TextSelection.create(editor.state.doc, 1, 3)));
    expect(editor.state.selection.from).toBe(1);
  });

  it('skips non-text inline nodes instead of counting them as highlighted line breaks', () => {
    const editor = create({ readPicture, content: { type: 'doc', content: [
      { type: 'paragraph', content: [
        { type: 'text', text: 'Left', marks: [{ type: 'highlight' }] },
        { type: 'annotationSource', attrs: { sourceId: 42, entryType: 'highlight', ordinal: 1 }, marks: [{ type: 'highlight' }] },
        { type: 'text', text: 'Right', marks: [{ type: 'highlight' }] },
      ] },
    ] } });
    const ranges = noteAnnotations(editor.state.doc);
    expect(ranges.map(({ text, ordinal }) => ({ text, ordinal }))).toEqual([
      { text: 'Left', ordinal: 1 }, { text: 'Right', ordinal: 2 },
    ]);
    expect(selectNoteAnnotation(editor, { entryType: 'highlight', ordinal: 2, prefix: 'Right' })).toBe(true);
    expect(editor.state.doc.textBetween(editor.state.selection.from, editor.state.selection.to)).toBe('Right');
  });


  it('activates source icons from the keyboard without edits or undo steps', async () => {
    const user = userEvent.setup();
    const open = vi.fn();
    const editor = create({ content: generated, readPicture, immutable: () => true, onOpenAnnotation: open });
    editor.setEditable(false);
    const element = editor.options.element as HTMLElement;
    document.body.appendChild(element);
    try {
      const before = editor.getJSON();
      editor.view.dom.querySelector('button')!.focus();
      await user.keyboard('{Enter}');
      expect(open).toHaveBeenCalledOnce();
      expect(open).toHaveBeenCalledWith(42, { entryType: 'comment', ordinal: 1, prefix: '\nComment\n\n' });
      expect(editor.getJSON()).toEqual(before);
      expect(editor.can().undo()).toBe(false);
    } finally { element.remove(); }
  });

  it('shows note groups in folder tree order and keeps unknown groups last', () => {
    const title = (text: string) => ({ type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text }] });
    const entry = (sourceId: number) => ({ type: 'paragraph', content: [
      { type: 'annotationSource', attrs: { sourceId, entryType: 'highlight', ordinal: 1 } }, { type: 'text', text: 'Entry' },
    ] });
    const chapter = { type: 'heading', attrs: { level: 2 }, content: [{ type: 'text', text: 'Primary goal' }] };
    const items = [
      { id: 1, parent_id: 7, kind: 'note' as const, name: 'Михал Михалыч' },
      { id: 2, parent_id: 7, kind: 'note' as const, name: 'Instructions' },
      { id: 3, parent_id: 7, kind: 'note' as const, name: 'apple notes' },
    ];
    const document = { type: 'doc', content: [
      title('Михал Михалыч'), entry(1), title('Gone'), entry(8), title('Instructions'), chapter, entry(2), title('apple notes'), entry(3),
    ] };
    expect(sortAnnotationGroups(document, items).content).toEqual([
      title('apple notes'), entry(3), title('Instructions'), chapter, entry(2), title('Михал Михалыч'), entry(1), title('Gone'), entry(8),
    ]);
    const empty = { type: 'doc', content: [{ type: 'paragraph', content: [{ type: 'text', text: 'No comments or highlights.' }] }] };
    expect(sortAnnotationGroups(empty, items)).toEqual(empty);
  });

  it('keeps annotations last and reserves the exact generated name', () => {
    const items = [ { kind: 'annotation' as const, name: 'ANNOTATIONS' }, { kind: 'note' as const, name: 'Zebra' }, { kind: 'folder' as const, name: 'Folder' } ];
    expect(items.sort(compareNoteSiblings).map((item) => item.name)).toEqual(['Folder', 'Zebra', 'ANNOTATIONS']);
    expect(availableNoteName([], null, 'ANNOTATIONS')).toBe('ANNOTATIONS 1');
    expect(availableNoteName([], null, 'Annotations')).toBe('Annotations');
  });
});

it('opens a maximum-size generated document', () => {
  const document = { type: 'doc', content: [
    { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text: 'Source' }] },
  ] as Record<string, unknown>[] };
  let bytes = JSON.stringify(document).length;
  for (let ordinal = 1; bytes < 10_000_000; ordinal++) {
    const text = { type: 'text', text: '' };
    const entry = { type: 'paragraph', content: [
      { type: 'annotationSource', attrs: { sourceId: 42, entryType: 'highlight', ordinal } }, text,
    ] };
    const overhead = JSON.stringify(entry).length + 1;
    const length = Math.min(4000, 10_000_000 - bytes - overhead);
    text.text = 'x'.repeat(length);
    bytes += overhead + length;
    document.content.push(entry);
  }
  expect(JSON.stringify(document).length).toBe(10_000_000);
  const rejected = vi.fn();
  const started = performance.now();
  const editor = create({ content: document, readPicture, immutable: () => true, onRejectedContent: rejected });
  process.stdout.write(`10 MB annotation editor open: ${Math.round(performance.now() - started)} ms\n`);
  expect(rejected).not.toHaveBeenCalled();
  expect(editor.view.dom.querySelectorAll('button')).toHaveLength(document.content.length - 1);
}, 30_000);
