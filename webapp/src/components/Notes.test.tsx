import type { Editor } from '@tiptap/core';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import type { NoteItem } from '../api/nativeClient';
import { fixtureClient } from '../test/fixtures';
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

  it('keeps the Notes draft and a disabled Send control across sidebar tabs', async () => {
    render(<App client={notesClient()} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    const prompt = await screen.findByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'Remember this' } });
    expect(screen.getByRole('button', { name: 'Send to Plan' })).toBeDisabled();
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(screen.getByRole('tab', { name: 'Notes' }));
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Remember this');
    expect(screen.getByRole('button', { name: 'Send to Plan' })).toBeDisabled();
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
});
