import type { AnnotationDestination } from '../notes/annotationSource';
import type { Editor } from '@tiptap/core';
import { act, fireEvent, render, screen, waitFor } from '@testing-library/react';
import userEvent from '@testing-library/user-event';
import { StrictMode, useLayoutEffect, useRef, useState, type ReactNode } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  ChaError,
  type NoteContent,
  type NoteItem,
  type NotePage,
  type NotesClient,
  type TextFileRequest,
} from '../api/client';
import { pictureTypeMessage } from '../notes/pictures';
import { noteHeadings, selectNoteEnd, withIsolatedHistory } from '../notes/editor';
import { noteHighlights } from '../notes/highlights';
import { useNoteLeave } from '../notes/useNoteLeave';
import { useNoteImports } from '../notes/useNoteImports';
import { noteSaveDelayMs, type NoteSaveHandle } from '../notes/useNoteSave';
import type { AppAction } from '../state/view';
import { fixtureClient } from '../test/fixtures';
import { noteItem } from '../test/notes';
import { NotesScreen } from './NotesScreen';
import { TransliterationProvider } from './TransliterationMode';

const plan: NoteItem = noteItem({ id: 4, name: 'Plan' });
const essay: NoteItem = noteItem({ id: 5, name: 'Essay' });

function documentWith(text: string) {
  return {
    type: 'doc',
    content: [{ type: 'paragraph', content: text ? [{ type: 'text', text }] : [] }],
  };
}

function insertText(editor: Editor, text: string) {
  editor.view.dispatch(editor.state.tr.insertText(text));
}

function historyAction(name: 'Undo' | 'Redo') {
  fireEvent.click(screen.getByRole('button', { name: 'History' }));
  return screen.getByRole('menuitem', { name });
}

let liveEditor: { current: Editor | null } | null = null;

