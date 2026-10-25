import { hotkeysCoreFeature, syncDataLoaderFeature } from '@headless-tree/core';
import { useTree } from '@headless-tree/react';
import { generateText, getSchema } from '@tiptap/core';
import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type KeyboardEvent as ReactKeyboardEvent,
} from 'react';
import { createPortal } from 'react-dom';

import {
  ChaError,
  publicErrorMessage,
  type NoteCompressionSkip,
  type NoteContent,
  type NoteItem,
  type NotesClient,
} from '../api/client';
import {
  availableNoteName,
  buildNoteIndex,
  focusAfterRemoval,
  type NoteTreeRow,
} from '../notes/tree';
import { NoteAudioDialog, type NoteAudioTarget } from './NoteAudioDialog';
import { noteReference } from '../notes/reference';
import { notesExtensions } from '../notes/editor';
import { noteAudioBlocks } from '../notes/useNoteAudio';

type CreateKind = 'note' | 'folder';

type CreateDialogState = {
  action: 'create';
  kind: CreateKind;
  parentId: number | null;
  name: string;
  error: string | null;
  pending: boolean;
};

type RenameDialogState = {
  action: 'rename';
  sourceId: number;
  name: string;
  error: string | null;
  pending: boolean;
};

type DeleteDialogState = {
  action: 'delete';
  sourceId: number;
  name: string;
  kind: 'note' | 'folder';
  error: string | null;
  pending: boolean;
};

type CompressDialogState = {
  action: 'compress';
  sourceId: number;
  name: string;
  phase: 'saving' | 'compressing';
  controller: AbortController;
  pending: boolean;
  error: string | null;
};

type CompressedDialogState = {
  action: 'show-compressed';
  sourceId: number;
  name: string;
};

type FolderCompressionRow = {
  id: number;
  name: string;
  status: 'Saving…' | 'Compressing…' | 'Compressed' | 'Already compressed' | 'Too small' | 'Cancelled' | 'Failed';
  error?: string;
};

type FolderCompressDialogState = {
  action: 'compress-folder';
  sourceId: number;
  name: string;
  controller: AbortController;
  pending: boolean;
  total: number;
  rows: FolderCompressionRow[];
  error: string | null;
};

type DialogState = CreateDialogState | RenameDialogState | DeleteDialogState | CompressDialogState
  | FolderCompressDialogState | CompressedDialogState;

type MenuState = {
  sourceId: string;
  x: number;
  y: number;
  restore: HTMLElement | null;
};

export type NoteDeleteResult = 'deleted' | 'refresh-failed' | 'busy' | 'failed';

export type DeleteNoteItem = (
  sourceId: number,
  listing: readonly NoteItem[],
  refresh: () => Promise<void>,
) => Promise<NoteDeleteResult>;

export type NoteActions = {
  updated(note: NoteItem): void;
  refresh(): void;
};

export type SetNoteReadOnly = (id: number, readOnly: boolean) => Promise<NoteItem>;

// True when saved, false when cancelled, or the reason a folder note was skipped.
export type CompressNote = (
  id: number, signal: AbortSignal, ready: () => void, skipIneligible?: boolean,
) => Promise<boolean | NoteCompressionSkip>;

type RefreshResult = 'applied' | 'ignored' | 'failed';

