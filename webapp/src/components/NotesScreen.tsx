import type { Editor } from '@tiptap/core';
import type { Content } from '@tiptap/core';
import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type Dispatch,
  type MutableRefObject,
  type ReactNode,
} from 'react';
import { createPortal } from 'react-dom';

import {
  publicErrorMessage,
  type ChaClient,
  type NoteItem,
  type NotesClient,
} from '../api/client';
import { readTextFile, textFileAccept } from '../textFile';
import {
  appendNoteHtml,
  createNotesEditor,
  importPictureFile,
  insertPromptParagraph,
  linkRangeAtTarget,
  noteHeadings,
  removeNoteLink,
  replaceLinkDestination,
  replaceNoteHtml,
  selectNoteEnd,
  selectNoteHeading,
  withIsolatedHistory,
  type LinkEditResult,
  type NoteLinkRange,
} from '../notes/editor';
import { noteTooLargeMessage } from '../notes/pictures';
import { notePageHtml, notePageName, promptPageUrl } from '../notes/pageImport';
import { noteFileHtml, noteFileTooLarge, promptFilePath } from '../notes/fileImport';
import { availableNoteName, noteItemPath } from '../notes/tree';
import type { useNoteLeave } from '../notes/useNoteLeave';
import { noteSaveFailedMessage, useNoteSave, type NoteSaveHandle } from '../notes/useNoteSave';
import type { DataAction, NavigationAction } from '../state/view';
import {
  CloseIcon,
  CodeIcon,
  FileUpIcon,
  HighlighterIcon,
  ListIcon,
  PictureIcon,
  RedoIcon,
  SearchIcon,
  TextLinesIcon,
  UndoIcon,
} from './Icons';
import { menuKeys, type NoteActions } from './NotesTree';
import { PromptInput } from './PromptInput';
import { SidebarToggle } from './TopBar';

type LeaveHandle = Pick<
  ReturnType<typeof useNoteLeave>,
  'decisionOpen' | 'retry' | 'stay' | 'discard'
>;

type Phase = 'empty' | 'loading' | 'ready' | 'error';

const defaultNoteName = /^New note(?: [1-9]\d*)?$/u;

function saveDetail(message: string): string {
  return message === noteSaveFailedMessage ? 'Check the vault and try again.' : message;
}

function preserveNoteScroll(editor: Editor, run: () => void) {
  const host = editor.view.dom.closest('.cha-notes-document');
  const top = host?.scrollTop ?? 0;
  const left = host?.scrollLeft ?? 0;
  run();
  if (host) {
    host.scrollTop = top;
    host.scrollLeft = left;
  }
}

