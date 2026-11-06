import type { NoteItem } from './client';
import { isRecord } from './guards';

export function isNoteId(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value >= 1;
}

export function isNoteParent(value: unknown): value is number | null {
  return value === null || isNoteId(value);
}

export function hasExactKeys(value: Record<string, unknown>, keys: readonly string[]): boolean {
  return Object.keys(value).length === keys.length && keys.every((key) => Object.hasOwn(value, key));
}

export function isNoteItem(value: unknown): value is NoteItem {
  return isRecord(value)
    && hasExactKeys(value, ['id', 'parent_id', 'kind', 'name', 'has_compressed', 'has_audio', 'read_only', 'is_summary',
      'summary_linked',
      ...(Object.hasOwn(value, 'forum_id') ? ['forum_id'] : [])])
    && (!Object.hasOwn(value, 'forum_id') || (value.kind === 'folder' && typeof value.forum_id === 'string'))
    && isNoteId(value.id)
    && isNoteParent(value.parent_id)
    && (value.kind === 'folder' || value.kind === 'note' || value.kind === 'annotation')
    && typeof value.name === 'string' && value.name.length > 0
    && typeof value.has_compressed === 'boolean'
    && typeof value.has_audio === 'boolean'
    && typeof value.read_only === 'boolean'
    && typeof value.is_summary === 'boolean' && typeof value.summary_linked === 'boolean'
    && (!value.is_summary || value.kind === 'note') && (!value.summary_linked || value.is_summary);
}

export function isNoteItemList(value: unknown): value is { items: NoteItem[] } {
  return isRecord(value)
    && hasExactKeys(value, ['items'])
    && Array.isArray(value.items)
    && value.items.every(isNoteItem);
}

