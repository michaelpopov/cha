export type NoteSibling = {
  name: string;
  kind: 'folder' | 'note';
};

export type NoteTreeItem = {
  id: number;
  parent_id: number | null;
  kind: 'folder' | 'note';
  name: string;
  has_compressed?: boolean;
};

export function availableNoteName(
  items: readonly NoteTreeItem[],
  parentId: number | null,
  base: string,
  excludeId?: number,
): string {
  const names = new Set(items
    .filter((item) => item.parent_id === parentId && item.id !== excludeId)
    .map((item) => item.name));
  let name = base;
  for (let suffix = 1; names.has(name); suffix++) name = `${base} ${suffix}`;
  return name;
}

export type NoteTreeRow = NoteSibling & {
  id: string;
  noteId: number | null;
  parentId: number | null;
  parentKey: string | null;
  hasCompressed?: boolean;
};

// Display order only. Equal names stay distinct; this is not an identity check.
export function compareNoteSiblings(left: NoteSibling, right: NoteSibling): number {
  if (left.kind !== right.kind) return left.kind === 'folder' ? -1 : 1;
  const byLocale = left.name.localeCompare(right.name);
  if (byLocale !== 0) return byLocale;
  if (left.name < right.name) return -1;
  if (left.name > right.name) return 1;
  return 0;
}

export function buildNoteIndex(items: readonly NoteTreeItem[]): {
  rows: Record<string, NoteTreeRow>;
  children: Record<string, string[]>;
} {
  const rows: Record<string, NoteTreeRow> = {
    root: {
      id: 'root',
      noteId: null,
      name: '',
      kind: 'folder',
      parentId: null,
      parentKey: null,
    },
  };
  const children: Record<string, string[]> = { root: [] };
  for (const item of items) {
    const id = String(item.id);
    rows[id] = {
      id,
      noteId: item.id,
      name: item.name,
      hasCompressed: item.has_compressed ?? false,
      kind: item.kind,
      parentId: item.parent_id,
      parentKey: item.parent_id === null ? 'root' : String(item.parent_id),
    };
    children[id] = [];
  }
  for (const item of items) {
    const parentKey = item.parent_id === null || !rows[String(item.parent_id)]
      ? 'root'
      : String(item.parent_id);
    children[parentKey].push(String(item.id));
  }
  for (const key of Object.keys(children)) {
    children[key].sort((left, right) => compareNoteSiblings(rows[left], rows[right]));
  }
  return { rows, children };
}

// True when noteId is the item or sits inside its current metadata subtree.
export function noteInSubtree(
  items: readonly NoteTreeItem[],
  rootId: number,
  noteId: number | null,
): boolean {
  if (noteId === null) return false;
  const byId = new Map(items.map((item) => [item.id, item]));
  const seen = new Set<number>();
  let current: number | null = noteId;
  while (current !== null) {
    if (seen.has(current)) return false;
    seen.add(current);
    if (current === rootId) return true;
    const item = byId.get(current);
    if (!item) return false;
    current = item.parent_id;
  }
  return false;
}

// After a deletion, focus the next sibling, the previous sibling, or the parent.
export function focusAfterRemoval(items: readonly NoteTreeItem[], removedId: number): number | null {
  const { rows, children } = buildNoteIndex(items);
  const row = rows[String(removedId)];
  if (!row) return null;
  const siblings = children[row.parentKey ?? 'root'] ?? [];
  const index = siblings.indexOf(String(removedId));
  const next = siblings[index + 1] ?? siblings[index - 1];
  if (next && rows[next]?.noteId != null) return rows[next].noteId;
  return row.parentId;
}

export function noteItemPath(items: readonly NoteTreeItem[], id: number): string {
  const byId = new Map(items.map((item) => [item.id, item]));
  const names: string[] = [];
  const seen = new Set<number>();
  let current: number | null = id;
  while (current !== null) {
    if (seen.has(current)) break;
    seen.add(current);
    const item = byId.get(current);
    if (!item) break;
    names.push(item.name);
    current = item.parent_id;
  }
  return names.reverse().join(' / ');
}
