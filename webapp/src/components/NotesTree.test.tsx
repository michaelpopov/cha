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

const study: NoteItem = { id: 1, parent_id: null, kind: 'folder', name: 'Study' };
const aardvark: NoteItem = { id: 2, parent_id: null, kind: 'note', name: 'Aardvark' };
const philosophy: NoteItem = { id: 3, parent_id: 1, kind: 'folder', name: 'Philosophy' };
const plan: NoteItem = { id: 4, parent_id: 3, kind: 'note', name: 'Plan' };

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
    listNoteItems: async () => [],
    importEpub: async () => { throw new Error('notes.importBook'); },
    createNoteItem: async () => { throw new Error('notes.create'); },
    getNote: async () => { throw new Error('notes.get'); },
    readNotePage: async () => { throw new Error('notes.readPage'); },
    saveNote: async () => { throw new Error('notes.save'); },
    renameNoteItem: async () => { throw new Error('notes.rename'); },
    deleteNoteItem: async () => { throw new Error('notes.delete'); },
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
}: {
  items?: NoteItem[];
  notes?: NotesClient;
  activeNoteId?: number | null;
  onOpenNote?: (id: number) => void;
  onBeforeCreateNote?: () => Promise<boolean>;
  onDeleteItem?: DeleteNoteItem;
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
        onBeforeCreateNote={onBeforeCreateNote}
        onDeleteItem={deleteItem}
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
  fireEvent.click(within(menu).getByRole('menuitem', { name: action }));
}

describe('Notes tree', () => {
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
      { id: 10, parent_id: null, kind: 'folder', name: 'Book' },
      { id: 11, parent_id: 10, kind: 'note', name: '001. First' },
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
        { id: 10, parent_id: philosophy.id, kind: 'folder', name: 'Book' },
        { id: 11, parent_id: 10, kind: 'note', name: '001. First' },
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
    const folder: NoteItem = { id: 10, parent_id: null, kind: 'folder', name: 'Book' };
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
      { id: 7, parent_id: 3, kind: 'folder', name: 'New note' },
      { id: 8, parent_id: 3, kind: 'note', name: 'New note 2' }];
    let nextId = 10;
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
      const item = { id: nextId++, parent_id, kind, name };
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
    let current: NoteItem[] = [study, { id: 5, parent_id: 1, kind: 'folder', name: 'New folder' }];
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
      const item = { id: 6, parent_id, kind, name };
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
    const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => ({ id: 9, parent_id, kind, name }));
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
      return { id: 9, parent_id, kind, name };
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
      .mockResolvedValueOnce({ id: 9, parent_id: 1, kind: 'folder', name: 'Archive' });
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
      const item = { id: 9, parent_id, kind, name };
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
      'Rename', 'Delete', 'Copy Ref ID',
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
      { id, parent_id: null, kind: 'note' as const, name }
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

  it('focuses Cancel, keeps rows on cancel, and deletes an empty folder with the contents warning', async () => {
    const deleteNoteItem = vi.fn(async (_id: number) => undefined);
    let removed = false;
    renderTree({
      items: [study],
      notes: client({
        listNoteItems: async () => (removed ? [] : [study]),
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
    fireEvent.click(screen.getByRole('button', { name: 'Delete folder' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('That item is no longer in the list.');
    expect(screen.getByRole('treeitem', { name: 'Study' })).toBeInTheDocument();
  });

  it('reports a busy deletion and keeps the captured source', async () => {
    const onDeleteItem = vi.fn<DeleteNoteItem>(async () => 'busy');
    renderTree({ items: [study, aardvark], onDeleteItem });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Delete');
    fireEvent.click(screen.getByRole('button', { name: 'Delete note' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('A notes action is already in progress.');
    expect(screen.getByRole('dialog', { name: 'Delete “Aardvark”? This cannot be undone in CHA.' })).toBeInTheDocument();
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toBeInTheDocument();
    expect(onDeleteItem).toHaveBeenCalledExactlyOnceWith(2, [study, aardvark], expect.any(Function));
  });

});