function focusNotesEditor(editor: Editor | null) {
  if (!editor || editor.isDestroyed || !editor.isEditable) return;
  // Restoring the DOM selection can scroll to an offscreen caret even when
  // focus itself uses preventScroll.
  preserveNoteScroll(editor, () => editor.view.focus());
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

function FormatBar({
  editor,
  fontSize,
  onFontSizeChange,
  toolbarRef,
  onFind,
  onPicture,
}: {
  editor: Editor;
  fontSize: number;
  onFontSizeChange(size: number): void;
  toolbarRef: { current: HTMLDivElement | null };
  onFind(): void;
  onPicture(file: File): void;
}) {
  const [, setTick] = useState(0);
  const [listOpen, setListOpen] = useState(false);
  const [headingsOpen, setHeadingsOpen] = useState(false);
  const listRoot = useRef<HTMLDivElement | null>(null);
  const listMenu = useRef<HTMLDivElement | null>(null);
  const headingsRoot = useRef<HTMLDivElement | null>(null);
  const headingsMenu = useRef<HTMLDivElement | null>(null);
  const pictureInput = useRef<HTMLInputElement | null>(null);
  const fontAnchor = useRef<{ block: Element; share: number } | null>(null);
  const editable = editor.isEditable && !editor.isDestroyed;
  const headings = noteHeadings(editor.state.doc);
  const showHeadings = headingsOpen && editable && headings.length > 0;

  function format(command: () => boolean) {
    preserveNoteScroll(editor, () => runMark(editor, command));
  }

  // A font size change keeps the text at the top of the note in place. The
  // same pixel offset would show other text.
  function changeFontSize(size: number) {
    const host = editor.view.dom.closest('.cha-notes-document');
    const top = host?.getBoundingClientRect().top ?? 0;
    const block = Array.from(editor.view.dom.children)
      .find((node) => node.getBoundingClientRect().bottom > top);
    const box = block?.getBoundingClientRect();
    if (block && box && box.height > 0) fontAnchor.current = { block, share: (top - box.top) / box.height };
    onFontSizeChange(size);
  }

  useLayoutEffect(() => {
    const anchor = fontAnchor.current;
    fontAnchor.current = null;
    const host = editor.view.dom.closest('.cha-notes-document');
    if (!anchor || !host || !anchor.block.isConnected) return;
    const box = anchor.block.getBoundingClientRect();
    host.scrollTop += box.top + anchor.share * box.height - host.getBoundingClientRect().top;
  }, [editor, fontSize]);

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

  // A menu that loses its entries or editor closes, so it cannot reappear later
  // and take focus from the editor.
  useEffect(() => {
    if (!showHeadings) {
      setHeadingsOpen(false);
      return;
    }
    headingsMenu.current?.querySelector<HTMLButtonElement>('button')?.focus();
    const dismiss = (event: PointerEvent) => {
      const target = event.target;
      if (target instanceof Node && headingsRoot.current?.contains(target)) return;
      setHeadingsOpen(false);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => window.removeEventListener('pointerdown', dismiss);
  }, [showHeadings]);

  function chooseHeading(position: number) {
    setHeadingsOpen(false);
    if (!selectNoteHeading(editor, position)) return;
    focusNotesEditor(editor);
    const element = editor.view.nodeDOM(position);
    if (element instanceof Element) element.scrollIntoView({ block: 'start', inline: 'nearest' });
  }

  function chooseList(list: 'bullet' | 'ordered') {
    setListOpen(false);
    format(() => (
      list === 'bullet' ? editor.chain().toggleBulletList().run() : editor.chain().toggleOrderedList().run()
    ));
  }

  // Each toolbar action returns focus to the note. A menu or dialog that an
  // action opens then focuses its own first control.
  return (
    <div className="cha-notes-toolbar" onClick={() => focusNotesEditor(editor)} ref={toolbarRef}>
      <div className="cha-notes-toolbar-group">
        <button
          aria-label="Undo"
          title="Undo"
          className="cha-button"
          disabled={!editable || !editor.can().undo()}
          onClick={() => runHistory(editor, 'undo')}
          type="button"
        >
          <UndoIcon />
        </button>
        <button
          aria-label="Redo"
          title="Redo"
          className="cha-button"
          disabled={!editable || !editor.can().redo()}
          onClick={() => runHistory(editor, 'redo')}
          type="button"
        >
          <RedoIcon />
        </button>
      </div>
      <div className="cha-notes-toolbar-group">
        <button
          aria-label="Bold"
          title="Bold"
          aria-pressed={editor.isActive('bold')}
          className="cha-button"
          disabled={!editable}
          onClick={() => format(() => editor.chain().toggleBold().run())}
          type="button"
        >
          B
        </button>
        <button
          aria-label="Italic"
          title="Italic"
          aria-pressed={editor.isActive('italic')}
          className="cha-button"
          disabled={!editable}
          onClick={() => format(() => editor.chain().toggleItalic().run())}
          type="button"
        >
          I
        </button>
        <button
          aria-label="Underline"
          title="Underline"
          aria-pressed={editor.isActive('underline')}
          className="cha-button"
          disabled={!editable}
          onClick={() => format(() => editor.chain().toggleUnderline().run())}
          type="button"
        >
          U
        </button>
        <button
          aria-label="Highlight"
          title="Highlight"
          aria-pressed={editor.isActive('highlight')}
          className="cha-button"
          disabled={!editable}
          onClick={() => format(() => editor.chain().toggleHighlight().run())}
          type="button"
        >
          <HighlighterIcon />
        </button>
      </div>
      <div className="cha-notes-toolbar-group">
        <button
          aria-label="Decrease editor font size"
          title="Decrease font size"
          className="cha-button"
          disabled={fontSize <= 10}
          onClick={() => changeFontSize(Math.max(10, fontSize - 2))}
          type="button"
        >
          A−
        </button>
        <button
          aria-label="Increase editor font size"
          title="Increase font size"
          className="cha-button"
          disabled={fontSize >= 30}
          onClick={() => changeFontSize(Math.min(30, fontSize + 2))}
          type="button"
        >
          A+
        </button>
      </div>
      <div className="cha-notes-toolbar-group">
        {([1, 2, 3] as const).map((level) => (
          <button
            aria-label={`Heading ${level}`}
            aria-pressed={editor.isActive('heading', { level })}
            className="cha-button cha-notes-heading"
            disabled={!editable}
            key={level}
            onClick={() => format(() => editor.chain().toggleHeading({ level }).run())}
            title={`Heading ${level}`}
            type="button"
          >
            H<sub>{level}</sub>
          </button>
        ))}
      </div>
      <div className="cha-notes-toolbar-group">
        <div className="cha-notes-list" ref={listRoot}>
          <button
            aria-label="List"
            title="List"
            aria-expanded={listOpen}
            aria-haspopup="menu"
            aria-pressed={editor.isActive('bulletList') || editor.isActive('orderedList')}
            className="cha-button"
            disabled={!editable}
            onClick={() => setListOpen((open) => !open)}
            type="button"
          >
            <ListIcon />
          </button>
          {listOpen && (
            <div
              className="cha-notes-list-menu"
              onKeyDown={(event) => menuKeys(event, () => {
                setListOpen(false);
                focusNotesEditor(editor);
              })}
              ref={listMenu}
              role="menu"
            >
              <button onClick={() => chooseList('bullet')} role="menuitem" type="button">Bullet list</button>
              <button onClick={() => chooseList('ordered')} role="menuitem" type="button">Numbered list</button>
            </div>
          )}
        </div>
        <input
          accept=".png,.jpg,.jpeg,.webp,.gif,image/png,image/jpeg,image/webp,image/gif"
          aria-hidden="true"
          hidden
          onChange={(event) => {
            const file = event.target.files?.[0];
            event.target.value = '';
            if (file) onPicture(file);
          }}
          ref={pictureInput}
          tabIndex={-1}
          type="file"
        />
        <button
          aria-label="Picture"
          title="Picture"
          className="cha-button"
          disabled={!editable}
          onClick={() => pictureInput.current?.click()}
          type="button"
        >
          <PictureIcon />
        </button>
        <button
          aria-label="Code"
          title="Code"
          aria-pressed={editor.isActive('code')}
          className="cha-button cha-notes-code"
          disabled={!editable}
          onClick={() => format(() => editor.chain().toggleCode().run())}
          type="button"
        >
          <CodeIcon />
        </button>
      </div>
      <div className="cha-notes-toolbar-group">
        <button aria-label="Find" className="cha-button" onClick={onFind} title="Find" type="button">
          <SearchIcon />
        </button>
        <div className="cha-notes-heading-jump" ref={headingsRoot}>
          <button
            aria-label="Jump to heading"
            title="Jump to heading"
            aria-expanded={showHeadings}
            aria-haspopup="menu"
            className="cha-button"
            disabled={!editable || headings.length === 0}
            onClick={() => setHeadingsOpen((open) => !open)}
            type="button"
          >
            <TextLinesIcon />
          </button>
          {showHeadings && (
            <div
              className="cha-notes-list-menu cha-notes-heading-menu"
              onKeyDown={(event) => menuKeys(event, () => {
                setHeadingsOpen(false);
                focusNotesEditor(editor);
              })}
              ref={headingsMenu}
              role="menu"
            >
              {headings.map((heading) => (
                <button
                  key={heading.position}
                  onClick={() => chooseHeading(heading.position)}
                  role="menuitem"
                  style={{ paddingInlineStart: `${0.6 + (heading.level - 1) * 0.75}rem` }}
                  type="button"
                >
                  {heading.text}
                </button>
              ))}
            </div>
          )}
        </div>
      </div>
    </div>
  );
}

function FindBar({
  editor,
  findRef,
  findInputRef,
  term,
  replacement,
  matchCase,
  wholeWord,
  regex,
  onTerm,
  onReplacement,
  onMatchCase,
  onWholeWord,
  onRegex,
  onClose,
}: {
  editor: Editor;
  findRef: { current: HTMLDivElement | null };
  findInputRef: { current: HTMLInputElement | null };
  term: string;
  replacement: string;
  matchCase: boolean;
  wholeWord: boolean;
  regex: boolean;
  onTerm(value: string): void;
  onReplacement(value: string): void;
  onMatchCase(value: boolean): void;
  onWholeWord(value: boolean): void;
  onRegex(value: boolean): void;
  onClose(): void;
}) {
  const [, setTick] = useState(0);
  useEffect(() => {
    const bump = () => setTick((value) => value + 1);
    editor.on('transaction', bump);
    return () => {
      editor.off('transaction', bump);
    };
  }, [editor]);

  const count = editor.storage.findAndReplace.results.length;
  const index = editor.storage.findAndReplace.currentIndex;
  const status = term === '' ? '' : count === 0 ? 'No matches' : `${(index ?? 0) + 1} / ${count}`;
  const canMove = count > 0;
  const canReplace = canMove && editor.isEditable && !editor.isDestroyed;

  function previous() {
    if (!canMove) return;
    editor.commands.goToPreviousResult();
    focusNotesEditor(editor);
  }

  function next() {
    if (!canMove) return;
    editor.commands.goToNextResult();
    focusNotesEditor(editor);
  }

  function replaceOne() {
    if (!canReplace) return;
    runMark(editor, () => editor.commands.replace());
    focusNotesEditor(editor);
  }

  function replaceEvery() {
    if (!canReplace) return;
    runMark(editor, () => editor.commands.replaceAll());
    focusNotesEditor(editor);
  }

  return (
    <div
      className="cha-notes-find"
      onKeyDown={(event) => {
        if (event.key !== 'Escape') return;
        event.preventDefault();
        onClose();
      }}
      ref={findRef}
    >
      <div className="cha-notes-find-row">
        <label className="cha-notes-find-field">
          Find
          <input
            className="cha-form-control"
            onChange={(event) => onTerm(event.target.value)}
            onKeyDown={(event) => {
              if (event.key !== 'Enter') return;
              event.preventDefault();
              if (event.shiftKey) previous();
              else next();
            }}
            ref={findInputRef}
            value={term}
          />
        </label>
        <div className="cha-notes-find-actions">
          <button className="cha-button" disabled={!canMove} onClick={previous} type="button">Previous</button>
          <button className="cha-button" disabled={!canMove} onClick={next} type="button">Next</button>
          <span role="status">{status}</span>
          <button aria-label="Close" className="cha-button" onClick={onClose} type="button">×</button>
        </div>
      </div>
      <div className="cha-notes-find-row">
        <label className="cha-notes-find-field">
          Replace
          <input
            className="cha-form-control"
            onChange={(event) => onReplacement(event.target.value)}
            onKeyDown={(event) => {
              if (event.key !== 'Enter') return;
              event.preventDefault();
              replaceOne();
            }}
            value={replacement}
          />
        </label>
        <div className="cha-notes-find-actions">
          <button className="cha-button" disabled={!canReplace} onClick={replaceOne} type="button">Replace</button>
          <button className="cha-button" disabled={!canReplace} onClick={replaceEvery} type="button">Replace all</button>
        </div>
      </div>
      <div className="cha-notes-find-row">
        <label>
          <input
            checked={matchCase}
            onChange={(event) => {
              onMatchCase(event.target.checked);
              focusNotesEditor(editor);
            }}
            type="checkbox"
          />
          Match case
        </label>
        <label>
          <input
            checked={wholeWord}
            disabled={regex}
            onChange={(event) => {
              onWholeWord(event.target.checked);
              focusNotesEditor(editor);
            }}
            type="checkbox"
          />
          Whole word
        </label>
        <label>
          <input
            checked={regex}
            onChange={(event) => {
              onRegex(event.target.checked);
              focusNotesEditor(editor);
            }}
            type="checkbox"
          />
          Regular expression
        </label>
      </div>
    </div>
  );
}

const linkEditErrors: Record<Exclude<LinkEditResult, 'applied' | 'unavailable'>, string> = {
  invalid: 'Enter an HTTP or HTTPS address.',
  missing: 'That link is no longer in the note.',
  rejected: noteTooLargeMessage,
};

function LinkEditDialog({
  initial,
  onApply,
  onCancel,
  onClosed,
}: {
  initial: string;
  onApply(value: string): string | null;
  onCancel(): void;
  onClosed(): void;
}) {
  const dialog = useRef<HTMLDialogElement | null>(null);
  const input = useRef<HTMLInputElement | null>(null);
  const closedRef = useRef(onClosed);
  closedRef.current = onClosed;
  const [value, setValue] = useState(initial);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    const element = dialog.current;
    if (!element) return;
    if (typeof element.showModal === 'function') element.showModal();
    else element.setAttribute('open', '');
    input.current?.focus();
    return () => {
      if (typeof element.close === 'function') element.close();
      // The menu that opened this dialog is gone, so focus returns to the editor.
      closedRef.current();
    };
  }, []);

  return createPortal(
    <dialog
      aria-label="Edit link"
      className="cha-dialog"
      onCancel={(event) => {
        event.preventDefault();
        onCancel();
      }}
      ref={dialog}
    >
      <form onSubmit={(event) => {
        event.preventDefault();
        setError(onApply(value));
      }}>
        <label>
          URL
          <input
            className="cha-form-control"
            onChange={(event) => {
              setValue(event.target.value);
              setError(null);
            }}
            ref={input}
            value={value}
          />
        </label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions">
          <button className="cha-button" onClick={onCancel} type="button">Cancel</button>
          <button className="cha-button cha-button-primary" type="submit">Apply</button>
        </div>
      </form>
    </dialog>,
    document.body,
  );
}

