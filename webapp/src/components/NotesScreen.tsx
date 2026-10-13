import type { Editor } from '@tiptap/core';
import type { Content } from '@tiptap/core';
import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type Dispatch,
  type MutableRefObject,
} from 'react';
import { createPortal } from 'react-dom';

import { publicErrorMessage, type ChaClient } from '../api/client';
import type { NoteItem, NotesClient } from '../api/nativeClient';
import { createNotesEditor, selectNoteEnd } from '../notes/editor';
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
}) {
  const [phase, setPhase] = useState<Phase>(activeNoteId === null ? 'empty' : 'loading');
  const [openError, setOpenError] = useState<string | null>(null);
  const [promptError, setPromptError] = useState<string | null>(null);
  const [editor, setEditor] = useState<Editor | null>(null);
  const editorRef = useRef<Editor | null>(null);
  const hostRef = useRef<HTMLDivElement | null>(null);
  const name = noteItems.find((item) => item.id === activeNoteId)?.name ?? 'note';
  const noteIdRef = useRef(activeNoteId);
  const nameRef = useRef(name);
  const clientRef = useRef(client);
  noteIdRef.current = activeNoteId;
  nameRef.current = name;
  clientRef.current = client;

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
    editorLiveRef.current = editor;
    const dom = editor.view.dom as HTMLElement & { chaEditor?: Editor };
    dom.chaEditor = editor;
    return () => {
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

  return (
    <section className="cha-notes-screen" aria-label="Note">
      <div className="cha-notes-document" ref={hostRef}>
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
        onSubmit={async () => ({ clear_input: false })}
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
        submitEnabled={false}
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
