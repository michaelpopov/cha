import { Editor, Extension, type Content } from '@tiptap/core';
import FindAndReplace from '@tiptap/extension-find-and-replace';
import Highlight from '@tiptap/extension-highlight';
import Image from '@tiptap/extension-image';
import { closeHistory, isHistoryTransaction } from '@tiptap/pm/history';
import { GapCursor } from '@tiptap/pm/gapcursor';
import {
  DOMParser as PmDOMParser,
  Fragment,
  type Node as PmNode,
  type ResolvedPos,
  type Schema,
  Slice,
} from '@tiptap/pm/model';
import {
  AllSelection,
  NodeSelection,
  Plugin,
  PluginKey,
  TextSelection,
  type EditorState,
  type Selection,
  type SelectionBookmark,
  type Transaction,
} from '@tiptap/pm/state';
import { Decoration, DecorationSet, type EditorProps, type EditorView } from '@tiptap/pm/view';
import { Transform } from '@tiptap/pm/transform';
import StarterKit from '@tiptap/starter-kit';
import { find } from 'linkifyjs';
import { ChaError } from '../api/client';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';
import {
  clipboardImageFile,
  embeddedImageError,
  loadNotePicture,
  type NotePictureReader,
  noteTooLargeMessage,
  pictureByteLimit,
  pictureFromBytes,
  pictureTooLargeMessage,
  pictureTypeMessage,
  readFileBytes,
} from './pictures';

const notesIsolatedMeta = 'notesIsolated';
const notesIsolatedCloseMeta = 'notesIsolatedClose';
const notesPictureFallbackMeta = 'notesPictureFallback';
const notesPasteSelectionMeta = 'notesPasteSelection';
const notesHighlightMeta = 'notesHighlight';
const notesCommentMeta = 'notesComment';
const notesDeleteCommentMeta = 'notesDeleteComment';
const noteCommentColor = '#b3d9ff';
// Overlapping imports share the four picture download slots.
let noteImportTail = Promise.resolve();

// Tiptap 3.30.3 types `capturedTransaction` as private. The capture path
// still reads and writes that runtime field.
type CaptureState = {
  isCapturingTransaction: boolean;
  capturedTransaction: Transaction | null;
};

function captureState(editor: Editor): CaptureState {
  return editor as unknown as CaptureState;
}

type Gate = {
  blocked: boolean;
};

export type NotesEditorOptions = {
  content?: Content;
  byteLimit?: number;
  readPicture: NotePictureReader;
  onAcceptedDocument?: () => void;
  onRejectedContent?: () => void;
  onCommandError?: (message: string) => void;
  element?: HTMLElement | null;
};

// Absolute http(s), or a bare web address which is stored with an http prefix.
// An explicit other scheme is rejected and is not rewritten.
export function canonicalNoteLink(input: string): string | null {
  const value = input.trim();
  if (!value || /\s/u.test(value)) return null;
  const scheme = /^([a-z][a-z0-9+.-]*):/iu.exec(value);
  if (scheme) {
    const name = scheme[1].toLowerCase();
    if (name !== 'http' && name !== 'https') return null;
    try {
      const url = new URL(value);
      if (url.protocol !== 'http:' && url.protocol !== 'https:') return null;
      return url.href;
    } catch {
      return null;
    }
  }
  const matches = find(value, { defaultProtocol: 'http' });
  if (matches.length !== 1) return null;
  const match = matches[0];
  if (match.type !== 'url' || !match.isLink || match.start !== 0 || match.end !== value.length) {
    return null;
  }
  try {
    const url = new URL(match.href);
    if (url.protocol !== 'http:') return null;
    return url.href;
  } catch {
    return null;
  }
}

export function isAllowedNoteLink(href: string): boolean {
  return canonicalNoteLink(href) !== null;
}

// Activation uses the stored destination. A bare domain is not repaired here.
export function storedWebDestination(href: string): string | null {
  const trimmed = href.trim();
  if (!/^[a-z][a-z0-9+.-]*:/iu.test(trimmed)) return null;
  return canonicalNoteLink(trimmed);
}

export function withIsolatedHistory(editor: Editor, run: () => boolean): boolean {
  const view = editor.view as EditorView & {
    dispatch: (transaction: Transaction) => void;
  };
  const original = view.dispatch.bind(view);
  view.dispatch = (transaction) => {
    if (!transaction.getMeta(notesIsolatedCloseMeta) && !isHistoryTransaction(transaction)) {
      closeHistory(transaction);
      transaction.setMeta(notesIsolatedMeta, true);
    }
    original(transaction);
  };
  try {
    return run();
  } finally {
    view.dispatch = original;
  }
}

export function toggleNoteHighlight(editor: Editor): boolean {
  if (editor.isDestroyed) return false;
  return withIsolatedHistory(editor, () => editor.chain()
    .setMeta(notesHighlightMeta, true)
    .toggleHighlight()
    .run());
}

function withoutHighlight(node: PmNode): PmNode {
  const content: PmNode[] = [];
  node.forEach((child) => content.push(withoutHighlight(child)));
  return (node.isLeaf ? node : node.copy(Fragment.fromArray(content)))
    .mark(node.marks.filter((mark) => mark.type.name !== 'highlight'));
}

export function addNoteComment(editor: Editor, position: number, text: string): boolean {
  if (editor.isDestroyed || !text.trim()) return false;
  const $position = editor.state.doc.resolve(position);
  const before = $position.parent.isTextblock ? $position.before() : position;
  const highlight = editor.schema.marks.highlight.create({ color: noteCommentColor });
  const content: PmNode[] = [];
  buildParagraph(editor.schema, text.replace(/\r\n?/g, '\n')).forEach((node) => {
    content.push(node.mark(highlight.addToSet(node.marks)));
  });
  const paragraph = editor.schema.nodes.paragraph.create(null, content);
  const tr = closeHistory(editor.state.tr.insert(before, paragraph))
    .setMeta(notesCommentMeta, before)
    .setMeta(notesIsolatedMeta, true)
    .setMeta('preventAutolink', true)
    .setStoredMarks([]);
  const previous = editor.state.doc;
  editor.view.dispatch(tr);
  return !editor.state.doc.eq(previous);
}

