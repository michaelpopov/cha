import { ChaError } from '../api/client';
import { sortAnnotationGroups, type AnnotationDestination } from '../notes/annotationSource';
import { selectNoteAnnotation } from '../notes/highlights';
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
  addNoteComment,
  appendNoteHtml,
  createNotesEditor,
  deleteNoteComment,
  importPictureFile,
  insertPromptParagraph,
  noteHeadings,
  noteCommentRange,
  replaceNoteHtml,
  selectNoteEnd,
  selectNoteHeading,
  withIsolatedHistory,
  toggleNoteHighlight,
} from '../notes/editor';
import { noteClipboardCommand } from '../notes/clipboard';
import { selectNoteHighlight, type NoteHighlight } from '../notes/highlights';
import { notePageHtml, notePageName, promptPageUrl } from '../notes/pageImport';
import { noteFileHtml, noteFileTooLarge, promptFilePath } from '../notes/fileImport';
import { availableNoteName, noteItemPath } from '../notes/tree';
import type { useNoteLeave } from '../notes/useNoteLeave';
import { noteSaveFailedMessage, useNoteSave, type NoteSaveHandle } from '../notes/useNoteSave';
import type { DataAction, NavigationAction } from '../state/view';
import {
  BlocksIcon,
  CheckIcon,
  ChevronRightIcon,
  CloseIcon,
  ContentIcon,
  FontSizeIcon,
  HighlighterIcon,
  HistoryIcon,
  SearchIcon,
  TextFormatIcon,
  TextLinesIcon,
} from './Icons';
import { menuKeys, type NoteActions, type SetNoteReadOnly } from './NotesTree';
import { NoteHighlights } from './NoteHighlights';
import { PromptInput } from './PromptInput';
import { SidebarToggle } from './TopBar';

type LeaveHandle = Pick<
  ReturnType<typeof useNoteLeave>,
  'decisionOpen' | 'retry' | 'stay' | 'discard'
>;

type Phase = 'empty' | 'loading' | 'ready' | 'error';

const defaultNoteName = /^New note(?: [1-9]\d*)?$/u;
// A source note that was deleted cannot open again, so this error has no Retry.
const missingSourceMessage = 'This note is no longer available.';

function saveDetail(message: string): string {
  return message === noteSaveFailedMessage ? 'Check the vault and try again.' : message;
}

function preserveNoteScroll<T>(editor: Editor, run: () => T): T {
  const host = editor.view.dom.closest('.cha-notes-document');
  const top = host?.scrollTop ?? 0;
  const left = host?.scrollLeft ?? 0;
  try {
    return run();
  } finally {
    if (host) {
      host.scrollTop = top;
      host.scrollLeft = left;
    }
  }
}

function focusNotesEditor(editor: Editor | null) {
  if (!editor || editor.isDestroyed) return;
  // Restoring the DOM selection can scroll to an offscreen caret even when
  // focus itself uses preventScroll.
  preserveNoteScroll(editor, () => {
    if (!editor.isEditable) {
      // ProseMirror's focus() restores DOM selections only in editable views.
      editor.view.dom.focus({ preventScroll: true });
      const { anchor, head } = editor.state.selection;
      const start = editor.view.domAtPos(anchor);
      const end = editor.view.domAtPos(head);
      window.getSelection()?.setBaseAndExtent(start.node, start.offset, end.node, end.offset);
    }
    editor.view.focus();
  });
}

