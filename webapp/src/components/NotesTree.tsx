import { hotkeysCoreFeature, syncDataLoaderFeature } from '@headless-tree/core';
import { useTree } from '@headless-tree/react';
import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type FormEvent,
  type KeyboardEvent as ReactKeyboardEvent,
} from 'react';
import { createPortal } from 'react-dom';

import { publicErrorMessage } from '../api/client';
import type { NoteItem, NotesClient } from '../api/nativeClient';
import {
  buildNoteIndex,
  focusAfterRemoval,
  noteFolderOptions,
  type NoteTreeRow,
} from '../notes/tree';

type CreateKind = 'note' | 'folder';

type CreateDialogState = {
  action: 'create';
  kind: CreateKind;
  name: string;
  parentId: number | null;
  holding: boolean;
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

type MoveDialogState = {
  action: 'move';
  sourceId: number;
  name: string;
  kind: 'note' | 'folder';
  parentId: number | null;
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

type DialogState = CreateDialogState | RenameDialogState | MoveDialogState | DeleteDialogState;

type MenuState = {
  sourceId: number;
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

type RefreshResult = 'applied' | 'ignored' | 'failed';

export function NotesTree({
  client,
  browsing,
  activeNoteId,
  onOpenNote,
  onBeforeCreateNote,
  onDeleteItem,
  onNoteItems,
  onRegisterCreateNote,
}: {
  client: NotesClient;
  browsing: boolean;
  activeNoteId: number | null;
  onOpenNote(id: number): void;
  onBeforeCreateNote(): Promise<boolean>;
  onDeleteItem?: DeleteNoteItem;
  onNoteItems(items: NoteItem[]): void;
  onRegisterCreateNote?(create: () => void): void;
}) {
  const [items, setItems] = useState<NoteItem[]>([]);
  const [loaded, setLoaded] = useState(false);
  const [stale, setStale] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [listing, setListing] = useState(false);
  const [dialog, setDialog] = useState<DialogState | null>(null);
  const [menu, setMenu] = useState<MenuState | null>(null);
  const generation = useRef(0);
  const alive = useRef(true);
  const started = useRef(false);
  const busy = useRef(false);
  const loadedRef = useRef(false);
  const dataRef = useRef(buildNoteIndex([]));
  const signatureRef = useRef<string | null>(null);
  const revealId = useRef<number | null>(null);
  const menuRef = useRef<HTMLDivElement | null>(null);
  const clientRef = useRef(client);
  const openRef = useRef(onOpenNote);
  const beforeCreateRef = useRef(onBeforeCreateNote);
  const onNoteItemsRef = useRef(onNoteItems);
  const onDeleteRef = useRef(onDeleteItem);
  const itemsRef = useRef(items);
  const staleRef = useRef(stale);
  const listingRef = useRef(listing);
  const dialogRef = useRef(dialog);
  const beginCreateRef = useRef<(kind: CreateKind) => void>(() => undefined);
  clientRef.current = client;
  openRef.current = onOpenNote;
  beforeCreateRef.current = onBeforeCreateNote;
  onNoteItemsRef.current = onNoteItems;
  onDeleteRef.current = onDeleteItem;
  itemsRef.current = items;
  staleRef.current = stale;
  listingRef.current = listing;
  dialogRef.current = dialog;

  const index = buildNoteIndex(items);
  dataRef.current = index;
  const signature = items.map((item) => (
    `${item.id}:${item.parent_id ?? ''}:${item.kind}:${item.name}`
  )).join('\n');

  const tree = useTree<NoteTreeRow>({
    rootItemId: 'root',
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
      if (data.kind !== 'note' || data.noteId === null) return;
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
    };
  }, []);

  useEffect(() => {
    onRegisterCreateNote?.(() => { beginCreateRef.current('note'); });
    return () => onRegisterCreateNote?.(() => undefined);
  }, [onRegisterCreateNote]);

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
    const id = revealId.current;
    if (id === null) return;
    const key = String(id);
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
    target.setFocused();
    current.updateDomFocus();
    void target.scrollTo().catch(() => undefined);
  });

  // Use the tree state, not getFocusedItem(): that falls back to the first row.
  function destinationParent(): number | null {
    const focusedId = tree.getState().focusedItem;
    const data = focusedId === null ? undefined : dataRef.current.rows[focusedId];
    if (!data) return null;
    if (data.kind === 'folder' && data.noteId !== null) return data.noteId;
    return data.parentId;
  }

  function canMutateNow() {
    return loadedRef.current
      && !staleRef.current
      && !listingRef.current
      && !busy.current
      && dialogRef.current === null;
  }

  async function refreshListing(reveal: number | null): Promise<RefreshResult> {
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
      revealId.current = reveal;
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

  function requireFresh(sourceId: number | null): NoteItem | null {
    if (!loadedRef.current || staleRef.current || listingRef.current || busy.current) {
      setDialog((current) => (current ? {
        ...current,
        error: 'Refresh the notes list and try again.',
      } : current));
      return null;
    }
    if (sourceId === null) return null;
    const source = itemsRef.current.find((item) => item.id === sourceId) ?? null;
    if (!source) {
      setDialog((current) => (current ? {
        ...current,
        error: 'That item is no longer in the list.',
      } : current));
    }
    return source;
  }

  async function beginCreate(kind: CreateKind) {
    if (!canMutateNow()) return;
    setMenu(null);
    const parentId = destinationParent();
    if (kind === 'folder') {
      setDialog({
        action: 'create', kind, name: '', parentId, holding: false, error: null, pending: false,
      });
      return;
    }
    setDialog({
      action: 'create', kind, name: '', parentId, holding: true, error: null, pending: false,
    });
    const accepted = await beforeCreateRef.current();
    if (!alive.current) return;
    if (!accepted) {
      setDialog(null);
      return;
    }
    setDialog((current) => (
      current?.action === 'create' && current.kind === 'note' ? { ...current, holding: false } : current
    ));
  }
  beginCreateRef.current = (kind) => { void beginCreate(kind); };

  async function finishMutation(reveal: number | null, openNote: number | null) {
    const refreshed = await refreshListing(reveal);
    if (!alive.current) return;
    if (refreshed === 'ignored') {
      setDialog((current) => (current ? { ...current, pending: false } : current));
      return;
    }
    setDialog(null);
    if (refreshed === 'applied' && openNote !== null) openRef.current(openNote);
  }

  async function submitCreate() {
    const captured = dialogRef.current;
    if (!captured || captured.action !== 'create' || captured.pending || captured.holding) return;
    if (captured.name.length === 0 || busy.current) return;
    if (!loadedRef.current || staleRef.current || listingRef.current) {
      setDialog({ ...captured, error: 'Refresh the notes list and try again.' });
      return;
    }
    busy.current = true;
    setDialog({ ...captured, pending: true, error: null });
    let created: NoteItem;
    try {
      created = await clientRef.current.createNoteItem(captured.parentId, captured.kind, captured.name);
    } catch (failure: unknown) {
      keepDialogError(failure, 'The item could not be created.');
      return;
    }
    try {
      await finishMutation(created.id, created.kind === 'note' ? created.id : null);
    } finally {
      if (alive.current) busy.current = false;
    }
  }

  function openMenu(sourceId: number, anchor: HTMLElement) {
    if (!canMutateNow()) return;
    const item = treeRef.current.getItemInstance(String(sourceId));
    item.setFocused();
    treeRef.current.updateDomFocus();
    const rect = anchor.getBoundingClientRect();
    setMenu({ sourceId, x: rect.left, y: rect.bottom, restore: anchor });
  }

  function closeMenu(restore: boolean) {
    const anchor = menu?.restore ?? null;
    setMenu(null);
    if (restore && anchor?.isConnected) anchor.focus();
  }

  function startRename() {
    const sourceId = menu?.sourceId ?? null;
    setMenu(null);
    const source = sourceId === null ? null : itemsRef.current.find((item) => item.id === sourceId);
    if (!source || !canMutateNow()) return;
    setDialog({
      action: 'rename', sourceId: source.id, name: source.name, error: null, pending: false,
    });
  }

  function startMove() {
    const sourceId = menu?.sourceId ?? null;
    setMenu(null);
    const source = sourceId === null ? null : itemsRef.current.find((item) => item.id === sourceId);
    if (!source || !canMutateNow()) return;
    setDialog({
      action: 'move',
      sourceId: source.id,
      name: source.name,
      kind: source.kind,
      parentId: source.parent_id,
      error: null,
      pending: false,
    });
  }

  function startDelete() {
    const sourceId = menu?.sourceId ?? null;
    setMenu(null);
    const source = sourceId === null ? null : itemsRef.current.find((item) => item.id === sourceId);
    if (!source || !canMutateNow()) return;
    setDialog({
      action: 'delete',
      sourceId: source.id,
      name: source.name,
      kind: source.kind,
      error: null,
      pending: false,
    });
  }

  async function submitRename() {
    const captured = dialogRef.current;
    if (!captured || captured.action !== 'rename' || captured.pending || busy.current) return;
    if (captured.name.length === 0) return;
    const source = requireFresh(captured.sourceId);
    if (!source) return;
    busy.current = true;
    setDialog({ ...captured, pending: true, error: null });
    try {
      await clientRef.current.renameNoteItem(captured.sourceId, captured.name);
    } catch (failure: unknown) {
      keepDialogError(failure, 'The item could not be renamed.');
      return;
    }
    try {
      await finishMutation(captured.sourceId, null);
    } finally {
      if (alive.current) busy.current = false;
    }
  }

  async function submitMove() {
    const captured = dialogRef.current;
    if (!captured || captured.action !== 'move' || captured.pending || busy.current) return;
    const source = requireFresh(captured.sourceId);
    if (!source) return;
    busy.current = true;
    setDialog({ ...captured, pending: true, error: null });
    try {
      await clientRef.current.moveNoteItem(captured.sourceId, captured.parentId);
    } catch (failure: unknown) {
      keepDialogError(failure, 'The item could not be moved.');
      return;
    }
    try {
      await finishMutation(captured.sourceId, null);
    } finally {
      if (alive.current) busy.current = false;
    }
  }

  async function submitDelete() {
    const captured = dialogRef.current;
    if (!captured || captured.action !== 'delete' || captured.pending || busy.current) return;
    const source = requireFresh(captured.sourceId);
    if (!source) return;
    const listing = itemsRef.current;
    const reveal = focusAfterRemoval(listing, captured.sourceId);
    busy.current = true;
    setDialog({ ...captured, pending: true, error: null });
    const refresh = async () => {
      const result = await refreshListing(reveal);
      if (result === 'failed') throw new Error('The notes list could not be refreshed.');
    };
    try {
      const outcome = onDeleteRef.current
        ? await onDeleteRef.current(captured.sourceId, listing, refresh)
        : await deleteWithoutSlot(captured.sourceId, refresh);
      if (!alive.current) return;
      if (outcome === 'deleted' || outcome === 'refresh-failed') {
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

  async function deleteWithoutSlot(
    sourceId: number,
    refresh: () => Promise<void>,
  ): Promise<NoteDeleteResult> {
    await clientRef.current.deleteNoteItem(sourceId);
    try {
      await refresh();
      return 'deleted';
    } catch {
      return 'refresh-failed';
    }
  }

  function onTreeKeyDown(event: ReactKeyboardEvent<HTMLDivElement>) {
    if (event.key !== 'ContextMenu' && !(event.key === 'F10' && event.shiftKey)) return;
    const focused = tree.getState().focusedItem;
    if (!focused || focused === 'root') return;
    event.preventDefault();
    const anchor = tree.getItemInstance(focused).getElement() ?? event.currentTarget;
    openMenu(Number(focused), anchor);
  }

  const canCreate = loaded && !stale && !listing && dialog === null && !busy.current;
  const rows = loaded ? tree.getItems() : [];
  const containerProps = tree.getContainerProps('Notes');

  return (
    <div className="cha-notes-browser">
      <div className="cha-notes-actions">
        <button
          className="cha-button"
          disabled={!canCreate}
          onClick={() => void beginCreate('note')}
          type="button"
        >
          New note
        </button>
        <button
          className="cha-button"
          disabled={!canCreate}
          onClick={() => void beginCreate('folder')}
          type="button"
        >
          New folder
        </button>
      </div>
      {error && (
        <p className="cha-error-message" role="alert">
          {error}
          {' '}
          <button className="cha-button cha-button-ghost" onClick={retryList} type="button">Retry</button>
        </p>
      )}
      {!loaded && !error && <p role="status">Loading notes…</p>}
      {loaded && (
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
                {data.noteId !== null && (
                  <button
                    aria-expanded={menu?.sourceId === data.noteId ? true : undefined}
                    aria-haspopup="menu"
                    aria-label={`Actions for ${data.name}`}
                    className="cha-notes-menu-button"
                    disabled={!canCreate}
                    onClick={(event) => {
                      event.preventDefault();
                      event.stopPropagation();
                      openMenu(data.noteId as number, event.currentTarget);
                    }}
                    onMouseDown={(event) => event.stopPropagation()}
                    onPointerDown={(event) => event.stopPropagation()}
                    tabIndex={-1}
                    type="button"
                  >
                    <span aria-hidden="true">⋯</span>
                  </button>
                )}
              </div>
            );
          })}
        </div>
      )}
      {menu && createPortal(
        <div
          className="cha-session-menu"
          onKeyDown={(event) => menuKeys(event, () => closeMenu(true))}
          ref={menuRef}
          role="menu"
          style={{
            left: Math.max(8, Math.min(menu.x, window.innerWidth - 168)),
            top: Math.max(8, Math.min(menu.y, window.innerHeight - 140)),
          }}
        >
          <button onClick={startRename} role="menuitem" type="button">Rename</button>
          <button onClick={startMove} role="menuitem" type="button">Move</button>
          <button onClick={startDelete} role="menuitem" type="button">Delete</button>
        </div>,
        document.body,
      )}
      {dialog?.action === 'create' && !dialog.holding && (
        <CreateDialog
          dialog={dialog}
          items={items}
          onChange={setDialog}
          onClose={() => { if (!dialog.pending) setDialog(null); }}
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
      {dialog?.action === 'move' && (
        <MoveDialog
          dialog={dialog}
          items={items}
          onChange={setDialog}
          onClose={() => { if (!dialog.pending) setDialog(null); }}
          onSubmit={() => void submitMove()}
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

function menuKeys(event: ReactKeyboardEvent<HTMLDivElement>, close: () => void) {
  const items = [...event.currentTarget.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')];
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

function CreateDialog({
  dialog,
  items,
  onChange,
  onClose,
  onSubmit,
}: {
  dialog: CreateDialogState;
  items: NoteItem[];
  onChange(next: CreateDialogState): void;
  onClose(): void;
  onSubmit(): void;
}) {
  const element = useRef<HTMLDialogElement | null>(null);
  const nameInput = useRef<HTMLInputElement | null>(null);
  const previousFocus = useRef<HTMLElement | null>(null);
  const title = dialog.kind === 'note' ? 'New note' : 'New folder';
  const options = noteFolderOptions(items);

  useEffect(() => {
    previousFocus.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const node = element.current;
    if (!node) return;
    if (typeof node.showModal === 'function') node.showModal();
    else node.setAttribute('open', '');
    nameInput.current?.focus();
    return () => {
      if (typeof node.close === 'function') node.close();
      if (previousFocus.current?.isConnected) previousFocus.current.focus();
    };
  }, []);

  function submit(event: FormEvent) {
    event.preventDefault();
    onSubmit();
  }

  return createPortal(
    <dialog
      aria-label={title}
      className="cha-dialog"
      onCancel={(event) => {
        event.preventDefault();
        onClose();
      }}
      ref={element}
    >
      <form className="cha-notes-create" onSubmit={submit}>
        <label>
          Name
          <input
            className="cha-form-control"
            disabled={dialog.pending}
            onChange={(event) => onChange({ ...dialog, name: event.target.value, error: null })}
            ref={nameInput}
            value={dialog.name}
          />
        </label>
        <label>
          In
          <select
            className="cha-form-control"
            disabled={dialog.pending}
            onChange={(event) => onChange({
              ...dialog,
              parentId: event.target.value === 'root' ? null : Number(event.target.value),
            })}
            value={dialog.parentId === null ? 'root' : String(dialog.parentId)}
          >
            <option value="root">Top level</option>
            {options.map((option) => (
              <option key={option.id} value={String(option.id)}>{option.path}</option>
            ))}
          </select>
        </label>
        {dialog.error && <p className="cha-error-message" role="alert">{dialog.error}</p>}
        <div className="cha-dialog-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={dialog.pending}
            onClick={onClose}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={dialog.pending || dialog.name.length === 0}
            type="submit"
          >
            Create
          </button>
        </div>
      </form>
    </dialog>,
    document.body,
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
  onClose,
  onName,
  onSubmit,
}: {
  error: string | null;
  label: string;
  name: string;
  pending: boolean;
  title: string;
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

function MoveDialog({
  dialog,
  items,
  onChange,
  onClose,
  onSubmit,
}: {
  dialog: MoveDialogState;
  items: NoteItem[];
  onChange(next: MoveDialogState): void;
  onClose(): void;
  onSubmit(): void;
}) {
  const selectRef = useRef<HTMLSelectElement | null>(null);
  const element = useDialogElement(() => {
    selectRef.current?.focus();
  });
  const options = noteFolderOptions(items, dialog.kind === 'folder' ? dialog.sourceId : null);
  const label = `Move “${dialog.name}” to`;

  return createPortal(
    <dialog
      aria-label={label}
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
        <label>
          {label}
          <select
            className="cha-form-control"
            disabled={dialog.pending}
            onChange={(event) => onChange({
              ...dialog,
              parentId: event.target.value === 'root' ? null : Number(event.target.value),
              error: null,
            })}
            ref={selectRef}
            value={dialog.parentId === null ? 'root' : String(dialog.parentId)}
          >
            <option value="root">Top level</option>
            {options.map((option) => (
              <option key={option.id} value={String(option.id)}>{option.path}</option>
            ))}
          </select>
        </label>
        {dialog.error && <p className="cha-error-message" role="alert">{dialog.error}</p>}
        <div className="cha-dialog-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={dialog.pending}
            onClick={onClose}
            type="button"
          >
            Cancel
          </button>
          <button className="cha-button cha-button-primary" disabled={dialog.pending} type="submit">
            Move
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
