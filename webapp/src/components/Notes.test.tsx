import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import {
  ChaError,
  type NoteContent,
  type NoteItem,
  type NotePage,
  type NotesClient,
  type NoteTextMode,
} from '../api/client';
import { bootstrapFixture, fixtureClient, forumDetailFixture, snapshotFixture } from '../test/fixtures';
import { noteItem } from '../test/notes';
import { App } from './App';

const plan: NoteItem = noteItem({ id: 4, name: 'Plan' });
const study: NoteItem = noteItem({ id: 1, kind: 'folder', name: 'Study' });

function noteDocument(text: string) {
  return {
    type: 'doc',
    content: [{ type: 'paragraph', content: [{ type: 'text', text }] }],
  };
}

function notesClient(text = 'Saved plan') {
  return fixtureClient({
    listNoteItems: async () => [study, plan],
    getNote: async (id: number) => ({ id, document_id: id, document: noteDocument(text) }),
    saveNote: async () => undefined,
  });
}

function importClient(readNotePage: NotesClient['readNotePage'], readNoteTextFile?: NotesClient['readNoteTextFile']) {
  let items = [plan];
  const documents = new Map<number, Record<string, unknown>>([[plan.id, noteDocument('Saved plan')]]);
  let nextId = 9;
  const createNoteItem = vi.fn(async (parent_id: number | null, kind: NoteItem['kind'], name: string) => {
    const created = { ...plan, id: nextId++, parent_id, kind, name };
    items = [...items, created];
    return created;
  });
  function update(id: number, changes: Partial<NoteItem>) {
    items = items.map(item => item.id === id ? { ...item, ...changes } : item);
    return items.find(item => item.id === id)!;
  }
  const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: documents.get(id) ?? { type: 'doc', content: [{ type: 'paragraph' }] } }));
  const saveNote = vi.fn(async (id: number, document: Record<string, unknown>) => { documents.set(id, document); });
  const renameNoteItem = vi.fn(async (id: number, name: string) => update(id, { name }));
  const setNoteReadOnly = vi.fn(async (id: number, read_only: boolean) => update(id, { read_only }));
  return {
    client: fixtureClient({ listNoteItems: async () => items, createNoteItem, getNote, saveNote,
      renameNoteItem, setNoteReadOnly, readNotePage, ...(readNoteTextFile ? { readNoteTextFile } : {}) }),
    createNoteItem, getNote, saveNote, renameNoteItem, setNoteReadOnly, documents,
  };
}

async function createUrlNote(url: string) {
  fireEvent.click(screen.getByRole('button', { name: `Actions for ${bootstrapFixture.vault_name}` }));
  fireEvent.click(screen.getByRole('menuitem', { name: 'New note' }));
  const dialog = await screen.findByRole('dialog', { name: 'New note' });
  fireEvent.change(within(dialog).getByLabelText('Name, URL, or file path'), { target: { value: url } });
  fireEvent.click(within(dialog).getByRole('button', { name: 'Create' }));
  await waitFor(() => expect(screen.queryByRole('dialog', { name: 'New note' })).toBeNull());
}

function contextBus() {
  type Handler = (event: { causing_request_id?: number; state?: string; context_epoch?: number }) => void;
  const handlers = new Map<string, Handler>();
  return {
    emit() {
      handlers.get('app.contextChanged')?.({ causing_request_id: 1, state: 'running' });
    },
    notesChanged() {
      handlers.get('notes.changed')?.({ context_epoch: 1 });
    },
    layout(mode: string) { window.__CHA_NATIVE_SET_LAYOUT__?.(mode); },
    on<T>(event: string, next: (payload: T) => void) {
      handlers.set(event, next as Handler);
      return () => {
        if (handlers.get(event) === next) handlers.delete(event);
      };
    },
  };
}

async function openNotes() {
  fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
  await screen.findByRole('treeitem', { name: bootstrapFixture.vault_name });
}

function chooseNoteMode(label: 'Full text' | 'Compressed') {
  fireEvent.click(screen.getByRole('button', { name: 'Note text mode' }));
  fireEvent.click(screen.getByRole('menuitemradio', { name: label }));
}

function compressionChanged(id: number) {
  window.dispatchEvent(new CustomEvent('cha-note-compression-changed', { detail: { id, epoch: 1 } }));
}

function deferred<T>() {
  let resolve!: (value: T) => void;
  const promise = new Promise<T>(done => { resolve = done; });
  return { promise, resolve };
}

function noteEditor(): Editor {
  const dom = document.querySelector('.ProseMirror') as (HTMLElement & { chaEditor?: Editor }) | null;
  const editor = dom?.chaEditor;
  if (!editor) throw new Error('The note editor is not available.');
  return editor;
}

function typeInNote(text: string) {
  const editor = noteEditor();
  editor.view.dispatch(editor.state.tr.insertText(text));
}

function historyAction(name: 'Undo' | 'Redo') {
  fireEvent.click(screen.getByRole('button', { name: 'History' }));
  return screen.getByRole('menuitem', { name });
}

