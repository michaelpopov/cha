import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { useRef, useState } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  ChaError,
  type NoteContent,
  type NoteItem,
  type NotePage,
} from '../api/client';
import { pictureTypeMessage } from '../notes/pictures';
import { useNoteLeave } from '../notes/useNoteLeave';
import { noteSaveDelayMs, type NoteSaveHandle } from '../notes/useNoteSave';
import type { AppAction } from '../state/view';
import { fixtureClient } from '../test/fixtures';
import { VoiceInputSession } from '../voiceInput';
import { NotesScreen } from './NotesScreen';

const plan: NoteItem = { id: 4, parent_id: null, kind: 'note', name: 'Plan' };
const essay: NoteItem = { id: 5, parent_id: null, kind: 'note', name: 'Essay' };

const voiceRuntime = {
  provider: 'openai' as const,
  url: 'https://api.openai.com/v1/realtime/calls',
  model: 'gpt-live-transcribe',
  delay: 'low' as const,
  prompt: '',
  send_phrase: '',
};

function documentWith(text: string) {
  return {
    type: 'doc',
    content: [{ type: 'paragraph', content: text ? [{ type: 'text', text }] : [] }],
  };
}

function insertText(editor: Editor, text: string) {
  editor.view.dispatch(editor.state.tr.insertText(text));
}

let liveEditor: { current: Editor | null } | null = null;

function Harness({
  initialId = 4 as number | null,
  items = [plan],
  getNote = async (id: number) => ({ id, document: documentWith(id === 5 ? 'Essay text' : 'Alpha') }),
  saveNote = async () => undefined,
  readNotePage = async () => { throw new Error('Page reading is unavailable.'); },
  renameNoteItem = async (id: number, name: string) => ({ ...plan, id, name }),
  draft = '',
  voice = false,
  sendPhrase = '',
}: {
  initialId?: number | null;
  items?: NoteItem[];
  getNote?: (id: number) => Promise<NoteContent>;
  saveNote?: (id: number, document: Record<string, unknown>) => Promise<void>;
  readNotePage?: (url: string) => Promise<NotePage>;
  renameNoteItem?: (id: number, name: string) => Promise<NoteItem>;
  draft?: string;
  voice?: boolean;
  sendPhrase?: string;
}) {
  const [noteId, setNoteId] = useState(initialId);
  const [request, setRequest] = useState(1);
  const [text, setText] = useState(draft);
  const [left, setLeft] = useState(false);
  const editorLiveRef = useRef<Editor | null>(null);
  liveEditor = editorLiveRef;
  const saveRef = useRef<NoteSaveHandle | null>(null);
  const disposeRef = useRef<() => void>(() => undefined);
  const getNoteRef = useRef(getNote);
  const saveNoteRef = useRef(saveNote);
  getNoteRef.current = getNote;
  saveNoteRef.current = saveNote;
  const client = useRef(fixtureClient({
    getNote: (id) => getNoteRef.current(id),
    saveNote: (id, document) => saveNoteRef.current(id, document),
    readNotePage,
    renameNoteItem,
    getVoiceInputRuntime: async () => (voice ? { ...voiceRuntime, send_phrase: sendPhrase } : null),
  })).current;
  const leave = useNoteLeave(() => saveRef.current, {
    disposeEditor: () => disposeRef.current(),
  });
  function dispatch(action: AppAction) {
    if (action.type === 'open-note') {
      setNoteId(action.noteId);
      setRequest((current) => current + 1);
    }
  }
  return (
    <div>
      <button type="button" onClick={() => void leave.leaveNoteThen(() => setLeft(true))}>Leave</button>
      <button type="button" onClick={() => dispatch({ type: 'open-note', noteId: 5 })}>Open essay</button>
      {left && <p>Left the note</p>}
      <NotesScreen
        activeNoteId={noteId}
        client={client}
        dispatch={dispatch} navigate={dispatch}
        draft={text}
        editorLiveRef={editorLiveRef}
        leave={leave}
        noteItems={noteId === 5 ? [plan, essay] : items}
        noteRequest={request}
        notesEpoch={1}
        onDraftChange={setText}
        onRegisterDispose={(dispose) => { disposeRef.current = dispose; }}
        onRegisterSave={(save) => { saveRef.current = save; }}
        sidebarOpen
      />
    </div>
  );
}

afterEach(() => {
  liveEditor = null;
  vi.useRealTimers();
  vi.restoreAllMocks();
});

async function openedEditor(): Promise<Editor> {
  await act(async () => {
    await Promise.resolve();
    await Promise.resolve();
  });
  const editor = liveEditor?.current;
  if (!editor || editor.isDestroyed) throw new Error('The note editor is not available.');
  return editor;
}

