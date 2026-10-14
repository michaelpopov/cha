import { hotkeysCoreFeature, syncDataLoaderFeature } from '@headless-tree/core';
import { useTree } from '@headless-tree/react';
import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type KeyboardEvent as ReactKeyboardEvent,
} from 'react';
import { createPortal } from 'react-dom';

import {
  publicErrorMessage,
  type NoteItem,
  type NotesClient,
} from '../api/client';
import {
  buildNoteIndex,
  focusAfterRemoval,
  type NoteTreeRow,
} from '../notes/tree';

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
};

export type NoteDeleteResult = 'deleted' | 'refresh-failed' | 'busy' | 'failed';

export type DeleteNoteItem = (
  sourceId: number,
  listing: readonly NoteItem[],
  refresh: () => Promise<void>,
) => Promise<NoteDeleteResult>;

export type NoteActions = {
  rename(): void;
  delete(): void;
};

type RefreshResult = 'applied' | 'ignored' | 'failed';

export function NotesTree({
  client,
  browsing,
  activeNoteId,
  onOpenNote,
  onBeforeCreateNote,
  onDeleteItem,
  onNoteItems,
  vaultName,
  onRegisterNoteActions,
}: {
  client: NotesClient;
  browsing: boolean;
  activeNoteId: number | null;
  onOpenNote(id: number): void;
  onBeforeCreateNote(): Promise<boolean>;
  onDeleteItem: DeleteNoteItem;
  onNoteItems(items: NoteItem[]): void;
  vaultName: string;
  onRegisterNoteActions?(actions: NoteActions | null): void;
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
  const revealId = useRef<number | string | null>(null);
  const menuRef = useRef<HTMLDivElement | null>(null);
  const clientRef = useRef(client);
  const openRef = useRef(onOpenNote);
  const activeNoteRef = useRef(activeNoteId);
  const beforeCreateRef = useRef(onBeforeCreateNote);
  const onNoteItemsRef = useRef(onNoteItems);
  const onDeleteRef = useRef(onDeleteItem);
  const itemsRef = useRef(items);
  const staleRef = useRef(stale);
  const listingRef = useRef(listing);
  const dialogRef = useRef(dialog);
  clientRef.current = client;
  openRef.current = onOpenNote;
  activeNoteRef.current = activeNoteId;
  beforeCreateRef.current = onBeforeCreateNote;
  onNoteItemsRef.current = onNoteItems;
  onDeleteRef.current = onDeleteItem;
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
    `${item.id}:${item.parent_id ?? ''}:${item.kind}:${item.name}`
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
      revealId.current = reveal ?? 'vault';
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
    const names = new Set(itemsRef.current
      .filter((item) => item.parent_id === parentId).map((item) => item.name));
    let name = base;
    for (let suffix = 1; names.has(name); suffix++) name = `${base} ${suffix}`;
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
      await finishMutation(created.id, captured.kind === 'note' ? created.id : null);
    } catch (failure: unknown) {
      keepDialogError(failure, 'The item could not be created.');
    } finally {
      busy.current = false;
    }
  }

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

  function openMenu(sourceId: string, anchor: HTMLElement) {
    if (!canMutateNow()) return;
    const item = treeRef.current.getItemInstance(sourceId);
    if (!item.isFolder()) return;
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

  function startRename(source = takeMenuSource()) {
    if (!source || !canMutateNow()) return;
    setDialog({
      action: 'rename', sourceId: source.id, name: source.name, error: null, pending: false,
    });
  }

  function startDelete(source = takeMenuSource()) {
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
      await finishMutation(captured.sourceId, null);
    } finally {
      if (alive.current) busy.current = false;
    }
  }

  async function submitDelete() {
    const captured = dialogRef.current;
    if (!captured || captured.action !== 'delete' || captured.pending || busy.current) return;
    if (!requireFresh(captured.sourceId)) return;
    const listing = itemsRef.current;
    const reveal = focusAfterRemoval(listing, captured.sourceId);
    busy.current = true;
    setDialog({ ...captured, pending: true, error: null });
    const refresh = async () => {
      const result = await refreshListing(reveal);
      if (result === 'failed') throw new Error('The notes list could not be refreshed.');
    };
    try {
      const outcome = await onDeleteRef.current(captured.sourceId, listing, refresh);
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

  function onTreeKeyDown(event: ReactKeyboardEvent<HTMLDivElement>) {
    if (event.key !== 'ContextMenu' && !(event.key === 'F10' && event.shiftKey)) return;
    const focused = tree.getState().focusedItem;
    if (!focused || focused === 'root') return;
    event.preventDefault();
    const anchor = tree.getItemInstance(focused).getElement() ?? event.currentTarget;
    openMenu(focused, anchor);
  }

  const canMutate = loaded && !stale && !listing && dialog === null && !busy.current;
  const activeNoteAvailable = items.some((item) => item.id === activeNoteId && item.kind === 'note');
  const rows = tree.getItems();
  const menuParentId = menu?.sourceId === 'vault' ? null : Number(menu?.sourceId);
  const containerProps = tree.getContainerProps('Notes');

  // The actions find the open note when they run, so the owner's state changes
  // only when they become available or unavailable.
  useEffect(() => {
    if (!canMutate || !activeNoteAvailable) return;
    const source = () => itemsRef.current.find((item) => (
      item.id === activeNoteRef.current && item.kind === 'note'
    ));
    onRegisterNoteActions?.({
      rename: () => {
        const note = source();
        if (note) startRename(note);
      },
      delete: () => {
        const note = source();
        if (note) startDelete(note);
      },
    });
    return () => onRegisterNoteActions?.(null);
  }, [activeNoteAvailable, canMutate, onRegisterNoteActions]);

  return (
    <div className="cha-notes-browser">
      {error && (
        <p className="cha-error-message" role="alert">
          {error}
          {' '}
          <button className="cha-button cha-button-ghost" onClick={retryList} type="button">Retry</button>
        </p>
      )}
      {!loaded && !error && <p role="status">Loading notes…</p>}
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
              {data.kind === 'folder' && (
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
              )}
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
            top: Math.max(8, Math.min(menu.y, window.innerHeight - (menu.sourceId !== 'vault' ? 180 : 100))),
          }}
        >
          <button onClick={() => beginCreate('note', menuParentId)} role="menuitem" type="button">New note</button>
          <button onClick={() => beginCreate('folder', menuParentId)} role="menuitem" type="button">New folder</button>
          {menu.sourceId !== 'vault' && (
            <>
              <button onClick={() => startRename()} role="menuitem" type="button">Rename</button>
              <button onClick={() => startDelete()} role="menuitem" type="button">Delete</button>
            </>
          )}
        </div>,
        document.body,
      )}
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
    </div>
  );
}

export function menuKeys(event: ReactKeyboardEvent<HTMLDivElement>, close: () => void) {
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
