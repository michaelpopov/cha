import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { backgroundAudioStartRejected, publicErrorMessage, type NoteAudioClient } from '../api/client';
import { NoteAudioVoicePicker } from './NoteAudioVoicePicker';

export const audioStartUnknown = 'Could not confirm whether audio generation started. Check Settings → Events before starting again.';

export function SingleNoteAudioSetup({ client, id, resolveDocumentId, prepare, onClose }: {
  client: NoteAudioClient; id: number; resolveDocumentId(id: number): Promise<number>;
  prepare(id: number, documentId: number): Promise<string[]>; onClose(): void;
}) {
  const element = useRef<HTMLDialogElement>(null);
  const submitting = useRef(false);
  const alive = useRef(true);
  const [voice, setVoice] = useState('');
  const [loading, setLoading] = useState(true);
  const [documentId, setDocumentId] = useState<number | null>(null);
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
    void (async () => {
      await Promise.resolve();
      if (disposed) return;
      try {
        const document = await resolveDocumentId(id);
        if (disposed) return;
        setDocumentId(document);
        const info = await client.info(document);
        if (!disposed) setExisting(info.count > 0);
      } catch (failure) {
        if (!disposed) setError(publicErrorMessage(failure, 'Could not load note audio.'));
      } finally { if (!disposed) setLoading(false); }
    })();
    return () => {
      disposed = true; alive.current = false; node.close?.();
      if (previous instanceof HTMLElement && previous.isConnected) previous.focus();
    };
  }, [client, id, resolveDocumentId]);

  async function generate() {
    if (submitting.current || loading || documentId === null || unknown || !voice) return;
    submitting.current = true; setPending(true); setError(null);
    let submitted = false;
    try {
      const blocks = await prepare(id, documentId);
      if (!alive.current) return;
      submitted = true;
      await client.startBackground(documentId, voice, blocks);
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
    <NoteAudioVoicePicker client={client} value={voice} onChange={setVoice}
      disabled={loading || documentId === null || pending || unknown} />
    {existing && <p>Existing audio will be replaced after successful generation.</p>}
    {error && <p className="cha-error-message" role="alert">{error}</p>}
    <div className="cha-dialog-actions">
      <button className="cha-button" type="button" onClick={onClose}>Close</button>
      <button className="cha-button cha-button-primary" type="button" disabled={loading || documentId === null || pending || unknown || !voice}
        onClick={() => void generate()}>Generate</button>
    </div>
  </dialog>, document.body);
}