function isNoteComment(node: PmNode | null): node is PmNode {
  if (node?.type.name !== 'paragraph' || !node.textContent.trim()) return false;
  let comment = true;
  node.forEach((child) => {
    if (child.type.name !== 'hardBreak' && (!child.isText
      || !child.marks.some((mark) => mark.type.name === 'highlight'
        && mark.attrs.color === noteCommentColor))) comment = false;
  });
  return comment;
}

export function noteCommentRange(editor: Editor, position: number): { from: number; to: number } | null {
  if (editor.isDestroyed || position < 0 || position > editor.state.doc.content.size) return null;
  const $position = editor.state.doc.resolve(position);
  const from = $position.parent.isTextblock ? $position.before() : position;
  const node = editor.state.doc.nodeAt(from);
  return isNoteComment(node) ? { from, to: from + node.nodeSize } : null;
}

export function deleteNoteComment(editor: Editor, position: number): boolean {
  const range = noteCommentRange(editor, position);
  if (!range) return false;
  const previous = editor.state.doc;
  editor.view.dispatch(closeHistory(editor.state.tr.delete(range.from, range.to))
    .setMeta(notesDeleteCommentMeta, range.from)
    .setMeta(notesIsolatedMeta, true)
    .setStoredMarks([]));
  return !editor.state.doc.eq(previous);
}

function onlyCommentDiffers(before: PmNode, after: PmNode, position: unknown): boolean {
  if (typeof position !== 'number' || !Number.isInteger(position)
    || position < 0 || position >= after.content.size) return false;
  const comment = after.nodeAt(position);
  if (!isNoteComment(comment)) return false;
  return withoutHighlight(before).eq(withoutHighlight(
    new Transform(after).delete(position, position + comment.nodeSize).doc,
  ));
}

export function createNotesEditor(options: NotesEditorOptions): Editor {
  const byteLimit = options.byteLimit ?? noteDocumentByteLimit;
  const gate: Gate = { blocked: false };
  let editor!: Editor;
  const editorProps = {
    dispatchTransaction(transaction: Transaction) {
      acceptNotesTransaction(editor, transaction, byteLimit, gate, options.onCommandError);
    },
    transformPastedHTML(html: string) {
      return transformClipboardHTML(html);
    },
    handleClick(view: EditorView, _pos: number, event: MouseEvent) {
      if (event.button !== 0) return false;
      const href = linkHrefAtTarget(view, event.target);
      if (href === null) return false;
      const destination = storedWebDestination(href);
      if (destination) window.open(destination, '_blank', 'noopener,noreferrer');
      return true;
    },
    handleDOMEvents: {
      // ProseMirror cancels only mouseup. A frozen editor would still follow the anchor.
      click(view: EditorView, event: MouseEvent) {
        const anchor = event.target instanceof Element ? event.target.closest('a') : null;
        if (anchor && view.dom.contains(anchor)) event.preventDefault();
        return false;
      },
    },
    handlePaste(view: EditorView, event: ClipboardEvent, slice: Slice) {
      if (slice.size > 0) {
        if (!hasPicture(slice.content)) return false;
        void importNoteSlice(editor, slice, options.readPicture, () => true, 'paste', byteLimit);
        return true;
      }
      const file = clipboardImageFile(event.clipboardData);
      if (file) {
        void importPictureFile(editor, file, () => !editor.isDestroyed && editor.isEditable)
          .then((message) => {
            if (message) options.onCommandError?.(message);
          });
        return true;
      }
      const text = clipboardPlainText(event);
      if (text) {
        view.pasteText(text);
        return true;
      }
      return true;
    },
    handleDrop(view: EditorView, event: DragEvent, slice: Slice, moved: boolean) {
      if (dropHasFiles(event)) return true;
      if (slice.size === 0) return true;
      if (!moved && hasPicture(slice.content)) {
        const target = view.posAtCoords({ left: event.clientX, top: event.clientY });
        const $pos = target && view.state.doc.resolve(target.pos);
        // A note without text has no cursor position, and a selected picture would be replaced.
        const at = $pos && TextSelection.between($pos, $pos);
        if (at?.empty) {
          void importNoteSlice(editor, slice, options.readPicture, () => true, 'drop', byteLimit,
            at.getBookmark());
          return true;
        }
      }
      const imageError = fragmentImageError(slice.content);
      if (imageError) {
        options.onCommandError?.(imageError);
        return true;
      }
      return false;
    },
  };
  editor = new Editor({
    element: options.element === undefined ? document.createElement('div') : options.element,
    content: options.content ?? '',
    extensions: notesExtensions(),
    coreExtensionOptions: { tabindex: { value: '0' } },
    injectCSS: false,
    enableInputRules: false,
    enablePasteRules: ['link'],
    enableContentCheck: true,
    onContentError: ({ editor: created }) => {
      gate.blocked = true;
      created.setEditable(false, false);
      options.onRejectedContent?.();
    },
    // Tiptap 3.30.3 types EditorProps without this ProseMirror view callback.
    editorProps: editorProps as EditorProps,
  });
  editor.on('update', () => {
    if (gate.blocked) return;
    options.onAcceptedDocument?.();
  });
  return editor;
}

export type NoteHeading = { position: number; level: number; text: string };

export function noteHeadings(doc: PmNode): NoteHeading[] {
  const headings: NoteHeading[] = [];
  doc.descendants((node, position) => {
    if (node.type.name === 'heading') {
      const text = node.textBetween(0, node.content.size, ' ', ' ').replace(/\s+/gu, ' ').trim();
      if (text) headings.push({ position, level: node.attrs.level as number, text });
    }
    return !node.isTextblock;
  });
  return headings;
}