export function NotesTree({
  client,
  getNoteLength,
  browsing,
  activeNoteId,
  onOpenNote,
  onBeforeCreateNote,
  onDeleteItem,
  onCompressNote,
  onSetNoteReadOnly,
  onNoteItems,
  vaultName,
  onRegisterNoteActions,
  onSourceChanged,
}: {
  onSourceChanged?(parentId: number | null): void;
  client: NotesClient;
  getNoteLength(id: number): Promise<number>;
  browsing: boolean;
  activeNoteId: number | null;
  onOpenNote(id: number): void;
  onBeforeCreateNote(): Promise<boolean>;
  onDeleteItem: DeleteNoteItem;
  onCompressNote: CompressNote;
  onSetNoteReadOnly: SetNoteReadOnly;
  onNoteItems(items: NoteItem[]): void;
  vaultName: string;
  onRegisterNoteActions?(actions: NoteActions | null): void;
}) {
  const [audioRun, setAudioRun] = useState<{ targets: NoteAudioTarget[]; folderMode: boolean } | null>(null);
  const [items, setItems] = useState<NoteItem[]>([]);
  const [loaded, setLoaded] = useState(false);
  const [stale, setStale] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [listing, setListing] = useState(false);
  const [importing, setImporting] = useState(false);
  const [dialog, setDialog] = useState<DialogState | null>(null);
  const [menu, setMenu] = useState<MenuState | null>(null);
  const generation = useRef(0);
  const alive = useRef(true);
  const started = useRef(false);
  const busy = useRef(false);
  const loadedRef = useRef(false);
  const dataRef = useRef(buildNoteIndex([]));
  const signatureRef = useRef<string | null>(null);
  const revealId = useRef<{ id: number | string; expand: boolean } | null>(null);
  const menuRef = useRef<HTMLDivElement | null>(null);
  const epubInput = useRef<HTMLInputElement | null>(null);
  const epubParentId = useRef<number | null>(null);
  const clientRef = useRef(client);
  const openRef = useRef(onOpenNote);
  const beforeCreateRef = useRef(onBeforeCreateNote);
  const onNoteItemsRef = useRef(onNoteItems);
  const onDeleteRef = useRef(onDeleteItem);
  const onCompressRef = useRef(onCompressNote);
  const itemsRef = useRef(items);
  const staleRef = useRef(stale);
  const listingRef = useRef(listing);
  const dialogRef = useRef(dialog);
  clientRef.current = client;
  openRef.current = onOpenNote;
  beforeCreateRef.current = onBeforeCreateNote;
  onNoteItemsRef.current = onNoteItems;
  onDeleteRef.current = onDeleteItem;
  onCompressRef.current = onCompressNote;
  itemsRef.current = items;
  staleRef.current = stale;
  listingRef.current = listing;
  dialogRef.current = dialog;

  const index = buildNoteIndex(items);
  index.rows.vault = {
    id: 'vault', noteId: null, name: vaultName, kind: 'folder', parentId: null, parentKey: 'root',
  };
  index.children.vault = index.children.root;
  index.children.root = ['vault'];
  for (const key of index.children.vault) index.rows[key].parentKey = 'vault';
  dataRef.current = index;
  const signature = vaultName + '\n' + items.map((item) => (
    `${item.id}:${item.parent_id ?? ''}:${item.kind}:${item.name}:${item.has_compressed}`
  )).join('\n');

  const tree = useTree<NoteTreeRow>({
    rootItemId: 'root',
    initialState: { expandedItems: ['vault'] },
    features: [syncDataLoaderFeature, hotkeysCoreFeature],
    getItemName: (item) => item.getItemData().name,
    isItemFolder: (item) => item.getItemData().kind === 'folder',
    dataLoader: {
      getItem: (id) => dataRef.current.rows[id],
      getChildren: (id) => dataRef.current.children[id] ?? [],
    },
    hotkeys: {
      customActivate: {
        hotkey: 'Enter',
        preventDefault: true,
        handler: (_event, instance) => {
          if (instance.getItems().length === 0) return;
          const item = instance.getFocusedItem();
          if (!item) return;
          if (item.isFolder()) {
            if (item.isExpanded()) item.collapse();
            else item.expand();
            return;
          }
          item.primaryAction();
        },
      },
    },
    onPrimaryAction: (item) => {
      const data = item.getItemData();
      if (data.kind === 'folder' || data.noteId === null) return;
      openRef.current(data.noteId);
    },
  });
  const treeRef = useRef(tree);
  treeRef.current = tree;

  if (signatureRef.current !== signature) {
    const first = signatureRef.current === null;
    signatureRef.current = signature;
    if (!first) tree.scheduleRebuildTree();
  }

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
      const current = dialogRef.current;
      if ((current?.action === 'compress' || current?.action === 'compress-folder') && current.pending) {
        current.controller.abort();
      }
    };
  }, []);

  useEffect(() => {
    if (!menu) return;
    menuRef.current?.querySelector<HTMLButtonElement>('button')?.focus();
    const dismiss = (event: PointerEvent) => {
      const target = event.target;
      if (target instanceof Node && menuRef.current?.contains(target)) return;
      setMenu(null);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => window.removeEventListener('pointerdown', dismiss);
  }, [menu?.sourceId, menu?.x, menu?.y]);

  // Reveal a newly active note. A visible row does not move.
  useEffect(() => {
    if (activeNoteId === null || !dataRef.current.rows[String(activeNoteId)]) return;
    const current = treeRef.current;
    let parent = dataRef.current.rows[String(activeNoteId)].parentKey;
    while (parent && parent !== 'root') {
      current.getItemInstance(parent).expand();
      parent = dataRef.current.rows[parent]?.parentKey ?? null;
    }
    void current.getItemInstance(String(activeNoteId)).scrollTo({ block: 'nearest' }).catch(() => undefined);
  }, [activeNoteId]);

  async function runList(gen: number) {
    setListing(true);
    try {
      const next = await clientRef.current.listNoteItems();
      if (!alive.current || gen !== generation.current) return;
      loadedRef.current = true;
      setItems(next);
      setLoaded(true);
      setStale(false);
      setError(null);
      onNoteItemsRef.current(next);
    } catch (failure: unknown) {
      if (!alive.current || gen !== generation.current) return;
      setError(publicErrorMessage(failure, 'Could not load notes.'));
      if (loadedRef.current) setStale(true);
    } finally {
      if (alive.current && gen === generation.current) setListing(false);
    }
  }

  useEffect(() => {
    if (!browsing || started.current) return;
    started.current = true;
    const gen = ++generation.current;
    void runList(gen);
  }, [browsing]);

  function retryList() {
    const gen = ++generation.current;
    void runList(gen);
  }

  useLayoutEffect(() => {
    const reveal = revealId.current;
    if (reveal === null || dialog !== null) return;
    const key = String(reveal.id);
    if (!dataRef.current.rows[key]) return;
    const current = treeRef.current;
    if (!current.getItems().some((item) => item.getId() === key)) {
      let parent = dataRef.current.rows[key].parentKey;
      const seen = new Set<string>();
      while (parent && parent !== 'root' && !seen.has(parent)) {
        seen.add(parent);
        const folder = current.getItemInstance(parent);
        if (!folder.isExpanded()) folder.expand();
        parent = dataRef.current.rows[parent]?.parentKey ?? null;
      }
      return;
    }
    const target = current.getItemInstance(key);
    if (!target.getElement()) return;
    revealId.current = null;
    if (reveal.expand && target.isFolder()) target.expand();
    target.setFocused();
    // A deferred tree focus could overwrite focus in the newly opened editor.
    target.getElement()?.focus();
    void target.scrollTo().catch(() => undefined);
  });

  function canMutateNow() {
    return loadedRef.current
      && !staleRef.current
      && !listingRef.current
      && !busy.current
      && dialogRef.current === null;
  }

  async function refreshListing(reveal: number | null, expand = false): Promise<RefreshResult> {
    const gen = ++generation.current;
    try {
      const next = await clientRef.current.listNoteItems();
      if (!alive.current || gen !== generation.current) return 'ignored';
      loadedRef.current = true;
      setItems(next);
      setLoaded(true);
      setStale(false);
      setError(null);
      onNoteItemsRef.current(next);
      revealId.current = { id: reveal ?? 'vault', expand };
      return 'applied';
    } catch (failure: unknown) {
      if (!alive.current || gen !== generation.current) return 'ignored';
      setStale(true);
      setError(publicErrorMessage(failure, 'Could not load notes.'));
      return 'failed';
    }
  }

  function keepDialogError(failure: unknown, fallback: string) {
    busy.current = false;
    if (!alive.current) return;
    setDialog((current) => (current ? {
      ...current,
      pending: false,
      error: publicErrorMessage(failure, fallback),
    } : current));
  }

  // A null sourceId checks only the list, as for the vault root.
  function requireFresh(sourceId: number | null): boolean {
    if (!loadedRef.current || staleRef.current || listingRef.current || busy.current) {
      setDialog((current) => (current ? {
        ...current,
        error: 'Refresh the notes list and try again.',
      } : current));
      return false;
    }
    if (sourceId === null || itemsRef.current.some((item) => item.id === sourceId)) return true;
    setDialog((current) => (current ? {
      ...current,
      error: 'That item is no longer in the list.',
    } : current));
    return false;
  }

  function beginCreate(kind: CreateKind, parentId: number | null) {
    if (!canMutateNow()) return;
    if (menu) tree.getItemInstance(menu.sourceId).getElement()?.focus();
    setMenu(null);
    const base = kind === 'note' ? 'New note' : 'New folder';
    const name = availableNoteName(itemsRef.current, parentId, base);
    setDialog({ action: 'create', kind, parentId, name, pending: false, error: null });
  }

  async function submitCreate() {
    const captured = dialogRef.current;
    if (!captured || captured.action !== 'create' || captured.pending || busy.current) return;
    if (captured.name.length === 0) return;
    if (!requireFresh(captured.parentId)) return;
    busy.current = true;
    setDialog({ ...captured, pending: true, error: null });
    try {
      if (captured.kind === 'note' && !await beforeCreateRef.current()) {
        if (alive.current) setDialog({ ...captured, pending: false });
        return;
      }
      if (!alive.current) return;
      const created = await clientRef.current.createNoteItem(captured.parentId, captured.kind, captured.name);
      if (!alive.current) return;
      const refreshed = await finishMutation(created.id, captured.kind === 'note' ? created.id : null);
      // A successful creation opens the new note. Refresh the old view only if it stays open.
      if (alive.current && captured.kind === 'note' && refreshed !== 'applied') onSourceChanged?.(captured.parentId);
    } catch (failure: unknown) {
      keepDialogError(failure, 'The item could not be created.');
    } finally {
      busy.current = false;
    }
  }

  async function importEpub(file: File, parentId: number | null) {
    if (!canMutateNow()) return;
    busy.current = true;
    setImporting(true);
    setError(null);
    try {
      const folderId = await clientRef.current.importEpub(file, itemsRef.current, parentId);
      if (!alive.current) return;
      await refreshListing(folderId, true);
    } catch (failure: unknown) {
      if (alive.current) setError(publicErrorMessage(failure, 'The EPUB could not be imported.'));
    } finally {
      busy.current = false;
      if (alive.current) setImporting(false);
    }
  }

  async function finishMutation(reveal: number | null, openNote: number | null) {
    const refreshed = await refreshListing(reveal);
    if (!alive.current) return refreshed;
    if (refreshed === 'ignored') {
      setDialog((current) => (current ? { ...current, pending: false } : current));
      return refreshed;
    }
    setDialog(null);
    if (refreshed === 'applied' && openNote !== null) openRef.current(openNote);
    return refreshed;
  }

  function openMenu(sourceId: string, anchor: HTMLElement) {
    if (!canMutateNow()) return;
    const item = treeRef.current.getItemInstance(sourceId);
    item.setFocused();
    // The row already exists. A deferred tree focus would steal focus back
    // after the menu (or its dialog) has focused its first control.
    item.getElement()?.focus();
    const rect = anchor.getBoundingClientRect();
    setMenu({ sourceId, x: rect.left, y: rect.bottom, restore: anchor });
  }

  function closeMenu(restore: boolean) {
    const anchor = menu?.restore ?? null;
    setMenu(null);
    if (restore && anchor?.isConnected) anchor.focus();
  }

  // The focused menu item is removed with the menu. Focus moves to its row
  // first, so the dialog that opens next restores focus there.
  function takeMenuSource(): NoteItem | null {
    const sourceId = menu ? Number(menu.sourceId) : null;
    setMenu(null);
    const source = itemsRef.current.find((entry) => entry.id === sourceId) ?? null;
    if (!source) return null;
    const item = treeRef.current.getItemInstance(String(source.id));
    item.setFocused();
    item.getElement()?.focus();
    return source;
  }

  function notifyAudioChanged(id: number) {
    window.dispatchEvent(new CustomEvent('cha-note-audio-changed', { detail: id }));
  }

  function startRename() {
    const source = takeMenuSource();
    if (!source || !canMutateNow()) return;
    setDialog({
      action: 'rename', sourceId: source.id, name: source.name, error: null, pending: false,
    });
  }

  function updateItem(note: NoteItem) {
    if (!alive.current) return;
    const next = itemsRef.current.map((item) => item.id === note.id ? note : item);
    itemsRef.current = next;
    setItems(next);
    onNoteItemsRef.current(next);
  }

  async function toggleReadOnly() {
    const source = takeMenuSource();
    if (!source || source.kind !== 'note' || !canMutateNow()) return;
    busy.current = true;
    setError(null);
    try {
      updateItem(await onSetNoteReadOnly(source.id, !source.read_only));
    } catch (failure: unknown) {
      if (alive.current) setError(publicErrorMessage(failure, 'The change could not be saved.'));
    } finally {
      busy.current = false;
    }
  }

  async function startDelete() {
    const source = takeMenuSource();
    if (!source || source.kind === 'annotation' || !canMutateNow()) return;
    const captured: DeleteDialogState = {
      action: 'delete',
      sourceId: source.id,
      name: source.name,
      kind: source.kind,
      error: null,
      pending: false,
    };
    busy.current = true;
    const gen = generation.current;
    let confirm = true;
    try {
      confirm = source.kind === 'folder'
        ? itemsRef.current.some((item) => item.parent_id === source.id)
        : await getNoteLength(source.id) >= 16;
    } catch {
      // If the note cannot be read, keep the existing confirmation.
    } finally {
      busy.current = false;
    }
    if (!alive.current || gen !== generation.current) return;
    if (confirm) setDialog(captured);
    else await submitDelete(captured);
  }

  async function startCompress() {
    const source = takeMenuSource();
    if (!source || !canMutateNow()) return;
    if (source.kind === 'folder') {
      await compressFolder(source);
      return;
    }
    const captured: CompressDialogState = {
      action: 'compress', sourceId: source.id, name: source.name,
      phase: 'saving', controller: new AbortController(), pending: true, error: null,
    };
    busy.current = true;
    setDialog(captured);
    try {
      const saved = await onCompressRef.current(source.id, captured.controller.signal, () => {
        if (alive.current) setDialog((current) => current?.action === 'compress'
          ? { ...current, phase: 'compressing' } : current);
      });
      if (!alive.current) return;
      if (saved) setDialog({ ...captured, pending: false });
      const refreshed = await refreshListing(source.id);
      if (!alive.current) return;
      if (refreshed === 'failed' && saved) {
        setDialog((current) => current?.action === 'compress' && current.controller === captured.controller
          ? { ...current, error: `Compression was saved for “${source.name}”, but the notes list could not be refreshed.` }
          : current);
      }
      if (!saved) setDialog(null);
    } catch (failure: unknown) {
      const reason = publicErrorMessage(failure, 'Try again.');
      if (alive.current) setDialog({ ...captured, pending: false,
        error: failure instanceof ChaError && failure.code === 'invalid_argument'
          ? reason : `Could not compress “${source.name}”.\n${reason}` });
    } finally {
      busy.current = false;
    }
  }

  async function compressFolder(source: NoteItem) {
    const notes: NoteTreeRow[] = [];
    const collect = (key: string) => {
      for (const child of index.children[key] ?? []) {
        const row = index.rows[child];
        if (row.kind === 'folder') collect(child);
        else if (row.kind === 'note') notes.push(row);
      }
    };
    collect(String(source.id));
    const controller = new AbortController();
    const captured: FolderCompressDialogState = {
      action: 'compress-folder', sourceId: source.id, name: source.name,
      controller, pending: true, total: notes.length, rows: [], error: null,
    };
    const update = (change: (current: FolderCompressDialogState) => FolderCompressDialogState) => {
      if (alive.current) setDialog((current) => current?.action === 'compress-folder'
        && current.controller === controller ? change(current) : current);
    };
    busy.current = true;
    setDialog(captured);
    try {
      for (const note of notes) {
        if (!alive.current || controller.signal.aborted) break;
        const id = note.noteId!;
        const updateRow = (patch: Partial<FolderCompressionRow>) => update((current) => ({
          ...current, rows: current.rows.map((row) => row.id === id ? { ...row, ...patch } : row),
        }));
        update((current) => ({ ...current, rows: [...current.rows, { id, name: note.name, status: 'Saving…' }] }));
        try {
          const result = await onCompressRef.current(id, controller.signal,
            () => updateRow({ status: 'Compressing…' }), true);
          updateRow({ status: result === 'already_compressed' ? 'Already compressed'
            : result === 'too_small' ? 'Too small' : result ? 'Compressed' : 'Cancelled' });
          if (result === false) {
            controller.abort();
            break;
          }
        } catch (failure: unknown) {
          if (!alive.current) break;
          if (controller.signal.aborted || (failure instanceof ChaError && failure.code === 'operation_cancelled')) {
            updateRow({ status: 'Cancelled' });
            controller.abort();
            break;
          }
          updateRow({ status: 'Failed', error: publicErrorMessage(failure, 'Try again.') });
          // Settings errors would repeat for every remaining note.
          if (failure instanceof ChaError
            && (failure.code === 'vault_changed' || failure.code === 'invalid_argument')) break;
        }
      }
      update((current) => ({ ...current, pending: false }));
      if (!alive.current) return;
      const refreshed = await refreshListing(source.id);
      if (refreshed === 'failed') update((current) => ({
        ...current, error: 'The notes list could not be refreshed.',
      }));
    } finally {
      busy.current = false;
    }
  }

  function showCompressed() {
    const source = takeMenuSource();
    if (!source || source.kind !== 'note' || !source.has_compressed) return;
    setDialog({ action: 'show-compressed', sourceId: source.id, name: source.name });
  }

  async function copyReference() {
    const source = takeMenuSource();
    if (!source || source.kind === 'folder') return;
    setError(null);
    try {
      await navigator.clipboard.writeText(noteReference(source.id));
    } catch {
      if (alive.current) setError('Could not copy the note reference. Try again.');
    }
  }

  async function submitRename() {
    const captured = dialogRef.current;
    if (!captured || captured.action !== 'rename' || captured.pending || busy.current) return;
    if (captured.name.length === 0) return;
    if (!requireFresh(captured.sourceId)) return;
    busy.current = true;
    setDialog({ ...captured, pending: true, error: null });
    try {
      await clientRef.current.renameNoteItem(captured.sourceId, captured.name);
    } catch (failure: unknown) {
      keepDialogError(failure, 'The item could not be renamed.');
      return;
    }
    try {
      if (!alive.current) return;
      const source = itemsRef.current.find((item) => item.id === captured.sourceId);
      await finishMutation(captured.sourceId, null);
      // ANNOTATIONS sorts its groups by note name, so it reloads after the new list.
      if (alive.current && source?.kind === 'note') onSourceChanged?.(source.parent_id);
    } finally {
      if (alive.current) busy.current = false;
    }
  }

  async function submitDelete(captured = dialogRef.current) {
    if (!captured || captured.action !== 'delete' || captured.pending || busy.current) return;
    if (!requireFresh(captured.sourceId)) return;
    const listing = itemsRef.current;
    const reveal = focusAfterRemoval(listing, captured.sourceId);
    busy.current = true;
    if (dialogRef.current) setDialog({ ...captured, pending: true, error: null });
    const refresh = async () => {
      const result = await refreshListing(reveal);
      if (result === 'failed') throw new Error('The notes list could not be refreshed.');
    };
    try {
      const outcome = await onDeleteRef.current(captured.sourceId, listing, refresh);
      if (!alive.current) return;
      if (outcome === 'deleted' || outcome === 'refresh-failed') {
        const source = listing.find((item) => item.id === captured.sourceId);
        if (source?.kind === 'note') onSourceChanged?.(source.parent_id);
        setDialog(null);
        return;
      }
      setDialog({
        ...captured,
        pending: false,
        error: outcome === 'busy'
          ? 'A notes action is already in progress.'
          : 'That item could not be deleted.',
      });
    } catch (failure: unknown) {
      if (!alive.current) return;
      setDialog({
        ...captured,
        pending: false,
        error: publicErrorMessage(failure, 'That item could not be deleted.'),
      });
    } finally {
      if (alive.current) busy.current = false;
    }
  }

  function onTreeKeyDown(event: ReactKeyboardEvent<HTMLDivElement>) {
    if (event.key !== 'ContextMenu' && !(event.key === 'F10' && event.shiftKey)) return;
    const focused = tree.getState().focusedItem;
    if (!focused || focused === 'root') return;
    event.preventDefault();
    const anchor = tree.getItemInstance(focused).getElement() ?? event.currentTarget;
    openMenu(focused, anchor);
  }

  const canMutate = loaded && !stale && !listing && dialog === null && !busy.current;
  const activeNoteAvailable = items.some((item) => item.id === activeNoteId && item.kind !== 'folder');
  const rows = tree.getItems();
  const menuIsNote = menu !== null && index.rows[menu.sourceId]?.kind !== 'folder';
  const menuParentId = menu?.sourceId === 'vault' ? null : Number(menu?.sourceId);
  const containerProps = tree.getContainerProps('Notes');

  // Imports update the open note without reloading the tree.
  useEffect(() => {
    if (!activeNoteAvailable) return;
    onRegisterNoteActions?.({
      updated: updateItem,
      refresh: retryList,
    });
    return () => onRegisterNoteActions?.(null);
  }, [activeNoteAvailable, onRegisterNoteActions]);

  return (
    <div className="cha-notes-browser">
      {error && (
        <p className="cha-error-message" role="alert">
          {error}
          {' '}
          {(!loaded || stale) && (
            <button className="cha-button cha-button-ghost" onClick={retryList} type="button">Retry</button>
          )}
        </p>
      )}
      {!loaded && !error && <p role="status">Loading notes…</p>}
      {importing && <p role="status">Importing EPUB…</p>}
      <input
        accept=".epub"
        aria-label="EPUB file"
        hidden
        onChange={(event) => {
          const file = event.currentTarget.files?.[0];
          event.currentTarget.value = '';
          if (file) void importEpub(file, epubParentId.current);
        }}
        ref={epubInput}
        type="file"
      />
      <div
        {...containerProps}
        className="cha-notes-tree"
        onKeyDown={(event) => {
          containerProps.onKeyDown?.(event);
          onTreeKeyDown(event);
        }}
      >
        {rows.map((item) => {
          const data = item.getItemData();
          const props = item.getProps();
          const current = data.noteId !== null && data.noteId === activeNoteId;
          const level = item.getItemMeta()?.level ?? 0;
          return (
            <div
              key={item.getId()}
              {...props}
              aria-current={current ? 'true' : undefined}
              className={`cha-notes-row${current ? ' is-current' : ''}${props.className ? ` ${props.className}` : ''}`}
              style={{ paddingLeft: `${0.45 + level * 0.85}rem` }}
            >
              <span aria-hidden="true" className="cha-notes-twist">
                {data.kind === 'folder' ? (item.isExpanded() ? '▾' : '▸') : ''}
              </span>
              <span className="cha-notes-label">{data.name}</span>
              {data.kind === 'note' && data.hasCompressed && (
                <span className="cha-notes-compressed" role="img"
                  aria-label="Compressed version saved" title="Compressed version saved" />
              )}
              <button
                aria-expanded={menu?.sourceId === data.id ? true : undefined}
                aria-haspopup="menu"
                aria-label={`Actions for ${data.name}`}
                className="cha-notes-menu-button"
                disabled={!canMutate}
                onClick={(event) => {
                  event.preventDefault();
                  event.stopPropagation();
                  openMenu(data.id, event.currentTarget);
                }}
                onMouseDown={(event) => event.stopPropagation()}
                onPointerDown={(event) => event.stopPropagation()}
                tabIndex={-1}
                type="button"
              >
                <span aria-hidden="true">⋯</span>
              </button>
            </div>
          );
        })}
      </div>
      {menu && createPortal(
        <div
          className="cha-session-menu"
          onKeyDown={(event) => menuKeys(event, () => closeMenu(true))}
          ref={menuRef}
          role="menu"
          style={{
            left: Math.max(8, Math.min(menu.x, window.innerWidth - 168)),
            top: Math.max(8, Math.min(menu.y, window.innerHeight - (menuIsNote ? 220 : menu.sourceId === 'vault' ? 140 : client.noteAudio ? 280 : 250))),
          }}
        >
          {!menuIsNote && (
            <>
              <button onClick={() => beginCreate('note', menuParentId)} role="menuitem" type="button">New note</button>
              <button onClick={() => beginCreate('folder', menuParentId)} role="menuitem" type="button">New folder</button>
            </>
          )}
          {!menuIsNote && (
            <button onClick={() => {
              epubParentId.current = menuParentId;
              closeMenu(true);
              epubInput.current?.click();
            }} role="menuitem" type="button">Import EPUB…</button>
          )}
          {menu.sourceId !== 'vault' && (
            <>
              {index.rows[menu.sourceId]?.kind !== 'annotation' && <button onClick={() => startRename()} role="menuitem" type="button">Rename</button>}
              {menuIsNote && index.rows[menu.sourceId]?.kind === 'note' && (
                <button onClick={() => void toggleReadOnly()} role="menuitem" type="button">
                  {items.find((item) => String(item.id) === menu.sourceId)?.read_only
                    ? 'Make editable' : 'Make read-only'}
                </button>
              )}
              {client.noteAudio && index.rows[menu.sourceId]?.kind !== 'annotation' && <button
                disabled={menuIsNote && !items.find(item => String(item.id) === menu.sourceId)?.read_only}
                onClick={() => {
                  const source = takeMenuSource();
                  if (!source) return;
                  const folderMode = source.kind === 'folder';
                  const targets = folderMode ? (index.children[String(source.id)] ?? []).flatMap(key => {
                    const note = items.find(item => String(item.id) === key);
                    return note?.kind === 'note' && note.read_only ? [{ id: note.id, name: note.name }] : [];
                  }) : [{ id: source.id, name: source.name }];
                  notifyAudioChanged(source.id);
                  setAudioRun({ targets, folderMode });
                }} role="menuitem" type="button">Generate audio</button>}
              {index.rows[menu.sourceId]?.kind !== 'annotation' && <button onClick={() => void startCompress()} role="menuitem" type="button">Compress</button>}
              {menuIsNote && (
                <button disabled={!index.rows[menu.sourceId]?.hasCompressed}
                  onClick={showCompressed} role="menuitem" type="button">Show compressed</button>
              )}
              {index.rows[menu.sourceId]?.kind !== 'annotation' && <button onClick={() => startDelete()} role="menuitem" type="button">Delete</button>}
            </>
          )}
          {menuIsNote && (
            <button onClick={() => void copyReference()} role="menuitem" type="button">Copy Ref ID</button>
          )}
        </div>,
        document.body,
      )}
      {audioRun !== null && client.noteAudio && <NoteAudioDialog client={client.noteAudio}
        targets={audioRun.targets} folderMode={audioRun.folderMode}
        prepare={async id => {
          const note = await client.getNote(id);
          return noteAudioBlocks(getSchema(notesExtensions()).nodeFromJSON({ type: 'doc', ...note.document }))
            .map(block => block.text);
        }}
        onSettled={notifyAudioChanged}
        onClose={() => setAudioRun(null)} />}
      {dialog?.action === 'create' && (
        <NameDialog
          error={dialog.error}
          label="Create"
          name={dialog.name}
          pending={dialog.pending}
          title={dialog.kind === 'note' ? 'New note' : 'New folder'}
          showTitle
          onClose={() => { if (!dialog.pending) setDialog(null); }}
          onName={(name) => setDialog({ ...dialog, name, error: null })}
          onSubmit={() => void submitCreate()}
        />
      )}
      {dialog?.action === 'rename' && (
        <NameDialog
          error={dialog.error}
          label="Rename"
          name={dialog.name}
          pending={dialog.pending}
          title={`Rename “${items.find((item) => item.id === dialog.sourceId)?.name ?? dialog.name}”`}
          onClose={() => { if (!dialog.pending) setDialog(null); }}
          onName={(name) => setDialog({ ...dialog, name, error: null })}
          onSubmit={() => void submitRename()}
        />
      )}
      {dialog?.action === 'delete' && (
        <DeleteDialog
          dialog={dialog}
          onClose={() => { if (!dialog.pending) setDialog(null); }}
          onSubmit={() => void submitDelete()}
        />
      )}
      {(dialog?.action === 'compress' || dialog?.action === 'compress-folder') && (
        <CompressDialog dialog={dialog} onClose={() => setDialog(null)} />
      )}
      {dialog?.action === 'show-compressed' && (
        <CompressedDialog client={client} sourceId={dialog.sourceId} name={dialog.name}
          onClose={() => setDialog(null)} />
      )}
    </div>
  );
}

