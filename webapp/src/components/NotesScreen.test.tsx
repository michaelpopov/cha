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
  type TextFileRequest,
} from '../api/client';
import { pictureTypeMessage } from '../notes/pictures';
import { noteHeadings, selectNoteEnd } from '../notes/editor';
import { useNoteLeave } from '../notes/useNoteLeave';
import { noteSaveDelayMs, type NoteSaveHandle } from '../notes/useNoteSave';
import type { AppAction } from '../state/view';
import { fixtureClient } from '../test/fixtures';
import { VoiceInputSession } from '../voiceInput';
import { NotesScreen } from './NotesScreen';

const plan: NoteItem = { id: 4, read_only: false, has_compressed: false, parent_id: null, kind: 'note', name: 'Plan' };
const essay: NoteItem = { id: 5, read_only: false, has_compressed: false, parent_id: null, kind: 'note', name: 'Essay' };

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
  saveNoteScrollPosition = async () => undefined,
  readNotePage = async () => { throw new Error('Page reading is unavailable.'); },
  readNoteTextFile = async () => { throw new Error('File reading is unavailable.'); },
  readNotePicture = async () => new Uint8Array(),
  renameNoteItem = async (id: number, name: string) => ({ ...plan, id, name }),
  draft = '',
  voice = false,
  sendPhrase = '',
}: {
  initialId?: number | null;
  items?: NoteItem[];
  getNote?: (id: number) => Promise<NoteContent>;
  saveNote?: (id: number, document: Record<string, unknown>) => Promise<void>;
  saveNoteScrollPosition?: (id: number, scrollTop: number) => Promise<void>;
  readNotePage?: (url: string) => Promise<NotePage>;
  readNoteTextFile?: (path: string) => Promise<TextFileRequest | null>;
  readNotePicture?: (url: string) => Promise<Uint8Array>;
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
    saveNoteScrollPosition,
    readNotePage,
    readNoteTextFile,
    readNotePicture,
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
        onSetNoteReadOnly={async (id, readOnly) => ({ ...plan, id, read_only: readOnly })}
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
  it('restores a note position and opens a note without one at the top', async () => {
    render(<Harness getNote={async (id) => ({
      id, document: documentWith('Reading'), ...(id === 4 ? { scroll_top: 680 } : {}),
    })} />);
    await openedEditor();
    const host = document.querySelector('.cha-notes-document')!;
    expect(host.scrollTop).toBe(680);
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    await waitFor(() => expect(liveEditor?.current?.getText()).toBe('Reading'));
    await waitFor(() => expect(host.scrollTop).toBe(0));
  });

  it('saves the latest reading position of an unchanged read-only note only when leaving', async () => {
    const saveNoteScrollPosition = vi.fn(async () => undefined);
    const saveNote = vi.fn(async () => undefined);
    render(<Harness items={[{ ...plan, read_only: true }]} saveNote={saveNote}
      saveNoteScrollPosition={saveNoteScrollPosition} />);
    const editor = await openedEditor();
    const host = document.querySelector('.cha-notes-document')!;
    host.scrollTop = 840;
    fireEvent.scroll(host);
    fireEvent.blur(editor.view.dom);
    expect(saveNoteScrollPosition).not.toHaveBeenCalled();
    host.scrollTop = 920;
    fireEvent.click(screen.getByRole('button', { name: 'Leave' }));
    await screen.findByText('Left the note');
    expect(saveNoteScrollPosition).toHaveBeenCalledExactlyOnceWith(4, 920);
    expect(saveNote).not.toHaveBeenCalled();
  });

  it.each([false, true])('restores after a picture loads unless the reader has moved (moved: %s)', async (moved) => {
    vi.spyOn(HTMLImageElement.prototype, 'complete', 'get').mockReturnValue(false);
    render(<Harness getNote={async (id) => ({
      id, scroll_top: 680, document: { type: 'doc', content: [
        { type: 'image', attrs: { src: 'data:image/png;base64,iVBORw0KGgo=' } },
        ...documentWith('Reading').content,
      ] },
    })} />);
    const editor = await openedEditor();
    const host = document.querySelector('.cha-notes-document')!;
    const picture = editor.view.dom.querySelector('img')!;
    host.scrollTop = 100;
    if (moved) fireEvent.wheel(host);
    fireEvent.load(picture);
    expect(host.scrollTop).toBe(moved ? 100 : 680);
  });

  it('toggles code on the selected text and supports undo and redo', async () => {
    const user = userEvent.setup();
    render(<Harness getNote={async (id) => ({ id, document: documentWith('Alpha beta') })} />);
    const editor = await openedEditor();
    act(() => { editor.commands.setTextSelection({ from: 7, to: 11 }); });
    const code = screen.getByRole('button', { name: 'Code' });
    expect(screen.queryByRole('button', { name: 'Link' })).not.toBeInTheDocument();
    await user.click(code);
    expect(code).toHaveAttribute('aria-pressed', 'true');
    expect(editor.view.dom.querySelector('code')?.textContent).toBe('beta');
    expect(editor.getText()).toBe('Alpha beta');
    expect(editor.view.dom).toHaveFocus();
    await user.click(screen.getByRole('button', { name: 'Undo' }));
    expect(editor.view.dom.querySelector('code')).toBeNull();
    await user.click(screen.getByRole('button', { name: 'Redo' }));
    expect(editor.view.dom.querySelector('code')?.textContent).toBe('beta');
    await user.click(code);
    expect(code).toHaveAttribute('aria-pressed', 'false');
    expect(editor.view.dom.querySelector('code')).toBeNull();
    expect(editor.getText()).toBe('Alpha beta');
  });

  it('resizes the whole document without formatting the selected text or saving a change', async () => {
    const user = userEvent.setup();
    const saveNote = vi.fn();
    render(<Harness saveNote={saveNote} />);
    const editor = await openedEditor();
    const document = editor.getJSON();
    act(() => { editor.commands.setTextSelection({ from: 2, to: 4 }); });
    const host = editor.view.dom.closest('.cha-notes-document');

    await user.click(screen.getByRole('button', { name: 'Increase editor font size' }));
    expect(host).toHaveStyle({ fontSize: '16px' });
    expect(editor.state.selection.from).toBe(2);
    expect(editor.state.selection.to).toBe(4);
    await user.click(screen.getByRole('button', { name: 'Decrease editor font size' }));
    expect(host).toHaveStyle({ fontSize: '14px' });
    expect(editor.getJSON()).toEqual(document);
    expect(editor.can().undo()).toBe(false);
    expect(saveNote).not.toHaveBeenCalled();
  });

  it('keeps note actions out of the editor toolbar', async () => {
    render(<Harness />);
    await openedEditor();
    for (const name of ['Rename', 'Delete', 'Copy note reference', 'Copy Ref ID']) {
      expect(screen.queryByRole('button', { name })).not.toBeInTheDocument();
    }
  });

  it('replaces a note from the picker, keeps the prompt, saves, and undoes in one step', async () => {
    const saveNote = vi.fn(async () => undefined);
    const renameNoteItem = vi.fn();
    render(<Harness draft="Keep this draft" saveNote={saveNote} renameNoteItem={renameNoteItem} />);
    const editor = await openedEditor();
    const input = screen.getByLabelText('Text file');
    const click = vi.spyOn(input, 'click');
    fireEvent.click(screen.getByRole('button', { name: 'Load text file' }));
    expect(click).toHaveBeenCalledOnce();
    fireEvent.change(input, { target: { files: [new File(['# Imported\n\n**Body**'], 'plan.md')] } });
    await waitFor(() => expect(editor.getText({ blockSeparator: '\n' })).toBe('Imported\nBody'));
    expect(editor.view.dom.querySelector('strong')?.textContent).toBe('Body');
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep this draft');
    expect(renameNoteItem).not.toHaveBeenCalled();
    await waitFor(() => expect(screen.getByRole('button', { name: 'Undo' })).toBeEnabled());
    fireEvent.click(screen.getByRole('button', { name: 'Undo' }));
    expect(editor.getText()).toBe('Alpha');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Redo' })).toBeEnabled());
    fireEvent.click(screen.getByRole('button', { name: 'Redo' }));
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Imported\nBody');
    const document = editor.getJSON();
    fireEvent.click(screen.getByRole('button', { name: 'Leave' }));
    await waitFor(() => expect(saveNote).toHaveBeenCalledWith(plan.id, document));
  });

  it.each(['New note', 'New note 12', 'My note', 'New note draft'])(
    'imports a path and renames %s only if it has a default name, even with existing content', async (name) => {
      const renameNoteItem = vi.fn(async (id: number, name: string) => ({ ...plan, id, name }));
      const readNoteTextFile = vi.fn(async () => ({ name: '/tmp/plan with spaces.md', text: '# Title\n\nNew content' }));
      render(<Harness
        items={[{ ...plan, name }, { ...essay, name: 'Title' }]}
        readNoteTextFile={readNoteTextFile} renameNoteItem={renameNoteItem}
        draft='  "/tmp/plan with spaces.md"  '
      />);
      const editor = await openedEditor();
      fireEvent.keyDown(screen.getByRole('textbox', { name: 'Prompt' }), { key: 'Enter' });
      await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
      expect(readNoteTextFile).toHaveBeenCalledWith('/tmp/plan with spaces.md');
      expect(editor.getText({ blockSeparator: '\n' })).toBe('Title\nNew content');
      if (/^New note(?: \d+)?$/u.test(name)) expect(renameNoteItem).toHaveBeenCalledWith(plan.id, 'Title 1');
      else expect(renameNoteItem).not.toHaveBeenCalled();
    },
  );

  it('keeps plain text literal and leaves a default name when there is no title', async () => {
    const renameNoteItem = vi.fn();
    render(<Harness items={[{ ...plan, name: 'New note' }]} renameNoteItem={renameNoteItem} />);
    const editor = await openedEditor();
    fireEvent.change(screen.getByLabelText('Text file'), {
      target: { files: [new File(['<b>Literal</b>\r\n**Text**\n    indented\tline  end'], 'input.txt')] },
    });
    await waitFor(() => expect(editor.getText()).toBe('<b>Literal</b>\n**Text**\n    indented\tline  end'));
    expect(editor.view.dom.querySelector('strong')).toBeNull();
    expect(renameNoteItem).not.toHaveBeenCalled();
  });

  it.each([
    ['empty.txt', ' \n', 'The file is empty.'],
    ['binary.txt', 'a\0b', 'Choose a UTF-8 text file.'],
    ['invalid.txt', new Uint8Array([0xff, 0xfe]), 'Choose a UTF-8 text file.'],
    ['large.txt', 'x'.repeat(256 * 1024 + 1), 'This file is too large to load into a note. The limit is 256 KiB of text.'],
    ['comment.md', '<!-- Only a comment -->', 'The file has no readable text.'],
    ['picture.md', '![Missing](https://example.org/missing.png)', 'The file has no readable text.'],
    ['list-picture.md', '- ![Missing](https://example.org/missing.png)', 'The file has no readable text.'],
    ['empty-picture.md', '<p></p>\n\n![Missing](https://example.org/missing.png)', 'The file has no readable text.'],
  ] as const)('preserves the note and prompt for rejected picker file %s', async (name, content, message) => {
    const file = new File([content], name);
    render(<Harness draft="Keep this" />);
    const editor = await openedEditor();
    fireEvent.change(screen.getByLabelText('Text file'), { target: { files: [file] } });
    expect(await screen.findByRole('alert')).toHaveTextContent(message);
    expect(editor.getText()).toBe('Alpha');
    expect(editor.can().undo()).toBe(false);
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep this');
  });

  it('accepts a picture-only Markdown file when the picture loads', async () => {
    const readNotePicture = vi.fn(async () => new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]));
    render(<Harness readNotePicture={readNotePicture} />);
    const editor = await openedEditor();
    fireEvent.change(screen.getByLabelText('Text file'), {
      target: { files: [new File(['![Photo](https://example.org/photo.png)'], 'picture.md')] },
    });
    await waitFor(() => expect(editor.view.dom.querySelector('img')).toHaveAttribute('src', 'data:image/png;base64,iVBORw0KGgo='));
    expect(readNotePicture).toHaveBeenCalledWith('https://example.org/photo.png');
    expect(editor.getText()).not.toContain('Alpha');
    expect(screen.queryByRole('alert')).toBeNull();
  });

  it('keeps a failed path, permits retry, and keeps text typed during the import', async () => {
    let finish!: (file: TextFileRequest) => void;
    const readNoteTextFile = vi.fn()
      .mockRejectedValueOnce(new ChaError('invalid_argument', 'That file was not found.'))
      .mockImplementationOnce(() => new Promise<TextFileRequest>((resolve) => { finish = resolve; }));
    render(<Harness draft="/tmp/plan.md" readNoteTextFile={readNoteTextFile} />);
    const editor = await openedEditor();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.keyDown(prompt, { key: 'Enter' });
    expect(await screen.findByRole('alert')).toHaveTextContent('That file was not found.');
    expect(prompt).toHaveValue('/tmp/plan.md');
    expect(editor.getText()).toBe('Alpha');
    await waitFor(() => expect(screen.getByRole('button', { name: 'Send to Plan' })).toBeEnabled());
    fireEvent.keyDown(prompt, { key: 'Enter' });
    await waitFor(() => expect(readNoteTextFile).toHaveBeenCalledTimes(2));
    fireEvent.change(prompt, { target: { value: '/tmp/plan.md Next' } });
    await act(async () => finish({ name: '/tmp/plan.md', text: '# Title\n\nBody' }));
    await waitFor(() => expect(prompt).toHaveValue('Next'));
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Title\nBody');
    expect(screen.queryByRole('alert')).toBeNull();
  });

  it('clears a successful path when the file already matches the note', async () => {
    render(<Harness draft="/tmp/plan.txt" readNoteTextFile={async () => ({ name: '/tmp/plan.txt', text: 'Alpha' })} />);
    const editor = await openedEditor();
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Prompt' }), { key: 'Enter' });
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
    expect(editor.getText()).toBe('Alpha');
    expect(screen.queryByRole('alert')).toBeNull();
  });

  it('keeps imported file content and clears the path if automatic renaming fails', async () => {
    const renameNoteItem = vi.fn().mockRejectedValue(new ChaError('internal_error', 'The item could not be changed.'));
    render(<Harness items={[{ ...plan, name: 'New note' }]} draft="/tmp/plan.md"
      readNoteTextFile={async () => ({ name: '/tmp/plan.md', text: '# Title\n\nBody' })}
      renameNoteItem={renameNoteItem} />);
    const editor = await openedEditor();
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Prompt' }), { key: 'Enter' });
    expect(await screen.findByRole('alert')).toHaveTextContent('The file was imported, but the note could not be renamed.');
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Title\nBody');
  });

  it('keeps ordinary prompts that start with a path', async () => {
    const readNoteTextFile = vi.fn(async () => null);
    render(<Harness draft="/tmp/build.log fails, why?" readNoteTextFile={readNoteTextFile} />);
    const editor = await openedEditor();
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Prompt' }), { key: 'Enter' });
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
    expect(editor.getText()).toContain('Alpha');
    expect(editor.getText()).toContain('/tmp/build.log fails, why?');
  });

  it('does not import into another note after switching while a path loads', async () => {
    let finish!: (file: TextFileRequest) => void;
    const readNoteTextFile = vi.fn(() => new Promise<TextFileRequest>((resolve) => { finish = resolve; }));
    const renameNoteItem = vi.fn();
    render(<Harness items={[{ ...plan, name: 'New note' }]} draft="/tmp/plan.md"
      readNoteTextFile={readNoteTextFile} renameNoteItem={renameNoteItem} />);
    await openedEditor();
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Prompt' }), { key: 'Enter' });
    await waitFor(() => expect(readNoteTextFile).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    const editor = await openedEditor();
    await act(async () => finish({ name: '/tmp/plan.md', text: '# Late' }));
    expect(editor.getText()).toBe('Essay text');
    expect(renameNoteItem).not.toHaveBeenCalled();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('/tmp/plan.md');
    expect(screen.getByRole('button', { name: 'Load text file' })).toBeEnabled();
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
        draft="https://example.org"
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
    const folder: NoteItem = { id: 3, read_only: false, has_compressed: false, parent_id: null, kind: 'folder', name: 'Folder' };
    const original = { ...plan, parent_id: folder.id, name: 'New note' };
    const renameNoteItem = vi.fn(async (id: number, name: string) => ({ ...original, id, name }));
    render(<Harness
      items={[folder, original,
        { id: 20, read_only: false, has_compressed: false, parent_id: null, kind: 'note', name: 'Title' },
        ...names.map((name, index) => ({ id: 30 + index, read_only: false, has_compressed: false, parent_id: folder.id, kind: 'note' as const, name })),
      ]}
      getNote={async (id) => ({ id, document: documentWith('') })}
      readNotePage={async () => ({ url: 'https://example.org/', title, markdown: 'Article' })}
      renameNoteItem={renameNoteItem}
      draft="https://example.org"
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
      readNotePage, renameNoteItem, draft: 'https://example.org',
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
      draft="https://example.org"
    />);
    const editor = await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Send to New note' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('The page was imported, but the note could not be renamed.');
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
    expect(editor.getText({ blockSeparator: '\n' })).toBe('Title\nArticle');
    expect(screen.getByRole('button', { name: 'Send to New note' })).toBeInTheDocument();
  });

  it.each(['https://example.org/article', 'www.example.org/article'])(
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

  it('uses the picture client for both page imports and HTML paste', async () => {
    const bytes = new Uint8Array([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    const readNotePicture = vi.fn(async () => bytes);
    render(<Harness
      readNotePage={async () => ({ url: 'https://example.org/article', title: 'Title',
        markdown: 'Before\n\n![Photo](/page.png)\n\nAfter' })}
      readNotePicture={readNotePicture}
      draft="https://example.org/article"
    />);
    const editor = await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    await waitFor(() => expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(''));
    expect(readNotePicture).toHaveBeenCalledWith('https://example.org/page.png');
    expect(editor.view.dom.querySelectorAll('img[src]')).toHaveLength(1);
    act(() => {
      selectNoteEnd(editor);
      editor.view.pasteHTML('<p>Pasted</p><img src="https://example.org/paste.png">',
        new Event('paste') as ClipboardEvent);
    });
    await waitFor(() => expect(editor.view.dom.querySelectorAll('img[src]')).toHaveLength(2));
    expect(readNotePicture).toHaveBeenCalledWith('https://example.org/paste.png');
    expect(editor.getText()).toContain('Before');
    expect(editor.getText()).toContain('After');
    expect(editor.getText()).toContain('Pasted');
  });

  it('keeps the URL on failure, dismisses the error, and retries with Send', async () => {
    const readNotePage = vi.fn()
      .mockRejectedValueOnce(new Error('Network details must stay hidden'))
      .mockResolvedValue({ url: 'https://example.org/', title: 'Title', markdown: 'Article' });
    render(<Harness readNotePage={readNotePage} draft="https://example.org" />);
    const editor = await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not retrieve the web page. Try again.');
    expect(editor.getText()).toBe('Alpha');
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    expect(prompt).toHaveValue('https://example.org');
    fireEvent.click(screen.getByRole('button', { name: 'Dismiss error' }));
    expect(screen.queryByRole('alert')).toBeNull();
    expect(prompt).toHaveValue('https://example.org');
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
    // The note text appears one render before the opened editor enables Send.
    await waitFor(() => expect(screen.getByRole('button', { name: 'Send to Essay' })).toBeEnabled());
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

  it('jumps to the selected duplicate heading, focuses and scrolls it, without saving', async () => {
    const saveNote = vi.fn();
    render(<Harness saveNote={saveNote} getNote={async (id) => ({
      id, document: {
        type: 'doc', content: [
          { type: 'heading', attrs: { level: 1 }, content: [{ type: 'text', text: 'Repeat' }] },
          { type: 'paragraph', content: [{ type: 'text', text: 'Body' }] },
          { type: 'heading', attrs: { level: 3 }, content: [{ type: 'text', text: 'Repeat' }] },
        ],
      },
    })} />);
    const editor = await openedEditor();
    const before = editor.getJSON();
    const target = noteHeadings(editor.state.doc)[1];
    const element = editor.view.nodeDOM(target.position) as HTMLElement;
    const scroll = vi.fn(() => {
      expect(editor.view.dom).toHaveFocus();
      expect(editor.state.selection.from).toBe(target.position + 1);
    });
    element.scrollIntoView = scroll;
    vi.useFakeTimers();
    fireEvent.click(screen.getByRole('button', { name: 'Jump to heading' }));
    const entries = screen.getAllByRole('menuitem', { name: 'Repeat' });
    expect(entries[0]).toHaveFocus();
    fireEvent.click(entries[1]);
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(scroll).toHaveBeenCalledExactlyOnceWith({ block: 'start', inline: 'nearest' });
    expect(editor.getJSON()).toEqual(before);
    expect(editor.can().undo()).toBe(false);
    await act(async () => { vi.advanceTimersByTime(noteSaveDelayMs + 1); });
    expect(saveNote).not.toHaveBeenCalled();
  });

  it('supports heading menu keys, Escape, and outside-pointer dismissal', async () => {
    const user = userEvent.setup();
    render(<Harness />);
    const editor = await openedEditor();
    act(() => editor.commands.setContent('<h1>First</h1><h2>Second</h2><h3>Third</h3>'));
    const jump = screen.getByRole('button', { name: 'Jump to heading' });
    act(() => jump.focus());
    await user.keyboard('{Enter}');
    expect(jump).toHaveAttribute('aria-expanded', 'true');
    const entries = screen.getAllByRole('menuitem');
    expect(entries[0]).toHaveFocus();
    await user.keyboard('{ArrowDown}');
    expect(entries[1]).toHaveFocus();
    await user.keyboard('{End}');
    expect(entries[2]).toHaveFocus();
    await user.keyboard('{Home}{ArrowUp}');
    expect(entries[2]).toHaveFocus();
    const target = editor.view.dom.querySelector('h3')!;
    const scroll = vi.fn();
    target.scrollIntoView = scroll;
    await user.keyboard(' ');
    expect(scroll).toHaveBeenCalledOnce();
    expect(editor.view.dom).toHaveFocus();
    fireEvent.click(jump);
    await user.keyboard('{Escape}');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(jump).toHaveAttribute('aria-expanded', 'false');
    expect(editor.view.dom).toHaveFocus();
    fireEvent.click(jump);
    await user.click(screen.getByRole('textbox', { name: 'Prompt' }));
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveFocus();
  });

  it('derives the heading menu after edits and Undo, and hides it while frozen', async () => {
    let finishSave!: () => void;
    render(<Harness saveNote={() => new Promise<void>((resolve) => { finishSave = resolve; })} getNote={async (id) => ({
      id, document: {
        type: 'doc', content: ['First', 'Second', 'Third'].map((text, index) => ({
          type: 'heading', attrs: { level: index + 1 }, content: [{ type: 'text', text }],
        })),
      },
    })} />);
    const editor = await openedEditor();
    const jump = screen.getByRole('button', { name: 'Jump to heading' });
    expect(jump).toBeEnabled();
    act(() => editor.view.dispatch(editor.state.tr.insertText('Renamed', 1, 6)));
    fireEvent.click(jump);
    expect(screen.getAllByRole('menuitem').map((entry) => entry.textContent)).toEqual(['Renamed', 'Second', 'Third']);
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    act(() => editor.commands.undo());
    fireEvent.click(jump);
    expect(screen.getAllByRole('menuitem').map((entry) => entry.textContent)).toEqual(['First', 'Second', 'Third']);
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    act(() => {
      const { doc } = editor.state;
      const third = doc.child(2);
      editor.view.dispatch(editor.state.tr.delete(doc.content.size - third.nodeSize, doc.content.size).insert(0, third));
    });
    fireEvent.click(jump);
    expect(screen.getAllByRole('menuitem').map((entry) => entry.textContent)).toEqual(['Third', 'First', 'Second']);
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    act(() => {
      const { doc } = editor.state;
      const start = doc.child(0).nodeSize;
      editor.view.dispatch(editor.state.tr.delete(start, start + doc.child(1).nodeSize));
    });
    fireEvent.click(jump);
    expect(screen.getAllByRole('menuitem').map((entry) => entry.textContent)).toEqual(['Third', 'Second']);
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    act(() => editor.view.dispatch(editor.state.tr.setNodeMarkup(0, editor.schema.nodes.paragraph)));
    fireEvent.click(jump);
    expect(screen.getAllByRole('menuitem').map((entry) => entry.textContent)).toEqual(['Second']);
    fireEvent.click(screen.getByRole('button', { name: 'Leave' }));
    expect(editor.isEditable).toBe(false);
    expect(jump).toBeDisabled();
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    await waitFor(() => expect(finishSave).toBeTypeOf('function'));
    await act(async () => { finishSave(); });
    expect(screen.getByText('Left the note')).toBeInTheDocument();
  });

  it('disables heading navigation when all headings are blank or removed', async () => {
    render(<Harness />);
    const editor = await openedEditor();
    const jump = screen.getByRole('button', { name: 'Jump to heading' });
    expect(jump).toBeDisabled();
    act(() => editor.commands.setContent('<h2> \u00a0 </h2>'));
    expect(jump).toBeDisabled();
    act(() => editor.commands.setContent('<h2>Section</h2>'));
    expect(jump).toBeEnabled();
    fireEvent.click(jump);
    act(() => editor.commands.setContent('<p>Section</p>'));
    expect(jump).toBeDisabled();
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    act(() => editor.view.focus());
    act(() => editor.commands.setContent('<h2>Section</h2>'));
    expect(editor.view.dom).toHaveFocus();
    expect(jump).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
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

  it('edits and removes a link from the context menu', async () => {
    const openWindow = vi.spyOn(window, 'open').mockReturnValue(null);
    render(<Harness />);
    const editor = await openedEditor();
    const openLinkMenu = () => {
      const anchor = editor.view.dom.querySelector('a');
      if (!anchor) throw new Error('Link is missing.');
      fireEvent.contextMenu(anchor);
    };
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
    openLinkMenu();
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    expect(screen.queryByRole('menuitem', { name: 'Edit link' })).not.toBeInTheDocument();
    openLinkMenu();
    fireEvent.click(screen.getByRole('menuitem', { name: 'Edit link' }));
    await screen.findByRole('dialog', { name: 'Edit link' });
    const url = screen.getByRole('textbox', { name: 'URL' });
    fireEvent.change(url, { target: { value: 'mailto:user@example.com' } });
    fireEvent.click(screen.getByRole('button', { name: 'Apply' }));
    expect(screen.getByRole('alert')).toHaveTextContent('Enter an HTTP or HTTPS address.');
    expect(JSON.stringify(editor.getJSON())).toContain('https://example.com/a');
    fireEvent.click(screen.getByRole('button', { name: 'Cancel' }));
    expect(screen.queryByRole('dialog')).not.toBeInTheDocument();
    expect(editor.view.dom).toHaveFocus();
    expect(JSON.stringify(editor.getJSON())).toContain('https://example.com/a');

    openLinkMenu();
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
  });
});
