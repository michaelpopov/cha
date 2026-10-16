import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import {
  ChaError,
  type NoteItem,
} from '../api/client';
import { bootstrapFixture, fixtureClient, snapshotFixture } from '../test/fixtures';
import { App } from './App';

const plan: NoteItem = { id: 4, parent_id: null, kind: 'note', name: 'Plan' };
const study: NoteItem = { id: 1, parent_id: null, kind: 'folder', name: 'Study' };

function noteDocument(text: string) {
  return {
    type: 'doc',
    content: [{ type: 'paragraph', content: [{ type: 'text', text }] }],
  };
}

function notesClient(text = 'Saved plan') {
  return fixtureClient({
    listNoteItems: async () => [study, plan],
    getNote: async (id: number) => ({ id, document: noteDocument(text) }),
    saveNote: async () => undefined,
  });
}

function contextBus() {
  type Handler = (event: { causing_request_id?: number; state?: string }) => void;
  let handler: Handler | null = null;
  return {
    emit() {
      handler?.({ causing_request_id: 1, state: 'running' });
    },
    on<T>(_event: string, next: (payload: T) => void) {
      handler = next as Handler;
      return () => {
        if (handler === next) handler = null;
      };
    },
  };
}

async function openNotes() {
  fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
  await screen.findByRole('treeitem', { name: bootstrapFixture.vault_name });
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

describe('Notes in the application', () => {
  it.each([0, 1, 3])('uses an available page title in the tree and Send label with %s existing names', async (count) => {
    const original = { ...plan, name: 'New note 1' };
    const siblings = Array.from({ length: count }, (_, index) => ({
      ...plan, id: 20 + index, name: index === 0 ? 'Article title' : `Article title ${index}`,
    }));
    const expected = count === 0 ? 'Article title' : `Article title ${count}`;
    const getNote = vi.fn(async (id: number) => ({ id, document: {
      type: 'doc', content: [{ type: 'paragraph' }],
    } }));
    const renameNoteItem = vi.fn(async (id: number, name: string) => ({ ...original, id, name }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [original, ...siblings],
      getNote,
      saveNote: async () => undefined,
      readNotePage: async () => ({ url: 'https://example.org/', title: 'Article title', markdown: 'Article text' }),
      renameNoteItem,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'New note 1' }));
    const prompt = await screen.findByRole('textbox', { name: 'Prompt' });
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => prompt.focus());
    fireEvent.change(prompt, { target: { value: 'example.org' } });
    fireEvent.keyDown(prompt, { key: 'Enter' });
    expect(await screen.findByRole('treeitem', { name: expected })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: `Send to ${expected}` })).toBeInTheDocument();
    expect(document.title).toBe(`CHA: Personal — ${expected}`);
    await waitFor(() => expect(prompt).toHaveValue(''));
    expect(prompt).toHaveFocus();
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
    expect(screen.getByLabelText('Recent forums')).toBeInTheDocument();
    expect(window.location.href).toBe(address);
  });

  it('opens one note, keeps the chat draft, and focuses the same note again', async () => {
    const getNote = vi.fn(async (id: number) => ({ id, document: noteDocument('Saved plan') }));
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
    expect(noteEditor().view.dom).toHaveFocus();
    expect(getNote).toHaveBeenCalledOnce();
    expect(screen.getByText('Saved plan')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    expect(document.title).toBe('CHA: Personal — Plan');
    fireEvent.click(screen.getByRole('button', { name: /^Welcome/ }));
    expect(await screen.findByRole('textbox', { name: 'Message' })).toHaveValue('Chat draft');
    expect(document.title).toBe('CHA: Personal — Welcome');
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    expect(document.title).toBe('CHA: Personal');
  });

  it.each(['immediate', 'delayed'])('focuses the new note editor with %s loading', async (loading) => {
    const created: NoteItem = { id: 9, parent_id: null, kind: 'note', name: 'New note' };
    let items = [plan];
    let finishOpen!: () => void;
    const newDocument = { type: 'doc', content: [{ type: 'paragraph' }] };
    const pendingOpen = new Promise<{ id: number; document: typeof newDocument }>((resolve) => {
      finishOpen = () => resolve({ id: created.id, document: newDocument });
    });
    const createNoteItem = vi.fn(async () => {
      items = [...items, created];
      return created;
    });
    render(<App client={fixtureClient({
      listNoteItems: async () => items,
      createNoteItem,
      getNote: async (id) => {
        if (id !== created.id) return { id, document: noteDocument('Saved plan') };
        return loading === 'delayed' ? pendingOpen : { id, document: newDocument };
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
    const essay: NoteItem = { id: 5, parent_id: null, kind: 'note', name: 'Essay' };
    const saveNote = vi.fn(async (_id: number, _document: Record<string, unknown>) => undefined);
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan, essay],
      getNote: async (id: number) => ({
        id,
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
      getNote: async (id: number) => ({ id, document: noteDocument('Saved plan') }),
      saveNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
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
      listNoteItems: async () => [plan], getNote: async (id) => ({ id, document: stored }), saveNote,
      listVaults: async () => [vault], uploadVault, createVault,
      checkVaultUpload: async () => ({ status: 'match', etag: 'known', context_epoch: 1 }),
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
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
      getNote: async (id) => ({ id, document: noteDocument(activeVault) }),
      saveNote, discardUnusedSession, switchVault,
    })} />);
    fireEvent.change(await screen.findByRole('textbox', { name: 'Message' }), { target: { value: 'Chat draft' } });
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(document.querySelector('.ProseMirror')).toHaveTextContent('Personal'));
    const originalEditor = noteEditor();
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
      getNote: async (id) => ({ id, document: noteDocument('Saved plan') }),
      saveNote,
      deleteSession,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    const editor = noteEditor();
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
    const getNote = vi.fn(async (id: number) => ({ id, document: noteDocument('Saved plan') }));
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
    const editor = noteEditor();
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
    fireEvent.click(screen.getByRole('button', { name: 'Undo' }));
    expect(editor.getText()).toBe('Saved plan');
    fireEvent.click(screen.getByRole('button', { name: 'Redo' }));
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
    const getNote = vi.fn(async (id: number) => ({ id, document: noteDocument('Saved plan') }));
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
    const editor = noteEditor();
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
      fireEvent.click(screen.getByRole('button', { name: 'Undo' }));
      expect(editor.getText()).toBe('Saved plan');
    }
  });

  it('replaces an open note when the vault context changes, even for the same id', async () => {
    const events = contextBus();
    const getNote = vi.fn()
      .mockResolvedValueOnce({ id: 4, document: noteDocument('Alpha') })
      .mockResolvedValueOnce({ id: 4, document: noteDocument('Gamma') });
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
      getNote: async (id: number) => ({ id, document: noteDocument('Alpha') }),
      saveNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Alpha');
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

  async function deleteItem(name: string, kind: 'note' | 'folder') {
    if (kind === 'folder') {
      fireEvent.click(screen.getByRole('button', { name: `Actions for ${name}` }));
      fireEvent.click(await screen.findByRole('menuitem', { name: 'Delete' }));
    } else {
      fireEvent.click(screen.getByRole('button', { name: 'Delete' }));
    }
    const label = kind === 'folder'
      ? `Delete “${name}” and all notes and folders inside it? This cannot be undone in CHA.`
      : `Delete “${name}”? This cannot be undone in CHA.`;
    const dialog = await screen.findByRole('dialog', { name: label });
    fireEvent.click(within(dialog).getByRole('button', {
      name: kind === 'folder' ? 'Delete folder' : 'Delete note',
    }));
    return dialog;
  }

  it('targets the open note and returns editor focus when Rename or Delete is canceled', async () => {
    const renameNoteItem = vi.fn();
    const deleteNoteItem = vi.fn();
    const saveNote = vi.fn();
    render(<App client={fixtureClient({
      listNoteItems: async () => [study, plan],
      getNote: async (id) => ({ id, document: noteDocument('Saved plan') }),
      renameNoteItem, deleteNoteItem, saveNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan');
    const editor = noteEditor();
    expect(screen.queryByRole('button', { name: 'Actions for Plan' })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeInTheDocument();
    act(() => screen.getByRole('treeitem', { name: 'Study' }).focus());
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    const rename = await screen.findByRole('dialog', { name: 'Rename “Plan”' });
    expect(within(rename).getByRole('textbox', { name: 'Name' })).toHaveValue('Plan');
    fireEvent.click(within(rename).getByRole('button', { name: 'Cancel' }));
    expect(editor.view.dom).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'Delete' }));
    const deletion = await screen.findByRole('dialog', { name: 'Delete “Plan”? This cannot be undone in CHA.' });
    expect(within(deletion).getByRole('button', { name: 'Cancel' })).toHaveFocus();
    fireEvent.click(within(deletion).getByRole('button', { name: 'Cancel' }));
    expect(editor.view.dom).toHaveFocus();
    expect(noteEditor()).toBe(editor);
    expect(renameNoteItem).not.toHaveBeenCalled();
    expect(deleteNoteItem).not.toHaveBeenCalled();
    expect(saveNote).not.toHaveBeenCalled();
  });

  it('renames a note without reloading its body', async () => {
    const getNote = vi.fn(async (id: number) => ({ id, document: noteDocument('Saved plan') }));
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
    expect(screen.queryByRole('button', { name: 'Actions for Plan' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Draft' } });
    fireEvent.click(within(screen.getByRole('dialog', { name: 'Rename “Plan”' })).getByRole('button', { name: 'Rename' }));
    expect(await screen.findByRole('button', { name: 'Send to Draft' })).toBeInTheDocument();
    expect(screen.getByText('Saved plan')).toBeInTheDocument();
    expect(getNote).toHaveBeenCalledOnce();
    expect(noteEditor().view.dom).toHaveFocus();
  });

  it('deletes an open note without saving the remainder and keeps the draft', async () => {
    const saveNote = vi.fn(async () => undefined);
    const getNote = vi.fn(async (id: number) => ({ id, document: noteDocument('Saved plan') }));
    let removed = false;
    render(<App client={fixtureClient({
      listNoteItems: async () => (removed ? [study] : [study, plan]),
      getNote,
      saveNote,
      deleteNoteItem: async () => { removed = true; },
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Saved plan')).toBeInTheDocument();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'Remember this' } });
    typeInNote(' more');
    await deleteItem('Plan', 'note');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled());
    expect(within(noteRegion()).queryByRole('button', { name: 'New note' })).not.toBeInTheDocument();
    expect(screen.queryByText('Saved plan more')).not.toBeInTheDocument();
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
      getNote: async (id: number) => ({ id, document: noteDocument('Saved plan') }),
      saveNote,
      deleteNoteItem: async () => {
        throw new ChaError('invalid_argument', 'The note is still open in the vault.');
      },
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Saved plan')).toBeInTheDocument();
    typeInNote(' more');
    await deleteItem('Plan', 'note');
    expect(await screen.findByRole('alert')).toHaveTextContent('The note is still open in the vault.');
    expect(screen.queryByRole('dialog', { name: 'Could not save “Plan”.' })).not.toBeInTheDocument();
    expect(noteEditor().isEditable).toBe(true);
    expect(noteEditor().getText()).toContain('Saved plan more');
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    expect(JSON.stringify(saveNote.mock.calls[0][1])).toContain('Saved plan more');
  });

  it('keeps an unrelated note editable and blocks navigation during deletion', async () => {
    const essay: NoteItem = { id: 5, parent_id: null, kind: 'note', name: 'Essay' };
    let finishDelete: () => void = () => {};
    const getNote = vi.fn(async (id: number) => ({
      id,
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
    await deleteItem('Study', 'folder');
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
    const getNote = vi.fn(async () => ({ id: 4, document: noteDocument('Gamma') }));
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
    expect(await screen.findByText('Gamma')).toBeInTheDocument();
    await deleteItem('Plan', 'note');
    events.emit();
    await waitFor(() => expect(screen.queryByText('Gamma')).not.toBeInTheDocument());
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(getNote).toHaveBeenCalledOnce();
    finishDelete();
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
    });
    fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Gamma')).toBeInTheDocument();
    await waitFor(() => expect(getNote).toHaveBeenCalledTimes(2));
    expect(screen.getByText('Gamma')).toBeInTheDocument();
  });
});
