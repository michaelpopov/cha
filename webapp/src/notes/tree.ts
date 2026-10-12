export type NoteSibling = {
  name: string;
  kind: 'folder' | 'note';
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
