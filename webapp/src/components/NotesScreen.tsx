import type { Editor } from '@tiptap/core';
import type { Content } from '@tiptap/core';
import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type Dispatch,
  type KeyboardEvent as ReactKeyboardEvent,
  type MutableRefObject,
} from 'react';
import { createPortal } from 'react-dom';

import { publicErrorMessage, type ChaClient } from '../api/client';
import type { NoteItem, NotesClient } from '../api/nativeClient';
import { createNotesEditor, insertPromptParagraph, selectNoteEnd, withIsolatedHistory } from '../notes/editor';
import { noteItemPath } from '../notes/tree';
import type { useNoteLeave } from '../notes/useNoteLeave';
import { noteSaveFailedMessage, useNoteSave, type NoteSaveHandle } from '../notes/useNoteSave';
import type { AppAction } from '../state/view';
import { PromptInput } from './PromptInput';
import { SidebarToggle } from './TopBar';

type LeaveHandle = Pick<
  ReturnType<typeof useNoteLeave>,
  'decisionOpen' | 'retry' | 'stay' | 'discard'
>;

type Phase = 'empty' | 'loading' | 'ready' | 'error';

function saveDetail(message: string): string {
  return message === noteSaveFailedMessage ? 'Check the vault and try again.' : message;
}

const headingLevels = [1, 2, 3, 4, 5, 6] as const;

function headingValue(editor: Editor): string {
  for (const level of headingLevels) {
    if (editor.isActive('heading', { level })) return String(level);
  }
  return 'paragraph';
}

function applyBlock(editor: Editor, value: string) {
  if (!editor.isEditable || editor.isDestroyed) return;
  if (value === 'paragraph') {
    if (editor.isActive('paragraph')) return;
    withIsolatedHistory(editor, () => editor.chain().setParagraph().run());
    return;
  }
  const level = headingLevels.find((item) => String(item) === value);
  if (level === undefined || editor.isActive('heading', { level })) return;
  withIsolatedHistory(editor, () => editor.chain().setHeading({ level }).run());
}

function runMark(editor: Editor, command: () => boolean) {
  if (!editor.isEditable || editor.isDestroyed) return;
  withIsolatedHistory(editor, command);
}

function runHistory(editor: Editor, command: 'undo' | 'redo') {
  if (!editor.isEditable || editor.isDestroyed) return;
  if (command === 'undo') editor.chain().undo().run();
  else editor.chain().redo().run();
}

