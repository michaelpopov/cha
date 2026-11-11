import type { Editor } from '@tiptap/core';
import { act, renderHook, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { noteItem } from '../test/notes';

import { ChaError, type NoteItem, type NotePage } from '../api/client';
import { fixtureClient } from '../test/fixtures';
import { createNotesEditor } from './editor';
import { noteTooLargeMessage } from './pictures';
import { useNoteImports } from './useNoteImports';
import { useNoteLeave } from './useNoteLeave';
import type { NoteSaveHandle } from './useNoteSave';

const note: NoteItem = noteItem({ id: 9, name: 'New note' });
const page: NotePage = { url: 'https://youtu.be/video', title: 'Video', markdown: 'Transcript' };

function pending<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(done => { resolve = done; });
  return { promise, resolve };
}

function setup(open?: { editor: Editor; save: NoteSaveHandle }) {
  const reading = pending<NotePage>();
  let epoch = 1;
  let items = [note];
  const saveNote = vi.fn(async () => ({ annotation_created: false }));
  const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: { type: 'doc', content: [{ type: 'paragraph' }] } }));
  const renameNoteItem = vi.fn(async (id: number, name: string) => ({ ...note, id, name }));
  const setNoteReadOnly = vi.fn(async (id: number, read_only: boolean) => ({ ...note, id, read_only }));
  const opening = pending<void>();
  const client = fixtureClient({
    contextEpoch: () => epoch,
    listNoteItems: async () => items,
    readNotePage: () => reading.promise,
    getNote, saveNote, renameNoteItem, setNoteReadOnly,
  });
  const hook = renderHook(({ appEpoch, openId }) => {
    const leave = useNoteLeave(() => null);
    const imports = useNoteImports({
      client, epoch: appEpoch, leave,
      openNote: () => ({ noteId: openId, documentId: open ? openId : null, editor: open?.editor ?? null, save: open?.save ?? null }),
      waitForOpen: () => open ? null : opening.promise,
      updated: () => undefined, refresh: () => undefined,
    });
    return { imports, leave };
  }, { initialProps: { appEpoch: 1, openId: null as number | null } });
  function start(documentId = note.id) {
    let done!: Promise<boolean | null>;
    act(() => { done = hook.result.current.imports.page({ noteId: note.id, documentId }, page.url); });
    return done;
  }
  return { ...hook, start, reading, opening, saveNote, getNote, renameNoteItem, setNoteReadOnly,
    wireEpoch: (value: number) => { epoch = value; }, listing: (value: NoteItem[]) => { items = value; } };
}