export function selectNoteHeading(editor: Editor, position: number): boolean {
  if (editor.isDestroyed) return false;
  const doc = editor.state.doc;
  if (position < 0 || position >= doc.content.size || doc.nodeAt(position)?.type.name !== 'heading') {
    return false;
  }
  editor.view.dispatch(editor.state.tr
    .setSelection(TextSelection.create(doc, position + 1))
    .setMeta('addToHistory', false));
  return true;
}

// Opening places the caret at the end. A final image uses a gap cursor.
// The selection is not a history step.
export function selectNoteEnd(editor: Editor): void {
  if (editor.isDestroyed) return;
  const doc = editor.state.doc;
  const last = doc.lastChild;
  let selection = TextSelection.atEnd(doc);
  if (last?.type.name === 'image') {
    const $pos = doc.resolve(doc.content.size);
    const valid = (GapCursor as unknown as { valid?: (pos: unknown) => boolean }).valid;
    if (!valid || valid($pos)) selection = new GapCursor($pos);
  }
  const tr = editor.state.tr.setSelection(selection);
  tr.setMeta('addToHistory', false);
  editor.view.dispatch(tr);
}

const inactiveSelectionKey = new PluginKey<boolean>('notesInactiveSelection');

// The browser hides a selection when the editor blurs. This decoration redraws
// that same selection. A microtask avoids dispatching inside the focus event.
function inactiveSelectionPlugin(): Plugin {
  return new Plugin<boolean>({
    key: inactiveSelectionKey,
    state: {
      init: () => false,
      apply(tr, value) {
        const next = tr.getMeta(inactiveSelectionKey);
        return typeof next === 'boolean' ? next : value;
      },
    },
    props: {
      decorations(state) {
        if (inactiveSelectionKey.getState(state) === true) return null;
        return inactiveDecorations(state);
      },
    },
    view(view) {
      const refresh = () => {
        if (view.isDestroyed) return;
        const focused = view.hasFocus();
        if (inactiveSelectionKey.getState(view.state) === focused) return;
        view.dispatch(
          view.state.tr.setMeta(inactiveSelectionKey, focused).setMeta('addToHistory', false),
        );
      };
      const schedule = () => {
        queueMicrotask(refresh);
      };
      view.dom.addEventListener('focus', schedule, true);
      view.dom.addEventListener('blur', schedule, true);
      queueMicrotask(refresh);
      return {
        destroy() {
          view.dom.removeEventListener('focus', schedule, true);
          view.dom.removeEventListener('blur', schedule, true);
        },
      };
    },
  });
}

// A widget at the start of a textblock becomes that block's first DOM child.
// Mark the block itself so the text node stays first.
function collapsedCaret(state: EditorState): Decoration {
  const $head = state.selection.$head;
  if ($head.parent.isTextblock && $head.parentOffset === 0) {
    const from = $head.before();
    return Decoration.node(from, from + $head.parent.nodeSize, {
      class: 'cha-notes-inactive-caret',
    });
  }
  return Decoration.widget(state.selection.head, () => {
    const caret = document.createElement('span');
    caret.className = 'cha-notes-inactive-caret';
    caret.setAttribute('aria-hidden', 'true');
    return caret;
  }, { side: -1, ignoreSelection: true, key: 'notes-caret' });
}

function inactiveDecorations(state: EditorState): DecorationSet {
  const selection = state.selection;
  if (selection instanceof NodeSelection) {
    return DecorationSet.create(state.doc, [
      Decoration.node(selection.from, selection.to, { class: 'cha-notes-inactive-node' }),
    ]);
  }
  if (selection.empty) {
    return DecorationSet.create(state.doc, [collapsedCaret(state)]);
  }
  const marks: Decoration[] = [];
  state.doc.nodesBetween(selection.from, selection.to, (node, pos) => {
    if (node.isText) {
      const from = Math.max(pos, selection.from);
      const to = Math.min(pos + node.nodeSize, selection.to);
      if (from < to) {
        marks.push(Decoration.inline(from, to, { class: 'cha-notes-inactive-selection' }));
      }
      return false;
    }
    if (node.type.name === 'image' && pos >= selection.from && pos + node.nodeSize <= selection.to) {
      marks.push(Decoration.node(pos, pos + node.nodeSize, { class: 'cha-notes-inactive-node' }));
    }
  });
  if (marks.length === 0) return DecorationSet.empty;
  return DecorationSet.create(state.doc, marks);
}

export function insertPromptParagraph(editor: Editor, text: string): boolean {
  if (editor.isDestroyed || !editor.isEditable) return false;
  const normalized = text.replace(/\r\n?/g, '\n');
  if (!normalized.trim()) return false;
  const paragraph = buildParagraph(editor.schema, normalized);
  const tr = editor.state.tr;
  if (!placeParagraph(tr, paragraph)) return false;
  closeHistory(tr);
  tr.setMeta(notesIsolatedMeta, true);
  tr.setMeta('preventAutolink', true);
  tr.setStoredMarks([]);
  const before = editor.state.doc;
  editor.view.dispatch(tr);
  return !editor.state.doc.eq(before);
}

function hasPicture(fragment: Fragment): boolean {
  let found = false;
  fragment.descendants((node) => {
    if (node.type.name === 'image') found = true;
  });
  return found;
}