function scrollToHighlight(editor: Editor) {
  const host = editor.view.dom.closest('.cha-notes-document');
  if (!host) return;
  const top = editor.view.coordsAtPos(editor.state.selection.from).top;
  host.scrollTop += top - host.getBoundingClientRect().top - host.clientHeight / 2;
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

// A toolbar button with a menu below it. The menu focuses its first enabled
// item only when it opens. It closes on Escape, on a press outside it, or when
// its button becomes disabled, so it cannot reappear later and take focus.
function ToolbarMenu({
  editor,
  label,
  icon,
  open,
  disabled = false,
  menuClassName,
  onOpenChange,
  children,
}: {
  editor: Editor;
  label: string;
  icon: ReactNode;
  open: boolean;
  disabled?: boolean;
  menuClassName?: string;
  onOpenChange(open: boolean): void;
  children: ReactNode;
}) {
  const root = useRef<HTMLDivElement | null>(null);
  const menu = useRef<HTMLDivElement | null>(null);
  const visible = open && !disabled;

  useEffect(() => {
    if (!open) return;
    if (disabled) {
      onOpenChange(false);
      return;
    }
    menu.current?.querySelector<HTMLButtonElement>('button:not(:disabled)')?.focus();
    const dismiss = (event: PointerEvent) => {
      const target = event.target;
      if (target instanceof Node && root.current?.contains(target)) return;
      onOpenChange(false);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => window.removeEventListener('pointerdown', dismiss);
  }, [open, disabled]);

  return (
    <div className="cha-notes-toolbar-menu" ref={root}>
      <button
        aria-label={label}
        title={label}
        aria-expanded={visible}
        aria-haspopup="menu"
        className="cha-button"
        disabled={disabled}
        onClick={() => onOpenChange(!visible)}
        type="button"
      >
        {icon}
      </button>
      {visible && (
        <div
          aria-label={label}
          className={`cha-notes-list-menu cha-notes-format-menu${menuClassName ? ` ${menuClassName}` : ''}`}
          onKeyDown={(event) => menuKeys(event, () => {
            onOpenChange(false);
            focusNotesEditor(editor);
          })}
          ref={menu}
          role="menu"
        >
          {children}
        </div>
      )}
    </div>
  );
}

function FormatBar({
  editor,
  onHighlight,
  frozen,
  immutable,
  fontSize,
  onFontSizeChange,
  toolbarRef,
  onFind,
  onPicture,
  onUpload,
  onCopyAll,
  uploadEnabled,
}: {
  editor: Editor;
  onHighlight(highlight: NoteHighlight): void;
  frozen: boolean;
  immutable: boolean;
  fontSize: number;
  onFontSizeChange(size: number): void;
  toolbarRef: { current: HTMLDivElement | null };
  onFind(): void;
  onPicture(file: File): void;
  onUpload(): void;
  onCopyAll(): void;
  uploadEnabled: boolean;
}) {
  const [, setTick] = useState(0);
  const [formatMenu, setFormatMenu] = useState<'content' | 'history' | 'edit' | 'font' | 'heading' | 'insert' | 'list' | null>(null);
  const [headingsOpen, setHeadingsOpen] = useState(false);
  const listButton = useRef<HTMLButtonElement | null>(null);
  const listMenu = useRef<HTMLDivElement | null>(null);
  const headingsRoot = useRef<HTMLDivElement | null>(null);
  const headingsMenu = useRef<HTMLDivElement | null>(null);
  const pictureInput = useRef<HTMLInputElement | null>(null);
  const fontAnchor = useRef<{ block: Element; share: number } | null>(null);
  const editable = editor.isEditable && !editor.isDestroyed;
  const canUndo = editable && editor.can().undo();
  const canRedo = editable && editor.can().redo();
  const historyAvailable = canUndo || canRedo;
  const listOpen = formatMenu === 'list';
  const headings = noteHeadings(editor.state.doc);
  const showHeadings = headingsOpen && !frozen && !editor.isDestroyed && headings.length > 0;

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

  // The List submenu focuses its first item when it opens. When it closes and
  // the Insert menu stays open, focus returns to the List item.
  useEffect(() => {
    if (listOpen) listMenu.current?.querySelector<HTMLButtonElement>('button')?.focus();
    else listButton.current?.focus();
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
    setFormatMenu(null);
    format(() => (
      list === 'bullet' ? editor.chain().toggleBulletList().run() : editor.chain().toggleOrderedList().run()
    ));
  }

  // Each toolbar action returns focus to the note. A menu or dialog that an
  // action opens then focuses its own first control.
  return (
    <div className="cha-notes-toolbar" onClick={() => focusNotesEditor(editor)} ref={toolbarRef}>
      <div className="cha-notes-toolbar-group">
        <ToolbarMenu
          editor={editor}
          icon={<ContentIcon />}
          label="Content"
          onOpenChange={(open) => setFormatMenu(open ? 'content' : null)}
          open={formatMenu === 'content'}
        >
          <button
            disabled={!uploadEnabled}
            onClick={() => {
              setFormatMenu(null);
              onUpload();
            }}
            role="menuitem"
            type="button"
          >
            Upload
          </button>
          <button
            onClick={() => {
              setFormatMenu(null);
              onCopyAll();
            }}
            role="menuitem"
            type="button"
          >
            Copy All
          </button>
        </ToolbarMenu>
      </div>
      <div className="cha-notes-toolbar-group">
        <ToolbarMenu
          disabled={!historyAvailable}
          editor={editor}
          icon={<HistoryIcon />}
          label="History"
          onOpenChange={(open) => setFormatMenu(open ? 'history' : null)}
          open={formatMenu === 'history'}
        >
          <button
            disabled={!canUndo}
            onClick={() => {
              setFormatMenu(null);
              runHistory(editor, 'undo');
            }}
            role="menuitem"
            type="button"
          >
            Undo
          </button>
          <button
            disabled={!canRedo}
            onClick={() => {
              setFormatMenu(null);
              runHistory(editor, 'redo');
            }}
            role="menuitem"
            type="button"
          >
            Redo
          </button>
        </ToolbarMenu>
      </div>
      <div className="cha-notes-toolbar-group">
        <ToolbarMenu
          disabled={!editable}
          editor={editor}
          icon={<TextFormatIcon />}
          label="Edit"
          onOpenChange={(open) => setFormatMenu(open ? 'edit' : null)}
          open={formatMenu === 'edit'}
        >
          {[
            { name: 'Bold', mark: 'bold', command: () => editor.chain().toggleBold().run() },
            { name: 'Italic', mark: 'italic', command: () => editor.chain().toggleItalic().run() },
            { name: 'Underline', mark: 'underline', command: () => editor.chain().toggleUnderline().run() },
          ].map(({ name, mark, command }) => (
            <button
              aria-checked={editor.isActive(mark)}
              key={mark}
              onClick={() => {
                setFormatMenu(null);
                format(command);
              }}
              role="menuitemcheckbox"
              type="button"
            >
              <span className={`cha-notes-edit-${mark}`}>{name}</span>
              {editor.isActive(mark) && <CheckIcon />}
            </button>
          ))}
        </ToolbarMenu>
        <button
          aria-label="Highlight"
          title="Highlight"
          aria-pressed={editor.isActive('highlight')}
          className="cha-button"
          disabled={frozen || immutable || editor.isDestroyed}
          onClick={() => preserveNoteScroll(editor, () => { toggleNoteHighlight(editor); })}
          type="button"
        >
          <HighlighterIcon />
        </button>
      </div>
      <div className="cha-notes-toolbar-group">
        <ToolbarMenu
          editor={editor}
          icon={<FontSizeIcon />}
          label="Font size"
          menuClassName="cha-notes-font-menu"
          onOpenChange={(open) => setFormatMenu(open ? 'font' : null)}
          open={formatMenu === 'font'}
        >
          <button
            disabled={fontSize <= 10}
            onClick={() => {
              setFormatMenu(null);
              changeFontSize(Math.max(10, fontSize - 2));
            }}
            role="menuitem"
            type="button"
          >
            Decrease font size
          </button>
          <button
            disabled={fontSize >= 30}
            onClick={() => {
              setFormatMenu(null);
              changeFontSize(Math.min(30, fontSize + 2));
            }}
            role="menuitem"
            type="button"
          >
            Increase font size
          </button>
        </ToolbarMenu>
      </div>
      <div className="cha-notes-toolbar-group">
        <ToolbarMenu
          disabled={!editable}
          editor={editor}
          icon="H"
          label="Headings"
          onOpenChange={(open) => setFormatMenu(open ? 'heading' : null)}
          open={formatMenu === 'heading'}
        >
          {([
            { name: 'Title', level: 1 },
            { name: 'Chapter', level: 2 },
            { name: 'Section', level: 3 },
          ] as const).map(({ name, level }) => (
            <button
              aria-checked={editor.isActive('heading', { level })}
              key={level}
              onClick={() => {
                setFormatMenu(null);
                format(() => editor.chain().toggleHeading({ level }).run());
              }}
              role="menuitemcheckbox"
              type="button"
            >
              <span>{name}</span>
              {editor.isActive('heading', { level }) && <CheckIcon />}
            </button>
          ))}
        </ToolbarMenu>
      </div>
      <div className="cha-notes-toolbar-group">
        <ToolbarMenu
          disabled={!editable}
          editor={editor}
          icon={<BlocksIcon />}
          label="Insert"
          onOpenChange={(open) => setFormatMenu(open ? 'insert' : null)}
          open={formatMenu === 'insert' || listOpen}
        >
          <div className="cha-notes-list">
            <button
              aria-expanded={listOpen}
              aria-haspopup="menu"
              onClick={() => setFormatMenu(listOpen ? 'insert' : 'list')}
              onKeyDown={(event) => {
                if (event.key !== 'ArrowRight') return;
                event.preventDefault();
                event.stopPropagation();
                setFormatMenu('list');
              }}
              ref={listButton}
              role="menuitem"
              type="button"
            >
              <span>List</span>
              <ChevronRightIcon />
            </button>
            {listOpen && (
              <div
                aria-label="List"
                className="cha-notes-list-menu cha-notes-list-submenu"
                onKeyDown={(event) => {
                  event.stopPropagation();
                  if (event.key === 'ArrowLeft' || event.key === 'Escape') {
                    event.preventDefault();
                    setFormatMenu('insert');
                  } else menuKeys(event, () => setFormatMenu('insert'));
                }}
                ref={listMenu}
                role="menu"
              >
                <button onClick={() => chooseList('bullet')} role="menuitem" type="button">Bullet list</button>
                <button onClick={() => chooseList('ordered')} role="menuitem" type="button">Numbered list</button>
              </div>
            )}
          </div>
          <button
            onClick={() => {
              setFormatMenu(null);
              pictureInput.current?.click();
            }}
            role="menuitem"
            type="button"
          >
            Picture
          </button>
          <button
            aria-checked={editor.isActive('code')}
            onClick={() => {
              setFormatMenu(null);
              format(() => editor.chain().toggleCode().run());
            }}
            role="menuitemcheckbox"
            type="button"
          >
            <span>Code</span>
            {editor.isActive('code') && <CheckIcon />}
          </button>
        </ToolbarMenu>
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
            disabled={frozen || editor.isDestroyed || headings.length === 0}
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
        <NoteHighlights
          editor={editor}
          frozen={frozen}
          onChoose={onHighlight}
          onClose={() => focusNotesEditor(editor)}
        />
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
            disabled={!editor.isEditable}
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

const contextCommands = ['Cut', 'Copy', 'Paste', 'Bold', 'Italic', 'Highlight'] as const;
type ContextCommand = typeof contextCommands[number] | 'Add comment' | 'Delete comment';

export function NotesScreen({
  client,
  notesEpoch,
  noteRequest,
  destination,
  activeNoteId,
  noteItems,
  noteActions = null,
  onSetNoteReadOnly,
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
  destination?: AnnotationDestination;
  activeNoteId: number | null;
  noteItems: readonly NoteItem[];
  noteActions?: NoteActions | null;
  onSetNoteReadOnly: SetNoteReadOnly;
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
  const scrollTopRef = useRef(0);
  const activeItem = noteItems.find((item) => item.id === activeNoteId);
  const name = activeItem?.name ?? 'note';
  // The note stays read-only until the tree list confirms its state.
  const readOnly = activeItem?.read_only ?? true;
  const immutable = activeItem?.kind !== 'note';
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
  const [contextMenu, setContextMenu] = useState<{ x: number; y: number; position: number } | null>(null);
  const toolbarRef = useRef<HTMLDivElement | null>(null);
  const findRef = useRef<HTMLDivElement | null>(null);
  const findInputRef = useRef<HTMLInputElement | null>(null);
  const contextMenuRef = useRef<HTMLDivElement | null>(null);
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
      const epoch = epochRef.current;
      const result = await client.saveNote(id, document);
      if (epoch === epochRef.current && result.annotation_created) noteActions?.refresh();
    },
    saveScrollPosition: async () => {
      const host = hostRef.current;
      if (!host || !editor || editor.isDestroyed || editorRef.current !== editor || activeNoteId === null) return;
      await client.saveNoteScrollPosition(activeNoteId, Math.max(0, Math.round(host.scrollTop)));
    },
  });

  useLayoutEffect(() => {
    onRegisterSave(save);
    return () => onRegisterSave(null);
  });

  useEffect(() => {
    setCommandError(null);
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
        const items = itemsRef.current;
        const shownDocument = items.find((item) => item.id === noteId)?.kind === 'annotation'
          ? sortAnnotationGroups(content.document, items) : content.document;
        let rejected = false;
        let created: Editor;
        try {
          created = createNotesEditor({
            content: shownDocument as Content,
            immutable: () => itemsRef.current.find((item) => item.id === noteId)?.kind !== 'note',
            onOpenAnnotation: (sourceId, target) => {
              if (!cancelled) navigate({ type: 'open-note', noteId: sourceId, destination: target });
            },
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
        scrollTopRef.current = content.scroll_top ?? 0;
        editorRef.current = created;
        setEditor(created);
        setPhase('ready');
      } catch (failure: unknown) {
        if (cancelled) return;
        setPhase('error');
        if (destination && failure instanceof ChaError && failure.code === 'not_found') {
          setOpenError(missingSourceMessage);
          return;
        }
        const detail = publicErrorMessage(failure, 'Could not open the note.');
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

  useLayoutEffect(() => {
    const host = hostRef.current;
    if (!editor || editor.isDestroyed) return;
    const element = editor.options.element;
    if (!host || !(element instanceof HTMLElement)) return;
    host.appendChild(element);
    const screen = host.parentElement ?? host;
    const pictures = Array.from(element.querySelectorAll('img'));
    const restore = () => {
      if (destination && selectNoteAnnotation(editor, destination)) {
        scrollToHighlight(editor);
        return true;
      }
      host.scrollTop = scrollTopRef.current;
      return false;
    };
    // Embedded pictures can change the document height after it is attached.
    // Stop restoring as soon as the user starts to interact with the note.
    const stopRestoring = () => {
      element.removeEventListener('load', pictureLoaded, true);
      element.removeEventListener('error', pictureLoaded, true);
      for (const event of ['wheel', 'pointerdown', 'touchstart', 'keydown']) {
        screen.removeEventListener(event, stopRestoring);
      }
    };
    const pictureLoaded = () => {
      restore();
      if (pictures.every((picture) => picture.complete)) stopRestoring();
    };
    focusNotesEditor(editor);
    if (!restore() && destination) setCommandError('This entry has changed or was removed.');
    if (pictures.some((picture) => !picture.complete)) {
      element.addEventListener('load', pictureLoaded, true);
      element.addEventListener('error', pictureLoaded, true);
      for (const event of ['wheel', 'pointerdown', 'touchstart', 'keydown']) {
        screen.addEventListener(event, stopRestoring, { passive: true });
      }
    }
    return () => {
      stopRestoring();
      element.remove();
    };
  }, [editor]);

  useEffect(() => {
    if (phase !== 'ready' || !editor || editor.isDestroyed) return;
    editor.setEditable(!readOnly, false);
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
  }, [editor, editorLiveRef, phase, readOnly]);

  useEffect(() => {
    setFindOpen(false);
    setFindTerm('');
    setReplaceTerm('');
    setMatchCase(false);
    setWholeWord(false);
    setUseRegex(false);
    setPromptError(null);
    setPromptPending(false);
    setContextMenu(null);
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
    const onContext = (event: MouseEvent) => {
      event.preventDefault();
      // Paragraph margins and the editor padding belong to the editor root.
      const position = event.target === dom
        ? editor.view.posAtCoords({ left: event.clientX, top: event.clientY })?.pos ?? 0
        : editor.view.posAtDOM(event.target as Node, 0);
      setContextMenu({ x: event.clientX, y: event.clientY, position });
    };
    dom.addEventListener('contextmenu', onContext);
    return () => dom.removeEventListener('contextmenu', onContext);
  }, [editor, phase]);

  useLayoutEffect(() => {
    const menu = contextMenuRef.current;
    if (!contextMenu || !menu) return;
    const rect = menu.getBoundingClientRect();
    menu.style.left = `${Math.max(0, Math.min(contextMenu.x, window.innerWidth - rect.width))}px`;
    menu.style.top = `${Math.max(0, Math.min(contextMenu.y, window.innerHeight - rect.height))}px`;
    menu.querySelector<HTMLButtonElement>('button:not(:disabled)')?.focus({ preventScroll: true });
    const dismiss = (event: PointerEvent) => {
      if (event.target instanceof Node && menu.contains(event.target)) return;
      setContextMenu(null);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => window.removeEventListener('pointerdown', dismiss);
  }, [contextMenu]);

  async function runContextCommand(command: ContextCommand | 'Copy All') {
    setContextMenu(null);
    const current = editorRef.current;
    if (!current || current.isDestroyed) return;
    const copying = command === 'Copy' || command === 'Copy All';
    if (!copying && (save.frozen || (immutable && ['Highlight', 'Add comment', 'Delete comment'].includes(command))
      || (!current.isEditable && command !== 'Highlight'
        && command !== 'Add comment' && command !== 'Delete comment'))) return;
    const noteId = noteIdRef.current;
    const epoch = epochRef.current;
    const available = () => editorRef.current === current && !current.isDestroyed
      && noteIdRef.current === noteId && epochRef.current === epoch
      && (copying || current.isEditable);
    focusNotesEditor(current);
    setCommandError(null);
    if (command === 'Add comment') {
      preserveNoteScroll(current, () => {
        if (contextMenu && addNoteComment(current, contextMenu.position, draft)) onDraftChange('');
      });
    } else if (command === 'Delete comment') {
      preserveNoteScroll(current, () => {
        if (contextMenu) deleteNoteComment(current, contextMenu.position);
      });
    } else if (command === 'Highlight') {
      preserveNoteScroll(current, () => { toggleNoteHighlight(current); });
    } else if (command === 'Bold' || command === 'Italic') {
      preserveNoteScroll(current, () => runMark(current, () => command === 'Bold'
        ? current.chain().toggleBold().run() : current.chain().toggleItalic().run()));
    } else {
      try {
        const runClipboard = () => noteClipboardCommand(current, command, available);
        await (command === 'Copy All' ? preserveNoteScroll(current, runClipboard) : runClipboard());
      } catch {
        if (editorRef.current === current && !current.isDestroyed) {
          setCommandError(command === 'Copy All'
            ? 'Could not copy the note. Try again.'
            : `${command} failed. Try the keyboard shortcut.`);
        }
      }
    }
  }

  function openFind() {
    const current = editorRef.current;
    if (!current || current.isDestroyed) return;
    setFindOpen(true);
    applyFind(current);
    queueMicrotask(() => findInputRef.current?.focus());
  }

  function chooseHighlight(highlight: NoteHighlight) {
    const current = editorRef.current;
    if (!current || current.isDestroyed || save.frozen) return;
    if (!selectNoteHighlight(current, highlight)) {
      setCommandError('This highlight has changed or was removed.');
      return;
    }
    focusNotesEditor(current);
    scrollToHighlight(current);
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
      noteActions?.updated(renamed);
    } catch (failure: unknown) {
      if (stillCurrent()) {
        setPromptError(`The ${source} was imported, but the note could not be renamed. `
          + publicErrorMessage(failure, 'Try renaming the note again.'));
      }
    }
  }

  async function lockImportedNote(noteId: number) {
    const current = editorRef.current;
    const epoch = epochRef.current;
    const sameNote = () => editorRef.current === current
      && noteIdRef.current === noteId && epochRef.current === epoch;
    try {
      noteActions?.updated(await onSetNoteReadOnly(noteId, true));
    } catch (failure: unknown) {
      if (sameNote()) {
        setPromptError('The content was imported, but the note could not be made read-only. '
          + publicErrorMessage(failure, 'Try Make read-only from the note menu.'));
      }
    }
    return sameNote();
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
      if (!stillCurrent()) return false;
      return await lockImportedNote(noteId);
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
  const menuCommands: ContextCommand[] = [...contextCommands,
    editor && contextMenu && noteCommentRange(editor, contextMenu.position) ? 'Delete comment' : 'Add comment'];
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
          onHighlight={chooseHighlight}
          frozen={save.frozen}
          immutable={immutable}
          fontSize={fontSize}
          onFontSizeChange={setFontSize}
          onFind={openFind}
          onPicture={onPicture}
          onUpload={() => fileInput.current?.click()}
          onCopyAll={() => void runContextCommand('Copy All')}
          uploadEnabled={submitEnabled && !promptPending}
          toolbarRef={toolbarRef}
        />
      )}
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
      <div
        className={activeItem?.kind === 'annotation' ? 'cha-notes-document cha-notes-annotations' : 'cha-notes-document'}
        ref={hostRef}
        style={{ fontSize }}
      >
        {phase === 'loading' && <p role="status">Opening…</p>}
        {phase === 'error' && (
          <div role="alert">
            <p>{openError}</p>
            {openError !== missingSourceMessage && (
              <button
                className="cha-button"
                onClick={() => {
                  if (activeNoteId === null) return;
                  navigate({ type: 'open-note', noteId: activeNoteId, destination });
                }}
                type="button"
              >
                Retry
              </button>
            )}
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
              const wasEmpty = current.isEmpty;
              const rename = wasEmpty && defaultNoteName.test(note?.name ?? '');
              accepted = await appendNoteHtml(current, notePageHtml(page),
                (url) => client.readNotePicture(url), () => sameEditorContext(current, noteId, epoch));
              if (!sameEditorContext(current, noteId, epoch)) return { clear_input: false };
              if (accepted && rename && title && note) {
                await renameImportedNote(note, title, 'page', () => sameEditorContext(current, noteId, epoch));
              }
              if (accepted && wasEmpty && sameEditorContext(current, noteId, epoch)) {
                return { clear_input: await lockImportedNote(noteId) };
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
            {transliteration}
          </div>
        )}
        submitEnabled={submitEnabled}
        submitLabel={sendLabel}
        submitTitle={sendLabel}
        value={draft}
      />
      {contextMenu && createPortal(
        <div
          className="cha-notes-list-menu"
          onKeyDown={(event) => menuKeys(event, () => {
            setContextMenu(null);
            focusNotesEditor(editorRef.current);
          })}
          ref={contextMenuRef}
          role="menu"
          style={{ position: 'fixed', top: contextMenu.y, left: contextMenu.x }}
        >
          {menuCommands.map((command) => (
            <button
              disabled={command !== 'Copy' && (save.frozen || (immutable && ['Highlight', 'Add comment', 'Delete comment'].includes(command))
                || (readOnly && command !== 'Highlight'
                  && command !== 'Add comment' && command !== 'Delete comment'))}
              key={command}
              onClick={() => { void runContextCommand(command); }}
              role="menuitem"
              type="button"
            >
              {command}
            </button>
          ))}
        </div>,
        document.body,
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
