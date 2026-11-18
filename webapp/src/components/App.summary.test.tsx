import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { ChaError, type NoteContent, type NoteItem, type SessionSummaryGenerated } from '../api/client';
import { bootstrapFixture, fixtureClient } from '../test/fixtures';
import { noteItem } from '../test/notes';
import { noteSaveDelayMs } from '../notes/useNoteSave';
import { summaryDocument } from '../notes/summaryImport';
import { App } from './App';

function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (failure: unknown) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
const documentFor = (text: string) => ({ type: 'doc', content: [{ type: 'paragraph', content: [{ type: 'text', text }] }] });
const generated: SessionSummaryGenerated = { run_id: 1, target_note_id: 4, parent_id: 10, markdown: '# Short summary\n\nOverview.\n\n**Keywords:** topic\n\n# Long summary\n\nNew summary' };
const summary: NoteItem = noteItem({ id: 4, name: 'Plan', is_summary: true, summary_linked: true });
function editor(): Editor {
  const element = document.querySelector('.ProseMirror') as (HTMLElement & { chaEditor: Editor });
  if (!element?.chaEditor) throw new Error('Editor not ready');
  return element.chaEditor;
}
async function startSummary() {
  fireEvent.click(await screen.findByLabelText('Actions for Planning'));
  fireEvent.click(screen.getByRole('menuitem', { name: 'Save summary' }));
}
async function openTarget(wait = true) {
  fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
  await screen.findByRole('treeitem', { name: bootstrapFixture.vault_name });
  fireEvent.click(await screen.findByRole('treeitem', { name: 'Plan' }));
  if (wait) {
    await screen.findByText('Old body');
    await waitFor(() => expect(editor().isEditable).toBe(true));
  }
}
beforeEach(() => window.history.replaceState(null, '', '/'));
afterEach(() => vi.useRealTimers());