describe('Notes screen', () => {
  it.each([
    [1, '##000001'],
    [10, '##00000A'],
    [35, '##00000Z'],
    [36, '##00000a'],
    [61, '##00000z'],
    [62, '##000010'],
    [3844, '##000100'],
    [56800235583, '##zzzzzz'],
  ])('copies note %s as the reference %s without editing the document', async (id, reference) => {
    const user = userEvent.setup();
    const writeText = vi.spyOn(navigator.clipboard, 'writeText').mockResolvedValue();
    const saveNote = vi.fn();
    render(<Harness initialId={id} items={[{ ...plan, id }]} saveNote={saveNote} />);
    const editor = await openedEditor();
    const document = editor.getJSON();

    await user.click(screen.getByRole('button', { name: 'Copy note reference' }));

    expect(writeText).toHaveBeenCalledExactlyOnceWith(reference);
    expect(editor.getJSON()).toEqual(document);
    expect(editor.can().undo()).toBe(false);
    expect(editor.view.dom).toHaveFocus();
    expect(saveNote).not.toHaveBeenCalled();
  });

  it('copies the current note after switching notes', async () => {
    const user = userEvent.setup();
    const writeText = vi.spyOn(navigator.clipboard, 'writeText').mockResolvedValue();
    render(<Harness />);
    await openedEditor();
    await user.click(screen.getByRole('button', { name: 'Copy note reference' }));
    expect(writeText).toHaveBeenLastCalledWith('##000004');

    await user.click(screen.getByRole('button', { name: 'Open essay' }));
    await openedEditor();
    await user.click(screen.getByRole('button', { name: 'Copy note reference' }));
    expect(writeText).toHaveBeenLastCalledWith('##000005');
  });

  it('reports a clipboard failure and allows retrying', async () => {
    const user = userEvent.setup();
    const writeText = vi.spyOn(navigator.clipboard, 'writeText')
      .mockRejectedValueOnce(new Error('Denied')).mockResolvedValue();
    render(<Harness />);
    await openedEditor();
    const button = screen.getByRole('button', { name: 'Copy note reference' });
    await user.click(button);
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not copy the note reference. Try again.');

    await user.click(button);
    expect(writeText).toHaveBeenLastCalledWith('##000004');
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('does not truncate IDs that cannot fit in six base62 characters', async () => {
    const user = userEvent.setup();
    const writeText = vi.spyOn(navigator.clipboard, 'writeText').mockResolvedValue();
    render(<Harness initialId={56800235584} />);
    await openedEditor();
    await user.click(screen.getByRole('button', { name: 'Copy note reference' }));

    expect(writeText).not.toHaveBeenCalled();
    expect(screen.getByRole('alert')).toHaveTextContent('This note ID cannot fit in a 6-character reference.');
  });

  it.each([
    ['New note', '', 'Title', true],
    ['New note 1', '', 'Title', true],
    ['New note 12', '', '  Title  ', true],
    ['My note', '', 'Title', false],
    ['New notebook', '', 'Title', false],
    ['New note draft', '', 'Title', false],
    ['New note', 'Existing text', 'Title', false],
    ['New note', '', '  ', false],
  ] as const)('names %s from the page only when empty with a default name (text=%s, title=%s)',
    async (name, text, title, shouldRename) => {
      const renameNoteItem = vi.fn(async (id: number, next: string) => ({ ...plan, id, name: next }));
      render(<Harness
        items={[{ ...plan, name }]}
        getNote={async (id) => ({ id, document: documentWith(text) })}
        readNotePage={async () => ({ url: 'https://example.org/', title, markdown: 'Article' })}
        renameNoteItem={renameNoteItem}
        draft="example.org"
      />);
      const editor = await openedEditor();
      fireEvent.click(screen.getByRole('button', { name: `Send to ${name}` }));
      await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
      expect(editor.getText()).toContain('Article');
      if (shouldRename) expect(renameNoteItem).toHaveBeenCalledWith(plan.id, 'Title');
      else expect(renameNoteItem).not.toHaveBeenCalled();
    },
  );

  it.each([
    [['Title'], 'Title', 'Title 1'],
    [['Title', 'Title 1', 'Title 3'], 'Title', 'Title 2'],
    [[], 'Title', 'Title'],
    [[], 'New note', 'New note'],
  ] as const)('chooses an available title among siblings %j', async (names, title, expected) => {
    const folder: NoteItem = { id: 3, parent_id: null, kind: 'folder', name: 'Folder' };
    const original = { ...plan, parent_id: folder.id, name: 'New note' };
    const renameNoteItem = vi.fn(async (id: number, name: string) => ({ ...original, id, name }));
    render(<Harness
      items={[folder, original,
        { id: 20, parent_id: null, kind: 'note', name: 'Title' },
        ...names.map((name, index) => ({ id: 30 + index, parent_id: folder.id, kind: 'note' as const, name })),
      ]}
      getNote={async (id) => ({ id, document: documentWith('') })}
      readNotePage={async () => ({ url: 'https://example.org/', title, markdown: 'Article' })}
      renameNoteItem={renameNoteItem}
      draft="example.org"
    />);
    const editor = await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Send to Folder / New note' }));
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
    expect(renameNoteItem).toHaveBeenCalledWith(plan.id, expected);
    expect(editor.getText({ blockSeparator: '\n' })).toBe(`${title}\nArticle`);
    expect(screen.queryByRole('alert')).toBeNull();
  });

  it.each(['name', 'body'])('keeps a %s changed while the page is loading', async (change) => {
    let finish!: (page: NotePage) => void;
    const readNotePage = vi.fn(() => new Promise<NotePage>((resolve) => { finish = resolve; }));
    const renameNoteItem = vi.fn();
    const props = {
      items: [{ ...plan, name: 'New note' }],
      getNote: async (id: number) => ({ id, document: documentWith('') }),
      readNotePage, renameNoteItem, draft: 'example.org',
    };
    const { rerender } = render(<Harness {...props} />);
    const editor = await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Send to New note' }));
    await waitFor(() => expect(readNotePage).toHaveBeenCalledOnce());
    if (change === 'name') rerender(<Harness {...props} items={[{ ...plan, name: 'My title' }]} />);
    else act(() => insertText(editor, 'Keep this'));
    await act(async () => finish({ url: 'https://example.org/', title: 'Title', markdown: 'Article' }));
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
    expect(renameNoteItem).not.toHaveBeenCalled();
    expect(editor.getText()).toContain('Article');
    if (change === 'body') expect(editor.getText()).toContain('Keep this');
  });

  it('keeps the imported article and clears the URL if automatic renaming fails', async () => {
    const renameNoteItem = vi.fn().mockRejectedValue(new ChaError('internal_error', 'The item could not be changed.'));
    render(<Harness
      items={[{ ...plan, name: 'New note' }]}
      getNote={async (id) => ({ id, document: documentWith('') })}
      readNotePage={async () => ({ url: 'https://example.org/', title: 'Title', markdown: 'Article' })}
      renameNoteItem={renameNoteItem}
      draft="example.org"
    />);
    const editor = await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Send to New note' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('The page was imported, but the note could not be renamed.');
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Title\nArticle');
    expect(screen.getByRole('button', { name: 'Send to New note' })).toBeInTheDocument();
  });

  it.each(['https://example.org/article', 'example.org/article'])(
    'imports %s at the end and keeps text typed while the fetch is pending', async (address) => {
      let finish!: (page: NotePage) => void;
      const readNotePage = vi.fn(() => new Promise<NotePage>((resolve) => { finish = resolve; }));
      render(<Harness readNotePage={readNotePage} />);
      const editor = await openedEditor();
      act(() => editor.commands.setTextSelection(2));
      const prompt = screen.getByRole('textbox', { name: 'Prompt' });
      fireEvent.change(prompt, { target: { value: address } });
      fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
      await waitFor(() => expect(readNotePage).toHaveBeenCalledWith(
        address.startsWith('https://') ? address : `http://${address}`,
      ));
      fireEvent.change(prompt, { target: { value: `${address} Next` } });
      await act(async () => finish({ url: 'https://example.org/article', title: 'Title', markdown: '**Article**' }));
      expect(editor.getText({ blockSeparator: '\n' })).toBe('Alpha\nTitle\nArticle');
      expect(editor.view.dom.querySelector('strong')).toHaveTextContent('Article');
      expect(prompt).toHaveValue('Next');
    },
  );

  it('keeps the URL on failure, dismisses the error, and retries with Send', async () => {
    const readNotePage = vi.fn()
      .mockRejectedValueOnce(new Error('Network details must stay hidden'))
      .mockResolvedValue({ url: 'https://example.org/', title: 'Title', markdown: 'Article' });
    render(<Harness readNotePage={readNotePage} draft="example.org" />);
    const editor = await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not retrieve the web page. Try again.');
    expect(editor.getText()).toBe('Alpha');
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    expect(prompt).toHaveValue('example.org');
    fireEvent.click(screen.getByRole('button', { name: 'Dismiss error' }));
    expect(screen.queryByRole('alert')).toBeNull();
    expect(prompt).toHaveValue('example.org');
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    await waitFor(() => expect(prompt).toHaveValue(''));
    expect(editor.getText()).toContain('Article');
  });

  it.each([false, true])('ignores a late URL result after switching notes (failure=%s)', async (fail) => {
    let finish!: (page: NotePage) => void;
    let reject!: (error: Error) => void;
    const readNotePage = vi.fn(() => new Promise<NotePage>((resolve, no) => { finish = resolve; reject = no; }));
    render(<Harness readNotePage={readNotePage} draft="https://example.org/" />);
    await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    await waitFor(() => expect(readNotePage).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    const essayEditor = await openedEditor();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'Keep this' } });
    await act(async () => {
      if (fail) reject(new Error('Failed'));
      else finish({ url: 'https://example.org/', title: 'Late', markdown: 'Article' });
    });
    expect(essayEditor.getText()).toBe('Essay text');
    expect(prompt).toHaveValue('Keep this');
    expect(screen.queryByRole('alert')).toBeNull();
  });

  it('keeps a URL with additional text on the literal insertion path', async () => {
    const readNotePage = vi.fn();
    render(<Harness readNotePage={readNotePage} draft="https://example.org/ read this" />);
    const editor = await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
    expect(readNotePage).not.toHaveBeenCalled();
    expect(editor.getText()).toContain('https://example.org/ read this');
  });

  it('opens a note, saves five seconds after an edit, and retries a save error', async () => {
    vi.useFakeTimers();
    const saveNote = vi.fn()
      .mockResolvedValueOnce(undefined)
      .mockRejectedValueOnce(new ChaError('invalid_argument', 'The vault is locked.'))
      .mockResolvedValue(undefined);
    render(<Harness saveNote={saveNote} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const editor = await openedEditor();
    expect(editor.isEditable).toBe(true);
    expect(editor.can().undo()).toBe(false);
    expect(screen.getByRole('button', { name: 'Send to Plan' })).toBeDisabled();
    act(() => insertText(editor, ' beta'));
    expect(saveNote).not.toHaveBeenCalled();
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).toHaveBeenCalledOnce();
    expect(JSON.stringify(saveNote.mock.calls[0][1])).toContain('Alpha beta');

    act(() => insertText(editor, '!'));
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(screen.getByRole('alert')).toHaveTextContent('Could not save “Plan”. The vault is locked.');
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(saveNote).toHaveBeenCalledTimes(3);
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('shows Retry for an opening failure and does not leave an editable document', async () => {
    const getNote = vi.fn()
      .mockRejectedValueOnce(new ChaError('not_found', 'The note is missing.'))
      .mockResolvedValueOnce({ id: 4, document: documentWith('Alpha') });
    render(<Harness getNote={getNote} />);
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not open “Plan”. The note is missing.');
    expect(document.querySelector('[contenteditable="true"]')).toBeNull();
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    expect(await screen.findByText('Alpha')).toBeInTheDocument();
    expect(getNote).toHaveBeenCalledTimes(2);
  });

  it('asks Retry, Stay, and Discard, and Escape stays', async () => {
    const saveNote = vi.fn(async () => {
      throw new ChaError('invalid_argument', 'The note could not be saved.');
    });
    render(<Harness saveNote={saveNote} />);
    const editor = await screen.findByText('Alpha').then(() => openedEditor());
    act(() => insertText(editor, ' beta'));
    fireEvent.click(screen.getByRole('button', { name: 'Leave' }));
    const dialog = await screen.findByRole('dialog', { name: 'Could not save “Plan”.' });
    expect(dialog).toHaveTextContent('Discard unsaved changes and continue?');
    expect(screen.getByRole('button', { name: 'Stay' })).toHaveFocus();
    fireEvent(dialog, new Event('cancel', { bubbles: true, cancelable: true }));
    await screen.findByText('Alpha beta');
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(screen.queryByText('Left the note')).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Leave' }));
    await screen.findByRole('dialog');
    fireEvent.click(screen.getByRole('button', { name: 'Discard' }));
    expect(await screen.findByText('Left the note')).toBeInTheDocument();
  });

  it('keeps the Notes draft and disables Send when no note is open', () => {
    render(<Harness initialId={null} items={[]} />);
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    const send = screen.getByRole('button', { name: 'Send' });
    expect(send).toBeDisabled();
    fireEvent.change(prompt, { target: { value: 'Keep this' } });
    fireEvent.click(send);
    fireEvent.keyDown(prompt, { key: 'Enter' });
    expect(prompt).toHaveValue('Keep this');
  });

  it('keeps history when the same open note is shown again', async () => {
    const getNote = vi.fn(async () => ({ id: 4, document: documentWith('Alpha') }));
    const view = render(<Harness getNote={getNote} />);
    const editor = await screen.findByText('Alpha').then(() => openedEditor());
    act(() => insertText(editor, ' beta'));
    expect(editor.can().undo()).toBe(true);
    view.rerender(<Harness getNote={getNote} />);
    expect(getNote).toHaveBeenCalledOnce();
    expect(screen.getByText('Alpha beta')).toBeInTheDocument();
    expect((await openedEditor()).can().undo()).toBe(true);
  });

  it('cancels voice work when the open note changes and keeps the draft', async () => {
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    const cancel = vi.fn();
    vi.spyOn(VoiceInputSession, 'start').mockResolvedValue({ stop: async () => undefined, cancel });
    const getNote = vi.fn(async (id: number) => ({
      id,
      document: documentWith(id === 5 ? 'Essay text' : 'Alpha'),
    }));
    render(<Harness draft="Spoken" getNote={getNote} voice />);
    await screen.findByText('Alpha');
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    expect(await screen.findByText('Essay text')).toBeInTheDocument();
    expect(cancel).toHaveBeenCalled();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Spoken');
    expect(screen.getByRole('button', { name: 'Send to Essay' })).toBeEnabled();
    expect((await openedEditor()).getText()).toBe('Essay text');
  });

  it('keeps Send disabled when no note is open', () => {
    render(<Harness draft="Keep this" initialId={null} items={[]} />);
    expect(screen.queryByRole('combobox', { name: 'Paragraph style' })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'New note' })).not.toBeInTheDocument();
    expect(screen.getByRole('button', { name: 'Send' })).toBeDisabled();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep this');
  });

  it('formats the retained selection and leaves the draft', async () => {
    render(<Harness />);
    const editor = await openedEditor();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'Keep' } });
    editor.commands.setTextSelection({ from: 1, to: 4 });
    function clickAction(name: string) {
      const control = screen.getByRole('button', { name });
      act(() => control.focus());
      fireEvent.click(control);
      expect(editor.view.dom).toHaveFocus();
    }
    expect(editor.can().undo()).toBe(false);
    clickAction('Heading 2');
    expect(editor.isActive('heading', { level: 2 })).toBe(true);
    expect(editor.can().undo()).toBe(true);
    clickAction('Bold');
    expect(editor.getJSON().content?.[0]?.content?.[0]?.marks?.[0]?.type).toBe('bold');
    expect(prompt).toHaveValue('Keep');
    expect(prompt).not.toHaveFocus();
    clickAction('Undo');
    expect(editor.isActive('bold')).toBe(false);
    clickAction('Undo');
    expect(editor.isActive('heading', { level: 2 })).toBe(false);
    expect(editor.can().undo()).toBe(false);
    expect(prompt).toHaveValue('Keep');
    clickAction('Redo');
    expect(editor.isActive('heading', { level: 2 })).toBe(true);

    fireEvent.click(screen.getByRole('button', { name: 'List' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Bullet list' }));
    expect(editor.view.dom).toHaveFocus();
    expect(editor.isActive('bulletList')).toBe(true);
    fireEvent.click(screen.getByRole('button', { name: 'List' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Numbered list' }));
    expect(editor.view.dom).toHaveFocus();
    expect(editor.isActive('orderedList')).toBe(true);
    expect(prompt).toHaveValue('Keep');
  });

  it.each([1, 2, 3])('toggles heading %s, preserves the selection, and returns focus', async (level) => {
    render(<Harness />);
    const editor = await openedEditor();
    act(() => editor.commands.setTextSelection({ from: 1, to: 4 }));
    expect(screen.queryByRole('combobox', { name: 'Paragraph style' })).not.toBeInTheDocument();
    const heading = screen.getByRole('button', { name: `Heading ${level}` });
    act(() => heading.focus());
    fireEvent.click(heading);
    expect(editor.isActive('heading', { level })).toBe(true);
    expect(heading).toHaveAttribute('aria-pressed', 'true');
    expect(editor.state.doc.textBetween(editor.state.selection.from, editor.state.selection.to)).toBe('Alp');
    expect(editor.view.dom).toHaveFocus();
    act(() => heading.focus());
    fireEvent.click(heading);
    expect(editor.isActive('paragraph')).toBe(true);
    expect(heading).toHaveAttribute('aria-pressed', 'false');
    expect(editor.view.dom).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'Undo' }));
    expect(editor.isActive('heading', { level })).toBe(true);
    expect(heading).toHaveAttribute('aria-pressed', 'true');
    fireEvent.click(screen.getByRole('button', { name: 'Redo' }));
    expect(editor.isActive('paragraph')).toBe(true);
  });

  it.each(['Bold', 'Italic', 'Underline', 'Highlight'])('returns focus after %s and keeps formatting for typing', async (name) => {
    render(<Harness />);
    const editor = await openedEditor();
    const control = screen.getByRole('button', { name });
    act(() => control.focus());
    fireEvent.click(control);
    expect(editor.view.dom).toHaveFocus();
    act(() => insertText(editor, '!'));
    expect(editor.getText()).toBe('Alpha!');
    expect(editor.getJSON().content?.[0]?.content?.at(-1)?.marks?.[0]?.type).toBe(name.toLowerCase());
  });

  it('returns focus after the picture picker is canceled', async () => {
    render(<Harness />);
    const editor = await openedEditor();
    const picture = screen.getByRole('button', { name: 'Picture' });
    act(() => picture.focus());
    fireEvent.click(picture);
    const input = document.querySelector<HTMLInputElement>('input[type="file"]');
    if (!input) throw new Error('Picture input is missing.');
    fireEvent(input, new Event('cancel'));
    expect(editor.view.dom).toHaveFocus();
    expect(editor.getText()).toBe('Alpha');
  });

  it('inserts literal prompts from Send and Enter and keeps later typing', async () => {
    render(<Harness />);
    const editor = await openedEditor();
    editor.commands.setTextSelection(3);
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    act(() => prompt.focus());
    fireEvent.change(prompt, { target: { value: 'One' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    fireEvent.change(prompt, { target: { value: 'One tail' } });
    await waitFor(() => expect(prompt).toHaveValue('tail'));
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Al\nOne\npha');
    expect(document.activeElement).not.toBe(editor.view.dom);

    fireEvent.change(prompt, { target: { value: 'ab' } });
    (prompt as HTMLTextAreaElement).setSelectionRange(1, 1);
    fireEvent.keyDown(prompt, { key: 'Enter', ctrlKey: true });
    expect(prompt).toHaveValue('a\nb');
    const composing = new KeyboardEvent('keydown', { key: 'Enter', bubbles: true, cancelable: true });
    Object.defineProperty(composing, 'isComposing', { value: true });
    prompt.dispatchEvent(composing);
    expect(prompt).toHaveValue('a\nb');
    fireEvent.click(screen.getByRole('button', { name: 'Latin to Russian transliteration' }));
    fireEvent.change(prompt, { target: { value: 'a\nbx' } });
    expect(prompt).toHaveValue('a\nbх');
    fireEvent.keyDown(prompt, { key: 'Enter' });
    await waitFor(() => expect(prompt).toHaveValue(''));
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Al\nOne\na\nbх\npha');
    expect(document.activeElement).toBe(prompt);
  });

  it('keeps one insertion when the later save fails', async () => {
    vi.useFakeTimers();
    const saveNote = vi.fn()
      .mockRejectedValueOnce(new ChaError('invalid_argument', 'The vault is locked.'))
      .mockResolvedValue(undefined);
    render(<Harness saveNote={saveNote} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const editor = await openedEditor();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'Hello' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    await act(async () => {
      await Promise.resolve();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(prompt).toHaveValue('');
    expect(editor.getText()).toContain('Hello');
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).toHaveBeenCalledOnce();
    expect(editor.getText().match(/Hello/g)).toHaveLength(1);
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(editor.getText().match(/Hello/g)).toHaveLength(1);
    expect(prompt).toHaveValue('');
  });

  it('sends a spoken prompt once and cancels it when the note changes', async () => {
    vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
    let appendVoice: (text: string, provisional?: boolean) => void = () => {};
    vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_configuration, onTranscription) => {
      appendVoice = onTranscription;
      return { stop: async () => undefined, cancel: vi.fn() };
    });
    const getNote = vi.fn(async (id: number) => ({
      id,
      document: documentWith(id === 5 ? 'Essay text' : 'Alpha'),
    }));
    render(<Harness getNote={getNote} sendPhrase="over to you" voice />);
    const editor = await openedEditor();
    fireEvent.click(await screen.findByRole('button', { name: 'Start voice input' }));
    await screen.findByRole('button', { name: 'Stop voice input' });
    act(() => appendVoice('Next line. Over to you'));
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1100)); });
    expect(editor.getText()).toContain('Next line.');
    expect(screen.getByRole('button', { name: 'Stop voice input' })).toBeEnabled();

    act(() => appendVoice(' Keep this over to you'));
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    expect(await screen.findByText('Essay text')).toBeInTheDocument();
    await act(async () => { await new Promise((resolve) => setTimeout(resolve, 1200)); });
    expect((await openedEditor()).getText()).toBe('Essay text');
  });

  it('inserts a picture and keeps a save error visible', async () => {
    vi.useFakeTimers();
    const saveNote = vi.fn(async () => {
      throw new ChaError('invalid_argument', 'The vault is locked.');
    });
    render(<Harness saveNote={saveNote} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const editor = await openedEditor();
    act(() => insertText(editor, ' beta'));
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(screen.getByRole('alert')).toHaveTextContent('Could not save “Plan”. The vault is locked.');
    vi.useRealTimers();

    const input = document.querySelector<HTMLInputElement>('input[type="file"]');
    if (!input) throw new Error('Picture input is missing.');
    const png = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    act(() => screen.getByRole('button', { name: 'Picture' }).focus());
    fireEvent.change(input, {
      target: { files: [new File([png], 'dot.png', { type: 'image/png' })] },
    });
    await waitFor(() => expect(JSON.stringify(editor.getJSON())).toContain('data:image/png'));
    expect(editor.view.dom).toHaveFocus();
    expect(screen.getByText(/Could not save “Plan”/)).toBeInTheDocument();
    expect(screen.queryByText(pictureTypeMessage)).not.toBeInTheDocument();

    act(() => screen.getByRole('button', { name: 'Picture' }).focus());
    fireEvent.change(input, {
      target: { files: [new File([Uint8Array.from([1, 2, 3, 4])], 'bad.png', { type: 'image/png' })] },
    });
    expect(await screen.findByText(pictureTypeMessage)).toBeInTheDocument();
    expect(editor.view.dom).toHaveFocus();
    expect(screen.getByText(/Could not save “Plan”/)).toBeInTheDocument();
    expect(screen.getAllByRole('alert').length).toBeGreaterThan(1);

    act(() => insertText(editor, '!'));
    expect(screen.queryByText(pictureTypeMessage)).not.toBeInTheDocument();
    expect(screen.getByText(/Could not save “Plan”/)).toBeInTheDocument();
  });

  it('finds, replaces, and keeps prompt Enter', async () => {
    render(<Harness />);
    const editor = await openedEditor();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    prompt.focus();
    fireEvent.keyDown(prompt, { key: 'f', metaKey: true });
    expect(screen.queryByRole('textbox', { name: 'Find' })).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Find' }));
    const find = await screen.findByRole('textbox', { name: 'Find' });
    expect(find).toHaveFocus();
    fireEvent.change(find, { target: { value: 'a' } });
    expect(screen.getByRole('status')).toHaveTextContent('1 / 2');
    act(() => screen.getByRole('button', { name: 'Next' }).focus());
    fireEvent.click(screen.getByRole('button', { name: 'Next' }));
    expect(editor.view.dom).toHaveFocus();
    expect(screen.getByRole('status')).toHaveTextContent('2 / 2');
    expect(editor.state.doc.textBetween(editor.state.selection.from, editor.state.selection.to)).toBe('a');
    fireEvent.keyDown(find, { key: 'Enter', shiftKey: true });
    expect(screen.getByRole('status')).toHaveTextContent('1 / 2');

    const replace = screen.getByRole('textbox', { name: 'Replace' });
    fireEvent.change(replace, { target: { value: '$1' } });
    act(() => replace.focus());
    fireEvent.keyDown(replace, { key: 'Enter' });
    expect(editor.view.dom).toHaveFocus();
    expect(editor.getText()).toBe('$1lpha');
    fireEvent.click(screen.getByRole('button', { name: 'Undo' }));
    expect(editor.getText()).toBe('Alpha');

    fireEvent.change(screen.getByRole('textbox', { name: 'Find' }), { target: { value: 'a' } });
    fireEvent.change(replace, { target: { value: '' } });
    act(() => screen.getByRole('button', { name: 'Replace all' }).focus());
    fireEvent.click(screen.getByRole('button', { name: 'Replace all' }));
    expect(editor.view.dom).toHaveFocus();
    expect(editor.getText()).toBe('lph');
    fireEvent.click(screen.getByRole('button', { name: 'Undo' }));
    expect(editor.getText()).toBe('Alpha');

    fireEvent.change(screen.getByRole('textbox', { name: 'Find' }), { target: { value: 'zzzz' } });
    expect(screen.getByRole('status')).toHaveTextContent('No matches');
    expect(screen.getByRole('button', { name: 'Next' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Replace' })).toBeDisabled();

    fireEvent.change(prompt, { target: { value: 'Ping' } });
    fireEvent.keyDown(prompt, { key: 'Enter' });
    await waitFor(() => expect(prompt).toHaveValue(''));
    expect(editor.getText()).toContain('Ping');
  });

  it('clears whole word in regex mode and resets Find on another note', async () => {
    render(<Harness />);
    const editor = await openedEditor();
    act(() => editor.commands.setContent('<p>cat catalog</p>'));
    fireEvent.click(screen.getByRole('button', { name: 'Find' }));
    const find = await screen.findByRole('textbox', { name: 'Find' });
    fireEvent.change(find, { target: { value: 'cat' } });
    fireEvent.click(screen.getByRole('checkbox', { name: 'Whole word' }));
    expect(screen.getByRole('status')).toHaveTextContent('1 / 1');
    fireEvent.click(screen.getByRole('checkbox', { name: 'Regular expression' }));
    expect(screen.getByRole('checkbox', { name: 'Whole word' })).not.toBeChecked();
    expect(screen.getByRole('checkbox', { name: 'Whole word' })).toBeDisabled();
    expect(screen.getByRole('status')).toHaveTextContent('1 / 2');
    fireEvent.click(screen.getByRole('checkbox', { name: 'Regular expression' }));
    expect(screen.getByRole('checkbox', { name: 'Whole word' })).toBeEnabled();
    expect(screen.getByRole('checkbox', { name: 'Whole word' })).not.toBeChecked();
    expect(screen.getByRole('status')).toHaveTextContent('1 / 2');

    expect(editor.view.dom.querySelector('.find-and-replace-result')).not.toBeNull();
    fireEvent.keyDown(find, { key: 'Escape' });
    expect(screen.queryByRole('textbox', { name: 'Find' })).not.toBeInTheDocument();
    expect(editor.view.dom.querySelector('.find-and-replace-result')).toBeNull();
    expect(editor.view.dom).toHaveFocus();
    fireEvent.click(screen.getByRole('button', { name: 'Find' }));
    expect(await screen.findByRole('textbox', { name: 'Find' })).toHaveValue('cat');

    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    expect(await screen.findByText('Essay text')).toBeInTheDocument();
    expect(screen.queryByRole('textbox', { name: 'Find' })).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Find' }));
    expect(await screen.findByRole('textbox', { name: 'Find' })).toHaveValue('');
  });

  it('does not save a search and does save a replacement', async () => {
    vi.useFakeTimers();
    const saveNote = vi.fn(async () => undefined);
    render(<Harness saveNote={saveNote} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const editor = await openedEditor();
    editor.view.dom.focus();
    fireEvent.keyDown(editor.view.dom, { key: 'а', code: 'KeyF', metaKey: true });
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const find = screen.getByRole('textbox', { name: 'Find' });
    fireEvent.change(find, { target: { value: 'Alpha' } });
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).not.toHaveBeenCalled();
    expect(editor.can().undo()).toBe(false);
    fireEvent.change(screen.getByRole('textbox', { name: 'Replace' }), { target: { value: 'Beta' } });
    fireEvent.click(screen.getByRole('button', { name: 'Replace' }));
    expect(editor.getText()).toBe('Beta');
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).toHaveBeenCalled();
  });

  it('edits and removes a link from the toolbar and the context menu', async () => {
    const openWindow = vi.spyOn(window, 'open').mockReturnValue(null);
    render(<Harness />);
    const editor = await openedEditor();
    expect(screen.getByRole('button', { name: 'Link' })).toBeDisabled();
    act(() => {
      editor.commands.setContent({
        type: 'doc',
        content: [{
          type: 'paragraph',
          content: [{
            type: 'text',
            text: 'docs',
            marks: [
              { type: 'bold' },
              { type: 'link', attrs: { href: 'https://example.com/a' } },
            ],
          }],
        }],
      });
      editor.commands.setTextSelection(2);
    });
    expect(screen.getByRole('button', { name: 'Link' })).toBeEnabled();
    fireEvent.click(screen.getByRole('button', { name: 'Link' }));
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    expect(screen.queryByRole('menuitem', { name: 'Edit link' })).not.toBeInTheDocument();
    expect(editor.view.dom).toHaveFocus();

    fireEvent.click(screen.getByRole('button', { name: 'Link' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Edit link' }));
    const dialog = await screen.findByRole('dialog', { name: 'Edit link' });
    const url = screen.getByRole('textbox', { name: 'URL' });
    fireEvent.change(url, { target: { value: 'mailto:user@example.com' } });
    fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    expect(screen.getByRole('alert')).toHaveTextContent('Enter an HTTP or HTTPS address.');
    expect(JSON.stringify(editor.getJSON())).toContain('https://example.com/a');
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(editor.view.dom).toHaveFocus();
    expect(JSON.stringify(editor.getJSON())).toContain('https://example.com/a');

    fireEvent.click(screen.getByRole('button', { name: 'Link' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Edit link' }));
    fireEvent.change(screen.getByRole('textbox', { name: 'URL' }), { target: { value: 'http://example.org/b' } });
    fireEvent.submit(screen.getByRole('textbox', { name: 'URL' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(editor.view.dom).toHaveFocus();
    expect(editor.getText()).toBe('docs');
    expect(JSON.stringify(editor.getJSON())).toContain('http://example.org/b');
    expect(JSON.stringify(editor.getJSON())).toContain('"type":"bold"');
    fireEvent.click(screen.getByRole('button', { name: 'Undo' }));
    expect(JSON.stringify(editor.getJSON())).toContain('https://example.com/a');

    const anchor = editor.view.dom.querySelector('a');
    if (!anchor) throw new Error('Link is missing.');
    fireEvent.contextMenu(anchor);
    expect(openWindow).not.toHaveBeenCalled();
    fireEvent.click(await screen.findByRole('menuitem', { name: 'Remove link' }));
    expect(editor.view.dom).toHaveFocus();
    expect(editor.getText()).toBe('docs');
    expect(editor.view.dom.querySelector('a')).toBeNull();
    expect(JSON.stringify(editor.getJSON())).toContain('"type":"bold"');
    fireEvent.click(screen.getByRole('button', { name: 'Undo' }));
    expect(editor.view.dom.querySelector('a')?.getAttribute('href')).toBe('https://example.com/a');
    fireEvent.click(screen.getByRole('button', { name: 'Link' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Remove link' }));
    expect(editor.view.dom).toHaveFocus();
    expect(editor.view.dom.querySelector('a')).toBeNull();
    expect(dialog).toBeTruthy();
  });
});
