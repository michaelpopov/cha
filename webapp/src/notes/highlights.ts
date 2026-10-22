import type { Editor } from '@tiptap/core';
import type { Node as PmNode } from '@tiptap/pm/model';
import { TextSelection } from '@tiptap/pm/state';

export type NoteHighlight = { from: number; to: number; text: string };

// Formatting and line breaks can split a highlighted passage into several
// adjacent inline nodes.
export function noteHighlights(doc: PmNode): NoteHighlight[] {
  const highlights: NoteHighlight[] = [];
  doc.descendants((node, position) => {
    if (!node.isInline || !node.marks.some((mark) => mark.type.name === 'highlight')) return;
    const text = node.isText ? node.text! : '\n';
    const previous = highlights.at(-1);
    if (previous?.to === position) {
      previous.to += node.nodeSize;
      previous.text += text;
    } else {
      highlights.push({ from: position, to: position + node.nodeSize, text });
    }
  });
  return highlights.filter(({ text }) => text.trim());
}

export function selectNoteHighlight(editor: Editor, highlight: NoteHighlight): boolean {
  if (editor.isDestroyed) return false;
  // A note may have changed since the list was opened. Never jump to unrelated text.
  if (!noteHighlights(editor.state.doc).some((entry) => entry.from === highlight.from
    && entry.to === highlight.to && entry.text === highlight.text)) return false;
  editor.view.dispatch(editor.state.tr
    .setSelection(TextSelection.create(editor.state.doc, highlight.from, highlight.to))
    .setMeta('addToHistory', false));
  return true;
}