describe('background note summaries', () => {
  const source = noteItem({ id: 5, name: 'Chapter' });
  const other = noteItem({ id: 6, name: 'Draft' });
  async function selectNote(name: string) {
    fireEvent.click(await screen.findByRole('treeitem', { name }));
    await waitFor(() => expect(editor().isEditable).toBe(true));
  }
  function summarize(name: string) {
    fireEvent.click(screen.getByRole('button', { name: `Actions for ${name}` }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Summarize' }));
  }
  function chooseL1() {
    fireEvent.click(screen.getByRole('button', { name: 'Note text mode' }));
    fireEvent.click(screen.getByRole('menuitemradio', { name: 'L1' }));
  }
  async function autosave() {
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    vi.useRealTimers();
  }

  it('keeps the editor and Settings usable, refuses another start, and leaves focus in Chat', async () => {
    const held = deferred<string>();
    const finish = deferred<void>();
    const summaryFinish = vi.fn(() => finish.promise);
    const summaryStart = vi.fn(async () => ({ notes: [source] }));
    const summaryGenerate = vi.fn().mockReturnValueOnce(held.promise).mockResolvedValue('L2');
    const saveNote = vi.fn(async () => {});
    const summaryCommit = vi.fn(async () => {});
    render(<App client={fixtureClient({
      listNoteItems: async () => [source, other],
      getNote: async id => ({ id, document_id: id, document: documentFor(id === source.id ? 'Source text' : 'Draft text') }),
      summaryStart, summaryGenerate, summaryCommit, summaryFinish, saveNote,
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    await selectNote(source.name);
    summarize(source.name);
    await waitFor(() => expect(summaryGenerate).toHaveBeenCalledExactlyOnceWith(source.id, 'l1'));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    vi.useFakeTimers();
    act(() => { editor().commands.insertContent(' Source edit'); });
    await autosave();
    expect(saveNote).toHaveBeenLastCalledWith(source.id, documentFor('Source text Source edit'));
    await selectNote(other.name);
    summarize(other.name);
    expect(await screen.findByText('Summarization is already running for “Chapter”.')).toBeInTheDocument();
    vi.useFakeTimers();
    act(() => { editor().commands.insertContent(' Draft edit'); });
    await autosave();
    expect(saveNote).toHaveBeenLastCalledWith(other.id, documentFor('Draft text Draft edit'));
    chooseL1();
    await waitFor(() => expect(screen.getByRole('button', { name: 'Note text mode' }))
      .toHaveAttribute('title', 'L1 mode; showing full text'));
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    expect(await screen.findByRole('button', { name: 'Events' })).toBeInTheDocument();
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    const message = await screen.findByRole('textbox', { name: 'Message' });
    act(() => { message.focus(); });
    await act(async () => held.resolve('L1'));
    expect(summaryCommit.mock.calls).toHaveLength(2);
    expect(summaryFinish).toHaveBeenCalledExactlyOnceWith(1);
    expect(message).toHaveFocus();
    fireEvent.click(screen.getByRole('tab', { name: 'Notes' }));
    await selectNote(other.name);
    summarize(other.name);
    expect(summaryStart).toHaveBeenCalledExactlyOnceWith(source.id, 1);
    await act(async () => finish.resolve());
  });

  it('preserves a failed initial save and never requests admission', async () => {
    const summaryStart = vi.fn();
    const summaryFinish = vi.fn();
    const saveNote = vi.fn(async () => { throw new ChaError('internal_error', 'Save failed.'); });
    render(<App client={fixtureClient({
      listNoteItems: async () => [source],
      getNote: async id => ({ id, document_id: id, document: documentFor('Old body') }),
      summaryStart, summaryFinish, saveNote,
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    await selectNote(source.name);
    act(() => { editor().commands.insertContent(' Draft edit'); });
    summarize(source.name);
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    await waitFor(() => expect(editor().isEditable).toBe(true));
    expect(editor().getText()).toBe('Old body Draft edit');
    expect(summaryStart).not.toHaveBeenCalled();
    expect(summaryFinish).not.toHaveBeenCalled();
  });

  it('finishes a possibly admitted batch after a lost start reply', async () => {
    const summaryFinish = vi.fn(async () => {});
    const summaryGenerate = vi.fn();
    render(<App client={fixtureClient({
      listNoteItems: async () => [source],
      getNote: async id => ({ id, document_id: id, document: documentFor('Original') }),
      summaryStart: async () => { throw new ChaError('command_timeout', 'Start reply timed out.'); },
      summaryFinish, summaryGenerate,
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    await selectNote(source.name);
    summarize(source.name);
    await screen.findByText('Start reply timed out.');
    expect(summaryFinish).toHaveBeenCalledExactlyOnceWith(1);
    expect(summaryGenerate).not.toHaveBeenCalled();
  });

  it('keeps the batch running when the user chooses to stay after a close attempt', async () => {
    const held = deferred<string>();
    const summaryGenerate = vi.fn().mockReturnValueOnce(held.promise).mockResolvedValue('L2');
    const summaryCommit = vi.fn(async () => {});
    const summaryFinish = vi.fn(async () => {});
    const post = vi.fn();
    window.__CHA_NATIVE_CONNECTION_ID__ = 'view-1';
    window.__CHA_NATIVE_POST__ = post;
    try {
      render(<App client={fixtureClient({
        listNoteItems: async () => [source],
        getNote: async id => ({ id, document_id: id, document: documentFor('Original') }),
        saveNote: async () => { throw new ChaError('internal_error', 'Save failed.'); },
        summaryStart: async () => ({ notes: [source] }), summaryGenerate, summaryCommit, summaryFinish,
      })} />);
      fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
      await selectNote(source.name);
      summarize(source.name);
      await waitFor(() => expect(summaryGenerate).toHaveBeenCalledOnce());
      act(() => { editor().commands.insertContent(' Draft edit'); });
      act(() => window.__CHA_NATIVE_PREPARE_CLOSE__?.(7));
      fireEvent.click(await screen.findByRole('button', { name: 'Stay' }));
      await waitFor(() => expect(post.mock.calls.map(([message]) => JSON.parse(message as string)))
        .toContainEqual({ native_action: 'close_reply', connection_id: 'view-1', attempt_id: 7, result: 'cancelled' }));
      expect(summaryFinish).not.toHaveBeenCalled();
      await act(async () => held.resolve('L1'));
      await waitFor(() => expect(summaryFinish).toHaveBeenCalledOnce());
      expect(summaryCommit).toHaveBeenCalledTimes(2);
      expect(editor().getText()).toBe('Original Draft edit');
    } finally {
      delete window.__CHA_NATIVE_POST__;
      delete window.__CHA_NATIVE_CONNECTION_ID__;
    }
  });

  it.each([true, false])('settles an L1 save before committing and only replaces the buffer on success (%s)', async success => {
    const held = deferred<string>();
    const write = deferred<void>();
    const summaryGenerate = vi.fn().mockReturnValueOnce(held.promise).mockResolvedValue('L2');
    const summaryFinish = vi.fn(async () => {});
    const saveNote = vi.fn(() => write.promise);
    let body = 'Old body';
    const summaryCommit = vi.fn(async (_id, part) => {
      if (part !== 'l1') return;
      if (!success) throw new ChaError('internal_error', 'Commit failed.');
      body = 'New L1';
    });
    render(<App client={fixtureClient({
      listNoteItems: async () => [{ ...source, has_l1: true }],
      getNote: async (id, mode) => ({ id, document_id: mode === 'l1' ? 40 : id, document: documentFor(body) }),
      summaryStart: async () => ({ notes: [source] }), summaryGenerate, summaryCommit, summaryFinish, saveNote,
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    await selectNote(source.name);
    summarize(source.name);
    await waitFor(() => expect(summaryGenerate).toHaveBeenCalledOnce());
    chooseL1();
    await waitFor(() => expect(screen.getByRole('button', { name: 'Note text mode' }))
      .toHaveAttribute('title', 'L1 mode; showing L1'));
    const old = editor();
    vi.useFakeTimers();
    act(() => { old.commands.insertContent(' In-flight edit'); });
    await autosave();
    expect(saveNote).toHaveBeenCalledExactlyOnceWith(40, documentFor('Old body In-flight edit'));
    await act(async () => held.resolve('New L1'));
    expect(old.isEditable).toBe(false);
    expect(summaryCommit).not.toHaveBeenCalled();
    await act(async () => write.resolve());
    await waitFor(() => expect(summaryFinish).toHaveBeenCalledOnce());
    if (success) {
      expect(await screen.findByText('New L1')).toBeInTheDocument();
      expect(old.isDestroyed).toBe(true);
    } else {
      expect(editor()).toBe(old);
      expect(old.isEditable).toBe(true);
      expect(old.getText()).toBe('Old body In-flight edit');
    }
    expect(saveNote).toHaveBeenCalledOnce();
  });

  it('discards a pending pre-commit L1 read and loads the saved result', async () => {
    const held = deferred<string>();
    const stale = deferred<NoteContent>();
    const summaryGenerate = vi.fn().mockReturnValueOnce(held.promise).mockResolvedValue('L2');
    const summaryFinish = vi.fn(async () => {});
    let committed = false;
    const getNote = vi.fn(async (id, mode) => {
      if (mode === 'l1' && !committed) return stale.promise;
      return { id, document_id: mode === 'l1' ? 40 : id, document: documentFor(committed ? 'New L1' : 'Original') };
    });
    render(<App client={fixtureClient({
      listNoteItems: async () => [{ ...source, has_l1: true }],
      getNote, summaryStart: async () => ({ notes: [source] }), summaryGenerate, summaryFinish,
      summaryCommit: async () => { committed = true; },
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    await selectNote(source.name);
    summarize(source.name);
    await waitFor(() => expect(summaryGenerate).toHaveBeenCalledOnce());
    chooseL1();
    await waitFor(() => expect(getNote).toHaveBeenLastCalledWith(source.id, 'l1'));
    await act(async () => held.resolve('New L1'));
    await screen.findByText('New L1');
    await act(async () => stale.resolve({ id: source.id, document_id: 40, document: documentFor('Stale L1') }));
    expect(editor().getText()).toBe('New L1');
    expect(summaryFinish).toHaveBeenCalledOnce();
  });

  it('keeps note actions blocked when an unknown L1 commit outcome reloads the connection', async () => {
    const held = deferred<string>();
    const reload = vi.fn();
    let barrierFails = false;
    const summaryGenerate = vi.fn().mockReturnValueOnce(held.promise).mockResolvedValue('L2');
    const summaryFinish = vi.fn(async () => {});
    const getNote = vi.fn(async (id, mode) => ({ id, document_id: mode === 'l1' ? 40 : id, document: documentFor('Old L1') }));
    render(<App reload={reload} client={fixtureClient({
      listNoteItems: async () => {
        if (barrierFails) {
          barrierFails = false;
          throw new ChaError('command_timeout', 'Barrier timed out.');
        }
        return [{ ...source, has_l1: true }, other];
      },
      getNote, summaryStart: async () => ({ notes: [source] }), summaryGenerate, summaryFinish,
      summaryCommit: async (_id, part) => {
        if (part !== 'l1') return;
        barrierFails = true;
        throw new ChaError('command_timeout', 'Commit timed out.');
      },
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    await selectNote(source.name);
    summarize(source.name);
    await waitFor(() => expect(summaryGenerate).toHaveBeenCalledOnce());
    chooseL1();
    await waitFor(() => expect(screen.getByRole('button', { name: 'Note text mode' }))
      .toHaveAttribute('title', 'L1 mode; showing L1'));
    const old = editor();
    await act(async () => held.resolve('New L1'));
    await waitFor(() => expect(summaryFinish).toHaveBeenCalledOnce());
    expect(reload).toHaveBeenCalledOnce();
    expect(old.isDestroyed).toBe(true);
    // A queued commit may still run, so no note opens before the reload.
    await act(async () => { fireEvent.click(screen.getByRole('treeitem', { name: other.name })); });
    expect(getNote).not.toHaveBeenCalledWith(other.id, expect.anything());
  });

  it.each([true, false])('saves the original fallback edits before reopening L1 (%s)', async success => {
    const held = deferred<string>();
    const summaryGenerate = vi.fn().mockReturnValueOnce(held.promise).mockResolvedValue('L2');
    const summaryFinish = vi.fn(async () => {});
    const saveNote = vi.fn(async () => {
      if (!success) throw new ChaError('internal_error', 'Save failed.');
    });
    let committed = false;
    const getNote = vi.fn(async (id, mode) => {
      const version = committed && mode === 'l1';
      return { id, document_id: version ? 40 : id, document: documentFor(version ? 'New L1' : 'Original') };
    });
    render(<App client={fixtureClient({
      listNoteItems: async () => [source], getNote, saveNote,
      summaryStart: async () => ({ notes: [source] }), summaryGenerate, summaryFinish,
      summaryCommit: async () => { committed = true; },
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    await selectNote(source.name);
    summarize(source.name);
    await waitFor(() => expect(summaryGenerate).toHaveBeenCalledOnce());
    chooseL1();
    await waitFor(() => expect(getNote).toHaveBeenLastCalledWith(source.id, 'l1'));
    const original = await waitFor(() => editor());
    act(() => { original.commands.insertContent(' Draft edit'); });
    await act(async () => held.resolve('New L1'));
    await waitFor(() => expect(summaryFinish).toHaveBeenCalledOnce());
    expect(saveNote).toHaveBeenCalledExactlyOnceWith(source.id, documentFor('Original Draft edit'));
    if (success) expect(await screen.findByText('New L1')).toBeInTheDocument();
    else {
      expect(editor()).toBe(original);
      expect(editor().getText()).toBe('Original Draft edit');
      expect(editor().isEditable).toBe(true);
      expect(screen.getByText(/Could not save “Chapter”/)).toBeInTheDocument();
    }
  });
});

describe('summarizing a notes folder', () => {
  it.each(['Summary', 'ANNOTATIONS'] as const)('reopens %s after the folder commit', async name => {
    const folder = noteItem({ id: 10, kind: 'folder', name: 'Study' });
    const source = noteItem({ id: 5, parent_id: folder.id, name: 'Chapter', has_compressed: true, has_l1: true, has_l2: true });
    const target = noteItem({ id: 6, parent_id: folder.id, name, read_only: true,
      kind: name === 'ANNOTATIONS' ? 'annotation' : 'note',
      folder_summary: name === 'Summary', is_summary: name === 'Summary' });
    const section = deferred<string>();
    const overview = deferred<string>();
    const commit = deferred<void>();
    const reopen = deferred<void>();
    let committed = false;
    const getNote = vi.fn(async (id: number) => {
      if (committed) await reopen.promise;
      return { id, document_id: id, document: documentFor(committed ? 'Updated content' : 'Old content') };
    });
    const summaryGenerate = vi.fn((_id, part) => part === 'l3_section' ? section.promise : overview.promise);
    const summaryCommit = vi.fn(async () => {
      await commit.promise;
      committed = true;
    });
    const summaryFinish = vi.fn(async () => {});
    const listNoteItems = vi.fn(async () => [folder, source, target]);
    render(<App client={fixtureClient({
      listNoteItems, getNote, summaryStart: async () => ({ notes: [source] }), summaryGenerate, summaryCommit, summaryFinish,
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    fireEvent.click(await screen.findByRole('treeitem', { name }));
    await screen.findByText('Old content');
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    const summarize = screen.getByRole('menuitem', { name: 'Summarize' });
    await waitFor(() => expect(summarize).toBeEnabled());
    fireEvent.click(summarize);

    await waitFor(() => expect(summaryGenerate).toHaveBeenCalledWith(source.id, 'l3_section'));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    await act(async () => section.resolve('Chapter section'));
    await waitFor(() => expect(summaryGenerate).toHaveBeenCalledWith(folder.id, 'l3_overview', '# Chapter\n\nChapter section'));
    await act(async () => overview.resolve('Overview'));
    expect(summaryCommit).toHaveBeenCalledExactlyOnceWith(folder.id, 'l3', expect.any(Object));
    listNoteItems.mockClear();
    await act(async () => commit.resolve());
    await waitFor(() => expect(summaryFinish).toHaveBeenCalledOnce());
    expect(listNoteItems).toHaveBeenCalled();
    expect(getNote).toHaveBeenCalledTimes(2);
    expect(getNote).toHaveBeenLastCalledWith(target.id, 'full_text');
    await act(async () => reopen.resolve());
    expect(editor().getText()).toBe('Updated content');
    expect(editor().isEditable).toBe(false);
  });

  it.each([true, false])('reloads an open Compressed version only when the run made it (%s)', async made => {
    const folder = noteItem({ id: 10, kind: 'folder', name: 'Study' });
    const source = noteItem({ id: 5, parent_id: folder.id, name: 'Chapter', has_compressed: true, has_l1: true, has_l2: true });
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: documentFor('Chapter text') }));
    const compressed = deferred<boolean>();
    const summaryCompress = vi.fn(() => compressed.promise);
    const summaryFinish = vi.fn(async () => {});
    render(<App client={fixtureClient({
      listNoteItems: async () => [folder, source],
      getNote,
      summaryStart: async () => ({ notes: [
        { id: source.id, name: source.name, has_compressed: false, has_l1: true, has_l2: true },
      ] }),
      summaryCompress, summaryFinish,
      summaryGenerate: async (_id, part) => part === 'l3_section' ? 'Section' : 'Overview',
      summaryCommit: async () => {},
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Chapter' }));
    await screen.findByText('Chapter text');
    fireEvent.click(screen.getByRole('button', { name: 'Note text mode' }));
    fireEvent.click(screen.getByRole('menuitemradio', { name: 'Compressed' }));
    await waitFor(() => expect(screen.getByRole('button', { name: 'Note text mode' }))
      .toHaveAttribute('title', 'Compressed mode; showing full text'));
    getNote.mockClear();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    const summarize = screen.getByRole('menuitem', { name: 'Summarize' });
    await waitFor(() => expect(summarize).toBeEnabled());
    fireEvent.click(summarize);
    await waitFor(() => expect(summaryCompress).toHaveBeenCalledOnce());
    await act(async () => compressed.resolve(made));
    await waitFor(() => expect(summaryFinish).toHaveBeenCalledOnce());
    if (made) expect(getNote).toHaveBeenCalledExactlyOnceWith(source.id, 'compressed');
    else expect(getNote).not.toHaveBeenCalled();
  });
});

describe('saving session summaries', () => {
  it('targets the clicked inactive session, closes the menu immediately and saves after navigation', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const generateSessionSummary = vi.fn(() => ready.promise);
    const commitSessionSummary = vi.fn(async () => ({ ...summary, read_only: true }));
    const openSession = vi.fn(async (forum_id: string, session_id: string) => ({ forum_id, session_id }));
    render(<App client={fixtureClient({ generateSessionSummary, commitSessionSummary, openSession })} />);
    await startSummary();
    expect(generateSessionSummary).toHaveBeenCalledWith('lobby', 'planning');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(screen.getByLabelText('Actions for Planning')).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    await act(async () => ready.resolve({ ...generated, target_note_id: null }));
    await waitFor(() => expect(commitSessionSummary).toHaveBeenCalledWith(1, summaryDocument(generated.markdown)));
    expect(screen.queryByRole('region', { name: 'Note' })).not.toBeInTheDocument();
    expect(openSession).not.toHaveBeenCalledWith('lobby', 'planning');
  });

  it('keeps the chat screen when the saved target is open only in a hidden Notes panel', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const commitSessionSummary = vi.fn(async () => ({ ...summary, read_only: true }));
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise, commitSessionSummary,
      listNoteItems: async () => [summary], getNote: async id => ({ id, document_id: id, document: documentFor('Old body') }) })} />);
    await startSummary();
    await openTarget();
    act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('split'));
    const chat = await screen.findByRole('textbox', { name: 'Message' });
    fireEvent.click(screen.getByRole('tab', { name: 'Chat' }));
    fireEvent.click(within(screen.getByLabelText('Recent sessions')).getByRole('button', { name: /^Welcome/ }));
    act(() => window.__CHA_NATIVE_SET_LAYOUT__?.('single'));
    await waitFor(() => expect(document.querySelector('.ProseMirror')).toBeNull());
    await act(async () => ready.resolve(generated));
    await waitFor(() => expect(commitSessionSummary).toHaveBeenCalledOnce());
    await act(async () => { await Promise.resolve(); });
    expect(document.querySelector('.ProseMirror')).toBeNull();
    expect(screen.getByRole('textbox', { name: 'Message' })).toBe(chat);
  });

  it('settles an in-flight autosave and disposes the old buffer before loading read-only content', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const oldWrite = deferred<void>();
    const saveNote = vi.fn(() => oldWrite.promise);
    let saved = documentFor('Old body');
    let item = summary;
    const commitSessionSummary = vi.fn(async () => { saved = documentFor('New summary'); item = { ...summary, read_only: true }; return item; });
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise, commitSessionSummary,
      listNoteItems: async () => [item], getNote: async id => ({ id, document_id: id, document: saved }), saveNote })} />);
    await startSummary();
    await openTarget();
    const oldEditor = editor();
    vi.useFakeTimers();
    act(() => { oldEditor.commands.insertContent(' Unsaved edits'); });
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).toHaveBeenCalledOnce();
    vi.useRealTimers();
    await act(async () => ready.resolve(generated));
    expect(oldEditor.isEditable).toBe(false);
    expect(commitSessionSummary).not.toHaveBeenCalled();
    await act(async () => oldWrite.resolve());
    await screen.findByText('New summary');
    expect(oldEditor.isDestroyed).toBe(true);
    expect(editor().isEditable).toBe(false);
    expect(saveNote).toHaveBeenCalledOnce();
  });

  it('preserves and resumes the old editable buffer on a definite commit failure', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const saveNote = vi.fn(async () => undefined);
    const commitSessionSummary = vi.fn(async () => { throw new ChaError('invalid_argument', 'The summary target changed.'); });
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise, commitSessionSummary,
      listNoteItems: async () => [summary], getNote: async id => ({ id, document_id: id, document: documentFor('Old body') }), saveNote })} />);
    await startSummary();
    await openTarget();
    const oldEditor = editor();
    act(() => { oldEditor.commands.insertContent(' Unsaved'); });
    await act(async () => ready.resolve(generated));
    await waitFor(() => expect(saveNote).toHaveBeenCalledOnce());
    expect(oldEditor.isDestroyed).toBe(false);
    expect(oldEditor.isEditable).toBe(true);
    expect(oldEditor.getText()).toContain('Unsaved');
    expect(await screen.findByRole('alert')).toHaveTextContent('The summary target changed.');
  });

  it('keeps a lost-reply buffer frozen until the abandon barrier acknowledges and then reloads', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const barrier = deferred<void>();
    let body = 'Old body';
    let item = summary;
    const abandonSessionSummary = vi.fn(() => barrier.promise);
    const saveNote = vi.fn(async () => undefined);
    const getNote = vi.fn(async id => ({ id, document_id: id, document: documentFor(body) }));
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise,
      commitSessionSummary: async () => { body = 'New summary'; item = { ...summary, read_only: true }; throw new TypeError('Lost transport reply'); },
      abandonSessionSummary, listNoteItems: async () => [item], getNote, saveNote })} />);
    await startSummary();
    await openTarget();
    const oldEditor = editor();
    act(() => { oldEditor.commands.insertContent(' Stale comment'); });
    await act(async () => ready.resolve(generated));
    await waitFor(() => expect(abandonSessionSummary).toHaveBeenCalledWith(1, 'commit_unknown'));
    expect(oldEditor.isEditable).toBe(false);
    expect(getNote).toHaveBeenCalledOnce();
    expect(saveNote).not.toHaveBeenCalled();
    await act(async () => barrier.resolve());
    await screen.findByText('New summary');
    expect(oldEditor.isDestroyed).toBe(true);
    expect(saveNote).not.toHaveBeenCalled();
  });

  it('discards the old buffer when the vault changes before a failed commit reply', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    let epoch = 1;
    const saveNote = vi.fn(async () => undefined);
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise,
      contextEpoch: () => epoch,
      commitSessionSummary: async () => {
        epoch = 2; // The bridge sees the new epoch before App has rendered the new vault.
        throw new ChaError('vault_changed', 'The active vault changed.');
      },
      listNoteItems: async () => [summary], getNote: async id => ({ id, document_id: id, document: documentFor('Old body') }), saveNote })} />);
    await startSummary();
    await openTarget();
    const oldEditor = editor();
    act(() => { oldEditor.commands.insertContent(' Unsaved old-vault edits'); });
    await act(async () => ready.resolve(generated));
    expect(saveNote).not.toHaveBeenCalled();
    expect(oldEditor.isDestroyed).toBe(true);
  });

  it('does not resume autosave if the vault changes while settling the old write', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const oldWrite = deferred<void>();
    let epoch = 1;
    const saveNote = vi.fn(() => oldWrite.promise);
    const commitSessionSummary = vi.fn(async () => ({ ...summary, read_only: true }));
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise, commitSessionSummary,
      contextEpoch: () => epoch,
      listNoteItems: async () => [summary], getNote: async id => ({ id, document_id: id, document: documentFor('Old body') }), saveNote })} />);
    await startSummary();
    await openTarget();
    const oldEditor = editor();
    vi.useFakeTimers();
    act(() => { oldEditor.commands.insertContent(' First edit'); });
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    act(() => { oldEditor.commands.insertContent(' Second edit'); });
    vi.useRealTimers();
    await act(async () => ready.resolve(generated));
    expect(oldEditor.isEditable).toBe(false);
    epoch = 2;
    await act(async () => oldWrite.resolve());
    expect(commitSessionSummary).not.toHaveBeenCalled();
    expect(saveNote).toHaveBeenCalledOnce();
    expect(oldEditor.isDestroyed).toBe(true);
  });

  it('recovers the connection if the unknown-commit barrier also fails', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const reload = vi.fn();
    const getNote = vi.fn(async id => ({ id, document_id: id, document: documentFor('Old body') }));
    render(<App reload={reload} client={fixtureClient({ generateSessionSummary: () => ready.promise,
      commitSessionSummary: async () => { throw new ChaError('command_timeout', 'Commit timed out.'); },
      abandonSessionSummary: async () => { throw new ChaError('command_timeout', 'Barrier timed out.'); },
      listNoteItems: async () => [summary], getNote })} />);
    await startSummary();
    await openTarget();
    const oldEditor = editor();
    await act(async () => ready.resolve(generated));
    expect(oldEditor.isDestroyed).toBe(true);
    expect(reload).toHaveBeenCalledOnce();
    // Reopening before the new connection is ready could load pre-commit content.
    await act(async () => { fireEvent.click(screen.getByRole('treeitem', { name: 'Plan' })); });
    expect(getNote).toHaveBeenCalledOnce();
  });

  it('cancels a slow pre-commit note load so its late reply cannot construct an editor', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const staleLoad = deferred<{ id: number; document_id: number; document: ReturnType<typeof documentFor> }>();
    const commit = deferred<NoteItem>();
    const getNote = vi.fn().mockImplementationOnce(() => staleLoad.promise)
      .mockImplementation(async id => ({ id, document_id: id, document: documentFor('New summary') }));
    const commitSessionSummary = vi.fn(() => commit.promise);
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise, commitSessionSummary,
      listNoteItems: async () => [summary], getNote })} />);
    await startSummary();
    await openTarget(false);
    await waitFor(() => expect(getNote).toHaveBeenCalledOnce());
    await act(async () => ready.resolve(generated));
    await waitFor(() => expect(commitSessionSummary).toHaveBeenCalledOnce());
    await act(async () => staleLoad.resolve({ id: 4, document_id: 4, document: documentFor('Stale loaded body') }));
    expect(screen.queryByText('Stale loaded body')).not.toBeInTheDocument();
    expect(document.querySelector('.ProseMirror')).toBeNull();
    await act(async () => commit.resolve({ ...summary, read_only: true }));
    await screen.findByText('New summary');
  });

  it('waits for a busy note action guard without discarding the generated document', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const action = deferred<NoteItem>();
    const setNoteReadOnly = vi.fn(() => action.promise);
    const commitSessionSummary = vi.fn(async () => ({ ...summary, read_only: true }));
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise, commitSessionSummary,
      setNoteReadOnly, listNoteItems: async () => [summary], getNote: async id => ({ id, document_id: id, document: documentFor('Old body') }) })} />);
    await startSummary();
    await openTarget();
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Plan' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Make read-only' }));
    await waitFor(() => expect(setNoteReadOnly).toHaveBeenCalledOnce());
    await act(async () => ready.resolve(generated));
    expect(commitSessionSummary).not.toHaveBeenCalled();
    await act(async () => action.resolve({ ...summary, read_only: true }));
    await waitFor(() => expect(commitSessionSummary).toHaveBeenCalledOnce());
  });

  it('abandons conversion failures without changing the open note', async () => {
    const ready = deferred<SessionSummaryGenerated>();
    const abandonSessionSummary = vi.fn(async () => undefined);
    const commitSessionSummary = vi.fn(async () => summary);
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise, abandonSessionSummary, commitSessionSummary,
      listNoteItems: async () => [summary], getNote: async id => ({ id, document_id: id, document: documentFor('Old body') }) })} />);
    await startSummary();
    await openTarget();
    const oldEditor = editor();
    await act(async () => ready.resolve({ ...generated, markdown: '![](https://example.org/image.png)' }));
    await waitFor(() => expect(abandonSessionSummary).toHaveBeenCalledWith(1, 'conversion_failed'));
    expect(commitSessionSummary).not.toHaveBeenCalled();
    expect(oldEditor.isEditable).toBe(true);
    expect(oldEditor.isDestroyed).toBe(false);
  });
});

it('refreshes an annotations document opened after generation started', async () => {
  const ready = deferred<SessionSummaryGenerated>();
  let body = 'Old annotations';
  const annotation: NoteItem = { ...summary, id: 9, name: 'ANNOTATIONS', kind: 'annotation',
    is_summary: false, summary_linked: false, parent_id: 10, read_only: true };
  const folder: NoteItem = { ...summary, id: 10, name: 'Forum folder', kind: 'folder', is_summary: false, summary_linked: false };
  const getNote = vi.fn(async id => ({ id, document_id: id, document: documentFor(body) }));
  render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise,
    commitSessionSummary: async () => { body = 'Fresh annotations'; return { ...summary, read_only: true }; },
    listNoteItems: async () => [folder, annotation], getNote })} />);
  await waitFor(() => expect(screen.getByRole('textbox', { name: 'Message' })).toBeEnabled());
  await startSummary();
  fireEvent.click(screen.getByRole('tab', { name: 'Notes' }));
  fireEvent.click(await screen.findByRole('treeitem', { name: 'Forum folder' }));
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  await screen.findByText('Old annotations');
  await act(async () => ready.resolve(generated));
  await screen.findByText('Fresh annotations');
  expect(getNote).toHaveBeenCalledTimes(2);
});
