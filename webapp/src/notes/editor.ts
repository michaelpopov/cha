import { Editor, Extension, type Content } from '@tiptap/core';
import FindAndReplace from '@tiptap/extension-find-and-replace';
import Highlight from '@tiptap/extension-highlight';
import Image from '@tiptap/extension-image';
import { closeHistory, isHistoryTransaction } from '@tiptap/pm/history';
import { GapCursor } from '@tiptap/pm/gapcursor';
import { type Node as PmNode, type Schema, type Slice } from '@tiptap/pm/model';
import {
  AllSelection,
  NodeSelection,
  Plugin,
  PluginKey,
  TextSelection,
  type EditorState,
  type Transaction,
} from '@tiptap/pm/state';
import { Decoration, DecorationSet, type EditorProps, type EditorView } from '@tiptap/pm/view';
import StarterKit from '@tiptap/starter-kit';
import { noteDocumentByteLength, noteDocumentByteLimit } from './documentBytes';

const notesIsolatedMeta = 'notesIsolated';
const notesIsolatedCloseMeta = 'notesIsolatedClose';

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
  onAcceptedDocument?: () => void;
  onRejectedContent?: () => void;
  element?: HTMLElement | null;
};

// Absolute http(s) only. An explicit other scheme is not rewritten.
export function isAllowedNoteLink(href: string): boolean {
  try {
    const url = new URL(href);
    return url.protocol === 'http:' || url.protocol === 'https:';
  } catch {
    return false;
  }
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

export function createNotesEditor(options: NotesEditorOptions = {}): Editor {
  const byteLimit = options.byteLimit ?? noteDocumentByteLimit;
  const gate: Gate = { blocked: false };
  let editor!: Editor;
  const editorProps = {
    dispatchTransaction(transaction: Transaction) {
      acceptNotesTransaction(editor, transaction, byteLimit, gate);
    },
    handlePaste(_view: EditorView, _event: ClipboardEvent, slice: Slice) {
      if (slice.size === 0) return true;
      if (fragmentHasImage(slice.content)) return true;
      return false;
    },
    handleDrop(_view: EditorView, event: DragEvent, slice: Slice) {
      if (dropHasFiles(event)) return true;
      if (slice.size === 0) return true;
      if (fragmentHasImage(slice.content)) return true;
      return false;
    },
  };
  editor = new Editor({
    element: options.element === undefined ? document.createElement('div') : options.element,
    content: options.content ?? '',
    extensions: notesExtensions(),
    injectCSS: false,
    enableInputRules: false,
    enablePasteRules: false,
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

function notesExtensions() {
  return [
    StarterKit.configure({
      code: false,
      codeBlock: false,
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
    Highlight.configure({ multicolor: false }),
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
// updateState with it. Event order on acceptance matches the upstream method.
function acceptNotesTransaction(
  editor: Editor,
  transaction: Transaction,
  byteLimit: number,
  gate: Gate,
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
  tagIsolated(transaction);
  const prevState = editor.state;
  const { state, transactions } = prevState.applyTransaction(transaction);
  const rootApplied = transactions.includes(transaction);
  if (!rootApplied) {
    editor.emit('beforeTransaction', { editor, transaction, nextState: state });
    return;
  }
  const changed = transactions.some((item) => item.docChanged) && !prevState.doc.eq(state.doc);
  if (changed && !editor.isEditable) return;
  if (changed && noteDocumentByteLength(state.doc) > byteLimit) return;
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
}

function fragmentHasImage(fragment: { descendants: (fn: (node: PmNode) => boolean | void) => void }): boolean {
  let found = false;
  fragment.descendants((node) => {
    if (node.type.name === 'image') {
      found = true;
      return false;
    }
  });
  return found;
}

function dropHasFiles(event: DragEvent): boolean {
  const transfer = event.dataTransfer;
  if (!transfer) return false;
  if (transfer.files && transfer.files.length > 0) return true;
  return Array.from(transfer.types).includes('Files');
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

const trailingLinkPunctuation = /[.,;:!?\])]+$/u;
// Explicit http(s) only, the same links that typing and paste create.
const linkPattern = /\bhttps?:\/\/[^\s]+/giu;

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
  for (const range of findLinkRanges(line)) {
    if (range.from > cursor) nodes.push(schema.text(line.slice(cursor, range.from)));
    nodes.push(schema.text(line.slice(range.from, range.to), [link.create({ href: range.href })]));
    cursor = range.to;
  }
  if (cursor < line.length) nodes.push(schema.text(line.slice(cursor)));
  return nodes;
}

type LinkRange = { from: number; to: number; href: string };

function findLinkRanges(line: string): LinkRange[] {
  const ranges: LinkRange[] = [];
  for (const match of line.matchAll(linkPattern)) {
    const index = match.index ?? 0;
    const trimmed = match[0].replace(trailingLinkPunctuation, '');
    if (!isAllowedNoteLink(trimmed)) continue;
    ranges.push({ from: index, to: index + trimmed.length, href: new URL(trimmed).href });
  }
  return ranges;
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
  const { nodes, paragraphPos } = replacementNodes(parent, keptStart, keptEnd, paragraph, startPos);
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
): { nodes: PmNode[]; paragraphPos: number } {
  const nodes: PmNode[] = [];
  let paragraphPos = startPos;
  if (keptStart && isListContainer(keptStart)) {
    const inserted = insertIntoSurviving(keptStart, paragraph);
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

// Follows the last child down to the item that held the selection start.
function insertIntoSurviving(node: PmNode, paragraph: PmNode): { node: PmNode; offset: number } {
  const children: PmNode[] = [];
  node.forEach((child) => children.push(child));
  const lastIndex = children.length - 1;
  const isList = (item: PmNode) => item.type.name === 'bulletList' || item.type.name === 'orderedList';
  if (isList(node) || isList(children[lastIndex])) {
    const inserted = insertIntoSurviving(children[lastIndex], paragraph);
    children[lastIndex] = inserted.node;
    let offset = 1;
    for (let index = 0; index < lastIndex; index += 1) offset += children[index].nodeSize;
    return {
      node: node.type.createChecked(node.attrs, children),
      offset: offset + inserted.offset,
    };
  }
  children.push(paragraph);
  let offset = 1;
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
