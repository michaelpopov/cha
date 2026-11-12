import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { ChaError, type NoteItem, type SessionSummaryGenerated } from '../api/client';
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

describe('summarizing a notes folder', () => {
  it.each(['Summary', 'ANNOTATIONS'] as const)('reopens %s after the folder commit with Cancel disabled', async name => {
    const folder = noteItem({ id: 10, kind: 'folder', name: 'Study' });
    const source = noteItem({ id: 5, parent_id: folder.id, name: 'Chapter', has_l1: true, has_l2: true });
    const target = noteItem({ id: 6, parent_id: folder.id, name, read_only: true,
      kind: name === 'ANNOTATIONS' ? 'annotation' : 'note',
      folder_summary: name === 'Summary', is_summary: name === 'Summary' });
    const section = deferred<string>();
    const overview = deferred<string>();
    const commit = deferred<void>();
    const reopen = deferred<void>();
    let committed = false;
    let cancelDisabledAtCommit: boolean | undefined;
    const getNote = vi.fn(async (id: number) => {
      if (committed) await reopen.promise;
      return { id, document_id: id, document: documentFor(committed ? 'Updated content' : 'Old content') };
    });
    const summaryGenerate = vi.fn((_id, part) => part === 'l3_section' ? section.promise : overview.promise);
    const summaryCommit = vi.fn(async () => {
      cancelDisabledAtCommit = screen.getByRole<HTMLButtonElement>('button', { name: 'Cancel' }).disabled;
      await commit.promise;
      committed = true;
    });
    const summaryCancel = vi.fn();
    const listNoteItems = vi.fn(async () => [folder, source, target]);
    render(<App client={fixtureClient({
      listNoteItems, getNote, summaryPlan: async () => ({ notes: [source] }), summaryGenerate, summaryCommit, summaryCancel,
    })} />);
    fireEvent.click(await screen.findByRole('tab', { name: 'Notes' }));
    fireEvent.click(await screen.findByRole('treeitem', { name: 'Study' }));
    fireEvent.click(await screen.findByRole('treeitem', { name }));
    await screen.findByText('Old content');
    fireEvent.click(screen.getByRole('button', { name: 'Actions for Study' }));
    const summarize = screen.getByRole('menuitem', { name: 'Summarize' });
    await waitFor(() => expect(summarize).toBeEnabled());
    fireEvent.click(summarize);

    const dialog = await screen.findByRole('dialog', { name: 'Note summaries' });
    await within(dialog).findByText('Summarizing “Chapter”…');
    expect(dialog).toHaveTextContent('Note 1 of 1');
    await act(async () => section.resolve('Chapter section'));
    await within(dialog).findByText('Writing the Summary note…');
    const cancel = within(dialog).getByRole('button', { name: 'Cancel' });
    expect(cancel).toBeEnabled();
    await act(async () => overview.resolve('Overview'));
    expect(summaryCommit).toHaveBeenCalledExactlyOnceWith(folder.id, 'l3', expect.any(Object));
    expect(cancelDisabledAtCommit).toBe(true);
    expect(cancel).toBeDisabled();
    fireEvent(dialog, new Event('cancel', { cancelable: true }));
    expect(summaryCancel).not.toHaveBeenCalled();
    listNoteItems.mockClear();
    await act(async () => commit.resolve());
    await within(dialog).findByText('Opening note…');
    expect(listNoteItems).toHaveBeenCalled();
    expect(cancel).toBeDisabled();
    expect(getNote).toHaveBeenCalledTimes(2);
    expect(getNote).toHaveBeenLastCalledWith(target.id, 'full_text');
    await act(async () => reopen.resolve());
    expect(dialog).not.toBeInTheDocument();
    expect(editor().getText()).toBe('Updated content');
    expect(editor().isEditable).toBe(false);
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
