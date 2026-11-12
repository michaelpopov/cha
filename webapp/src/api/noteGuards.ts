import type { NoteItem, SummaryGenerated, SummaryPlan, SummaryTarget } from './client';
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
    && hasExactKeys(value, ['id', 'parent_id', 'kind', 'name', 'has_compressed', 'has_audio', 'has_l1_audio', 'read_only', 'is_summary',
      'summary_linked', 'has_l1', 'has_l2', 'summary_excluded', 'folder_summary',
      ...(Object.hasOwn(value, 'forum_id') ? ['forum_id'] : [])])
    && (!Object.hasOwn(value, 'forum_id') || (value.kind === 'folder' && typeof value.forum_id === 'string'))
    && isNoteId(value.id)
    && isNoteParent(value.parent_id)
    && (value.kind === 'folder' || value.kind === 'note' || value.kind === 'annotation')
    && typeof value.name === 'string' && value.name.length > 0
    && typeof value.has_compressed === 'boolean'
    && typeof value.has_audio === 'boolean'
    && typeof value.has_l1_audio === 'boolean'
    && typeof value.read_only === 'boolean'
    && typeof value.is_summary === 'boolean' && typeof value.summary_linked === 'boolean'
    && typeof value.has_l1 === 'boolean' && typeof value.has_l2 === 'boolean'
    && typeof value.summary_excluded === 'boolean' && typeof value.folder_summary === 'boolean'
    && (!value.is_summary || value.kind === 'note') && (!value.summary_linked || value.is_summary)
    && (!value.folder_summary || value.is_summary);
}

export function isNoteItemList(value: unknown): value is { items: NoteItem[] } {
  return isRecord(value)
    && hasExactKeys(value, ['items'])
    && Array.isArray(value.items)
    && value.items.every(isNoteItem);
}


function isSummaryTarget(value: unknown): value is SummaryTarget {
  return isRecord(value)
    && hasExactKeys(value, ['id', 'name', 'has_l1', 'has_l2'])
    && isNoteId(value.id)
    && typeof value.name === 'string' && value.name.length > 0
    && typeof value.has_l1 === 'boolean' && typeof value.has_l2 === 'boolean';
}

export function isSummaryPlan(value: unknown): value is SummaryPlan {
  return isRecord(value)
    && hasExactKeys(value, ['notes'])
    && Array.isArray(value.notes) && value.notes.every(isSummaryTarget);
}

export function isSummaryGenerated(value: unknown): value is SummaryGenerated {
  return isRecord(value)
    && hasExactKeys(value, ['markdown'])
    && typeof value.markdown === 'string';
}