// Page import, paste, and drop use the existing embedded picture nodes.
async function importNoteSlice(
  editor: Editor,
  slice: Slice,
  readPicture: NotePictureReader,
  stillCurrent: () => boolean,
  place: 'end' | 'replace' | 'paste' | 'drop',
  byteLimit: number,
  at: SelectionBookmark = editor.state.selection.getBookmark(),
): Promise<boolean> {
  const available = () => !editor.isDestroyed && editor.isEditable && stillCurrent();
  if (!available()) return false;
  const tracked = trackSelectionBookmark(editor, at);
  const previousImport = noteImportTail;
  let finishImport!: () => void;
  noteImportTail = new Promise<void>((resolve) => { finishImport = resolve; });
  try {
    await previousImport;
    if (!available()) return false;
    const sources: string[] = [];
    slice.content.descendants((node) => {
      if (node.type.name === 'image') sources.push(String(node.attrs.src ?? ''));
    });
    const pictures: Array<string | null> = [];
    let budget = byteLimit;
    // Leave room for saves and other requests under the bridge's 16-request limit.
    for (let start = 0; start < sources.length; start += 4) {
      if (!available()) return false;
      const batch = await Promise.all(sources.slice(start, start + 4).map((src) => loadNotePicture(src, readPicture)));
      for (const src of batch) {
        const kept = src && src.length <= budget ? src : null;
        if (kept) budget -= kept.length;
        pictures.push(kept);
      }
    }
    if (!available()) return false;
    let next = 0;
    const embed = (fragment: Fragment): Fragment => {
      const nodes: PmNode[] = [];
      fragment.forEach((node) => {
        if (node.type.name !== 'image') {
          nodes.push(node.isLeaf ? node : node.copy(embed(node.content)));
          return;
        }
        const src = pictures[next++];
        if (src) {
          nodes.push(node.type.create({ ...node.attrs, src }, null, node.marks));
        }
      });
      return Fragment.fromArray(nodes);
    };
    let content = embed(slice.content);
    if (place === 'replace' && !content.textBetween(0, content.size).trim() && !hasPicture(content)) {
      throw new ChaError('invalid_argument', noteFileNoText);
    }
    if (!content.size) {
      if (place === 'end') throw new Error('The page has no readable text.');
      return false;
    }
    const build = () => {
      const tr = editor.state.tr;
      if (place === 'end' || place === 'replace') {
        const doc = editor.state.doc;
        const empty = doc.childCount === 1 && doc.firstChild?.type.name === 'paragraph'
          && doc.firstChild.content.size === 0;
        tr.replaceWith(empty || place === 'replace' ? 0 : doc.content.size, doc.content.size, content);
      } else {
        const open = Slice.maxOpen(content);
        const selection = tracked.current().resolve(editor.state.doc);
        tr.setSelection(selection).replaceSelection(
          new Slice(content, Math.min(slice.openStart, open.openStart), Math.min(slice.openEnd, open.openEnd)),
        );
        if (place === 'paste') {
          let end: number | undefined;
          tr.mapping.maps.at(-1)?.forEach((_from, _to, _newFrom, newTo) => { end ??= newTo; });
          if (end !== undefined) {
            const live = editor.state.selection;
            const movedOutside = live.from < selection.from || live.to > selection.to;
            // Keep a selection moved elsewhere; otherwise leave a caret after
            // the slice, including a final picture that the next paste must keep.
            tr.setSelection(movedOutside
              ? live.map(tr.doc, tr.mapping)
              : new GapCursor(tr.doc.resolve(end)).getBookmark().resolve(tr.doc));
            tr.setMeta(notesPasteSelectionMeta, { from: selection, to: end });
          }
        }
      }
      closeHistory(tr);
      tr.setMeta(notesIsolatedMeta, true);
      tr.setMeta('preventAutolink', true);
      tr.setMeta(notesPictureFallbackMeta, withoutLastPicture);
      tr.setStoredMarks([]);
      // Paste rules and scrolling expect ProseMirror's paste and drop metadata.
      if (place === 'paste' || place === 'drop') tr.setMeta('uiEvent', place).scrollIntoView();
      return tr;
    };
    // The size gate can retry after paste rules, without changing existing pictures.
    const withoutLastPicture = (): Transaction | null => {
      let removed = false;
      const omitLastPicture = (fragment: Fragment): Fragment => {
        const nodes = [...fragment.content];
        for (let index = nodes.length - 1; index >= 0 && !removed; index -= 1) {
          const node = nodes[index];
          if (node.type.name === 'image') {
            nodes.splice(index, 1);
            removed = true;
          } else if (!node.isLeaf) {
            nodes[index] = node.copy(omitLastPicture(node.content));
          }
        }
        return Fragment.fromArray(nodes);
      };
      content = omitLastPicture(content);
      return removed && content.size ? build() : null;
    };
    const before = editor.state.doc;
    editor.view.dispatch(build());
    return !editor.state.doc.eq(before) || (place === 'replace' && editor.state.doc.content.eq(content));
  } finally {
    tracked.stop();
    finishImport();
  }
}

export async function appendNoteHtml(
  editor: Editor,
  html: string,
  readPicture: NotePictureReader,
  stillCurrent: () => boolean,
  byteLimit = noteDocumentByteLimit,
): Promise<boolean> {
  const body = document.createElement('div');
  body.innerHTML = html;
  const content = PmDOMParser.fromSchema(editor.schema).parse(body).content;
  return importNoteSlice(editor, new Slice(content, 0, 0), readPicture, stillCurrent, 'end', byteLimit);
}

export const noteFileNoText = 'The file has no readable text.';

export async function replaceNoteHtml(
  editor: Editor,
  html: string,
  readPicture: NotePictureReader,
  stillCurrent: () => boolean,
  preserveWhitespace = false,
): Promise<boolean> {
  const body = document.createElement('div');
  body.innerHTML = html;
  const content = PmDOMParser.fromSchema(editor.schema).parse(body, { preserveWhitespace }).content;
  return importNoteSlice(editor, new Slice(content, 0, 0), readPicture, stillCurrent, 'replace', noteDocumentByteLimit);
}

