import type { Editor } from '@tiptap/core';
import { act, renderHook, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import { ChaError, type NoteItem, type NotePage } from '../api/client';
import { fixtureClient } from '../test/fixtures';
import { createNotesEditor } from './editor';
import { noteTooLargeMessage } from './pictures';
import { useNoteImports } from './useNoteImports';
import { useNoteLeave } from './useNoteLeave';
import type { NoteSaveHandle } from './useNoteSave';

const note: NoteItem = { id: 9, parent_id: null, kind: 'note', name: 'New note', read_only: false,
  is_summary: false, summary_linked: false, has_compressed: false };
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
  const getNote = vi.fn(async (id: number) => ({ id, document: { type: 'doc', content: [{ type: 'paragraph' }] } }));
  const renameNoteItem = vi.fn(async (id: number, name: string) => ({ ...note, id, name }));
  const setNoteReadOnly = vi.fn(async (id: number, read_only: boolean) => ({ ...note, id, read_only }));
  const cancelOpen = vi.fn();
  const reload = vi.fn();
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
      openNote: () => ({ id: openId, editor: open?.editor ?? null, save: open?.save ?? null }),
      updated: () => undefined, refresh: () => undefined, cancelOpen, reload,
    });
    return { imports, leave };
  }, { initialProps: { appEpoch: 1, openId: null as number | null } });
  function start() {
    let done!: Promise<boolean | null>;
    act(() => { done = hook.result.current.imports.page(note.id, page.url); });
    return done;
  }
  return { ...hook, start, reading, saveNote, getNote, renameNoteItem, setNoteReadOnly, cancelOpen, reload,
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

  it('reloads a target whose editor was still opening when the import committed', async () => {
    const test = setup();
    test.rerender({ appEpoch: 1, openId: note.id });
    const done = test.start();
    await act(async () => { test.reading.resolve(page); await done; });
    expect(test.cancelOpen).toHaveBeenCalledOnce();
    expect(test.saveNote).toHaveBeenCalledOnce();
    expect(test.reload).toHaveBeenCalledExactlyOnceWith(note.id);
    expect(test.cancelOpen.mock.invocationCallOrder[0]).toBeLessThan(test.saveNote.mock.invocationCallOrder[0]);
    await waitFor(() => expect(test.result.current.imports.state(note.id)).toBeUndefined());
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
