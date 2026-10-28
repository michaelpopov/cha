import { act, cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { useState } from 'react';
import { describe, expect, it, vi } from 'vitest';

import {
  ChaError,
  type NoteItem,
  type NotesClient,
} from '../api/client';
import { NotesTree, type DeleteNoteItem } from './NotesTree';

const study: NoteItem = { id: 1, read_only: false, has_compressed: false, parent_id: null, kind: 'folder', name: 'Study' };
const aardvark: NoteItem = { id: 2, read_only: false, has_compressed: false, parent_id: null, kind: 'note', name: 'Aardvark' };
const philosophy: NoteItem = { id: 3, read_only: false, has_compressed: false, parent_id: 1, kind: 'folder', name: 'Philosophy' };
const plan: NoteItem = { id: 4, read_only: false, has_compressed: false, parent_id: 3, kind: 'note', name: 'Plan' };

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (error: unknown) => void;
  const promise = new Promise<T>((ok, err) => {
    resolve = ok;
    reject = err;
  });
  return { promise, resolve, reject };
}

function client(overrides: Partial<NotesClient> = {}): NotesClient {
  return {
    contextEpoch: () => 1,
    compressionStatus: async () => ({ pending: false }),
    cancelCompression: async () => undefined,
    listNoteItems: async () => [],
    storeChoices: async () => ({ choices: [] }),
    storeOperation: async () => ({ present: false }),
    storeUpload: async () => { throw new Error('notes.storeUpload'); },
    storeRemove: async () => { throw new Error('notes.storeRemove'); },
    storeCancel: async () => undefined,
    storeClose: async () => undefined,
    folderDelete: async () => { throw new Error('notes.folderDelete'); },
    importEpub: async () => { throw new Error('notes.importBook'); },
    createNoteItem: async () => { throw new Error('notes.create'); },
    getNote: async () => { throw new Error('notes.get'); },
    readNotePicture: async () => new Uint8Array(),
    readNotePage: async () => { throw new Error('notes.readPage'); },
    readNoteTextFile: async () => { throw new Error('notes.readTextFile'); },
    saveNote: async () => { throw new Error('notes.save'); },
    saveNoteScrollPosition: async () => undefined,
    setNoteReadOnly: async () => { throw new Error('notes.setReadOnly'); },
    renameNoteItem: async () => { throw new Error('notes.rename'); },
    deleteNoteItem: async () => { throw new Error('notes.delete'); },
    compressNote: async () => { throw new Error('notes.compressStart'); },
    ...overrides,
  };
}

function press(code: string) {
  const tree = screen.getByRole('tree');
  fireEvent.keyDown(tree, { key: code, code });
  fireEvent.keyUp(tree, { key: code, code });
}

function renderTree({
  items = [study, aardvark, philosophy, plan],
  notes,
  activeNoteId = null as number | null,
  onOpenNote = vi.fn(),
  onBeforeCreateNote = vi.fn(async () => true),
  onDeleteItem,
  getNoteLength = vi.fn(async () => 16),
}: {
  items?: NoteItem[];
  notes?: NotesClient;
  activeNoteId?: number | null;
  onOpenNote?: (id: number) => void;
  onBeforeCreateNote?: () => Promise<boolean>;
  onDeleteItem?: DeleteNoteItem;
  getNoteLength?: (id: number) => Promise<number>;
} = {}) {
  const onNoteItems = vi.fn();
  const notesClient = notes ?? client({ listNoteItems: async () => items });
  // App deletes under the operation slot. This stub keeps only the RPC and refresh.
  const deleteItem: DeleteNoteItem = onDeleteItem ?? (async (id, _listing, refresh) => {
    await notesClient.deleteNoteItem(id);
    await refresh();
    return 'deleted';
  });
  function Host() {
    const [active, setActive] = useState(activeNoteId);
    return (
      <NotesTree
        activeNoteId={active}
        browsing
        client={notesClient}
        getNoteLength={getNoteLength}
        onBeforeCreateNote={onBeforeCreateNote}
        onDeleteItem={deleteItem}
        onCompressNote={notesClient.compressNote}
        onSetNoteReadOnly={notesClient.setNoteReadOnly}
        onNoteItems={onNoteItems}
        onOpenNote={(id) => { setActive(id); onOpenNote(id); }}
        vaultName="Personal"
      />
    );
  }
  render(<Host />);
  return { onOpenNote, onBeforeCreateNote, onNoteItems };
}

async function chooseAction(name: string, action: string) {
  fireEvent.click(screen.getByRole('button', { name: `Actions for ${name}` }));
  const menu = await screen.findByRole('menu');
  await waitFor(() => expect(within(menu).getByRole('menuitem', { name: action })).toBeEnabled());
  fireEvent.click(within(menu).getByRole('menuitem', { name: action }));
}

