import { useEffect, useMemo, useRef, useState } from 'react';

import type { NoteItem } from '../api/client';
import { compareNoteSiblings, noteItemPath } from '../notes/tree';
import { ChaWebError, chaWebMessage, type ChaWebClient, type ChaWebNote } from './client';
import type { ChawebNotesRoute, NotesFolder } from './route';

export function noteFolders(items: readonly NoteItem[]): { id: NotesFolder; name: string }[] {
  const parents = new Set(items.filter((item) => item.kind !== 'folder').map((item) => item.parent_id));
  const folders = items.filter((item) => item.kind === 'folder' && parents.has(item.id))
    .map((item) => ({ id: item.id, name: noteItemPath(items, item.id) }))
    .sort((a, b) => a.name.localeCompare(b.name) || (a.name < b.name ? -1 : a.name > b.name ? 1 : 0)
      || a.id - b.id);
  return parents.has(null) ? [{ id: 'root', name: 'Top level' }, ...folders] : folders;
}

export function useChawebNotes(client: ChaWebClient, route: ChawebNotesRoute | null,
  recoverContext: () => Promise<void>) {
  const [listing, setListing] = useState<{ items: NoteItem[]; route: ChawebNotesRoute } | null>(null);
  const items = listing?.items ?? null;
  const [detail, setDetail] = useState<{
    route: ChawebNotesRoute; note?: ChaWebNote; error?: string; missing?: boolean;
  } | null>(null);
  const [listLoading, setListLoading] = useState(false);
  const [listError, setListError] = useState<string | null>(null);
  const [retry, setRetry] = useState(0);
  const [remembered, setRemembered] = useState<NotesFolder | undefined>();
  const sequence = useRef(0);
  const controller = useRef<AbortController | null>(null);
  const itemsRef = useRef(items);
  const recoveryRef = useRef(recoverContext);
  recoveryRef.current = recoverContext;
  itemsRef.current = items;

  function cancel() {
    ++sequence.current;
    controller.current?.abort();
    controller.current = null;
  }

  useEffect(() => {
    cancel();
    setDetail(null);
    setListLoading(route?.kind === 'notes');
    if (!route) return cancel;
    const request = new AbortController();
    controller.current = request;
    const seq = sequence.current;
    const current = () => seq === sequence.current && !request.signal.aborted;

    async function readItems() {
      const result = await client.listNoteItems(request.signal);
      if (current()) {
        itemsRef.current = result;
        if (route) setListing({ items: result, route });
        setListError(null);
      }
    }

    const load = async () => {
      try {
        if (route?.kind === 'notes') {
          setListError(null);
          await readItems();
        } else if (route?.kind === 'note') {
          const result = await client.getNote(route.id, request.signal);
          if (!current()) return;
          if (result.item.kind === 'annotation' && itemsRef.current === null) await readItems();
          if (!current()) return;
          setDetail({ route, note: result });
        }
      } catch (error) {
        if (!current()) return;
        if (error instanceof ChaWebError && error.status === 409) {
          itemsRef.current = null;
          setListing(null);
          setRemembered(undefined);
          void recoveryRef.current().catch(() => undefined);
        }
        if (route?.kind === 'notes') {
          setListError(chaWebMessage(error, 'Notes could not be loaded.'));
        } else {
          const unavailable = error instanceof ChaWebError && error.status === 404;
          if (route) setDetail({ route, missing: unavailable,
            error: unavailable ? 'This note is no longer available.'
              : chaWebMessage(error, 'The note could not be loaded.') });
        }
      } finally {
        if (current()) {
          setListLoading(false);
        }
      }
    };
    void load();
    return cancel;
  }, [client, route, retry]);

  const folders = useMemo(() => noteFolders(items ?? []), [items]);
  const folder = route?.kind === 'notes' && route.folder !== undefined ? route.folder
    : folders.find((option) => option.id === remembered)?.id ?? folders[0]?.id;
  const rows = useMemo(() => (items ?? []).filter((item) => item.kind !== 'folder'
    && item.parent_id === (folder === 'root' ? null : folder))
    .sort((a, b) => compareNoteSiblings(a, b) || a.id - b.id), [items, folder]);
  useEffect(() => {
    if (route?.kind === 'notes' && folders.some((option) => option.id === folder)) setRemembered(folder);
  }, [route, folders, folder]);

  return {
    items, folders, folder, rows, remembered, listLoading, listError,
    listReady: listing?.route === route && !listLoading,
    noteError: detail?.route === route ? detail?.error ?? null : null,
    missing: detail?.route === route && detail?.missing === true,
    // Never display the previous document while the route effect catches up.
    note: detail?.route === route ? detail?.note ?? null : null,
    retry: () => setRetry((count) => count + 1),
  };
}
