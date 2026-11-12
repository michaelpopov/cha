import type { NoteImportSource } from '../notes/importSource';
import { hotkeysCoreFeature, syncDataLoaderFeature } from '@headless-tree/core';
import { useTree } from '@headless-tree/react';
import { getSchema } from '@tiptap/core';
import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type CSSProperties,
  type KeyboardEvent as ReactKeyboardEvent,
} from 'react';
import { createPortal } from 'react-dom';

import {
  ChaError,
  publicErrorMessage,
  type FolderStoreOperation,
  type NoteItem,
  type NotesClient,
  type NoteTextMode,
} from '../api/client';
import { downloadEpub, noteFolderFilename } from '../download';
import { buildFolderEpub, EpubExportError } from '../notes/epubExport';
import {
  availableNoteName,
  buildNoteIndex,
  focusAfterRemoval,
  noteInSubtree,
  type NoteTreeRow,
} from '../notes/tree';
import { useDialogElement } from './useDialogElement';
import { CheckIcon } from './Icons';
import { FolderStoreDialog } from './FolderStoreDialog';
import { NoteAudioDialog, type NoteAudioTarget } from './NoteAudioDialog';
import { newNoteImportSource } from '../notes/importSource';
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

type DialogState = CreateDialogState | RenameDialogState | DeleteDialogState;

