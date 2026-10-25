import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { ChaError, publicErrorMessage, type NoteAudioClient, type NoteAudioStatus } from '../api/client';

export function noteAudioPhase(status: NoteAudioStatus): string {
  switch (status.phase) {
    case 'uploading': return 'Uploading…';
    case 'queued': return 'Queued';
    case 'generating': return status.completed !== undefined && status.total !== undefined
      ? `Generating · ${status.completed} of ${status.total} complete` : 'Generating';
    case 'downloading': return 'Downloading…';
    case 'saving': return 'Saving…';
    case 'complete': return `Audio ready · ${status.total} clips`;
  }
}

// These replies confirm rejection before any worker was launched; native start
// cannot fail after a launch. Other failures can mean that the start reply was
// lost, so the dialog must read native status.
function rejectedBeforeStart(failure: unknown): boolean {
  return failure instanceof ChaError && ['invalid_argument', 'not_found', 'body_too_large',
    'command_queue_full', 'operation_cancelled', 'internal_error'].includes(failure.code);
}
function contextEnded(failure: unknown): boolean {
  return failure instanceof ChaError && ['vault_changed', 'application_unavailable', 'server_stopping'].includes(failure.code);
}

export function NoteAudioDialog({ client, id, prepare, onClose }: {
  client: NoteAudioClient; id: number; prepare(): Promise<string[]>; onClose(): void;
}) {
  const element = useRef<HTMLDialogElement>(null);
  const voiceSelect = useRef<HTMLSelectElement>(null);
  const [voice, setVoice] = useState('');
  const [existing, setExisting] = useState(false);
  const [loading, setLoading] = useState(true);
  const [status, setStatus] = useState<NoteAudioStatus | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [pending, setPending] = useState(false);
  const [elapsed, setElapsed] = useState(0);
  const start = useRef(0);
  const alive = useRef(true);
  const started = useRef(false);

  useEffect(() => {
    alive.current = true;
    const previous = document.activeElement;
    const node = element.current!;
    if (node.showModal) node.showModal(); else node.setAttribute('open', '');
    void client.info(id).then(info => {
      if (!alive.current) return;
      setExisting(info.count > 0); setLoading(false);
    }, failure => {
      if (alive.current) { setError(publicErrorMessage(failure, 'Could not load note audio.')); setLoading(false); }
    });
    return () => { alive.current = false; node.close?.(); if (previous instanceof HTMLElement && previous.isConnected) previous.focus(); };
  }, [client, id]);

  // The selector is disabled until the stored audio count loads.
  useEffect(() => { if (!loading) voiceSelect.current?.focus(); }, [loading]);

  useEffect(() => {
    if (!pending) return;
    const timer = setInterval(() => setElapsed(Math.max(0, Date.now() - start.current)), 1000);
    return () => clearInterval(timer);
  }, [pending]);

  async function generate() {
    if (started.current || !voice || loading) return;
    started.current = true;
    start.current = Date.now(); setElapsed(0); setPending(true); setError(null);
    function finish(failure: unknown) {
      if (!alive.current) return;
      setElapsed(Math.max(0, Date.now() - start.current));
      setError(publicErrorMessage(failure, 'Could not generate note audio.'));
      setPending(false);
    }
    let blocks: string[];
    try { blocks = await prepare(); }
    catch (failure) { finish(failure); return; }
    if (!alive.current) return;
    let next: NoteAudioStatus | undefined;
    try { next = await client.start(id, voice, blocks); }
    catch (failure) {
      if (rejectedBeforeStart(failure) || contextEnded(failure)) { finish(failure); return; }
      if (alive.current) setError(publicErrorMessage(failure, 'Could not read generation status. Check the connection to CHA.'));
    }
    while (alive.current) {
      if (next && (!next.done || next.finished_at_ms! >= start.current)) {
        setStatus(next); setError(next.error || null);
        if (next.done) {
          setElapsed(Math.max(0, next.finished_at_ms! - start.current));
          setPending(false);
          return;
        }
      }
      // Local state reads share this cadence, including after a lost start reply.
      await new Promise(resolve => setTimeout(resolve, 1000));
      if (!alive.current) return;
      next = undefined;
      try { next = await client.status(id); }
      catch (failure) {
        if (contextEnded(failure)) { finish(failure); return; }
        if (alive.current) setError(publicErrorMessage(failure, 'Could not read generation status. Check the connection to CHA.'));
      }
    }
  }
  const seconds = Math.floor(elapsed / 1000);
  const time = seconds >= 3600
    ? `${Math.floor(seconds / 3600)}:${String(Math.floor(seconds / 60) % 60).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`
    : `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`;
  return createPortal(<dialog ref={element} className="cha-dialog cha-note-audio-dialog" aria-label="Generate note audio"
    onCancel={event => { event.preventDefault(); if (!pending) onClose(); }}
    onKeyDown={event => { if (pending && event.key === 'Escape') event.preventDefault(); }}>
    <label>Voice<select ref={voiceSelect} className="cha-form-control" value={voice} disabled={loading || started.current}
      onChange={event => setVoice(event.target.value)}>
      <option value="">Select voice</option>
      {['boroda', 'grant', 'dzig', 'gerdt', 'Kore'].map(value => <option key={value} value={value}>{value}</option>)}
    </select></label>
    {existing && !started.current && <p>Existing audio will be replaced after successful generation.</p>}
    {status && <p role="status">{noteAudioPhase(status)}</p>}
    {error && <p className="cha-error-message" role="alert">{error}</p>}
    {started.current && <p className="cha-note-audio-time"><span>Elapsed</span><span role="timer" aria-label="Elapsed time">{time}</span></p>}
    <div className="cha-dialog-actions">
      <button className="cha-button" type="button" disabled={pending} onClick={() => { if (!pending) onClose(); }}>Close</button>
      {!started.current && <button className="cha-button cha-button-primary" type="button" disabled={loading || !voice || !!error}
        onClick={() => void generate()}>Generate</button>}
    </div>
  </dialog>, document.body);
}