describe('Note text mode', () => {
  it('keeps fallback edits on the original row even when compression appears before the save', async () => {
    const documents = new Map<number, Record<string, unknown>>([[plan.id, noteDocument('Original text')]]);
    const positions = new Map<number, number>([[plan.id, 20], [40, 70]]);
    const getNote = vi.fn(async (id: number, mode?: NoteTextMode) => {
      const documentId = mode === 'compressed' && documents.has(40) ? 40 : id;
      return { id, document_id: documentId, document: documents.get(documentId)!, scroll_top: positions.get(documentId) };
    });
    const saveNote = vi.fn(async (id: number, document: Record<string, unknown>) => { documents.set(id, document); });
    const saveNoteScrollPosition = vi.fn(async (id: number, top: number) => { positions.set(id, top); });
    render(<App client={fixtureClient({ listNoteItems: async () => [plan], getNote, saveNote, saveNoteScrollPosition })} />);
    await openNotes();
    const row = await screen.findByRole('treeitem', { name: 'Plan' });
    fireEvent.click(row);
    await screen.findByText('Original text');
    const mode = screen.getByRole('button', { name: 'Note text mode' });
    expect(mode).toHaveAttribute('title', 'Full text mode; showing full text');
    chooseNoteMode('Compressed');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Note text mode' }))
      .toHaveAttribute('title', 'Compressed mode; showing full text'));
    expect(saveNote).not.toHaveBeenCalled();
    const host = document.querySelector('.cha-notes-document')!;
    expect(host.scrollTop).toBe(20);
    host.scrollTop = 150;
    documents.set(40, noteDocument('Compressed body'));
    act(() => typeInNote(' edited'));
    fireEvent.click(row);
    await screen.findByText('Compressed body');
    expect(screen.getByRole('button', { name: 'Note text mode' }))
      .toHaveAttribute('title', 'Compressed mode; showing compressed');
    expect(row).toHaveAttribute('aria-current', 'true');
    expect(saveNote).toHaveBeenCalledExactlyOnceWith(plan.id, noteDocument('Original text edited'));
    expect(saveNoteScrollPosition).toHaveBeenLastCalledWith(plan.id, 150);
    expect(host.scrollTop).toBe(70);
    expect(noteEditor().can().undo()).toBe(false);
    host.scrollTop = 175;
    act(() => typeInNote(' revised'));
    chooseNoteMode('Full text');
    await screen.findByText('Original text edited');
    expect(saveNote).toHaveBeenLastCalledWith(40, noteDocument('Compressed body revised'));
    expect(saveNoteScrollPosition).toHaveBeenLastCalledWith(40, 175);
    expect(host.scrollTop).toBe(150);
    chooseNoteMode('Compressed');
    await screen.findByText('Compressed body revised');
    expect(host.scrollTop).toBe(175);
    expect(noteEditor().can().undo()).toBe(false);
    const opens = getNote.mock.calls.length;
    chooseNoteMode('Compressed');
    expect(getNote).toHaveBeenCalledTimes(opens);
    expect(saveNote).toHaveBeenCalledTimes(2);
  });

  it.each(['Stay', 'Retry', 'Discard'] as const)('uses %s when a compressed edit cannot save during a mode change', async choice => {
    const saveNote = vi.fn().mockRejectedValueOnce(new ChaError('internal_error', 'Save failed.')).mockResolvedValue(undefined);
    render(<App client={fixtureClient({
      listNoteItems: async () => [{ ...plan, has_compressed: true }],
      getNote: async (id, mode) => ({
        id, document_id: mode === 'compressed' ? 40 : id,
        document: noteDocument(mode === 'compressed' ? 'Compressed body' : 'Original text'),
      }),
      saveNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Show compressed' }));
    await screen.findByText('Compressed body');
    act(() => typeInNote(' revised'));
    chooseNoteMode('Full text');
    const dialog = await screen.findByRole('dialog', { name: 'Could not save “Plan”.' });
    const mode = screen.getByRole('button', { name: 'Note text mode' });
    expect(mode).toHaveAttribute('title', expect.stringMatching(/^Compressed mode/));
    expect(mode).toBeDisabled();
    fireEvent.click(within(dialog).getByRole('button', { name: choice }));
    if (choice === 'Stay') {
      await waitFor(() => expect(mode).toBeEnabled());
      expect(noteEditor().getText()).toBe('Compressed body revised');
      expect(mode).toHaveAttribute('title', 'Compressed mode; showing compressed');
    } else {
      await screen.findByText('Original text');
      expect(screen.getByRole('button', { name: 'Note text mode' }))
        .toHaveAttribute('title', 'Full text mode; showing full text');
    }
    expect(saveNote.mock.calls.map(([id]) => id)).toEqual(choice === 'Retry' ? [40, 40] : [40]);
  });

  it('shows the mode button only with the regular Notes toolbar', async () => {
    const opening = deferred<void>();
    const client = fixtureClient({
      listNoteItems: async () => [{ ...plan, read_only: true }],
      getNote: vi.fn()
        .mockImplementationOnce(async () => {
          await opening.promise;
          throw new ChaError('internal_error', 'Cannot read this note.');
        })
        .mockImplementation(async (id: number, mode: NoteTextMode) => mode === 'compressed'
          ? { id, document_id: 40, document: noteDocument('Compressed body') }
          : { id, document_id: id, document: noteDocument('Original text') }),
    });
    const bus = contextBus();
    render(<App client={client} contextEvents={bus} />);
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Message' })).toBeEnabled());
    act(() => bus.layout('split'));
    const note = within(await screen.findByRole('region', { name: 'Note' }));
    expect(screen.getByRole('tab', { name: 'Chat' })).toHaveAttribute('aria-selected', 'true');
    expect(note.queryByRole('button', { name: 'Note text mode' })).not.toBeInTheDocument();
    await openNotes();
    expect(note.queryByRole('button', { name: 'Note text mode' })).not.toBeInTheDocument();

    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await note.findByText('Opening…');
    expect(note.queryByRole('button', { name: 'Note text mode' })).not.toBeInTheDocument();
    await act(async () => opening.resolve());
    await note.findByText(/Cannot read this note/);
    expect(note.queryByRole('button', { name: 'Note text mode' })).not.toBeInTheDocument();

    fireEvent.click(note.getByRole('button', { name: 'Retry' }));
    await note.findByText('Original text');
    const mode = await screen.findByRole('button', { name: 'Note text mode' });
    const toolbar = mode.closest('.cha-notes-toolbar') as HTMLElement;
    expect(toolbar).toContainElement(screen.getByRole('button', { name: 'Playback speed' }));
    expect(within(toolbar).getAllByRole('button')[0]).toBe(mode);
    expect(mode.textContent).toBe('');
    expect(mode).toHaveAttribute('aria-expanded', 'false');
    fireEvent.click(mode);
    const menu = screen.getByRole('menu', { name: 'Note text mode' });
    const choices = within(menu).getAllByRole('menuitemradio');
    expect(choices.map(item => item.textContent)).toEqual(['Full text', 'Compressed']);
    expect(choices[0]).toBeChecked();
    expect(choices[1]).not.toBeChecked();
    fireEvent.keyDown(choices[0], { key: 'ArrowDown' });
    expect(choices[1]).toHaveFocus();
    fireEvent.keyDown(choices[1], { key: 'Escape' });
    expect(noteEditor().isEditable).toBe(false);
    expect(noteEditor().view.dom).toHaveFocus();

    chooseNoteMode('Compressed');
    await note.findByText('Compressed body');
    act(() => bus.layout('toggle-toolbar'));
    expect(note.queryByRole('button', { name: 'Note text mode' })).not.toBeInTheDocument();
    expect(note.queryByRole('button', { name: 'Playback speed' })).not.toBeInTheDocument();
    act(() => bus.layout('toggle-toolbar'));
    fireEvent.click(note.getByRole('button', { name: 'Note text mode' }));
    expect(note.getByRole('menuitemradio', { name: 'Compressed' })).toBeChecked();
    expect(note.getByRole('menuitemradio', { name: 'Compressed' })).toHaveFocus();
    expect(note.getByRole('menuitemradio', { name: 'Full text' })).not.toBeChecked();
  });

  it('keeps the find bar through a sync reload and closes it on a mode change', async () => {
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument('Original text') }));
    const bus = contextBus();
    render(<App client={fixtureClient({ listNoteItems: async () => [plan], getNote })} contextEvents={bus} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    fireEvent.click(screen.getByRole('button', { name: 'Find' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Find' }), { target: { value: 'Original' } });
    act(() => bus.notesChanged());
    await waitFor(() => expect(getNote).toHaveBeenCalledTimes(2));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    expect(screen.getByRole('textbox', { name: 'Find' })).toHaveValue('Original');
    chooseNoteMode('Compressed');
    await waitFor(() => expect(getNote).toHaveBeenCalledTimes(3));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    expect(screen.queryByRole('textbox', { name: 'Find' })).not.toBeInTheDocument();
  });

  it('finishes a file import into the compressed row after switching to full text', async () => {
    const reading = deferred<{ name: string; text: string }>();
    const documents = new Map<number, Record<string, unknown>>([
      [plan.id, noteDocument('Original text')], [40, noteDocument('Compressed body')],
    ]);
    const saveNote = vi.fn(async (id: number, document: Record<string, unknown>) => { documents.set(id, document); });
    const setNoteReadOnly = vi.fn(async (id: number, read_only: boolean) => ({ ...plan, id, read_only, has_compressed: true }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [{ ...plan, has_compressed: true }],
      getNote: async (id, mode) => {
        const documentId = mode === 'compressed' ? 40 : id;
        return { id, document_id: documentId, document: documents.get(documentId)! };
      },
      readNoteTextFile: () => reading.promise,
      saveNote, setNoteReadOnly,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Show compressed' }));
    await screen.findByText('Compressed body');
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: '/tmp/plan.txt' } });
    fireEvent.keyDown(prompt, { key: 'Enter' });
    await screen.findByText('Retrieving content…');
    chooseNoteMode('Full text');
    await screen.findByText('Original text');
    const original = noteEditor();
    await act(async () => reading.resolve({ name: 'plan.txt', text: 'Imported body' }));
    await waitFor(() => expect(setNoteReadOnly).toHaveBeenCalledExactlyOnceWith(plan.id, true));
    expect(saveNote).toHaveBeenCalledExactlyOnceWith(40, noteDocument('Imported body'));
    expect(noteEditor()).toBe(original);
    expect(original.getText()).toBe('Original text');
    expect(original.isEditable).toBe(false);
    chooseNoteMode('Compressed');
    await screen.findByText('Imported body');
    expect(noteEditor().isEditable).toBe(false);
  });
});

describe('Notes in the application', () => {
  it.each(['chat', 'note', 'chat during load'])('keeps typing focus in %s when an incoming change reloads the open note', async (focus) => {
    const bus = contextBus();
    let finish!: () => void;
    const getNote = vi.fn()
      .mockResolvedValueOnce({ id: plan.id, document_id: plan.id, document: noteDocument('Saved plan') })
      .mockImplementation(() => new Promise(resolve => {
        finish = () => resolve({ id: plan.id, document_id: plan.id, document: noteDocument('Incoming plan') });
      }));
    const saveNote = vi.fn(async () => undefined);
    render(<App client={fixtureClient({ listNoteItems: async () => [plan], getNote, saveNote })} contextEvents={bus} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => bus.layout('split'));
    const chat = await screen.findByRole('textbox', { name: 'Message' });
    act(() => {
      if (focus === 'chat') chat.focus();
      else noteEditor().view.focus();
    });
    fireEvent.change(chat, { target: { value: 'Keep typing here' } });

    act(() => bus.notesChanged());
    await waitFor(() => expect(getNote).toHaveBeenCalledTimes(2));
    if (focus === 'chat during load') act(() => chat.focus());
    await act(async () => finish());
    await screen.findByText('Incoming plan');
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    expect(focus === 'note' ? noteEditor().view.dom : chat).toHaveFocus();
    expect(chat).toHaveValue('Keep typing here');
    expect(getNote).toHaveBeenCalledTimes(2);
    expect(saveNote).not.toHaveBeenCalled();
  });

  it.each([
    ['https://example.org/article', false], ['https://youtu.be/video', true],
  ] as const)('creates, imports, titles, saves, and locks %s without submitting the prompt (hidden=%s)', async (url, hidden) => {
    let finish!: (page: NotePage) => void;
    const readNotePage = vi.fn(() => new Promise<NotePage>(resolve => { finish = resolve; }));
    const notes = importClient(readNotePage);
    const bus = contextBus();
    render(<App client={notes.client} contextEvents={bus} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    fireEvent.change(screen.getByRole('textbox', { name: 'Prompt' }), { target: { value: 'Keep my draft' } });
    act(() => typeInNote(' edited'));
    if (hidden) act(() => bus.layout('toggle-notes-prompt'));
    await createUrlNote(url);
    await waitFor(() => expect(readNotePage).toHaveBeenCalledExactlyOnceWith(url, expect.any(Function)));
    expect(notes.createNoteItem).toHaveBeenCalledExactlyOnceWith(null, 'note', 'New note');
    expect(notes.documents.get(plan.id)).toEqual(noteDocument('Saved plan edited'));
    expect(notes.saveNote.mock.invocationCallOrder[0]).toBeLessThan(notes.createNoteItem.mock.invocationCallOrder[0]);
    expect(screen.getByText('Loading content…')).toHaveAttribute('role', 'status');
    const editor = noteEditor();
    expect(editor.isEditable).toBe(true);
    let finishSave!: () => void;
    notes.saveNote.mockImplementationOnce(async (id, document) => {
      await new Promise<void>(resolve => { finishSave = resolve; });
      notes.documents.set(id, document);
    });
    await act(async () => finish({ url, title: 'Content title', markdown: '**Imported content**' }));
    await screen.findByText('Saving note…');
    expect(notes.setNoteReadOnly).not.toHaveBeenCalled();
    await act(async () => finishSave());
    await screen.findByRole('treeitem', { name: 'Content title' });
    await waitFor(() => expect(editor.isEditable).toBe(false));
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Content title\nImported content');
    expect(editor.view.dom.querySelector('strong')).toHaveTextContent('Imported content');
    expect(notes.documents.get(9)).toEqual(editor.getJSON());
    expect(notes.setNoteReadOnly).toHaveBeenCalledExactlyOnceWith(9, true);
    expect(notes.saveNote.mock.invocationCallOrder.at(-1)).toBeLessThan(notes.setNoteReadOnly.mock.invocationCallOrder[0]);
    expect(screen.queryByText('Loading content…')).toBeNull();
    if (hidden) act(() => bus.layout('toggle-notes-prompt'));
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep my draft');
    expect(readNotePage).toHaveBeenCalledOnce();
  });

  it('creates a note from a file path and saves its title and content', async () => {
    const readFile = vi.fn(async () => ({ name: '/tmp/plan.md', text: '# File title\n\nFile content' }));
    const notes = importClient(vi.fn(), readFile);
    render(<App client={notes.client} contextEvents={contextBus()} />);
    await openNotes();
    await createUrlNote('/tmp/plan.md');
    await screen.findByRole('treeitem', { name: 'File title' });
    await waitFor(() => expect(notes.setNoteReadOnly).toHaveBeenCalledWith(9, true));
    expect(readFile).toHaveBeenCalledExactlyOnceWith('/tmp/plan.md');
    expect(notes.createNoteItem).toHaveBeenCalledExactlyOnceWith(null, 'note', 'New note');
    expect(JSON.stringify(notes.documents.get(9))).toContain('File content');
  });

  it.each([false, true])('saves concurrent imports to their original notes across repeated switches (second finishes first=%s)', async secondFirst => {
    const finishes: ((page: NotePage) => void)[] = [];
    const reports: NonNullable<Parameters<NotesClient['readNotePage']>[1]>[] = [];
    const readNotePage = vi.fn<NotesClient['readNotePage']>((_url, report) => {
      reports.push(report!);
      return new Promise(resolve => { finishes.push(resolve); });
    });
    const notes = importClient(readNotePage);
    render(<App client={notes.client} />);
    await openNotes();
    await createUrlNote('https://youtu.be/first');
    await waitFor(() => expect(readNotePage).toHaveBeenCalledTimes(1));
    const original = noteEditor();
    await createUrlNote('https://youtu.be/second');
    await waitFor(() => expect(readNotePage).toHaveBeenCalledTimes(2));
    expect(original.isDestroyed).toBe(true);
    fireEvent.click(screen.getByRole('treeitem', { name: 'New note' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    fireEvent.click(screen.getByRole('treeitem', { name: 'New note 1' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    fireEvent.click(screen.getByRole('treeitem', { name: 'New note' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => { reports[0]('formatting_transcript'); reports[1]('formatting_article'); });
    expect(screen.getByText('Loading content…')).toHaveTextContent('Formatting transcript…');
    const pages = [
      { url: 'https://youtu.be/first', title: 'First title', markdown: 'First content' },
      { url: 'https://youtu.be/second', title: 'Second title', markdown: 'Second content' },
    ];
    for (const index of secondFirst ? [1, 0] : [0, 1]) {
      await act(async () => finishes[index](pages[index]));
      await waitFor(() => expect(notes.setNoteReadOnly).toHaveBeenCalledWith(9 + index, true));
    }
    expect(noteEditor().getText()).toContain('First content');
    expect(noteEditor().getText()).not.toContain('Second content');
    expect(JSON.stringify(notes.documents.get(9))).toContain('First content');
    expect(JSON.stringify(notes.documents.get(10))).toContain('Second content');
    expect(notes.renameNoteItem).toHaveBeenCalledWith(9, 'First title');
    expect(notes.renameNoteItem).toHaveBeenCalledWith(10, 'Second title');
    fireEvent.click(screen.getByRole('treeitem', { name: 'Second title' }));
    await waitFor(() => expect(noteEditor().getText()).toContain('Second content'));
    expect(noteEditor().isEditable).toBe(false);
    expect(readNotePage).toHaveBeenCalledTimes(2);
    expect(screen.queryByText('Loading content…')).toBeNull();
  });

  it.each([false, true])('preserves edits made after returning to an importing note (finish while hidden=%s)', async hidden => {
    let finish!: (page: NotePage) => void;
    const readNotePage = vi.fn(() => new Promise<NotePage>(resolve => { finish = resolve; }));
    const notes = importClient(readNotePage);
    render(<App client={notes.client} />);
    await openNotes();
    await createUrlNote('https://youtu.be/video');
    await waitFor(() => expect(readNotePage).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    fireEvent.click(screen.getByRole('treeitem', { name: 'New note' }));
    await waitFor(() => {
      expect(noteEditor().isEditable).toBe(true);
      expect(noteEditor().getText()).toBe('');
    });
    act(() => typeInNote('My edits'));
    if (hidden) {
      fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' }));
      await screen.findByText('Saved plan');
    }
    await act(async () => finish({ url: 'https://youtu.be/video', title: 'Video', markdown: 'Imported transcript' }));
    await waitFor(() => expect(JSON.stringify(notes.documents.get(9))).toContain('Imported transcript'));
    expect(JSON.stringify(notes.documents.get(9))).toContain('My edits');
    expect(notes.setNoteReadOnly).not.toHaveBeenCalled();
    if (hidden) {
      expect(noteEditor().getText()).toBe('Saved plan');
      fireEvent.click(screen.getByRole('treeitem', { name: 'New note' }));
    }
    await waitFor(() => expect(noteEditor().getText()).toContain('Imported transcript'));
    expect(noteEditor().getText()).toContain('My edits');
    expect(readNotePage).toHaveBeenCalledOnce();
  });

  it('assigns distinct titles when concurrent imports finish together', async () => {
    const finishes: ((page: NotePage) => void)[] = [];
    const notes = importClient(() => new Promise(resolve => { finishes.push(resolve); }));
    render(<App client={notes.client} />);
    await openNotes();
    await createUrlNote('https://youtu.be/first');
    await waitFor(() => expect(finishes).toHaveLength(1));
    await createUrlNote('https://youtu.be/second');
    await waitFor(() => expect(finishes).toHaveLength(2));
    await act(async () => finishes.forEach((finish, index) => finish({ url: `https://youtu.be/${index}`,
      title: 'Shared title', markdown: `Content ${index}` })));
    await waitFor(() => expect(notes.setNoteReadOnly).toHaveBeenCalledTimes(2));
    expect(notes.renameNoteItem).toHaveBeenCalledWith(9, 'Shared title');
    expect(notes.renameNoteItem).toHaveBeenCalledWith(10, 'Shared title 1');
    expect(JSON.stringify(notes.documents.get(9))).toContain('Content 0');
    expect(JSON.stringify(notes.documents.get(10))).toContain('Content 1');
  });

  it.each([
    ['page', false], ['page', true], ['file', false], ['file', true],
  ] as const)('saves a hidden %s import and does not restart it on remount (finish while hidden=%s)', async (kind, finishHidden) => {
    let finish!: () => void;
    const pending = new Promise<void>(resolve => { finish = resolve; });
    const readNotePage = vi.fn(async () => { await pending; return { url: 'https://example.org/article', title: 'Late title', markdown: 'Late content' }; });
    const readNoteTextFile = vi.fn(async () => { await pending; return { name: '/plan.md', text: '# Late title\n\nLate content' }; });
    const readSource = kind === 'page' ? readNotePage : readNoteTextFile;
    const notes = importClient(readNotePage, readNoteTextFile);
    const bus = contextBus();
    render(<App client={notes.client} contextEvents={bus} />);
    await openNotes();
    await createUrlNote(kind === 'page' ? 'https://example.org/article' : '/plan.md');
    await waitFor(() => expect(readSource).toHaveBeenCalledOnce());
    const original = noteEditor();
    act(() => bus.layout('split'));
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Welcome/ }));
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Message' })).toBeInTheDocument());
    expect(noteEditor()).toBe(original);
    expect(screen.getByText('Loading content…')).toBeInTheDocument();
    act(() => bus.layout('single'));
    // DOM removal can precede the passive effect cleanup that destroys the editor.
    await waitFor(() => {
      expect(document.querySelector('.ProseMirror')).toBeNull();
      expect(original.isDestroyed).toBe(true);
    });
    const resolve = async () => act(async () => finish());
    if (finishHidden) await resolve();
    act(() => bus.layout('split'));
    await waitFor(() => expect(document.querySelector('.ProseMirror')).not.toBeNull());
    if (!finishHidden) await resolve();
    await waitFor(() => expect(noteEditor().isEditable).toBe(false));
    expect(noteEditor().getText()).toContain('Late content');
    expect(screen.queryByText('Loading content…')).toBeNull();
    expect(readSource).toHaveBeenCalledOnce();
    expect(notes.renameNoteItem).toHaveBeenCalledExactlyOnceWith(9, 'Late title');
    expect(notes.setNoteReadOnly).toHaveBeenCalledExactlyOnceWith(9, true);
    expect(notes.documents.get(9)).toEqual(noteEditor().getJSON());
  });

  it('generates direct read-only folder notes in tree order and restores folder focus', async () => {
    const info = vi.fn(async (id: number) => ({ count: id === 7 ? 2 : 0 }));
    const start = vi.fn(async (id: number) => ({ attempt_id: id }));
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument(`Text ${id}`) }));
    const changed = vi.fn();
    window.addEventListener('cha-note-audio-changed', changed);
    const child = { ...plan, parent_id: study.id, read_only: true };
    try {
      render(<App client={fixtureClient({
        listNoteItems: async () => [study,
          { ...child, id: 4, name: 'Z chapter' },
          { ...child, id: 5, name: 'A chapter' },
          { ...child, id: 6, name: 'Editable', read_only: false },
          { ...child, id: 7, name: 'B existing' },
          { ...child, id: 8, name: 'ANNOTATIONS', kind: 'annotation' },
          { ...study, id: 2, parent_id: study.id, name: 'Subfolder' },
          { ...child, id: 3, parent_id: 2, name: 'Nested' }],
        getNote, noteAudio: { contextEpoch: () => 1, startBackground: start, backgroundStatus: vi.fn().mockResolvedValue({ pending: false }), info, source: vi.fn(), release: vi.fn() },
      })} />);
      await openNotes();
      fireEvent.click(await screen.findByRole('button', { name: 'Actions for Study' }));
      const menuItems = screen.getAllByRole('menuitem').map(node => node.textContent);
      expect(menuItems.indexOf('Generate audio') + 1).toBe(menuItems.indexOf('Compress'));
      await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Generate audio' })).toBeEnabled());
      fireEvent.click(screen.getByRole('menuitem', { name: 'Generate audio' }));
      await waitFor(() => expect(screen.getByLabelText('Voice')).toBeEnabled());
      expect(info.mock.calls.map(([id]) => id)).toEqual([5, 7, 4]);
      expect(getNote).not.toHaveBeenCalled();
      fireEvent.change(screen.getByLabelText('Voice'), { target: { value: 'Kore' } });
      fireEvent.click(screen.getByRole('button', { name: 'Generate' }));
      await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
      expect(start.mock.calls).toEqual([[5, 'Kore', ['Text 5'], true], [4, 'Kore', ['Text 4'], true]]);
      expect(getNote.mock.calls.map(([id]) => id)).toEqual([5, 4]);
      expect(changed.mock.calls.map(([event]) => event.detail)).toEqual([study.id]);
      // DOM removal can precede the passive effect cleanup that restores focus.
      await waitFor(() => expect(screen.getByRole('treeitem', { name: 'Study' })).toHaveFocus());
      fireEvent.click(screen.getByRole('button', { name: `Actions for ${bootstrapFixture.vault_name}` }));
      expect(screen.queryByRole('menuitem', { name: 'Generate audio' })).toBeNull();
    } finally { window.removeEventListener('cha-note-audio-changed', changed); }
  });

  it('rejects stale folder targets after a context notice without a causing request', async () => {
    let epoch = 1;
    let changed!: (event: { state: string }) => void;
    const bus = { on: vi.fn((name: string, handler: (event: never) => void) => {
      if (name === 'app.contextChanged') changed = handler as never;
      return () => {};
    }) };
    const getNote = vi.fn();
    const start = vi.fn();
    render(<App contextEvents={bus} client={fixtureClient({
      listNoteItems: async () => [study, { ...plan, parent_id: study.id, read_only: true }],
      getNote,
      noteAudio: { contextEpoch: () => epoch, startBackground: start,
        backgroundStatus: async () => ({ pending: false }), info: async () => ({ count: 0 }), source: vi.fn(), release: vi.fn() },
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('button', { name: 'Actions for Study' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Generate audio' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Generate audio' }));
    await waitFor(() => expect(screen.getByLabelText('Voice')).toBeEnabled());
    const dialog = screen.getByRole('dialog');
    act(() => { epoch = 2; changed({ state: 'running' }); });
    expect(screen.getByRole('dialog')).toBe(dialog);
    fireEvent.change(screen.getByLabelText('Voice'), { target: { value: 'Kore' } });
    fireEvent.click(screen.getByRole('button', { name: 'Generate' }));
    await screen.findByText('The vault changed. Close this dialog and start again.');
    expect(getNote).not.toHaveBeenCalled();
    expect(start).not.toHaveBeenCalled();
  });

  it.each([false, true])('opens another read-only book note after earlier deletion: %s', async (deleteEarlier) => {
    const first = { ...plan, read_only: true };
    const second = { ...first, id: 5, name: 'Next chapter' };
    let items = [{ ...first, id: 3, name: 'Earlier chapter' }, first, second];
    const info = vi.fn(async () => ({ count: 0 }));
    render(<App client={fixtureClient({
      listNoteItems: async () => items,
      deleteNoteItem: async id => { items = items.filter(item => item.id !== id); },
      getNote: async id => ({ id, document_id: id, document: noteDocument(`Chapter ${id}`) }),
      noteAudio: { contextEpoch: () => 1, startBackground: vi.fn().mockResolvedValue({ attempt_id: 1 }), backgroundStatus: vi.fn().mockResolvedValue({ pending: false }), info, source: vi.fn(), release: vi.fn() },
    })} />);
    await openNotes();
    if (deleteEarlier) {
      fireEvent.click(await screen.findByRole('treeitem', { name: 'Earlier chapter' }));
      await waitFor(() => expect(noteEditor().isEditable).toBe(false));
      await deleteItem('Earlier chapter', 'note', false);
      await waitFor(() => expect(screen.queryByRole('treeitem', { name: 'Earlier chapter' })).not.toBeInTheDocument());
    }
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(false));
    fireEvent.click(screen.getByRole('treeitem', { name: 'Next chapter' }));
    await screen.findByText('Chapter 5');
    await waitFor(() => expect(info).toHaveBeenCalledWith(second.id));
    expect(screen.queryByText('Something went wrong while showing CHA.')).not.toBeInTheDocument();
  });

  it('generates saved blocks and disables another generation while the note is pending', async () => {
    const changed = vi.fn();
    window.addEventListener('cha-note-audio-changed', changed);
    const document = { type: 'doc', content: [
      { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text: 'Title' }] },
      { type: 'paragraph', content: [{ type: 'text', text: 'First' }, { type: 'hardBreak' }, { type: 'text', text: 'Second' }] },
    ] };
    const start = vi.fn(async () => ({ attempt_id: 1 }));
    const backgroundStatus = vi.fn()
      .mockResolvedValueOnce({ pending: false })
      .mockResolvedValueOnce({ pending: true, attempt_id: 1 })
      .mockResolvedValueOnce({ pending: false });
    try {
      render(<App client={fixtureClient({
        listNoteItems: async () => [{ ...plan, read_only: true }],
        getNote: async id => ({ id, document_id: id, document }),
        noteAudio: { contextEpoch: () => 1, startBackground: start, backgroundStatus, info: async () => ({ count: 0 }), source: vi.fn(), release: vi.fn() },
      })} />);
      await openNotes();
      fireEvent.click(await screen.findByRole('button', { name: 'Actions for Plan' }));
      expect(screen.getByRole('menuitem', { name: 'Generate audio' })).toBeDisabled();
      await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Generate audio' })).toBeEnabled());
      fireEvent.click(screen.getByRole('menuitem', { name: 'Generate audio' }));
      const dialog = await screen.findByRole('dialog', { name: 'Generate note audio' });
      expect(changed).toHaveBeenCalledOnce();
      expect(start).not.toHaveBeenCalled();
      await waitFor(() => expect(within(dialog).getByLabelText('Voice')).toBeEnabled());
      fireEvent.change(within(dialog).getByLabelText('Voice'), { target: { value: 'grant' } });
      fireEvent.click(within(dialog).getByRole('button', { name: 'Generate' }));
      await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
      expect(start).toHaveBeenCalledExactlyOnceWith(plan.id, 'grant', ['Title', 'First\nSecond']);
      expect(changed).toHaveBeenCalledTimes(1);

      await act(async () => fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' })));
      expect(backgroundStatus).toHaveBeenLastCalledWith(plan.id);
      expect(screen.getByRole('menuitem', { name: 'Generate audio' })).toBeDisabled();
      expect(screen.getByRole('menuitem', { name: 'Make editable' })).toBeEnabled();
      fireEvent.click(screen.getByRole('menuitem', { name: 'Generate audio' }));
      expect(screen.queryByRole('dialog')).toBeNull();
      expect(start).toHaveBeenCalledOnce();

      fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
      fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
      await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Generate audio' })).toBeEnabled());
      expect(backgroundStatus).toHaveBeenCalledTimes(3);
    } finally { window.removeEventListener('cha-note-audio-changed', changed); }
  });

  it('finishes a pending Notes prompt submission while hidden and preserves text typed after sending', async () => {
    const page = { url: 'https://example.org/', title: 'Article', markdown: 'Imported article' };
    let finish!: (value: typeof page) => void;
    const readNotePage = vi.fn(() => new Promise<typeof page>((resolve) => { finish = resolve; }));
    const bus = contextBus();
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document_id: id, document: noteDocument('Original') }),
      saveNote: async () => undefined,
      readNotePage,
    })} contextEvents={bus} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    const editor = noteEditor();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: page.url } });
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    await waitFor(() => expect(readNotePage).toHaveBeenCalledOnce());
    fireEvent.change(prompt, { target: { value: `${page.url} trailing draft` } });
    act(() => bus.layout('toggle-notes-prompt'));
    expect(screen.queryByRole('textbox', { name: 'Prompt' })).toBeNull();
    await act(async () => { finish(page); });
    await waitFor(() => expect(editor.getText()).toContain('Imported article'));
    await waitFor(() => expect(prompt).toHaveValue('trailing draft'));
    act(() => bus.layout('toggle-notes-prompt'));
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toBe(prompt);
    expect(prompt).toHaveValue('trailing draft');
    expect(noteEditor()).toBe(editor);
  });

  it('reports the applied toggle state and keeps the menu checked when a note cannot be saved', async () => {
    const post = vi.fn();
    window.__CHA_NATIVE_CONNECTION_ID__ = 'layout-test';
    window.__CHA_NATIVE_POST__ = post;
    const bus = contextBus();
    let failSave = true;
    const reportedStates = () => post.mock.calls
      .map(([message]) => JSON.parse(message as string) as { native_action: string; split: boolean })
      .filter((message) => message.native_action === 'view_state')
      .map((message) => message.split);
    try {
      render(<App client={fixtureClient({
        listNoteItems: async () => [plan],
        getNote: async (id) => ({ id, document_id: id, document: noteDocument('Saved plan') }),
        saveNote: async () => { if (failSave) throw new ChaError('internal_error', 'Save failed.'); },
      })} contextEvents={bus} />);
      await openNotes();
      expect(reportedStates()).toEqual([false]);
      fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
      await waitFor(() => expect(noteEditor().isEditable).toBe(true));
      act(() => bus.layout('toggle'));
      await screen.findByRole('textbox', { name: 'Message' });
      expect(reportedStates()).toEqual([false, true]);
      fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
      fireEvent.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Welcome/ }));
      act(() => typeInNote(' unsaved'));
      act(() => bus.layout('toggle'));
      const decision = await screen.findByRole('dialog');
      expect(reportedStates()).toEqual([false, true]);
      await act(async () => { fireEvent.click(within(decision).getByRole('button', { name: 'Stay' })); });
      expect(screen.getByRole('separator', { name: 'Resize notes and chat' })).toBeInTheDocument();
      expect(reportedStates()).toEqual([false, true]);
      failSave = false;
      act(() => bus.layout('toggle'));
      await waitFor(() => expect(reportedStates()).toEqual([false, true, false]));
      expect(screen.queryByRole('separator', { name: 'Resize notes and chat' })).toBeNull();
    } finally {
      delete window.__CHA_NATIVE_POST__;
      delete window.__CHA_NATIVE_CONNECTION_ID__;
    }
  });

  it('saves and restores a note hidden by layout while keeping chat mounted', async () => {
    const bus = contextBus();
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument('Saved plan') }));
    const saveNote = vi.fn(async () => undefined);
    render(<App client={fixtureClient({ listNoteItems: async () => [plan], getNote, saveNote })} contextEvents={bus} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => bus.layout('split'));
    const chat = await screen.findByRole('textbox', { name: 'Message' });
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Welcome/ }));
    act(() => typeInNote(' pending'));
    act(() => bus.layout('single'));
    await waitFor(() => expect(document.querySelector('.ProseMirror')).toBeNull());
    expect(saveNote).toHaveBeenCalledOnce();
    expect(screen.getByRole('textbox', { name: 'Message' })).toBe(chat);
    fireEvent.click(screen.getByRole('tab', { name: 'Notes' }));
    act(() => bus.layout('split'));
    await waitFor(() => expect(getNote).toHaveBeenCalledTimes(2));
    expect(getNote.mock.calls[1]).toEqual([plan.id, 'full_text']);
    expect(screen.getByRole('textbox', { name: 'Message' })).toBe(chat);
  });

  it('keeps the visible editor and chat composer mounted across layout and navigation changes', async () => {
    const bus = contextBus();
    const client = notesClient();
    render(<App client={client} contextEvents={bus} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    const editor = noteEditor();
    act(() => bus.layout('split'));
    expect(noteEditor()).toBe(editor);
    const chat = await screen.findByRole('textbox', { name: 'Message' });
    fireEvent.change(chat, { target: { value: 'Unsent chat' } });
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    expect(screen.getByRole('textbox', { name: 'Message' })).toBe(chat);
    act(() => bus.layout('single'));
    expect(noteEditor()).toBe(editor);
    expect(screen.queryByRole('textbox', { name: 'Message' })).toBeNull();
    act(() => bus.layout('split'));
    expect(noteEditor()).toBe(editor);
    expect(await screen.findByRole('textbox', { name: 'Message' })).toHaveValue('Unsent chat');
  });

  it('saves pending edits before locking, then unlocks and locks again', async () => {
    let finishSave!: () => void;
    const saveNote = vi.fn(() => new Promise<void>((resolve) => { finishSave = resolve; }));
    const setNoteReadOnly = vi.fn(async (id: number, readOnly: boolean) => ({ ...plan, id, read_only: readOnly }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document_id: id, document: noteDocument('Original') }),
      saveNote, setNoteReadOnly,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => typeInNote(' edited'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Make read-only' }));
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    expect(setNoteReadOnly).not.toHaveBeenCalled();
    expect(noteEditor().isEditable).toBe(false);
    await act(async () => finishSave());
    await waitFor(() => expect(setNoteReadOnly).toHaveBeenCalledWith(plan.id, true));
    expect(saveNote.mock.calls[0]).toEqual([plan.id, noteEditor().getJSON()]);
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toBeEnabled();
    expect(screen.getByRole('button', { name: 'Edit' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Headings' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Insert' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'History' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Make editable' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toBeEnabled());
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Make read-only' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(false));
    expect(setNoteReadOnly.mock.calls.map((call) => call[1])).toEqual([true, false, true]);
    expect(saveNote).toHaveBeenCalledOnce();
  });

  it.each(['save', 'lock'])('keeps the note editable and its content intact when %s fails', async (failure) => {
    const saveNote = vi.fn(async () => {
      if (failure === 'save') throw new ChaError('internal_error', 'Save failed.');
    });
    const setNoteReadOnly = vi.fn(async (): Promise<NoteItem> => {
      throw new ChaError('internal_error', 'Lock failed.');
    });
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document_id: id, document: noteDocument('Original') }),
      saveNote, setNoteReadOnly,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => typeInNote(' edited'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Make read-only' }));
    await waitFor(() => expect(screen.getAllByRole('alert').some((alert) =>
      alert.textContent?.includes(failure === 'save' ? 'Save failed.' : 'Lock failed.'))).toBe(true));
    expect(noteEditor().isEditable).toBe(true);
    expect(noteEditor().getText()).toContain('edited');
    if (failure === 'save') expect(setNoteReadOnly).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    expect(await screen.findByRole('menuitem', { name: 'Make read-only' })).toBeInTheDocument();
  });

  it('opens read-only notes with search and copying available and stays read-only after compression', async () => {
    const locked = { ...plan, read_only: true };
    const saveNote = vi.fn();
    const compressNote = vi.fn(async () => ({ accepted: true as const }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [locked],
      getNote: async (id) => ({ id, document_id: id, document: noteDocument('Original text') }),
      saveNote, compressNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByRole('button', { name: 'Find' });
    const editor = await waitFor(() => noteEditor());
    expect(editor.isEditable).toBe(false);
    const original = editor.getJSON();
    act(() => {
      editor.commands.selectAll();
      typeInNote('Rejected');
      editor.commands.toggleBold();
      editor.commands.undo();
    });
    expect(editor.state.selection.empty).toBe(false);
    expect(editor.getJSON()).toEqual(original);
    fireEvent.click(screen.getByRole('button', { name: 'Find' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Find' }), { target: { value: 'Original' } });
    expect(screen.getByRole('button', { name: 'Next' })).toBeEnabled();
    expect(screen.getByRole('button', { name: 'Replace' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await waitFor(() => expect(compressNote).toHaveBeenCalledOnce());
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(editor.isEditable).toBe(false);
    expect(saveNote).not.toHaveBeenCalled();
  });

  it.each(['web', 'path', 'picker'])('saves and locks the note after a successful %s import', async (source) => {
    let saved: Record<string, unknown> = source === 'web'
      ? { type: 'doc', content: [{ type: 'paragraph' }] } : noteDocument('Original');
    const saveNote = vi.fn(async (_id: number, document: Record<string, unknown>) => { saved = document; });
    const setNoteReadOnly = vi.fn(async (id: number, readOnly: boolean) => ({ ...plan, id, read_only: readOnly }));
    const other = { ...plan, id: 5, name: 'Other' };
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan, other],
      getNote: async (id) => ({ id, document_id: id, document: saved }),
      readNotePage: async () => ({ url: 'https://example.org/', title: 'Title', markdown: 'Imported' }),
      readNoteTextFile: async () => ({ name: '/tmp/file.txt', text: 'Imported' }),
      saveNote, setNoteReadOnly,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    if (source === 'picker') {
      fireEvent.change(screen.getByLabelText('Text file', { selector: 'input' }), {
        target: { files: [new File(['Imported'], 'file.txt')] },
      });
    } else {
      const prompt = screen.getByRole('textbox', { name: 'Prompt' });
      fireEvent.change(prompt, { target: { value: source === 'web' ? 'https://example.org/' : '/tmp/file.txt' } });
      fireEvent.keyDown(prompt, { key: 'Enter' });
    }
    await waitFor(() => expect(setNoteReadOnly).toHaveBeenCalledWith(plan.id, true));
    expect(saveNote).toHaveBeenCalledOnce();
    expect(saveNote.mock.invocationCallOrder[0]).toBeLessThan(setNoteReadOnly.mock.invocationCallOrder[0]);
    expect(noteEditor().getText()).toContain('Imported');
    expect(noteEditor().isEditable).toBe(false);
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toBeEnabled();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('');
    fireEvent.click(screen.getByRole('treeitem', { name: 'Other' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => {
      expect(noteEditor().isEditable).toBe(false);
      expect(noteEditor().getText()).toContain('Imported');
    });
  });

  it('undoes an uploaded file in one step after Make editable', async () => {
    const notes = importClient(vi.fn());
    render(<App client={notes.client} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    fireEvent.change(screen.getByLabelText('Text file', { selector: 'input' }), {
      target: { files: [new File(['# Imported\n\n**Body**'], 'plan.md')] },
    });
    await waitFor(() => expect(notes.setNoteReadOnly).toHaveBeenCalledExactlyOnceWith(plan.id, true));
    await waitFor(() => expect(noteEditor().isEditable).toBe(false));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Make editable' }));
    await waitFor(() => expect(screen.getByRole('button', { name: 'History' })).toBeEnabled());
    fireEvent.click(historyAction('Undo'));
    expect(noteEditor().getText()).toBe('Saved plan');
    fireEvent.click(historyAction('Redo'));
    expect(noteEditor().getText({ blockSeparator: '\n' })).toBe('Imported\nBody');
  });

  it('keeps a note with text editable after a web import', async () => {
    const saveNote = vi.fn(async () => undefined);
    const setNoteReadOnly = vi.fn(async (id: number, readOnly: boolean) => ({ ...plan, id, read_only: readOnly }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document_id: id, document: noteDocument('Original') }),
      readNotePage: async () => ({ url: 'https://example.org/', title: 'Title', markdown: 'Imported' }),
      saveNote, setNoteReadOnly,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'https://example.org/' } });
    fireEvent.keyDown(prompt, { key: 'Enter' });
    await waitFor(() => expect(noteEditor().getText()).toContain('Imported'));
    await waitFor(() => expect(prompt).toHaveValue(''));
    expect(setNoteReadOnly).not.toHaveBeenCalled();
    expect(noteEditor().isEditable).toBe(true);
  });

  it('saves an edited subfolder note and keeps the editor blocked until its job ends', async () => {
    const nested = { ...study, id: 2, parent_id: study.id, name: 'Nested' };
    const child = { ...plan, parent_id: nested.id };
    let finishSave!: () => void;
    const saveNote = vi.fn((_id: number, _document: Record<string, unknown>) => new Promise<void>(resolve => { finishSave = resolve; }));
    let finishAcceptance!: (result: { accepted: true }) => void;
    const compressNote = vi.fn(() => new Promise<{ accepted: true }>(resolve => { finishAcceptance = resolve; }));
    let pending = true;
    const compressionStatus = vi.fn(async (id: number) => ({ pending: id === plan.id && pending }));
    render(<App client={fixtureClient({ listNoteItems: async () => [study, nested, child],
      getNote: async id => ({ id, document_id: id, document: noteDocument('Original') }),
      saveNote, compressNote, compressionStatus })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Nested' }));
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    const editor = noteEditor();
    act(() => typeInNote(' edited'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    expect(compressNote).not.toHaveBeenCalled();
    expect(noteEditor().isEditable).toBe(false);
    const dialog = screen.getByRole('dialog', { name: 'Note compression' });
    expect(within(dialog).getByRole('status')).toHaveTextContent('Saving note…');
    expect(within(dialog).getByRole('button', { name: 'Cancel' })).toHaveFocus();
    await act(async () => finishSave());
    await waitFor(() => expect(compressNote).toHaveBeenCalledExactlyOnceWith(plan.id, 1, true));
    expect(noteEditor().isEditable).toBe(false);
    await act(async () => finishAcceptance({ accepted: true }));
    expect(within(dialog).getByRole('status')).toHaveTextContent('Note 1 of 1');
    expect(noteEditor().isEditable).toBe(false);
    pending = false;
    act(() => compressionChanged(plan.id));
    await waitFor(() => expect(dialog).not.toBeInTheDocument());
    expect(noteEditor()).toBe(editor);
    expect(editor.isEditable).toBe(true);
    expect(saveNote.mock.calls[0][1]).toEqual(noteEditor().getJSON());
  });

  it('does not compress after a failed save and saves again on the next attempt', async () => {
    const compressNote = vi.fn(async () => ({ accepted: true as const }));
    const saveNote = vi.fn()
      .mockRejectedValueOnce(new ChaError('internal_error', 'The note could not be saved.'))
      .mockResolvedValue(undefined);
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document_id: id, document: noteDocument('Original') }),
      saveNote,
      compressNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => typeInNote(' edited'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await screen.findByText('Could not save “Plan”. Check the vault and try again.');
    expect(compressNote).not.toHaveBeenCalled();
    expect(noteEditor().isEditable).toBe(true);
    expect(noteEditor().getText()).toContain('edited');
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await waitFor(() => expect(compressNote).toHaveBeenCalledOnce());
    expect(saveNote).toHaveBeenCalledTimes(2);
  });

  it('blocks navigation and waits for the compressed reload even if the first job status read fails', async () => {
    const reopened = deferred<NoteContent>();
    let started = false;
    let pending = true;
    const getNote = vi.fn(async (id: number, mode?: NoteTextMode) => (
      started && mode === 'compressed' ? reopened.promise : { id, document_id: id, document: noteDocument('Original') }
    ));
    const compressionStatus = vi.fn()
      .mockResolvedValueOnce({ pending: false })
      .mockRejectedValueOnce(new Error('Temporary status error'))
      .mockImplementation(async () => ({ pending }));
    const bus = contextBus();
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan], getNote, compressionStatus,
      compressNote: async () => { started = true; return { accepted: true }; },
    })} contextEvents={bus} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Original');
    chooseNoteMode('Compressed');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Note text mode' }))
      .toHaveAttribute('title', 'Compressed mode; showing full text'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    const dialog = await screen.findByRole('dialog', { name: 'Note compression' });
    await waitFor(() => expect(compressionStatus).toHaveBeenCalledTimes(2));
    expect(noteEditor().isEditable).toBe(false);
    expect(screen.getByRole('button', { name: 'Actions for Plan' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    act(() => bus.layout('split'));
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'notes');
    expect(screen.queryByRole('separator', { name: 'Resize notes and chat' })).not.toBeInTheDocument();
    pending = false;
    act(() => compressionChanged(plan.id));
    await within(dialog).findByText('Opening note…');
    expect(within(dialog).getByRole('button', { name: 'Cancel' })).toBeDisabled();
    expect(screen.queryByText('Original')).not.toBeInTheDocument();
    await act(async () => reopened.resolve({ id: plan.id, document_id: 40, document: noteDocument('New compression') }));
    expect(dialog).not.toBeInTheDocument();
    expect(noteEditor().getText()).toBe('New compression');
    expect(noteEditor().isEditable).toBe(true);
  });

  it('checks an early compression notice even when the first post-acceptance status read fails', async () => {
    const compressionStatus = vi.fn()
      .mockResolvedValueOnce({ pending: false })
      .mockRejectedValueOnce(new Error('Temporary status error'))
      .mockResolvedValueOnce({ pending: false });
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async id => ({ id, document_id: id, document: noteDocument('Original') }),
      compressionStatus,
      compressNote: async () => {
        compressionChanged(plan.id);
        return { accepted: true };
      },
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Original');
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await waitFor(() => expect(compressionStatus).toHaveBeenCalledTimes(3));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(noteEditor().isEditable).toBe(true);
  });

  it('reloads a selected fallback after folder compression skips a newly compressed note', async () => {
    let hasCompressed = false;
    const compressNote = vi.fn(async () => {
      hasCompressed = true;
      return { skipped: true as const };
    });
    render(<App client={fixtureClient({
      listNoteItems: async () => [study, { ...plan, parent_id: study.id, has_compressed: hasCompressed }],
      getNote: async (id, mode) => (
        hasCompressed && mode === 'compressed'
          ? { id, document_id: 40, document: noteDocument('Existing compression') }
          : { id, document_id: id, document: noteDocument('Original') }
      ),
      compressNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Original');
    chooseNoteMode('Compressed');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Note text mode' }))
      .toHaveAttribute('title', 'Compressed mode; showing full text'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await screen.findByText('Existing compression');
    expect(compressNote).toHaveBeenCalledExactlyOnceWith(plan.id, 1, true);
    expect(screen.getByRole('button', { name: 'Note text mode' }))
      .toHaveAttribute('title', 'Compressed mode; showing compressed');
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
  });

  it('lets the initial save settle before cancelling without starting a job', async () => {
    const saving = deferred<void>();
    const saveNote = vi.fn(() => saving.promise);
    const compressNote = vi.fn();
    const cancelCompression = vi.fn();
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async id => ({ id, document_id: id, document: noteDocument('Original') }),
      saveNote, compressNote, cancelCompression,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Original');
    act(() => typeInNote(' edited'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    const dialog = await screen.findByRole('dialog', { name: 'Note compression' });
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    fireEvent(dialog, new Event('cancel', { cancelable: true }));
    expect(dialog).toHaveTextContent('Cancelling…');
    expect(within(dialog).getByRole('button', { name: 'Cancel' })).toBeDisabled();
    await act(async () => saving.resolve());
    expect(dialog).not.toBeInTheDocument();
    expect(compressNote).not.toHaveBeenCalled();
    expect(cancelCompression).not.toHaveBeenCalled();
    expect(noteEditor().getText()).toBe('Original edited');
    expect(noteEditor().isEditable).toBe(true);
  });

  it('keeps the job blocking after Cancel fails and allows another attempt', async () => {
    const cancel = deferred<void>();
    const cancelCompression = vi.fn().mockRejectedValueOnce(new Error('Temporary failure'))
      .mockReturnValueOnce(cancel.promise);
    const compressNote = vi.fn(async () => ({ accepted: true as const }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async id => ({ id, document_id: id, document: noteDocument('Original') }),
      compressNote, cancelCompression,
      compressionStatus: async () => ({ pending: compressNote.mock.calls.length > 0 }),
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Original');
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    const dialog = await screen.findByRole('dialog', { name: 'Note compression' });
    await within(dialog).findByText('Compressing “Plan”…');
    const button = within(dialog).getByRole('button', { name: 'Cancel' });
    fireEvent.click(button);
    await waitFor(() => expect(cancelCompression).toHaveBeenCalledOnce());
    await waitFor(() => expect(button).toBeEnabled());
    expect(noteEditor().isEditable).toBe(false);
    fireEvent.click(button);
    expect(button).toBeDisabled();
    expect(dialog).toBeInTheDocument();
    await act(async () => cancel.resolve());
    await waitFor(() => expect(dialog).not.toBeInTheDocument());
    expect(noteEditor().isEditable).toBe(true);
    expect(cancelCompression.mock.calls).toEqual([[plan.id, 1], [plan.id, 1]]);
  });

  it('applies an incoming sync change before releasing a full-text editor after compression', async () => {
    let body = 'Original';
    let pending = false;
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument(body) }));
    const bus = contextBus();
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote,
      compressNote: async () => { pending = true; return { accepted: true }; },
      compressionStatus: async () => ({ pending }),
    })} contextEvents={bus} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Original');
    const old = noteEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    const dialog = await screen.findByRole('dialog', { name: 'Note compression' });
    await within(dialog).findByText('Compressing “Plan”…');
    body = 'Incoming full text';
    act(() => bus.notesChanged());
    expect(noteEditor()).toBe(old);
    expect(old.isEditable).toBe(false);
    pending = false;
    act(() => compressionChanged(plan.id));
    await waitFor(() => expect(dialog).not.toBeInTheDocument());
    expect(noteEditor().getText()).toBe(body);
    expect(noteEditor().isEditable).toBe(true);
  });

  it('defers a window close until the compression operation releases the note', async () => {
    const post = vi.fn();
    window.__CHA_NATIVE_CONNECTION_ID__ = 'view-1';
    window.__CHA_NATIVE_POST__ = post;
    try {
      let accept!: (result: { accepted: true }) => void;
      const compressNote = vi.fn(() => new Promise<{ accepted: true }>(resolve => { accept = resolve; }));
      let pending = true;
      render(<App client={fixtureClient({
        listNoteItems: async () => [plan],
        getNote: async (id) => ({ id, document_id: id, document: noteDocument('Original') }),
        compressNote,
        compressionStatus: async () => ({ pending: compressNote.mock.calls.length > 0 && pending }),
      })} />);
      await openNotes();
      fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
      await waitFor(() => expect(noteEditor().isEditable).toBe(true));
      fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
      await waitFor(() => expect(screen.getByRole('menuitem', { name: 'Compress' })).toBeEnabled());
      fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
      await waitFor(() => expect(compressNote).toHaveBeenCalledOnce());
      act(() => window.__CHA_NATIVE_PREPARE_CLOSE__?.(7));
      expect(post.mock.calls.map(([message]) => JSON.parse(message as string)))
        .not.toContainEqual(expect.objectContaining({ native_action: 'close_reply', attempt_id: 7 }));
      await act(async () => accept({ accepted: true }));
      expect(post.mock.calls.map(([message]) => JSON.parse(message as string)))
        .not.toContainEqual(expect.objectContaining({ native_action: 'close_reply', attempt_id: 7 }));
      pending = false;
      act(() => compressionChanged(plan.id));
      await waitFor(() => expect(post.mock.calls.map(([message]) => JSON.parse(message as string)))
        .toContainEqual({ native_action: 'close_reply', connection_id: 'view-1', attempt_id: 7, result: 'ready' }));
    } finally {
      delete window.__CHA_NATIVE_POST__;
      delete window.__CHA_NATIVE_CONNECTION_ID__;
    }
  });

  it.each([0, 1, 3])('uses an available page title in the tree and Send label with %s existing names', async (count) => {
    const original = { ...plan, name: 'New note 1' };
    const siblings = Array.from({ length: count }, (_, index) => ({
      ...plan, id: 20 + index, name: index === 0 ? 'Article title' : `Article title ${index}`,
    }));
    const expected = count === 0 ? 'Article title' : `Article title ${count}`;
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: {
      type: 'doc', content: [{ type: 'paragraph' }],
    } }));
    const renameNoteItem = vi.fn(async (id: number, name: string) => ({ ...original, id, name }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [original, ...siblings],
      getNote,
      saveNote: async () => undefined,
      readNotePage: async () => ({ url: 'https://example.org/', title: 'Article title', markdown: 'Article text' }),
      renameNoteItem,
      setNoteReadOnly: async (id, readOnly) => ({ ...original, id, name: expected, read_only: readOnly }),
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'New note 1' }));
    const prompt = await screen.findByRole('textbox', { name: 'Prompt' });
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => prompt.focus());
    fireEvent.change(prompt, { target: { value: 'https://example.org' } });
    fireEvent.keyDown(prompt, { key: 'Enter' });
    expect(await screen.findByRole('treeitem', { name: expected })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: `Send to ${expected}` })).toBeInTheDocument();
    await waitFor(() => expect(document.title).toBe(`CHA: Personal — ${expected}`));
    await waitFor(() => expect(prompt).toHaveValue(''));
    expect(prompt).toBeEnabled();
    expect(noteEditor().isEditable).toBe(false);
    expect(renameNoteItem).toHaveBeenCalledWith(plan.id, expected);
    expect(noteEditor().getText({ blockSeparator: '\n' })).toBe('Article title\nArticle text');
    expect(getNote).toHaveBeenCalledOnce();
  });

  it('switches sidebar tabs without changing the main panel, chat draft, or address', async () => {
    render(<App client={notesClient()} />);
    const message = await screen.findByRole('textbox', { name: 'Message' });
    fireEvent.change(message, { target: { value: 'Chat draft' } });
    const address = window.location.href;
    await openNotes();
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Chat draft');
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'chat');
    expect(document.title).toBe('CHA: Personal — Welcome');
    expect(window.location.href).toBe(address);
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Chat draft');
    expect(screen.getByLabelText('Forums')).toBeInTheDocument();
    expect(window.location.href).toBe(address);
  });

  it('reopens the selected note and keeps the chat draft', async () => {
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument('Saved plan') }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote,
      saveNote: async () => undefined,
    })} />);
    fireEvent.change(await screen.findByRole('textbox', { name: 'Message' }), {
      target: { value: 'Chat draft' },
    });
    const address = window.location.href;
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Saved plan')).toBeInTheDocument();
    await waitFor(() => expect(noteEditor().view.dom).toHaveFocus());
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'notes');
    expect(document.title).toBe('CHA: Personal — Plan');
    expect(window.location.href).toBe(address);
    expect(screen.queryByRole('heading', { name: 'Notes' })).not.toBeInTheDocument();
    act(() => screen.getByRole('treeitem', { name: 'Plan' }).focus());
    fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(getNote).toHaveBeenCalledTimes(2));
    await waitFor(() => expect(noteEditor().view.dom).toHaveFocus());
    expect(screen.getByText('Saved plan')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    expect(document.title).toBe('CHA: Personal — Plan');
    fireEvent.click(screen.getByRole('button', { name: /^Welcome/ }));
    expect(await screen.findByRole('textbox', { name: 'Message' })).toHaveValue('Chat draft');
    await waitFor(() => expect(document.title).toBe('CHA: Personal — Welcome'));
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    expect(document.title).toBe('CHA: Personal');
  });

  it.each(['immediate', 'delayed'])('focuses the new note editor with %s loading', async (loading) => {
    const created: NoteItem = noteItem({ id: 9, name: 'New note' });
    let items = [plan];
    let finishOpen!: () => void;
    const newDocument = { type: 'doc', content: [{ type: 'paragraph' }] };
    const pendingOpen = new Promise<{ id: number; document_id: number; document: typeof newDocument }>((resolve) => {
      finishOpen = () => resolve({ id: created.id, document_id: created.id, document: newDocument });
    });
    const createNoteItem = vi.fn(async () => {
      items = [...items, created];
      return created;
    });
    render(<App client={fixtureClient({
      listNoteItems: async () => items,
      createNoteItem,
      getNote: async (id) => {
        if (id !== created.id) return { id, document_id: id, document: noteDocument('Saved plan') };
        return loading === 'delayed' ? pendingOpen : { id, document_id: id, document: newDocument };
      },
      saveNote: async () => undefined,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    fireEvent.click(screen.getByRole('button', { name: `Actions for ${bootstrapFixture.vault_name}` }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'New note' }));
    fireEvent.click(within(await screen.findByRole('dialog', { name: 'New note' })).getByRole('button', { name: 'Create' }));
    if (loading === 'delayed') {
      await screen.findByText('Opening…');
      expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
      await act(async () => finishOpen());
    }
    await waitFor(() => expect(noteEditor().view.dom).toHaveFocus());
    expect(noteEditor().isEditable).toBe(true);
    act(() => typeInNote('Start typing'));
    expect(noteEditor().getText()).toBe('Start typing');
    expect(noteEditor().view.dom).toHaveFocus();
  });

  it('keeps the Notes draft and Send available across sidebar tabs', async () => {
    render(<App client={notesClient()} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Saved plan')).toBeInTheDocument();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'Remember this' } });
    expect(screen.getByRole('button', { name: 'Send to Plan' })).toBeEnabled();
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(screen.getByRole('tab', { name: 'Notes' }));
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Remember this');
    expect(screen.getByRole('button', { name: 'Send to Plan' })).toBeEnabled();
  });

  it('saves before another note and before a Chat session', async () => {
    const essay: NoteItem = noteItem({ id: 5, name: 'Essay' });
    const saveNote = vi.fn(async (_id: number, _document: Record<string, unknown>) => undefined);
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan, essay],
      getNote: async (id: number) => ({
        id, document_id: id,
        document: noteDocument(id === 5 ? 'Essay text' : 'Saved plan'),
      }),
      saveNote,
    })} />);
    fireEvent.change(await screen.findByRole('textbox', { name: 'Message' }), {
      target: { value: 'Chat draft' },
    });
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    await waitFor(() => noteEditor());
    typeInNote(' more');
    fireEvent.click(screen.getByRole('treeitem', { name: 'Essay' }));
    expect(await screen.findByText('Essay text')).toBeInTheDocument();
    await waitFor(() => expect(noteEditor().view.dom).toHaveFocus());
    expect(saveNote).toHaveBeenCalledOnce();
    expect(saveNote.mock.calls[0][0]).toBe(4);
    expect(JSON.stringify(saveNote.mock.calls[0][1])).toContain('Saved plan more');
    typeInNote(' too');
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
    expect(await screen.findByRole('textbox', { name: 'Message' })).toHaveValue('');
    expect(saveNote).toHaveBeenCalledTimes(2);
    expect(saveNote.mock.calls[1][0]).toBe(5);
    expect(JSON.stringify(saveNote.mock.calls[1][1])).toContain('Essay text too');
  });

  it('saves before Settings and returns to the same conversation', async () => {
    const saveNote = vi.fn(async (_id: number, _document: Record<string, unknown>) => undefined);
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id: number) => ({ id, document_id: id, document: noteDocument('Saved plan') }),
      saveNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    await waitFor(() => noteEditor());
    typeInNote(' more');
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    await screen.findByRole('heading', { name: 'Settings' });
    expect(saveNote).toHaveBeenCalledOnce();
    expect(JSON.stringify(saveNote.mock.calls[0][1])).toContain('Saved plan more');
  });

  it.each(['Upload', 'Copy', 'Discard then Copy'])('%s uses the Settings leave decision once', async (operation) => {
    let stored = noteDocument('Saved plan');
    let transferred: unknown;
    let finishSave!: () => void;
    const saveNote = vi.fn((_id: number, document: Record<string, unknown>) => new Promise<void>((resolve, reject) => {
      if (operation === 'Discard then Copy') {
        reject(new ChaError('internal_error', 'Save failed.'));
      } else {
        finishSave = () => { stored = document as typeof stored; resolve(); };
      }
    }));
    const vault = { display_name: 'Personal', protected: false, active: true, can_delete: false,
      data_path: '/data/personal.sqlite3', mirror_path: null, modify_path: null };
    const uploadVault = vi.fn(async () => { transferred = stored; return 42; });
    const createVault = vi.fn(async () => { transferred = stored; return { ...vault, display_name: 'Copy', active: false }; });
    render(<App client={fixtureClient({
      getBootstrap: async () => ({ ...bootstrapFixture, capabilities: { can_transfer_r2: true, can_modify: false } }),
      listNoteItems: async () => [plan], getNote: async (id) => ({ id, document_id: id, document: stored }), saveNote,
      listVaults: async () => [vault], uploadVault, createVault,
      checkVaultUpload: async () => ({ status: 'match', etag: 'known', context_epoch: 1 }),
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    await waitFor(() => noteEditor());
    act(() => typeInNote(' final edit'));
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'notes');
    expect(uploadVault).not.toHaveBeenCalled();
    expect(createVault).not.toHaveBeenCalled();
    if (operation === 'Discard then Copy') {
      fireEvent.click(within(await screen.findByRole('dialog')).getByRole('button', { name: 'Discard' }));
    } else {
      await act(async () => finishSave());
    }
    fireEvent.click(await screen.findByRole('button', { name: 'Vaults' }));
    if (operation === 'Upload') {
      fireEvent.click(await within(await screen.findByRole('region', { name: 'Vaults settings' })).findByRole('button', { name: /Personal/ }));
      fireEvent.click(await screen.findByRole('button', { name: 'Upload' }));
      await waitFor(() => expect(uploadVault).toHaveBeenCalledOnce());
    } else {
      fireEvent.click(await screen.findByRole('button', { name: /New vault/ }));
      fireEvent.change(await screen.findByRole('textbox', { name: 'Display name' }), { target: { value: 'Copy' } });
      fireEvent.change(screen.getByRole('combobox', { name: 'Initial database' }), { target: { value: 'Personal' } });
      fireEvent.click(screen.getByRole('button', { name: 'Create vault' }));
      await waitFor(() => expect(createVault).toHaveBeenCalledOnce());
      expect(createVault).toHaveBeenCalledWith({ display_name: 'Copy', copy_from: 'Personal', password: null });
    }
    expect(JSON.stringify(transferred)).toContain(operation === 'Discard then Copy' ? 'Saved plan' : 'Saved plan final edit');
    if (operation === 'Discard then Copy') expect(JSON.stringify(transferred)).not.toContain('final edit');
    expect(saveNote).toHaveBeenCalledOnce();
  });

  it('reports an upload that fails after the vault page closed', async () => {
    let reject!: (failure: Error) => void;
    const vault = { display_name: 'Personal', protected: false, active: true, can_delete: false,
      data_path: '/data/personal.sqlite3', mirror_path: null, modify_path: null };
    render(<App client={fixtureClient({
      getBootstrap: async () => ({ ...bootstrapFixture, capabilities: { can_transfer_r2: true, can_modify: false } }),
      listNoteItems: async () => [plan], getNote: async (id) => ({ id, document_id: id, document: noteDocument('Saved plan') }),
      listVaults: async () => [vault],
      checkVaultUpload: async () => ({ status: 'match', etag: 'known', context_epoch: 1 }),
      uploadVault: () => new Promise<number>((_resolve, fail) => { reject = fail; }),
    })} />);
    fireEvent.click(await screen.findByRole('button', { name: 'Settings' }));
    fireEvent.click(await screen.findByRole('button', { name: 'Vaults' }));
    fireEvent.click(await within(await screen.findByRole('region', { name: 'Vaults settings' })).findByRole('button', { name: /Personal/ }));
    fireEvent.click(await screen.findByRole('button', { name: 'Upload' }));
    await screen.findByRole('button', { name: 'Uploading…' });
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    expect(screen.queryByRole('button', { name: 'Uploading…' })).toBeNull();
    await act(async () => reject(new ChaError('invalid_argument', 'R2 upload failed with HTTP status 503')));
    expect(await screen.findByText('Upload failed. R2 upload failed with HTTP status 503')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Dismiss message error' }));
    expect(screen.queryByText(/Upload failed/)).toBeNull();
  });

  it.each(['success', 'failure', 'cancel'] as const)('guards vault switching and draft reset on %s', async (outcome) => {
    const events = contextBus();
    let activeVault = 'Personal';
    let finishSave!: () => void;
    const saveNote = vi.fn(() => new Promise<void>((resolve, reject) => {
      finishSave = () => outcome === 'cancel'
        ? reject(new ChaError('internal_error', 'Save failed.')) : resolve();
    }));
    const discardUnusedSession = vi.fn(async () => undefined);
    const switchVault = vi.fn(async () => {
      if (outcome === 'failure') throw new ChaError('internal_error', 'Switch failed.');
      activeVault = 'Projects';
      events.emit();
    });
    render(<App contextEvents={events} client={fixtureClient({
      getBootstrap: async () => ({ ...bootstrapFixture, vault_name: activeVault }),
      getSessionSnapshot: async () => ({ ...snapshotFixture, recent_pending: true, discardable: true }),
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document_id: id, document: noteDocument(activeVault) }),
      saveNote, discardUnusedSession, switchVault,
    })} />);
    fireEvent.change(await screen.findByRole('textbox', { name: 'Message' }), { target: { value: 'Chat draft' } });
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(document.querySelector('.ProseMirror')).toHaveTextContent('Personal'));
    const originalEditor = await waitFor(() => noteEditor());
    act(() => typeInNote(' final edit'));
    fireEvent.change(screen.getByRole('textbox', { name: 'Prompt' }), { target: { value: 'Notes draft' } });
    discardUnusedSession.mockClear();
    fireEvent.change(screen.getByRole('combobox', { name: 'Vault' }), { target: { value: 'Projects' } });
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    expect(discardUnusedSession).not.toHaveBeenCalled();
    expect(switchVault).not.toHaveBeenCalled();
    await act(async () => finishSave());
    if (outcome === 'cancel') {
      fireEvent.click(within(await screen.findByRole('dialog')).getByRole('button', { name: 'Stay' }));
      await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
      expect(switchVault).not.toHaveBeenCalled();
      expect(discardUnusedSession).not.toHaveBeenCalled();
    } else if (outcome === 'failure') {
      await screen.findByText('Switch failed.');
      expect(switchVault).toHaveBeenCalledOnce();
    } else {
      await waitFor(() => expect(screen.getByRole('combobox', { name: 'Vault' })).toHaveValue('Projects'));
      expect(originalEditor.isDestroyed).toBe(true);
      fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
      await waitFor(() => expect(document.querySelector('.ProseMirror')).toHaveTextContent('Projects'));
      expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('');
      expect(saveNote).toHaveBeenCalledOnce();
      fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
      fireEvent.click(screen.getByRole('button', { name: /^Welcome/ }));
      expect(await screen.findByRole('textbox', { name: 'Message' })).toHaveValue('');
      return;
    }
    expect(noteEditor()).toBe(originalEditor);
    expect(originalEditor.isEditable).toBe(true);
    expect(originalEditor.getText()).toBe('Personal final edit');
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Notes draft');
  });

  it('keeps the open note when the active conversation is deleted from the sidebar', async () => {
    let deleted = false;
    const deleteSession = vi.fn(async () => { deleted = true; });
    const saveNote = vi.fn(async () => undefined);
    render(<App client={fixtureClient({
      getBootstrap: async () => (deleted
        ? bootstrapFixture
        : { ...bootstrapFixture, initial_forum_id: 'lobby', initial_session_id: 'planning' }),
      getSessionSnapshot: async (forumId) => (forumId === 'lobby'
        ? { ...snapshotFixture, forum: bootstrapFixture.forums[1], session_id: 'planning', session_label: 'Planning' }
        : snapshotFixture),
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document_id: id, document: noteDocument('Saved plan') }),
      saveNote,
      deleteSession,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    const editor = await waitFor(() => noteEditor());
    act(() => typeInNote(' edit'));
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(screen.getByLabelText('Actions for Planning'));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Delete…' }));
    fireEvent.click(within(await screen.findByRole('dialog')).getByRole('button', { name: 'Delete' }));
    await waitFor(() => expect(screen.getByRole('button', { name: /^Welcome/ }))
      .toHaveAttribute('aria-current', 'page'));
    expect(deleteSession).toHaveBeenCalledWith('lobby', 'planning');
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'notes');
    expect(window.location.hash).toBe('#/');
    expect(noteEditor()).toBe(editor);
    expect(editor.isEditable).toBe(true);
    expect(editor.getText()).toBe('Saved plan edit');
    expect(saveNote).not.toHaveBeenCalled();
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it('retains the editor and history after a failed session open, then opens it on Retry', async () => {
    let rejectOpen!: (error: unknown) => void;
    const pendingOpen = new Promise<void>((_resolve, reject) => { rejectOpen = reject; });
    let failOpen = true;
    const saveNote = vi.fn(async () => undefined);
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument('Saved plan') }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote,
      saveNote,
      openSession: async (forumId, sessionId) => {
        if (sessionId === 'planning' && failOpen) await pendingOpen;
        return { forum_id: forumId, session_id: sessionId };
      },
      getSessionSnapshot: async (forumId, sessionId) => ({
        ...snapshotFixture,
        forum: bootstrapFixture.forums.find(({ id }) => id === forumId)!,
        session_id: sessionId,
      }),
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    const editor = await waitFor(() => noteEditor());
    act(() => typeInNote(' more'));
    fireEvent.change(screen.getByRole('textbox', { name: 'Prompt' }), {
      target: { value: 'Keep draft' },
    });
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(screen.getByRole('button', { name: /^Planning/ }));
    await screen.findByText('Opening session…');
    expect(noteEditor()).toBe(editor);
    expect(editor.isEditable).toBe(false);
    expect(saveNote).toHaveBeenCalledOnce();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toBeEnabled();
    expect(screen.getByRole('button', { name: 'Send to Plan' })).toBeDisabled();

    await act(async () => rejectOpen(new ChaError('session_open_timeout', 'Session opening timed out.')));
    const error = await screen.findByRole('alert');
    expect(error).toHaveTextContent('Session opening timed out.');
    expect(noteEditor()).toBe(editor);
    expect(editor.isEditable).toBe(true);
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'notes');
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep draft');
    fireEvent.click(historyAction('Undo'));
    expect(editor.getText()).toBe('Saved plan');
    fireEvent.click(historyAction('Redo'));
    expect(editor.getText()).toBe('Saved plan more');

    failOpen = false;
    fireEvent.click(within(error).getByRole('button', { name: 'Retry' }));
    await screen.findByRole('textbox', { name: 'Message' });
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'chat');
    expect(editor.isDestroyed).toBe(true);
    expect(getNote).toHaveBeenCalledOnce();
    expect(saveNote).toHaveBeenCalledTimes(2);
    expect(screen.queryByText('Session opening timed out.')).not.toBeInTheDocument();
  });

  it.each([false, true])('keeps Notes after session creation fails (discard: %s)', async (discard) => {
    const createSession = vi.fn(async () => {
      throw new ChaError('invalid_argument', 'The session could not be created.');
    });
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument('Saved plan') }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote,
      saveNote: async () => {
        if (discard) throw new ChaError('invalid_argument', 'The note could not be saved.');
      },
      createSession,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    const editor = await waitFor(() => noteEditor());
    act(() => typeInNote(' more'));
    fireEvent.change(screen.getByRole('textbox', { name: 'Prompt' }), {
      target: { value: 'Keep draft' },
    });
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for forum The Lobby' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Create new session' }));
    if (discard) {
      const dialog = await screen.findByRole('dialog', { name: 'Could not save “Plan”.' });
      expect(createSession).not.toHaveBeenCalled();
      fireEvent.click(within(dialog).getByRole('button', { name: 'Discard' }));
    }
    await screen.findByText('The session could not be created.');
    const error = screen.getByRole('alert');
    expect(within(error).queryByRole('button', { name: 'Retry' })).not.toBeInTheDocument();
    expect(createSession).toHaveBeenCalledWith('lobby', '');
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'notes');
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep draft');
    expect(getNote).toHaveBeenCalledOnce();
    if (discard) {
      expect(editor.isDestroyed).toBe(true);
      expect(document.querySelector('.ProseMirror')).toBeNull();
      expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
    } else {
      expect(noteEditor()).toBe(editor);
      expect(editor.isEditable).toBe(true);
      fireEvent.click(historyAction('Undo'));
      expect(editor.getText()).toBe('Saved plan');
    }
  });

  it('replaces an open note when the vault context changes, even for the same id', async () => {
    const events = contextBus();
    const getNote = vi.fn()
      .mockResolvedValueOnce({ id: 4, document_id: 4, document: noteDocument('Alpha') })
      .mockResolvedValueOnce({ id: 4, document_id: 4, document: noteDocument('Gamma') });
    render(<App
      client={fixtureClient({
        listNoteItems: async () => [plan],
        getNote,
        saveNote: async () => undefined,
      })}
      contextEvents={events}
    />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Alpha')).toBeInTheDocument();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'Keep the draft' } });
    events.emit();
    await waitFor(() => expect(screen.queryByText('Alpha')).not.toBeInTheDocument());
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep the draft');
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Gamma')).toBeInTheDocument();
    expect(screen.queryByText('Alpha')).not.toBeInTheDocument();
    expect(getNote).toHaveBeenCalledTimes(2);
  });

  it('shows Stay with focus and discards unsaved changes on request', async () => {
    const saveNote = vi.fn(async () => {
      throw new ChaError('invalid_argument', 'The note could not be saved.');
    });
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id: number) => ({ id, document_id: id, document: noteDocument('Alpha') }),
      saveNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Alpha');
    await waitFor(() => noteEditor());
    typeInNote(' changed');
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    const dialog = await screen.findByRole('dialog', { name: 'Could not save “Plan”.' });
    expect(screen.getByRole('button', { name: 'Stay' })).toHaveFocus();
    fireEvent(dialog, new Event('cancel', { bubbles: true, cancelable: true }));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'notes');
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    await screen.findByRole('dialog');
    fireEvent.click(screen.getByRole('button', { name: 'Discard' }));
    expect(await screen.findByRole('heading', { name: 'Settings' })).toBeInTheDocument();
  });

  function noteRegion() {
    return screen.getByRole('region', { name: 'Note' });
  }

  async function deleteItem(name: string, kind: 'note' | 'folder', confirm = true) {
    fireEvent.click(screen.getByRole('button', { name: `Actions for ${name}` }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Delete' }));
    if (!confirm) return;
    const label = kind === 'folder'
      ? `Delete “${name}” and all notes and folders inside it? This cannot be undone in CHA.`
      : `Delete “${name}”? This cannot be undone in CHA.`;
    const dialog = await screen.findByRole('dialog', { name: label });
    fireEvent.click(within(dialog).getByRole('button', {
      name: kind === 'folder' ? 'Delete folder' : 'Delete note',
    }));
    return dialog;
  }

  it.each([
    { text: '', confirm: false },
    { text: 'x'.repeat(15), confirm: false },
    { text: 'x'.repeat(16), confirm: true },
    { text: '😀'.repeat(15), confirm: false },
  ])('checks the body of an unopened note before deletion ($text)', async ({ text, confirm }) => {
    let removed = false;
    const deleteNoteItem = vi.fn(async () => { removed = true; });
    const getNote = vi.fn(async (id: number) => ({
      id, document_id: id,
      document: text ? noteDocument(text) : { type: 'doc', content: [{ type: 'paragraph' }] },
    }));
    render(<App client={fixtureClient({
      listNoteItems: async () => removed ? [] : [plan],
      getNote,
      deleteNoteItem,
    })} />);
    await openNotes();
    await deleteItem('Plan', 'note', confirm);
    await waitFor(() => expect(deleteNoteItem).toHaveBeenCalledExactlyOnceWith(4));
    expect(getNote).toHaveBeenCalledExactlyOnceWith(4);
    await waitFor(() => expect(screen.queryByRole('treeitem', { name: 'Plan' })).not.toBeInTheDocument());
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
  });

  it.each([
    { saved: 'A long saved note body', edited: 'Short', confirm: false },
    { saved: 'Short', edited: 'A long unsaved note body', confirm: true },
  ])('uses unsaved text for deletion confirmation ($edited)', async ({ saved, edited, confirm }) => {
    let removed = false;
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument(saved) }));
    const deleteNoteItem = vi.fn(async () => { removed = true; });
    render(<App client={fixtureClient({
      listNoteItems: async () => removed ? [] : [plan],
      getNote,
      deleteNoteItem,
      saveNote: async () => undefined,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText(saved);
    await waitFor(() => noteEditor());
    act(() => { noteEditor().commands.setContent(noteDocument(edited)); });
    await deleteItem('Plan', 'note', confirm);
    await waitFor(() => expect(deleteNoteItem).toHaveBeenCalledExactlyOnceWith(4));
    expect(getNote).toHaveBeenCalledOnce();
  });

  it('requires delete confirmation based on the original even with a short compressed document open', async () => {
    const deleteNoteItem = vi.fn();
    render(<App client={fixtureClient({
      listNoteItems: async () => [{ ...plan, has_compressed: true }],
      getNote: async (id, mode) => ({
        id, document_id: mode === 'compressed' ? 40 : id,
        document: noteDocument(mode === 'compressed' ? 'Short' : 'The original is longer than sixteen characters.'),
      }),
      deleteNoteItem,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Show compressed' }));
    await screen.findByText('Short');
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
    expect(await screen.findByRole('dialog', { name: /^Delete “Plan”\?/ })).toBeInTheDocument();
    expect(deleteNoteItem).not.toHaveBeenCalled();
  });

  it('uses live edits if a note opens while its deletion length is being read', async () => {
    let finishRead!: (note: { id: number; document_id: number; document: Record<string, unknown> }) => void;
    const getNote = vi.fn((id: number) => getNote.mock.calls.length === 1
      ? new Promise<{ id: number; document_id: number; document: Record<string, unknown> }>((resolve) => { finishRead = resolve; })
      : Promise.resolve({ id, document_id: id, document: noteDocument('Short') }));
    const deleteNoteItem = vi.fn();
    render(<App client={fixtureClient({ listNoteItems: async () => [plan], getNote, deleteNoteItem })} />);
    await openNotes();
    await deleteItem('Plan', 'note', false);
    await waitFor(() => expect(getNote).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Short');
    await waitFor(() => noteEditor());
    act(() => { noteEditor().commands.setContent(noteDocument('A long unsaved note body')); });
    await act(async () => { finishRead({ id: 4, document_id: 4, document: noteDocument('Short') }); });
    expect(await screen.findByRole('dialog')).toHaveTextContent('Delete “Plan”?');
    expect(deleteNoteItem).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
  });

  it('renames a note without reloading its body', async () => {
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument('Saved plan') }));
    let renamed = false;
    render(<App client={fixtureClient({
      listNoteItems: async () => {
        const note = {
          ...plan,
          name: renamed ? 'Draft' : 'Plan',
        };
        return [study, note];
      },
      getNote,
      saveNote: async () => undefined,
      renameNoteItem: async () => {
        renamed = true;
        return { ...plan, name: 'Draft' };
      },
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Saved plan')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Rename' }));
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Draft' } });
    fireEvent.click(within(screen.getByRole('dialog', { name: 'Rename “Plan”' })).getByRole('button', { name: 'Rename' }));
    expect(await screen.findByRole('button', { name: 'Send to Draft' })).toBeInTheDocument();
    expect(screen.getByText('Saved plan')).toBeInTheDocument();
    expect(getNote).toHaveBeenCalledOnce();
    expect(screen.getByRole('treeitem', { name: 'Draft' })).toHaveFocus();
  });

  it('deletes an open note without saving the remainder and keeps the draft', async () => {
    const saveNote = vi.fn(async () => undefined);
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: noteDocument('Saved plan contents') }));
    let removed = false;
    render(<App client={fixtureClient({
      listNoteItems: async () => (removed ? [study] : [study, plan]),
      getNote,
      saveNote,
      deleteNoteItem: async () => { removed = true; },
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Saved plan contents')).toBeInTheDocument();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'Remember this' } });
    typeInNote(' more');
    await deleteItem('Plan', 'note');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled());
    expect(within(noteRegion()).queryByRole('button', { name: 'New note' })).not.toBeInTheDocument();
    expect(screen.queryByText('Saved plan contents more')).not.toBeInTheDocument();
    expect(prompt).toHaveValue('Remember this');
    expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
    expect(saveNote).not.toHaveBeenCalled();
    expect(getNote).toHaveBeenCalledOnce();
    expect(screen.queryByRole('treeitem', { name: 'Plan' })).not.toBeInTheDocument();
  });

  it('restores the editor when deletion fails and sends the canceled save', async () => {
    const saveNote = vi.fn(async (_id: number, _document: Record<string, unknown>) => undefined);
    render(<App client={fixtureClient({
      listNoteItems: async () => [study, plan],
      getNote: async (id: number) => ({ id, document_id: id, document: noteDocument('Saved plan contents') }),
      saveNote,
      deleteNoteItem: async () => {
        throw new ChaError('invalid_argument', 'The note is still open in the vault.');
      },
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Saved plan contents')).toBeInTheDocument();
    await waitFor(() => noteEditor());
    typeInNote(' more');
    await deleteItem('Plan', 'note');
    expect(await screen.findByRole('alert')).toHaveTextContent('The note is still open in the vault.');
    expect(screen.queryByRole('dialog', { name: 'Could not save “Plan”.' })).not.toBeInTheDocument();
    expect(noteEditor().isEditable).toBe(true);
    expect(noteEditor().getText()).toContain('Saved plan contents more');
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    expect(JSON.stringify(saveNote.mock.calls[0][1])).toContain('Saved plan contents more');
  });

  it('keeps an unrelated note editable and blocks navigation during deletion', async () => {
    const essay: NoteItem = noteItem({ id: 5, name: 'Essay' });
    let finishDelete: () => void = () => {};
    const getNote = vi.fn(async (id: number) => ({
      id, document_id: id,
      document: noteDocument(id === 5 ? 'Essay text' : 'Saved plan'),
    }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [study, plan, essay],
      getNote,
      saveNote: async () => undefined,
      deleteNoteItem: () => new Promise<void>((resolve) => { finishDelete = resolve; }),
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Saved plan')).toBeInTheDocument();
    await deleteItem('Study', 'folder', false);
    expect(noteEditor().isEditable).toBe(true);
    typeInNote(' stays');
    fireEvent.click(screen.getByRole('treeitem', { name: 'Essay' }));
    expect(screen.queryByText('Essay text')).not.toBeInTheDocument();
    expect(getNote).toHaveBeenCalledOnce();
    finishDelete();
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(noteEditor().getText()).toContain('Saved plan stays');
    expect(noteEditor().isEditable).toBe(true);
    expect(screen.queryByText('Essay text')).not.toBeInTheDocument();
  });

  it('does not close a note opened after a deletion from the previous vault', async () => {
    const events = contextBus();
    let finishDelete: () => void = () => {};
    const getNote = vi.fn(async () => ({ id: 4, document_id: 4, document: noteDocument('Gamma note contents') }));
    render(<App
      client={fixtureClient({
        listNoteItems: async () => [plan],
        getNote,
        saveNote: async () => undefined,
        deleteNoteItem: () => new Promise<void>((resolve) => { finishDelete = resolve; }),
      })}
      contextEvents={events}
    />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Gamma note contents')).toBeInTheDocument();
    await deleteItem('Plan', 'note');
    events.emit();
    await waitFor(() => expect(screen.queryByText('Gamma note contents')).not.toBeInTheDocument());
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(getNote).toHaveBeenCalledOnce();
    finishDelete();
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });
    fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Gamma note contents')).toBeInTheDocument();
    await waitFor(() => expect(getNote).toHaveBeenCalledTimes(2));
    expect(screen.getByText('Gamma note contents')).toBeInTheDocument();
  });
});

it('opens annotation sources, protects generated content and reports stale entries', async () => {
  const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS', read_only: true };
  const source = { type: 'doc', content: [{ type: 'paragraph', content: [{ type: 'text', text: 'Target', marks: [{ type: 'highlight' }] }] }] };
  const generated = { type: 'doc', content: [{ type: 'paragraph', content: [
    { type: 'annotationSource', attrs: { sourceId: 4, entryType: 'highlight', ordinal: 1 } }, { type: 'text', text: 'Target' },
  ] }] };
  let stale = false;
  let missing = false;
  const saveNote = vi.fn();
  render(<App client={fixtureClient({
    listNoteItems: async () => [plan, annotation],
    getNote: async (id, mode) => {
      if (missing && id !== 9) throw new ChaError('not_found', 'That note was not found.');
      if (id === plan.id && mode === 'compressed') return { id, document_id: 40, document: noteDocument('Compressed body') };
      return { id, document_id: id, scroll_top: 500, document: id === 9 ? generated : stale ? noteDocument('Changed') : source };
    },
    saveNote,
  })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  await screen.findByRole('button', { name: 'Go to highlight' });
  chooseNoteMode('Compressed');
  await waitFor(() => expect(screen.getByRole('button', { name: 'Note text mode' }))
    .toHaveAttribute('title', 'Compressed mode; showing full text'));
  fireEvent.click(await screen.findByRole('button', { name: 'Go to highlight' }));
  await waitFor(() => expect(noteEditor().state.doc.textBetween(noteEditor().state.selection.from, noteEditor().state.selection.to)).toBe('Target'));
  expect(screen.getByRole('button', { name: 'Note text mode' }))
    .toHaveAttribute('title', 'Full text mode; showing full text');
  expect(saveNote).not.toHaveBeenCalled();
  expect(document.querySelector('.cha-notes-document')).not.toHaveClass('cha-notes-annotations');
  fireEvent.click(screen.getByRole('treeitem', { name: 'ANNOTATIONS' }));
  await screen.findByRole('button', { name: 'Go to highlight' });
  expect(document.querySelector('.cha-notes-document')).toHaveClass('cha-notes-annotations');
  expect(screen.getByRole('button', { name: 'Highlight' })).toBeDisabled();
  const before = noteEditor().getJSON();
  act(() => typeInNote('mutation'));
  expect(noteEditor().getJSON()).toEqual(before);
  fireEvent.click(screen.getByRole('button', { name: 'Actions for ANNOTATIONS' }));
  expect(screen.queryByRole('menuitem', { name: 'Rename' })).not.toBeInTheDocument();
  expect(screen.queryByRole('menuitem', { name: 'Delete' })).not.toBeInTheDocument();
  expect(screen.getByRole('menuitem', { name: 'Show compressed' })).toBeDisabled();
  expect(screen.getByRole('menuitem', { name: 'Copy Ref ID' })).toBeEnabled();
  fireEvent.keyDown(screen.getByRole('menuitem', { name: 'Copy Ref ID' }), { key: 'Escape' });
  stale = true;
  fireEvent.click(screen.getByRole('button', { name: 'Go to highlight' }));
  expect(await screen.findByText('This entry has changed or was removed.')).toBeInTheDocument();
  fireEvent.click(screen.getByRole('treeitem', { name: 'ANNOTATIONS' }));
  missing = true;
  fireEvent.click(await screen.findByRole('button', { name: 'Go to highlight' }));
  expect(await screen.findByText('This note is no longer available.')).toBeInTheDocument();
  expect(screen.queryByRole('button', { name: 'Retry' })).not.toBeInTheDocument();
  expect(screen.queryByText('This entry has changed or was removed.')).not.toBeInTheDocument();
});

it('clears a source navigation error when the source note closes', async () => {
  const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS', read_only: true };
  const generated = { type: 'doc', content: [{ type: 'paragraph', content: [
    { type: 'annotationSource', attrs: { sourceId: 4, entryType: 'highlight', ordinal: 1 } }, { type: 'text', text: 'Target' },
  ] }] };
  let items = [plan, annotation];
  render(<App client={fixtureClient({
    listNoteItems: async () => items,
    getNote: async (id) => ({ id, document_id: id, document: id === 9 ? generated : noteDocument('Changed') }),
    deleteNoteItem: async (id) => { items = items.filter((item) => item.id !== id); },
  })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  fireEvent.click(await screen.findByRole('button', { name: 'Go to highlight' }));
  expect(await screen.findByText('This entry has changed or was removed.')).toBeInTheDocument();
  fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
  fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
  await waitFor(() => expect(screen.queryByRole('treeitem', { name: 'Plan' })).not.toBeInTheDocument());
  expect(screen.queryByText('This entry has changed or was removed.')).not.toBeInTheDocument();
});

it('refreshes a visible annotation note after source rename and deletion', async () => {
  const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS', read_only: true };
  let items = [plan, annotation];
  let text = 'Old annotations';
  render(<App client={fixtureClient({
    listNoteItems: async () => items,
    getNote: async (id) => ({ id, document_id: id, document: noteDocument(text) }),
    renameNoteItem: async (id, name) => { text = 'Renamed annotations'; items = items.map((item) => item.id === id ? { ...item, name } : item); return items[0]; },
    deleteNoteItem: async (id) => { text = 'No comments or highlights.'; items = items.filter((item) => item.id !== id); },
  })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  await screen.findByText('Old annotations');
  fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
  fireEvent.click(screen.getByRole('menuitem', { name: 'Rename' }));
  fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'Renamed' } });
  fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
  await screen.findByText('Renamed annotations');
  fireEvent.click(screen.getByRole('button', { name: 'Actions for Renamed' }));
  fireEvent.click(screen.getByRole('menuitem', { name: 'Delete' }));
  fireEvent.click(await screen.findByRole('button', { name: 'Delete note' }));
  await screen.findByText('No comments or highlights.');
});

it.each(['rename', 'delete'])('refreshes visible annotations after %s even when the tree reload fails', async (operation) => {
  const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS', read_only: true };
  let changed = false;
  const saveNote = vi.fn();
  const saveNoteScrollPosition = vi.fn(async () => undefined);
  render(<App client={fixtureClient({
    listNoteItems: async () => {
      if (changed) throw new Error('List unavailable');
      return [plan, annotation];
    },
    getNote: async (id) => ({ id, document_id: id, document: noteDocument(changed ? 'Fresh annotations' : 'Old annotations'), scroll_top: 240 }),
    renameNoteItem: async (id, name) => { changed = true; return { ...plan, id, name }; },
    deleteNoteItem: async () => { changed = true; },
    saveNote,
    saveNoteScrollPosition,
  })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  await screen.findByText('Old annotations');
  document.querySelector('.cha-notes-document')!.scrollTop = 640;
  fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
  fireEvent.click(screen.getByRole('menuitem', { name: operation === 'rename' ? 'Rename' : 'Delete' }));
  if (operation === 'rename') {
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'Renamed' } });
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
  }
  await screen.findByText('Fresh annotations');
  expect(saveNoteScrollPosition).toHaveBeenCalledWith(9, 640);
  expect(saveNote).not.toHaveBeenCalled();
});

it('refreshes the tree after automatic creation and keeps typing focus in the source', async () => {
  const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS', read_only: true };
  let created = false;
  const listNoteItems = vi.fn(async () => created ? [plan, annotation] : [plan]);
  render(<App client={fixtureClient({
    listNoteItems,
    getNote: async (id) => ({ id, document_id: id, document: noteDocument('Source') }),
    saveNote: async () => { const first = !created; created = true; return { annotation_created: first }; },
  })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
  await waitFor(() => expect(noteEditor().isEditable).toBe(true));
  vi.useFakeTimers();
  try {
    act(() => typeInNote(' added'));
    await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
    expect(screen.getByRole('treeitem', { name: 'ANNOTATIONS' })).toBeInTheDocument();
    expect(noteEditor().view.dom).toHaveFocus();
    expect(noteEditor().getText()).toContain('added');
    expect(listNoteItems).toHaveBeenCalledTimes(2);
    act(() => typeInNote(' more'));
    await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
    expect(listNoteItems).toHaveBeenCalledTimes(2);
  } finally { vi.useRealTimers(); }
});

it('keeps a committed source save successful when the annotation tree refresh fails', async () => {
  const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS', read_only: true };
  const listNoteItems = vi.fn().mockResolvedValueOnce([plan]).mockRejectedValueOnce(new Error('List unavailable'))
    .mockResolvedValue([plan, annotation]);
  const saveNote = vi.fn(async () => ({ annotation_created: true }));
  render(<App client={fixtureClient({ listNoteItems, saveNote, getNote: async (id) => ({ id, document_id: id, document: noteDocument('Source') }) })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
  await waitFor(() => expect(noteEditor().isEditable).toBe(true));
  vi.useFakeTimers();
  try {
    act(() => typeInNote(' saved'));
    await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
    expect(saveNote).toHaveBeenCalledOnce();
    expect(noteEditor().view.dom).toHaveFocus();
    expect(screen.queryByText('The note could not be saved.')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    await act(async () => {});
    expect(screen.getByRole('treeitem', { name: 'ANNOTATIONS' })).toBeInTheDocument();
    expect(saveNote).toHaveBeenCalledOnce();
  } finally { vi.useRealTimers(); }
});

it('discards a pending source navigation when the vault changes during scroll save', async () => {
  const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS', read_only: true };
  const document = { type: 'doc', content: [{ type: 'paragraph', content: [
    { type: 'annotationSource', attrs: { sourceId: 4, entryType: 'highlight', ordinal: 1 } }, { type: 'text', text: 'Target' },
  ] }] };
  let finishScroll!: () => void;
  const events = contextBus();
  const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document }));
  const saveNoteScrollPosition = vi.fn(() => new Promise<void>((resolve) => { finishScroll = resolve; }));
  render(<App client={fixtureClient({ listNoteItems: async () => [plan, annotation], getNote, saveNoteScrollPosition })} contextEvents={events} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  fireEvent.click(await screen.findByRole('button', { name: 'Go to highlight' }));
  await waitFor(() => expect(saveNoteScrollPosition).toHaveBeenCalledOnce());
  events.emit();
  await waitFor(() => expect(screen.queryByRole('button', { name: 'Go to highlight' })).not.toBeInTheDocument());
  await act(async () => { finishScroll(); });
  expect(getNote).toHaveBeenCalledTimes(1);
});

it('opens a newly created note while ANNOTATIONS is visible, even with a slow scroll save', async () => {
  const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS', read_only: true };
  const created: NoteItem = { ...plan, id: 10, name: 'New note' };
  let items = [plan, annotation];
  let finishScroll: (() => void) | undefined;
  const saveScroll = vi.fn(async () => {
    if (saveScroll.mock.calls.length > 1) await new Promise<void>((resolve) => { finishScroll = resolve; });
  });
  render(<App client={fixtureClient({
    listNoteItems: async () => items,
    getNote: async (id) => ({ id, document_id: id, document: noteDocument(id === 10 ? 'Newly created contents' : 'Generated contents') }),
    saveNoteScrollPosition: saveScroll,
    createNoteItem: async () => { items = [...items, created]; return created; },
  })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  await screen.findByText('Generated contents');
  fireEvent.click(screen.getByRole('button', { name: `Actions for ${bootstrapFixture.vault_name}` }));
  fireEvent.click(screen.getByRole('menuitem', { name: 'New note' }));
  fireEvent.click(within(await screen.findByRole('dialog', { name: 'New note' })).getByRole('button', { name: 'Create' }));
  await screen.findByRole('treeitem', { name: 'New note' });
  await waitFor(() => expect(saveScroll).toHaveBeenCalledTimes(2));
  await act(async () => { finishScroll?.(); });
  expect(await screen.findByText('Newly created contents')).toBeInTheDocument();
});

it('shows annotation groups in folder tree order, also after a source rename', async () => {
  const annotation: NoteItem = { ...plan, id: 9, kind: 'annotation', name: 'ANNOTATIONS', read_only: true };
  let items: NoteItem[] = [{ ...plan, name: 'Zebra' }, { ...plan, id: 5, name: 'Apple' }, annotation];
  const group = (item: NoteItem) => [
    { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text: item.name }] },
    { type: 'paragraph', content: [
      { type: 'annotationSource', attrs: { sourceId: item.id, entryType: 'highlight', ordinal: 1 } },
      { type: 'text', text: `Entry ${item.id}` },
    ] },
  ];
  // The stored order is not the tree order.
  const stored = () => ({ type: 'doc', content: items.filter((item) => item.kind === 'note')
    .sort((left, right) => right.name.localeCompare(left.name)).flatMap(group) });
  render(<App client={fixtureClient({
    listNoteItems: async () => items,
    getNote: async (id) => ({ id, document_id: id, document: id === 9 ? stored() : noteDocument('Source') }),
    renameNoteItem: async (id, name) => {
      items = items.map((item) => item.id === id ? { ...item, name } : item);
      return items.find((item) => item.id === id)!;
    },
  })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  await waitFor(() => expect(noteEditor().getText()).toMatch(/Apple[\s\S]*Zebra/u));
  fireEvent.click(screen.getByRole('button', { name: 'Actions for Apple' }));
  fireEvent.click(screen.getByRole('menuitem', { name: 'Rename' }));
  fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'Zulu' } });
  fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
  await waitFor(() => expect(noteEditor().getText()).toMatch(/Zebra[\s\S]*Zulu/u));
});

it.each(['create', 'rename', 'delete'])('refreshes a visible Notes sidebar after local forum %s with no active note', async (operation) => {
  let items: NoteItem[] = [{ ...study, name: 'Forums', forum_id: '' }];
  const linked = { ...study, id: 2, parent_id: 1, name: 'Research', forum_id: 'research' };
  const listNoteItems = vi.fn(async () => items);
  const change = () => { items = [...items, linked]; };
  render(<App client={fixtureClient({
    listNoteItems,
    createForum: async () => { change(); return { ...forumDetailFixture, id: 'research', display_name: 'Research' }; },
    updateForum: async (_id, update) => { change(); return { ...forumDetailFixture, ...update }; },
    deleteForum: async () => { change(); },
  })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'Forums' }));
  const settings = within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' });
  await waitFor(() => expect(settings).toBeEnabled());
  fireEvent.click(settings);
  fireEvent.click(await screen.findByRole('button', { name: 'Forums' }));
  if (operation === 'create') {
    fireEvent.click(await screen.findByRole('button', { name: 'New forum' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'Name' }), { target: { value: 'Research' } });
    fireEvent.click(screen.getByRole('button', { name: 'Create forum' }));
  } else {
    fireEvent.click(await screen.findByRole('button', { name: 'The LobbyGuide' }));
    fireEvent.click(within(await screen.findByLabelText('Forum sessions navigation')).getByRole('button', { name: 'The LobbyGuide' }));
    if (operation === 'rename') {
      fireEvent.click(await screen.findByRole('button', { name: 'Rename The Lobby' }));
      fireEvent.change(screen.getByRole('textbox', { name: 'Forum name' }), { target: { value: 'Research' } });
      fireEvent.click(screen.getByRole('button', { name: 'Save forum name' }));
    } else {
      fireEvent.click(await screen.findByRole('button', { name: 'Delete The Lobby' }));
      expect(screen.getByRole('dialog')).toHaveTextContent('and its Notes folder and contents');
      fireEvent.click(screen.getByRole('button', { name: 'Delete forum' }));
    }
  }
  await screen.findByRole('treeitem', { name: 'Research' });
  expect(listNoteItems).toHaveBeenCalledTimes(2);
});