export function notesExtensions() {
  return [
    StarterKit.configure({
      blockquote: false,
      strike: false,
      horizontalRule: false,
      trailingNode: false,
      dropcursor: false,
      link: {
        autolink: true,
        openOnClick: false,
        markdownLinks: false,
        protocols: [],
        defaultProtocol: 'http',
        isAllowedUri: (url) => isAllowedNoteLink(url),
        shouldAutoLink: (url) => isAllowedNoteLink(url),
      },
    }),
    Image.configure({ allowBase64: true, inline: false, resize: false }),
    Highlight.configure({ multicolor: true }),
    FindAndReplace.configure({ injectCSS: false, searchDebounceMs: 0 }),
    Extension.create({
      name: 'notesInactiveSelection',
      addProseMirrorPlugins() {
        return [inactiveSelectionPlugin()];
      },
    }),
  ];
}

// Dispatch adapter for Tiptap 3.30.3 Editor.dispatchTransaction.
// applyTransaction already folded plugin appends (including autolink) into
// `state`. The size check uses that candidate. Rejection does not call
// updateState with it and reports the limit. Event order on acceptance
// matches the upstream method.
function acceptNotesTransaction(
  editor: Editor,
  transaction: Transaction,
  byteLimit: number,
  gate: Gate,
  onCommandError?: (message: string) => void,
): void {
  const capturing = captureState(editor);
  if (editor.isDestroyed) return;
  if (capturing.isCapturingTransaction) {
    if (!capturing.capturedTransaction) {
      capturing.capturedTransaction = transaction;
      return;
    }
    transaction.steps.forEach((step) => {
      capturing.capturedTransaction?.step(step);
    });
    return;
  }
  if (gate.blocked) return;
  const prevState = editor.state;
  for (;;) {
    tagIsolated(transaction);
    const { state, transactions } = prevState.applyTransaction(transaction);
    const rootApplied = transactions.includes(transaction);
    if (!rootApplied) {
      editor.emit('beforeTransaction', { editor, transaction, nextState: state });
      return;
    }
    const changed = transactions.some((item) => item.docChanged) && !prevState.doc.eq(state.doc);
    if (changed && !editor.isEditable && !(
      (transaction.getMeta(notesHighlightMeta) === true
        && withoutHighlight(prevState.doc).eq(withoutHighlight(state.doc)))
      || onlyCommentDiffers(prevState.doc, state.doc, transaction.getMeta(notesCommentMeta))
      || onlyCommentDiffers(state.doc, prevState.doc, transaction.getMeta(notesDeleteCommentMeta))
    )) return;
    if (changed && noteDocumentByteLength(state.doc) > byteLimit) {
      const withoutPicture = transaction.getMeta(notesPictureFallbackMeta) as (() => Transaction | null) | undefined;
      const retry = withoutPicture?.();
      if (retry) {
        transaction = retry;
        continue;
      }
      onCommandError?.(noteTooLargeMessage);
      return;
    }
    publishNotesTransaction(editor, transaction, prevState, state, transactions);
    if (
      changed
      && transaction.getMeta(notesIsolatedMeta) === true
      && !isHistoryTransaction(transaction)
    ) {
      const closeTr = closeHistory(editor.state.tr);
      closeTr.setMeta(notesIsolatedCloseMeta, true);
      editor.view.dispatch(closeTr);
    }
    return;
  }
}

function fragmentImageError(fragment: Fragment): string | null {
  let error: string | null = null;
  fragment.descendants((node) => {
    if (error || node.type.name !== 'image') return;
    error = embeddedImageError(node.attrs.src);
    return false;
  });
  return error;
}

function clipboardPlainText(event: ClipboardEvent): string {
  const data = event.clipboardData;
  if (!data) return '';
  return data.getData('text/plain');
}

function dropHasFiles(event: DragEvent): boolean {
  const transfer = event.dataTransfer;
  if (!transfer) return false;
  if (transfer.files && transfer.files.length > 0) return true;
  return Array.from(transfer.types).includes('Files');
}

// A pointer acts on a link only when the event target is inside its <a> element.
// A click next to a link, such as after a link at a line end, is not on it.
function linkHrefAtTarget(view: EditorView, target: EventTarget | null): string | null {
  const anchor = target instanceof Element ? target.closest('a') : null;
  if (!anchor || !view.dom.contains(anchor)) return null;
  const mark = view.state.doc.nodeAt(view.posAtDOM(anchor, 0))?.marks
    .find((item) => item.type.name === 'link');
  return typeof mark?.attrs.href === 'string' ? mark.attrs.href : null;
}

export function trackSelectionBookmark(
  editor: Editor,
  start: SelectionBookmark = editor.state.selection.getBookmark(),
): {
  current(): SelectionBookmark;
  stop(): void;
} {
  let bookmark = start;
  const apply = ({ transaction }: { transaction: Transaction }) => {
    const pasted = transaction.getMeta(notesPasteSelectionMeta) as { from: Selection; to: number } | undefined;
    // Another paste at the same selection follows the entire inserted slice,
    // including a final picture where a text bookmark cannot place the caret.
    bookmark = pasted?.from.eq(bookmark.resolve(transaction.before))
      ? new GapCursor(transaction.doc.resolve(pasted.to)).getBookmark()
      : bookmark.map(transaction.mapping);
  };
  editor.on('transaction', apply);
  return {
    current: () => bookmark,
    stop: () => {
      editor.off('transaction', apply);
    },
  };
}

export function insertTrackedPicture(
  editor: Editor,
  bookmark: SelectionBookmark,
  src: string,
): 'inserted' | 'rejected' | 'unavailable' {
  if (editor.isDestroyed || !editor.isEditable) return 'unavailable';
  let selection;
  try {
    selection = bookmark.resolve(editor.state.doc);
  } catch {
    return 'unavailable';
  }
  const image = editor.schema.nodes.image.create({ src });
  const tr = editor.state.tr.setSelection(selection);
  tr.replaceSelectionWith(image, false);
  closeHistory(tr);
  tr.setMeta(notesIsolatedMeta, true);
  const before = editor.state.doc;
  editor.view.dispatch(tr);
  return editor.state.doc.eq(before) ? 'rejected' : 'inserted';
}

