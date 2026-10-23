import { isNoteComment } from './editor';
import type { AnnotationDestination } from './annotationSource';
import type { Editor } from '@tiptap/core';
import type { Node as PmNode } from '@tiptap/pm/model';
import { TextSelection } from '@tiptap/pm/state';

export type NoteHighlight = { from: number; to: number; text: string };

// Formatting and line breaks can split a highlighted passage into several
// adjacent inline nodes.
export function noteHighlights(doc: PmNode): NoteHighlight[] {
  return highlightPassages(doc, false);
}

function highlightPassages(doc: PmNode, textOnly: boolean): NoteHighlight[] {
  const highlights: NoteHighlight[] = [];
  doc.descendants((node, position) => {
    if (!node.isInline || !node.marks.some((mark) => mark.type.name === 'highlight')) return;
    if (textOnly && !node.isText && node.type.name !== 'hardBreak') return;
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

export type NoteAnnotation = NoteHighlight & { entryType: 'comment' | 'highlight'; ordinal: number };
export function noteAnnotations(doc: PmNode): NoteAnnotation[] {
  const result: NoteAnnotation[] = [];
  let comments = 0;
  let highlights = 0;
  doc.descendants((node, position) => {
    const comment: boolean = isNoteComment(node);
    if (comment) {
      const text = node.textBetween(0, node.content.size, '', '\n');
      result.push({ entryType: 'comment', ordinal: ++comments, text, from: position + 1, to: position + 1 + node.content.size });
      return false;
    }
    if (!node.isTextblock) return;
    // Comments are excluded above. Other passages use the Highlights rule.
    for (const passage of highlightPassages(node, true)) {
      result.push({
        entryType: 'highlight',
        ordinal: ++highlights,
        text: passage.text,
        from: position + 1 + passage.from,
        to: position + 1 + passage.to,
      });
    }
    return false;
  });
  return result;
}
export function selectNoteAnnotation(editor: Editor, target: AnnotationDestination): boolean {
  const range = noteAnnotations(editor.state.doc).find((entry) => entry.entryType === target.entryType && entry.ordinal === target.ordinal);
  if (!range || !range.text.startsWith(target.prefix)) return false;
  editor.view.dispatch(editor.state.tr.setSelection(TextSelection.create(editor.state.doc, range.from, range.to)).setMeta('addToHistory', false));
  return true;
}
