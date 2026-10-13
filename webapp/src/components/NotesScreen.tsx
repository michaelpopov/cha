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
import {
  createNotesEditor,
  importPictureFile,
  insertPromptParagraph,
  linkRangeAt,
  linkRangeFromSelection,
  removeNoteLink,
  replaceLinkDestination,
  selectNoteEnd,
  withIsolatedHistory,
  type NoteLinkRange,
} from '../notes/editor';
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

function onMenuKey(event: ReactKeyboardEvent<HTMLDivElement>, onEscape: () => void) {
  const items = [...event.currentTarget.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')];
  const current = items.indexOf(document.activeElement as HTMLButtonElement);
  let next = current;
  if (event.key === 'ArrowDown') next = (current + 1) % items.length;
  else if (event.key === 'ArrowUp') next = (current - 1 + items.length) % items.length;
  else if (event.key === 'Home') next = 0;
  else if (event.key === 'End') next = items.length - 1;
  else if (event.key === 'Escape') {
    event.preventDefault();
    onEscape();
    return;
  } else return;
  event.preventDefault();
  items[next]?.focus();
}

function FormatBar({
  editor,
  toolbarRef,
  onFind,
  onPicture,
  onEditLink,
  onRemoveLink,
}: {
  editor: Editor;
  toolbarRef: { current: HTMLDivElement | null };
  onFind(): void;
  onPicture(file: File): void;
  onEditLink(range: NoteLinkRange): void;
  onRemoveLink(range: NoteLinkRange): void;
}) {
  const [, setTick] = useState(0);
  const [listOpen, setListOpen] = useState(false);
  const [linkOpen, setLinkOpen] = useState(false);
  const listRoot = useRef<HTMLDivElement | null>(null);
  const listMenu = useRef<HTMLDivElement | null>(null);
  const listButton = useRef<HTMLButtonElement | null>(null);
  const linkRoot = useRef<HTMLDivElement | null>(null);
  const linkMenu = useRef<HTMLDivElement | null>(null);
  const linkButton = useRef<HTMLButtonElement | null>(null);
  const pictureInput = useRef<HTMLInputElement | null>(null);
  const editable = editor.isEditable && !editor.isDestroyed;
  const linkRange = linkRangeFromSelection(editor.state);

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

  useEffect(() => {
    if (!linkOpen) return;
    linkMenu.current?.querySelector<HTMLButtonElement>('button')?.focus();
    const dismiss = (event: PointerEvent) => {
      const target = event.target;
      if (target instanceof Node && linkRoot.current?.contains(target)) return;
      setLinkOpen(false);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => window.removeEventListener('pointerdown', dismiss);
  }, [linkOpen]);

  function chooseList(list: 'bullet' | 'ordered') {
    setListOpen(false);
    runMark(editor, () => (
      list === 'bullet' ? editor.chain().toggleBulletList().run() : editor.chain().toggleOrderedList().run()
    ));
  }

  return (
    <div className="cha-notes-toolbar" ref={toolbarRef}>
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
          <div
            className="cha-notes-list-menu"
            onKeyDown={(event) => onMenuKey(event, () => {
              setListOpen(false);
              listButton.current?.focus();
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
        className="cha-button"
        disabled={!editable}
        onClick={() => pictureInput.current?.click()}
        type="button"
      >
        Picture
      </button>
      <div className="cha-notes-list" ref={linkRoot}>
        <button
          aria-expanded={linkOpen}
          aria-haspopup="menu"
          className="cha-button"
          disabled={!editable || !linkRange}
          onClick={() => setLinkOpen((open) => !open)}
          ref={linkButton}
          type="button"
        >
          Link
        </button>
        {linkOpen && linkRange && (
          <div
            className="cha-notes-list-menu"
            onKeyDown={(event) => onMenuKey(event, () => {
              setLinkOpen(false);
              linkButton.current?.focus();
            })}
            ref={linkMenu}
            role="menu"
          >
            <button
              onClick={() => {
                setLinkOpen(false);
                onEditLink(linkRange);
              }}
              role="menuitem"
              type="button"
            >
              Edit link
            </button>
            <button
              onClick={() => {
                setLinkOpen(false);
                onRemoveLink(linkRange);
              }}
              role="menuitem"
              type="button"
            >
              Remove link
            </button>
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
      <button className="cha-button" onClick={onFind} type="button">Find</button>
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
  }

  function next() {
    if (!canMove) return;
    editor.commands.goToNextResult();
  }

  function replaceOne() {
    if (!canReplace) return;
    withIsolatedHistory(editor, () => editor.commands.replace());
  }

  function replaceEvery() {
    if (!canReplace) return;
    withIsolatedHistory(editor, () => editor.commands.replaceAll());
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
        <label>
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
        <button className="cha-button" disabled={!canMove} onClick={previous} type="button">Previous</button>
        <button className="cha-button" disabled={!canMove} onClick={next} type="button">Next</button>
        <span role="status">{status}</span>
        <button aria-label="Close" className="cha-button" onClick={onClose} type="button">×</button>
      </div>
      <div className="cha-notes-find-row">
        <label>
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
        <button className="cha-button" disabled={!canReplace} onClick={replaceOne} type="button">Replace</button>
        <button className="cha-button" disabled={!canReplace} onClick={replaceEvery} type="button">Replace all</button>
      </div>
      <div className="cha-notes-find-row">
        <label>
          <input checked={matchCase} onChange={(event) => onMatchCase(event.target.checked)} type="checkbox" />
          Match case
        </label>
        <label>
          <input
            checked={wholeWord}
            disabled={regex}
            onChange={(event) => onWholeWord(event.target.checked)}
            type="checkbox"
          />
          Whole word
        </label>
        <label>
          <input checked={regex} onChange={(event) => onRegex(event.target.checked)} type="checkbox" />
          Regular expression
        </label>
      </div>
    </div>
  );
}

function LinkEditDialog({
  initial,
  onApply,
  onCancel,
}: {
  initial: string;
  onApply(value: string): boolean;
  onCancel(): void;
}) {
  const dialog = useRef<HTMLDialogElement | null>(null);
  const input = useRef<HTMLInputElement | null>(null);
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
      {error && <p role="alert">{error}</p>}
      <div className="cha-dialog-actions">
        <button className="cha-button" onClick={onCancel} type="button">Cancel</button>
        <button
          className="cha-button cha-button-primary"
          onClick={() => {
            if (!onApply(value)) setError('Enter an HTTP or HTTPS address.');
          }}
          type="button"
        >
          Apply
        </button>
      </div>
    </dialog>,
    document.body,
  );
}

function linkRangeFromPointer(editor: Editor, event: MouseEvent): NoteLinkRange | null {
  try {
    const coords = editor.view.posAtCoords({ left: event.clientX, top: event.clientY });
    if (coords) {
      const range = linkRangeAt(editor.state.doc, coords.pos);
      if (range) return range;
    }
  } catch {
    // A host without hit testing still exposes the element under the pointer.
  }
  const target = event.target;
  if (!(target instanceof Node)) return null;
  try {
    return linkRangeAt(editor.state.doc, editor.view.posAtDOM(target, 0));
  } catch {
    return null;
  }
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
            onCommandError: (message) => {
              if (cancelled || noteIdRef.current !== noteId) return;
              setCommandError(message);
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
    setLinkEdit(null);
    setLinkMenu(null);
  }, [activeNoteId]);

  useEffect(() => {
    if (!findOpen) return;
    findInputRef.current?.focus();
  }, [findOpen]);

  useEffect(() => {
    const current = editor;
    if (!findOpenRef.current || !current || current.isDestroyed) return;
    const controls = findControls.current;
    current.commands.setCaseSensitive(controls.matchCase);
    current.commands.setUseRegex(controls.regex);
    current.commands.setWholeWord(controls.regex ? false : controls.wholeWord);
    current.commands.setReplaceTerm(controls.replacement);
    current.commands.setSearchTerm(controls.term);
  }, [editor]);

  useEffect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key.toLowerCase() !== 'f' || event.altKey || event.shiftKey) return;
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
      const range = linkRangeFromPointer(editor, event);
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

  function applyFind(current: Editor) {
    const controls = findControls.current;
    current.commands.setCaseSensitive(controls.matchCase);
    current.commands.setUseRegex(controls.regex);
    current.commands.setWholeWord(controls.regex ? false : controls.wholeWord);
    current.commands.setReplaceTerm(controls.replacement);
    current.commands.setSearchTerm(controls.term);
  }

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
    current.commands.focus();
  }

  function samePictureContext(current: Editor, noteId: number, epoch: number): boolean {
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
    void importPictureFile(current, file, () => samePictureContext(current, noteId, epoch)).then((message) => {
      if (!samePictureContext(current, noteId, epoch)) return;
      setCommandError(message);
    });
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
    && !save.frozen;

  return (
    <section className="cha-notes-screen" aria-label="Note">
      {phase === 'ready' && editor && (
        <FormatBar
          editor={editor}
          onEditLink={(range) => {
            setLinkMenu(null);
            setLinkEdit(range);
          }}
          onFind={openFind}
          onPicture={onPicture}
          onRemoveLink={(range) => {
            setLinkMenu(null);
            const current = editorRef.current;
            if (current) removeNoteLink(current, range);
          }}
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
      {commandError && <p className="cha-error-message" role="alert">{commandError}</p>}
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
      {linkMenu && createPortal(
        <div
          className="cha-notes-list-menu"
          onKeyDown={(event) => onMenuKey(event, () => setLinkMenu(null))}
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
              if (current) removeNoteLink(current, range);
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
            if (!current) return false;
            const applied = replaceLinkDestination(current, linkEdit, value);
            if (applied) setLinkEdit(null);
            return applied;
          }}
          onCancel={() => setLinkEdit(null)}
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