export function menuKeys(event: ReactKeyboardEvent<HTMLDivElement>, close: () => void) {
  const items = [...event.currentTarget.querySelectorAll<HTMLButtonElement>(
    '[role="menuitem"]:not(:disabled), [role="menuitemcheckbox"]:not(:disabled)',
  )].filter((item) => item.closest('[role="menu"]') === event.currentTarget);
  const current = items.indexOf(document.activeElement as HTMLButtonElement);
  let next = current;
  if (event.key === 'ArrowDown') next = (current + 1) % items.length;
  else if (event.key === 'ArrowUp') next = (current - 1 + items.length) % items.length;
  else if (event.key === 'Home') next = 0;
  else if (event.key === 'End') next = items.length - 1;
  else if (event.key === 'Escape') {
    event.preventDefault();
    close();
    return;
  } else return;
  event.preventDefault();
  items[next]?.focus();
}

function CompressedDialog({ client, sourceId, name, onClose }: {
  client: NotesClient;
  sourceId: number;
  name: string;
  onClose(): void;
}) {
  const button = useRef<HTMLButtonElement | null>(null);
  const element = useDialogElement(() => button.current?.focus());
  const [note, setNote] = useState<NoteContent | null>(null);
  const [error, setError] = useState<string | null>(null);
  const text = note?.compressed_text ?? null;
  const originalText = note
    ? generateText({ type: 'doc', ...note.document }, notesExtensions(), { blockSeparator: '\n' }) : '';
  const ratio = text ? `${([...originalText].length / [...text].length).toFixed(1)}:1` : 'N/A';

  useEffect(() => {
    let cancelled = false;
    void client.getNote(sourceId).then(
      (note) => {
        if (cancelled) return;
        if (note.compressed_text === undefined) setError('No compressed version is stored for this note.');
        else setNote(note);
      },
      (failure: unknown) => {
        if (!cancelled) setError(publicErrorMessage(failure, 'The compressed note could not be loaded.'));
      },
    );
    return () => { cancelled = true; };
  }, [client, sourceId]);

  return createPortal(
    <dialog className="cha-dialog cha-compressed-note-dialog" ref={element}
      aria-label={`Compressed version of “${name}”`}
      onCancel={(event) => { event.preventDefault(); onClose(); }}>
      {text === null && !error && <p role="status">Loading…</p>}
      {text !== null && <pre aria-label="Compressed note text" tabIndex={0}>{text}</pre>}
      {error && <p className="cha-error-message" role="alert">{error}</p>}
      <div className="cha-dialog-actions">
        {text !== null && <span className="cha-compression-ratio">Compression: {ratio}</span>}
        <button className="cha-button cha-button-ghost" type="button" ref={button} onClick={onClose}>Close</button>
      </div>
    </dialog>, document.body,
  );
}

