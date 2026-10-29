import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { ChaError, type CommandResult, type NoteItem, type SessionSummaryGenerated } from '../api/client';
import { bootstrapFixture, fixtureClient, snapshotFixture } from '../test/fixtures';
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
const summary: NoteItem = { id: 4, parent_id: null, name: 'Plan', kind: 'note', is_summary: true,
  summary_linked: true, has_compressed: false, read_only: false };
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
  if (wait) await screen.findByText('Old body');
}
beforeEach(() => window.history.replaceState(null, '', '/'));
afterEach(() => vi.useRealTimers());

describe('saving session summaries', () => {
  const snapshot = { ...snapshotFixture, forum: bootstrapFixture.forums[1],
    session_id: 'planning', session_label: 'Planning', default_character_id: 'guide',
    characters: [bootstrapFixture.characters[1]] };
  // Waits for the live snapshot: a composer can show before the route opens the session.
  async function renderPromptApp(overrides: Parameters<typeof fixtureClient>[0]) {
    window.history.replaceState(null, '', '/#/s/lobby/planning/');
    const client = fixtureClient({
      getBootstrap: async () => ({ ...bootstrapFixture, initial_forum_id: 'lobby', initial_session_id: 'planning' }),
      getSessionSnapshot: async () => snapshot,
      ...overrides,
    });
    let opened = false;
    render(<App client={client} connectSessionEvents={(_forumId, _sessionId, handlers) => {
      queueMicrotask(() => { handlers.onSnapshot(snapshot); opened = true; });
      return { close() {} };
    }} />);
    await waitFor(() => expect(opened).toBe(true));
  }
  async function submitPrompt(text: string) {
    await screen.findByRole('button', { name: 'Send message' });
    fireEvent.change(screen.getByRole('textbox', { name: 'Message' }), { target: { value: text } });
    fireEvent.click(screen.getByRole('button', { name: 'Send message' }));
  }

  it.each(['Save this conversation.', 'Save this conversation. Why did you recommend that?'])(
    'uses the existing save procedure when JEV accepts "%s"', async text => {
      const submitInput = vi.fn(async () => ({ clear_input: true, save_summary: true }));
      const generateSessionSummary = vi.fn(async () => ({ ...generated, target_note_id: null }));
      const commitSessionSummary = vi.fn(async () => ({ ...summary, read_only: true }));
      await renderPromptApp({ submitInput, generateSessionSummary, commitSessionSummary });
      await submitPrompt(text);
      await waitFor(() => expect(commitSessionSummary).toHaveBeenCalledWith(1, summaryDocument(generated.markdown)));
      expect(submitInput).toHaveBeenCalledWith('lobby', 'planning', { text });
      expect(generateSessionSummary).toHaveBeenCalledExactlyOnceWith('lobby', 'planning');
      expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('');
      expect(screen.queryByRole('region', { name: 'Note' })).not.toBeInTheDocument();
    },
  );

  it('saves after the reply to an accompanying question ends', async () => {
    let replying = false;
    const getSessionSnapshot = vi.fn(async () => ({ ...snapshot,
      generation: { ...snapshot.generation, active: replying } }));
    const generateSessionSummary = vi.fn(async () => ({ ...generated, target_note_id: null }));
    const commitSessionSummary = vi.fn(async () => ({ ...summary, read_only: true }));
    await renderPromptApp({ getSessionSnapshot, generateSessionSummary, commitSessionSummary,
      submitInput: async () => { replying = true; return { clear_input: true, save_summary: true }; } });
    const loads = getSessionSnapshot.mock.calls.length;
    await submitPrompt('Save this conversation. Why did you recommend that?');
    await waitFor(() => expect(getSessionSnapshot.mock.calls.length).toBeGreaterThan(loads));
    await act(async () => { await Promise.resolve(); });
    expect(generateSessionSummary).not.toHaveBeenCalled();
    replying = false;
    await waitFor(() => expect(commitSessionSummary).toHaveBeenCalledOnce(), { timeout: 3_000 });
    expect(getSessionSnapshot).toHaveBeenLastCalledWith('lobby', 'planning');
    expect(generateSessionSummary).toHaveBeenCalledExactlyOnceWith('lobby', 'planning');
  });

  it.each<CommandResult>([{ clear_input: true }, { clear_input: true, save_summary: false },
    { clear_input: false, save_summary: true }])('does not save for an unaccepted save decision %j', async result => {
    const submitInput = vi.fn(async () => result);
    const generateSessionSummary = vi.fn();
    await renderPromptApp({ submitInput, generateSessionSummary });
    await submitPrompt('That was interesting.');
    await waitFor(() => expect(submitInput).toHaveBeenCalledOnce());
    await act(async () => { await Promise.resolve(); });
    expect(generateSessionSummary).not.toHaveBeenCalled();
  });

  it('saves the submitted conversation if the user navigates before classification completes', async () => {
    const submitted = deferred<CommandResult>();
    const generateSessionSummary = vi.fn(async () => ({ ...generated, target_note_id: null }));
    const commitSessionSummary = vi.fn(async () => ({ ...summary, read_only: true }));
    await renderPromptApp({ submitInput: () => submitted.promise, generateSessionSummary, commitSessionSummary });
    await submitPrompt('Save this conversation.');
    fireEvent.click(screen.getByRole('button', { name: 'Settings' }));
    await act(async () => submitted.resolve({ clear_input: true, save_summary: true }));
    await waitFor(() => expect(commitSessionSummary).toHaveBeenCalledOnce());
    expect(generateSessionSummary).toHaveBeenCalledExactlyOnceWith('lobby', 'planning');
  });

  it('ignores a save decision that arrives after the vault context changes', async () => {
    const submitted = deferred<CommandResult>();
    const generateSessionSummary = vi.fn();
    let epoch = 1;
    await renderPromptApp({ contextEpoch: () => epoch,
      submitInput: () => submitted.promise, generateSessionSummary });
    await submitPrompt('Save this conversation.');
    epoch++;
    await act(async () => submitted.resolve({ clear_input: true, save_summary: true }));
    expect(generateSessionSummary).not.toHaveBeenCalled();
  });

  it('reports a summary failure through the existing error path without restoring the accepted prompt', async () => {
    await renderPromptApp({
      submitInput: async () => ({ clear_input: true, save_summary: true }),
      generateSessionSummary: async () => { throw new ChaError('invalid_argument', 'Notes provider is unavailable.'); },
    });
    await submitPrompt('Save this conversation.');
    expect(await screen.findByRole('alert')).toHaveTextContent('Notes provider is unavailable.');
    expect(screen.getByRole('textbox', { name: 'Message' })).toHaveValue('');
  });

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
      listNoteItems: async () => [summary], getNote: async id => ({ id, document: documentFor('Old body') }) })} />);
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
      listNoteItems: async () => [item], getNote: async id => ({ id, document: saved }), saveNote })} />);
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
      listNoteItems: async () => [summary], getNote: async id => ({ id, document: documentFor('Old body') }), saveNote })} />);
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
    const getNote = vi.fn(async id => ({ id, document: documentFor(body) }));
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
      listNoteItems: async () => [summary], getNote: async id => ({ id, document: documentFor('Old body') }), saveNote })} />);
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
      listNoteItems: async () => [summary], getNote: async id => ({ id, document: documentFor('Old body') }), saveNote })} />);
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
    const getNote = vi.fn(async id => ({ id, document: documentFor('Old body') }));
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
    const staleLoad = deferred<{ id: number; document: ReturnType<typeof documentFor> }>();
    const commit = deferred<NoteItem>();
    const getNote = vi.fn().mockImplementationOnce(() => staleLoad.promise)
      .mockImplementation(async id => ({ id, document: documentFor('New summary') }));
    const commitSessionSummary = vi.fn(() => commit.promise);
    render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise, commitSessionSummary,
      listNoteItems: async () => [summary], getNote })} />);
    await startSummary();
    await openTarget(false);
    await waitFor(() => expect(getNote).toHaveBeenCalledOnce());
    await act(async () => ready.resolve(generated));
    await waitFor(() => expect(commitSessionSummary).toHaveBeenCalledOnce());
    await act(async () => staleLoad.resolve({ id: 4, document: documentFor('Stale loaded body') }));
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
      setNoteReadOnly, listNoteItems: async () => [summary], getNote: async id => ({ id, document: documentFor('Old body') }) })} />);
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
      listNoteItems: async () => [summary], getNote: async id => ({ id, document: documentFor('Old body') }) })} />);
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
  const getNote = vi.fn(async id => ({ id, document: documentFor(body) }));
  render(<App client={fixtureClient({ generateSessionSummary: () => ready.promise,
    commitSessionSummary: async () => { body = 'Fresh annotations'; return { ...summary, read_only: true }; },
    listNoteItems: async () => [folder, annotation], getNote })} />);
  await startSummary();
  fireEvent.click(screen.getByRole('tab', { name: 'Notes' }));
  fireEvent.click(await screen.findByRole('treeitem', { name: 'Forum folder' }));
  fireEvent.click(await screen.findByRole('treeitem', { name: 'ANNOTATIONS' }));
  await screen.findByText('Old annotations');
  await act(async () => ready.resolve(generated));
  await screen.findByText('Fresh annotations');
  expect(getNote).toHaveBeenCalledTimes(2);
});
