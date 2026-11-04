import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import {
  ChaError, publicErrorMessage, type FolderStoreChoice, type FolderStoreOperation,
  type NoteItem, type NotesClient,
} from '../api/client';

type Operation = Extract<FolderStoreOperation, { present: true }>;
const warning = 'Local removal completed. Some remote data could not be confirmed deleted. See Events for manual-cleanup details.';

export function FolderStoreDialog({ client, source, epoch, onClose }: {
  client: NotesClient; source: NoteItem; epoch: number; onClose(): void;
}) {
  const element = useRef<HTMLDialogElement>(null);
  const alive = useRef(false);
  const busy = useRef(false);
  const operation = useRef<Operation | null>(null);
  const cancelling = useRef(false);
  const [op, setOp] = useState<Operation | null>(null);
  const [choices, setChoices] = useState<FolderStoreChoice[]>([]);
  const [key, setKey] = useState('');
  const choice = choices.find(item => item.api_key_id === key);
  const [description, setDescription] = useState('');
  const descriptionError = choice?.service === 'openai' && [...description].length > 512
    ? 'Description must be 512 characters or fewer for OpenAI.' : null;
  const [loading, setLoading] = useState(true);
  const [pending, setPending] = useState(false);
  const [blocked, setBlocked] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [statusError, setStatusError] = useState<string | null>(null);

  function current() {
    if (!alive.current) return false;
    try {
      if (client.contextEpoch() !== epoch) { onClose(); return false; }
    } catch { onClose(); return false; }
    return true;
  }
  function publish(next: Operation | null) {
    // A status read started before Cancel may return after cancellation was accepted.
    if (next && !next.terminal && next.operation_id === operation.current?.operation_id
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
    setKey(unique.length === 1 ? unique[0].api_key_id : '');
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
  async function follow(first: Operation) {
    let next = first;
    while (current()) {
      publish(next);
      if (next.terminal) return;
      await new Promise(resolve => setTimeout(resolve, 500));
      if (!current()) return;
      try {
        const result = await read(first.operation_id);
        if (!result) return;
        next = result;
      } catch (failure) {
        if (!current()) return;
        if (failure instanceof ChaError && failure.code === 'invalid_argument') throw failure;
        setStatusError(publicErrorMessage(failure, 'Could not read store status. Checking again…'));
      }
    }
  }
  async function finish() {
    if (!current()) return;
    const next = operation.current;
    if (!next?.terminal) return;
    if (next.result === 'preflight') {
      await acknowledge(next);
      if (!current()) return;
      publish(null);
      setError(next.error || 'The saved notes could not be uploaded.');
      await loadChoices();
    } else if (next.result === 'success' || next.result === 'cancelled') {
      await acknowledge(next);
      if (current()) onClose();
    }
  }
  async function recover(result: Operation) {
    publish(result);
    setPending(false);
    setLoading(false);
    await follow(result);
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
        else await loadChoices();
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

  async function submit(action: 'upload' | 'cleanup' = 'upload') {
    if (busy.current || blocked || loading || (action === 'upload' && (!choice || descriptionError)) || !current()) return;
    busy.current = true; setPending(true); setError(null);
    const failed = operation.current;
    try {
      if (action === 'cleanup') {
        if (!failed?.terminal || !failed.known_cleanup_remains) return;
        const previous = await read(failed.operation_id);
        if (!previous) return;
        if (!previous.terminal || !previous.known_cleanup_remains)
          throw new ChaError('invalid_argument', 'The store operation changed. Close this dialog and reopen the folder menu.');
      }
      const accepted = action === 'cleanup' ? await client.storeCleanup(failed!.operation_id, epoch)
        : await client.storeUpload(source.id, choice!.service, key, description, epoch);
      if (!current()) return;
      const result = await read(accepted.operation_id);
      if (!result) return;
      setPending(false);
      await follow(result);
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
          await loadChoices();
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
    if (cancelling.current || pending || next.phase === 'cleaning' || !current()) return;
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
  const canRetryCleanup = !!op?.terminal && op.known_cleanup_remains;
  const editable = !loading && !blocked && !op;
  const canCancel = active && op.phase !== 'cleaning';
  const status = op?.phase === 'uploading' ? `Uploading ${op.completed} of ${op.total} notes…`
    : op?.phase === 'indexing' ? 'Indexing notes…'
      : op?.phase === 'preparing' ? 'Preparing notes…' : 'Cleaning up…';
  return createPortal(<dialog ref={element} className="cha-dialog cha-folder-store-dialog" aria-label="Upload folder to store"
    onCancel={event => { event.preventDefault(); if (canCancel) void cancel(); else if (!active) void close(); }}>
    <form className="cha-notes-create" onSubmit={event => { event.preventDefault(); if (editable) void submit(); }}>
      {loading && !op && <p role="status">Checking store…</p>}
      {active && <p role="status">{status}</p>}
      {editable && <>
        <label>API key<select className="cha-form-control" value={key} disabled={pending} onChange={event => setKey(event.target.value)}>
          <option value="">Select API key</option>
          {choices.map(item => <option key={item.api_key_id} value={item.api_key_id}>{item.api_key_name}</option>)}
        </select></label>
        <label>Description (optional)<textarea className="cha-form-control" rows={6} value={description} disabled={pending}
          aria-invalid={!!descriptionError} aria-describedby={descriptionError ? 'store-description-error' : undefined}
          onChange={event => setDescription(event.target.value)} /></label>
        {descriptionError && <p id="store-description-error" className="cha-error-message" role="alert">{descriptionError}</p>}
        {choices.length === 0 && <p className="cha-error-message" role="alert">Set cloud store API key</p>}
      </>}
      {op?.terminal && op.result === 'warning' && <p role="alert">{warning}</p>}
      {op?.terminal && op.error && <p className="cha-error-message" role="alert">{op.error}</p>}
      {statusError && <p className="cha-error-message" role="alert">{statusError}</p>}
      {error && <p className="cha-error-message" role="alert">{error}</p>}
      <div className="cha-dialog-actions">
        {canCancel && <button className="cha-button" type="button" disabled={pending} onClick={() => void cancel()}>Cancel</button>}
        {!active && <button className="cha-button cha-button-ghost" type="button" disabled={pending || loading} onClick={() => void close()}>
          {editable ? 'Cancel' : 'Close'}</button>}
        {editable && <button className="cha-button cha-button-primary" type="submit" disabled={pending || !choice || !!descriptionError}>
          Upload</button>}
        {canRetryCleanup && !blocked && <button className="cha-button cha-button-primary" type="button" disabled={pending || loading}
          onClick={() => void submit('cleanup')}>Retry cleanup</button>}
      </div>
    </form>
  </dialog>, document.body);
}
