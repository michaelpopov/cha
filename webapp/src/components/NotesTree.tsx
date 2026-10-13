import { hotkeysCoreFeature, syncDataLoaderFeature } from '@headless-tree/core';
import { useTree } from '@headless-tree/react';
import { useEffect, useLayoutEffect, useRef, useState, type FormEvent } from 'react';
import { createPortal } from 'react-dom';

import { publicErrorMessage } from '../api/client';
import type { NoteItem, NotesClient } from '../api/nativeClient';
import { buildNoteIndex, noteFolderOptions, type NoteTreeRow } from '../notes/tree';

type CreateKind = 'note' | 'folder';

type DialogState = {
  kind: CreateKind;
  name: string;
  parentId: number | null;
  holding: boolean;
  error: string | null;
  pending: boolean;
};

export function NotesTree({
  client,
  browsing,
  activeNoteId,
  onOpenNote,
  onBeforeCreateNote,
  onNoteItems,
}: {
  client: NotesClient;
  browsing: boolean;
  activeNoteId: number | null;
  onOpenNote(id: number): void;
  onBeforeCreateNote(): Promise<boolean>;
  onNoteItems(items: NoteItem[]): void;
}) {
  const [items, setItems] = useState<NoteItem[]>([]);
  const [loaded, setLoaded] = useState(false);
  const [stale, setStale] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [listing, setListing] = useState(false);
  const [dialog, setDialog] = useState<DialogState | null>(null);
  const generation = useRef(0);
  const alive = useRef(true);
  const started = useRef(false);
  const busy = useRef(false);
  const loadedRef = useRef(false);
  const dataRef = useRef(buildNoteIndex([]));
  const signatureRef = useRef<string | null>(null);
  const revealId = useRef<number | null>(null);
  const clientRef = useRef(client);
  const openRef = useRef(onOpenNote);
  const beforeCreateRef = useRef(onBeforeCreateNote);
  const onNoteItemsRef = useRef(onNoteItems);
  clientRef.current = client;
  openRef.current = onOpenNote;
  beforeCreateRef.current = onBeforeCreateNote;
  onNoteItemsRef.current = onNoteItems;

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

  async function beginCreate(kind: CreateKind) {
    if (!loadedRef.current || stale || listing || busy.current || dialog) return;
    const parentId = destinationParent();
    if (kind === 'folder') {
      setDialog({ kind, name: '', parentId, holding: false, error: null, pending: false });
      return;
    }
    setDialog({ kind, name: '', parentId, holding: true, error: null, pending: false });
    const accepted = await beforeCreateRef.current();
    if (!alive.current) return;
    if (!accepted) {
      setDialog(null);
      return;
    }
    setDialog((current) => (
      current && current.kind === 'note' ? { ...current, holding: false } : current
    ));
  }

  async function submitCreate() {
    if (!dialog || dialog.pending || dialog.holding || dialog.name.length === 0 || busy.current) return;
    const captured = dialog;
    busy.current = true;
    setDialog({ ...captured, pending: true, error: null });
    let created: NoteItem;
    try {
      created = await clientRef.current.createNoteItem(captured.parentId, captured.kind, captured.name);
    } catch (failure: unknown) {
      busy.current = false;
      if (!alive.current) return;
      setDialog((current) => (current ? {
        ...current,
        pending: false,
        error: publicErrorMessage(failure, 'The item could not be created.'),
      } : current));
      return;
    }
    const gen = ++generation.current;
    try {
      const next = await clientRef.current.listNoteItems();
      if (!alive.current || gen !== generation.current) return;
      loadedRef.current = true;
      setItems(next);
      setLoaded(true);
      setStale(false);
      setError(null);
      onNoteItemsRef.current(next);
      setDialog(null);
      revealId.current = created.id;
      if (created.kind === 'note') openRef.current(created.id);
    } catch (failure: unknown) {
      if (!alive.current || gen !== generation.current) return;
      setStale(true);
      setError(publicErrorMessage(failure, 'Could not load notes.'));
      setDialog(null);
    } finally {
      if (alive.current) busy.current = false;
    }
  }

  const canCreate = loaded && !stale && !listing && dialog === null && !busy.current;
  const rows = loaded ? tree.getItems() : [];

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
        <div {...tree.getContainerProps('Notes')} className="cha-notes-tree">
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
                <span>{data.name}</span>
              </div>
            );
          })}
        </div>
      )}
      {dialog && !dialog.holding && (
        <CreateDialog
          dialog={dialog}
          items={items}
          onChange={setDialog}
          onClose={() => { if (!dialog.pending) setDialog(null); }}
          onSubmit={() => void submitCreate()}
        />
      )}
    </div>
  );
}

function CreateDialog({
  dialog,
  items,
  onChange,
  onClose,
  onSubmit,
}: {
  dialog: DialogState;
  items: NoteItem[];
  onChange(next: DialogState): void;
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
