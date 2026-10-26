import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { SingleNoteAudioSetup } from './SingleNoteAudioSetup';
import { ChaError, publicErrorMessage, type NoteAudioClient, type NoteAudioStatus } from '../api/client';

export type NoteAudioTarget = { id: number; name: string };

export function noteAudioPhase(status: NoteAudioStatus): string {
  switch (status.phase) {
    case 'uploading': return 'Uploading…';
    case 'queued': return 'Queued';
    case 'generating': return status.completed !== undefined && status.total !== undefined
      ? `Generating · ${status.completed} of ${status.total} clips complete` : 'Generating';
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

type NoteAudioDialogProps = {
  client: NoteAudioClient; targets: NoteAudioTarget[]; folderMode: boolean;
  prepare(id: number): Promise<string[]>; onSettled(id: number): void; onClose(): void;
};

export function NoteAudioDialog(props: NoteAudioDialogProps) {
  return props.folderMode ? <FolderNoteAudioDialog {...props} />
    : <SingleNoteAudioSetup client={props.client} id={props.targets[0].id} prepare={props.prepare} onClose={props.onClose} />;
}

function FolderNoteAudioDialog({ client, targets, prepare, onSettled, onClose }: Omit<NoteAudioDialogProps, 'folderMode'>) {
  const element = useRef<HTMLDialogElement>(null);
  const voiceSelect = useRef<HTMLSelectElement>(null);
  const [voice, setVoice] = useState('');
  const [eligible, setEligible] = useState<NoteAudioTarget[]>([]);
  // A finished position retains the current note until the next note starts.
  const [position, setPosition] = useState({ index: 0, finished: false });
  const [skipped, setSkipped] = useState<{ name: string; reason: string }[]>([]);
  const [stopRequested, setStopRequested] = useState(false);
  const stop = useRef(false);
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
    let disposed = false;
    const previous = document.activeElement;
    const node = element.current!;
    if (node.showModal) node.showModal(); else node.setAttribute('open', '');
    void (async () => {
      // Let effect cleanup discard React's development replay before scanning.
      await Promise.resolve();
      if (disposed) return;
      try {
        const selected: NoteAudioTarget[] = [];
        for (const target of targets) {
          const info = await client.info(target.id);
          if (disposed) return;
          if (info.count === 0) selected.push(target);
        }
        if (!disposed) setEligible(selected);
      } catch (failure) {
        if (!disposed) setError(publicErrorMessage(failure, 'Could not load note audio.'));
      } finally {
        if (!disposed) setLoading(false);
      }
    })();
    return () => { disposed = true; alive.current = false; node.close?.(); if (previous instanceof HTMLElement && previous.isConnected) previous.focus(); };
  }, [client, targets]);

  // The selector is disabled until the stored audio counts load.
  useEffect(() => { if (!loading) voiceSelect.current?.focus(); }, [loading]);

  useEffect(() => {
    if (!pending) return;
    const timer = setInterval(() => setElapsed(Math.max(0, Date.now() - start.current)), 1000);
    return () => clearInterval(timer);
  }, [pending]);

  async function generate() {
    if (started.current || !voice || loading || error || !eligible.length) return;
    started.current = true;
    const selectedVoice = voice;
    stop.current = false; setStopRequested(false);
    setPending(true); setError(null); setSkipped([]);

    async function generateNote(id: number, blocks: string[]): Promise<NoteAudioStatus | undefined> {
      let next: NoteAudioStatus | undefined;
      let startFailure: unknown;
      let ended = false;
      try {
        try { next = await client.start(id, selectedVoice, blocks); }
        catch (failure) {
          if (rejectedBeforeStart(failure) || contextEnded(failure)) throw failure;
          startFailure = failure;
          if (alive.current) setError(publicErrorMessage(failure, 'Could not read generation status. Check the connection to CHA.'));
        }
        while (alive.current) {
          if (next && (!next.done || next.finished_at_ms! >= start.current)) {
            startFailure = undefined;
            setStatus(next); setError(next.error || null);
            if (next.done) {
              setElapsed(Math.max(0, next.finished_at_ms! - start.current));
              return next;
            }
            if (stop.current) void client.cancel(id).catch(() => {});
          }
          // Ordinary bridge requests run in order. After a lost start reply,
          // the first successful status read proves whether that start ran.
          if (next?.done && startFailure !== undefined) throw startFailure;
          // Local state reads share this cadence, including after a lost start reply.
          await new Promise(resolve => setTimeout(resolve, 1000));
          if (!alive.current) return;
          next = undefined;
          try { next = await client.status(id); }
          catch (failure) {
            if (contextEnded(failure)) throw failure;
            if (startFailure !== undefined && failure instanceof ChaError && failure.code === 'not_found') throw startFailure;
            if (alive.current) setError(publicErrorMessage(failure, 'Could not read generation status. Check the connection to CHA.'));
          }
        }
      } catch (failure) {
        ended = contextEnded(failure);
        throw failure;
      } finally {
        if (alive.current && !ended) onSettled(id);
      }
    }

    try {
      for (let index = 0; index < eligible.length; index++) {
        if (!alive.current || stop.current) break;
        const target = eligible[index];
        setPosition({ index, finished: false });
        setStatus(null); setError(null);
        start.current = Date.now(); setElapsed(0);
        const blocks = await prepare(target.id);
        if (!alive.current) return;
        if (stop.current) { setElapsed(Math.max(0, Date.now() - start.current)); break; }
        let reason: string | undefined;
        if (!blocks.length) reason = 'No text to narrate';
        else {
          let result: NoteAudioStatus | undefined;
          try { result = await generateNote(target.id, blocks); }
          catch (failure) {
            if (!(failure instanceof ChaError) || failure.code !== 'body_too_large') throw failure;
            reason = publicErrorMessage(failure, 'Could not generate note audio.');
          }
          if (!alive.current) return;
          if (result?.error) break;
        }
        if (reason) {
          setSkipped(current => [...current, { name: target.name, reason }]);
          setError(null);
          setElapsed(Math.max(0, Date.now() - start.current));
        }
        setPosition({ index, finished: true });
      }
    } catch (failure) {
      if (alive.current) {
        setElapsed(Math.max(0, Date.now() - start.current));
        setError(publicErrorMessage(failure, 'Could not generate note audio.'));
      }
    } finally {
      if (alive.current) {
        setPending(false);
      }
    }
  }
  const progress = position.index + Number(position.finished);
  const complete = progress - skipped.length;
  const allFinished = started.current && progress === eligible.length;
  const skipCount = skipped.length ? ` · ${skipped.length} skipped` : '';
  let phase = status ? noteAudioPhase(status) : null;
  if (started.current) {
    if (pending && stopRequested) phase = 'Stopping…';
    else if (!pending && allFinished) phase = complete > 0 ? 'Audio ready' : null;
    else if (!pending && (stopRequested || error || status?.error)) phase = 'Folder run stopped';
  }
  const seconds = Math.floor(elapsed / 1000);
  const time = seconds >= 3600
    ? `${Math.floor(seconds / 3600)}:${String(Math.floor(seconds / 60) % 60).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`
    : `${String(Math.floor(seconds / 60)).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')}`;
  return createPortal(<dialog ref={element} className="cha-dialog cha-note-audio-dialog" aria-label="Generate note audio"
    onCancel={event => { event.preventDefault(); if (!pending) onClose(); }}
    onKeyDown={event => { if (pending && event.key === 'Escape') event.preventDefault(); }}>
    {(loading || eligible.length > 0) && <label>Voice<select ref={voiceSelect} className="cha-form-control" value={voice} disabled={loading || started.current}
      onChange={event => setVoice(event.target.value)}>
      <option value="">Select voice</option>
      {['boroda', 'grant', 'dzig', 'gerdt', 'Kore'].map(value => <option key={value} value={value}>{value}</option>)}
    </select></label>}
    {!started.current && <p role="status">{loading ? 'Checking notes…'
      : eligible.length ? `${eligible.length} notes to generate` : !error ? 'No notes need audio' : null}</p>}
    {started.current && <div className="cha-note-audio-progress">
      <p>{allFinished ? `${complete} of ${eligible.length} notes complete${skipCount}`
        : `Note ${position.index + 1} of ${eligible.length} · ${complete} complete${skipCount}`}</p>
      {!allFinished && <p className="cha-note-audio-name">{eligible[position.index]?.name}</p>}
      <progress aria-label="Folder audio progress" max={eligible.length} value={progress} />
    </div>}
    {phase && <p role="status">{phase}</p>}
    {error && !(stopRequested && !pending) && <p className="cha-error-message" role="alert">{error}</p>}
    {skipped.map((note, index) => <div key={index} className="cha-note-audio-skipped cha-error-message" role="alert">
      <p className="cha-note-audio-name">{note.name}</p><p>{note.reason}</p>
    </div>)}
    {started.current && <p className="cha-note-audio-time"><span>Elapsed</span><span role="timer" aria-label="Elapsed time">{time}</span></p>}
    <div className="cha-dialog-actions">
      <button className="cha-button" type="button" disabled={pending} onClick={() => { if (!pending) onClose(); }}>Close</button>
      {!started.current && (loading || eligible.length > 0) && <button className="cha-button cha-button-primary" type="button" disabled={loading || !voice || !!error}
        onClick={() => void generate()}>Generate</button>}
      {pending && <button className="cha-button cha-button-primary" type="button" disabled={stopRequested}
        onClick={() => { stop.current = true; setStopRequested(true); }}>Stop</button>}
    </div>
  </dialog>, document.body);
}
