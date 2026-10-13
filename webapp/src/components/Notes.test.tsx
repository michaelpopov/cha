import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import type { NoteItem } from '../api/nativeClient';
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
  await screen.findByRole('button', { name: 'New note' });
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
  it('switches sidebar tabs without changing the main panel, chat draft, or address', async () => {
    render(<App client={notesClient()} />);
    const message = await screen.findByRole('textbox', { name: 'Message' });
    fireEvent.change(message, { target: { value: 'Chat draft' } });
    const address = window.location.href;
    await openNotes();
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('Chat draft');
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'chat');
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
    expect(screen.getByRole('main')).toHaveAttribute('data-view', 'notes');
    expect(window.location.href).toBe(address);
    expect(screen.queryByRole('heading', { name: 'Notes' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' }));
    expect(getNote).toHaveBeenCalledOnce();
    expect(screen.getByText('Saved plan')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(screen.getByRole('button', { name: /^Welcome/ }));
    expect(await screen.findByRole('textbox', { name: 'Message' })).toHaveValue('Chat draft');
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

  it('keeps the open note editable when New note is canceled', async () => {
    render(<App client={notesClient('Alpha')} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Alpha');
    fireEvent.click(screen.getByRole('button', { name: 'New note' }));
    await screen.findByRole('dialog', { name: 'New note' });
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(noteEditor().isEditable).toBe(true);
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
    fireEvent.click(screen.getByRole('button', { name: `Actions for ${name}` }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Delete' }));
    const label = kind === 'folder'
      ? `Delete “${name}” and all notes and folders inside it? This cannot be undone in CHA.`
      : `Delete “${name}”? This cannot be undone in CHA.`;
    const dialog = await screen.findByRole('dialog', { name: label });
    fireEvent.click(within(dialog).getByRole('button', {
      name: kind === 'folder' ? 'Delete folder' : 'Delete note',
    }));
    return dialog;
  }

  it('renames and moves a note without reloading its body', async () => {
    const getNote = vi.fn(async (id: number) => ({ id, document: noteDocument('Saved plan') }));
    let renamed = false;
    let moved = false;
    render(<App client={fixtureClient({
      listNoteItems: async () => {
        const note = {
          ...plan,
          name: renamed ? 'Draft' : 'Plan',
          parent_id: moved ? 1 : null,
        };
        return [study, note];
      },
      getNote,
      saveNote: async () => undefined,
      renameNoteItem: async () => {
        renamed = true;
        return { ...plan, name: 'Draft' };
      },
      moveNoteItem: async () => {
        moved = true;
        return { ...plan, name: renamed ? 'Draft' : 'Plan', parent_id: 1 };
      },
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    expect(await screen.findByText('Saved plan')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Rename' }));
    fireEvent.change(screen.getByLabelText('Name'), { target: { value: 'Draft' } });
    fireEvent.click(screen.getByRole('button', { name: 'Rename' }));
    expect(await screen.findByRole('button', { name: 'Send to Draft' })).toBeInTheDocument();
    expect(screen.getByText('Saved plan')).toBeInTheDocument();
    expect(getNote).toHaveBeenCalledOnce();

    fireEvent.click(screen.getByRole('button', { name: 'Actions for Draft' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Move' }));
    fireEvent.change(screen.getByRole('combobox', { name: 'Move “Draft” to' }), { target: { value: '1' } });
    fireEvent.click(screen.getByRole('button', { name: 'Move' }));
    expect(await screen.findByRole('button', { name: 'Send to Study / Draft' })).toBeInTheDocument();
    expect(noteEditor().getText()).toContain('Saved plan');
    expect(getNote).toHaveBeenCalledOnce();
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
    expect(await within(noteRegion()).findByRole('button', { name: 'New note' })).toBeInTheDocument();
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