export async function importPictureFile(
  editor: Editor,
  file: File,
  stillCurrent: () => boolean,
): Promise<string | null> {
  // File.size is the byte length. A large file is not read only to be rejected.
  if (file.size > pictureByteLimit) return pictureTooLargeMessage;
  const tracked = trackSelectionBookmark(editor);
  const read = readFileBytes(file);
  const cancelRead = () => read.release();
  editor.on('destroy', cancelRead);
  try {
    const bytes = await read.bytes;
    if (!stillCurrent() || editor.isDestroyed || !editor.isEditable) return null;
    const picture = pictureFromBytes(bytes);
    if ('error' in picture) return picture.error;
    // The acceptance boundary reports a note that would become too large.
    insertTrackedPicture(editor, tracked.current(), picture.src);
    return null;
  } catch (error) {
    if (error instanceof DOMException && error.name === 'AbortError') return null;
    return pictureTypeMessage;
  } finally {
    editor.off('destroy', cancelRead);
    tracked.stop();
    read.release();
  }
}

const removedClipboardTags = new Set([
  'SCRIPT', 'STYLE', 'IFRAME', 'OBJECT', 'EMBED', 'APPLET', 'VIDEO', 'AUDIO', 'CANVAS', 'SVG', 'FORM',
]);

export function transformClipboardHTML(html: string): string {
  const document = new DOMParser().parseFromString(html, 'text/html');
  cleanClipboardNode(document.body, wordClipboardHTML(html, document));
  return document.body.innerHTML;
}

// Office namespace, or a Generator/ProgId meta marker. Body text is not a marker.
function wordClipboardHTML(html: string, document: Document): boolean {
  if (/schemas-microsoft-com:office/iu.test(html)) return true;
  for (const meta of document.querySelectorAll('meta')) {
    const name = (meta.getAttribute('name') ?? '').trim().toLowerCase();
    const content = meta.getAttribute('content') ?? '';
    if (name === 'generator' && /microsoft\s+word/iu.test(content)) return true;
    if (name === 'progid' && /word\.document/iu.test(content)) return true;
  }
  return false;
}

function cleanClipboardNode(node: Node, word: boolean): void {
  for (const child of [...node.childNodes]) cleanClipboardNode(child, word);
  if (!(node instanceof HTMLElement)) return;
  const tag = node.tagName;
  if (removedClipboardTags.has(tag)) {
    node.remove();
    return;
  }
  if (tag === 'IMG') {
    const src = node.getAttribute('src') ?? '';
    if (src.startsWith('data:')) return;
    try {
      const url = new URL(src, node.ownerDocument.baseURI);
      if (['http:', 'https:'].includes(url.protocol)) node.setAttribute('src', url.href);
      else node.remove();
    } catch {
      node.remove();
    }
    return;
  }
  if (node instanceof HTMLTableElement) {
    flattenClipboardTable(node);
    return;
  }
  if (tag === 'A') {
    normalizeClipboardAnchor(node);
    return;
  }
  if (word && wordHighlight(node)) {
    const mark = node.ownerDocument.createElement('mark');
    while (node.firstChild) mark.appendChild(node.firstChild);
    node.replaceWith(mark);
    return;
  }
  stripClipboardPresentation(node);
}

function wordHighlight(element: HTMLElement): boolean {
  const names = (element.getAttribute('style') ?? '')
    .split(';')
    .map((part) => part.split(':')[0]?.trim().toLowerCase() ?? '');
  if (names.includes('mso-highlight')) return true;
  return element.tagName === 'SPAN'
    && (names.includes('background') || names.includes('background-color'));
}

function normalizeClipboardAnchor(anchor: HTMLElement): void {
  const destination = canonicalNoteLink(anchor.getAttribute('href') ?? '');
  if (!destination) {
    anchor.replaceWith(...anchor.childNodes);
    return;
  }
  anchor.setAttribute('href', destination);
  anchor.removeAttribute('target');
  anchor.removeAttribute('rel');
}

export function flattenClipboardTable(table: HTMLTableElement): void {
  const fragment = table.ownerDocument.createDocumentFragment();
  if (table.caption) {
    const caption = table.ownerDocument.createElement('div');
    caption.append(...table.caption.childNodes);
    fragment.append(caption);
  }
  for (const row of [...table.rows]) {
    const paragraph = table.ownerDocument.createElement('p');
    // ProseMirror keeps a tab only when the element white-space contains "pre".
    paragraph.style.whiteSpace = 'pre-wrap';
    for (let index = 0; index < row.cells.length; index += 1) {
      if (index > 0) paragraph.append('\t');
      appendClipboardCell(paragraph, row.cells[index]);
    }
    fragment.append(paragraph);
  }
  table.replaceWith(fragment);
}

function appendClipboardCell(target: HTMLElement, cell: HTMLElement): void {
  const start = target.childNodes.length;
  for (const child of [...cell.childNodes]) {
    if (child instanceof HTMLElement && (child.tagName === 'P' || child.tagName === 'DIV')) {
      if (target.childNodes.length > start) target.appendChild(target.ownerDocument.createElement('br'));
      while (child.firstChild) target.appendChild(child.firstChild);
      child.remove();
    } else {
      target.appendChild(child);
    }
  }
}

