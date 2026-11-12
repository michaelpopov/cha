import { useCallback, useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';
import { getSchema } from '@tiptap/core';
import { SingleNoteAudioSetup } from './SingleNoteAudioSetup';
import { NoteAudioVoicePicker } from './NoteAudioVoicePicker';
import { backgroundAudioStartRejected, ChaError, publicErrorMessage, type NoteAudioClient, type NotesClient } from '../api/client';
import { notesExtensions } from '../notes/editor';
import { noteAudioBlocks } from '../notes/useNoteAudio';

export type NoteAudioTarget = { id: number; name: string };
type ResolvedNoteAudioTarget = NoteAudioTarget & { documentId: number };
type AudioSetup = {
  client: NoteAudioClient;
  resolveDocumentId(id: number): Promise<number>;
  prepare(id: number, documentId: number): Promise<string[]>;
  onClose(): void;
};
type FolderAudioSetup = AudioSetup & { targets: NoteAudioTarget[]; epoch: number };
const vaultChanged = 'The vault changed. Close this dialog and start again.';
function contextEnded(failure: unknown): boolean {
  return failure instanceof ChaError && ['vault_changed', 'application_unavailable', 'server_stopping'].includes(failure.code);
}

export function NoteAudioDialog({ client, targets, epoch, folderMode, mode, getNote, onClose }: {
  client: NoteAudioClient; targets: NoteAudioTarget[]; epoch: number; folderMode: boolean;
  mode: 'full_text' | 'l1'; getNote: NotesClient['getNote']; onClose(): void;
}) {
  const readDocument = useCallback(async (id: number) => {
    if (client.contextEpoch() !== epoch) throw new ChaError('vault_changed', vaultChanged);
    const note = mode === 'l1' ? await getNote(id, mode) : await getNote(id);
    if (client.contextEpoch() !== epoch) throw new ChaError('vault_changed', vaultChanged);
    return note;
  }, [client, epoch, getNote, mode]);

  const resolveDocumentId = useCallback(async (id: number) => {
    if (mode === 'full_text') return id;
    const note = await readDocument(id);
    if (note.document_id === id) throw new ChaError('not_found', 'This note has no L1. Reopen the dialog to try again.');
    return note.document_id;
  }, [mode, readDocument]);

  async function prepare(id: number, documentId: number) {
    const note = await readDocument(id);
    if (mode === 'l1' && note.document_id !== documentId)
      throw new ChaError('not_found', 'The L1 changed. Reopen the dialog to try again.');
    return noteAudioBlocks(getSchema(notesExtensions()).nodeFromJSON({ type: 'doc', ...note.document }))
      .map(block => block.text);
  }

  const setup = { client, resolveDocumentId, prepare, onClose };
  return folderMode
    ? <FolderNoteAudioDialog {...setup} targets={targets} epoch={epoch} />
    : <SingleNoteAudioSetup {...setup} id={targets[0].id} />;
}

function FolderNoteAudioDialog({ client, targets, epoch, resolveDocumentId, prepare, onClose }: FolderAudioSetup) {
  const element = useRef<HTMLDialogElement>(null);
  const alive = useRef(true);
  const submitting = useRef(false);
  const accepted = useRef(0);
  const [voice, setVoice] = useState('');
  const [loading, setLoading] = useState(true);
  const [documents, setDocuments] = useState<ResolvedNoteAudioTarget[] | null>(null);
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
        const resolved: ResolvedNoteAudioTarget[] = [];
        for (const target of targets) {
          checkContext();
          let documentId: number;
          try { documentId = await resolveDocumentId(target.id); }
          catch (failure) {
            checkContext();
            if (failure instanceof ChaError && failure.code === 'not_found') continue;
            throw failure;
          }
          if (disposed) return;
          resolved.push({ ...target, documentId });
        }
        checkContext();
        setDocuments(resolved);
        let count = 0;
        for (const { documentId } of resolved) {
          if (await needsAudio(documentId)) ++count;
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
  }, [client, targets, epoch, resolveDocumentId]);

  async function generate() {
    if (submitting.current || loading || documents === null || blocked || empty || !voice) return;
    submitting.current = true;
    setPending(true); setError(null);
    const selectedVoice = voice;
    let target: ResolvedNoteAudioTarget | undefined;
    let submitted = false;
    try {
      checkContext();
      for (target of documents) {
        submitted = false;
        if (!await needsAudio(target.documentId)) continue;
        checkContext();
        let blocks: string[];
        try { blocks = await prepare(target.id, target.documentId); }
        catch (failure) {
          checkContext();
          if (failure instanceof ChaError && failure.code === 'not_found') continue;
          throw failure;
        }
        checkContext();
        submitted = true;
        const result = await client.startBackground(target.documentId, selectedVoice, blocks, true);
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
    {!empty && <NoteAudioVoicePicker client={client} value={voice} onChange={setVoice}
      disabled={loading || documents === null || pending || blocked} />}
    {loading && <p role="status">Checking notes…</p>}
    {empty && !error && <p role="status">No notes need audio</p>}
    {error && <p className="cha-error-message" role="alert">{error}</p>}
    <div className="cha-dialog-actions">
      <button className="cha-button" type="button" disabled={pending} onClick={() => { if (!submitting.current) onClose(); }}>Close</button>
      {!empty && <button className="cha-button cha-button-primary" type="button" disabled={loading || documents === null || pending || blocked || !voice}
        onClick={() => void generate()}>Generate</button>}
    </div>
  </dialog>, document.body);
}
