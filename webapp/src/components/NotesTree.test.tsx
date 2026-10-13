import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { useState } from 'react';
import { describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import type { NoteItem, NotesClient } from '../api/nativeClient';
import { NotesTree } from './NotesTree';

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
}: {
  items?: NoteItem[];
  notes?: NotesClient;
  activeNoteId?: number | null;
  onOpenNote?: (id: number, name: string) => void;
  onBeforeCreateNote?: () => Promise<boolean>;
} = {}) {
  const onNoteItems = vi.fn();
  render(
    <NotesTree
      activeNoteId={activeNoteId}
      browsing
      client={notes ?? client({ listNoteItems: async () => items })}
      onBeforeCreateNote={onBeforeCreateNote}
      onNoteItems={onNoteItems}
      onOpenNote={onOpenNote}
    />,
  );
  return { onOpenNote, onBeforeCreateNote, onNoteItems };
}

describe('Notes tree', () => {
  it('orders folders first and exposes levels, expansion, and keyboard activation', async () => {
    const onOpenNote = vi.fn();
    renderTree({ onOpenNote });
    expect(await screen.findByRole('tree', { name: 'Notes' })).toBeInTheDocument();
    const root = screen.getAllByRole('treeitem');
    expect(root.map((item) => item.textContent)).toEqual(['▸Study', 'Aardvark']);
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
    expect(onOpenNote).toHaveBeenCalledExactlyOnceWith(4, 'Plan');
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
    const dialog = await screen.findByRole('dialog', { name: 'New note' });
    expect(dialog).toHaveTextContent('Top level');
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: '  Plan  ' } });
    fireEvent.change(screen.getByLabelText('In'), { target: { value: 'root' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create' }));
    await waitFor(() => expect(createNoteItem).toHaveBeenCalledExactlyOnceWith(null, 'note', '  Plan  '));
    await waitFor(() => expect(onOpenNote).toHaveBeenCalledWith(10, '  Plan  '));
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
    await screen.findByRole('treeitem', { name: 'Study' });
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
    await screen.findByRole('treeitem', { name: 'Study' });
    fireEvent.click(screen.getByRole('button', { name: 'New folder' }));
    expect(await screen.findByLabelText('In')).toHaveValue('1');
    fireEvent.click(screen.getByRole('treeitem', { name: 'Aardvark' }));
    expect(screen.getByLabelText('In')).toHaveValue('1');
  });
});