type MenuState = {
  sourceId: string;
  x: number;
  y: number;
  restore: HTMLElement | null;
  audioPending: boolean;
  identity: number;
  epoch: number;
  compressionPending: boolean | null;
  storeStatus: FolderStoreOperation | null;
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

export type CompressNote = (
  targets: readonly Pick<NoteItem, 'id' | 'name'>[], epoch: number, folder: boolean, refresh: () => Promise<void>,
) => Promise<void>;
export type SummarizeNotes = (source: NoteItem, epoch: number, refresh: () => Promise<void>) => Promise<void>;

type RefreshResult = 'applied' | 'ignored' | 'failed';

const folderTransferStatus = {
  export: 'Exporting folder…',
  import: 'Importing folder…',
  epub: 'Exporting EPUB…',
};

export function NotesTree({
  client,
  getNoteLength,
  browsing,
  activeNoteId,
  noteTextMode,
  onOpenNote,
  onBeforeCreateNote,
  onDeleteItem,
  onCompressNote,
  onSummarize,
  compressionInProgress,
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
  noteTextMode: NoteTextMode;
  onOpenNote(id: number, importSource?: NoteImportSource): void;
  onBeforeCreateNote(): Promise<boolean>;
  onDeleteItem: DeleteNoteItem;
  onCompressNote: CompressNote;
  onSummarize: SummarizeNotes;
  compressionInProgress?(): boolean;
  onSetNoteReadOnly: SetNoteReadOnly;
  onNoteItems(items: NoteItem[]): void;
  vaultName: string;
  onRegisterNoteActions?(actions: NoteActions | null): void;
}) {
  const [storeRun, setStoreRun] = useState<{ source: NoteItem; epoch: number } | null>(null);
  const storeRunRef = useRef(storeRun);
  storeRunRef.current = storeRun;
  const [audioRun, setAudioRun] = useState<{ targets: NoteAudioTarget[]; epoch: number; folderMode: boolean } | null>(null);
  const [items, setItems] = useState<NoteItem[]>([]);
  const [loaded, setLoaded] = useState(false);
  const [stale, setStale] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [listError, setListError] = useState<string | null>(null);
  const [listing, setListing] = useState(false);
  const [importing, setImporting] = useState(false);
  const [folderTransfer, setFolderTransfer] = useState<keyof typeof folderTransferStatus | null>(null);
  const [dialog, setDialog] = useState<DialogState | null>(null);
  const [menu, setMenu] = useState<MenuState | null>(null);
  const generation = useRef(0);
  const alive = useRef(true);
  const busy = useRef(false);
  const pendingRefresh = useRef(false);
  const menuIdentity = useRef(0);
  const statusRead = useRef(0);
  const currentMenu = useRef<MenuState | null>(null);
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
  const itemsRef = useRef(items);
  const staleRef = useRef(stale);
  const listingRef = useRef(listing);
  const dialogRef = useRef(dialog);
  const audioRunRef = useRef(audioRun);
  clientRef.current = client;
  openRef.current = onOpenNote;
  beforeCreateRef.current = onBeforeCreateNote;
  onNoteItemsRef.current = onNoteItems;
  onDeleteRef.current = onDeleteItem;
  itemsRef.current = items;
  staleRef.current = stale;
  listingRef.current = listing;
  dialogRef.current = dialog;
  audioRunRef.current = audioRun;
  currentMenu.current = menu;

  const index = buildNoteIndex(items);
  index.rows.vault = {
    id: 'vault', noteId: null, name: vaultName, kind: 'folder', parentId: null, parentKey: 'root',
  };
  index.children.vault = index.children.root;
  index.children.root = ['vault'];
  for (const key of index.children.vault) index.rows[key].parentKey = 'vault';
  dataRef.current = index;
  const signature = vaultName + '\n' + items.map((item) => (
    `${item.id}:${item.parent_id ?? ''}:${item.kind}:${item.name}:${item.has_compressed}:${item.has_audio}:${item.read_only}:${item.is_summary}:${item.summary_linked}:${JSON.stringify(item.forum_id)}:${item.has_l1}:${item.has_l2}:${item.summary_excluded}:${item.folder_summary}`
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
      pendingRefresh.current = false;
    };
  }, []);

  // The menu height depends on its commands, so measure it to keep every command in the window.
  useLayoutEffect(() => {
    const element = menuRef.current;
    if (!menu || !element) return;
    const height = element.getBoundingClientRect().height;
    element.style.top = `${Math.max(8, Math.min(menu.y, window.innerHeight - height - 8))}px`;
  }, [menu]);

  useLayoutEffect(() => {
    if (!menu) return;
    if (menu.sourceId !== 'vault' && dataRef.current.rows[menu.sourceId]?.kind === 'folder' && dataRef.current.rows[menu.sourceId]?.forumId === undefined && menu.storeStatus === null)
      menuRef.current?.focus();
    else menuRef.current?.querySelector<HTMLButtonElement>('button:not(:disabled)')?.focus();
    const dismiss = (event: PointerEvent) => {
      const target = event.target;
      if (target instanceof Node && menuRef.current?.contains(target)) return;
      setMenu(null);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => window.removeEventListener('pointerdown', dismiss);
  }, [menu?.sourceId, menu?.x, menu?.y, menu?.storeStatus === null]);

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

  async function runList(gen: number, background = false) {
    listingRef.current = true;
    setListing(true);
    try {
      const next = await clientRef.current.listNoteItems();
      if (!alive.current || gen !== generation.current) return;
      const focused = treeRef.current.getState().focusedItem;
      if (focused && focused !== 'vault' && !next.some(item => String(item.id) === focused)) {
        const survivors = new Set(next.map(item => item.id));
        const target = focusAfterRemoval(itemsRef.current.filter(item =>
          String(item.id) === focused || survivors.has(item.id)), Number(focused));
        revealId.current = { id: target !== null && survivors.has(target) ? target : 'vault', expand: false };
      }
      loadedRef.current = true;
      setItems(next);
      setLoaded(true);
      setStale(false);
      setListError(null);
      if (!background) setError(null);
      onNoteItemsRef.current(next);
    } catch (failure: unknown) {
      if (!alive.current || gen !== generation.current) return;
      setListError(publicErrorMessage(failure, 'Could not load notes.'));
      if (loadedRef.current) setStale(true);
    } finally {
      if (alive.current && gen === generation.current) {
        listingRef.current = false;
        setListing(false);
        flushBackgroundRefresh();
      }
    }
  }

  useEffect(() => {
    if (browsing) requestBackgroundRefresh();
  }, [browsing]);

  function retryList() {
    const gen = ++generation.current;
    void runList(gen);
  }

  function requestBackgroundRefresh() {
    pendingRefresh.current = true;
    flushBackgroundRefresh();
  }

  function flushBackgroundRefresh() {
    if (!alive.current || !pendingRefresh.current || busy.current || compressionInProgress?.()
      || listingRef.current || dialogRef.current !== null || audioRunRef.current !== null
      || currentMenu.current !== null || storeRunRef.current !== null) return;
    pendingRefresh.current = false;
    void runList(++generation.current, true);
  }

  async function readCompressionStatus(opened: MenuState) {
    const read = ++statusRead.current;
    setMenu(current => current?.identity === opened.identity ? { ...current, compressionPending: null } : current);
    try {
      const epoch = opened.epoch;
      const status = await clientRef.current.compressionStatus(Number(opened.sourceId), epoch);
      if (!alive.current || read !== statusRead.current || currentMenu.current?.identity !== opened.identity
        || clientRef.current.contextEpoch() !== epoch) return;
      setMenu(current => current?.identity === opened.identity ? { ...current, compressionPending: status.pending } : current);
    } catch (failure) {
      if (!alive.current || read !== statusRead.current || currentMenu.current?.identity !== opened.identity) return;
      setMenu(current => current?.identity === opened.identity ? { ...current, compressionPending: null } : current);
      setError(publicErrorMessage(failure, 'Could not read compression status. Reopen the note menu to try again.'));
    }
  }

  useEffect(() => {
    const changed = (event: Event) => {
      const notice = (event as CustomEvent<{ id: number; epoch: number }>).detail;
      try { if (clientRef.current.contextEpoch() !== notice.epoch) return; }
      catch { return; }
      const opened = currentMenu.current;
      if (opened && noteInSubtree(itemsRef.current, Number(opened.sourceId), notice.id))
        void readCompressionStatus(opened);
      pendingRefresh.current = true;
      flushBackgroundRefresh();
    };
    window.addEventListener('cha-note-compression-changed', changed);
    window.addEventListener('cha-note-audio-changed', requestBackgroundRefresh);
    return () => {
      pendingRefresh.current = false;
      window.removeEventListener('cha-note-compression-changed', changed);
      window.removeEventListener('cha-note-audio-changed', requestBackgroundRefresh);
    };
  }, [client]);

  useEffect(() => { flushBackgroundRefresh(); });

  useLayoutEffect(() => {
    const reveal = revealId.current;
    if (reveal === null || dialog !== null || storeRun !== null) return;
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
      && !compressionInProgress?.()
      && dialogRef.current === null && storeRunRef.current === null;
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
      const target = reveal !== null && !next.some(item => item.id === reveal)
        ? focusAfterRemoval(itemsRef.current, reveal) : reveal;
      revealId.current = { id: target ?? 'vault', expand };
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
      const importSource = captured.kind === 'note' ? newNoteImportSource(captured.name) : undefined;
      const name = importSource ? availableNoteName(itemsRef.current, captured.parentId, 'New note') : captured.name;
      const created = await clientRef.current.createNoteItem(captured.parentId, captured.kind, name);
      if (!alive.current) return;
      const refreshed = await finishMutation(created.id, captured.kind === 'note' ? created.id : null, importSource);
      // A successful creation opens the new note. Refresh the old view only if it stays open.
      if (alive.current && captured.kind === 'note' && refreshed !== 'applied') onSourceChanged?.(captured.parentId);
    } catch (failure: unknown) {
      keepDialogError(failure, 'The item could not be created.');
    } finally {
      busy.current = false;
      flushBackgroundRefresh();
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
      flushBackgroundRefresh();
      if (alive.current) setImporting(false);
    }
  }

  async function exportFolder() {
    const source = takeMenuSource();
    const transfer = clientRef.current.noteFolderTransfer;
    if (!source || !transfer || !canMutateNow()) return;
    busy.current = true;
    setFolderTransfer('export');
    setError(null);
    try {
      await transfer.exportFolder(noteFolderFilename(source.name, 'sqlite'), source.id);
    } catch (failure: unknown) {
      if (alive.current) setError(publicErrorMessage(failure, 'The folder could not be exported.'));
    } finally {
      busy.current = false;
      flushBackgroundRefresh();
      if (alive.current) setFolderTransfer(null);
    }
  }

  async function exportEpub() {
    const epoch = menu?.epoch;
    const source = takeMenuSource();
    if (source?.kind !== 'folder' || epoch === undefined || !canMutateNow()) return;
    const exportClient = clientRef.current;

    function requireContext() {
      if (!alive.current) throw new Error('Notes tree unmounted');
      if (clientRef.current !== exportClient || exportClient.contextEpoch() !== epoch) {
        throw new ChaError('vault_changed', 'The vault changed. Start again in the current vault.');
      }
    }

    async function readCurrent<T>(read: () => Promise<T>): Promise<T> {
      requireContext();
      try { return await read(); }
      finally { requireContext(); }
    }

    busy.current = true;
    setFolderTransfer('epub');
    setError(null);
    try {
      requireContext();
      if (!await beforeCreateRef.current()) return;
      const listing = await readCurrent(() => exportClient.listNoteItems());
      const book = await buildFolderEpub(source.id, listing, noteTextMode, (note, mode) =>
        readCurrent(async () => (await exportClient.getNote(note.id, mode)).document));
      requireContext();
      downloadEpub(book.title, book.bytes);
    } catch (failure: unknown) {
      if (alive.current) {
        const expected = failure instanceof EpubExportError
          || failure instanceof ChaError && (failure.code === 'vault_changed' || failure.code === 'application_unavailable');
        setError(expected ? failure.message : 'The EPUB could not be exported.');
      }
    } finally {
      busy.current = false;
      flushBackgroundRefresh();
      if (alive.current) setFolderTransfer(null);
    }
  }

  async function importFolder(parentId: number | null) {
    closeMenu(true);
    const transfer = clientRef.current.noteFolderTransfer;
    if (!transfer || !canMutateNow()) return;
    busy.current = true;
    setFolderTransfer('import');
    setError(null);
    try {
      await transfer.importFolder(parentId);
      if (!alive.current) return;
      await refreshListing(parentId, true);
    } catch (failure: unknown) {
      if (alive.current) setError(publicErrorMessage(failure, 'The folder could not be imported.'));
    } finally {
      busy.current = false;
      flushBackgroundRefresh();
      if (alive.current) setFolderTransfer(null);
    }
  }

  async function finishMutation(reveal: number | null, openNote: number | null, importSource?: NoteImportSource) {
    const refreshed = await refreshListing(reveal);
    if (!alive.current) return refreshed;
    if (refreshed === 'ignored') {
      setDialog((current) => (current ? { ...current, pending: false } : current));
      return refreshed;
    }
    setDialog(null);
    if (refreshed === 'applied' && openNote !== null) openRef.current(openNote, importSource);
    return refreshed;
  }

  function storeContextCurrent(epoch: number) {
    try { return alive.current && clientRef.current.contextEpoch() === epoch; }
    catch { return false; }
  }

  async function openMenu(sourceId: string, anchor: HTMLElement) {
    if (!canMutateNow()) return;
    const item = treeRef.current.getItemInstance(sourceId);
    item.setFocused();
    // The row already exists. A deferred tree focus would steal focus back
    // after the menu (or its dialog) has focused its first control.
    item.getElement()?.focus();
    const rect = anchor.getBoundingClientRect();
    const note = itemsRef.current.find(entry => String(entry.id) === sourceId);
    const audio = clientRef.current.noteAudio;
    const checkAudio = audio && note?.kind === 'note' && note.read_only;
    let epoch: number;
    try { epoch = clientRef.current.contextEpoch(); }
    catch (failure) { setError(publicErrorMessage(failure, 'The connection to CHA ended.')); return; }
    const opened: MenuState = { sourceId, x: rect.left, y: rect.bottom, restore: anchor,
      audioPending: !!checkAudio, identity: ++menuIdentity.current, epoch, compressionPending: null, storeStatus: null };
    currentMenu.current = opened;
    setMenu(opened);
    if (note?.forum_id !== undefined) return;
    if (note && note.kind !== 'annotation') void readCompressionStatus(opened);
    if (note?.kind === 'folder') {
      void clientRef.current.storeOperation(note.id, epoch).then(status => {
        if (storeContextCurrent(epoch))
          setMenu(current => current?.identity === opened.identity ? { ...current, storeStatus: status } : current);
      }).catch(failure => {
        if (storeContextCurrent(epoch) && currentMenu.current?.identity === opened.identity)
          setError(publicErrorMessage(failure, 'Could not read store status. Reopen the folder menu to try again.'));
      });
    }
    if (!checkAudio) return;
    let audioPending = false;
    try {
      audioPending = (await audio.backgroundStatus(note.id)).pending;
    } catch {
      // Starting generation still checks the native pending status.
    }
    if (alive.current) {
      setMenu(current => current?.identity === opened.identity ? { ...current, audioPending } : current);
    }
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
      flushBackgroundRefresh();
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

  async function cancelCompression() {
    const epoch = menu?.epoch;
    const source = takeMenuSource();
    if (!source || epoch === undefined) return;
    try {
      if (clientRef.current.contextEpoch() !== epoch) throw new ChaError('vault_changed', 'The vault changed. Start again in the current vault.');
      await clientRef.current.cancelCompression(source.id, epoch);
      pendingRefresh.current = true;
      flushBackgroundRefresh();
    } catch (failure) {
      if (alive.current) setError(publicErrorMessage(failure, 'Could not cancel compression. Reopen the note menu to try again.'));
    }
  }

  async function refreshAfterOperation() {
    if (!alive.current) return;
    pendingRefresh.current = false;
    await runList(++generation.current, true);
  }

  async function startCompress() {
    const epoch = menu?.epoch;
    const source = takeMenuSource();
    if (!source || epoch === undefined || !canMutateNow()) return;
    const folder = source.kind === 'folder';
    const targets: Pick<NoteItem, 'id' | 'name'>[] = [];
    const collect = (key: string) => {
      for (const child of index.children[key] ?? []) {
        const row = index.rows[child];
        if (row.kind === 'folder') collect(child);
        else if (row.kind === 'note') targets.push({ id: row.noteId!, name: row.name });
      }
    };
    if (folder) collect(String(source.id));
    else targets.push(source);
    if (targets.length === 0) return;
    setError(null);
    try {
      await onCompressNote(targets, epoch, folder, refreshAfterOperation);
    } catch (failure) {
      if (alive.current) setError(publicErrorMessage(failure, 'Could not start compression. Try again.'));
    } finally {
      flushBackgroundRefresh();
    }
  }

  async function startSummarize() {
    const epoch = menu?.epoch;
    const source = takeMenuSource();
    if (!source || epoch === undefined || !canMutateNow()) return;
    setError(null);
    try {
      await onSummarize(source, epoch, refreshAfterOperation);
    } catch (failure) {
      if (alive.current) setError(publicErrorMessage(failure, 'Could not start summaries. Try again.'));
    } finally {
      flushBackgroundRefresh();
    }
  }

  async function toggleSummaryExcluded() {
    const epoch = menu?.epoch;
    const source = takeMenuSource();
    if (!source || epoch === undefined || !canMutateNow()) return;
    busy.current = true;
    setError(null);
    try {
      if (clientRef.current.contextEpoch() !== epoch) throw new ChaError('vault_changed', 'The vault changed. Start again in the current vault.');
      await clientRef.current.setNoteSummaryExcluded(source.id, !source.summary_excluded);
      if (!storeContextCurrent(epoch)) return;
      await refreshAfterOperation();
    } catch (failure) {
      if (alive.current) setError(publicErrorMessage(failure, 'The change could not be saved.'));
    } finally {
      busy.current = false;
      flushBackgroundRefresh();
    }
  }

  async function copyReference() {
    const source = takeMenuSource();
    if (!source) return;
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
      if (alive.current) { busy.current = false; flushBackgroundRefresh(); }
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
      if (alive.current) { busy.current = false; flushBackgroundRefresh(); }
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

  const canMutate = loaded && !stale && !listing && dialog === null && storeRun === null
    && !busy.current && !compressionInProgress?.();
  const rows = tree.getItems();
  const menuRow = menu ? index.rows[menu.sourceId] : undefined;
  const menuIsNote = menu !== null && menuRow?.kind !== 'folder';
  const menuManaged = menuRow?.forumId !== undefined;
  const menuFolderBusy = menu !== null && !menuIsNote && menu.sourceId !== 'vault'
    && (menu.storeStatus === null || menu.storeStatus.present && !menu.storeStatus.terminal);
  const menuParentId = menu?.sourceId === 'vault' ? null : Number(menu?.sourceId);
  const containerProps = tree.getContainerProps('Notes');

  // Imports update the open note without reloading the tree.
  useEffect(() => {
    onRegisterNoteActions?.({
      updated: updateItem,
      refresh: requestBackgroundRefresh,
    });
    return () => onRegisterNoteActions?.(null);
  }, [onRegisterNoteActions]);

  return (
    <div className="cha-notes-browser">
      {(error ?? listError) && (
        <p className="cha-error-message" role="alert">
          {error ?? listError}
          {' '}
          {(!loaded || stale) && (
            <button className="cha-button cha-button-ghost" onClick={retryList} type="button">Retry</button>
          )}
        </p>
      )}
      {!loaded && !error && !listError && <p role="status">Loading notes…</p>}
      {importing && <p role="status">Importing EPUB…</p>}
      {folderTransfer && <p role="status">{folderTransferStatus[folderTransfer]}</p>}
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
              className={`cha-notes-row${current ? ' is-current' : ''}${data.audioReady ? ' has-audio' : ''}${props.className ? ` ${props.className}` : ''}`}
              style={{ paddingLeft: `${0.45 + level * 0.85}rem` }}
            >
              <span aria-hidden="true" className="cha-notes-twist">
                {data.kind === 'folder' ? (item.isExpanded() ? '▾' : '▸') : ''}
              </span>
              <span className="cha-notes-label">{data.name}</span>
              {data.kind === 'note' && data.hasVersions && (
                <span className="cha-notes-compressed" role="img"
                  aria-label="Other versions saved" title="Other versions saved" />
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
          tabIndex={-1}
          role="menu"
          style={{
            '--menu-x': `${menu.x}px`,
            top: menu.y,
          } as CSSProperties}
        >
          {!menuManaged && !menuIsNote && (
            <>
              <button disabled={menuFolderBusy} onClick={() => beginCreate('note', menuParentId)} role="menuitem" type="button">New note</button>
              <button disabled={menuFolderBusy} onClick={() => beginCreate('folder', menuParentId)} role="menuitem" type="button">New folder</button>
            </>
          )}
          {!menuManaged && !menuIsNote && (
            <button disabled={menuFolderBusy} onClick={() => {
              epubParentId.current = menuParentId;
              closeMenu(true);
              epubInput.current?.click();
            }} role="menuitem" type="button">Import EPUB…</button>
          )}
          {client.noteFolderTransfer && !menuManaged && !menuIsNote && (
            <button disabled={menuFolderBusy} onClick={() => void importFolder(menuParentId)}
              role="menuitem" type="button">Import folder…</button>
          )}
          {client.noteFolderTransfer && !menuIsNote && menu.sourceId !== 'vault' && (
            <button disabled={menuFolderBusy && !menuManaged} onClick={() => void exportFolder()}
              role="menuitem" type="button">Export folder…</button>
          )}
          {menuRow?.kind === 'folder' && menu.sourceId !== 'vault' && (
            <button disabled={menuFolderBusy && !menuManaged} onClick={() => void exportEpub()}
              role="menuitem" type="button">Export EPUB…</button>
          )}
          {!menuManaged && menu.sourceId !== 'vault' && (
            <>
              {menuRow?.kind !== 'annotation' && !menuRow?.summaryLinked && !menuRow?.folderSummary && <button disabled={menuFolderBusy} onClick={() => startRename()} role="menuitem" type="button">Rename</button>}
              {menuIsNote && index.rows[menu.sourceId]?.kind === 'note' && (
                <button onClick={() => void toggleReadOnly()} role="menuitem" type="button">
                  {items.find((item) => String(item.id) === menu.sourceId)?.read_only
                    ? 'Make editable' : 'Make read-only'}
                </button>
              )}
              {client.noteAudio && index.rows[menu.sourceId]?.kind !== 'annotation' && <button
                disabled={menuFolderBusy || menu.audioPending || (menuIsNote && !items.find(item => String(item.id) === menu.sourceId)?.read_only)}
                onClick={() => {
                  const source = takeMenuSource();
                  if (!source) return;
                  const folderMode = source.kind === 'folder';
                  const targets = folderMode ? (index.children[String(source.id)] ?? []).flatMap(key => {
                    const note = items.find(item => String(item.id) === key);
                    return note?.kind === 'note' && note.read_only ? [{ id: note.id, name: note.name }] : [];
                  }) : [{ id: source.id, name: source.name }];
                  notifyAudioChanged(source.id);
                  try { setAudioRun({ targets, folderMode, epoch: client.noteAudio!.contextEpoch() }); }
                  catch (failure) { setError(publicErrorMessage(failure, 'The connection to CHA ended.')); }
                }} role="menuitem" type="button">Generate audio</button>}
              {index.rows[menu.sourceId]?.kind !== 'annotation' && !index.rows[menu.sourceId]?.isSummary && <button
                disabled={menuFolderBusy || menu.compressionPending === null || (!menuIsNote && menu.compressionPending)}
                onClick={() => void (menuIsNote && menu.compressionPending ? cancelCompression() : startCompress())}
                role="menuitem" type="button">{menuIsNote && menu.compressionPending ? 'Cancel compression' : 'Compress'}</button>}
              {menuRow && menuRow.kind !== 'annotation' && !menuRow.isSummary && (
                <button disabled={menuFolderBusy} onClick={() => void startSummarize()} role="menuitem" type="button">Summarize</button>
              )}
              {menuRow?.kind === 'note' && !menuRow.isSummary && (
                <button aria-checked={menuRow.summaryExcluded ?? false} onClick={() => void toggleSummaryExcluded()}
                  role="menuitemcheckbox" type="button">
                  <span>Exclude from summaries</span>
                  {menuRow.summaryExcluded && <CheckIcon />}
                </button>
              )}
              {index.rows[menu.sourceId]?.kind !== 'annotation' && <button onClick={() => startDelete()} role="menuitem" type="button">Delete</button>}
            </>
          )}
          {menuIsNote && (
            <button onClick={() => void copyReference()} role="menuitem" type="button">Copy Ref ID</button>
          )}
          {!menuManaged && !menuIsNote && menu.sourceId !== 'vault' && <button role="menuitem" type="button"
            disabled={menu.storeStatus === null}
            onClick={() => {
              const source = takeMenuSource();
              if (source) setStoreRun({ source, epoch: menu.epoch });
            }}>Upload to store</button>}
        </div>,
        document.body,
      )}
      {storeRun && <FolderStoreDialog client={client} source={storeRun.source}
        epoch={storeRun.epoch} onClose={() => setStoreRun(null)} />}
      {audioRun !== null && client.noteAudio && <NoteAudioDialog client={client.noteAudio}
        targets={audioRun.targets} epoch={audioRun.epoch} folderMode={audioRun.folderMode}
        prepare={async id => {
          const note = await client.getNote(id);
          return noteAudioBlocks(getSchema(notesExtensions()).nodeFromJSON({ type: 'doc', ...note.document }))
            .map(block => block.text);
        }}
        onClose={() => setAudioRun(null)} />}
      {dialog?.action === 'create' && (
        <NameDialog
          error={dialog.error}
          label="Create"
          fieldLabel={dialog.kind === 'note' ? 'Name, URL, or file path' : 'Name'}
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
    </div>
  );
}

export function menuKeys(event: ReactKeyboardEvent<HTMLDivElement>, close: () => void) {
  const items = [...event.currentTarget.querySelectorAll<HTMLButtonElement>(
    ':is([role="menuitem"], [role="menuitemcheckbox"], [role="menuitemradio"]):not(:disabled)',
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

function NameDialog({
  error,
  label,
  name,
  pending,
  title,
  showTitle = false,
  fieldLabel = 'Name',
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
  fieldLabel?: string;
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
          {fieldLabel}
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