function stripClipboardPresentation(element: HTMLElement): void {
  if (element.tagName === 'FONT') element.removeAttribute('color');
  if (element.tagName === 'MARK') element.removeAttribute('data-color');
  const style = element.getAttribute('style');
  if (!style) return;
  const kept = style.split(';').map((part) => part.trim()).filter((part) => {
    if (!part) return false;
    const name = part.split(':')[0]?.trim().toLowerCase() ?? '';
    return name !== 'color' && name !== 'background' && name !== 'background-color' && !name.startsWith('mso-');
  });
  if (kept.length === 0) element.removeAttribute('style');
  else element.setAttribute('style', kept.join('; '));
}

function tagIsolated(transaction: Transaction): void {
  if (transaction.getMeta(notesIsolatedCloseMeta) || isHistoryTransaction(transaction)) return;
  const uiEvent = transaction.getMeta('uiEvent');
  const isolate = transaction.getMeta(notesIsolatedMeta) === true
    || uiEvent === 'paste'
    || uiEvent === 'drop';
  if (!isolate) return;
  closeHistory(transaction);
  transaction.setMeta(notesIsolatedMeta, true);
}

function publishNotesTransaction(
  editor: Editor,
  transaction: Transaction,
  prevState: EditorState,
  state: EditorState,
  transactions: readonly Transaction[],
): void {
  const selectionChanged = !prevState.selection.eq(state.selection);
  editor.emit('beforeTransaction', { editor, transaction, nextState: state });
  editor.view.updateState(state);
  editor.emit('transaction', {
    editor,
    transaction,
    appendedTransactions: transactions.slice(1),
  });
  if (selectionChanged) editor.emit('selectionUpdate', { editor, transaction });
  let focusTr: Transaction | undefined;
  for (let index = transactions.length - 1; index >= 0; index -= 1) {
    const item = transactions[index];
    if (item.getMeta('focus') || item.getMeta('blur')) {
      focusTr = item;
      break;
    }
  }
  const focus = focusTr?.getMeta('focus') as { event: FocusEvent } | undefined;
  const blur = focusTr?.getMeta('blur') as { event: FocusEvent } | undefined;
  if (focus && focusTr) editor.emit('focus', { editor, event: focus.event, transaction: focusTr });
  if (blur && focusTr) editor.emit('blur', { editor, event: blur.event, transaction: focusTr });
  const changed = transactions.some((item) => item.docChanged) && !prevState.doc.eq(state.doc);
  if (transaction.getMeta('preventUpdate') || !changed) return;
  editor.emit('update', {
    editor,
    transaction,
    appendedTransactions: transactions.slice(1),
  });
}

function buildParagraph(schema: Schema, text: string): PmNode {
  const lines = text.split('\n');
  const content: PmNode[] = [];
  lines.forEach((line, index) => {
    content.push(...linkifyLine(schema, line));
    if (index < lines.length - 1) content.push(schema.nodes.hardBreak.create());
  });
  return schema.nodes.paragraph.create(null, content);
}

function linkifyLine(schema: Schema, line: string): PmNode[] {
  if (!line) return [];
  const link = schema.marks.link;
  const nodes: PmNode[] = [];
  let cursor = 0;
  for (const match of find(line, { defaultProtocol: 'http' })) {
    if (match.type !== 'url' || !match.isLink || match.start < cursor) continue;
    const href = canonicalNoteLink(match.value);
    if (!href) continue;
    if (match.start > cursor) nodes.push(schema.text(line.slice(cursor, match.start)));
    nodes.push(schema.text(line.slice(match.start, match.end), [link.create({ href })]));
    cursor = match.end;
  }
  if (cursor < line.length) nodes.push(schema.text(line.slice(cursor)));
  return nodes;
}

function placeParagraph(tr: Transaction, paragraph: PmNode): boolean {
  const { selection } = tr;
  if (selection instanceof AllSelection) {
    tr.replaceWith(0, tr.doc.content.size, paragraph);
    setCursorAtEnd(tr, 0);
    return true;
  }
  if (selection instanceof NodeSelection) {
    if (selection.node.type.name !== 'image') return false;
    const pos = selection.from;
    tr.replaceWith(pos, selection.to, paragraph);
    setCursorAtEnd(tr, pos);
    return true;
  }
  if (selection instanceof GapCursor) {
    const pos = selection.from;
    tr.insert(pos, paragraph);
    setCursorAtEnd(tr, pos);
    return true;
  }
  const { $from, $to } = selection;
  if (!$from.parent.isTextblock || !$to.parent.isTextblock) return false;
  if ($from.sameParent($to)) return replaceTextblock(tr, paragraph);
  return replaceAcross(tr, paragraph);
}

function replaceTextblock(tr: Transaction, paragraph: PmNode): boolean {
  const { $from, $to } = tr.selection;
  const start = $from.before();
  const prefix = $from.parent.cut(0, $from.parentOffset);
  const suffix = $to.parent.cut($to.parentOffset);
  const nodes: PmNode[] = [];
  if (prefix.content.size > 0) nodes.push(prefix);
  nodes.push(paragraph);
  if (suffix.content.size > 0) nodes.push(suffix);
  tr.replaceWith(start, $from.after(), nodes);
  const paragraphPos = prefix.content.size > 0 ? start + prefix.nodeSize : start;
  setCursorAtEnd(tr, paragraphPos);
  return true;
}

function replaceAcross(tr: Transaction, paragraph: PmNode): boolean {
  const { $from, $to } = tr.selection;
  const depth = $from.sharedDepth($to.pos);
  if (depth >= $from.depth || depth >= $to.depth) return false;
  const parent = $from.node(depth);
  const startPos = $from.before(depth + 1);
  const endPos = $to.after(depth + 1);
  const keptStart = cutBefore($from.node(depth + 1), startPos, $from.pos);
  const keptEnd = cutAfter($to.node(depth + 1), $to.before(depth + 1), $to.pos);
  const { nodes, paragraphPos } = replacementNodes(
    parent,
    keptStart,
    keptEnd,
    paragraph,
    startPos,
    $from,
    depth + 1,
  );
  tr.replaceWith(startPos, endPos, nodes);
  setCursorAtEnd(tr, paragraphPos);
  return true;
}