function CompressDialog({ dialog, onClose }: {
  dialog: CompressDialogState | FolderCompressDialogState;
  onClose(): void;
}) {
  const button = useRef<HTMLButtonElement | null>(null);
  const element = useDialogElement(() => button.current?.focus());
  const [startedAt] = useState(() => performance.now());
  const [elapsedSeconds, setElapsedSeconds] = useState(0);
  const rows = dialog.action === 'compress-folder' ? dialog.rows : null;
  const lastRow = useRef<HTMLLIElement | null>(null);
  useEffect(() => {
    lastRow.current?.scrollIntoView?.({ block: 'nearest' });
  }, [rows?.length]);
  useEffect(() => {
    const update = () => setElapsedSeconds(Math.floor((performance.now() - startedAt) / 1000));
    update();
    if (!dialog.pending) return;
    const timer = window.setInterval(update, 1000);
    return () => window.clearInterval(timer);
  }, [dialog.pending, startedAt]);
  return createPortal(
    <dialog className={`cha-dialog cha-compress-dialog${rows ? ' cha-folder-compress-dialog' : ''}`} ref={element}
      aria-label={`Compress “${dialog.name}”`}
      onCancel={(event) => {
        event.preventDefault();
        if (!dialog.pending) onClose();
      }}
      onKeyDown={(event) => { if (event.key === 'Escape' && dialog.pending) event.preventDefault(); }}>
      {dialog.action === 'compress-folder' ? (
        <>
          <ul className="cha-folder-compress-list" aria-label="Note compression progress" aria-live="polite">
            {dialog.rows.map((row, position) => {
              const active = row.status === 'Saving…' || row.status === 'Compressing…';
              return <li key={row.id} ref={position === dialog.rows.length - 1 ? lastRow : undefined}>
                <span className={active ? 'cha-compress-spinner' : undefined} aria-hidden="true">
                  {active ? null : row.status === 'Compressed' ? '✓' : row.status === 'Failed' ? '!'
                    : row.status === 'Cancelled' ? '×' : '–'}
                </span>
                <span>{row.name}</span><span>{row.status}</span>
                {row.error && <span className="cha-folder-compress-error">{row.error}</span>}
              </li>;
            })}
          </ul>
          {dialog.error && <p role="alert">{dialog.error}</p>}
        </>
      ) : dialog.error ? (
        <p role="alert">{dialog.error}</p>
      ) : (
        <p className="cha-compress-status" role="status">
          {dialog.pending && <span className="cha-compress-spinner" aria-hidden="true" />}
          <span>{dialog.pending
            ? `${dialog.phase === 'saving' ? 'Saving' : 'Compressing'} “${dialog.name}”…`
            : `Compressed “${dialog.name}”.`}</span>
        </p>
      )}
      <div className={rows ? 'cha-folder-compress-footer' : undefined}>
        {dialog.action === 'compress-folder' && <span role="status">
          {!dialog.pending && dialog.controller.signal.aborted ? 'Cancelled · ' : ''}
          {dialog.rows.filter((row) => ['Compressed', 'Already compressed', 'Too small', 'Failed'].includes(row.status)).length}
          {' / '}{dialog.total} processed
        </span>}
        <p role="timer" aria-label="Elapsed time">
          {String(Math.floor(elapsedSeconds / 60)).padStart(2, '0')}:{String(elapsedSeconds % 60).padStart(2, '0')}
        </p>
        <div className="cha-dialog-actions">
          <button className="cha-button" type="button" ref={button}
            onClick={() => { if (dialog.pending) dialog.controller.abort(); else onClose(); }}>
            {dialog.pending ? 'Cancel' : 'Close'}
          </button>
        </div>
      </div>
    </dialog>, document.body,
  );
}