function Harness({
  initialId = 4 as number | null,
  destination,
  importUrl,
  importPath,
  promptVisible = true,
  onNavigate = () => undefined,
  onSetNoteReadOnly = async (id: number, readOnly: boolean) => ({ ...plan, id, read_only: readOnly }),
  items = [plan],
  getNote = async (id: number) => ({ id, document_id: id, document: documentWith(id === 5 ? 'Essay text' : 'Alpha') }),
  saveNote = async () => undefined,
  saveNoteScrollPosition = async () => undefined,
  readNotePage = async () => { throw new Error('Page reading is unavailable.'); },
  readNoteTextFile = async () => { throw new Error('File reading is unavailable.'); },
  readNotePicture = async () => new Uint8Array(),
  renameNoteItem = async (id: number, name: string) => ({ ...plan, id, name }),
  draft = '',
  after,
  extraRequest = 0,
}: {
  initialId?: number | null;
  destination?: AnnotationDestination;
  importUrl?: string;
  importPath?: string;
  promptVisible?: boolean;
  onNavigate?: (action: AppAction) => void;
  onSetNoteReadOnly?: (id: number, readOnly: boolean) => Promise<NoteItem>;
  items?: NoteItem[];
  getNote?: (id: number) => Promise<NoteContent>;
  saveNote?: (id: number, document: Record<string, unknown>) => Promise<void>;
  saveNoteScrollPosition?: (id: number, scrollTop: number) => Promise<void>;
  readNotePage?: NotesClient['readNotePage'];
  readNoteTextFile?: (path: string) => Promise<TextFileRequest | null>;
  readNotePicture?: (url: string) => Promise<Uint8Array>;
  renameNoteItem?: (id: number, name: string) => Promise<NoteItem>;
  draft?: string;
  after?: (noteId: number | null) => ReactNode;
  extraRequest?: number;
}) {
  const [noteId, setNoteId] = useState(initialId);
  const [request, setRequest] = useState(1);
  const [text, setText] = useState(draft);
  const [left, setLeft] = useState(false);
  const editorLiveRef = useRef<Editor | null>(null);
  liveEditor = editorLiveRef;
  const saveRef = useRef<NoteSaveHandle | null>(null);
  const documentIdRef = useRef<number | null>(null);
  const noteLoadRef = useRef<Promise<void> | null>(null);
  const disposeRef = useRef<() => void>(() => undefined);
  const getNoteRef = useRef(getNote);
  const saveNoteRef = useRef(saveNote);
  getNoteRef.current = getNote;
  saveNoteRef.current = saveNote;
  const itemsRef = useRef(items);
  itemsRef.current = items.some(item => item.id === essay.id) ? items : [...items, essay];
  const client = useRef(fixtureClient({
    listNoteItems: async () => itemsRef.current,
    setNoteReadOnly: onSetNoteReadOnly,
    getNote: (id) => getNoteRef.current(id),
    saveNote: (id, document) => saveNoteRef.current(id, document),
    saveNoteScrollPosition,
    readNotePage,
    readNoteTextFile,
    readNotePicture,
    renameNoteItem,
    getVoiceInputRuntime: async () => null,
  })).current;
  const leave = useNoteLeave(() => saveRef.current, {
    disposeEditor: () => disposeRef.current(),
  });
  const imports = useNoteImports({
    client, epoch: 1, leave,
    openNote: () => ({ noteId, documentId: documentIdRef.current, editor: editorLiveRef.current, save: saveRef.current }),
    waitForOpen: () => noteLoadRef.current,
    updated: () => undefined,
    refresh: () => undefined,
  });
  function dispatch(action: AppAction) {
    onNavigate(action);
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
        destination={destination}
        importSource={noteId === initialId ? importPath ? { kind: 'file', path: importPath } : importUrl ? { kind: 'page', url: importUrl } : undefined : undefined}
        promptVisible={promptVisible}
        client={client}
        navigate={dispatch}
        draft={text}
        editorLiveRef={editorLiveRef}
        leave={leave}
        noteItems={noteId === 5 ? [plan, essay] : items}
        noteRequest={request + extraRequest}
        noteTextMode="full_text"
        noteActionPending={leave.pendingKind !== null}
        onNoteTextModeChange={() => undefined}
        notesEpoch={1}
        onDraftChange={setText}
        imports={imports}
        onRegisterDispose={(dispose) => { disposeRef.current = dispose; }}
        onRegisterSave={(save, documentId) => { saveRef.current = save; documentIdRef.current = documentId; }}
        onRegisterLoad={(load) => { noteLoadRef.current = load; }}
      />
      {after?.(noteId)}
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
  it.each(['formatting_article', 'formatting_transcript'] as const)('shows real import stages for %s with a hidden prompt', async stage => {
    let report!: NonNullable<Parameters<NotesClient['readNotePage']>[1]>;
    let finishPage!: (page: NotePage) => void;
    const readNotePage = vi.fn<NotesClient['readNotePage']>((_url, onProgress) => {
      report = onProgress!;
      return new Promise(resolve => { finishPage = resolve; });
    });
    let finishPicture!: (bytes: Uint8Array) => void;
    const readNotePicture = vi.fn(() => new Promise<Uint8Array>(resolve => { finishPicture = resolve; }));
    let finishLock!: (note: NoteItem) => void;
    const lock = vi.fn(() => new Promise<NoteItem>(resolve => { finishLock = resolve; }));
    render(<Harness importUrl="https://example.org/article" promptVisible={false}
      getNote={async id => ({ id, document_id: id, document: documentWith('') })}
      readNotePage={readNotePage} readNotePicture={readNotePicture} onSetNoteReadOnly={lock} draft="Keep this" />);
    await openedEditor();
    const loading = await screen.findByText('Loading content…');
    expect(loading).toHaveTextContent('Retrieving content…');
    expect(loading.querySelector('.cha-notes-import-spinner')).toHaveAttribute('aria-hidden', 'true');
    act(() => report(stage));
    expect(loading).toHaveTextContent(stage === 'formatting_article' ? 'Formatting article…' : 'Formatting transcript…');
    await act(async () => finishPage({ url: 'https://example.org/article', title: '', markdown: 'Article\n\n![Photo](/photo.png)' }));
    expect(readNotePicture).toHaveBeenCalledExactlyOnceWith('https://example.org/photo.png');
    expect(loading).toHaveTextContent('Loading pictures…');
    await act(async () => finishPicture(new Uint8Array()));
    expect(lock).toHaveBeenCalledExactlyOnceWith(plan.id, true);
    expect(loading).toHaveTextContent('Saving note…');
    await act(async () => finishLock({ ...plan, read_only: true }));
    expect(screen.queryByText('Loading content…')).toBeNull();
  });

  it('ignores late stage updates from an abandoned import while another import is loading', async () => {
    const reports: NonNullable<Parameters<NotesClient['readNotePage']>[1]>[] = [];
    const finishes: ((page: NotePage) => void)[] = [];
    const readNotePage = vi.fn<NotesClient['readNotePage']>((_url, onProgress) => {
      reports.push(onProgress!);
      return new Promise(resolve => { finishes.push(resolve); });
    });
    render(<Harness importUrl="https://example.org/first" readNotePage={readNotePage}
      getNote={async id => ({ id, document_id: id, document: documentWith('') })} />);
    await openedEditor();
    await waitFor(() => expect(readNotePage).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    await openedEditor();
    fireEvent.change(screen.getByRole('textbox', { name: 'Prompt' }), { target: { value: 'https://example.org/second' } });
    fireEvent.click(screen.getByRole('button', { name: 'Send to Essay' }));
    await waitFor(() => expect(readNotePage).toHaveBeenCalledTimes(2));
    act(() => reports[1]('formatting_article'));
    act(() => reports[0]('formatting_transcript'));
    expect(screen.getByText('Loading content…')).toHaveTextContent('Formatting article…');
    await act(async () => finishes[0]({ url: 'https://example.org/first', title: '', markdown: 'Old article' }));
    expect(screen.getByText('Loading content…')).toHaveTextContent('Formatting article…');
    await act(async () => finishes[1]({ url: 'https://example.org/second', title: '', markdown: 'Current article' }));
    expect(screen.queryByText('Loading content…')).toBeNull();
  });


  it.each([false, true])('starts a URL opening once after editor setup and preserves the prompt (hidden=%s)', async (hidden) => {
    let open!: (content: NoteContent) => void;
    const getNote = vi.fn(() => new Promise<NoteContent>(resolve => { open = resolve; }));
    let finish!: (page: NotePage) => void;
    const readNotePage = vi.fn(() => {
      expect(liveEditor?.current?.isEditable).toBe(true);
      return new Promise<NotePage>(resolve => { finish = resolve; });
    });
    const props = { getNote, readNotePage, importUrl: 'https://example.org/article',
      draft: 'Keep this draft', promptVisible: !hidden };
    const view = render(<StrictMode><Harness {...props} /></StrictMode>);
    expect(readNotePage).not.toHaveBeenCalled();
    await act(async () => open({ id: plan.id, document_id: plan.id, document: documentWith('') }));
    const editor = await openedEditor();
    await waitFor(() => expect(readNotePage).toHaveBeenCalledExactlyOnceWith(props.importUrl, expect.any(Function)));
    expect(screen.getByText('Loading content…')).toHaveAttribute('role', 'status');
    fireEvent.click(screen.getByRole('button', { name: 'Content' }));
    expect(screen.getByRole('menuitem', { name: 'Upload' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Content' }));
    expect(editor.isEditable).toBe(true);
    view.rerender(<StrictMode><Harness {...props} promptVisible={!hidden} items={[{ ...plan }]} /></StrictMode>);
    expect(readNotePage).toHaveBeenCalledOnce();
    await act(async () => finish({ url: props.importUrl, title: '', markdown: 'Article' }));
    expect(editor.getText()).toBe('Article');
    expect(screen.queryByText('Loading content…')).toBeNull();
    view.rerender(<StrictMode><Harness {...props} promptVisible /></StrictMode>);
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep this draft');
    expect(readNotePage).toHaveBeenCalledOnce();
  });

  it('skips a nonempty note for the whole opening even when it is later emptied', async () => {
    const readNotePage = vi.fn();
    const props = { importUrl: 'https://example.org/article', readNotePage };
    const view = render(<Harness {...props} />);
    const editor = await openedEditor();
    expect(editor.getText()).toBe('Alpha');
    expect(readNotePage).not.toHaveBeenCalled();
    act(() => editor.commands.clearContent());
    view.rerender(<Harness {...props} promptVisible={false} />);
    expect(readNotePage).not.toHaveBeenCalled();
  });

  it('does not restart a failed shortcut after rerender or dismissal and permits manual recovery', async () => {
    const readNotePage = vi.fn().mockRejectedValueOnce(new Error('offline'))
      .mockResolvedValue({ url: 'https://example.org/article', title: '', markdown: 'Recovered article' });
    const props = { importUrl: 'https://example.org/article', readNotePage,
      getNote: async (id: number) => ({ id, document_id: id, document: documentWith('') }), draft: 'Keep this' };
    const view = render(<Harness {...props} />);
    const editor = await openedEditor();
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not retrieve the web page. Try again.');
    view.rerender(<Harness {...props} items={[{ ...plan }]} />);
    fireEvent.click(screen.getByRole('button', { name: 'Dismiss error' }));
    expect(readNotePage).toHaveBeenCalledOnce();
    expect(screen.queryByText('Loading content…')).toBeNull();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep this');
    fireEvent.change(screen.getByRole('textbox', { name: 'Prompt' }), { target: { value: props.importUrl } });
    fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    await waitFor(() => expect(editor.getText()).toBe('Recovered article'));
    expect(readNotePage).toHaveBeenCalledTimes(2);
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('');
  });

  it.each(['page', 'file'] as const)('preserves the %s source through the existing opening Retry', async kind => {
    const getNote = vi.fn().mockRejectedValueOnce(new Error('offline'))
      .mockResolvedValue({ id: plan.id, document_id: plan.id, document: documentWith('') });
    const readNotePage = vi.fn().mockResolvedValue({ url: 'https://example.org/article', title: '', markdown: 'Article' });
    const readNoteTextFile = vi.fn().mockResolvedValue({ name: '/plan.txt', text: 'Article' });
    const onNavigate = vi.fn();
    const sourceProps = kind === 'page' ? { importUrl: 'https://example.org/article' } : { importPath: '/plan.txt' };
    const readSource = kind === 'page' ? readNotePage : readNoteTextFile;
    render(<Harness {...sourceProps} getNote={getNote} readNotePage={readNotePage}
      readNoteTextFile={readNoteTextFile} onNavigate={onNavigate} />);
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not open');
    expect(readSource).not.toHaveBeenCalled();
    fireEvent.click(screen.getByRole('button', { name: 'Retry' }));
    expect(onNavigate).toHaveBeenCalledWith({ type: 'open-note', noteId: plan.id,
      destination: undefined, importSource: kind === 'page'
        ? { kind: 'page', url: 'https://example.org/article' } : { kind: 'file', path: '/plan.txt' } });
    await screen.findByText('Article');
    expect(readSource).toHaveBeenCalledOnce();
  });

  it.each([false, true])('keeps late shortcut results and errors away from the other note (failure=%s)', async fail => {
    let finish!: (page: NotePage) => void;
    let reject!: (failure: Error) => void;
    const readNotePage = vi.fn(() => new Promise<NotePage>((ok, no) => { finish = ok; reject = no; }));
    const renameNoteItem = vi.fn();
    render(<Harness importUrl="https://example.org/article" readNotePage={readNotePage}
      items={[{ ...plan, name: 'New note' }]} renameNoteItem={renameNoteItem}
      getNote={async id => ({ id, document_id: id, document: documentWith(id === plan.id ? '' : 'Essay text') })} draft="Keep this" />);
    await openedEditor();
    await waitFor(() => expect(readNotePage).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    const editor = await openedEditor();
    await act(async () => {
      if (fail) reject(new Error('offline'));
      else finish({ url: 'https://example.org/article', title: 'Late title', markdown: 'Late article' });
    });
    expect(editor.getText()).toBe('Essay text');
    if (fail) expect(renameNoteItem).not.toHaveBeenCalled();
    else expect(renameNoteItem).toHaveBeenCalledExactlyOnceWith(plan.id, 'Late title');
    expect(screen.queryByRole('alert')).toBeNull();
    expect(screen.queryByText('Loading content…')).toBeNull();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep this');
  });

  it('does not offer the previous editor to the newly opened note before effects run', async () => {
    const seen: Array<[number | null, Editor | null]> = [];
    function Probe({ noteId }: { noteId: number | null }) {
      useLayoutEffect(() => { seen.push([noteId, liveEditor?.current ?? null]); });
      return null;
    }
    render(<Harness after={(noteId) => <Probe noteId={noteId} />} />);
    const first = await openedEditor();
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    await openedEditor();
    expect(seen.some(([id]) => id === essay.id)).toBe(true);
    expect(seen.filter(([id, editor]) => id === essay.id && editor === first)).toEqual([]);
  });

  it.each(['page', 'path', 'upload'])('shows shared loading status for a manual %s import and disables overlap', async source => {
    let finish!: () => void;
    const pending = new Promise<void>(resolve => { finish = resolve; });
    const readNotePage = vi.fn(async () => { await pending; return { url: 'https://example.org', title: '', markdown: 'Article' }; });
    const readNoteTextFile = vi.fn(async () => { await pending; return { name: 'file.txt', text: 'File content' }; });
    render(<Harness readNotePage={readNotePage} readNoteTextFile={readNoteTextFile}
      draft={source === 'page' ? 'https://example.org' : '/tmp/file.txt'} />);
    await openedEditor();
    if (source === 'upload') {
      const file = new File(['File content'], 'file.txt');
      // Keep the upload pending at its file read.
      vi.spyOn(FileReader.prototype, 'readAsArrayBuffer').mockImplementation(function (this: FileReader) {
        void pending.then(() => {
          Object.defineProperty(this, 'result', { value: new TextEncoder().encode('File content').buffer });
          this.onload?.(new ProgressEvent('load') as ProgressEvent<FileReader>);
        });
      });
      fireEvent.change(screen.getByLabelText('Text file'), { target: { files: [file] } });
    } else fireEvent.click(screen.getByRole('button', { name: 'Send to Plan' }));
    expect(await screen.findByText('Loading content…')).toHaveAttribute('role', 'status');
    fireEvent.click(screen.getByRole('button', { name: 'Content' }));
    expect(screen.getByRole('menuitem', { name: 'Upload' })).toBeDisabled();
    fireEvent.click(screen.getByRole('button', { name: 'Content' }));
    expect(screen.getByRole('button', { name: 'Send to Plan' })).toBeDisabled();
    act(() => finish());
    await waitFor(() => expect(screen.queryByText('Loading content…')).toBeNull());
  });


  it.each([false, true])('copies the whole note from Content and keeps the selection, prompt, and scroll (read-only: %s)', async (readOnly) => {
    const user = userEvent.setup();
    const saveNote = vi.fn();
    render(<Harness items={[{ ...plan, read_only: readOnly }]} draft="Keep this draft" saveNote={saveNote} />);
    const editor = await openedEditor();
    act(() => editor.commands.setTextSelection({ from: 2, to: 4 }));
    const before = editor.getJSON();
    const host = editor.view.dom.closest('.cha-notes-document')!;
    host.scrollTop = 640;
    const original = Object.getOwnPropertyDescriptor(document, 'execCommand');
    const exec = vi.fn(() => {
      expect(window.getSelection()?.toString()).toBe('Alpha');
      return true;
    });
    Object.defineProperty(document, 'execCommand', { configurable: true, value: exec });
    try {
      const content = screen.getByRole('button', { name: 'Content' });
      expect(document.querySelectorAll('.cha-notes-toolbar button')[1]).toBe(content);
      await user.click(content);
      expect(screen.getAllByRole('menuitem').map((item) => item.textContent)).toEqual(['Upload', 'Copy All']);
      expect(screen.getByRole('menuitem', { name: 'Upload' }).hasAttribute('disabled')).toBe(readOnly);
      await user.click(screen.getByRole('menuitem', { name: 'Copy All' }));
      expect(exec).toHaveBeenCalledExactlyOnceWith('copy');
      expect(content).toHaveAttribute('aria-expanded', 'false');
      expect(editor.view.dom).toHaveFocus();
      expect(editor.state.selection.from).toBe(2);
      expect(editor.state.selection.to).toBe(4);
      expect(host.scrollTop).toBe(640);
      expect(editor.getJSON()).toEqual(before);
      expect(editor.can().undo()).toBe(false);
      expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep this draft');
      expect(saveNote).not.toHaveBeenCalled();
    } finally {
      if (original) Object.defineProperty(document, 'execCommand', original);
      else Reflect.deleteProperty(document, 'execCommand');
    }
  });

  it('restores a note position and opens a note without one at the top', async () => {
    render(<Harness getNote={async (id) => ({
      id, document_id: id, document: documentWith('Reading'), ...(id === 4 ? { scroll_top: 680 } : {}),
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
      id, document_id: id, scroll_top: 680, document: { type: 'doc', content: [
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
    render(<Harness getNote={async (id) => ({ id, document_id: id, document: documentWith('Alpha beta') })} />);
    const editor = await openedEditor();
    act(() => { editor.commands.setTextSelection({ from: 7, to: 11 }); });
    const insert = screen.getByRole('button', { name: 'Insert' });
    await user.click(insert);
    const code = screen.getByRole('menuitemcheckbox', { name: 'Code' });
    expect(code).toHaveAttribute('aria-checked', 'false');
    expect(screen.queryByRole('button', { name: 'Link' })).not.toBeInTheDocument();
    await user.click(code);
    expect(insert).toHaveAttribute('aria-expanded', 'false');
    expect(editor.view.dom.querySelector('code')?.textContent).toBe('beta');
    expect(editor.getText()).toBe('Alpha beta');
    expect(editor.view.dom).toHaveFocus();
    await user.click(historyAction('Undo'));
    expect(editor.view.dom.querySelector('code')).toBeNull();
    await user.click(historyAction('Redo'));
    expect(editor.view.dom.querySelector('code')?.textContent).toBe('beta');
    await user.click(insert);
    expect(screen.getByRole('menuitemcheckbox', { name: 'Code' })).toHaveAttribute('aria-checked', 'true');
    await user.click(screen.getByRole('menuitemcheckbox', { name: 'Code' }));
    expect(editor.view.dom.querySelector('code')).toBeNull();
    expect(editor.getText()).toBe('Alpha beta');
  });

  it('replaces a note from the picker, keeps the prompt, and saves', async () => {
    const saveNote = vi.fn(async () => undefined);
    const renameNoteItem = vi.fn();
    render(<Harness draft="Keep this draft" saveNote={saveNote} renameNoteItem={renameNoteItem} />);
    const editor = await openedEditor();
    const input = screen.getByLabelText('Text file');
    const click = vi.spyOn(input, 'click');
    fireEvent.click(screen.getByRole('button', { name: 'Content' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Upload' }));
    expect(click).toHaveBeenCalledOnce();
    expect(screen.queryByRole('menu', { name: 'Content' })).not.toBeInTheDocument();
    fireEvent.change(input, { target: { files: [new File(['# Imported\n\n**Body**'], 'plan.md')] } });
    await waitFor(() => expect(editor.getText({ blockSeparator: '\n' })).toBe('Imported\nBody'));
    expect(editor.view.dom.querySelector('strong')?.textContent).toBe('Body');
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep this draft');
    expect(renameNoteItem).not.toHaveBeenCalled();
    await waitFor(() => expect(screen.queryByText('Loading content…')).toBeNull());
    const document = editor.getJSON();
    fireEvent.click(screen.getByRole('button', { name: 'Leave' }));
    await waitFor(() => expect(saveNote).toHaveBeenCalledWith(plan.id, document));
  });

  it.each([false, true])('freezes the note while a file replaces it (save fails=%s)', async fails => {
    let finishPicture!: (bytes: Uint8Array) => void;
    const readNotePicture = vi.fn(() => new Promise<Uint8Array>(resolve => { finishPicture = resolve; }));
    const saveNote = vi.fn(async () => {
      if (fails) throw new ChaError('internal_error', 'Save failed.');
      return { annotation_created: false } as never;
    });
    const lock = vi.fn(async (id: number, readOnly: boolean) => ({ ...plan, id, read_only: readOnly }));
    render(<Harness importPath="/plan.md" saveNote={saveNote} onSetNoteReadOnly={lock} readNotePicture={readNotePicture}
      readNoteTextFile={async () => ({ name: '/plan.md', text: 'File body\n\n![Photo](https://example.org/photo.png)' })}
      getNote={async id => ({ id, document_id: id, document: documentWith('') })} />);
    const editor = await openedEditor();
    await waitFor(() => expect(readNotePicture).toHaveBeenCalledOnce());
    expect(screen.getByText('Loading content…')).toHaveTextContent('Loading pictures…');
    expect(editor.isEditable).toBe(false);
    expect(screen.getByRole('button', { name: 'Edit' })).toBeDisabled();
    expect(screen.getByRole('button', { name: 'Highlight' })).toBeDisabled();
    await act(async () => finishPicture(new Uint8Array()));
    await waitFor(() => expect(screen.queryByText('Loading content…')).toBeNull());
    expect(editor.getText()).toContain('File body');
    expect(lock).toHaveBeenCalledTimes(fails ? 0 : 1);
    // After a lock, App applies the read-only item. This harness does not.
    if (fails) {
      expect(editor.isEditable).toBe(true);
      expect(screen.getByRole('button', { name: 'Highlight' })).toBeEnabled();
    }
  });

  it('automatically imports a file into a new note without changing the prompt', async () => {
    const readNoteTextFile = vi.fn(async () => ({ name: '/tmp/a.md', text: '# File title\n\nBody' }));
    const renameNoteItem = vi.fn(async (id: number, name: string) => ({ ...plan, id, name }));
    render(<Harness items={[{ ...plan, name: 'New note' }]} getNote={async id => ({ id, document_id: id, document: documentWith('') })}
      importPath="/tmp/a.md" readNoteTextFile={readNoteTextFile} renameNoteItem={renameNoteItem} draft="Keep draft" />);
    const editor = await openedEditor();
    await waitFor(() => expect(renameNoteItem).toHaveBeenCalledWith(plan.id, 'File title'));
    expect(editor.getText()).toContain('Body');
    expect(readNoteTextFile).toHaveBeenCalledOnce();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep draft');
  });

  it('reports a missing explicit file without inserting the path or automatically retrying', async () => {
    const readNoteTextFile = vi.fn(async () => null);
    const props = { items: [{ ...plan, name: 'New note' }],
      getNote: async (id: number) => ({ id, document_id: id, document: documentWith('') }),
      importPath: '/tmp/missing file.md', readNoteTextFile, draft: 'Keep draft' };
    const view = render(<Harness {...props} />);
    expect(await screen.findByRole('alert')).toHaveTextContent('The file could not be loaded. Check the path and permissions.');
    expect(liveEditor!.current!.isEmpty).toBe(true);
    view.rerender(<Harness {...props} promptVisible={false} />);
    fireEvent.click(screen.getByRole('button', { name: 'Dismiss error' }));
    expect(screen.queryByRole('alert')).toBeNull();
    expect(readNoteTextFile).toHaveBeenCalledOnce();
    view.rerender(<Harness {...props} />);
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep draft');
    expect(readNoteTextFile).toHaveBeenCalledOnce();
  });

  it.each(['content', 'null', 'error'] as const)('keeps a late file %s result away from the other note', async result => {
    let finish!: (value: TextFileRequest | null) => void;
    let reject!: (error: Error) => void;
    const readNoteTextFile = vi.fn(() => new Promise<TextFileRequest | null>((ok, no) => { finish = ok; reject = no; }));
    const renameNoteItem = vi.fn();
    const onSetNoteReadOnly = vi.fn();
    render(<Harness importPath="/tmp/plan.md" readNoteTextFile={readNoteTextFile}
      items={[{ ...plan, name: 'New note' }]} renameNoteItem={renameNoteItem} onSetNoteReadOnly={onSetNoteReadOnly}
      getNote={async id => ({ id, document_id: id, document: documentWith(id === plan.id ? '' : 'Essay text') })} draft="Keep draft" />);
    await openedEditor();
    await waitFor(() => expect(readNoteTextFile).toHaveBeenCalledOnce());
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    const editor = await openedEditor();
    await act(async () => {
      if (result === 'error') reject(new Error('offline'));
      else finish(result === 'null' ? null : { name: '/tmp/plan.md', text: '# Late title\n\nLate content' });
    });
    expect(editor.getText()).toBe('Essay text');
    if (result === 'content') {
      expect(renameNoteItem).toHaveBeenCalledExactlyOnceWith(plan.id, 'Late title');
      expect(onSetNoteReadOnly).toHaveBeenCalledExactlyOnceWith(plan.id, true);
    } else {
      expect(renameNoteItem).not.toHaveBeenCalled();
      expect(onSetNoteReadOnly).not.toHaveBeenCalled();
    }
    expect(screen.queryByRole('alert')).toBeNull();
    expect(screen.queryByText('Loading content…')).toBeNull();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep draft');
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

  it.each([
    ['/tmp/plan.md', false], ['/tmp/plan.md', true], ['https://example.org/article', false],
  ] as const)('saves %s only into its note after a switch and clears only the sent prompt (edited=%s)', async (sent, edited) => {
    let finish!: () => void;
    const pending = new Promise<void>((resolve) => { finish = resolve; });
    const readNoteTextFile = vi.fn(async () => { await pending; return { name: '/tmp/plan.md', text: '# Late' }; });
    const readNotePage = vi.fn(async () => { await pending; return { url: sent, title: 'Late', markdown: 'Late article' }; });
    const renameNoteItem = vi.fn();
    const lock = vi.fn(async (id: number, readOnly: boolean) => ({ ...plan, id, read_only: readOnly }));
    render(<Harness items={[{ ...plan, name: 'New note' }]} draft={sent} onSetNoteReadOnly={lock}
      getNote={async id => ({ id, document_id: id, document: documentWith(id === essay.id ? 'Essay text' : '') })}
      readNoteTextFile={readNoteTextFile} readNotePage={readNotePage} renameNoteItem={renameNoteItem} />);
    await openedEditor();
    fireEvent.keyDown(screen.getByRole('textbox', { name: 'Prompt' }), { key: 'Enter' });
    await waitFor(() => expect(readNoteTextFile.mock.calls.length + readNotePage.mock.calls.length).toBe(1));
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    const editor = await openedEditor();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(sent);
    if (edited) fireEvent.change(screen.getByRole('textbox', { name: 'Prompt' }), { target: { value: 'Other text' } });
    await act(async () => finish());
    await waitFor(() => expect(lock).toHaveBeenCalledExactlyOnceWith(plan.id, true));
    await act(async () => undefined);
    expect(editor.getText()).toBe('Essay text');
    expect(renameNoteItem).toHaveBeenCalledExactlyOnceWith(plan.id, 'Late');
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(edited ? 'Other text' : '');
    fireEvent.click(screen.getByRole('button', { name: 'Content' }));
    expect(screen.getByRole('menuitem', { name: 'Upload' })).toBeEnabled();
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
        getNote={async (id) => ({ id, document_id: id, document: documentWith(text) })}
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
    const folder: NoteItem = noteItem({ id: 3, kind: 'folder', name: 'Folder' });
    const original = { ...plan, parent_id: folder.id, name: 'New note' };
    const renameNoteItem = vi.fn(async (id: number, name: string) => ({ ...original, id, name }));
    render(<Harness
      items={[folder, original,
        noteItem({ id: 20, name: 'Title' }),
        ...names.map((name, index) => (noteItem({ id: 30 + index, parent_id: folder.id, name }))),
      ]}
      getNote={async (id) => ({ id, document_id: id, document: documentWith('') })}
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
      getNote: async (id: number) => ({ id, document_id: id, document: documentWith('') }),
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
      getNote={async (id) => ({ id, document_id: id, document: documentWith('') })}
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
        address.startsWith('https://') ? address : `http://${address}`, expect.any(Function),
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
      .mockResolvedValueOnce({ id: 4, document_id: 4, document: documentWith('Alpha') });
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
    expect(screen.queryByRole('combobox', { name: 'Paragraph style' })).not.toBeInTheDocument();
    expect(screen.queryByRole('button', { name: 'New note' })).not.toBeInTheDocument();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    const send = screen.getByRole('button', { name: 'Send' });
    expect(send).toBeDisabled();
    fireEvent.change(prompt, { target: { value: 'Keep this' } });
    fireEvent.click(send);
    fireEvent.keyDown(prompt, { key: 'Enter' });
    expect(prompt).toHaveValue('Keep this');
  });

  it('keeps history when the same open note is shown again', async () => {
    const getNote = vi.fn(async () => ({ id: 4, document_id: 4, document: documentWith('Alpha') }));
    const view = render(<Harness getNote={getNote} />);
    const editor = await screen.findByText('Alpha').then(() => openedEditor());
    act(() => insertText(editor, ' beta'));
    expect(editor.can().undo()).toBe(true);
    view.rerender(<Harness getNote={getNote} />);
    expect(getNote).toHaveBeenCalledOnce();
    expect(screen.getByText('Alpha beta')).toBeInTheDocument();
    expect((await openedEditor()).can().undo()).toBe(true);
  });

  it('replaces the open note with incoming content without echoing an unsaved edit', async () => {
    vi.useFakeTimers();
    const getNote = vi.fn()
      .mockResolvedValueOnce({ id: 4, document_id: 4, document: documentWith('Alpha') })
      .mockResolvedValue({ id: 4, document_id: 4, document: documentWith('Incoming') });
    const saveNote = vi.fn(async () => undefined);
    const view = render(<Harness getNote={getNote} saveNote={saveNote} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    const editor = await openedEditor();
    act(() => insertText(editor, ' local'));
    expect(editor.can().undo()).toBe(true);
    view.rerender(<Harness getNote={getNote} saveNote={saveNote} extraRequest={1} />);
    await act(async () => { await vi.advanceTimersByTimeAsync(0); });
    expect(screen.getByText('Incoming')).toBeInTheDocument();
    expect(screen.queryByText('Alpha local')).not.toBeInTheDocument();
    expect(getNote).toHaveBeenCalledTimes(2);
    // Advance past the debounced save delay to prove the replaced edit
    // was discarded rather than queued as an outgoing change.
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs + 1000); });
    expect(saveNote).not.toHaveBeenCalled();
  });

  it('formats the retained selection and leaves the draft', async () => {
    render(<Harness />);
    const editor = await openedEditor();
    const prompt = screen.getByRole('textbox', { name: 'Prompt' });
    fireEvent.change(prompt, { target: { value: 'Keep' } });
    editor.commands.setTextSelection({ from: 1, to: 4 });
    function clickAction(name: string) {
      if (name === 'Bold') fireEvent.click(screen.getByRole('button', { name: 'Edit' }));
      if (name === 'Chapter') fireEvent.click(screen.getByRole('button', { name: 'Headings' }));
      if (name === 'Undo' || name === 'Redo') fireEvent.click(screen.getByRole('button', { name: 'History' }));
      const control = screen.getByRole(name === 'Bold' || name === 'Chapter' ? 'menuitemcheckbox' : 'menuitem', { name });
      act(() => control.focus());
      fireEvent.click(control);
      expect(editor.view.dom).toHaveFocus();
    }
    expect(editor.can().undo()).toBe(false);
    clickAction('Chapter');
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

    fireEvent.click(screen.getByRole('button', { name: 'Insert' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'List' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Bullet list' }));
    expect(editor.view.dom).toHaveFocus();
    expect(editor.isActive('bulletList')).toBe(true);
    fireEvent.click(screen.getByRole('button', { name: 'Insert' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'List' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Numbered list' }));
    expect(editor.view.dom).toHaveFocus();
    expect(editor.isActive('orderedList')).toBe(true);
    expect(prompt).toHaveValue('Keep');
  });

  it('jumps to the selected duplicate heading, focuses and scrolls it, without saving', async () => {
    const saveNote = vi.fn();
    render(<Harness saveNote={saveNote} getNote={async (id) => ({
      id, document_id: id, document: {
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

  it('jumps to the selected duplicate highlight in a read-only note without saving', async () => {
    const saveNote = vi.fn();
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: {
      type: 'doc', content: [{ type: 'paragraph', content: [
        { type: 'text', text: 'Repeat', marks: [{ type: 'highlight' }] },
        { type: 'text', text: ' gap ' },
        { type: 'text', text: 'Repeat', marks: [{ type: 'highlight' }] },
      ] }],
    } }));
    render(<Harness items={[{ ...plan, read_only: true }]} getNote={getNote} saveNote={saveNote} />);
    const editor = await openedEditor();
    const before = editor.getJSON();
    const target = noteHighlights(editor.state.doc)[1];
    const host = editor.view.dom.closest('.cha-notes-document') as HTMLElement;
    Object.defineProperty(host, 'clientHeight', { value: 200 });
    vi.spyOn(editor.view, 'coordsAtPos').mockReturnValue({ top: 550, bottom: 570, left: 0, right: 10 });
    fireEvent.click(screen.getByRole('button', { name: 'Highlights' }));
    const entries = await screen.findAllByRole('menuitem', { name: 'Repeat' });
    expect(entries[0]).toHaveFocus();
    fireEvent.click(entries[1]);
    expect(editor.state.selection.from).toBe(target.from);
    expect(editor.state.selection.to).toBe(target.to);
    expect(editor.view.dom).toHaveFocus();
    expect(host.scrollTop).toBe(450);
    expect(editor.isEditable).toBe(false);
    expect(editor.getJSON()).toEqual(before);
    expect(editor.can().undo()).toBe(false);
    expect(getNote).toHaveBeenCalledOnce();
    expect(saveNote).not.toHaveBeenCalled();
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
  });

  it('lists only the current note in document order without fetching other notes, and refreshes on note changes', async () => {
    const getNote = vi.fn(async (id: number) => ({ id, document_id: id, document: {
      type: 'doc', content: (id === 4 ? ['Zebra', 'Alpha'] : ['Other']).map((text) => ({
        type: 'paragraph', content: [{ type: 'text', text, marks: [{ type: 'highlight' }] }],
      })),
    } }));
    render(<Harness items={[plan, essay]} getNote={getNote} />);
    const first = await openedEditor();
    const jump = screen.getByRole('button', { name: 'Highlights' });
    expect(jump.textContent).toBe('');
    expect(jump.querySelector('svg')).toBeInTheDocument();
    expect(jump).toHaveAttribute('title', 'Highlights');
    fireEvent.click(jump);
    expect(screen.getAllByRole('menuitem').map((item) => item.textContent)).toEqual(['Zebra', 'Alpha']);
    expect(getNote.mock.calls.map(([id]) => id)).toEqual([4]);
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    fireEvent.click(screen.getByRole('button', { name: 'Open essay' }));
    await waitFor(() => expect(liveEditor?.current).not.toBe(first));
    await openedEditor();
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: 'Highlights' }));
    expect(screen.getAllByRole('menuitem').map((item) => item.textContent)).toEqual(['Other']);
    expect(getNote.mock.calls.map(([id]) => id)).toEqual([4, 5]);
  });

  it('refreshes highlights after edits, Undo, and highlight removal', async () => {
    const user = userEvent.setup();
    render(<Harness />);
    const editor = await openedEditor();
    const jump = screen.getByRole('button', { name: 'Highlights' });
    await user.click(jump);
    expect(screen.getByRole('status')).toHaveTextContent('No highlights');
    await user.keyboard('{Escape}');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(editor.view.dom).toHaveFocus();
    act(() => { editor.commands.setContent('<p><mark>First</mark> gap <mark>Second</mark></p>'); });
    fireEvent.click(jump);
    const entries = await screen.findAllByRole('menuitem');
    expect(entries[0]).toHaveFocus();
    await user.keyboard('{End}');
    expect(entries[1]).toHaveFocus();
    act(() => { withIsolatedHistory(editor, () => editor.commands.insertContentAt({ from: 1, to: 6 }, 'New')); });
    expect(screen.getByRole('menuitem', { name: 'New' })).toBeInTheDocument();
    act(() => { editor.commands.undo(); });
    expect(screen.getByRole('menuitem', { name: 'First' })).toBeInTheDocument();
    act(() => { editor.commands.selectAll(); editor.commands.unsetHighlight(); });
    expect(screen.getByRole('status')).toHaveTextContent('No highlights');
    await user.click(screen.getByRole('textbox', { name: 'Prompt' }));
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveFocus();
  });

  it('derives the heading menu after edits and Undo, and hides it while frozen', async () => {
    let finishSave!: () => void;
    render(<Harness saveNote={() => new Promise<void>((resolve) => { finishSave = resolve; })} getNote={async (id) => ({
      id, document_id: id, document: {
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

  it.each([
    ['Title', 1], ['Chapter', 2], ['Section', 3],
  ] as const)('toggles %s, preserves the selection, and returns focus', async (name, level) => {
    const user = userEvent.setup();
    render(<Harness />);
    const editor = await openedEditor();
    act(() => editor.commands.setTextSelection({ from: 1, to: 4 }));
    const headings = screen.getByRole('button', { name: 'Headings' });
    expect(headings).toHaveTextContent('H');
    await user.click(headings);
    expect(screen.getByRole('menuitemcheckbox', { name })).toHaveAttribute('aria-checked', 'false');
    await user.click(screen.getByRole('menuitemcheckbox', { name }));
    expect(editor.isActive('heading', { level })).toBe(true);
    expect(headings).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByRole('menu', { name: 'Headings' })).not.toBeInTheDocument();
    expect(editor.state.doc.textBetween(editor.state.selection.from, editor.state.selection.to)).toBe('Alp');
    expect(editor.view.dom).toHaveFocus();
    await user.click(headings);
    expect(screen.getByRole('menuitemcheckbox', { name })).toHaveAttribute('aria-checked', 'true');
    await user.click(screen.getByRole('menuitemcheckbox', { name }));
    expect(editor.isActive('paragraph')).toBe(true);
    expect(editor.view.dom).toHaveFocus();
    fireEvent.click(historyAction('Undo'));
    expect(editor.isActive('heading', { level })).toBe(true);
    fireEvent.click(historyAction('Redo'));
    expect(editor.isActive('paragraph')).toBe(true);
  });

  it.each(['Bold', 'Italic', 'Underline', 'Highlight', 'Code'])('returns focus after %s and keeps formatting for typing', async (name) => {
    render(<Harness />);
    const editor = await openedEditor();
    if (name !== 'Highlight') fireEvent.click(screen.getByRole('button', { name: name === 'Code' ? 'Insert' : 'Edit' }));
    const control = screen.getByRole(name === 'Highlight' ? 'button' : 'menuitemcheckbox', { name });
    act(() => control.focus());
    fireEvent.click(control);
    expect(editor.view.dom).toHaveFocus();
    act(() => insertText(editor, '!'));
    expect(editor.getText()).toBe('Alpha!');
    expect(editor.getJSON().content?.[0]?.content?.at(-1)?.marks?.[0]?.type).toBe(name.toLowerCase());
  });

  it.each(['Bold', 'Italic', 'Underline'])('toggles %s on the selected text through the Edit menu', async (name) => {
    const user = userEvent.setup();
    render(<Harness />);
    const editor = await openedEditor();
    act(() => editor.commands.setTextSelection({ from: 2, to: 4 }));
    const edit = screen.getByRole('button', { name: 'Edit' });
    await user.click(edit);
    const item = screen.getByRole('menuitemcheckbox', { name });
    expect(item).toHaveAttribute('aria-checked', 'false');
    await user.click(item);
    expect(editor.state.doc.textBetween(editor.state.selection.from, editor.state.selection.to)).toBe('lp');
    expect(editor.isActive(name.toLowerCase())).toBe(true);
    expect(editor.view.dom).toHaveFocus();
    expect(edit).toHaveAttribute('aria-expanded', 'false');
    expect(screen.queryByRole('menu', { name: 'Edit' })).not.toBeInTheDocument();
    await user.click(edit);
    expect(screen.getByRole('menuitemcheckbox', { name })).toHaveAttribute('aria-checked', 'true');
    await user.click(screen.getByRole('menuitemcheckbox', { name }));
    expect(editor.isActive(name.toLowerCase())).toBe(false);
    expect(editor.view.dom).toHaveFocus();
  });

  it('inserts literal prompts from Send and Enter and keeps later typing', async () => {
    render(<TransliterationProvider controlsVisible><Harness /></TransliterationProvider>);
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
    fireEvent.click(screen.getByRole('button', { name: 'Insert' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Picture' }));
    fireEvent.change(input, {
      target: { files: [new File([png], 'dot.png', { type: 'image/png' })] },
    });
    await waitFor(() => expect(JSON.stringify(editor.getJSON())).toContain('data:image/png'));
    expect(editor.view.dom).toHaveFocus();
    expect(screen.getByText(/Could not save “Plan”/)).toBeInTheDocument();
    expect(screen.queryByText(pictureTypeMessage)).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Insert' }));
    fireEvent.click(screen.getByRole('menuitem', { name: 'Picture' }));
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
    fireEvent.click(historyAction('Undo'));
    expect(editor.getText()).toBe('Alpha');

    fireEvent.change(screen.getByRole('textbox', { name: 'Find' }), { target: { value: 'a' } });
    fireEvent.change(replace, { target: { value: '' } });
    act(() => screen.getByRole('button', { name: 'Replace all' }).focus());
    fireEvent.click(screen.getByRole('button', { name: 'Replace all' }));
    expect(editor.view.dom).toHaveFocus();
    expect(editor.getText()).toBe('lph');
    fireEvent.click(historyAction('Undo'));
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

  it.each([false, true])('shows the context actions, including on links (read-only: %s)', async (readOnly) => {
    render(<Harness items={[{ ...plan, read_only: readOnly }]} getNote={async (id) => ({
      id, document_id: id, document: { type: 'doc', content: [{ type: 'paragraph', content: [
        { type: 'text', text: 'Alpha' },
        { type: 'text', text: ' link', marks: [{ type: 'link', attrs: { href: 'https://example.com/' } }] },
      ] }] },
    })} />);
    const editor = await openedEditor();
    act(() => { editor.commands.setTextSelection({ from: 1, to: 6 }); });
    for (const target of [editor.view.dom.querySelector('p')!, editor.view.dom.querySelector('a')!]) {
      expect(fireEvent.contextMenu(target, { clientX: 25, clientY: 30 })).toBe(false);
      const items = screen.getAllByRole('menuitem');
      expect(items.map((item) => item.textContent)).toEqual(['Cut', 'Copy', 'Copy JSON', 'Paste', 'Bold', 'Italic', 'Highlight', 'Add comment']);
      for (const item of items) {
        if (readOnly && !['Copy', 'Copy JSON', 'Highlight', 'Add comment'].includes(item.textContent!)) expect(item).toBeDisabled();
        else expect(item).toBeEnabled();
      }
      expect(screen.getByRole('menuitem', { name: readOnly ? 'Copy' : 'Cut' })).toHaveFocus();
      fireEvent.keyDown(screen.getByRole('menu'), { key: 'ArrowDown' });
      expect(screen.getByRole('menuitem', { name: readOnly ? 'Copy JSON' : 'Copy' })).toHaveFocus();
      fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
      expect(screen.queryByRole('menu')).not.toBeInTheDocument();
      expect(editor.view.dom).toHaveFocus();
      expect(editor.state.doc.textBetween(editor.state.selection.from, editor.state.selection.to)).toBe('Alpha');
    }
  });

  it.each([false, true])('copies the current whole note as formatted JSON (read-only: %s)', async (readOnly) => {
    const user = userEvent.setup();
    const writeText = vi.spyOn(navigator.clipboard, 'writeText').mockResolvedValue();
    render(<Harness items={[{ ...plan, read_only: readOnly }]} />);
    const editor = await openedEditor();
    act(() => {
      if (!readOnly) insertText(editor, 'Unsaved ');
      editor.commands.setTextSelection({ from: 2, to: 4 });
    });
    const before = editor.getJSON();
    const selection = editor.state.selection;
    fireEvent.contextMenu(editor.view.dom.querySelector('p')!);
    await user.click(screen.getByRole('menuitem', { name: 'Copy JSON' }));
    expect(writeText).toHaveBeenCalledExactlyOnceWith(JSON.stringify(before, null, 2));
    expect(JSON.parse(writeText.mock.calls[0][0])).toEqual(before);
    expect(editor.getJSON()).toEqual(before);
    expect(editor.state.selection.eq(selection)).toBe(true);
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(editor.view.dom).toHaveFocus();
  });

  it('reports a Copy JSON clipboard failure without changing the note', async () => {
    const user = userEvent.setup();
    vi.spyOn(navigator.clipboard, 'writeText').mockRejectedValue(new Error('Denied'));
    render(<Harness />);
    const editor = await openedEditor();
    const before = editor.getJSON();
    fireEvent.contextMenu(editor.view.dom.querySelector('p')!);
    await user.click(screen.getByRole('menuitem', { name: 'Copy JSON' }));
    expect(await screen.findByRole('alert')).toHaveTextContent('Could not copy the note. Try again.');
    expect(editor.getJSON()).toEqual(before);
  });

  it.each([false, true])('adds a blue comment before the clicked paragraph and saves it (read-only: %s)', async (readOnly) => {
    let saved: Record<string, unknown> = { type: 'doc', content: [
      ...documentWith('First paragraph').content,
      { type: 'paragraph', content: [
        { type: 'text', text: 'Clicked paragraph', marks: [{ type: 'link', attrs: { href: 'https://example.com/' } }] },
      ] },
    ] };
    const saveNote = vi.fn(async (_id: number, document: Record<string, unknown>) => { saved = document; });
    const props = { items: [{ ...plan, read_only: readOnly }], saveNote,
      getNote: async (id: number) => ({ id, document_id: id, document: saved }) };
    const first = render(<Harness {...props} draft="My comment" />);
    const editor = await openedEditor();
    vi.useFakeTimers();
    act(() => { editor.commands.setTextSelection({ from: 1, to: 6 }); });
    fireEvent.contextMenu(editor.view.dom.querySelector('a')!);
    expect(screen.getByRole('menuitem', { name: 'Add comment' })).toBeEnabled();
    const host = editor.view.dom.closest('.cha-notes-document') as HTMLElement;
    host.scrollTop = 120;
    const jump = () => { host.scrollTop = 900; };
    editor.on('update', jump);
    fireEvent.click(screen.getByRole('menuitem', { name: 'Add comment' }));
    editor.off('update', jump);
    expect(host.scrollTop).toBe(120);
    expect(Array.from(editor.view.dom.querySelectorAll('p')).map((p) => p.textContent))
      .toEqual(['First paragraph', 'My comment', 'Clicked paragraph']);
    expect(editor.view.dom.querySelector('mark')?.style.backgroundColor).toBe('rgb(179, 217, 255)');
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(editor.isEditable).toBe(!readOnly);
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).toHaveBeenCalledOnce();
    first.unmount();
    render(<Harness {...props} />);
    const reopened = await openedEditor();
    expect(reopened.getJSON()).toEqual(saved);
    expect(reopened.view.dom.querySelector('mark')?.style.backgroundColor).toBe('rgb(179, 217, 255)');
    expect(reopened.isEditable).toBe(!readOnly);
  });

  it.each([false, true])('deletes the clicked saved comment, keeps the prompt, and persists deletion (read-only: %s)', async (readOnly) => {
    const comment = (text: string) => ({ type: 'paragraph', content: [{ type: 'text', text, marks: [
      { type: 'highlight', attrs: { color: '#b3d9ff' } },
      { type: 'link', attrs: { href: 'https://example.com/' } },
    ] }] });
    let saved: Record<string, unknown> = { type: 'doc', content: [
      comment('Keep this comment'), comment('Delete this comment'), ...documentWith('Original paragraph').content,
    ] };
    const saveNote = vi.fn(async (_id: number, document: Record<string, unknown>) => { saved = document; });
    const props = { items: [{ ...plan, read_only: readOnly }], saveNote,
      getNote: async (id: number) => ({ id, document_id: id, document: saved }), draft: 'Keep this prompt' };
    const first = render(<Harness {...props} />);
    const editor = await openedEditor();
    vi.useFakeTimers();
    act(() => { editor.commands.setTextSelection(1); });
    const host = editor.view.dom.closest('.cha-notes-document') as HTMLElement;
    host.scrollTop = 120;
    fireEvent.contextMenu(editor.view.dom.querySelectorAll('a')[1]);
    expect(screen.getAllByRole('menuitem').at(-1)).toHaveTextContent('Delete comment');
    expect(screen.queryByRole('menuitem', { name: 'Add comment' })).not.toBeInTheDocument();
    expect(screen.getByRole('menuitem', { name: 'Delete comment' })).toBeEnabled();
    const jump = () => { host.scrollTop = 900; };
    editor.on('update', jump);
    fireEvent.click(screen.getByRole('menuitem', { name: 'Delete comment' }));
    editor.off('update', jump);
    expect(host.scrollTop).toBe(120);
    expect(Array.from(editor.view.dom.querySelectorAll('p')).map((p) => p.textContent))
      .toEqual(['Keep this comment', 'Original paragraph']);
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue('Keep this prompt');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(editor.isEditable).toBe(!readOnly);
    fireEvent.contextMenu(editor.view.dom.querySelectorAll('p')[1]);
    expect(screen.getAllByRole('menuitem').at(-1)).toHaveTextContent('Add comment');
    fireEvent.keyDown(screen.getByRole('menu'), { key: 'Escape' });
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).toHaveBeenCalledOnce();
    first.unmount();
    render(<Harness {...props} />);
    const reopened = await openedEditor();
    expect(reopened.getText()).toBe('Keep this comment\n\nOriginal paragraph');
    expect(reopened.isEditable).toBe(!readOnly);
  });

  it('keeps Add comment on an ordinary highlighted paragraph', async () => {
    render(<Harness items={[{ ...plan, read_only: true }]} getNote={async (id) => ({ id, document_id: id,
      document: { type: 'doc', content: [{ type: 'paragraph', content: [
        { type: 'text', text: 'Highlighted note', marks: [{ type: 'highlight' }] },
      ] }] },
    })} />);
    const editor = await openedEditor();
    fireEvent.contextMenu(editor.view.dom.querySelector('mark')!);
    expect(screen.getByRole('menuitem', { name: 'Add comment' })).toBeEnabled();
    expect(screen.queryByRole('menuitem', { name: 'Delete comment' })).not.toBeInTheDocument();
  });

  it('adds a comment before the paragraph nearest to a click between paragraphs', async () => {
    render(<Harness items={[{ ...plan, read_only: true }]} draft="Gap comment" getNote={async (id) => ({
      id, document_id: id, document: { type: 'doc', content: [...documentWith('First').content, ...documentWith('Second').content] },
    })} />);
    const editor = await openedEditor();
    const posAtCoords = vi.spyOn(editor.view, 'posAtCoords').mockReturnValue({ pos: 8, inside: -1 });
    fireEvent.contextMenu(editor.view.dom, { clientX: 25, clientY: 40 });
    fireEvent.click(screen.getByRole('menuitem', { name: 'Add comment' }));
    expect(posAtCoords).toHaveBeenCalledWith({ left: 25, top: 40 });
    expect(Array.from(editor.view.dom.querySelectorAll('p')).map((p) => p.textContent))
      .toEqual(['First', 'Gap comment', 'Second']);
  });

  it.each(['', ' \n '])('does nothing when Add comment is used with an empty prompt (%j)', async (draft) => {
    const saveNote = vi.fn();
    render(<Harness items={[{ ...plan, read_only: true }]} draft={draft} saveNote={saveNote} />);
    const editor = await openedEditor();
    const before = editor.getJSON();
    vi.useFakeTimers();
    fireEvent.contextMenu(editor.view.dom.querySelector('p')!);
    expect(screen.getByRole('menuitem', { name: 'Add comment' })).toBeEnabled();
    fireEvent.click(screen.getByRole('menuitem', { name: 'Add comment' }));
    expect(editor.getJSON()).toEqual(before);
    expect(screen.getByRole('textbox', { name: 'Prompt' })).toHaveValue(draft);
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).not.toHaveBeenCalled();
  });

  it('formats the selected text through the menu and keeps each action undoable', async () => {
    render(<Harness />);
    const editor = await openedEditor();
    act(() => { editor.commands.setTextSelection({ from: 2, to: 5 }); });
    for (const command of ['Bold', 'Italic', 'Highlight']) {
      fireEvent.contextMenu(editor.view.dom.querySelector('p')!);
      fireEvent.click(screen.getByRole('menuitem', { name: command }));
    }
    expect(editor.view.dom.querySelector('strong em mark')?.textContent).toBe('lph');
    expect(editor.getText()).toBe('Alpha');
    expect(screen.queryByRole('menu')).not.toBeInTheDocument();
    expect(editor.view.dom).toHaveFocus();
    fireEvent.click(historyAction('Undo'));
    expect(editor.view.dom.querySelector('mark')).toBeNull();
    expect(editor.view.dom.querySelector('strong em')?.textContent).toBe('lph');
  });

  it('copies the selected text in read-only mode', async () => {
    const exec = vi.fn(() => true);
    Object.defineProperty(document, 'execCommand', { configurable: true, value: exec });
    try {
      render(<Harness items={[{ ...plan, read_only: true }]} />);
      const editor = await openedEditor();
      act(() => { editor.commands.setTextSelection({ from: 2, to: 5 }); });
      fireEvent.contextMenu(editor.view.dom.querySelector('p')!);
      fireEvent.click(screen.getByRole('menuitem', { name: 'Copy' }));
      expect(exec).toHaveBeenCalledWith('copy');
      expect(window.getSelection()?.toString()).toBe('lph');
      expect(editor.getText()).toBe('Alpha');
    } finally {
      Reflect.deleteProperty(document, 'execCommand');
    }
  });

  it.each(['toolbar', 'context menu'])('saves read-only highlights from the %s, restores them on reopen, and saves their removal', async (source) => {
    function highlight(editor: Editor) {
      if (source === 'context menu') {
        fireEvent.contextMenu(editor.view.dom.querySelector('p')!);
        fireEvent.click(screen.getByRole('menuitem', { name: 'Highlight' }));
      } else {
        const button = screen.getByRole('button', { name: 'Highlight' });
        expect(button).toBeEnabled();
        fireEvent.click(button);
      }
    }
    let saved: Record<string, unknown> = documentWith('Alpha');
    const saveNote = vi.fn(async (_id: number, document: Record<string, unknown>) => { saved = document; });
    const props = { items: [{ ...plan, read_only: true }], saveNote,
      getNote: async (id: number) => ({ id, document_id: id, document: saved }) };
    const first = render(<Harness {...props} />);
    const editor = await openedEditor();
    vi.useFakeTimers();
    act(() => { editor.commands.setTextSelection({ from: 2, to: 5 }); });
    highlight(editor);
    expect(editor.isEditable).toBe(false);
    expect(editor.view.dom.querySelector('mark')?.textContent).toBe('lph');
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).toHaveBeenCalledOnce();
    first.unmount();
    render(<Harness {...props} />);
    const reopened = await openedEditor();
    expect(reopened.isEditable).toBe(false);
    expect(reopened.view.dom.querySelector('mark')?.textContent).toBe('lph');
    act(() => { reopened.commands.setTextSelection({ from: 2, to: 5 }); });
    highlight(reopened);
    expect(reopened.view.dom.querySelector('mark')).toBeNull();
    await act(async () => { await vi.advanceTimersByTimeAsync(noteSaveDelayMs); });
    expect(saveNote).toHaveBeenCalledTimes(2);
    expect(reopened.getText()).toBe('Alpha');
  });
});

it('applies a source destination once and preserves later selection when read-only metadata changes', async () => {
  const destination: AnnotationDestination = { entryType: 'highlight', ordinal: 1, prefix: 'Target' };
  const getNote = async (id: number) => ({ id, document_id: id, document: { type: 'doc', content: [
    { type: 'paragraph', content: [{ type: 'text', text: 'Target', marks: [{ type: 'highlight' }] }] },
    { type: 'paragraph', content: [{ type: 'text', text: 'Other' }] },
  ] } });
  const view = render(<Harness getNote={getNote} destination={destination} />);
  const editor = await openedEditor();
  expect(editor.state.selection.from).toBe(1);
  expect(editor.state.selection.to).toBe(7);
  act(() => { editor.commands.setTextSelection(10); });
  view.rerender(<Harness getNote={getNote} destination={destination} items={[{ ...plan, read_only: true }]} />);
  expect(editor.state.selection.from).toBe(10);
  expect(editor.state.selection.empty).toBe(true);
});

it.each([false, true])('keeps the source target visible as pictures load until the reader interacts (moved: %s)', async (moved) => {
  vi.spyOn(HTMLImageElement.prototype, 'complete', 'get').mockReturnValue(false);
  const destination: AnnotationDestination = { entryType: 'highlight', ordinal: 1, prefix: 'Target' };
  render(<Harness destination={destination} getNote={async (id) => ({
    id, document_id: id, scroll_top: 680, document: { type: 'doc', content: [
      { type: 'image', attrs: { src: 'data:image/png;base64,iVBORw0KGgo=' } },
      { type: 'paragraph', content: [{ type: 'text', text: 'Target', marks: [{ type: 'highlight' }] }] },
      ...documentWith('Other').content,
    ] },
  })} />);
  const editor = await openedEditor();
  const host = document.querySelector('.cha-notes-document')!;
  expect(host.scrollTop).not.toBe(680);
  expect(editor.state.doc.textBetween(editor.state.selection.from, editor.state.selection.to)).toBe('Target');
  const position = vi.spyOn(editor.view, 'coordsAtPos').mockReturnValue({ top: 200, bottom: 220, left: 0, right: 1 });
  host.scrollTop = 100;
  if (moved) fireEvent.wheel(host);
  fireEvent.load(editor.view.dom.querySelector('img')!);
  expect(position).toHaveBeenCalledTimes(moved ? 0 : 1);
  expect(host.scrollTop).toBe(moved ? 100 : 300);
  expect(editor.can().undo()).toBe(false);
});
