export type NoteSibling = {
  name: string;
  kind: 'folder' | 'note';
};

export type NoteTreeItem = {
  id: number;
  parent_id: number | null;
  kind: 'folder' | 'note';
  name: string;
};

export type NoteTreeRow = NoteSibling & {
  id: string;
  noteId: number | null;
  parentId: number | null;
  parentKey: string | null;
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

export function noteFolderOptions(items: readonly NoteTreeItem[]): { id: number; path: string }[] {
  const { rows, children } = buildNoteIndex(items);
  const options: { id: number; path: string }[] = [];
  const walk = (parentKey: string, prefix: string[]) => {
    for (const id of children[parentKey] ?? []) {
      const row = rows[id];
      if (row.kind !== 'folder' || row.noteId === null) continue;
      const path = [...prefix, row.name];
      options.push({ id: row.noteId, path: path.join(' / ') });
      walk(id, path);
    }
  };
  walk('root', []);
  return options;
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