function useDialogElement(focus: () => void) {
  const element = useRef<HTMLDialogElement | null>(null);
  const previousFocus = useRef<HTMLElement | null>(null);
  const focusRef = useRef(focus);
  focusRef.current = focus;
  useEffect(() => {
    previousFocus.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const node = element.current;
    if (!node) return;
    if (typeof node.showModal === 'function') node.showModal();
    else node.setAttribute('open', '');
    focusRef.current();
    return () => {
      if (typeof node.close === 'function') node.close();
      if (previousFocus.current?.isConnected) previousFocus.current.focus();
    };
  }, []);
  return element;
}

function NameDialog({
  error,
  label,
  name,
  pending,
  title,
  showTitle = false,
  onClose,
  onName,
  onSubmit,
}: {
  error: string | null;
  label: string;
  name: string;
  pending: boolean;
  title: string;
  showTitle?: boolean;
  onClose(): void;
  onName(name: string): void;
  onSubmit(): void;
}) {
  const nameInput = useRef<HTMLInputElement | null>(null);
  const element = useDialogElement(() => {
    nameInput.current?.focus();
    nameInput.current?.select();
  });

  return createPortal(
    <dialog
      aria-label={title}
      className="cha-dialog"
      onCancel={(event) => {
        event.preventDefault();
        if (!pending) onClose();
      }}
      ref={element}
    >
      {showTitle && <h2>{title}</h2>}
      <form className="cha-notes-create" onSubmit={(event) => {
        event.preventDefault();
        onSubmit();
      }}>
        <label>
          Name
          <input
            className="cha-form-control"
            disabled={pending}
            onChange={(event) => onName(event.target.value)}
            ref={nameInput}
            value={name}
          />
        </label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={pending}
            onClick={onClose}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={pending || name.length === 0}
            type="submit"
          >
            {label}
          </button>
        </div>
      </form>
    </dialog>,
    document.body,
  );
}

