import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { backgroundAudioStartRejected, publicErrorMessage, type NoteAudioClient } from '../api/client';

export const audioStartUnknown = 'Could not confirm whether audio generation started. Check Settings → Events before starting again.';

export function SingleNoteAudioSetup({ client, id, prepare, onClose }: {
  client: NoteAudioClient; id: number; prepare(id: number): Promise<string[]>; onClose(): void;
}) {
  const element = useRef<HTMLDialogElement>(null);
  const selector = useRef<HTMLSelectElement>(null);
  const submitting = useRef(false);
  const alive = useRef(true);
  const [voice, setVoice] = useState('');
  const [loading, setLoading] = useState(true);
  const [existing, setExisting] = useState(false);
  const [pending, setPending] = useState(false);
  const [unknown, setUnknown] = useState(false);
  const [error, setError] = useState<string | null>(null);
  useEffect(() => {
    alive.current = true;
    let disposed = false;
    const previous = document.activeElement;
    const node = element.current!;
    if (node.showModal) node.showModal(); else node.setAttribute('open', '');
    void client.info(id).then(info => { if (!disposed) setExisting(info.count > 0); }, failure => {
      if (!disposed) setError(publicErrorMessage(failure, 'Could not load note audio.'));
    }).finally(() => { if (!disposed) setLoading(false); });
    return () => {
      disposed = true; alive.current = false; node.close?.();
      if (previous instanceof HTMLElement && previous.isConnected) previous.focus();
    };
  }, [client, id]);
  useEffect(() => { if (!loading) selector.current?.focus(); }, [loading]);

  async function generate() {
    if (submitting.current || loading || unknown || !voice) return;
    submitting.current = true; setPending(true); setError(null);
    let submitted = false;
    try {
      const blocks = await prepare(id);
      if (!alive.current) return;
      submitted = true;
      await client.startBackground(id, voice, blocks);
      if (alive.current) onClose();
    } catch (failure) {
      if (!alive.current) return;
      if (submitted && !backgroundAudioStartRejected(failure)) {
        setUnknown(true); setError(audioStartUnknown);
      } else {
        submitting.current = false;
        setError(publicErrorMessage(failure, 'Could not generate note audio.'));
      }
    } finally { if (alive.current) setPending(false); }
  }
  return createPortal(<dialog ref={element} className="cha-dialog cha-note-audio-dialog" aria-label="Generate note audio"
    onCancel={event => { event.preventDefault(); onClose(); }}>
    <label>Voice<select ref={selector} className="cha-form-control" value={voice} disabled={loading || pending || unknown}
      onChange={event => setVoice(event.target.value)}>
      <option value="">Select voice</option>
      {['boroda', 'grant', 'dzig', 'gerdt', 'Kore'].map(value => <option key={value} value={value}>{value}</option>)}
    </select></label>
    {existing && <p>Existing audio will be replaced after successful generation.</p>}
    {error && <p className="cha-error-message" role="alert">{error}</p>}
    <div className="cha-dialog-actions">
      <button className="cha-button" type="button" onClick={onClose}>Close</button>
      <button className="cha-button cha-button-primary" type="button" disabled={loading || pending || unknown || !voice}
        onClick={() => void generate()}>Generate</button>
    </div>
  </dialog>, document.body);
}
