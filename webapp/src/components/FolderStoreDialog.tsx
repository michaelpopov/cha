import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import {
  ChaError, publicErrorMessage, type FolderStoreChoice, type FolderStoreOperation,
  type FolderStoreService, type NoteItem, type NotesClient,
} from '../api/client';
import { noteInSubtree } from '../notes/tree';
import type { DeleteNoteItem } from './NotesTree';

type Operation = Extract<FolderStoreOperation, { present: true }>;
type Mode = 'upload' | 'remove' | 'delete';
const warning = 'Local removal completed. Some remote data could not be confirmed deleted. See Events for manual-cleanup details.';
const providerName = (service: FolderStoreService) => service === 'openai' ? 'OpenAI' : 'Google';

export function FolderStoreDialog({ client, source, mode: requestedMode, epoch, listing, refresh, onDeleteItem, onClose }: {
  client: NotesClient; source: NoteItem; mode: Mode; epoch: number; listing: readonly NoteItem[];
  refresh(): Promise<void>; onDeleteItem: DeleteNoteItem; onClose(): void;
}) {
  const element = useRef<HTMLDialogElement>(null);
  const alive = useRef(false);
  const busy = useRef(false);
  const operation = useRef<Operation | null>(null);
  const cancelling = useRef(false);
  const [op, setOp] = useState<Operation | null>(null);
  const [mode, setMode] = useState(requestedMode);
  const [choices, setChoices] = useState<FolderStoreChoice[]>([]);
  const [service, setService] = useState<FolderStoreService>(source.store?.service ?? 'openai');
  const [key, setKey] = useState('');
  const [loading, setLoading] = useState(true);
  const [pending, setPending] = useState(false);
  const [blocked, setBlocked] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [statusError, setStatusError] = useState<string | null>(null);
  const count = listing.filter(item => item.store && noteInSubtree(listing, source.id, item.id)).length;
  const eligible = choices.filter(choice => choice.service === service);

  function current() {
    if (!alive.current) return false;
    try {
      if (client.contextEpoch() !== epoch) { onClose(); return false; }
    } catch { onClose(); return false; }
    return true;
  }
  function publish(next: Operation | null) {
    // A status read started before Cancel may return after cancellation was accepted.
    if (next?.kind === 'upload' && !next.terminal && next.operation_id === operation.current?.operation_id
      && operation.current.phase === 'cleaning') next = { ...next, phase: 'cleaning' };
    operation.current = next;
    setOp(next);
  }
  async function loadChoices() {
    const result = await client.storeChoices(epoch);
    if (!current()) return;
    const unique = result.choices.filter((choice, i, all) => all.findIndex(other =>
      other.service === choice.service && other.api_key_id === choice.api_key_id) === i);
    setChoices(unique);
    const keys = unique.filter(choice => choice.service === service);
    setKey(keys.length === 1 ? keys[0].api_key_id : '');
  }
  async function reload() {
    await refresh();
    return current();
  }
  async function acknowledge(next: Operation) {
    await client.storeClose(next.operation_id, epoch);
  }
  async function read(expected: number): Promise<Operation | null> {
    const result = await client.storeOperation(source.id, epoch);
    if (!current()) return null;
    if (!result.present || result.operation_id !== expected) {
      publish(null);
      setBlocked(true);
      throw new ChaError('invalid_argument', 'The store operation changed. Close this dialog and reopen the folder menu.');
    }
    setStatusError(null);
    publish(result);
    return result;
  }
  async function follow(first: Operation): Promise<boolean> {
    let next = first;
    while (current()) {
      publish(next);
      if (next.terminal) return next.folder_deleted;
      await new Promise(resolve => setTimeout(resolve, 500));
      if (!current()) return false;
      try {
        const result = await read(first.operation_id);
        if (!result) return false;
        next = result;
      } catch (failure) {
        if (!current()) return false;
        if (failure instanceof ChaError && failure.code === 'invalid_argument') throw failure;
        setStatusError(publicErrorMessage(failure, 'Could not read store status. Checking again…'));
      }
    }
    return false;
  }
  async function finish() {
    if (!current()) return;
    const next = operation.current;
    if (!next?.terminal) return;
    // Local deletion is reconciled by App before acknowledging its identity.
    if (next.result === 'preflight') {
      await acknowledge(next);
      if (!current()) return;
      publish(null);
      setError(next.error || 'The saved notes could not be uploaded.');
      await loadChoices();
    } else if (next.result === 'success' || next.result === 'cancelled' || next.result === 'folder_deleted') {
      if (!next.folder_deleted && !await reload()) return;
      await acknowledge(next);
      if (current()) onClose();
    } else if (!next.folder_deleted) {
      await reload();
    }
  }
  async function guardedDelete(run: () => Promise<boolean>) {
    let ran = false;
    const result = await onDeleteItem(source.id, listing, refresh, () => { ran = true; return run(); });
    if (current() && !ran && result !== 'busy') throw new ChaError('invalid_argument', 'The delete attempt could not start. Close and reopen the folder menu.');
    if (current() && result === 'busy') throw new ChaError('invalid_argument', 'A notes action is already in progress.');
    if (current() && result === 'refresh-failed') setError('The folder was deleted, but the notes list could not be refreshed.');
  }
  async function recover(result: Operation) {
    setMode(result.kind === 'folder_delete' ? 'delete' : result.kind);
    publish(result);
    setPending(false);
    setLoading(false);
    if (result.kind === 'folder_delete') await guardedDelete(() => follow(result));
    else await follow(result);
    await finish();
  }

  useEffect(() => {
    alive.current = true;
    let disposed = false;
    const previous = document.activeElement;
    const node = element.current!;
    if (node.showModal) node.showModal(); else node.setAttribute('open', '');
    void (async () => {
      await Promise.resolve();
      if (disposed) return;
      busy.current = true;
      try {
        const result = await client.storeOperation(source.id, epoch);
        if (disposed || !current()) return;
        if (result.present) await recover(result);
        else if (requestedMode === 'upload') await loadChoices();
      } catch (failure) {
        if (current()) { setBlocked(true); setError(publicErrorMessage(failure, 'Could not recover store status. Close and reopen the folder menu.')); }
      } finally {
        busy.current = false;
        if (current()) setLoading(false);
      }
    })();
    return () => {
      disposed = true;
      alive.current = false;
      node.close?.();
      if (previous instanceof HTMLElement && previous.isConnected) previous.focus();
    };
  }, [client, source.id, epoch]);

  useEffect(() => {
    if (!loading) element.current?.querySelector<HTMLElement>('select:not(:disabled), button:not(:disabled)')?.focus();
  }, [loading, op?.terminal, op?.phase]);

  async function submit(deleteAnyway = false) {
    if (busy.current || blocked || loading || (mode === 'upload' && !key) || !current()) return;
    busy.current = true; setPending(true); setError(null);
    const failed = operation.current;
    try {
      const start = async () => {
        const accepted = mode === 'delete'
          ? await client.folderDelete(failed ? { operation_id: failed.operation_id, delete_anyway: deleteAnyway }
            : { folder_id: source.id, confirmed_store_count: count }, epoch)
          : mode === 'upload' ? await client.storeUpload(source.id, service, key, epoch)
            : await client.storeRemove(source.id, source.store!.id, epoch);
        if (!current()) return false;
        const result = await read(accepted.operation_id);
        if (!result) return false;
        setPending(false);
        return follow(result);
      };
      if (mode === 'delete') await guardedDelete(start);
      else await start();
      await finish();
    } catch (failure) {
      if (!current()) return;
      setError(publicErrorMessage(failure, 'Could not complete the store operation.'));
      // A timeout can lose the start reply. Recover by folder; never resubmit it.
      try {
        const result = await client.storeOperation(source.id, epoch);
        if (!current()) return;
        if (result.present) await recover(result);
        else {
          if (operation.current) { publish(null); setBlocked(true); }
          await reload(); // Includes count mismatch: require a new confirmation.
          if (current() && mode === 'upload') await loadChoices();
        }
      } catch {
        if (current()) setBlocked(true);
      }
    } finally {
      busy.current = false;
      if (current()) setPending(false);
    }
  }
  async function close() {
    if (busy.current || !current()) return;
    const next = operation.current;
    if (next && !next.terminal) return;
    busy.current = true; setPending(true);
    try {
      if (next) await acknowledge(next);
      if (current()) onClose();
    } catch (failure) {
      if (current()) setError(publicErrorMessage(failure, 'Could not close the store operation.'));
    } finally { busy.current = false; if (current()) setPending(false); }
  }
  async function cancel() {
    const next = operation.current;
    if (!next || next.terminal) { await close(); return; }
    if (cancelling.current || pending || next.kind !== 'upload' || next.phase === 'cleaning' || !current()) return;
    cancelling.current = true;
    setPending(true);
    try {
      await client.storeCancel(next.operation_id, epoch);
      if (current() && operation.current?.operation_id === next.operation_id && !operation.current.terminal)
        publish({ ...operation.current, phase: 'cleaning' });
    } catch (failure) {
      if (current()) setError(publicErrorMessage(failure, 'Could not request cancellation.'));
    } finally { cancelling.current = false; if (current()) setPending(false); }
  }
  const active = !!op && !op.terminal;
  const failedDelete = mode === 'delete' && !!op?.terminal && !op.folder_deleted;
  const editable = !loading && !blocked && !op;
  const canCancel = active && op.kind === 'upload' && !['cleaning', 'removing'].includes(op.phase);
  const status = op?.phase === 'uploading' ? `Uploading ${op.completed} of ${op.total} notes…`
    : op?.phase === 'indexing' ? 'Indexing notes…'
      : op?.phase === 'cleaning' ? 'Cleaning up…'
        : op?.phase === 'preparing' ? 'Preparing notes…'
          : mode === 'delete' ? 'Removing remote stores…' : 'Removing store…';
  return createPortal(<dialog ref={element} className="cha-dialog cha-folder-store-dialog"
    aria-label={mode === 'upload' ? 'Upload folder to store' : mode === 'remove' ? 'Remove folder from store' : `Delete “${source.name}”`}
    onCancel={event => { event.preventDefault(); if (canCancel) void cancel(); else if (!active) void close(); }}>
    <form className="cha-notes-create" onSubmit={event => { event.preventDefault(); if (editable) void submit(); }}>
      {loading && !op && <p role="status">Checking store…</p>}
      {active && <p role="status">{status}</p>}
      {editable && mode === 'upload' && <>
        <label>Provider<select className="cha-form-control" value={service} disabled={pending} onChange={event => {
          const selected = event.target.value as FolderStoreService;
          setService(selected); setError(null);
          const keys = choices.filter(choice => choice.service === selected);
          setKey(keys.length === 1 ? keys[0].api_key_id : '');
        }}><option value="openai">OpenAI</option><option value="google">Google</option></select></label>
        <label>API key<select className="cha-form-control" value={key} disabled={pending} onChange={event => setKey(event.target.value)}>
          <option value="">Select API key</option>
          {eligible.map(choice => <option key={choice.api_key_id} value={choice.api_key_id}>{choice.api_key_name}</option>)}
        </select></label>
        {eligible.length === 0 && <p className="cha-error-message" role="alert">Add a compatible provider and saved key in Settings.</p>}
      </>}
      {editable && mode === 'remove' && <p>Remove “{source.name}” from {providerName(source.store!.service)}?</p>}
      {editable && mode === 'delete' && <><p>Delete “{source.name}” and all its notes?</p>
        {count > 0 && <p>{count} remote {count === 1 ? 'store will' : 'stores will'} also be removed.</p>}</>}
      {op?.terminal && op.result === 'warning' && <p role="alert">{warning}</p>}
      {op?.terminal && op.error && <p className="cha-error-message" role="alert">{op.error}</p>}
      {op?.terminal && mode === 'upload' && op.known_cleanup_remains && <p>Use Remove from store to retry cleanup.</p>}
      {failedDelete && <p>Delete anyway removes the local folder and notes. Remote data and charges may remain.</p>}
      {statusError && <p className="cha-error-message" role="alert">{statusError}</p>}
      {error && <p className="cha-error-message" role="alert">{error}</p>}
      <div className="cha-dialog-actions">
        {canCancel && <button className="cha-button" type="button" disabled={pending} onClick={() => void cancel()}>Cancel</button>}
        {!active && <button className="cha-button cha-button-ghost" type="button" disabled={pending || loading} onClick={() => void close()}>
          {editable || failedDelete ? 'Cancel' : 'Close'}</button>}
        {editable && <button className="cha-button cha-button-primary" type="submit" disabled={pending || mode === 'upload' && !key}>
          {mode === 'upload' ? 'Upload' : mode === 'remove' ? 'Remove' : 'Delete'}</button>}
        {failedDelete && !blocked && <>
          <button className="cha-button" type="button" disabled={pending} onClick={() => void submit()}>Retry</button>
          <button className="cha-button cha-button-primary" type="button" disabled={pending} onClick={() => void submit(true)}>Delete anyway</button>
        </>}
      </div>
    </form>
  </dialog>, document.body);
}
