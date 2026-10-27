import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { SingleNoteAudioSetup } from './SingleNoteAudioSetup';
import { backgroundAudioStartRejected, ChaError, publicErrorMessage, type NoteAudioClient } from '../api/client';

export type NoteAudioTarget = { id: number; name: string };
type NoteAudioDialogProps = {
  client: NoteAudioClient; targets: NoteAudioTarget[]; epoch: number; folderMode: boolean;
  prepare(id: number): Promise<string[]>; onClose(): void;
};
const vaultChanged = 'The vault changed. Close this dialog and start again.';
function contextEnded(failure: unknown): boolean {
  return failure instanceof ChaError && ['vault_changed', 'application_unavailable', 'server_stopping'].includes(failure.code);
}

export function NoteAudioDialog(props: NoteAudioDialogProps) {
  return props.folderMode ? <FolderNoteAudioDialog {...props} />
    : <SingleNoteAudioSetup client={props.client} id={props.targets[0].id} prepare={props.prepare} onClose={props.onClose} />;
}

function FolderNoteAudioDialog({ client, targets, epoch, prepare, onClose }: Omit<NoteAudioDialogProps, 'folderMode'>) {
  const element = useRef<HTMLDialogElement>(null);
  const selector = useRef<HTMLSelectElement>(null);
  const alive = useRef(true);
  const submitting = useRef(false);
  const accepted = useRef(0);
  const [voice, setVoice] = useState('');
  const [loading, setLoading] = useState(true);
  const [empty, setEmpty] = useState(false);
  const [pending, setPending] = useState(false);
  const [blocked, setBlocked] = useState(false);
  const [error, setError] = useState<string | null>(null);

  function checkContext() {
    if (!alive.current) throw new ChaError('application_unavailable', 'This dialog was closed.');
    if (client.contextEpoch() !== epoch) throw new ChaError('vault_changed', vaultChanged);
  }
  function currentFailure(failure: unknown) {
    try { checkContext(); } catch (changed) { return changed; }
    return failure;
  }
  async function needsAudio(id: number) {
    checkContext();
    const info = await client.info(id);
    checkContext();
    if (info.count > 0) return false;
    const status = await client.backgroundStatus(id);
    checkContext();
    return !status.pending;
  }

  useEffect(() => {
    alive.current = true;
    let disposed = false;
    const previous = document.activeElement;
    const node = element.current!;
    if (node.showModal) node.showModal(); else node.setAttribute('open', '');
    void (async () => {
      // Discard StrictMode's effect replay before sending any reads.
      await Promise.resolve();
      if (disposed) return;
      try {
        let count = 0;
        for (const target of targets) {
          if (await needsAudio(target.id)) ++count;
          if (disposed) return;
        }
        checkContext();
        setEmpty(count === 0);
      } catch (failure) {
        if (disposed) return;
        const cause = currentFailure(failure);
        if (contextEnded(cause)) setBlocked(true);
        setError(publicErrorMessage(cause, 'Could not load note audio.'));
      } finally { if (!disposed) setLoading(false); }
    })();
    return () => {
      disposed = true; alive.current = false; node.close?.();
      if (previous instanceof HTMLElement && previous.isConnected) previous.focus();
    };
  }, [client, targets, epoch]);
  useEffect(() => { if (!loading) selector.current?.focus(); }, [loading]);

  async function generate() {
    if (submitting.current || loading || blocked || empty || !voice) return;
    submitting.current = true;
    setPending(true); setError(null);
    const selectedVoice = voice;
    let target: NoteAudioTarget | undefined;
    let submitted = false;
    try {
      checkContext();
      for (target of targets) {
        submitted = false;
        if (!await needsAudio(target.id)) continue;
        checkContext();
        let blocks: string[];
        try { blocks = await prepare(target.id); }
        catch (failure) {
          checkContext();
          if (failure instanceof ChaError && failure.code === 'not_found') continue;
          throw failure;
        }
        checkContext();
        submitted = true;
        const result = await client.startBackground(target.id, selectedVoice, blocks, true);
        checkContext();
        if ('attempt_id' in result) ++accepted.current;
      }
      checkContext();
      onClose();
    } catch (failure) {
      if (!alive.current) return;
      const cause = currentFailure(failure);
      if (contextEnded(cause)) {
        setBlocked(true);
        setError(publicErrorMessage(cause, vaultChanged));
      } else {
        const continuing = accepted.current ? ' Jobs already accepted continue in the background.' : '';
        if (submitted && !backgroundAudioStartRejected(cause)) {
          setBlocked(true);
          setError(`Could not confirm whether audio generation started for “${target!.name}”.${continuing} Check Settings → Events before starting again.`);
        } else {
          setError(`Could not start audio for “${target!.name}”: ${publicErrorMessage(cause, 'Check the connection and try again.')}${continuing}`);
        }
      }
    } finally {
      submitting.current = false;
      if (alive.current) setPending(false);
    }
  }

  return createPortal(<dialog ref={element} className="cha-dialog cha-note-audio-dialog" aria-label="Generate note audio"
    onCancel={event => { event.preventDefault(); if (!submitting.current) onClose(); }}>
    {!empty && <label>Voice<select ref={selector} className="cha-form-control" value={voice} disabled={loading || pending || blocked}
      onChange={event => setVoice(event.target.value)}>
      <option value="">Select voice</option>
      {['boroda', 'grant', 'dzig', 'gerdt', 'Kore'].map(value => <option key={value} value={value}>{value}</option>)}
    </select></label>}
    {loading && <p role="status">Checking notes…</p>}
    {empty && !error && <p role="status">No notes need audio</p>}
    {error && <p className="cha-error-message" role="alert">{error}</p>}
    <div className="cha-dialog-actions">
      <button className="cha-button" type="button" disabled={pending} onClick={() => { if (!submitting.current) onClose(); }}>Close</button>
      {!empty && <button className="cha-button cha-button-primary" type="button" disabled={loading || pending || blocked || !voice}
        onClick={() => void generate()}>Generate</button>}
    </div>
  </dialog>, document.body);
}
