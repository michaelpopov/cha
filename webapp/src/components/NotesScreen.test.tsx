import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { useRef, useState } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ChaError } from '../api/client';
import type { NoteContent, NoteItem } from '../api/nativeClient';
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
  draft = '',
  voice = false,
  sendPhrase = '',
}: {
  initialId?: number | null;
  items?: NoteItem[];
  getNote?: (id: number) => Promise<NoteContent>;
  saveNote?: (id: number, document: Record<string, unknown>) => Promise<void>;
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
