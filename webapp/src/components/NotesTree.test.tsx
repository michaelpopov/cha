import { cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { useState } from 'react';
import { describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import type { NoteItem, NotesClient } from '../api/nativeClient';
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
    createNoteItem: async () => { throw new Error('notes.create'); },
    getNote: async () => { throw new Error('notes.get'); },
    saveNote: async () => { throw new Error('notes.save'); },
    renameNoteItem: async () => { throw new Error('notes.rename'); },
    moveNoteItem: async () => { throw new Error('notes.move'); },
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
  onRegisterCreateNote,
}: {
  items?: NoteItem[];
  notes?: NotesClient;
  activeNoteId?: number | null;
  onOpenNote?: (id: number) => void;
  onBeforeCreateNote?: () => Promise<boolean>;
  onDeleteItem?: DeleteNoteItem;
  onRegisterCreateNote?: (create: () => void) => void;
} = {}) {
  const onNoteItems = vi.fn();
  render(
    <NotesTree
      activeNoteId={activeNoteId}
      browsing
      client={notes ?? client({ listNoteItems: async () => items })}
      onBeforeCreateNote={onBeforeCreateNote}
      onDeleteItem={onDeleteItem}
      onNoteItems={onNoteItems}
      onOpenNote={onOpenNote}
      onRegisterCreateNote={onRegisterCreateNote}
    />,
  );
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
    expect(root.map((item) => item.getAttribute('aria-label'))).toEqual(['Study', 'Aardvark']);
    expect(root[0].textContent).toContain('Study');
    expect(root[1].textContent).toContain('Aardvark');
    expect(root[0]).toHaveAttribute('aria-level', '1');
    expect(root[0]).toHaveAttribute('aria-expanded', 'false');
    expect(root[1]).not.toHaveAttribute('aria-expanded');
    expect(root[0]).toHaveAttribute('tabindex', '0');

    press('ArrowDown');
    expect(onOpenNote).not.toHaveBeenCalled();
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toHaveAttribute('tabindex', '0');
    press('ArrowUp');
    press('ArrowRight');
    expect(screen.getByRole('treeitem', { name: 'Study' })).toHaveAttribute('aria-expanded', 'true');
    press('ArrowRight');
    expect(screen.getByRole('treeitem', { name: 'Philosophy' })).toHaveAttribute('aria-level', '2');
    press('ArrowRight');
    press('ArrowRight');
    const planRow = screen.getByRole('treeitem', { name: 'Plan' });
    expect(planRow).toHaveAttribute('aria-level', '3');
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
    expect(screen.getByRole('treeitem', { name: 'Study' })).toHaveAttribute('tabindex', '0');
    expect(screen.getByRole('treeitem', { name: 'Study' })).not.toHaveAttribute('aria-current');
  });

  it('shows an empty tree after a successful listing and keeps create available', async () => {
    renderTree({ items: [] });
    expect(screen.getByRole('status')).toHaveTextContent('Loading notes…');
    expect(screen.queryByRole('tree')).not.toBeInTheDocument();
    await waitFor(() => expect(screen.getByRole('button', { name: 'New note' })).toBeEnabled());
    expect(screen.getByRole('button', { name: 'New folder' })).toBeEnabled();
    expect(screen.getByRole('tree')).toBeInTheDocument();
    expect(screen.queryByRole('treeitem')).not.toBeInTheDocument();
    expect(screen.queryByText(/no notes/i)).not.toBeInTheDocument();
  });

  it('keeps the error and disables create when the first listing fails', async () => {
    renderTree({
      notes: client({ listNoteItems: async () => { throw new Error('offline'); } }),
    });
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load notes.');
    expect(screen.queryByRole('tree')).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'New note' })).toBeDisabled();
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

  it('creates a root note and a nested note at the captured destination', async () => {
    const created: NoteItem[] = [];
    let current = [study, philosophy];
    const createNoteItem = vi.fn(async (parentId: number | null, kind: NoteItem['kind'], name: string) => {
      const item: NoteItem = { id: 10 + created.length, parent_id: parentId, kind, name };
      created.push(item);
      current = [...current, item];
      return item;
    });
    const onOpenNote = vi.fn();
    renderTree({
      items: current,
      notes: client({
        listNoteItems: async () => current,
        createNoteItem,
      }),
      onOpenNote,
    });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'New note' }));
    await screen.findByRole('dialog', { name: 'New note' });
    expect(screen.getByLabelText('In')).toHaveValue('root');
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: '  Plan  ' } });
    fireEvent.change(screen.getByLabelText('In'), { target: { value: 'root' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(null, 'note', '  Plan  '));
    await waitFor(() => expect(onOpenNote).toHaveBeenCalledWith(10));
    expect(screen.getByRole('treeitem', { name: 'Plan' })).toHaveAttribute('tabindex', '0');

    fireEvent.click(screen.getByRole('treeitem', { name: 'Study' }));
    fireEvent.click(screen.getByRole('button', { name: 'New note' }));
    await screen.findByRole('dialog', { name: 'New note' });
    expect(screen.getByLabelText('In')).toHaveValue('1');
    fireEvent.change(screen.getByLabelText('In'), { target: { value: '3' } });
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Nested' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(createNoteItem).toHaveBeenLastCalledWith(3, 'note', 'Nested'));
  });

  it('creates nothing when leaving the current note stays', async () => {
    const createNoteItem = vi.fn();
    const onBeforeCreateNote = vi.fn(async () => false);
    renderTree({
      notes: client({
        listNoteItems: async () => [study],
        createNoteItem,
      }),
      onBeforeCreateNote,
    });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'New note' }));
    await waitFor(() => expect(onBeforeCreateNote).toHaveBeenCalledOnce());
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(createNoteItem).not.toHaveBeenCalled();
  });

  it('creates a folder without asking to leave the open note', async () => {
    let current = [study];
    const createNoteItem = vi.fn(async (parentId: number | null, kind: NoteItem['kind'], name: string) => {
      const item: NoteItem = { id: 6, parent_id: parentId, kind, name };
      current = [...current, item];
      return item;
    });
    const onBeforeCreateNote = vi.fn(async () => false);
    renderTree({
      notes: client({
        listNoteItems: async () => current,
        createNoteItem,
      }),
      onBeforeCreateNote,
    });
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    fireEvent.click(screen.getByRole('button', { name: 'New folder' }));
    expect(onBeforeCreateNote).not.toHaveBeenCalled();
    fireEvent.change(await screen.findByLabelText('Name'), { target: { value: 'Archive' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(1, 'folder', 'Archive'));
    expect(await screen.findByRole('treeitem', { name: 'Archive' })).toHaveAttribute('tabindex', '0');
  });

  it('keeps the old rows when refresh fails and retries the listing only', async () => {
    let failRefresh = false;
    const createNoteItem = vi.fn(async (parentId: number | null, kind: NoteItem['kind'], name: string) => {
      failRefresh = true;
      return { id: 9, parent_id: parentId, kind, name };
    });
    renderTree({
      notes: client({
        listNoteItems: async () => {
          if (failRefresh) {
            failRefresh = false;
            throw new Error('offline');
          }
          return [study, aardvark];
        },
        createNoteItem,
      }),
    });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'New note' }));
    fireEvent.change(await screen.findByLabelText('Name'), { target: { value: 'Extra' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load notes.');
    expect(screen.getByRole('treeitem', { name: 'Study' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'New note' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    await waitFor(() => expect(screen.queryByRole('alert')).not.toBeInTheDocument());
    expect(createNoteItem).toHaveBeenCalledOnce();
    expect(screen.getByRole('button', { name: 'New note' })).toBeEnabled();
  });

  it('keeps a create error in the dialog', async () => {
    renderTree({
      notes: client({
        listNoteItems: async () => [study],
        createNoteItem: async () => {
          throw new ChaError('invalid_argument', 'A note with that name already exists.');
        },
      }),
    });
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'New folder' }));
    fireEvent.change(await screen.findByLabelText('Name'), { target: { value: 'Study' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('A note with that name already exists.');
    expect(screen.getByRole('dialog', { name: 'New folder' })).toBeInTheDocument();
    expect(screen.getAllByRole('treeitem')).toHaveLength(1);
  });

  it('does not retarget an open dialog when tree focus moves', async () => {
    function Host() {
      const [active, setActive] = useState<number | null>(null);
      return (
        <NotesTree
          activeNoteId={active}
          browsing
          client={client({ listNoteItems: async () => [study, aardvark, philosophy] })}
          onBeforeCreateNote={async () => true}
          onNoteItems={() => undefined}
          onOpenNote={(id) => setActive(id)}
        />
      );
    }
    render(<Host />);
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    fireEvent.click(screen.getByRole('button', { name: 'New folder' }));
    expect(await screen.findByLabelText('In')).toHaveValue('1');
    fireEvent.click(screen.getByRole('treeitem', { name: 'Aardvark' }));
    expect(screen.getByLabelText('In')).toHaveValue('1');
  });

  it('opens the same menu from the row button and the keyboard', async () => {
    renderTree();
    const study = await screen.findByRole('treeitem', { name: 'Study' });
    expect(study).toHaveAttribute('aria-expanded', 'false');
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    expect(study).toHaveAttribute('aria-expanded', 'false');
    const menu = await screen.findByRole('menu');
    expect(within(menu).getByRole('menuitem', { name: 'Rename' })).toHaveFocus();
    expect(within(menu).getByRole('menuitem', { name: 'Move' })).toBeInTheDocument();
    expect(within(menu).getByRole('menuitem', { name: 'Delete' })).toBeInTheDocument();
    fireEvent.keyDown(menu, { key: 'Escape' });
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();

    fireEvent.keyDown(screen.getByRole('tree'), { key: 'F10', shiftKey: true });
    expect(await screen.findByRole('menuitem', { name: 'Rename' })).toHaveFocus();
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    expect(study).toHaveFocus();

    press('ArrowDown');
    fireEvent.keyDown(screen.getByRole('tree'), { key: 'ContextMenu' });
    expect(await screen.findByRole('menuitem', { name: 'Delete' })).toBeInTheDocument();
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).not.toHaveAttribute('aria-expanded');
  });

  it('renames and moves metadata without opening or leaving the note', async () => {
    const renameNoteItem = vi.fn(async (id: number, name: string) => (
      { id, parent_id: null, kind: 'note' as const, name }
    ));
    const moveNoteItem = vi.fn(async (id: number, parentId: number | null) => (
      { ...philosophy, id, parent_id: parentId }
    ));
    let renamed = false;
    let moved = false;
    const onBeforeCreateNote = vi.fn(async () => true);
    const onOpenNote = vi.fn();
    renderTree({
      onBeforeCreateNote,
      onOpenNote,
      notes: client({
        listNoteItems: async () => {
          if (moved) return [study, { ...philosophy, parent_id: null }, aardvark, plan];
          if (renamed) return [study, { ...aardvark, name: '  Zebra  ' }, philosophy, plan];
          return [study, aardvark, philosophy, plan];
        },
        renameNoteItem: async (id, name) => {
          renamed = true;
          return renameNoteItem(id, name);
        },
        moveNoteItem: async (id, parentId) => {
          moved = true;
          return moveNoteItem(id, parentId);
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

    fireEvent.click(screen.getByRole('treeitem', { name: 'Study' }));
    await screen.findByRole('treeitem', { name: 'Philosophy' });
    await chooseAction('Philosophy', 'Move');
    const move = await screen.findByRole('dialog', { name: 'Move “Philosophy” to' });
    const destination = within(move).getByRole('combobox', { name: 'Move “Philosophy” to' });
    expect(within(destination).queryByRole('option', { name: 'Study / Philosophy' })).not.toBeInTheDocument();
    expect(within(destination).getByRole('option', { name: 'Study' })).toBeInTheDocument();
    fireEvent.change(destination, { target: { value: 'root' } });
    fireEvent.click(within(move).getByRole('button', { name: 'Move' }));
    await waitFor(() => expect(moveNoteItem).toHaveBeenCalledExactlyOnceWith(3, null));
    expect(onOpenNote).not.toHaveBeenCalled();
  });

  it('keeps a duplicate name and a folder cycle in the dialog', async () => {
    renderTree({
      notes: client({
        listNoteItems: async () => [study, aardvark, philosophy],
        renameNoteItem: async () => {
          throw new ChaError('invalid_argument', 'An item with that name already exists.');
        },
        moveNoteItem: async () => {
          throw new ChaError('invalid_argument', 'A folder cannot contain itself.');
        },
      }),
    });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Rename');
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Study' } });
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('An item with that name already exists.');
    expect(screen.getByRole('dialog', { name: 'Rename “Aardvark”' })).toBeInTheDocument();
    expect(screen.getAllByRole('treeitem')).toHaveLength(2);
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));

    await chooseAction('Study', 'Move');
    const destination = screen.getByRole('combobox', { name: 'Move “Study” to' });
    expect(within(destination).queryByRole('option', { name: 'Study' })).not.toBeInTheDocument();
    expect(within(destination).queryByRole('option', { name: 'Study / Philosophy' })).not.toBeInTheDocument();
    expect(within(destination).getByRole('option', { name: 'Top level' })).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Move' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('A folder cannot contain itself.');
    expect(screen.getByRole('treeitem', { name: 'Study' })).toHaveAttribute('aria-expanded', 'false');
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
    await waitFor(() => expect(screen.queryByRole('treeitem')).not.toBeInTheDocument());
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

  it('requires a fresh listing before deleting a folder after a failed move refresh', async () => {
    let phase: 'old' | 'fail' | 'fresh' | 'gone' = 'old';
    const captured: NoteItem[][] = [];
    const onDeleteItem = vi.fn<DeleteNoteItem>(async (_id, listing, refresh) => {
      captured.push([...listing]);
      phase = 'gone';
      await refresh();
      return 'deleted';
    });
    renderTree({
      items: [study, aardvark],
      onDeleteItem,
      notes: client({
        listNoteItems: async () => {
          if (phase === 'fail') throw new Error('offline');
          if (phase === 'gone') return [];
          if (phase === 'fresh') return [study, { ...aardvark, parent_id: 1 }];
          return [study, aardvark];
        },
        moveNoteItem: async () => {
          phase = 'fail';
          return { ...aardvark, parent_id: 1 };
        },
      }),
    });
    await screen.findByRole('treeitem', { name: 'Aardvark' });
    await chooseAction('Aardvark', 'Move');
    fireEvent.change(screen.getByRole('combobox', { name: 'Move “Aardvark” to' }), { target: { value: '1' } });
    fireEvent.click(screen.getByRole('button', { name: 'Move' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not load notes.');
    expect(screen.getByRole('treeitem', { name: 'Aardvark' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeDisabled();
    fireEvent.keyDown(screen.getByRole('tree'), { key: 'F10', shiftKey: true });
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(onDeleteItem).not.toHaveBeenCalled();

    phase = 'fresh';
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    await waitFor(() => expect(screen.queryByRole('alert')).not.toBeInTheDocument());
    await chooseAction('Study', 'Delete');
    fireEvent.click(screen.getByRole('button', { name: 'Delete folder' }));
    await waitFor(() => expect(onDeleteItem).toHaveBeenCalledOnce());
    expect(captured[0]?.find((item) => item.id === 2)?.parent_id).toBe(1);
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

  it('starts a note from the registered create action', async () => {
    const registered = vi.fn();
    renderTree({
      onRegisterCreateNote: (create) => registered.mockImplementation(create),
    });
    await screen.findByRole('treeitem', { name: 'Study' });
    registered();
    expect(await screen.findByRole('dialog', { name: 'New note' })).toBeInTheDocument();
  });
});
