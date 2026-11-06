import { act, renderHook, waitFor } from '@testing-library/react';
import { expect, it, vi } from 'vitest';

import type { NoteItem } from '../api/client';
import { noteItem, noteDocument, paragraph } from '../test/notes';
import { ChaWebError, createChaWebClient, type ChaWebNote } from './client';
import type { ChawebNotesRoute } from './route';
import { noteFolders, useChawebNotes } from './useChawebNotes';

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const note = (id: number): ChaWebNote => ({ item: noteItem({ id }), document: noteDocument(paragraph(`Note ${id}`)) });

it('derives a flat picker from immediate readable parents, with deterministic full paths', () => {
  const items = [
    noteItem({ id: 8, kind: 'folder', name: 'Projects' }),
    noteItem({ id: 7, kind: 'folder', name: 'CHA', parent_id: 8 }),
    noteItem({ id: 6, kind: 'folder', name: 'Grouping only' }),
    noteItem({ id: 5, kind: 'folder', name: '日本語 / долгий путь'.repeat(10), parent_id: 6 }),
    noteItem({ id: 4, name: 'Root note' }),
    noteItem({ id: 3, parent_id: 8 }),
    noteItem({ id: 2, parent_id: 7, kind: 'annotation', name: 'ANNOTATIONS' }),
    noteItem({ id: 1, parent_id: 5, is_summary: true, summary_linked: true }),
    noteItem({ id: 11, kind: 'folder', name: 'Projects / CHA' }),
    noteItem({ id: 12, parent_id: 11, read_only: true }),
  ];
  const folders = noteFolders(items);
  expect(folders[0]).toEqual({ id: 'root', name: 'Top level' });
  expect(folders.map((folder) => folder.id)).not.toContain(6);
  expect(folders.filter((folder) => folder.name === 'Projects / CHA').map((folder) => folder.id)).toEqual([7, 11]);
  expect(folders).toContainEqual({ id: 8, name: 'Projects' });
  expect(folders).toContainEqual({ id: 5, name: `Grouping only / ${items[3].name}` });
});

it('refreshes lists on entry, remembers folders, and keeps empty notes and annotation order', async () => {
  const items = [noteItem({ id: 10, kind: 'folder', name: 'Folder' }),
    noteItem({ id: 1, parent_id: 10, name: 'Zebra' }), noteItem({ id: 2, parent_id: 10, name: 'Alpha' }),
    noteItem({ id: 3, parent_id: 10, kind: 'annotation', name: 'ANNOTATIONS' }), noteItem({ id: 4 })];
  const client = { ...createChaWebClient(), listNoteItems: vi.fn(async () => items), getNote: vi.fn(async () => note(1)) };
  const recover = vi.fn(async () => undefined);
  const { result, rerender } = renderHook(({ route }: { route: ChawebNotesRoute | null }) => useChawebNotes(client, route, recover),
    { initialProps: { route: { kind: 'notes', folder: 10 } as ChawebNotesRoute | null } });
  await waitFor(() => expect(result.current.rows.map((item) => item.id)).toEqual([2, 1, 3]));
  expect(result.current.remembered).toBe(10);
  rerender({ route: { kind: 'note', id: 1 } });
  await waitFor(() => expect(result.current.note?.item.id).toBe(1));
  rerender({ route: { kind: 'notes' } });
  await waitFor(() => expect(client.listNoteItems).toHaveBeenCalledTimes(2));
  expect(result.current.folder).toBe(10);
  rerender({ route: { kind: 'notes', folder: 999 } });
  expect(result.current.folder).toBe(999);
  expect(result.current.rows).toEqual([]);
});