export function NotesScreen({
  client,
  notesEpoch,
  noteRequest,
  activeNoteId,
  noteItems,
  noteActions = null,
  draft,
  onDraftChange,
  sidebarOpen,
  dispatch,
  navigate,
  editorLiveRef,
  leave,
  onRegisterSave,
  onRegisterDispose,
  sessionStatus,
}: {
  client: ChaClient & NotesClient;
  notesEpoch: number;
  noteRequest: number;
  activeNoteId: number | null;
  noteItems: readonly NoteItem[];
  noteActions?: NoteActions | null;
  draft: string;
  onDraftChange(next: string): void;
  sidebarOpen: boolean;
  dispatch: Dispatch<DataAction>;
  navigate: Dispatch<NavigationAction>;
  editorLiveRef: MutableRefObject<Editor | null>;
  leave: LeaveHandle;
  onRegisterSave(save: NoteSaveHandle | null): void;
  onRegisterDispose(dispose: () => void): void;
  sessionStatus?: ReactNode;
}) {
  const [phase, setPhase] = useState<Phase>(activeNoteId === null ? 'empty' : 'loading');
  const [editing, setEditing] = useState(false);
  const [fontSize, setFontSize] = useState(14);
  const [openError, setOpenError] = useState<string | null>(null);
  const [promptError, setPromptError] = useState<string | null>(null);
  const [importingFile, setImportingFile] = useState(false);
  const [promptPending, setPromptPending] = useState(false);
  const fileInput = useRef<HTMLInputElement | null>(null);
  const [editor, setEditor] = useState<Editor | null>(null);
  const editorRef = useRef<Editor | null>(null);
  const hostRef = useRef<HTMLDivElement | null>(null);
  const name = noteItems.find((item) => item.id === activeNoteId)?.name ?? 'note';
  const noteIdRef = useRef(activeNoteId);
  const itemsRef = useRef(noteItems);
  const clientRef = useRef(client);
  const epochRef = useRef(notesEpoch);
  noteIdRef.current = activeNoteId;
  itemsRef.current = noteItems;
  clientRef.current = client;
  epochRef.current = notesEpoch;
  const [findOpen, setFindOpen] = useState(false);
  const [findTerm, setFindTerm] = useState('');
  const [replaceTerm, setReplaceTerm] = useState('');
  const [matchCase, setMatchCase] = useState(false);
  const [wholeWord, setWholeWord] = useState(false);
  const [useRegex, setUseRegex] = useState(false);
  const [commandError, setCommandError] = useState<string | null>(null);
  const [linkEdit, setLinkEdit] = useState<NoteLinkRange | null>(null);
  const [linkMenu, setLinkMenu] = useState<{ x: number; y: number; range: NoteLinkRange } | null>(null);
  const toolbarRef = useRef<HTMLDivElement | null>(null);
  const findRef = useRef<HTMLDivElement | null>(null);
  const findInputRef = useRef<HTMLInputElement | null>(null);
  const linkContextRef = useRef<HTMLDivElement | null>(null);
  const findControls = useRef({
    term: '',
    replacement: '',
    matchCase: false,
    wholeWord: false,
    regex: false,
  });
  const openFindRef = useRef<() => void>(() => undefined);
  const findOpenRef = useRef(false);
  findOpenRef.current = findOpen;
  findControls.current = {
    term: findTerm,
    replacement: replaceTerm,
    matchCase,
    wholeWord,
    regex: useRegex,
  };

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
    const noteName = itemsRef.current.find((item) => item.id === noteId)?.name ?? 'note';
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
            readPicture: (url) => clientRef.current.readNotePicture(url),
            onRejectedContent: () => { rejected = true; },
            onCommandError: (message) => {
              if (cancelled || noteIdRef.current !== noteId) return;
              setCommandError(message);
            },
            // An accepted change means the user continued, so an old command error goes.
            onAcceptedDocument: () => {
              if (cancelled || noteIdRef.current !== noteId) return;
              setCommandError(null);
            },
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
    focusNotesEditor(editor);
    return () => {
      setEditing(false);
      if (dom.chaEditor === editor) delete dom.chaEditor;
      if (editorLiveRef.current === editor) editorLiveRef.current = null;
    };
  }, [editor, editorLiveRef, phase]);

  useEffect(() => {
    setFindOpen(false);
    setFindTerm('');
    setReplaceTerm('');
    setMatchCase(false);
    setWholeWord(false);
    setUseRegex(false);
    setCommandError(null);
    setPromptError(null);
    setPromptPending(false);
    setLinkEdit(null);
    setLinkMenu(null);
  }, [activeNoteId]);

  useEffect(() => {
    if (!findOpen) return;
    findInputRef.current?.focus();
  }, [findOpen]);

  function applyFind(current: Editor) {
    const controls = findControls.current;
    current.commands.setCaseSensitive(controls.matchCase);
    current.commands.setUseRegex(controls.regex);
    current.commands.setWholeWord(controls.regex ? false : controls.wholeWord);
    current.commands.setReplaceTerm(controls.replacement);
    current.commands.setSearchTerm(controls.term);
  }

  useEffect(() => {
    const current = editor;
    if (!findOpenRef.current || !current || current.isDestroyed) return;
    applyFind(current);
  }, [editor]);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      // event.code keeps Mod+F on the F key with a non-Latin layout.
      const isF = event.key.toLowerCase() === 'f' || event.code === 'KeyF';
      if (!isF || event.altKey || event.shiftKey) return;
      if (!event.metaKey && !event.ctrlKey) return;
      const target = event.target;
      if (!(target instanceof Node)) return;
      const current = editorRef.current;
      const inEditor = !!current && !current.isDestroyed && current.view.dom.contains(target);
      const inToolbar = toolbarRef.current?.contains(target) ?? false;
      const inFind = findRef.current?.contains(target) ?? false;
      if (!inEditor && !inToolbar && !inFind) return;
      event.preventDefault();
      openFindRef.current();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, []);

  useEffect(() => {
    if (!editor || editor.isDestroyed || phase !== 'ready') return;
    const dom = editor.view.dom;
    const onContext = (event: Event) => {
      if (!(event instanceof MouseEvent) || !editor.isEditable || editor.isDestroyed) return;
      const range = linkRangeAtTarget(editor.view, event.target);
      if (!range) return;
      event.preventDefault();
      setLinkMenu({ x: event.clientX, y: event.clientY, range });
    };
    dom.addEventListener('contextmenu', onContext);
    return () => dom.removeEventListener('contextmenu', onContext);
  }, [editor, phase]);

  useEffect(() => {
    if (!linkMenu) return;
    linkContextRef.current?.querySelector<HTMLButtonElement>('button')?.focus();
    const dismiss = (event: PointerEvent) => {
      const target = event.target;
      if (target instanceof Node && linkContextRef.current?.contains(target)) return;
      setLinkMenu(null);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => window.removeEventListener('pointerdown', dismiss);
  }, [linkMenu]);

  function openFind() {
    const current = editorRef.current;
    if (!current || current.isDestroyed) return;
    setFindOpen(true);
    applyFind(current);
    queueMicrotask(() => findInputRef.current?.focus());
  }

  openFindRef.current = openFind;

  function closeFind() {
    setFindOpen(false);
    const current = editorRef.current;
    if (!current || current.isDestroyed) return;
    current.commands.clearSearch();
    focusNotesEditor(current);
  }

  function sameEditorContext(current: Editor, noteId: number, epoch: number): boolean {
    return editorRef.current === current
      && noteIdRef.current === noteId
      && epochRef.current === epoch
      && !current.isDestroyed
      && current.isEditable;
  }

  function onPicture(file: File) {
    const current = editorRef.current;
    const noteId = noteIdRef.current;
    const epoch = epochRef.current;
    if (!current || noteId === null) return;
    void importPictureFile(current, file, () => sameEditorContext(current, noteId, epoch)).then((message) => {
      if (!sameEditorContext(current, noteId, epoch)) return;
      if (message) setCommandError(message);
      focusNotesEditor(current);
    });
  }

  async function renameImportedNote(
    note: NoteItem, title: string, source: 'page' | 'file', stillCurrent: () => boolean,
  ) {
    try {
      const name = availableNoteName(itemsRef.current, note.parent_id, title, note.id);
      const renamed = await client.renameNoteItem(note.id, name);
      noteActions?.renamed(renamed);
    } catch (failure: unknown) {
      if (stillCurrent()) {
        setPromptError(`The ${source} was imported, but the note could not be renamed. `
          + publicErrorMessage(failure, 'Try renaming the note again.'));
      }
    }
  }

  async function loadNoteFile(file: File | string): Promise<boolean | null> {
    const current = editorRef.current;
    const noteId = noteIdRef.current;
    const epoch = epochRef.current;
    if (!current || noteId === null || !sameEditorContext(current, noteId, epoch)) return false;
    const stillCurrent = () => sameEditorContext(current, noteId, epoch);
    setImportingFile(true);
    setPromptError(null);
    try {
      let source;
      if (typeof file === 'string') {
        source = await client.readNoteTextFile(file);
      } else {
        try {
          source = { name: file.name, text: await readTextFile(file, noteFileTooLarge) };
        } catch (failure: unknown) {
          if (stillCurrent()) setPromptError((failure as Error).message);
          return false;
        }
      }
      if (!stillCurrent()) return false;
      // A prompt can start with a path and still be ordinary text.
      if (!source) return null;
      const { html, title, preserveWhitespace } = noteFileHtml(source);
      const accepted = await replaceNoteHtml(current, html,
        (url) => client.readNotePicture(url), stillCurrent, preserveWhitespace);
      if (!accepted || !stillCurrent()) return false;
      const note = itemsRef.current.find((item) => item.id === noteId);
      if (title && note && defaultNoteName.test(note.name)) {
        await renameImportedNote(note, title, 'file', stillCurrent);
      }
      return stillCurrent();
    } catch (failure: unknown) {
      if (stillCurrent()) setPromptError(publicErrorMessage(failure, 'The file could not be loaded. Try again.'));
      return false;
    } finally {
      setImportingFile(false);
    }
  }

  function onFindTerm(value: string) {
    setFindTerm(value);
    editorRef.current?.commands.setSearchTerm(value);
  }

  function onReplaceTerm(value: string) {
    setReplaceTerm(value);
    editorRef.current?.commands.setReplaceTerm(value);
  }

  function onMatchCase(value: boolean) {
    setMatchCase(value);
    editorRef.current?.commands.setCaseSensitive(value);
  }

  function onWholeWord(value: boolean) {
    setWholeWord(value);
    editorRef.current?.commands.setWholeWord(value);
  }

  function onFindRegex(value: boolean) {
    setUseRegex(value);
    const current = editorRef.current;
    if (!current || current.isDestroyed) return;
    current.commands.setUseRegex(value);
    if (value) {
      setWholeWord(false);
      current.commands.setWholeWord(false);
    }
  }

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
    && !importingFile
    && !save.frozen;

  return (
    <section className="cha-notes-screen" aria-label="Note">
      {phase === 'ready' && editor && (
        <FormatBar
          editor={editor}
          fontSize={fontSize}
          onFontSizeChange={setFontSize}
          onFind={openFind}
          onPicture={onPicture}
          toolbarRef={toolbarRef}
        />
      )}
      {phase === 'ready' && editor && findOpen && (
        <FindBar
          editor={editor}
          findInputRef={findInputRef}
          findRef={findRef}
          matchCase={matchCase}
          onClose={closeFind}
          onMatchCase={onMatchCase}
          onRegex={onFindRegex}
          onReplacement={onReplaceTerm}
          onTerm={onFindTerm}
          onWholeWord={onWholeWord}
          regex={useRegex}
          replacement={replaceTerm}
          term={findTerm}
          wholeWord={wholeWord}
        />
      )}
      <div className="cha-notes-document" ref={hostRef} style={{ fontSize }}>
        {phase === 'loading' && <p role="status">Opening…</p>}
        {phase === 'error' && (
          <div role="alert">
            <p>{openError}</p>
            <button
              className="cha-button"
              onClick={() => {
                if (activeNoteId === null) return;
                navigate({ type: 'open-note', noteId: activeNoteId });
              }}
              type="button"
            >
              Retry
            </button>
          </div>
        )}
      </div>
      {commandError && <p className="cha-error-message" role="alert">{commandError}</p>}
      {failure && (
        <div className="cha-notes-status" role="alert">
          <p>Could not save “{name}”. {failure}</p>
          <button className="cha-button" onClick={() => save.submitNow()} type="button">Retry</button>
        </div>
      )}
      {promptError && (
        <div className="cha-notes-status cha-error-message" role="alert">
          <p>{promptError}</p>
          <button
            aria-label="Dismiss error"
            className="cha-button"
            onClick={() => setPromptError(null)}
            title="Dismiss error"
            type="button"
          >
            <CloseIcon />
          </button>
        </div>
      )}
      {sessionStatus}
      <PromptInput
        key={activeNoteId}
        client={client}
        enabled
        inputLabel="Prompt"
        microphoneEnabled
        onChange={onDraftChange}
        onError={setPromptError}
        onSubmittingChange={setPromptPending}
        onSubmit={async (text) => {
          const current = editorRef.current;
          const noteId = noteIdRef.current;
          const epoch = epochRef.current;
          if (!current || current.isDestroyed || !current.isEditable || noteId === null) {
            return { clear_input: false };
          }
          const filePath = promptFilePath(text);
          if (filePath) {
            const imported = await loadNoteFile(filePath);
            if (imported !== null) return { clear_input: imported };
            if (!sameEditorContext(current, noteId, epoch)) return { clear_input: false };
          }
          const url = promptPageUrl(text);
          let accepted: boolean;
          if (url) {
            try {
              const page = await client.readNotePage(url);
              if (!sameEditorContext(current, noteId, epoch)) return { clear_input: false };
              const title = notePageName(page.title);
              const note = itemsRef.current.find((item) => item.id === noteId);
              const rename = current.isEmpty && defaultNoteName.test(note?.name ?? '');
              accepted = await appendNoteHtml(current, notePageHtml(page),
                (url) => client.readNotePicture(url), () => sameEditorContext(current, noteId, epoch));
              if (!sameEditorContext(current, noteId, epoch)) return { clear_input: false };
              if (accepted && rename && title && note) {
                await renameImportedNote(note, title, 'page', () => sameEditorContext(current, noteId, epoch));
              }
            } catch (failure: unknown) {
              if (sameEditorContext(current, noteId, epoch)) {
                setPromptError(publicErrorMessage(failure, 'Could not retrieve the web page. Try again.'));
              }
              return { clear_input: false };
            }
          } else {
            accepted = insertPromptParagraph(current, text);
          }
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
            <button
              aria-label="Load text file"
              aria-busy={importingFile}
              className="cha-speech-cache-toggle"
              disabled={!submitEnabled || promptPending}
              onClick={() => fileInput.current?.click()}
              title="Load text file"
              type="button"
            >
              <FileUpIcon />
            </button>
            <input
              accept={textFileAccept}
              aria-label="Text file"
              className="cha-file-input"
              onChange={(event) => {
                const file = event.target.files?.[0];
                event.target.value = '';
                if (file) void loadNoteFile(file);
              }}
              ref={fileInput}
              type="file"
            />
            {transliteration}
          </div>
        )}
        submitEnabled={submitEnabled}
        submitLabel={sendLabel}
        submitTitle={sendLabel}
        value={draft}
      />
      {linkMenu && createPortal(
        <div
          className="cha-notes-list-menu"
          onKeyDown={(event) => menuKeys(event, () => setLinkMenu(null))}
          ref={linkContextRef}
          role="menu"
          style={{ position: 'fixed', top: linkMenu.y, left: linkMenu.x }}
        >
          <button
            onClick={() => {
              const range = linkMenu.range;
              setLinkMenu(null);
              setLinkEdit(range);
            }}
            role="menuitem"
            type="button"
          >
            Edit link
          </button>
          <button
            onClick={() => {
              const range = linkMenu.range;
              setLinkMenu(null);
              const current = editorRef.current;
              if (current) {
                removeNoteLink(current, range);
                focusNotesEditor(current);
              }
            }}
            role="menuitem"
            type="button"
          >
            Remove link
          </button>
        </div>,
        document.body,
      )}
      {linkEdit && (
        <LinkEditDialog
          initial={linkEdit.href}
          onApply={(value) => {
            const current = editorRef.current;
            if (!current) return null;
            const result = replaceLinkDestination(current, linkEdit, value);
            if (result === 'applied') setLinkEdit(null);
            if (result === 'applied' || result === 'unavailable') return null;
            return linkEditErrors[result];
          }}
          onCancel={() => setLinkEdit(null)}
          onClosed={() => {
            const current = editorRef.current;
            focusNotesEditor(current);
          }}
        />
      )}
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