describe('background note imports', () => {
  it.each(['app epoch', 'bridge epoch'])('does not save an old import after the %s changes', async change => {
    const test = setup();
    const done = test.start();
    if (change === 'app epoch') test.rerender({ appEpoch: 2, openId: null });
    else test.wireEpoch(2);
    await act(async () => { test.reading.resolve(page); await done; });
    expect(test.getNote).not.toHaveBeenCalled();
    expect(test.saveNote).not.toHaveBeenCalled();
    expect(test.renameNoteItem).not.toHaveBeenCalled();
    expect(test.setNoteReadOnly).not.toHaveBeenCalled();
  });

  it.each(['deleted', 'read-only'])('does not overwrite or recreate a target that becomes %s', async change => {
    const test = setup();
    const done = test.start();
    test.listing(change === 'deleted' ? [] : [{ ...note, read_only: true }]);
    await act(async () => { test.reading.resolve(page); await done; });
    expect(test.saveNote).not.toHaveBeenCalled();
    expect(test.renameNoteItem).not.toHaveBeenCalled();
    expect(test.setNoteReadOnly).not.toHaveBeenCalled();
    expect(test.result.current.imports.state(note.id)?.error).toBe(change === 'deleted'
      ? 'This note is no longer available.' : 'The note is read-only.');
  });

  it('waits for an existing note action before merging and saving', async () => {
    const test = setup();
    const done = test.start();
    act(() => { expect(test.result.current.leave.beginNoteAction()).toBe(true); });
    await act(async () => { test.reading.resolve(page); });
    expect(test.getNote).not.toHaveBeenCalled();
    expect(test.saveNote).not.toHaveBeenCalled();
    await act(async () => { test.result.current.leave.endNoteAction(); await done; });
    expect(test.saveNote).toHaveBeenCalledOnce();
    expect(test.setNoteReadOnly).toHaveBeenCalledExactlyOnceWith(note.id, true);
  });

  it('retains a background save error on its original note until dismissed', async () => {
    const test = setup();
    test.saveNote.mockRejectedValueOnce(new ChaError('internal_error', 'Save failed.'));
    const done = test.start();
    await act(async () => { test.reading.resolve(page); await done; });
    expect(test.result.current.imports.state(note.id)).toMatchObject({ stage: null, error: 'Save failed.' });
    expect(test.result.current.imports.state(10)).toBeUndefined();
    expect(test.setNoteReadOnly).not.toHaveBeenCalled();
    act(() => test.result.current.imports.dismiss(note.id));
    expect(test.result.current.imports.state(note.id)).toBeUndefined();
  });

  it('waits for an opening document before deciding where to merge', async () => {
    const test = setup();
    test.rerender({ appEpoch: 1, openId: note.id });
    const done = test.start();
    await act(async () => { test.reading.resolve(page); });
    expect(test.saveNote).not.toHaveBeenCalled();
    await act(async () => { test.opening.resolve(); await done; });
    expect(test.saveNote).toHaveBeenCalledOnce();
    await waitFor(() => expect(test.result.current.imports.state(note.id)).toBeUndefined());
  });

  it('saves a background import to its captured compressed row and changes metadata on the logical note', async () => {
    const test = setup();
    test.getNote.mockResolvedValueOnce({ id: note.id, document_id: 18, document: { type: 'doc', content: [{ type: 'paragraph' }] } });
    const done = test.start(18);
    await act(async () => { test.reading.resolve(page); await done; });
    expect(test.getNote).toHaveBeenCalledExactlyOnceWith(note.id, 'compressed');
    expect(test.saveNote).toHaveBeenCalledExactlyOnceWith(18, expect.any(Object));
    expect(test.renameNoteItem).toHaveBeenCalledExactlyOnceWith(note.id, 'Video');
    expect(test.setNoteReadOnly).toHaveBeenCalledExactlyOnceWith(note.id, true);
  });

  it('rejects a background import when its compressed row is gone', async () => {
    const test = setup();
    const done = test.start(18);
    await act(async () => { test.reading.resolve(page); expect(await done).toBe(false); });
    expect(test.saveNote).not.toHaveBeenCalled();
    expect(test.result.current.imports.state(18)?.error).toBe('This note document is no longer available.');
  });

  it('leaves a rejected open-note merge to the editor error and restarts the canceled save', async () => {
    const errors: string[] = [];
    const editor = createNotesEditor({ content: '<p>Existing</p>', byteLimit: 200,
      readPicture: async () => new Uint8Array(), onCommandError: message => { errors.push(message); } });
    const save: NoteSaveHandle = {
      isOpen: true, frozen: false, saveError: null,
      hasUnsaved: () => true, inFlightSerial: () => null, waitInFlight: async () => undefined,
      freeze: vi.fn(() => true), unfreeze: vi.fn(), submitNow: vi.fn(), flushRemaining: vi.fn(async () => true), dispose: vi.fn(),
    };
    const test = setup({ editor, save });
    test.rerender({ appEpoch: 1, openId: note.id });
    const done = test.start();
    await act(async () => { test.reading.resolve({ ...page, markdown: 'Transcript '.repeat(40) }); await done; });
    expect(errors).toEqual([noteTooLargeMessage]);
    expect(test.result.current.imports.state(note.id)).toBeUndefined();
    expect(editor.getText()).toBe('Existing');
    expect(save.flushRemaining).not.toHaveBeenCalled();
    expect(save.submitNow).toHaveBeenCalledOnce();
    expect(test.saveNote).not.toHaveBeenCalled();
    editor.destroy();
  });
});