it('rejects late documents and failures even when a transport ignores abort', async () => {
  const a = deferred<ChaWebNote>();
  const b = deferred<ChaWebNote>();
  const c = deferred<ChaWebNote>();
  const getNote = vi.fn().mockReturnValueOnce(a.promise).mockReturnValueOnce(b.promise).mockReturnValueOnce(c.promise);
  const client = { ...createChaWebClient(), getNote };
  const { result, rerender } = renderHook(({ route }: { route: ChawebNotesRoute | null }) => useChawebNotes(client, route, async () => undefined),
    { initialProps: { route: { kind: 'note', id: 1 } as ChawebNotesRoute | null } });
  const signal = getNote.mock.calls[0][1] as AbortSignal;
  rerender({ route: { kind: 'note', id: 2 } });
  expect(signal.aborted).toBe(true);
  await act(async () => b.resolve(note(2)));
  await act(async () => a.reject(new Error('Late error')));
  expect(result.current.note?.item.id).toBe(2);
  expect(result.current.noteError).toBeNull();
  rerender({ route: { kind: 'note', id: 3 } });
  expect(result.current.note).toBeNull();
  rerender({ route: null });
  await act(async () => c.resolve(note(3)));
  expect(result.current.note).toBeNull();
});

it('loads metadata for a direct annotation, retries explicitly, and never polls', async () => {
  const getNote = vi.fn().mockRejectedValueOnce(new ChaWebError(503, 'Unavailable'))
    .mockResolvedValueOnce({ ...note(1), item: noteItem({ kind: 'annotation', name: 'ANNOTATIONS' }) });
  const client = { ...createChaWebClient(), getNote, listNoteItems: vi.fn(async () => []) };
  const route = { kind: 'note', id: 1 } as const;
  const { result } = renderHook(() => useChawebNotes(client, route, async () => undefined));
  await waitFor(() => expect(result.current.noteError).toBe('Unavailable'));
  act(() => result.current.retry());
  await waitFor(() => expect(result.current.note?.item.kind).toBe('annotation'));
  expect(client.listNoteItems).toHaveBeenCalledTimes(1);
  expect(getNote).toHaveBeenCalledTimes(2);
});

it('clears vault state and rejects an in-flight list or note after reset', async () => {
  const metadata = deferred<NoteItem[]>();
  const client = { ...createChaWebClient(), listNoteItems: vi.fn(() => metadata.promise) };
  const route = { kind: 'notes' } as const;
  const { result } = renderHook(() => useChawebNotes(client, route, async () => undefined));
  act(() => result.current.reset());
  await act(async () => metadata.resolve([noteItem()]));
  expect(result.current.items).toBeNull();
  expect(result.current.note).toBeNull();
  expect(result.current.remembered).toBeUndefined();
});

it('rejects an in-flight document after vault reset even when its numeric ID is reused', async () => {
  const old = deferred<ChaWebNote>();
  const client = { ...createChaWebClient(), getNote: vi.fn().mockReturnValueOnce(old.promise).mockResolvedValueOnce(note(1)) };
  const { result, rerender } = renderHook(({ route }: { route: ChawebNotesRoute | null }) => useChawebNotes(client, route, async () => undefined),
    { initialProps: { route: { kind: 'note', id: 1 } as ChawebNotesRoute | null } });
  act(() => result.current.reset());
  rerender({ route: null });
  rerender({ route: { kind: 'note', id: 1 } });
  await waitFor(() => expect(result.current.note?.item.id).toBe(1));
  await act(async () => old.resolve({ item: noteItem({ name: 'Old vault' }), document: noteDocument(paragraph('Old content')) }));
  expect(result.current.note?.item.name).toBe('Article');
});

it('uses bootstrap recovery for a changed vault context without a background retry', async () => {
  const client = { ...createChaWebClient(), getNote: vi.fn(async () => { throw new ChaWebError(409, 'Vault changed', 'vault_changed'); }) };
  const route = { kind: 'note', id: 1 } as const;
  const recover = vi.fn(async () => undefined);
  const { result } = renderHook(() => useChawebNotes(client, route, recover));
  await waitFor(() => expect(result.current.noteError).toBe('Vault changed'));
  expect(recover).toHaveBeenCalledOnce();
  expect(result.current.items).toBeNull();
  expect(result.current.note).toBeNull();
  expect(client.getNote).toHaveBeenCalledOnce();
});