describe('Notes tree', () => {
  it('locks and unlocks an unopened note without offering the action for folders', async () => {
    const setNoteReadOnly = vi.fn(async (id: number, readOnly: boolean) => ({ ...aardvark, id, read_only: readOnly }));
    const { onOpenNote, onNoteItems } = renderTree({ notes: client({
      listNoteItems: async () => [study, aardvark], setNoteReadOnly,
    }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Make read-only');
    await waitFor(() => expect(setNoteReadOnly).toHaveBeenCalledWith(aardvark.id, true));
    expect(onNoteItems).toHaveBeenLastCalledWith([study, { ...aardvark, read_only: true }]);
    await chooseAction('Aardvark', 'Make editable');
    await waitFor(() => expect(setNoteReadOnly).toHaveBeenCalledWith(aardvark.id, false));
    expect(onOpenNote).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    expect(screen.queryByRole('menuitem', { name: 'Make read-only' })).not.toBeInTheDocument();
    expect(screen.queryByRole('menuitem', { name: 'Make editable' })).not.toBeInTheDocument();
  });

  it('accepts recursive folder notes sequentially, skips annotations, and stops on a local error', async () => {
    const next = { ...aardvark, parent_id: study.id };
    const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS' };
    const compressNote = vi.fn<NotesClient['compressNote']>()
      .mockResolvedValueOnce({ accepted: true })
      .mockRejectedValueOnce(new ChaError('invalid_argument', 'Missing key'));
    renderTree({ notes: client({ listNoteItems: async () => [study, philosophy, plan, next, annotation], compressNote }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Compress');
    await waitFor(() => expect(compressNote).toHaveBeenCalledTimes(2));
    expect(compressNote.mock.calls).toEqual([[plan.id, 1, true], [next.id, 1, true]]);
    expect(await screen.findByRole('alert')).toHaveTextContent('Missing key Jobs already accepted continue in the background.');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('stops remaining folder acceptance after unmount without cancelling accepted jobs', async () => {
    const acceptance = deferred<{ accepted: true }>();
    const compressNote = vi.fn<NotesClient['compressNote']>(() => acceptance.promise);
    const cancelCompression = vi.fn();
    renderTree({ notes: client({ listNoteItems: async () => [study, philosophy, plan, { ...aardvark, parent_id: study.id }], compressNote, cancelCompression }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Compress');
    cleanup();
    await act(async () => acceptance.resolve({ accepted: true }));
    expect(compressNote).toHaveBeenCalledOnce();
    expect(cancelCompression).not.toHaveBeenCalled();
  });

  it('does not submit IDs from a menu opened in a previous vault context', async () => {
    let epoch = 1;
    const compressNote = vi.fn<NotesClient['compressNote']>();
    renderTree({ notes: client({ contextEpoch: () => epoch, listNoteItems: async () => [aardvark], compressNote }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Aardvark' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    epoch = 2;
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    expect(compressNote).not.toHaveBeenCalled();
  });

  it('stops captured folder IDs after a vault epoch changes', async () => {
    let epoch = 1;
    const acceptance = deferred<{ accepted: true }>();
    const compressNote = vi.fn<NotesClient['compressNote']>(() => acceptance.promise);
    renderTree({ notes: client({ contextEpoch: () => epoch, listNoteItems: async () => [study, philosophy, plan, { ...aardvark, parent_id: study.id }], compressNote }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Compress');
    epoch = 2;
    await act(async () => acceptance.resolve({ accepted: true }));
    expect(compressNote).toHaveBeenCalledExactlyOnceWith(plan.id, 1, true);
  });

  it('disables Show compressed without stored text and skips it with the arrow keys', async () => {
    renderTree();
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Aardvark' }));
    expect(screen.getByRole('menuitem', { name: 'Show compressed' })).toBeDisabled();
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    screen.getByRole('menuitem', { name: 'Compress' }).focus();
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'ArrowDown' });
    expect(screen.getByRole('menuitem', { name: 'Delete' })).toHaveFocus();
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'ArrowUp' });
    expect(screen.getByRole('menuitem', { name: 'Compress' })).toHaveFocus();
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    expect(screen.queryByRole('menuitem', { name: 'Show compressed' })).not.toBeInTheDocument();
  });

  it('loads the saved compressed text in a read-only dialog and restores focus on close', async () => {
    const content = deferred<{ id: number; document: {}; compressed_text: string }>();
    const getNote = vi.fn(() => content.promise);
    const onOpenNote = vi.fn();
    renderTree({ notes: client({
      listNoteItems: async () => [{ ...aardvark, read_only: false, has_compressed: true }], getNote,
    }), onOpenNote });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Show compressed');
    const dialog = screen.getByRole('dialog', { name: 'Compressed version of “Aardvark”' });
    expect(within(dialog).getByRole('status')).toHaveTextContent('Loading…');
    expect(within(dialog).queryByText(/^Compression:/)).not.toBeInTheDocument();
    expect(within(dialog).getByRole('button', { name: 'Close' })).toHaveFocus();
    expect(getNote).toHaveBeenCalledExactlyOnceWith(aardvark.id);
    await act(async () => content.resolve({
      id: aardvark.id, document: {}, compressed_text: 'Stored summary\n<script>plain text</script>',
    }));
    const text = within(dialog).getByLabelText('Compressed note text');
    expect(text.textContent).toBe('Stored summary\n<script>plain text</script>');
    expect(dialog.querySelector('script')).toBeNull();
    expect(within(dialog).queryByRole('textbox')).not.toBeInTheDocument();
    expect(onOpenNote).not.toHaveBeenCalled();
    fireEvent.click(within(dialog).getByRole('button', { name: 'Close' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toHaveFocus();
    await chooseAction('Aardvark', 'Show compressed');
    fireEvent(screen.getByRole('dialog'), new Event('cancel', { cancelable: true }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it.each([false, true])('shows a closable error if compressed text cannot be read (missing=%s)', async (missing) => {
    renderTree({ notes: client({
      listNoteItems: async () => [{ ...aardvark, read_only: false, has_compressed: true }],
      getNote: async () => {
        if (missing) return { id: aardvark.id, document: {} };
        throw new ChaError('not_found', 'That note was not found.');
      },
    }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Show compressed');
    expect(await screen.findByRole('alert')).toHaveTextContent(missing
      ? 'No compressed version is stored for this note.' : 'That note was not found.');
    expect(screen.queryByText(/^Compression:/)).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Close' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it.each([
    ['Original text has twenty chars.', 'Short', '6.2:1'],
    ['😀😀😀😀', '😀', '4.0:1'],
    ['Short', 'Longer summary', '0.4:1'],
    ['', 'Summary', '0.0:1'],
    ['Original', '', 'N/A'],
  ])('shows the compression ratio for original %j and compressed %j', async (original, compressed, ratio) => {
    renderTree({ notes: client({
      listNoteItems: async () => [{ ...aardvark, read_only: false, has_compressed: true }],
      getNote: async () => ({ id: aardvark.id, compressed_text: compressed,
        document: { type: 'doc', content: [{ type: 'paragraph',
          ...(original ? { content: [{ type: 'text', text: original }] } : {}),
        }] },
      }),
    }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Show compressed');
    expect(await screen.findByText(`Compression: ${ratio}`)).toBeInTheDocument();
  });

  it('reads status on every menu opening and cancels by note ID without a dialog', async () => {
    const compressionStatus = vi.fn().mockResolvedValue({ pending: true });
    const cancelCompression = vi.fn().mockResolvedValue(undefined);
    renderTree({ notes: client({ listNoteItems: async () => [aardvark], compressionStatus, cancelCompression }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Cancel compression');
    expect(cancelCompression).toHaveBeenCalledExactlyOnceWith(aardvark.id, 1);
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    await waitFor(() => expect(screen.getByRole('button', { name: 'Actions for Aardvark' })).toBeEnabled());
    await chooseAction('Aardvark', 'Cancel compression');
    expect(compressionStatus).toHaveBeenCalledTimes(2);
  });

  it.each([false, true])('releases disabled note menus after compression acceptance settles (failed=%s)', async failed => {
    const acceptance = deferred<{ accepted: true }>();
    const compressNote = vi.fn<NotesClient['compressNote']>(() => acceptance.promise);
    renderTree({ notes: client({ listNoteItems: async () => [aardvark], compressNote }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Compress');
    expect(screen.getByRole('button', { name: 'Actions for Aardvark' })).toBeDisabled();
    await act(async () => {
      if (failed) acceptance.reject(new ChaError('invalid_argument', 'Missing key'));
      else acceptance.resolve({ accepted: true });
    });
    await waitFor(() => expect(screen.getByRole('button', { name: 'Actions for Aardvark' })).toBeEnabled());
    expect(compressNote).toHaveBeenCalledOnce();
    if (failed) expect(screen.getByRole('alert')).toHaveTextContent('Missing key');
  });

  it.each([false, true])('disables folder Compress during acceptance and restores it afterward (failed=%s)', async failed => {
    const acceptance = deferred<{ accepted: true }>();
    const compressNote = vi.fn<NotesClient['compressNote']>(() => acceptance.promise);
    renderTree({ notes: client({ listNoteItems: async () => [study, philosophy, plan], compressNote }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Compress');
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeDisabled();
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    expect(compressNote).toHaveBeenCalledOnce();
    await act(async () => {
      if (failed) acceptance.reject(new ChaError('invalid_argument', 'Missing key'));
      else acceptance.resolve({ accepted: true });
    });
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    expect(compressNote).toHaveBeenCalledOnce();
  });

  it.each([false, true])('blocks another start during folder acceptance while keeping Cancel available (running=%s)', async running => {
    const acceptance = deferred<{ accepted: true }>();
    const compressNote = vi.fn<NotesClient['compressNote']>(() => acceptance.promise);
    const cancelCompression = vi.fn().mockResolvedValue(undefined);
    const compressionStatus = vi.fn(async (id: number) => ({ pending: id === aardvark.id && running }));
    renderTree({ notes: client({
      listNoteItems: async () => [study, philosophy, plan, { ...aardvark, has_compressed: true }],
      compressNote, compressionStatus, cancelCompression,
    }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Compress');
    await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Actions for Aardvark' })); });
    expect(compressionStatus).toHaveBeenNthCalledWith(1, study.id, 1);
    expect(compressionStatus).toHaveBeenLastCalledWith(aardvark.id, 1);
    const action = screen.getByRole('menuitem', { name: running ? 'Cancel compression' : 'Compress' });
    if (running) {
      expect(action).toBeEnabled();
      fireEvent.click(action);
      expect(cancelCompression).toHaveBeenCalledExactlyOnceWith(aardvark.id, 1);
    } else {
      expect(action).toBeDisabled();
      // Safe actions and the previous compressed result remain usable.
      expect(screen.getByRole('menuitem', { name: 'Rename' })).toBeEnabled();
      expect(screen.getByRole('menuitem', { name: 'Delete' })).toBeEnabled();
      expect(screen.getByRole('menuitem', { name: 'Show compressed' })).toBeEnabled();
    }
    expect(compressNote).toHaveBeenCalledOnce();
    await act(async () => acceptance.resolve({ accepted: true }));
    if (!running) await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
  });

  it('keeps folder Compress disabled after acceptance while background jobs run', async () => {
    let pending = false;
    const compressionStatus = vi.fn(async () => ({ pending }));
    const compressNote = vi.fn<NotesClient['compressNote']>(async () => {
      pending = true;
      return { accepted: true };
    });
    renderTree({ notes: client({ listNoteItems: async () => [study, philosophy, plan], compressNote, compressionStatus }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Compress');
    await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' })); });
    expect(compressionStatus).toHaveBeenLastCalledWith(study.id, 1);
    expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeDisabled();
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    expect(compressNote).toHaveBeenCalledOnce();
    pending = false;
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: plan.id, epoch: 1 } })));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    expect(compressionStatus).toHaveBeenCalledTimes(3);
  });

  it('disables a folder for nested running jobs and waits for the last job to retire', async () => {
    const second = { ...plan, id: 5, parent_id: study.id, name: 'Second' };
    const running = new Set([plan.id, second.id]);
    const compressionStatus = vi.fn(async (id: number) => ({ pending: id === study.id && running.size > 0 }));
    renderTree({ notes: client({ listNoteItems: async () => [study, philosophy, plan, second, aardvark], compressionStatus }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' })); });
    expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeDisabled();
    expect(screen.queryByRole('menuitem', { name: 'Cancel compression' })).not.toBeInTheDocument();
    // A change outside this subtree does not trigger a status read for this menu.
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } })));
    expect(compressionStatus).toHaveBeenCalledTimes(1);
    running.delete(plan.id);
    await act(async () => {
      window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: plan.id, epoch: 1 } }));
    });
    expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeDisabled();
    running.delete(second.id);
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: second.id, epoch: 1 } })));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    expect(compressionStatus.mock.calls).toEqual([[study.id, 1], [study.id, 1], [study.id, 1]]);
  });

  it('leaves an unrelated folder available when jobs run elsewhere', async () => {
    const compressionStatus = vi.fn(async (id: number) => ({ pending: id === aardvark.id }));
    renderTree({ notes: client({ listNoteItems: async () => [study, aardvark], compressionStatus }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    expect(compressionStatus).toHaveBeenCalledExactlyOnceWith(study.id, 1);
  });

  it('restores Compress in an open note menu when its running job retires', async () => {
    let pending = true;
    const compressionStatus = vi.fn(async () => ({ pending }));
    renderTree({ notes: client({ listNoteItems: async () => [aardvark], compressionStatus }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await act(async () => { fireEvent.click(screen.getByRole('button', { name: 'Actions for Aardvark' })); });
    expect(screen.getByRole('menuitem', { name: 'Cancel compression' })).toBeEnabled();
    pending = false;
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } })));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    expect(compressionStatus).toHaveBeenCalledTimes(2);
  });

  it.each([aardvark, study])('keeps compression disabled after a failed menu status read for $kind', async item => {
    renderTree({ notes: client({ listNoteItems: async () => [item], compressionStatus: async () => { throw new Error('Offline'); } }) });
    await screen.findByRole('treeitem', { name: item.name });
    fireEvent.click(screen.getByRole('button', { name: `Actions for ${item.name}` }));
    await screen.findByRole('alert');
    expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeDisabled();
  });

  it.each(['note', 'folder'])('ignores an older %s status reply after a completion invalidation', async kind => {
    const old = deferred<{ pending: boolean }>();
    const compressionStatus = vi.fn().mockReturnValueOnce(old.promise).mockResolvedValue({ pending: false });
    const target = kind === 'folder' ? study : aardvark;
    const items = kind === 'folder' ? [study, { ...aardvark, parent_id: study.id }] : [aardvark];
    renderTree({ notes: client({ listNoteItems: async () => items, compressionStatus }) });
    await screen.findByRole('treeitem', { name: target.name });
    fireEvent.click(screen.getByRole('button', { name: `Actions for ${target.name}` }));
    expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeDisabled();
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } })));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    await act(async () => old.resolve({ pending: true }));
    expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled();
    expect(screen.queryByRole('menuitem', { name: 'Cancel compression' })).not.toBeInTheDocument();
  });

  it('defers background refresh through a rename dialog and preserves focus', async () => {
    const listNoteItems = vi.fn().mockResolvedValueOnce([aardvark, { ...plan, parent_id: null }])
      .mockResolvedValue([aardvark, { ...plan, parent_id: null, has_compressed: true }]);
    const renameNoteItem = vi.fn(async (_id: number, name: string) => ({ ...aardvark, name }));
    renderTree({ notes: client({ listNoteItems, renameNoteItem }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Rename');
    const input = screen.getByRole('textbox', { name: 'Name' });
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: plan.id, epoch: 1 } })));
    expect(listNoteItems).toHaveBeenCalledTimes(1);
    expect(input).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    await waitFor(() => expect(listNoteItems).toHaveBeenCalledTimes(2));
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toHaveFocus();
    expect(screen.getByRole('img', { name: 'Compressed version saved' })).toBeInTheDocument();
  });

  it.each(['Compress', 'Rename', 'Delete', 'Make read-only'])('keeps the open-menu %s command usable when compression changes', async action => {
    const compressNote = vi.fn<NotesClient['compressNote']>().mockResolvedValue({ accepted: true });
    const setNoteReadOnly = vi.fn(async (id: number) => ({ ...aardvark, id, read_only: true }));
    const listNoteItems = vi.fn(async () => [aardvark]);
    renderTree({ notes: client({ listNoteItems, compressNote, setNoteReadOnly }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Aardvark' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } })));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    expect(listNoteItems).toHaveBeenCalledTimes(1);
    fireEvent.click(screen.getByRole('menuitem', { name: action }));
    if (action === 'Compress') await waitFor(() => expect(compressNote).toHaveBeenCalledOnce());
    else if (action === 'Make read-only') await waitFor(() => expect(setNoteReadOnly).toHaveBeenCalledOnce());
    else expect(await screen.findByRole('dialog')).toBeInTheDocument();
  });

  it('does not release an EPUB import when folder acceptance ends', async () => {
    const acceptance = deferred<{ accepted: true }>();
    const imported = deferred<number>();
    const listNoteItems = vi.fn(async () => [study, philosophy, plan, aardvark]);
    renderTree({ notes: client({ listNoteItems, compressNote: () => acceptance.promise, importEpub: () => imported.promise }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Compress');
    await chooseAction('Personal', 'Import EPUB…');
    fireEvent.change(screen.getByLabelText('EPUB file'), { target: { files: [new File(['epub'], 'book.epub')] } });
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: plan.id, epoch: 1 } })));
    await act(async () => acceptance.resolve({ accepted: true }));
    expect(listNoteItems).toHaveBeenCalledTimes(1);
    expect(screen.getByRole('button', { name: 'Actions for Personal' })).toBeDisabled();
    await act(async () => imported.resolve(study.id));
    await waitFor(() => expect(screen.queryByText('Importing EPUB…')).not.toBeInTheDocument());
  });

  it('defers compression refresh until the audio dialog closes', async () => {
    const listNoteItems = vi.fn(async () => [{ ...aardvark, read_only: true }]);
    renderTree({ notes: client({ listNoteItems, noteAudio: {
      contextEpoch: () => 1, backgroundStatus: async () => ({ pending: false }), info: async () => ({ count: 0 }),
      startBackground: vi.fn(), source: vi.fn(), release: vi.fn(),
    } }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Generate audio');
    const dialog = await screen.findByRole('dialog', { name: 'Generate note audio' });
    await waitFor(() => expect(within(dialog).getByRole('combobox', { name: 'Voice' })).toBeEnabled());
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } })));
    expect(listNoteItems).toHaveBeenCalledTimes(1);
    expect(within(dialog).getByRole('combobox', { name: 'Voice' })).toHaveFocus();
    fireEvent.click(within(dialog).getByRole('button', { name: 'Close' }));
    await waitFor(() => expect(listNoteItems).toHaveBeenCalledTimes(2));
  });

  it('defers a refresh through rename submission without invalidating the mutation', async () => {
    const renamed = { ...aardvark, name: 'Renamed' };
    const mutation = deferred<NoteItem>();
    const listNoteItems = vi.fn().mockResolvedValueOnce([aardvark]).mockResolvedValue([renamed]);
    const renameNoteItem = vi.fn(() => mutation.promise);
    renderTree({ notes: client({ listNoteItems, renameNoteItem }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Rename');
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'Renamed' } });
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } })));
    expect(listNoteItems).toHaveBeenCalledTimes(1);
    await act(async () => mutation.resolve(renamed));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(screen.getByRole('treeitem', { name: 'Renamed' })).toHaveFocus();
    expect(renameNoteItem).toHaveBeenCalledExactlyOnceWith(aardvark.id, 'Renamed');
    await waitFor(() => expect(listNoteItems).toHaveBeenCalledTimes(3));
  });

  it('reads again for a notification received during a background list read', async () => {
    const reading = deferred<NoteItem[]>();
    const listNoteItems = vi.fn().mockResolvedValueOnce([aardvark]).mockReturnValueOnce(reading.promise)
      .mockResolvedValue([{ ...aardvark, has_compressed: true }]);
    renderTree({ notes: client({ listNoteItems }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    const changed = () => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } }));
    act(changed);
    expect(listNoteItems).toHaveBeenCalledTimes(2);
    act(changed);
    expect(listNoteItems).toHaveBeenCalledTimes(2);
    await act(async () => reading.resolve([aardvark]));
    await waitFor(() => expect(listNoteItems).toHaveBeenCalledTimes(3));
    expect(screen.getByRole('img', { name: 'Compressed version saved' })).toBeInTheDocument();
  });

  it('runs the deferred refresh after folder acceptance ends', async () => {
    const acceptance = deferred<{ accepted: true }>();
    const listNoteItems = vi.fn(async () => [study, philosophy, plan]);
    renderTree({ notes: client({ listNoteItems, compressNote: () => acceptance.promise }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Compress');
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: plan.id, epoch: 1 } })));
    expect(listNoteItems).toHaveBeenCalledTimes(1);
    await act(async () => acceptance.resolve({ accepted: true }));
    await waitFor(() => expect(listNoteItems).toHaveBeenCalledTimes(2));
  });

  it('offers Retry after a failed background read and clears its own list error on recovery', async () => {
    const listNoteItems = vi.fn().mockResolvedValueOnce([aardvark])
      .mockRejectedValueOnce(new Error('Offline')).mockResolvedValue([aardvark]);
    renderTree({ notes: client({ listNoteItems }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } })));
    await screen.findByRole('alert');
    expect(screen.getByRole('button', { name: 'Retry' })).toBeEnabled();
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } })));
    await waitFor(() => expect(screen.queryByRole('alert')).not.toBeInTheDocument());
    expect(screen.getByRole('button', { name: 'Actions for Aardvark' })).toBeEnabled();
  });

  it('preserves a submission error across completion refreshes', async () => {
    const listNoteItems = vi.fn(async () => [aardvark]);
    renderTree({ notes: client({ listNoteItems, compressNote: async () => { throw new ChaError('invalid_argument', 'Missing key'); } }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Compress');
    expect(await screen.findByRole('alert')).toHaveTextContent('Missing key');
    act(() => window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id: aardvark.id, epoch: 1 } })));
    await waitFor(() => expect(listNoteItems).toHaveBeenCalledTimes(2));
    expect(screen.getByRole('alert')).toHaveTextContent('Missing key');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('orders folders first and exposes levels, expansion, and keyboard activation', async () => {
    const onOpenNote = vi.fn();
    renderTree({ onOpenNote });
    expect(await screen.findByRole('tree', { name: 'Notes' })).toBeInTheDocument();
    const root = screen.getAllByRole('treeitem');
    expect(root.map((item) => item.getAttribute('aria-label'))).toEqual(['Personal', 'Study', 'Aardvark']);
    expect(root[0]).toHaveAttribute('aria-expanded', 'true');
    expect(root[0]).toHaveAttribute('aria-level', '1');
    expect(root[1]).toHaveAttribute('aria-level', '2');
    expect(root[1]).toHaveAttribute('aria-expanded', 'false');
    press('ArrowDown');

    press('ArrowDown');
    expect(onOpenNote).not.toHaveBeenCalled();
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toHaveAttribute('tabindex', '0');
    press('ArrowUp');
    press('ArrowRight');
    expect(screen.getByRole('treeitem', { name: 'Study' })).toHaveAttribute('aria-expanded', 'true');
    press('ArrowRight');
    expect(screen.getByRole('treeitem', { name: 'Philosophy' })).toHaveAttribute('aria-level', '3');
    press('ArrowRight');
    press('ArrowRight');
    const planRow = screen.getByRole('treeitem', { name: 'Plan' });
    expect(planRow).toHaveAttribute('aria-level', '4');
    expect(planRow).not.toHaveAttribute('aria-expanded');
    press('Enter');
    expect(onOpenNote).toHaveBeenCalledExactlyOnceWith(4);
    press('ArrowLeft');
    expect(screen.getByRole('treeitem', { name: 'Philosophy' })).toHaveAttribute('tabindex', '0');
  });

  it('marks the open note independently of keyboard focus', async () => {
    renderTree({ activeNoteId: 2 });
    const note = await screen.findByRole('treeitem', { name: 'Aardvark' });
    expect(note).toHaveAttribute('aria-current', 'true');
    expect(note).toHaveClass('is-current');
    expect(note).toHaveAttribute('tabindex', '-1');
    expect(screen.getByRole('treeitem', { name: 'Personal' })).toHaveAttribute('tabindex', '0');
    expect(screen.getByRole('treeitem', { name: 'Study' })).not.toHaveAttribute('aria-current');
  });

  it('keeps the named root present when empty or loading', async () => {
    renderTree({ items: [] });
    expect(screen.getByRole('treeitem', { name: 'Personal' })).toBeInTheDocument();
    await waitFor(() => expect(screen.getByRole('button', { name: 'Actions for Personal' })).toBeEnabled());
    expect(screen.getAllByRole('treeitem')).toHaveLength(1);
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Personal' }));
    expect(screen.getAllByRole('menuitem').map((item) => item.textContent)).toEqual(['New note', 'New folder', 'Import EPUB…']);
    expect(screen.queryByRole('button', { name: 'New note' })).not.toBeInTheDocument();
  });

  it('imports once, disables mutations while busy, expands the folder and leaves the active note open', async () => {
    let items = [aardvark];
    const pending = deferred<number>();
    const importEpub = vi.fn(() => pending.promise);
    const { onOpenNote, onBeforeCreateNote } = renderTree({
      activeNoteId: aardvark.id,
      notes: client({ listNoteItems: async () => items, importEpub }),
    });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Personal', 'Import EPUB…');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    const file = new File(['epub'], 'book.epub');
    const input = screen.getByLabelText('EPUB file');
    fireEvent.change(input, { target: { files: [file] } });
    fireEvent.change(input, { target: { files: [file] } });
    expect(importEpub).toHaveBeenCalledExactlyOnceWith(file, [aardvark], null);
    expect(screen.getByRole('status')).toHaveTextContent('Importing EPUB…');
    expect(screen.getByRole('button', { name: 'Actions for Personal' })).toBeDisabled();
    items = [aardvark,
      { id: 10, read_only: false, has_compressed: false, parent_id: null, kind: 'folder', name: 'Book' },
      { id: 11, read_only: false, has_compressed: false, parent_id: 10, kind: 'note', name: '001. First' },
    ];
    await act(async () => pending.resolve(10));
    expect(await screen.findByRole('treeitem', { name: '001. First' })).toBeInTheDocument();
    expect(screen.getByRole('treeitem', { name: 'Book' })).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toHaveAttribute('aria-current', 'true');
    expect(onOpenNote).not.toHaveBeenCalled();
    expect(onBeforeCreateNote).not.toHaveBeenCalled();
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Actions for Personal' })).toBeEnabled();
  });

  it('shows import errors and can select the same file again', async () => {
    const importEpub = vi.fn(async () => { throw new ChaError('invalid_argument', 'Could not parse XML in “text.xhtml”.'); });
    renderTree({ notes: client({ listNoteItems: async () => [study], importEpub }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    const input = screen.getByLabelText('EPUB file');
    const file = new File(['epub'], 'bad.epub');
    for (let attempt = 1; attempt <= 2; attempt++) {
      await chooseAction('Study', 'Import EPUB…');
      fireEvent.change(input, { target: { files: [file] } });
      expect(await screen.findByRole('alert')).toHaveTextContent('Could not parse XML in “text.xhtml”.');
      expect(importEpub).toHaveBeenCalledTimes(attempt);
      expect(importEpub).toHaveBeenLastCalledWith(file, [study], study.id);
      expect(screen.queryByRole('button', { name: 'Retry' })).not.toBeInTheDocument();
    }
  });

  it('imports into the selected subfolder and reveals the book without opening a note', async () => {
    let items = [study, philosophy, aardvark];
    const importEpub = vi.fn(async () => {
      items = [...items,
        { id: 10, read_only: false, has_compressed: false, parent_id: philosophy.id, kind: 'folder', name: 'Book' },
        { id: 11, read_only: false, has_compressed: false, parent_id: 10, kind: 'note', name: '001. First' },
      ];
      return 10;
    });
    const { onOpenNote } = renderTree({
      activeNoteId: aardvark.id,
      notes: client({ listNoteItems: async () => items, importEpub }),
    });
    const studyRow = await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(studyRow);
    await chooseAction('Philosophy', 'Import EPUB…');
    // Changing the focused folder while the file picker is open does not change the destination.
    fireEvent.click(studyRow);
    const file = new File(['epub'], 'book.epub');
    const before = items;
    fireEvent.change(screen.getByLabelText('EPUB file'), { target: { files: [file] } });
    expect(importEpub).toHaveBeenCalledExactlyOnceWith(file, before, philosophy.id);
    expect(await screen.findByRole('treeitem', { name: '001. First' })).toHaveAttribute('aria-level', '5');
    expect(screen.getByRole('treeitem', { name: 'Book' })).toHaveAttribute('aria-expanded', 'true');
    expect(onOpenNote).not.toHaveBeenCalled();
  });

  it('retries only the listing after an imported book commits but the refresh fails', async () => {
    const folder: NoteItem = { id: 10, read_only: false, has_compressed: false, parent_id: null, kind: 'folder', name: 'Book' };
    const importEpub = vi.fn(async () => 10);
    const listNoteItems = vi.fn().mockResolvedValueOnce([])
      .mockRejectedValueOnce(new Error('offline')).mockResolvedValueOnce([folder]);
    renderTree({ notes: client({ listNoteItems, importEpub }) });
    await waitFor(() => expect(screen.getByRole('button', { name: 'Actions for Personal' })).toBeEnabled());
    fireEvent.change(screen.getByLabelText('EPUB file'), { target: { files: [new File(['epub'], 'book.epub')] } });
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load notes.');
    expect(screen.getByRole('button', { name: 'Actions for Personal' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    expect(await screen.findByRole('treeitem', { name: 'Book' })).toBeInTheDocument();
    expect(importEpub).toHaveBeenCalledTimes(1);
  });

  it('keeps the root and disables its menu when the first listing fails', async () => {
    renderTree({ notes: client({ listNoteItems: async () => { throw new Error('offline'); } }) });
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load notes.');
    expect(screen.getByRole('button', { name: 'Actions for Personal' })).toBeDisabled();
  });

  it('ignores a stale listing and retries without repeating a failed first load', async () => {
    const first = deferred<NoteItem[]>();
    const second = deferred<NoteItem[]>();
    const third = deferred<NoteItem[]>();
    const lists: Promise<NoteItem[]>[] = [first.promise, second.promise, third.promise];
    renderTree({
      notes: client({
        listNoteItems: () => lists.shift() ?? Promise.resolve([]),
      }),
    });
    first.reject(new Error('offline'));
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load notes.');
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    third.resolve([plan]);
    await waitFor(() => expect(screen.getByRole('treeitem', { name: 'Plan' })).toBeInTheDocument());
    second.resolve([aardvark]);
    await waitFor(() => expect(screen.queryByRole('treeitem', { name: 'Aardvark' })).not.toBeInTheDocument());
    expect(screen.getByRole('treeitem', { name: 'Plan' })).toBeInTheDocument();
  });

  it('suggests available names and creates notes in the chosen folder after confirmation', async () => {
    let current: NoteItem[] = [study, philosophy,
      { id: 7, read_only: false, has_compressed: false, parent_id: 3, kind: 'folder', name: 'New note' },
      { id: 8, read_only: false, has_compressed: false, parent_id: 3, kind: 'note', name: 'New note 2' }];
    let nextId = 10;
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
      const item = { id: nextId++, read_only: false, has_compressed: false, parent_id, kind, name };
      current = [...current, item];
      return item;
    });
    const onOpenNote = vi.fn();
    renderTree({ notes: client({ listNoteItems: async () => current, createNoteItem }), onOpenNote });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('treeitem', { name: 'Personal' }));
    expect(screen.queryByRole('treeitem', { name: 'Study' })).not.toBeInTheDocument();
    await chooseAction('Personal', 'New note');
    const name = await screen.findByRole('textbox', { name: 'Name' });
    expect(name).toHaveValue('New note');
    expect(name).toHaveFocus();
    expect((name as HTMLInputElement).selectionStart).toBe(0);
    expect((name as HTMLInputElement).selectionEnd).toBe('New note'.length);
    expect(createNoteItem).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(onOpenNote).toHaveBeenCalledWith(10));
    expect(screen.getByRole('treeitem', { name: 'Personal' })).toHaveAttribute('aria-expanded', 'true');
    expect(createNoteItem).toHaveBeenCalledWith(null, 'note', 'New note');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('treeitem', { name: 'Study' }));
    await chooseAction('Philosophy', 'New note');
    expect(screen.getByRole('textbox', { name: 'Name' })).toHaveValue('New note 1');
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(onOpenNote).toHaveBeenCalledWith(11));
    expect(createNoteItem).toHaveBeenLastCalledWith(3, 'note', 'New note 1');
    expect(screen.getByRole('treeitem', { name: 'New note 1' })).toHaveAttribute('tabindex', '0');
    await chooseAction('Philosophy', 'New note');
    expect(screen.getByRole('textbox', { name: 'Name' })).toHaveValue('New note 3');
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(onOpenNote).toHaveBeenCalledWith(12));
    expect(createNoteItem).toHaveBeenLastCalledWith(3, 'note', 'New note 3');
  });

  it('creates nothing when leaving the current note stays', async () => {
    const createNoteItem = vi.fn();
    const onBeforeCreateNote = vi.fn(async () => false);
    renderTree({ notes: client({ listNoteItems: async () => [study], createNoteItem }), onBeforeCreateNote });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'New note');
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(onBeforeCreateNote).toHaveBeenCalledOnce());
    expect(screen.getByRole('dialog', { name: 'New note' })).toBeInTheDocument();
    await waitFor(() => expect(screen.getByRole('button', { name: 'Create' })).toBeEnabled());
    expect(createNoteItem).not.toHaveBeenCalled();
  });

  it('creates numbered folders without leaving the open note', async () => {
    let current: NoteItem[] = [study, { id: 5, read_only: false, has_compressed: false, parent_id: 1, kind: 'folder', name: 'New folder' }];
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
      const item = { id: 6, read_only: false, has_compressed: false, parent_id, kind, name };
      current = [...current, item];
      return item;
    });
    const onBeforeCreateNote = vi.fn(async () => false);
    renderTree({ notes: client({ listNoteItems: async () => current, createNoteItem }), onBeforeCreateNote });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'New folder');
    expect(screen.getByRole('textbox', { name: 'Name' })).toHaveValue('New folder 1');
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(1, 'folder', 'New folder 1'));
    expect(await screen.findByRole('treeitem', { name: 'New folder 1' })).toHaveAttribute('tabindex', '0');
    expect(onBeforeCreateNote).not.toHaveBeenCalled();
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('disables menus while creating and captures the destination', async () => {
    const leave = deferred<boolean>();
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => ({ id: 9, read_only: false, has_compressed: false, parent_id, kind, name }));
    renderTree({ notes: client({ listNoteItems: async () => [study, aardvark], createNoteItem }), onBeforeCreateNote: () => leave.promise });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'New note');
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    expect(screen.getByRole('button', { name: 'Create' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeDisabled();
    fireEvent.focus(screen.getByRole('treeitem', { name: 'Aardvark' }));
    await act(async () => leave.resolve(true));
    await waitFor(() => expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(1, 'note', 'New note'));
  });

  it('keeps old rows when refresh fails and retries the listing only', async () => {
    let failRefresh = false;
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
      failRefresh = true;
      return { id: 9, read_only: false, has_compressed: false, parent_id, kind, name };
    });
    renderTree({ notes: client({
      listNoteItems: async () => {
        if (failRefresh) { failRefresh = false; throw new Error('offline'); }
        return [study, aardvark];
      }, createNoteItem,
    }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Personal', 'New note');
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load notes.');
    expect(screen.getByRole('treeitem', { name: 'Study' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Actions for Personal' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    await waitFor(() => expect(screen.queryByRole('alert')).not.toBeInTheDocument());
    expect(createNoteItem).toHaveBeenCalledOnce();
  });

  it('keeps creation errors and the entered name in the dialog for retry', async () => {
    const createNoteItem = vi.fn()
      .mockRejectedValueOnce(new ChaError('invalid_argument', 'That name is already used.'))
      .mockResolvedValueOnce({ id: 9, read_only: false, has_compressed: false, parent_id: 1, kind: 'folder', name: 'Archive' });
    renderTree({ notes: client({ listNoteItems: async () => [study], createNoteItem }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'New folder');
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'Study' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('That name is already used.');
    expect(screen.getByRole('dialog', { name: 'New folder' })).toBeInTheDocument();
    expect(screen.getByRole('textbox', { name: 'Name' })).toHaveValue('Study');
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'Archive' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(createNoteItem).toHaveBeenLastCalledWith(1, 'folder', 'Archive');
  });

  it.each(['note', 'folder'] as const)('creates a %s with the entered name', async (kind) => {
    let current: NoteItem[] = [study];
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
      const item = { id: 9, read_only: false, has_compressed: false, parent_id, kind, name };
      current = [...current, item];
      return item;
    });
    renderTree({ notes: client({ listNoteItems: async () => current, createNoteItem }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', `New ${kind}`);
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: '' } });
    expect(screen.getByRole('button', { name: 'Create' })).toBeDisabled();
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'My idea' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(1, kind, 'My idea'));
    expect(await screen.findByRole('treeitem', { name: 'My idea' })).toBeInTheDocument();
  });

  it.each(['note', 'folder'] as const)('cancels a new %s without creating or leaving the open note', async (kind) => {
    const createNoteItem = vi.fn();
    const onBeforeCreateNote = vi.fn(async () => true);
    renderTree({ notes: client({ listNoteItems: async () => [study], createNoteItem }), onBeforeCreateNote });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', `New ${kind}`);
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('treeitem', { name: 'Study' })).toHaveFocus();
    expect(createNoteItem).not.toHaveBeenCalled();
    expect(onBeforeCreateNote).not.toHaveBeenCalled();
  });

  it('opens the same menu from the row button and the keyboard', async () => {
    const onOpenNote = vi.fn();
    renderTree({ onOpenNote });
    const study = await screen.findByRole('treeitem', { name: 'Study' });
    expect(study).toHaveAttribute('aria-expanded', 'false');
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    expect(study).toHaveAttribute('aria-expanded', 'false');
    const menu = await screen.findByRole('menu');
    expect(within(menu).getByRole('menuitem', { name: 'New note' })).toHaveFocus();
    expect(within(menu).queryByRole('menuitem', { name: 'Move' })).not.toBeInTheDocument();
    expect(within(menu).getByRole('menuitem', { name: 'Delete' })).toBeInTheDocument();
    fireEvent.keyDown(menu, { key: 'Escape' });
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();

    fireEvent.keyDown(screen.getByRole('tree'), { key: 'F10', shiftKey: true });
    expect(await screen.findByRole('menuitem', { name: 'New note' })).toHaveFocus();
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    expect(study).toHaveFocus();

    const noteButton = await screen.findByRole('button', { name: 'Actions for Aardvark' });
    fireEvent.click(noteButton);
    expect(screen.getAllByRole('menuitem').map((item) => item.textContent)).toEqual([
      'Rename', 'Make read-only', 'Compress', 'Show compressed', 'Delete', 'Copy Ref ID',
    ]);
    expect(onOpenNote).not.toHaveBeenCalled();
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    expect(noteButton).toHaveFocus();
    fireEvent.keyDown(screen.getByRole('tree'), { key: 'ContextMenu' });
    expect(await screen.findByRole('menuitem', { name: 'Rename' })).toHaveFocus();
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).not.toHaveAttribute('aria-expanded');
  });

  it.each([
    [1, '##000001'],
    [10, '##00000A'],
    [35, '##00000Z'],
    [36, '##00000a'],
    [61, '##00000z'],
    [62, '##000010'],
    [3844, '##000100'],
    [56800235583, '##zzzzzz'],
  ])('copies note %s as the reference %s from its menu', async (id, reference) => {
    userEvent.setup();
    const writeText = vi.spyOn(navigator.clipboard, 'writeText').mockReset().mockResolvedValue();
    const { onOpenNote } = renderTree({
      items: [aardvark, { ...plan, id, parent_id: null }],
      activeNoteId: aardvark.id,
    });
    await screen.findByRole('treeitem', { name: 'Plan' });
    await chooseAction('Plan', 'Copy Ref ID');
    expect(writeText).toHaveBeenCalledExactlyOnceWith(reference);
    expect(onOpenNote).not.toHaveBeenCalled();
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(screen.getByRole('treeitem', { name: 'Plan' })).toHaveFocus();
  });

  it('reports a clipboard failure and allows retrying', async () => {
    userEvent.setup();
    const writeText = vi.spyOn(navigator.clipboard, 'writeText')
      .mockReset().mockRejectedValueOnce(new Error('Denied')).mockResolvedValue();
    renderTree();
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Copy Ref ID');
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not copy the note reference. Try again.');
    await chooseAction('Aardvark', 'Copy Ref ID');
    expect(writeText).toHaveBeenLastCalledWith('##000002');
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('returns focus to the row when a dialog from the menu is canceled', async () => {
    renderTree();
    const study = await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    fireEvent.click(within(screen.getByRole('menu')).getByRole('menuitem', { name: 'Rename' }));
    fireEvent.click(within(await screen.findByRole('dialog')).getByRole('button', { name: 'Cancel' }));
    expect(study).toHaveFocus();

    fireEvent.keyDown(screen.getByRole('tree'), { key: 'F10', shiftKey: true });
    fireEvent.click(within(screen.getByRole('menu')).getByRole('menuitem', { name: 'Delete' }));
    const dialog = await screen.findByRole('dialog');
    expect(within(dialog).getByRole('button', { name: 'Cancel' })).toHaveFocus();
    fireEvent.click(within(dialog).getByRole('button', { name: 'Cancel' }));
    expect(study).toHaveFocus();
  });

  it('renames the active note without reopening or leaving it', async () => {
    const renameNoteItem = vi.fn(async (id: number, name: string) => (
      { id, read_only: false, has_compressed: false, parent_id: null, kind: 'note' as const, name }
    ));
    let renamed = false;
    const onBeforeCreateNote = vi.fn(async () => true);
    const onOpenNote = vi.fn();
    renderTree({
      activeNoteId: aardvark.id,
      onBeforeCreateNote,
      onOpenNote,
      notes: client({
        listNoteItems: async () => {
          if (renamed) return [study, { ...aardvark, name: '  Zebra  ' }, philosophy, plan];
          return [study, aardvark, philosophy, plan];
        },
        renameNoteItem: async (id, name) => {
          renamed = true;
          return renameNoteItem(id, name);
        },
      }),
    });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Rename');
    const dialog = await screen.findByRole('dialog', { name: 'Rename “Aardvark”' });
    const input = within(dialog).getByLabelText('Name');
    expect(input).toHaveFocus();
    expect(input).toHaveValue('Aardvark');
    expect((input as HTMLInputElement).selectionStart).toBe(0);
    expect((input as HTMLInputElement).selectionEnd).toBe('Aardvark'.length);
    fireEvent.change(input, { target: { value: '' } });
    expect(within(dialog).getByRole('button', { name: 'Rename' })).toBeDisabled();
    fireEvent.change(input, { target: { value: '  Zebra  ' } });
    fireEvent.click(within(dialog).getByRole('button', { name: 'Rename' }));
    await waitFor(() => expect(renameNoteItem).toHaveBeenCalledExactlyOnceWith(2, '  Zebra  '));
    const renamedRow = await screen.findByRole('treeitem', { name: 'Zebra' });
    expect(renamedRow).toHaveAttribute('aria-label', '  Zebra  ');
    expect(renamedRow).toHaveAttribute('tabindex', '0');
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(onBeforeCreateNote).not.toHaveBeenCalled();
    expect(onOpenNote).not.toHaveBeenCalled();
  });

  it('keeps a duplicate name in the rename dialog', async () => {
    renderTree({
      notes: client({
        listNoteItems: async () => [study, aardvark, philosophy],
        renameNoteItem: async () => {
          throw new ChaError('invalid_argument', 'An item with that name already exists.');
        },
      }),
    });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Rename');
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Study' } });
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('An item with that name already exists.');
    expect(screen.getByRole('dialog', { name: 'Rename “Aardvark”' })).toBeInTheDocument();
    expect(screen.getAllByRole('treeitem')).toHaveLength(3);
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
  });

  it('focuses Cancel and keeps the contents warning for a folder containing another folder', async () => {
    const deleteNoteItem = vi.fn(async (_id: number) => undefined);
    let removed = false;
    renderTree({
      items: [study, philosophy],
      notes: client({
        listNoteItems: async () => (removed ? [] : [study, philosophy]),
        deleteNoteItem: async (id) => {
          removed = true;
          await deleteNoteItem(id);
        },
      }),
    });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Delete');
    const dialog = await screen.findByRole('dialog', {
      name: 'Delete “Study” and all notes and folders inside it? This cannot be undone in CHA.',
    });
    expect(dialog).toHaveTextContent('Delete “Study” and all notes and folders inside it? This cannot be undone in CHA.');
    expect(within(dialog).getByRole('button', { name: 'Cancel' })).toHaveFocus();
    fireEvent.click(within(dialog).getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(deleteNoteItem).not.toHaveBeenCalled();
    expect(screen.getByRole('treeitem', { name: 'Study' })).toBeInTheDocument();

    await chooseAction('Study', 'Delete');
    fireEvent.click(screen.getByRole('button', { name: 'Delete folder' }));
    await waitFor(() => expect(deleteNoteItem).toHaveBeenCalledExactlyOnceWith(1));
    await waitFor(() => expect(screen.queryByRole('treeitem', { name: 'Study' })).not.toBeInTheDocument());
  });

  it('deletes an empty folder without a confirmation or a note read', async () => {
    const getNoteLength = vi.fn(async () => 16);
    let removed = false;
    const deleteNoteItem = vi.fn(async () => { removed = true; });
    renderTree({
      getNoteLength,
      notes: client({
        listNoteItems: async () => removed ? [] : [study],
        deleteNoteItem,
      }),
    });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Delete');
    await waitFor(() => expect(deleteNoteItem).toHaveBeenCalledExactlyOnceWith(1));
    await waitFor(() => expect(screen.queryByRole('treeitem', { name: 'Study' })).not.toBeInTheDocument());
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(getNoteLength).not.toHaveBeenCalled();
  });

  it.each([0, 15, 16, 17])('checks confirmation for a note with %s characters', async (length) => {
    const getNoteLength = vi.fn(async () => length);
    let removed = false;
    const deleteNoteItem = vi.fn(async () => { removed = true; });
    renderTree({
      getNoteLength,
      notes: client({ listNoteItems: async () => removed ? [study] : [study, aardvark], deleteNoteItem }),
    });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Delete');
    if (length >= 16) {
      const dialog = await screen.findByRole('dialog');
      expect(deleteNoteItem).not.toHaveBeenCalled();
      fireEvent.click(within(dialog).getByRole('button', { name: 'Delete note' }));
    }
    await waitFor(() => expect(deleteNoteItem).toHaveBeenCalledExactlyOnceWith(2));
    expect(getNoteLength).toHaveBeenCalledExactlyOnceWith(2);
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    await waitFor(() => expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeEnabled());
  });

  it('keeps confirmation when a note cannot be read', async () => {
    const onDeleteItem = vi.fn<DeleteNoteItem>(async () => 'deleted');
    renderTree({ getNoteLength: async () => { throw new Error('read failed'); }, onDeleteItem });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Delete');
    expect(await screen.findByRole('dialog')).toHaveTextContent('Delete “Aardvark”?');
    expect(onDeleteItem).not.toHaveBeenCalled();
  });

  it('reports a failed immediate deletion in the existing dialog', async () => {
    const onDeleteItem = vi.fn<DeleteNoteItem>(async () => { throw new Error('Delete failed'); });
    renderTree({ getNoteLength: async () => 15, onDeleteItem });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Delete');
    expect(await screen.findByRole('alert')).toHaveTextContent('That item could not be deleted.');
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toBeInTheDocument();
    expect(onDeleteItem).toHaveBeenCalledOnce();
  });

  it('does not delete after the tree unmounts during a note read', async () => {
    const length = deferred<number>();
    const onDeleteItem = vi.fn<DeleteNoteItem>(async () => 'deleted');
    renderTree({ getNoteLength: () => length.promise, onDeleteItem });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Delete');
    cleanup();
    await act(async () => { length.resolve(0); });
    expect(onDeleteItem).not.toHaveBeenCalled();
  });

  it('focuses a nearby row after deletion and reports a missing item', async () => {
    const onDeleteItem = vi.fn<DeleteNoteItem>(async (_id, _listing, refresh) => {
      await refresh();
      return 'deleted';
    });
    let present = true;
    renderTree({
      items: [study, aardvark],
      onDeleteItem,
      notes: client({
        listNoteItems: async () => (present ? [study, aardvark] : [study]),
      }),
    });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Delete');
    const dialog = await screen.findByRole('dialog', {
      name: 'Delete “Aardvark”? This cannot be undone in CHA.',
    });
    expect(within(dialog).getByRole('button', { name: 'Cancel' })).toHaveFocus();
    present = false;
    fireEvent.click(within(dialog).getByRole('button', { name: 'Delete note' }));
    await waitFor(() => expect(screen.queryByRole('treeitem', { name: 'Aardvark' })).not.toBeInTheDocument());
    expect(screen.getByRole('treeitem', { name: 'Study' })).toHaveAttribute('tabindex', '0');
    cleanup();

    renderTree({
      items: [study],
      notes: client({
        listNoteItems: async () => [study],
        deleteNoteItem: async () => {
          throw new ChaError('not_found', 'That item is no longer in the list.');
        },
      }),
    });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Delete');
    expect(await screen.findByRole('alert')).toHaveTextContent('That item is no longer in the list.');
    expect(screen.getByRole('treeitem', { name: 'Study' })).toBeInTheDocument();
  });

  it('reports a busy deletion and keeps the captured source', async () => {
    const onDeleteItem = vi.fn<DeleteNoteItem>(async () => 'busy');
    renderTree({ items: [study, aardvark], onDeleteItem });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Delete');
    fireEvent.click(await screen.findByRole('button', { name: 'Delete note' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('A notes action is already in progress.');
    expect(screen.getByRole('dialog', { name: 'Delete “Aardvark”? This cannot be undone in CHA.' })).toBeInTheDocument();
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toBeInTheDocument();
    expect(onDeleteItem).toHaveBeenCalledExactlyOnceWith(2, [study, aardvark], expect.any(Function));
  });

});