function DeleteDialog({
  dialog,
  onClose,
  onSubmit,
}: {
  dialog: DeleteDialogState;
  onClose(): void;
  onSubmit(): void;
}) {
  const cancelButton = useRef<HTMLButtonElement | null>(null);
  const element = useDialogElement(() => {
    cancelButton.current?.focus();
  });
  const message = dialog.kind === 'folder'
    ? `Delete “${dialog.name}” and all notes and folders inside it? This cannot be undone in CHA.`
    : `Delete “${dialog.name}”? This cannot be undone in CHA.`;

  return createPortal(
    <dialog
      aria-label={message}
      className="cha-dialog"
      onCancel={(event) => {
        event.preventDefault();
        if (!dialog.pending) onClose();
      }}
      ref={element}
    >
      <form className="cha-notes-create" onSubmit={(event) => {
        event.preventDefault();
        onSubmit();
      }}>
        <p>{message}</p>
        {dialog.error && <p className="cha-error-message" role="alert">{dialog.error}</p>}
        <div className="cha-dialog-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={dialog.pending}
            onClick={onClose}
            ref={cancelButton}
            type="button"
          >
            Cancel
          </button>
          <button className="cha-button cha-button-primary" disabled={dialog.pending} type="submit">
            {dialog.kind === 'folder' ? 'Delete folder' : 'Delete note'}
          </button>
        </div>
      </form>
    </dialog>,
    document.body,
  );
}
