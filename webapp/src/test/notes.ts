import type { JSONContent } from '@tiptap/core';
import type { NoteItem } from '../api/client';

export function noteItem(overrides: Partial<NoteItem> = {}): NoteItem {
  return { id: 1, parent_id: null, kind: 'note', name: 'Article', has_compressed: false,
    has_audio: false, read_only: false, is_summary: false, summary_linked: false, ...overrides };
}

export const textNode = (text: string): JSONContent => ({ type: 'text', text });
export const paragraph = (text: string): JSONContent => ({ type: 'paragraph', content: [textNode(text)] });
export const noteDocument = (...content: JSONContent[]) => ({ type: 'doc', content });
