import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import {
  ChaError,
  type NoteItem,
} from '../api/client';
import { bootstrapFixture, fixtureClient, snapshotFixture } from '../test/fixtures';
import { App } from './App';

const plan: NoteItem = { id: 4, read_only: false, has_compressed: false, parent_id: null, kind: 'note', name: 'Plan' };
const study: NoteItem = { id: 1, read_only: false, has_compressed: false, parent_id: null, kind: 'folder', name: 'Study' };

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
    layout(mode: string) { window.__CHA_NATIVE_SET_LAYOUT__?.(mode); },
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

function historyAction(name: 'Undo' | 'Redo') {
  fireEvent.click(screen.getByRole('button', { name: 'History' }));
  return screen.getByRole('menuitem', { name });
}

describe('Notes in the application', () => {
  it('generates from the selected saved document and stops playback when its dialog opens', async () => {
    const changed = vi.fn();
    window.addEventListener('cha-note-audio-changed', changed);
    const document = { type: 'doc', content: [
      { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text: 'Title' }] },
      { type: 'paragraph', content: [{ type: 'text', text: 'First' }, { type: 'hardBreak' }, { type: 'text', text: 'Second' }] },
    ] };
    const start = vi.fn(async () => ({ id: plan.id, phase: 'complete' as const, done: true,
      error: '', completed: 2, total: 2, finished_at_ms: Date.now() }));
    try {
      render(<App client={fixtureClient({
        listNoteItems: async () => [{ ...plan, read_only: true }],
        getNote: async id => ({ id, document }),
        noteAudio: { info: async () => ({ count: 0 }), start, status: vi.fn(), source: vi.fn(), release: vi.fn() },
      })} />);
      await openNotes();
      fireEvent.click(await screen.findByRole('button', { name: 'Actions for Plan' }));
      fireEvent.click(screen.getByRole('menuitem', { name: 'Generate audio' }));
      const dialog = await screen.findByRole('dialog', { name: 'Generate note audio' });
      expect(changed).toHaveBeenCalledOnce();
      expect(start).not.toHaveBeenCalled();
      await waitFor(() => expect(within(dialog).getByLabelText('Voice')).toBeEnabled());
      fireEvent.change(within(dialog).getByLabelText('Voice'), { target: { value: 'grant' } });
      fireEvent.click(within(dialog).getByRole('button', { name: 'Generate' }));
      await screen.findByText('Audio ready · 2 clips');
      expect(start).toHaveBeenCalledExactlyOnceWith(plan.id, 'grant', ['Title', 'First\nSecond']);
      fireEvent.click(within(dialog).getByRole('button', { name: 'Close' }));
      expect(changed).toHaveBeenCalledTimes(2);
    } finally { window.removeEventListener('cha-note-audio-changed', changed); }
  });

  it('toggles Notes prompt visibility and its menu checkmark without losing the draft or editor', async () => {
    const post = vi.fn();
    window.__CHA_NATIVE_CONNECTION_ID__ = 'notes-prompt-test';
    window.__CHA_NATIVE_POST__ = post;
    const bus = contextBus();
    const reportedPrompt = () => post.mock.calls
      .map(([message]) => JSON.parse(message as string) as { native_action: string; notes_prompt: boolean })
      .filter((message) => message.native_action === 'view_state').at(-1)?.notes_prompt;
    try {
      render(<App client={notesClient()} contextEvents={bus} />);
      await openNotes();
      expect(reportedPrompt()).toBe(true);
      fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
      await waitFor(() => expect(noteEditor().isEditable).toBe(true));
      const editor = noteEditor();
      const prompt = screen.getByRole('textbox', { name: 'Prompt' });
      const toolbar = document.querySelector('.cha-notes-toolbar');
      fireEvent.change(prompt, { target: { value: 'Unsent notes draft' } });
      act(() => typeInNote(' edited'));
      const selection = editor.state.selection.toJSON();
      act(() => bus.layout('toggle-notes-prompt'));
      expect(screen.queryByRole('textbox', { name: 'Prompt' })).toBeNull();
      expect(prompt.closest('.cha-composer-area')).toHaveAttribute('hidden');
      expect(prompt).toBeDisabled();
      expect(reportedPrompt()).toBe(false);
      expect(noteEditor()).toBe(editor);
      expect(editor.isEditable).toBe(true);
      expect(editor.state.selection.toJSON()).toEqual(selection);
      expect(document.querySelector('.cha-notes-toolbar')).toBe(toolbar);
      act(() => bus.layout('split'));
      expect(await screen.findByRole('textbox', { name: 'Message' })).toBeVisible();
      expect(screen.queryByRole('textbox', { name: 'Prompt' })).toBeNull();
      act(() => bus.layout('toggle-notes-prompt'));
      expect(screen.getByRole('textbox', { name: 'Prompt' })).toBe(prompt);
      expect(prompt).toBeEnabled();
      expect(prompt).toHaveValue('Unsent notes draft');
      expect(reportedPrompt()).toBe(true);
      expect(noteEditor()).toBe(editor);
    } finally {
      delete window.__CHA_NATIVE_POST__;
      delete window.__CHA_NATIVE_CONNECTION_ID__;
    }
  });

  it('finishes a pending Notes prompt submission while hidden and preserves text typed after sending', async () => {
    const page = { url: 'https://example.org/', title: 'Article', markdown: 'Imported article' };
    let finish!: (value: typeof page) => void;
    const readNotePage = vi.fn(() => new Promise<typeof page>((resolve) => { finish = resolve; }));
    const bus = contextBus();
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document: noteDocument('Original') }),
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

  it('toggles the Notes toolbar through View without replacing the editor or its history', async () => {
    const post = vi.fn();
    window.__CHA_NATIVE_CONNECTION_ID__ = 'toolbar-test';
    window.__CHA_NATIVE_POST__ = post;
    const bus = contextBus();
    const reportedToolbar = () => post.mock.calls
      .map(([message]) => JSON.parse(message as string) as { native_action: string; toolbar: boolean })
      .filter((message) => message.native_action === 'view_state').at(-1)?.toolbar;
    try {
      render(<App client={notesClient()} contextEvents={bus} />);
      await openNotes();
      expect(reportedToolbar()).toBe(true);
      act(() => bus.layout('toggle-toolbar'));
      fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
      await waitFor(() => expect(noteEditor().isEditable).toBe(true));
      expect(document.querySelector('.cha-notes-toolbar')).toBeNull();
      expect(reportedToolbar()).toBe(false);
      const editor = noteEditor();
      const prompt = screen.getByRole('textbox', { name: 'Prompt' });
      act(() => typeInNote(' edited'));
      const selection = editor.state.selection.toJSON();
      act(() => bus.layout('toggle-toolbar'));
      expect(document.querySelector('.cha-notes-toolbar')).toBeInTheDocument();
      expect(reportedToolbar()).toBe(true);
      expect(noteEditor()).toBe(editor);
      expect(editor.state.selection.toJSON()).toEqual(selection);
      expect(screen.getByRole('textbox', { name: 'Prompt' })).toBe(prompt);
      act(() => bus.layout('toggle-toolbar'));
      act(() => bus.layout('split'));
      await screen.findByRole('textbox', { name: 'Message' });
      expect(document.querySelector('.cha-notes-toolbar')).toBeNull();
      expect(reportedToolbar()).toBe(false);
      act(() => bus.layout('toggle-toolbar'));
      expect(document.querySelector('.cha-notes-toolbar')).toBeInTheDocument();
      expect(reportedToolbar()).toBe(true);
      expect(noteEditor()).toBe(editor);
      act(() => { editor.commands.undo(); });
      expect(editor.getText()).toBe('Saved plan');
      act(() => { editor.commands.redo(); });
      expect(editor.getText()).toContain(' edited');
    } finally {
      delete window.__CHA_NATIVE_POST__;
      delete window.__CHA_NATIVE_CONNECTION_ID__;
    }
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
        getNote: async (id) => ({ id, document: noteDocument('Saved plan') }),
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

  it('ignores startup and invalid layout commands and applies a preference set in Settings on return', async () => {
    const bus = contextBus();
    render(<App client={notesClient()} contextEvents={bus} />);
    act(() => bus.layout('split'));
    await openNotes();
    expect(screen.queryByRole('separator', { name: 'Resize notes and chat' })).toBeNull();
    act(() => bus.layout('invalid'));
    expect(screen.queryByRole('separator', { name: 'Resize notes and chat' })).toBeNull();
    fireEvent.click(within(screen.getByLabelText('Sidebar')).getByRole('button', { name: 'Settings' }));
    await screen.findByRole('region', { name: 'Settings' });
    act(() => bus.layout('split'));
    expect(screen.getByRole('region', { name: 'Settings' })).toBeInTheDocument();
    expect(screen.queryByRole('separator', { name: 'Resize notes and chat' })).toBeNull();
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Welcome/ }));
    await screen.findByRole('separator', { name: 'Resize notes and chat' });
    expect(document.querySelector('.cha-workspace-panel.is-notes')).toBeInTheDocument();
    expect(document.querySelector('.ProseMirror')).toBeNull();
  });

  it('saves and restores a note hidden by layout while keeping chat mounted', async () => {
    const bus = contextBus();
    const getNote = vi.fn(async (id: number) => ({ id, document: noteDocument('Saved plan') }));
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
    expect(getNote.mock.calls[1]).toEqual([plan.id]);
    expect(screen.getByRole('textbox', { name: 'Message' })).toBe(chat);
  });

  it('ignores a layout command while a chat open started in Split View is pending', async () => {
    const bus = contextBus();
    let finishOpen!: () => void;
    const planningOpen = new Promise<void>((resolve) => { finishOpen = resolve; });
    const saveNote = vi.fn(async () => undefined);
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id: number) => ({ id, document: noteDocument('Saved plan') }),
      saveNote,
      openSession: async (forumId, sessionId) => {
        if (sessionId === 'planning') await planningOpen;
        return { forum_id: forumId, session_id: sessionId };
      },
    })} contextEvents={bus} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => bus.layout('split'));
    await screen.findByRole('textbox', { name: 'Message' });
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Planning/ }));
    await screen.findByText('Opening session…');
    act(() => bus.layout('single'));
    await act(async () => finishOpen());
    await screen.findByRole('textbox', { name: 'Message' });
    expect(screen.getByRole('separator', { name: 'Resize notes and chat' })).toBeInTheDocument();
    expect(noteEditor().isEditable).toBe(true);
    expect(saveNote).not.toHaveBeenCalled();
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
      getNote: async (id) => ({ id, document: noteDocument('Original') }),
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
      getNote: async (id) => ({ id, document: noteDocument('Original') }),
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
    const compressNote = vi.fn(async () => undefined);
    render(<App client={fixtureClient({
      listNoteItems: async () => [locked],
      getNote: async (id) => ({ id, document: noteDocument('Original text') }),
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
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await waitFor(() => expect(compressNote).toHaveBeenCalledOnce());
    await within(screen.getByRole('dialog')).findByRole('button', { name: 'Close' });
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
      getNote: async (id) => ({ id, document: saved }),
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

  it('keeps a note with text editable after a web import', async () => {
    const saveNote = vi.fn(async () => undefined);
    const setNoteReadOnly = vi.fn(async (id: number, readOnly: boolean) => ({ ...plan, id, read_only: readOnly }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document: noteDocument('Original') }),
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

  it.each([false, true])('saves an edited subfolder note before compression or stops when cancelled (cancel=%s)', async (cancel) => {
    const nested = { ...study, id: 2, parent_id: study.id, name: 'Nested' };
    const child = { ...plan, parent_id: nested.id };
    let finishSave!: () => void;
    const saveNote = vi.fn((_id: number, _document: Record<string, unknown>) => new Promise<void>((resolve) => { finishSave = resolve; }));
    const compressNote = vi.fn(async () => 'too_small' as const);
    render(<App client={fixtureClient({
      listNoteItems: async () => [study, nested, child],
      getNote: async (id) => ({ id, document: noteDocument('Original') }),
      saveNote, compressNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Nested' }));
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => typeInNote(' edited'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    const dialog = screen.getByRole('dialog');
    expect(within(dialog).getByRole('listitem')).toHaveTextContent('PlanSaving…');
    expect(compressNote).not.toHaveBeenCalled();
    expect(noteEditor().isEditable).toBe(false);
    if (cancel) fireEvent.click(within(dialog).getByRole('button', { name: 'Cancel' }));
    await act(async () => finishSave());
    await waitFor(() => expect(within(dialog).getByRole('button', { name: 'Close' })).toBeInTheDocument());
    if (cancel) {
      expect(compressNote).not.toHaveBeenCalled();
      expect(within(dialog).getByRole('listitem')).toHaveTextContent('PlanCancelled');
      expect(within(dialog).getByRole('status')).toHaveTextContent('Cancelled · 0 / 1 processed');
    } else {
      expect(compressNote).toHaveBeenCalledExactlyOnceWith(plan.id, expect.any(AbortSignal), true);
      expect(within(dialog).getByRole('listitem')).toHaveTextContent('PlanToo small');
      expect(within(dialog).getByRole('status')).toHaveTextContent('1 / 1 processed');
    }
    expect(saveNote.mock.calls[0][1]).toEqual(noteEditor().getJSON());
    expect(noteEditor().isEditable).toBe(true);
  });

  it('saves pending edits before compression and cancels during saving without sending', async () => {
    let finishSave!: () => void;
    const compressNote = vi.fn();
    const saveNote = vi.fn((_id: number, _document: Record<string, unknown>) => new Promise<void>((resolve) => { finishSave = resolve; }));
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document: noteDocument('Original') }),
      saveNote, compressNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => typeInNote(' edited'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    expect(screen.getByRole('status')).toHaveTextContent('Saving “Plan”…');
    expect(noteEditor().isEditable).toBe(false);
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    await act(async () => finishSave());
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(compressNote).not.toHaveBeenCalled();
    expect(noteEditor().isEditable).toBe(true);
    expect(saveNote.mock.calls[0][1]).toEqual(noteEditor().getJSON());
  });

  it('does not compress after a failed save and saves again on the next attempt', async () => {
    const compressNote = vi.fn(async () => undefined);
    const saveNote = vi.fn()
      .mockRejectedValueOnce(new ChaError('internal_error', 'The note could not be saved.'))
      .mockResolvedValue(undefined);
    render(<App client={fixtureClient({
      listNoteItems: async () => [plan],
      getNote: async (id) => ({ id, document: noteDocument('Original') }),
      saveNote,
      compressNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    act(() => typeInNote(' edited'));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await waitFor(() => expect(within(screen.getByRole('dialog')).getByRole('alert'))
      .toHaveTextContent('The note could not be saved.'));
    expect(compressNote).not.toHaveBeenCalled();
    expect(noteEditor().isEditable).toBe(true);
    expect(noteEditor().getText()).toContain('edited');
    fireEvent.click(within(screen.getByRole('dialog')).getByRole('button', { name: 'Close' }));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    await waitFor(() => expect(compressNote).toHaveBeenCalledOnce());
    expect(saveNote).toHaveBeenCalledTimes(2);
  });

  it('does not delay a window close while the compression request runs', async () => {
    const post = vi.fn();
    window.__CHA_NATIVE_CONNECTION_ID__ = 'view-1';
    window.__CHA_NATIVE_POST__ = post;
    try {
      const compressNote = vi.fn((_id: number, _signal: AbortSignal) => new Promise<void>(() => undefined));
      render(<App client={fixtureClient({
        listNoteItems: async () => [plan],
        getNote: async (id) => ({ id, document: noteDocument('Original') }),
        compressNote,
      })} />);
      await openNotes();
      fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
      await waitFor(() => expect(noteEditor().isEditable).toBe(true));
      fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
      fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
      await waitFor(() => expect(compressNote).toHaveBeenCalledOnce());
      act(() => window.__CHA_NATIVE_PREPARE_CLOSE__?.(7));
      await waitFor(() => expect(post.mock.calls.map(([message]) => JSON.parse(message as string)))
        .toContainEqual({ native_action: 'close_reply', connection_id: 'view-1', attempt_id: 7, result: 'ready' }));
    } finally {
      delete window.__CHA_NATIVE_POST__;
      delete window.__CHA_NATIVE_CONNECTION_ID__;
    }
  });

  it('keeps editing frozen and delays refresh until cancellation is confirmed', async () => {
    let fail!: (error: unknown) => void;
    const compressNote = vi.fn((_id: number, _signal: AbortSignal) => new Promise<void>((_resolve, reject) => { fail = reject; }));
    const listNoteItems = vi.fn(async () => [{ ...plan, read_only: false, has_compressed: true }]);
    render(<App client={fixtureClient({
      listNoteItems,
      getNote: async (id) => ({ id, document: noteDocument('Original') }),
      compressNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await waitFor(() => expect(noteEditor().isEditable).toBe(true));
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Compress' }));
    // The open note has nothing to save, so Saving never shows.
    expect(screen.getByRole('status')).toHaveTextContent('Compressing “Plan”…');
    expect(noteEditor().isEditable).toBe(false);
    await waitFor(() => expect(compressNote).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(compressNote.mock.calls[0][1].aborted).toBe(true);
    expect(listNoteItems).toHaveBeenCalledTimes(1);
    expect(noteEditor().isEditable).toBe(false);
    await act(async () => fail(new ChaError('operation_cancelled', 'Cancelled.')));
    await waitFor(() => expect(screen.queryByRole('dialog')).not.toBeInTheDocument());
    expect(listNoteItems).toHaveBeenCalledTimes(2);
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
    expect(noteEditor().isEditable).toBe(true);
    expect(screen.getByRole('img', { name: 'Compressed version saved' })).toBeInTheDocument();
  });

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
    await waitFor(() => expect(document.title).toBe('CHA: Personal — Welcome'));
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    expect(document.title).toBe('CHA: Personal');
  });

  it.each(['immediate', 'delayed'])('focuses the new note editor with %s loading', async (loading) => {
    const created: NoteItem = { id: 9, read_only: false, has_compressed: false, parent_id: null, kind: 'note', name: 'New note' };
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
    const essay: NoteItem = { id: 5, read_only: false, has_compressed: false, parent_id: null, kind: 'note', name: 'Essay' };
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
      getNote: async (id: number) => ({ id, document: noteDocument('Saved plan') }),
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
      listNoteItems: async () => [plan], getNote: async (id) => ({ id, document: stored }), saveNote,
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
      getNote: async (id) => ({ id, document: noteDocument('Saved plan') }),
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
      id,
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
    const getNote = vi.fn(async (id: number) => ({ id, document: noteDocument(saved) }));
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

  it('uses live edits if a note opens while its deletion length is being read', async () => {
    let finishRead!: (note: { id: number; document: Record<string, unknown> }) => void;
    const getNote = vi.fn((id: number) => getNote.mock.calls.length === 1
      ? new Promise<{ id: number; document: Record<string, unknown> }>((resolve) => { finishRead = resolve; })
      : Promise.resolve({ id, document: noteDocument('Short') }));
    const deleteNoteItem = vi.fn();
    render(<App client={fixtureClient({ listNoteItems: async () => [plan], getNote, deleteNoteItem })} />);
    await openNotes();
    await deleteItem('Plan', 'note', false);
    await waitFor(() => expect(getNote).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Short');
    await waitFor(() => noteEditor());
    act(() => { noteEditor().commands.setContent(noteDocument('A long unsaved note body')); });
    await act(async () => { finishRead({ id: 4, document: noteDocument('Short') }); });
    expect(await screen.findByRole('dialog')).toHaveTextContent('Delete “Plan”?');
    expect(deleteNoteItem).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
  });

  it('targets the note menu and returns row focus when Rename or Delete is canceled', async () => {
    const renameNoteItem = vi.fn();
    const deleteNoteItem = vi.fn();
    const saveNote = vi.fn();
    render(<App client={fixtureClient({
      listNoteItems: async () => [study, plan],
      getNote: async (id) => ({ id, document: noteDocument('Saved plan contents') }),
      renameNoteItem, deleteNoteItem, saveNote,
    })} />);
    await openNotes();
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
    await screen.findByText('Saved plan contents');
    const editor = await waitFor(() => noteEditor());
    expect(screen.getByRole('button', { name: 'Actions for Plan' })).toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Actions for Study' })).toBeInTheDocument();
    act(() => screen.getByRole('treeitem', { name: 'Study' }).focus());
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Rename' }));
    const rename = await screen.findByRole('dialog', { name: 'Rename “Plan”' });
    expect(within(rename).getByRole('textbox', { name: 'Name' })).toHaveValue('Plan');
    fireEvent.click(within(rename).getByRole('button', { name: 'Cancel' }));
    expect(screen.getByRole('treeitem', { name: 'Plan' })).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Delete' }));
    const deletion = await screen.findByRole('dialog', { name: 'Delete “Plan”? This cannot be undone in CHA.' });
    expect(within(deletion).getByRole('button', { name: 'Cancel' })).toHaveFocus();
    fireEvent.click(within(deletion).getByRole('button', { name: 'Cancel' }));
    expect(screen.getByRole('treeitem', { name: 'Plan' })).toHaveFocus();
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
    const getNote = vi.fn(async (id: number) => ({ id, document: noteDocument('Saved plan contents') }));
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
      getNote: async (id: number) => ({ id, document: noteDocument('Saved plan contents') }),
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
    const essay: NoteItem = { id: 5, read_only: false, has_compressed: false, parent_id: null, kind: 'note', name: 'Essay' };
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
    const getNote = vi.fn(async () => ({ id: 4, document: noteDocument('Gamma note contents') }));
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
    getNote: async (id) => {
      if (missing && id !== 9) throw new ChaError('not_found', 'That note was not found.');
      return { id, scroll_top: 500, document: id === 9 ? generated : stale ? noteDocument('Changed') : source };
    },
    saveNote,
  })} />);
  await openNotes();
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  fireEvent.click(await screen.findByRole('button', { name: 'Go to highlight' }));
  await waitFor(() => expect(noteEditor().state.doc.textBetween(noteEditor().state.selection.from, noteEditor().state.selection.to)).toBe('Target'));
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
    getNote: async (id) => ({ id, document: id === 9 ? generated : noteDocument('Changed') }),
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
    getNote: async (id) => ({ id, document: noteDocument(text) }),
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
    getNote: async (id) => ({ id, document: noteDocument(changed ? 'Fresh annotations' : 'Old annotations'), scroll_top: 240 }),
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
    getNote: async (id) => ({ id, document: noteDocument('Source') }),
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
  render(<App client={fixtureClient({ listNoteItems, saveNote, getNote: async (id) => ({ id, document: noteDocument('Source') }) })} />);
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
  const getNote = vi.fn(async (id: number) => ({ id, document }));
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
    getNote: async (id) => ({ id, document: noteDocument(id === 10 ? 'Newly created contents' : 'Generated contents') }),
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
    getNote: async (id) => ({ id, document: id === 9 ? stored() : noteDocument('Source') }),
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