function replacementNodes(
  parent: PmNode,
  keptStart: PmNode | null,
  keptEnd: PmNode | null,
  paragraph: PmNode,
  startPos: number,
  $from: ResolvedPos,
  keptDepth: number,
): { nodes: PmNode[]; paragraphPos: number } {
  const nodes: PmNode[] = [];
  let paragraphPos = startPos;
  if (keptStart && isListContainer(keptStart)) {
    const inserted = insertIntoSurviving(keptStart, paragraph, $from, keptDepth);
    nodes.push(inserted.node);
    paragraphPos = startPos + inserted.offset;
  } else if (keptStart) {
    nodes.push(keptStart);
    nodes.push(paragraph);
    paragraphPos = startPos + keptStart.nodeSize;
  } else {
    const fitted = fitBlock(parent, paragraph);
    nodes.push(fitted);
    paragraphPos = fitted === paragraph ? startPos : startPos + 1;
  }
  if (keptEnd) nodes.push(keptEnd);
  return { nodes, paragraphPos };
}

function isListContainer(node: PmNode): boolean {
  return node.type.name === 'listItem'
    || node.type.name === 'bulletList'
    || node.type.name === 'orderedList';
}

function fitBlock(parent: PmNode, paragraph: PmNode): PmNode {
  if (parent.type.name === 'bulletList' || parent.type.name === 'orderedList') {
    return parent.type.schema.nodes.listItem.create(null, paragraph);
  }
  return paragraph;
}

// `node` is the kept part of $from.node(depth). Follows the kept children down
// to the item that held the selection start. When cutBefore removed that item,
// the last kept child is an earlier sibling; the list then gets a new item, so
// the paragraph does not join an unselected bullet.
function insertIntoSurviving(
  node: PmNode,
  paragraph: PmNode,
  $from: ResolvedPos,
  depth: number,
): { node: PmNode; offset: number } {
  const children: PmNode[] = [];
  node.forEach((child) => children.push(child));
  const lastIndex = children.length - 1;
  const isList = (item: PmNode) => item.type.name === 'bulletList' || item.type.name === 'orderedList';
  const lastHeldStart = lastIndex === $from.index(depth);
  if (lastHeldStart && (isList(node) || isList(children[lastIndex]))) {
    const inserted = insertIntoSurviving(children[lastIndex], paragraph, $from, depth + 1);
    children[lastIndex] = inserted.node;
    let offset = 1;
    for (let index = 0; index < lastIndex; index += 1) offset += children[index].nodeSize;
    return {
      node: node.type.createChecked(node.attrs, children),
      offset: offset + inserted.offset,
    };
  }
  const added = isList(node) ? node.type.schema.nodes.listItem.create(null, paragraph) : paragraph;
  children.push(added);
  let offset = added === paragraph ? 1 : 2;
  for (let index = 0; index < children.length - 1; index += 1) offset += children[index].nodeSize;
  return { node: node.type.createChecked(node.attrs, children), offset };
}

function cutBefore(node: PmNode, nodePos: number, pos: number): PmNode | null {
  if (pos <= nodePos) return null;
  if (pos >= nodePos + node.nodeSize) return node;
  if (node.isTextblock) {
    const offset = pos - (nodePos + 1);
    if (offset <= 0) return null;
    const cut = node.cut(0, offset);
    return cut.content.size > 0 ? cut : null;
  }
  const children: PmNode[] = [];
  let childPos = nodePos + 1;
  for (let index = 0; index < node.childCount; index += 1) {
    const child = node.child(index);
    const childEnd = childPos + child.nodeSize;
    if (childEnd <= pos) children.push(child);
    else if (childPos < pos) {
      const kept = cutBefore(child, childPos, pos);
      if (kept) children.push(kept);
      break;
    } else break;
    childPos = childEnd;
  }
  return finishContainer(node, children);
}

function cutAfter(node: PmNode, nodePos: number, pos: number): PmNode | null {
  if (pos >= nodePos + node.nodeSize) return null;
  if (pos <= nodePos) return node;
  if (node.isTextblock) {
    const offset = pos - (nodePos + 1);
    if (offset >= node.content.size) return null;
    const cut = node.cut(offset);
    return cut.content.size > 0 ? cut : null;
  }
  const children: PmNode[] = [];
  let childPos = nodePos + 1;
  let passed = false;
  for (let index = 0; index < node.childCount; index += 1) {
    const child = node.child(index);
    const childEnd = childPos + child.nodeSize;
    if (!passed && pos >= childEnd) {
      childPos = childEnd;
      continue;
    }
    if (!passed && pos > childPos) {
      const kept = cutAfter(child, childPos, pos);
      if (kept) children.push(kept);
      passed = true;
    } else {
      children.push(child);
      passed = true;
    }
    childPos = childEnd;
  }
  return finishContainer(node, children);
}

function finishContainer(node: PmNode, children: PmNode[]): PmNode | null {
  let content = children;
  if (node.type.name === 'listItem' && (content.length === 0 || content[0].type.name !== 'paragraph')) {
    if (content.length === 0) return null;
    content = [node.type.schema.nodes.paragraph.create(), ...content];
  }
  if (node.type.name === 'bulletList' || node.type.name === 'orderedList') {
    content = content.filter((child) => child.type.name === 'listItem');
    if (content.length === 0) return null;
  }
  if (content.length === 0) return null;
  return node.type.createChecked(node.attrs, content);
}

function setCursorAtEnd(tr: Transaction, paragraphPos: number): void {
  const node = tr.doc.nodeAt(paragraphPos);
  if (!node) return;
  const pos = paragraphPos + node.nodeSize - 1;
  tr.setSelection(TextSelection.create(tr.doc, pos));
}
