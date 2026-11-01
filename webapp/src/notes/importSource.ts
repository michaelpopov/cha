import { absoluteNoteFilePath } from './fileImport';
import { promptPageUrl } from './pageImport';

export type NoteImportSource = { kind: 'page'; url: string } | { kind: 'file'; path: string };

export function newNoteImportSource(text: string): NoteImportSource | undefined {
  const url = promptPageUrl(text);
  if (url) return { kind: 'page', url };
  const path = absoluteNoteFilePath(text);
  return path ? { kind: 'file', path } : undefined;
}
