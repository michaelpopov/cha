import type { NoteImportSource } from '../notes/importSource';
import { act, cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { strFromU8, unzipSync } from 'fflate';
import { useState } from 'react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { noteDocument, noteItem, paragraph } from '../test/notes';

import {
  ChaError,
  type NoteContent,
  type NoteItem,
  type NotesClient,
  type NoteTextMode,
} from '../api/client';
import { downloadEpub } from '../download';
import { NotesTree, type CompressNote, type DeleteNoteItem, type NoteActions, type SummarizeNotes } from './NotesTree';

vi.mock('../download', async importOriginal => ({
  ...await importOriginal<typeof import('../download')>(),
  downloadEpub: vi.fn(),
}));

const study: NoteItem = noteItem({ id: 1, kind: 'folder', name: 'Study' });
const aardvark: NoteItem = noteItem({ id: 2, name: 'Aardvark' });
const philosophy: NoteItem = noteItem({ id: 3, parent_id: 1, kind: 'folder', name: 'Philosophy' });
const plan: NoteItem = noteItem({ id: 4, parent_id: 3, name: 'Plan' });
const noteAudio: NonNullable<NotesClient['noteAudio']> = {
  contextEpoch: () => 1,
  listVoices: async () => [],
  startBackground: vi.fn(),
  backgroundStatus: async () => ({ pending: false }),
  info: vi.fn(),
  source: vi.fn(),
  release: vi.fn(),
};

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
    generateSessionSummary: async () => { throw new Error('sessionSummary.generate'); },
    commitSessionSummary: async () => { throw new Error('sessionSummary.commit'); },
    abandonSessionSummary: async () => undefined,
    contextEpoch: () => 1,
    summaryPlan: async () => ({ notes: [] }),
    summaryGenerate: async () => { throw new Error('notes.summaryGenerate'); },
    summaryCommit: async () => undefined,
    summaryAbandon: async () => undefined,
    summaryCancel: async () => undefined,
    setNoteSummaryExcluded: async () => { throw new Error('notes.setSummaryExcluded'); },
    compressionStatus: async () => ({ pending: false }),
    cancelCompression: async () => undefined,
    listNoteItems: async () => [],
    storeChoices: async () => ({ choices: [] }),
    storeOperation: async () => ({ present: false }),
    storeUpload: async () => { throw new Error('notes.storeUpload'); },
    storeCleanup: async () => { throw new Error('notes.storeCleanup'); },
    storeCancel: async () => undefined,
    storeClose: async () => undefined,
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
  onCompressNote,
  onSummarize = vi.fn(),
  getNoteLength = vi.fn(async () => 16),
  noteTextMode = 'full_text' as NoteTextMode,
}: {
  items?: NoteItem[];
  notes?: NotesClient;
  noteTextMode?: NoteTextMode;
  activeNoteId?: number | null;
  onOpenNote?: (id: number, importSource?: NoteImportSource) => void;
  onBeforeCreateNote?: () => Promise<boolean>;
  onDeleteItem?: DeleteNoteItem;
  onCompressNote?: CompressNote;
  onSummarize?: SummarizeNotes;
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
        noteTextMode={noteTextMode}
        onBeforeCreateNote={onBeforeCreateNote}
        onDeleteItem={deleteItem}
        onCompressNote={onCompressNote ?? (async ([target], epoch, folder, refresh) => {
          await notesClient.compressNote(target.id, epoch, folder);
          await refresh();
        })}
        onSummarize={onSummarize}
        onSetNoteReadOnly={notesClient.setNoteReadOnly}
        onNoteItems={onNoteItems}
        onOpenNote={(id, importSource) => { setActive(id); if (importSource) onOpenNote(id, importSource); else onOpenNote(id); }}
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

  it('passes recursive folder note targets to one compression action', async () => {
    const next = { ...aardvark, parent_id: study.id };
    const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS' };
    const onCompressNote = vi.fn(async () => undefined);
    renderTree({ items: [study, philosophy, plan, next, annotation], onCompressNote });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Compress');
    expect(onCompressNote).toHaveBeenCalledExactlyOnceWith([
      { id: plan.id, name: plan.name }, { id: next.id, name: next.name },
    ], 1, true, expect.any(Function));
  });

  it.each([false, true])('omits Show compressed when has_compressed is %s and navigates summary actions with arrow keys', async has_compressed => {
    renderTree({ notes: client({ listNoteItems: async () => [{ ...aardvark, has_compressed }], noteAudio }) });
    fireEvent.click(await screen.findByRole('button', { name: 'Actions for Aardvark' }));
    const menu = screen.getByRole('menu');
    expect([...menu.querySelectorAll('[role="menuitem"], [role="menuitemcheckbox"]')].map(node => node.textContent))
      .toEqual(['Rename', 'Make read-only', 'Generate audio', 'Compress', 'Summarize', 'Exclude from summaries',
        'Delete', 'Copy Ref ID']);
    expect(within(menu).queryByRole('menuitem', { name: 'Show compressed' })).not.toBeInTheDocument();
    const compress = within(menu).getByRole('menuitem', { name: 'Compress' });
    await waitFor(() => expect(compress).toBeEnabled());
    const summarize = within(menu).getByRole('menuitem', { name: 'Summarize' });
    compress.focus();
    fireEvent.keyDown(menu, { key: 'ArrowDown' });
    expect(summarize).toHaveFocus();
    fireEvent.keyDown(menu, { key: 'ArrowDown' });
    expect(within(menu).getByRole('menuitemcheckbox', { name: 'Exclude from summaries' })).toHaveFocus();
    fireEvent.keyDown(menu, { key: 'ArrowUp' });
    expect(summarize).toHaveFocus();
    fireEvent.keyDown(menu, { key: 'ArrowUp' });
    expect(compress).toHaveFocus();
  });

  it('offers summaries only on ordinary notes and unmanaged folders', async () => {
    const folderSummary = { ...plan, name: 'Summary', parent_id: study.id, folder_summary: true, is_summary: true, read_only: true };
    const sessionSummary = { ...aardvark, name: 'Session', is_summary: true, summary_linked: true };
    const managed = { ...philosophy, name: 'Chats', parent_id: null, forum_id: 'chat' };
    const onSummarize = vi.fn();
    renderTree({ items: [study, folderSummary, sessionSummary, managed], onSummarize });

    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    await screen.findByRole('button', { name: 'Actions for Summary' });
    for (const name of ['Summary', 'Session', 'Chats', 'Personal']) {
      fireEvent.click(screen.getByRole('button', { name: `Actions for ${name}` }));
      const menu = screen.getByRole('menu');
      expect(within(menu).queryByRole('menuitem', { name: 'Summarize' })).not.toBeInTheDocument();
      expect(within(menu).queryByRole('menuitemcheckbox', { name: 'Exclude from summaries' })).not.toBeInTheDocument();
      if (name === 'Summary' || name === 'Session') {
        expect(within(menu).queryByRole('menuitem', { name: 'Rename' })).not.toBeInTheDocument();
        expect(within(menu).queryByRole('menuitem', { name: 'Compress' })).not.toBeInTheDocument();
      }
      fireEvent.keyDown(menu, { key: 'Escape' });
    }
    await chooseAction('Study', 'Summarize');
    expect(onSummarize).toHaveBeenCalledExactlyOnceWith(study, 1, expect.any(Function));
  });

  it('refreshes the exclusion check mark and displays a refused summary plan', async () => {
    let item = aardvark;
    const message = 'This note is under 4 KiB and does not need summaries.';
    const listNoteItems = vi.fn(async () => [item]);
    const setNoteSummaryExcluded = vi.fn(async (_id: number, summary_excluded: boolean) => {
      item = { ...item, summary_excluded };
      return item;
    });
    const onSummarize = vi.fn(async () => { throw new ChaError('invalid_argument', message); });
    renderTree({ notes: client({ listNoteItems, setNoteSummaryExcluded }), onSummarize });
    const actions = await screen.findByRole('button', { name: 'Actions for Aardvark' });
    fireEvent.click(actions);
    const checkbox = () => screen.getByRole('menuitemcheckbox', { name: 'Exclude from summaries' });
    expect(checkbox()).not.toBeChecked();
    expect(checkbox().querySelector('svg')).toBeNull();
    fireEvent.click(checkbox());
    await waitFor(() => expect(actions).toBeEnabled());
    expect(setNoteSummaryExcluded).toHaveBeenCalledExactlyOnceWith(item.id, true);
    expect(listNoteItems).toHaveBeenCalledTimes(2);
    fireEvent.click(actions);
    expect(checkbox()).toBeChecked();
    expect(checkbox().querySelector('svg')).toBeInTheDocument();
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    await chooseAction('Aardvark', 'Summarize');
    expect(await screen.findByText(message)).toBeInTheDocument();
    expect(onSummarize).toHaveBeenCalledExactlyOnceWith(item, 1, expect.any(Function));
  });

  it.each(['has_compressed', 'has_l1', 'has_l2'] as const)('shows the saved version dot for %s', async field => {
    renderTree({ items: [{ ...aardvark, [field]: true }, { ...plan, parent_id: null }] });
    const dot = await screen.findByRole('img', { name: 'Other versions saved' });
    expect(dot).toHaveAttribute('title', 'Other versions saved');
    expect(dot).toHaveClass('cha-notes-compressed');
    expect(screen.getAllByRole('img', { name: 'Other versions saved' })).toHaveLength(1);
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

  it('defers background refresh through a rename dialog and preserves focus', async () => {
    const listNoteItems = vi.fn().mockResolvedValueOnce([aardvark, { ...plan, parent_id: null }])
      .mockResolvedValue([aardvark, { ...plan, parent_id: null, is_summary: false, summary_linked: false, has_compressed: true }]);
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
    expect(screen.getByRole('img', { name: 'Other versions saved' })).toBeInTheDocument();
  });

  it('colors read-only notes with saved audio and refreshes when audio changes', async () => {
    const listNoteItems = vi.fn().mockResolvedValueOnce([aardvark, { ...plan, parent_id: null, has_audio: true }])
      .mockResolvedValue([{ ...aardvark, read_only: true, has_audio: true }, { ...plan, parent_id: null, has_audio: true }]);
    renderTree({ notes: client({ listNoteItems }) });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).not.toHaveClass('has-audio');
    act(() => window.dispatchEvent(new CustomEvent('cha-note-audio-changed', { detail: { id: aardvark.id, affectedNoteOnly: true } })));
    await waitFor(() => expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toHaveClass('has-audio'));
    expect(screen.getByRole('treeitem', { name: 'Plan' })).not.toHaveClass('has-audio');
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
      noteItem({ id: 10, kind: 'folder', name: 'Book' }),
      noteItem({ id: 11, parent_id: 10, name: '001. First' }),
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

  it('offers folder import on the root and folders, and folder export only on folders', async () => {
    const transfer = { exportFolder: vi.fn(async () => undefined), importFolder: vi.fn(async () => undefined) };
    renderTree({ notes: client({ listNoteItems: async () => [study, aardvark], noteFolderTransfer: transfer, noteAudio }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Personal' }));
    expect(screen.getAllByRole('menuitem').map((item) => item.textContent))
      .toEqual(['New note', 'New folder', 'Import EPUB…', 'Import folder…']);
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    const menu = screen.getByRole('menu');
    expect([...menu.querySelectorAll('[role="menuitem"], [role="menuitemcheckbox"]')].map(node => node.textContent))
      .toEqual(['New note', 'New folder', 'Import EPUB…', 'Import folder…', 'Export folder…', 'Export EPUB…',
        'Rename', 'Generate audio', 'Compress', 'Summarize', 'Delete', 'Upload to store']);
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Aardvark' }));
    expect(screen.queryByRole('menuitem', { name: 'Export folder…' })).not.toBeInTheDocument();
    expect(screen.queryByRole('menuitem', { name: 'Export EPUB…' })).not.toBeInTheDocument();
  });

  it('exports a folder under its name and shows progress until the save ends', async () => {
    const pending = deferred<void>();
    const transfer = { exportFolder: vi.fn(() => pending.promise), importFolder: vi.fn(async () => undefined) };
    renderTree({ notes: client({ listNoteItems: async () => [study], noteFolderTransfer: transfer }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Export folder…');
    expect(transfer.exportFolder).toHaveBeenCalledExactlyOnceWith('Study.sqlite', study.id);
    expect(screen.getByRole('status')).toHaveTextContent('Exporting folder…');
    expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeDisabled();
    await act(async () => pending.resolve());
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeEnabled();
  });

  it('imports a folder into the selected folder and reveals it', async () => {
    let items = [study, philosophy];
    const imported: NoteItem = noteItem({ id: 10, parent_id: philosophy.id, kind: 'folder', name: 'Stoics' });
    const transfer = {
      exportFolder: vi.fn(async () => undefined),
      importFolder: vi.fn(async () => { items = [...items, imported]; }),
    };
    renderTree({ notes: client({ listNoteItems: async () => items, noteFolderTransfer: transfer }) });
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    await chooseAction('Philosophy', 'Import folder…');
    expect(transfer.importFolder).toHaveBeenCalledExactlyOnceWith(philosophy.id);
    expect(await screen.findByRole('treeitem', { name: 'Stoics' })).toBeInTheDocument();
    expect(screen.getByRole('treeitem', { name: 'Philosophy' })).toHaveAttribute('aria-expanded', 'true');
  });

  it('shows a folder import error', async () => {
    const transfer = {
      exportFolder: vi.fn(async () => undefined),
      importFolder: vi.fn(async () => { throw new ChaError('invalid_argument', 'This file is not a CHA folder export.'); }),
    };
    renderTree({ notes: client({ listNoteItems: async () => [study], noteFolderTransfer: transfer }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Personal', 'Import folder…');
    expect(transfer.importFolder).toHaveBeenCalledExactlyOnceWith(null);
    expect(await screen.findByRole('alert')).toHaveTextContent('This file is not a CHA folder export.');
  });

  it('imports into the selected subfolder and reveals the book without opening a note', async () => {
    let items = [study, philosophy, aardvark];
    const importEpub = vi.fn(async () => {
      items = [...items,
        noteItem({ id: 10, parent_id: philosophy.id, kind: 'folder', name: 'Book' }),
        noteItem({ id: 11, parent_id: 10, name: '001. First' }),
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
    const folder: NoteItem = noteItem({ id: 10, kind: 'folder', name: 'Book' });
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

  it.each<[string, NoteImportSource]>([
    ['https://example.org/article', { kind: 'page', url: 'https://example.org/article' }],
    [' http://example.org/article ', { kind: 'page', url: 'http://example.org/article' }],
    ['www.example.org/article', { kind: 'page', url: 'http://www.example.org/article' }],
    ['https://youtu.be/video', { kind: 'page', url: 'https://youtu.be/video' }],
    [`https://example.org/${'a'.repeat(210)}`, { kind: 'page', url: `https://example.org/${'a'.repeat(210)}` }],
    ['/plan.md', { kind: 'file', path: '/plan.md' }],
    ['"/tmp/my plan.md"', { kind: 'file', path: '/tmp/my plan.md' }],
    [`/tmp/${'a'.repeat(210)}.md`, { kind: 'file', path: `/tmp/${'a'.repeat(210)}.md` }],
  ])(
    'creates a default-named note for %s in the captured parent and hands off its source', async (input, source) => {
      let items = [study, { ...plan, parent_id: study.id, name: 'New note' }];
      const leave = deferred<boolean>();
      const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
        const created = { ...plan, id: 9, parent_id, kind, name };
        items = [...items, created];
        return created;
      });
      const { onOpenNote } = renderTree({ notes: client({ listNoteItems: async () => items, createNoteItem }),
        onBeforeCreateNote: () => leave.promise });
      await screen.findByRole('treeitem', { name: 'Study' });
      await chooseAction('Study', 'New note');
      fireEvent.change(screen.getByLabelText('Name, URL, or file path'), { target: { value: input } });
      expect(createNoteItem).not.toHaveBeenCalled();
      fireEvent.submit(screen.getByRole('button', { name: 'Create' }).closest('form')!);
      fireEvent.focus(screen.getByRole('treeitem', { name: 'Personal' }));
      await act(async () => leave.resolve(true));
      await waitFor(() => expect(onOpenNote).toHaveBeenCalledExactlyOnceWith(9, source));
      expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(study.id, 'note', 'New note 1');
    },
  );

  it.each([
    ['note', 'example.org'], ['note', 'README.md'], ['note', 'Read https://example.org'],
    ['note', 'ftp://example.org'], ['folder', 'https://example.org/article'],
  ] as const)('keeps %s input %s as a literal name', async (kind, name) => {
    let items = [study];
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
      const created = { ...plan, id: 9, parent_id, kind, name };
      items = [...items, created];
      return created;
    });
    const { onOpenNote } = renderTree({ notes: client({ listNoteItems: async () => items, createNoteItem }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', `New ${kind}`);
    fireEvent.change(screen.getByRole('textbox', { name: kind === 'note' ? 'Name, URL, or file path' : 'Name' }), { target: { value: name } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
    expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(study.id, kind, name);
    if (kind === 'note') expect(onOpenNote).toHaveBeenCalledExactlyOnceWith(9);
    else expect(onOpenNote).not.toHaveBeenCalled();
  });

  it('keeps a URL in the dialog after creation fails without opening it', async () => {
    const createNoteItem = vi.fn().mockRejectedValue(new ChaError('invalid_argument', 'Cannot create the note.'));
    const { onOpenNote } = renderTree({ notes: client({ listNoteItems: async () => [study], createNoteItem }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'New note');
    fireEvent.change(screen.getByLabelText('Name, URL, or file path'), { target: { value: 'https://example.org/article' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Cannot create the note.');
    expect(screen.getByLabelText('Name, URL, or file path')).toHaveValue('https://example.org/article');
    expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(study.id, 'note', 'New note');
    expect(onOpenNote).not.toHaveBeenCalled();
  });

  it('suggests available names and creates notes in the chosen folder after confirmation', async () => {
    let current: NoteItem[] = [study, philosophy,
      noteItem({ id: 7, parent_id: 3, kind: 'folder', name: 'New note' }),
      noteItem({ id: 8, parent_id: 3, name: 'New note 2' })];
    let nextId = 10;
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
      const item = noteItem({ id: nextId++, parent_id, kind, name });
      current = [...current, item];
      return item;
    });
    const onOpenNote = vi.fn();
    renderTree({ notes: client({ listNoteItems: async () => current, createNoteItem }), onOpenNote });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('treeitem', { name: 'Personal' }));
    expect(screen.queryByRole('treeitem', { name: 'Study' })).not.toBeInTheDocument();
    await chooseAction('Personal', 'New note');
    const name = await screen.findByRole('textbox', { name: 'Name, URL, or file path' });
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
    expect(screen.getByRole('textbox', { name: 'Name, URL, or file path' })).toHaveValue('New note 1');
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(onOpenNote).toHaveBeenCalledWith(11));
    expect(createNoteItem).toHaveBeenLastCalledWith(3, 'note', 'New note 1');
    expect(screen.getByRole('treeitem', { name: 'New note 1' })).toHaveAttribute('tabindex', '0');
    await chooseAction('Philosophy', 'New note');
    expect(screen.getByRole('textbox', { name: 'Name, URL, or file path' })).toHaveValue('New note 3');
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
    fireEvent.change(screen.getByLabelText('Name, URL, or file path'), { target: { value: 'https://example.org/article' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(onBeforeCreateNote).toHaveBeenCalledOnce());
    expect(screen.getByRole('dialog', { name: 'New note' })).toBeInTheDocument();
    await waitFor(() => expect(screen.getByRole('button', { name: 'Create' })).toBeEnabled());
    expect(createNoteItem).not.toHaveBeenCalled();
  });

  it('creates numbered folders without leaving the open note', async () => {
    let current: NoteItem[] = [study, noteItem({ id: 5, parent_id: 1, kind: 'folder', name: 'New folder' })];
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
      const item = noteItem({ id: 6, parent_id, kind, name });
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
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => (noteItem({ id: 9, parent_id, kind, name })));
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
      return noteItem({ id: 9, parent_id, kind, name });
    });
    const { onOpenNote } = renderTree({ notes: client({
      listNoteItems: async () => {
        if (failRefresh) { failRefresh = false; throw new Error('offline'); }
        return [study, aardvark];
      }, createNoteItem,
    }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Personal', 'New note');
    fireEvent.change(screen.getByLabelText('Name, URL, or file path'), { target: { value: 'https://example.org/article' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load notes.');
    expect(screen.getByRole('treeitem', { name: 'Study' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Actions for Personal' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    await waitFor(() => expect(screen.queryByRole('alert')).not.toBeInTheDocument());
    expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(null, 'note', 'New note');
    expect(onOpenNote).not.toHaveBeenCalled();
  });

  it('keeps creation errors and the entered name in the dialog for retry', async () => {
    const createNoteItem = vi.fn()
      .mockRejectedValueOnce(new ChaError('invalid_argument', 'That name is already used.'))
      .mockResolvedValueOnce(noteItem({ id: 9, parent_id: 1, kind: 'folder', name: 'Archive' }));
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
      const item = noteItem({ id: 9, parent_id, kind, name });
      current = [...current, item];
      return item;
    });
    renderTree({ notes: client({ listNoteItems: async () => current, createNoteItem }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', `New ${kind}`);
    fireEvent.change(screen.getByRole('textbox', { name: kind === 'note' ? 'Name, URL, or file path' : 'Name' }), { target: { value: '' } });
    expect(screen.getByRole('button', { name: 'Create' })).toBeDisabled();
    fireEvent.change(screen.getByRole('textbox', { name: kind === 'note' ? 'Name, URL, or file path' : 'Name' }), { target: { value: 'My idea' } });
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
    fireEvent.change(screen.getByRole('textbox', { name: kind === 'note' ? 'Name, URL, or file path' : 'Name' }),
      { target: { value: 'https://example.org/article' } });
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.getByRole('treeitem', { name: 'Study' })).toHaveFocus();
    expect(createNoteItem).not.toHaveBeenCalled();
    expect(onBeforeCreateNote).not.toHaveBeenCalled();
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

  it('renames the active note without reopening or leaving it', async () => {
    const renameNoteItem = vi.fn(async (id: number, name: string) => (
      noteItem({ id, name })
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

describe('Folder store tree actions', () => {
  it('offers only Upload to store and no folder ref', async () => {
    renderTree({ items: [study] });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Upload to store' })).toBeEnabled());
    for (const name of ['Remove from store', 'Copy Ref ID'])
      expect(screen.queryByRole('menuitem', { name })).not.toBeInTheDocument();
  });
  it('excludes the synthetic root from folder store and reference actions', async () => {
    renderTree();
    await screen.findByRole('treeitem', { name: 'Personal' });
    await chooseAction('Personal', 'New folder');
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Personal' }));
    for (const name of ['Copy Ref ID', 'Upload to store', 'Remove from store'])
      expect(screen.queryByRole('menuitem', { name })).not.toBeInTheDocument();
  });
  it('recovers incomplete work when the dialog is reopened without a duplicate upload', async () => {
    const upload = vi.fn();
    const read = vi.fn<NotesClient['storeOperation']>().mockResolvedValue({ present: true, operation_id: 8,
      phase: 'indexing', completed: 2, total: 2, terminal: false,
      error: '', result: '', known_cleanup_remains: false });
    renderTree({ items: [study], notes: client({ listNoteItems: async () => [study], storeOperation: read, storeUpload: upload }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Upload to store');
    await waitFor(() => expect(screen.getByRole('status')).toHaveTextContent('Indexing notes…'));
    expect(upload).not.toHaveBeenCalled();
    expect(read).toHaveBeenCalledWith(1, 1);
  });
  it('deletes an uploaded folder through the normal confirmation without store calls', async () => {
    const remove = vi.fn(async () => undefined); const read = vi.fn(async () => ({ present: false as const }));
    renderTree({ items: [study, philosophy], notes: client({ listNoteItems: async () => [study, philosophy], deleteNoteItem: remove, storeOperation: read }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Delete');
    const dialog = await screen.findByRole('dialog', { name: 'Delete “Study” and all notes and folders inside it? This cannot be undone in CHA.' });
    expect(dialog).not.toHaveTextContent('remote store');
    fireEvent.click(within(dialog).getByRole('button', { name: 'Delete folder' }));
    await waitFor(() => expect(remove).toHaveBeenCalledExactlyOnceWith(1));
  });
  it('keeps store actions disabled after a failed status read', async () => {
    renderTree({ items: [study], notes: client({ listNoteItems: async () => [study], storeOperation: async () => { throw new Error('private body'); } }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not read store status');
    expect(screen.getByRole('menuitem', { name: 'Upload to store' })).toBeDisabled();
    expect(screen.getByRole('menuitem', { name: 'Delete' })).toBeEnabled();
    expect(document.body).not.toHaveTextContent('private body');
    expect(screen.getByRole('menuitem', { name: 'Summarize' })).toBeDisabled();
  });
});

it('disables conflicting folder actions while an upload is active', async () => {
  renderTree({ items: [study], notes: client({ listNoteItems: async () => [study], storeOperation: async () => ({
    present: true, operation_id: 8, phase: 'indexing', completed: 2, total: 2,
    terminal: false, error: '', result: '', known_cleanup_remains: false,
  }) }) });
  await screen.findByRole('treeitem', { name: 'Study' });
  fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
  await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Upload to store' })).toBeEnabled());
  for (const name of ['New note', 'New folder', 'Import EPUB…', 'Export EPUB…', 'Rename', 'Compress', 'Summarize'])
    expect(screen.getByRole('menuitem', { name })).toBeDisabled();
});

it.each(['mouse', 'keyboard'])('shows the export commands for both managed folders via %s', async (entry) => {
  const items: NoteItem[] = [
    { ...study, name: 'Forums', forum_id: '' },
    { ...philosophy, name: 'Research', forum_id: 'research' },
  ];
  const compressionStatus = vi.fn(async () => ({ pending: false }));
  const storeOperation = vi.fn(async () => ({ present: false as const }));
  const noteFolderTransfer = { exportFolder: vi.fn(async () => undefined), importFolder: vi.fn(async () => undefined) };
  renderTree({ notes: client({ listNoteItems: async () => items, compressionStatus, storeOperation, noteFolderTransfer }) });
  await screen.findByText('Forums');
  fireEvent.click(screen.getByRole('treeitem', { name: /Forums/ }));
  for (const name of ['Forums', 'Research']) {
    const row = screen.getByRole('treeitem', { name: new RegExp(name) });
    if (entry === 'keyboard') {
      fireEvent.focus(row);
      fireEvent.keyDown(row, { key: 'F10', code: 'F10', shiftKey: true });
    } else fireEvent.click(within(row).getByRole('button', { name: `Actions for ${name}` }));
    const menu = await screen.findByRole('menu');
    expect(within(menu).getAllByRole('menuitem').map(item => item.textContent)).toEqual(['Export folder…', 'Export EPUB…']);
    expect(within(menu).getByRole('menuitem', { name: 'Export folder…' })).toHaveFocus();
    fireEvent.keyDown(menu, { key: 'Escape' });
  }
  expect(compressionStatus).not.toHaveBeenCalled();
  expect(storeOperation).not.toHaveBeenCalled();
});

it('moves a menu up only as far as needed to show all of its commands', async () => {
  // jsdom has no layout: rows end at y=500 and the window is 768 pixels high.
  const rect = vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockImplementation(function (this: HTMLElement) {
    const height = this.getAttribute('role') === 'menu' ? this.childElementCount * 39 + 13 : 20;
    const top = this.getAttribute('role') === 'menu' ? 0 : 480;
    return { x: 0, y: top, top, left: 0, right: 160, bottom: top + height, width: 160, height, toJSON: () => ({}) };
  });
  try {
    const items: NoteItem[] = [study, { ...aardvark, name: 'Forums', kind: 'folder', forum_id: '' }];
    const noteFolderTransfer = { exportFolder: vi.fn(async () => undefined), importFolder: vi.fn(async () => undefined) };
    renderTree({ items, notes: client({ listNoteItems: async () => items, noteFolderTransfer }) });
    // Study has 11 commands: 768 - (11 * 39 + 13) - 8 = 318. Forums has 2 commands and fits below its row.
    for (const [name, top] of [['Study', '318px'], ['Forums', '500px']]) {
      fireEvent.click(await screen.findByRole('button', { name: `Actions for ${name}` }));
      expect(await screen.findByRole('menu')).toHaveStyle({ top });
      fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    }
  } finally {
    rect.mockRestore();
  }
});

it('keeps ordinary folders named Forums editable', async () => {
  renderTree({ items: [{ ...study, name: 'Forums' }] });
  await screen.findByText('Forums');
  fireEvent.click(screen.getByRole('button', { name: 'Actions for Forums' }));
  const menu = await screen.findByRole('menu');
  expect(within(menu).getByRole('menuitem', { name: 'New note' })).toBeInTheDocument();
  expect(within(menu).getByRole('menuitem', { name: 'Rename' })).toBeInTheDocument();
});

it('refreshes on visibility and registered actions without an active note, deferring while a menu is open', async () => {
  const listNoteItems = vi.fn(async () => [study]);
  let actions: NoteActions | null = null;
  const register = (next: NoteActions | null) => { actions = next; };
  const props = {
    activeNoteId: null, client: client({ listNoteItems }), vaultName: 'Personal',
    noteTextMode: 'full_text' as const,
    onBeforeCreateNote: async () => true, onOpenNote: vi.fn(), onNoteItems: vi.fn(),
    onCompressNote: vi.fn(), onSummarize: vi.fn(), onSetNoteReadOnly: vi.fn(), onDeleteItem: vi.fn(),
    getNoteLength: async () => 0, onRegisterNoteActions: register,
  };
  const view = render(<NotesTree {...props} browsing />);
  await screen.findByText('Study');
  expect(listNoteItems).toHaveBeenCalledTimes(1);
  view.rerender(<NotesTree {...props} browsing={false} />);
  view.rerender(<NotesTree {...props} browsing />);
  await waitFor(() => expect(listNoteItems).toHaveBeenCalledTimes(2));
  await waitFor(() => expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeEnabled());
  fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
  await screen.findByRole('menu');
  act(() => actions!.refresh());
  expect(listNoteItems).toHaveBeenCalledTimes(2);
  fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
  await waitFor(() => expect(listNoteItems).toHaveBeenCalledTimes(3));
});

it('keeps summary identity after detachment and restores only direct Rename', async () => {
  const linked = { ...plan, is_summary: true, summary_linked: true, read_only: true };
  const listing = vi.fn().mockResolvedValueOnce([linked]).mockResolvedValue([{ ...linked, summary_linked: false }]);
  let actions: NoteActions | null = null;
  render(<NotesTree client={client({ listNoteItems: listing })} vaultName="Personal" browsing
    noteTextMode="full_text"
    activeNoteId={null} getNoteLength={async () => 0} onBeforeCreateNote={async () => true} onOpenNote={vi.fn()}
    onDeleteItem={vi.fn()} onSetNoteReadOnly={vi.fn()} onCompressNote={vi.fn()} onSummarize={vi.fn()} onNoteItems={vi.fn()}
    onSourceChanged={vi.fn()} onRegisterNoteActions={next => { actions = next; }} />);
  fireEvent.click(await screen.findByRole('button', { name: 'Actions for Plan' }));
  expect(screen.queryByRole('menuitem', { name: 'Rename' })).not.toBeInTheDocument();
  expect(screen.queryByRole('menuitem', { name: 'Compress' })).not.toBeInTheDocument();
  expect(screen.queryByRole('menuitem', { name: 'Show compressed' })).not.toBeInTheDocument();
  expect(screen.getByRole('menuitem', { name: 'Make editable' })).toBeInTheDocument();
  expect(screen.getByRole('menuitem', { name: 'Delete' })).toBeInTheDocument();
  fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
  act(() => actions!.refresh());
  await waitFor(() => expect(listing).toHaveBeenCalledTimes(2));
  fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
  expect(screen.getByRole('menuitem', { name: 'Rename' })).toBeInTheDocument();
  expect(screen.queryByRole('menuitem', { name: 'Compress' })).not.toBeInTheDocument();
});

describe('EPUB export', () => {
  const book = [study, philosophy, { ...plan, has_l1: true }];
  const content = (text: string): NoteContent => ({ id: plan.id, document_id: plan.id, document: noteDocument(paragraph(text)) });
  beforeEach(() => vi.mocked(downloadEpub).mockClear());

  it('saves the open note first, shows progress during reads and downloads the L1 book', async () => {
    const order: string[] = [];
    const read = deferred<NoteContent>();
    renderTree({
      noteTextMode: 'l1',
      onBeforeCreateNote: async () => { order.push('save'); return true; },
      notes: client({
        listNoteItems: async () => { order.push('list'); return book; },
        getNote: async (id, mode) => { order.push(`read ${id} ${mode}`); return read.promise; },
      }),
    });
    await screen.findByRole('treeitem', { name: 'Study' });
    order.length = 0;
    await chooseAction('Study', 'Export EPUB…');
    await waitFor(() => expect(order).toEqual(['save', 'list', 'read 4 l1']));
    expect(screen.getByRole('treeitem', { name: 'Study' })).toHaveFocus();
    expect(screen.getByRole('status')).toHaveTextContent('Exporting EPUB…');
    expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeDisabled();
    await act(async () => read.resolve(content('Saved edit')));
    await waitFor(() => expect(screen.queryByRole('status')).not.toBeInTheDocument());
    expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeEnabled();
    expect(downloadEpub).toHaveBeenCalledExactlyOnceWith('Study (L1)', expect.any(Uint8Array));
    const [, bytes] = vi.mocked(downloadEpub).mock.calls[0];
    expect(strFromU8(unzipSync(bytes)['EPUB/chapter-0001.xhtml'])).toContain('Saved edit');
  });

  it('ends without a read, message or download when the open note stays unsaved', async () => {
    const listNoteItems = vi.fn(async () => book);
    const getNote = vi.fn();
    renderTree({ onBeforeCreateNote: async () => false, notes: client({ listNoteItems, getNote }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Export EPUB…');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeEnabled());
    expect(listNoteItems).toHaveBeenCalledTimes(1);
    expect(getNote).not.toHaveBeenCalled();
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    expect(screen.queryByRole('status')).not.toBeInTheDocument();
    expect(downloadEpub).not.toHaveBeenCalled();
  });

  it('stops on a vault change during a read without a download, then exports again', async () => {
    let epoch = 1;
    const read = deferred<NoteContent>();
    const getNote = vi.fn().mockReturnValueOnce(read.promise).mockResolvedValue(content('New vault'));
    renderTree({ notes: client({ contextEpoch: () => epoch, listNoteItems: async () => book, getNote }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Export EPUB…');
    await waitFor(() => expect(getNote).toHaveBeenCalledTimes(1));
    epoch = 2;
    await act(async () => read.resolve(content('Old vault')));
    expect(await screen.findByRole('alert')).toHaveTextContent('The vault changed. Start again in the current vault.');
    expect(downloadEpub).not.toHaveBeenCalled();
    await chooseAction('Study', 'Export EPUB…');
    await waitFor(() => expect(downloadEpub).toHaveBeenCalledExactlyOnceWith('Study', expect.any(Uint8Array)));
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it.each([
    ['a missing note', 'full_text', 'Could not export the note “Plan”.'],
    ['no L2 text', 'l2', 'This folder has no L2 text to export.'],
  ] as const)('shows the error for %s without a download', async (_, noteTextMode, message) => {
    const getNote = vi.fn(async () => { throw new ChaError('not_found', 'The note was not found.'); });
    renderTree({ noteTextMode, notes: client({ listNoteItems: async () => book, getNote }) });
    await screen.findByRole('treeitem', { name: 'Study' });
    await chooseAction('Study', 'Export EPUB…');
    expect(await screen.findByRole('alert')).toHaveTextContent(message);
    expect(getNote).toHaveBeenCalledTimes(noteTextMode === 'l2' ? 0 : 1);
    expect(downloadEpub).not.toHaveBeenCalled();
  });
});
