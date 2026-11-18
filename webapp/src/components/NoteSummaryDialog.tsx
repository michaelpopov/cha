import { useEffect, useRef, useState, type FormEvent } from 'react';
import { createPortal } from 'react-dom';

import {
  publicErrorMessage, type ChaClient, type NoteItem, type NotesClient, type ProviderSummary, type SummaryLevel,
  type SummaryStartRequest,
} from '../api/client';
import { useDialogElement } from './useDialogElement';

const outputs = [
  ['compressed', 'Compressed'], ['l1', 'L1'], ['l2', 'L2'], ['l3', 'L3'],
] as const;

export function NoteSummaryDialog({ client, source, epoch, onStart, onClose }: {
  client: ChaClient & NotesClient;
  source: NoteItem;
  epoch: number;
  onStart(options: SummaryStartRequest): Promise<void>;
  onClose(): void;
}) {
  const folder = source.kind === 'folder';
  const available = folder ? outputs : outputs.slice(0, 3);
  const [levels, setLevels] = useState<SummaryLevel[]>(() => available.map(([level]) => level));
  const [overwrite, setOverwrite] = useState(!folder);
  const [providers, setProviders] = useState<ProviderSummary[] | null>(null);
  const [provider, setProvider] = useState('');
  const [effort, setEffort] = useState<SummaryStartRequest['reasoning_effort']>('none');
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const submitting = useRef(false);
  const alive = useRef(false);
  const cancel = useRef<HTMLButtonElement>(null);
  const dialog = useDialogElement(() => cancel.current?.focus());

  useEffect(() => {
    let disposed = false;
    alive.current = true;
    void (async () => {
      try {
        if (client.contextEpoch() !== epoch) { onClose(); return; }
        const [settings, configured] = await Promise.all([client.getNotesSettings(), client.listProviders()]);
        if (disposed) return;
        if (client.contextEpoch() !== epoch) { onClose(); return; }
        setProviders(configured);
        setProvider(configured.some(entry => entry.id === settings.compression_provider) ? settings.compression_provider : '');
        setEffort(settings.compression_reasoning_effort);
      } catch (failure) {
        if (!disposed) setError(publicErrorMessage(failure, 'Notes settings could not be loaded.'));
      }
    })();
    return () => { disposed = true; alive.current = false; };
  }, [client, epoch, onClose]);

  async function start(event: FormEvent) {
    event.preventDefault();
    if (submitting.current || !providers || !provider || levels.length === 0) return;
    submitting.current = true;
    setPending(true);
    setError(null);
    try {
      await onStart({ id: source.id, provider_id: provider, reasoning_effort: effort, levels, overwrite });
      if (alive.current) onClose();
    } catch (failure) {
      if (alive.current) {
        setError(publicErrorMessage(failure, 'Could not start summarization. Try again.'));
        setPending(false);
        submitting.current = false;
      }
    }
  }

  return createPortal(
    <dialog aria-label="Summarize" className="cha-dialog cha-note-summary-dialog" ref={dialog}
      onCancel={event => { event.preventDefault(); if (!submitting.current) onClose(); }}>
      <form onSubmit={event => void start(event)}>
        <label>Provider<select className="cha-form-control" disabled={!providers || pending}
          value={provider} onChange={event => setProvider(event.target.value)}>
          <option value="">Select provider</option>
          {providers?.map(entry => <option key={entry.id} value={entry.id}>{entry.display_name}</option>)}
        </select></label>
        <label>Effort<select className="cha-form-control" disabled={!providers || pending}
          value={effort} onChange={event => setEffort(event.target.value as typeof effort)}>
          <option value="none">None</option><option value="low">Low</option><option value="medium">Medium</option>
          <option value="high">High</option><option value="xhigh">Extra high</option>
        </select></label>
        <div className="cha-note-summary-outputs">
          {available.map(([level, label]) => <label key={level}>
            <input type="checkbox" checked={levels.includes(level)} disabled={pending}
              onChange={event => setLevels(event.target.checked ? [...levels, level] : levels.filter(value => value !== level))} />
            {label}
          </label>)}
        </div>
        <label className="cha-note-summary-overwrite">
          <input type="checkbox" checked={overwrite} disabled={pending}
            onChange={event => setOverwrite(event.target.checked)} />
          Overwrite
        </label>
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions">
          <button className="cha-button" ref={cancel} disabled={pending} onClick={onClose} type="button">Cancel</button>
          <button className="cha-button cha-button-primary" type="submit"
            disabled={!providers || !provider || levels.length === 0 || pending}>Start</button>
        </div>
      </form>
    </dialog>, document.body,
  );
}
