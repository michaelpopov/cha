import { afterEach, describe, expect, it } from 'vitest';
import type { Editor } from '@tiptap/core';

import { createNotesEditor } from './editor';
import { noteHighlights, selectNoteHighlight } from './highlights';

let editor: Editor;
afterEach(() => editor?.destroy());

describe('note highlights', () => {
  it('joins formatting fragments, preserves duplicate passages, and uses document positions', () => {
    editor = createNotesEditor({
      readPicture: async () => new Uint8Array(),
      content: '<h2>Heading</h2><p><mark>One <strong>two</strong> 😀</mark> gap <mark>One two 😀</mark></p>'
        + '<ul><li><p><mark>Nested</mark></p></li></ul><p><mark>  </mark></p>',
    });
    const entries = noteHighlights(editor.state.doc);
    expect(entries.map(({ text }) => text)).toEqual(['One two 😀', 'One two 😀', 'Nested']);
    for (const { from, to, text } of entries) {
      expect(editor.state.doc.textBetween(from, to)).toBe(text);
    }
    const before = editor.getJSON();
    editor.setEditable(false, false);
    expect(selectNoteHighlight(editor, entries[1])).toBe(true);
    expect(editor.state.selection.from).toBe(entries[1].from);
    expect(editor.state.selection.to).toBe(entries[1].to);
    expect(editor.isEditable).toBe(false);
    expect(editor.getJSON()).toEqual(before);
    expect(editor.can().undo()).toBe(false);
  });

  it('does not select a stale passage after its text or highlight changes', () => {
    editor = createNotesEditor({ readPicture: async () => new Uint8Array(), content: '<p><mark>Alpha</mark></p>' });
    const [entry] = noteHighlights(editor.state.doc);
    editor.commands.setContent('<p><mark>Bravo</mark></p>');
    expect(selectNoteHighlight(editor, entry)).toBe(false);
    editor.commands.setContent('<p>Alpha</p>');
    expect(selectNoteHighlight(editor, entry)).toBe(false);
  });
});