function FormatBar({ editor }: { editor: Editor }) {
  const [, setTick] = useState(0);
  const [listOpen, setListOpen] = useState(false);
  const listRoot = useRef<HTMLDivElement | null>(null);
  const listMenu = useRef<HTMLDivElement | null>(null);
  const listButton = useRef<HTMLButtonElement | null>(null);
  const editable = editor.isEditable && !editor.isDestroyed;

  useEffect(() => {
    const bump = () => setTick((value) => value + 1);
    editor.on('transaction', bump);
    editor.on('selectionUpdate', bump);
    editor.on('focus', bump);
    editor.on('blur', bump);
    return () => {
      editor.off('transaction', bump);
      editor.off('selectionUpdate', bump);
      editor.off('focus', bump);
      editor.off('blur', bump);
    };
  }, [editor]);

  useEffect(() => {
    if (!listOpen) return;
    listMenu.current?.querySelector<HTMLButtonElement>('button')?.focus();
    const dismiss = (event: PointerEvent) => {
      const target = event.target;
      if (target instanceof Node && listRoot.current?.contains(target)) return;
      setListOpen(false);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => window.removeEventListener('pointerdown', dismiss);
  }, [listOpen]);

  function onListKey(event: ReactKeyboardEvent<HTMLDivElement>) {
    const items = [...event.currentTarget.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')];
    const current = items.indexOf(document.activeElement as HTMLButtonElement);
    let next = current;
    if (event.key === 'ArrowDown') next = (current + 1) % items.length;
    else if (event.key === 'ArrowUp') next = (current - 1 + items.length) % items.length;
    else if (event.key === 'Home') next = 0;
    else if (event.key === 'End') next = items.length - 1;
    else if (event.key === 'Escape') {
      event.preventDefault();
      setListOpen(false);
      listButton.current?.focus();
      return;
    } else return;
    event.preventDefault();
    items[next]?.focus();
  }

  function chooseList(list: 'bullet' | 'ordered') {
    setListOpen(false);
    runMark(editor, () => (
      list === 'bullet' ? editor.chain().toggleBulletList().run() : editor.chain().toggleOrderedList().run()
    ));
  }

  return (
    <div className="cha-notes-toolbar">
      <select
        aria-label="Paragraph style"
        className="cha-form-control"
        disabled={!editable}
        onChange={(event) => applyBlock(editor, event.target.value)}
        value={headingValue(editor)}
      >
        <option value="paragraph">Paragraph</option>
        {headingLevels.map((level) => (
          <option key={level} value={String(level)}>{`Heading ${level}`}</option>
        ))}
      </select>
      <button
        aria-label="Bold"
        aria-pressed={editor.isActive('bold')}
        className="cha-button"
        disabled={!editable}
        onClick={() => runMark(editor, () => editor.chain().toggleBold().run())}
        type="button"
      >
        B
      </button>
      <button
        aria-label="Italic"
        aria-pressed={editor.isActive('italic')}
        className="cha-button"
        disabled={!editable}
        onClick={() => runMark(editor, () => editor.chain().toggleItalic().run())}
        type="button"
      >
        I
      </button>
      <button
        aria-label="Underline"
        aria-pressed={editor.isActive('underline')}
        className="cha-button"
        disabled={!editable}
        onClick={() => runMark(editor, () => editor.chain().toggleUnderline().run())}
        type="button"
      >
        U
      </button>
      <button
        aria-label="Highlight"
        aria-pressed={editor.isActive('highlight')}
        className="cha-button"
        disabled={!editable}
        onClick={() => runMark(editor, () => editor.chain().toggleHighlight().run())}
        type="button"
      >
        Highlight
      </button>
      <div className="cha-notes-list" ref={listRoot}>
        <button
          aria-expanded={listOpen}
          aria-haspopup="menu"
          aria-pressed={editor.isActive('bulletList') || editor.isActive('orderedList')}
          className="cha-button"
          disabled={!editable}
          onClick={() => setListOpen((open) => !open)}
          ref={listButton}
          type="button"
        >
          List
        </button>
        {listOpen && (
          <div className="cha-notes-list-menu" onKeyDown={onListKey} ref={listMenu} role="menu">
            <button onClick={() => chooseList('bullet')} role="menuitem" type="button">Bullet list</button>
            <button onClick={() => chooseList('ordered')} role="menuitem" type="button">Numbered list</button>
          </div>
        )}
      </div>
      <button
        className="cha-button"
        disabled={!editable || !editor.can().undo()}
        onClick={() => runHistory(editor, 'undo')}
        type="button"
      >
        Undo
      </button>
      <button
        className="cha-button"
        disabled={!editable || !editor.can().redo()}
        onClick={() => runHistory(editor, 'redo')}
        type="button"
      >
        Redo
      </button>
    </div>
  );
}

export function NotesScreen({
  client,
  notesEpoch,
  noteRequest,
  activeNoteId,
  noteItems,
  draft,
  onDraftChange,
  sidebarOpen,
  dispatch,
  editorLiveRef,
  leave,
  onRegisterSave,
  onRegisterDispose,
  onNewNote,
}: {
  client: ChaClient & NotesClient;
  notesEpoch: number;
  noteRequest: number;
  activeNoteId: number | null;
  noteItems: readonly NoteItem[];
  draft: string;
  onDraftChange(next: string): void;
  sidebarOpen: boolean;
  dispatch: Dispatch<AppAction>;
  editorLiveRef: MutableRefObject<Editor | null>;
  leave: LeaveHandle;
  onRegisterSave(save: NoteSaveHandle | null): void;
  onRegisterDispose(dispose: () => void): void;
  onNewNote?(): void;
}) {
  const [phase, setPhase] = useState<Phase>(activeNoteId === null ? 'empty' : 'loading');
  const [editing, setEditing] = useState(false);
  const [openError, setOpenError] = useState<string | null>(null);
  const [promptError, setPromptError] = useState<string | null>(null);
  const [editor, setEditor] = useState<Editor | null>(null);
  const editorRef = useRef<Editor | null>(null);
  const hostRef = useRef<HTMLDivElement | null>(null);
  const name = noteItems.find((item) => item.id === activeNoteId)?.name ?? 'note';
  const noteIdRef = useRef(activeNoteId);
  const nameRef = useRef(name);
  const clientRef = useRef(client);
  const epochRef = useRef(notesEpoch);
  noteIdRef.current = activeNoteId;
  nameRef.current = name;
  clientRef.current = client;
  epochRef.current = notesEpoch;

  const save = useNoteSave({
    editor: phase === 'ready' ? editor : null,
    noteId: activeNoteId,
    contextKey: notesEpoch,
    save: async (document) => {
      const id = noteIdRef.current;
      if (id === null) return;
      await client.saveNote(id, document);
    },
  });

  useLayoutEffect(() => {
    onRegisterSave(save);
    return () => onRegisterSave(null);
  });

  useEffect(() => {
    if (activeNoteId === null) {
      setPhase('empty');
      setOpenError(null);
      return;
    }
    const noteId = activeNoteId;
    const noteName = nameRef.current;
    let cancelled = false;
    setPhase('loading');
    setOpenError(null);
    void (async () => {
      try {
        const content = await clientRef.current.getNote(noteId);
        if (cancelled) return;
        let rejected = false;
        let created: Editor;
        try {
          created = createNotesEditor({
            content: content.document as Content,
            onRejectedContent: () => { rejected = true; },
          });
        } catch {
          if (!cancelled) {
            setPhase('error');
            setOpenError(`Could not open “${noteName}”.`);
          }
          return;
        }
        if (cancelled || rejected) {
          if (!created.isDestroyed) created.destroy();
          if (!cancelled && rejected) {
            setPhase('error');
            setOpenError(`Could not open “${noteName}”.`);
          }
          return;
        }
        selectNoteEnd(created);
        created.setEditable(false, false);
        editorRef.current = created;
        setEditor(created);
        setPhase('ready');
      } catch (failure: unknown) {
        if (cancelled) return;
        const detail = publicErrorMessage(failure, 'Could not open the note.');
        setPhase('error');
        setOpenError(detail === 'Could not open the note.'
          ? `Could not open “${noteName}”.`
          : `Could not open “${noteName}”. ${detail}`);
      }
    })();
    return () => {
      cancelled = true;
      const current = editorRef.current;
      editorRef.current = null;
      if (current && !current.isDestroyed) current.destroy();
      setEditor(null);
    };
  }, [activeNoteId, noteRequest, notesEpoch]);

  useEffect(() => {
    const host = hostRef.current;
    if (!editor || editor.isDestroyed) return;
    const element = editor.options.element;
    if (!host || !(element instanceof HTMLElement)) return;
    host.appendChild(element);
    return () => {
      element.remove();
    };
  }, [editor]);

  useEffect(() => {
    if (phase !== 'ready' || !editor || editor.isDestroyed) return;
    editor.setEditable(true, false);
    setEditing(true);
    editorLiveRef.current = editor;
    const dom = editor.view.dom as HTMLElement & { chaEditor?: Editor };
    dom.chaEditor = editor;
    return () => {
      setEditing(false);
      if (dom.chaEditor === editor) delete dom.chaEditor;
      if (editorLiveRef.current === editor) editorLiveRef.current = null;
    };
  }, [editor, editorLiveRef, phase]);

  useEffect(() => {
    onRegisterDispose(() => {
      if (editorLiveRef.current && editorLiveRef.current === editorRef.current) {
        editorLiveRef.current = null;
      }
      const current = editorRef.current;
      editorRef.current = null;
      if (current && !current.isDestroyed) current.destroy();
      setEditor(null);
    });
    return () => onRegisterDispose(() => undefined);
  }, [editorLiveRef, onRegisterDispose]);

  const path = activeNoteId === null ? '' : noteItemPath(noteItems, activeNoteId);
  const sendLabel = path ? `Send to ${path}` : 'Send';
  const failure = save.saveError ? saveDetail(save.saveError.message) : null;
  const submitEnabled = phase === 'ready'
    && editing
    && editor !== null
    && !editor.isDestroyed
    && editor.isEditable
    && !save.frozen;

  return (
    <section className="cha-notes-screen" aria-label="Note">
      {phase === 'ready' && editor && <FormatBar editor={editor} />}
      <div className="cha-notes-document" ref={hostRef}>
        {phase === 'empty' && (
          <button className="cha-button" onClick={() => onNewNote?.()} type="button">New note</button>
        )}
        {phase === 'loading' && <p role="status">Opening…</p>}
        {phase === 'error' && (
          <div role="alert">
            <p>{openError}</p>
            <button
              className="cha-button"
              onClick={() => {
                if (activeNoteId === null) return;
                dispatch({ type: 'open-note', noteId: activeNoteId });
              }}
              type="button"
            >
              Retry
            </button>
          </div>
        )}
      </div>
      {failure && (
        <div className="cha-notes-status" role="alert">
          <p>Could not save “{name}”. {failure}</p>
          <button className="cha-button" onClick={() => save.submitNow()} type="button">Retry</button>
        </div>
      )}
      {promptError && <p className="cha-error-message" role="alert">{promptError}</p>}
      <PromptInput
        key={activeNoteId}
        client={client}
        enabled
        inputLabel="Prompt"
        microphoneEnabled
        onChange={onDraftChange}
        onError={setPromptError}
        onSubmit={async (text) => {
          const current = editorRef.current;
          const noteId = noteIdRef.current;
          const epoch = epochRef.current;
          if (!current || current.isDestroyed || !current.isEditable || noteId === null) {
            return { clear_input: false };
          }
          const accepted = insertPromptParagraph(current, text);
          if (
            !accepted
            || editorRef.current !== current
            || noteIdRef.current !== noteId
            || epochRef.current !== epoch
          ) {
            return { clear_input: false };
          }
          return { clear_input: true };
        }}
        statusRow={({ transliteration }) => (
          <div className="cha-chat-status">
            <SidebarToggle
              className="cha-sidebar-toggle"
              dispatch={dispatch}
              sidebarOpen={sidebarOpen}
            />
            {transliteration}
          </div>
        )}
        submitEnabled={submitEnabled}
        submitLabel={sendLabel}
        submitTitle={sendLabel}
        value={draft}
      />
      {leave.decisionOpen && (
        <LeaveDialog
          detail={failure}
          name={name}
          onDiscard={leave.discard}
          onRetry={leave.retry}
          onStay={leave.stay}
        />
      )}
    </section>
  );
}

function LeaveDialog({
  detail,
  name,
  onDiscard,
  onRetry,
  onStay,
}: {
  detail: string | null;
  name: string;
  onDiscard(): void;
  onRetry(): void;
  onStay(): void;
}) {
  const dialog = useRef<HTMLDialogElement | null>(null);
  const stayButton = useRef<HTMLButtonElement | null>(null);
  const explanation = detail && detail !== 'Check the vault and try again.' ? `${detail} ` : '';

  useEffect(() => {
    const element = dialog.current;
    if (!element) return;
    if (typeof element.showModal === 'function') element.showModal();
    else element.setAttribute('open', '');
    stayButton.current?.focus();
    return () => {
      if (typeof element.close === 'function') element.close();
    };
  }, []);

  return createPortal(
    <dialog
      aria-label={`Could not save “${name}”.`}
      className="cha-dialog"
      onCancel={(event) => {
        event.preventDefault();
        onStay();
      }}
      ref={dialog}
    >
      <h2>Could not save “{name}”.</h2>
      <p>{explanation}Discard unsaved changes and continue?</p>
      <div className="cha-dialog-actions">
        <button className="cha-button" onClick={onRetry} type="button">Retry</button>
        <button className="cha-button cha-button-primary" onClick={onStay} ref={stayButton} type="button">
          Stay
        </button>
        <button className="cha-button cha-button-ghost" onClick={onDiscard} type="button">Discard</button>
      </div>
    </dialog>,
    document.body,
  );
}
