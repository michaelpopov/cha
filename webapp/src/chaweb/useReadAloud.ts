import { useEffect, useRef, useState } from 'react';

import type { AudioDownloadAcceptance, SessionSnapshot } from '../api/client';
import { TextToSpeechError, TextToSpeechSession, useTextToSpeechConfiguration } from '../textToSpeech';
import { audioUrl, chaWebMessage, type ChaWebClient } from './client';
import type { TranscriptEntry } from './transcript';

export interface ReadAloud {
  available: boolean;
  entryId: number | null;
  state: 'loading' | 'playing';
  error: string | null;
  clearing: boolean;
  clearDisabled: boolean;
  clear(): Promise<void>;
  isCached(entry: TranscriptEntry): boolean;
  toggle(entry: TranscriptEntry): void;
}

export function useReadAloud(
  client: ChaWebClient,
  snapshot: SessionSnapshot | null,
  vaultName: string | undefined,
  deleting: boolean,
): ReadAloud {
  const configuration = useTextToSpeechConfiguration(client);
  const [entryId, setEntryId] = useState<number | null>(null);
  const [state, setState] = useState<'loading' | 'playing'>('loading');
  const [error, setError] = useState<string | null>(null);
  const [clearing, setClearing] = useState(false);
  const [downloaded, setDownloaded] = useState<ReadonlySet<number>>(new Set());
  const clearingRef = useRef(false);
  const cacheCleared = useRef(false);
  const pending = useRef(new Set<Promise<AudioDownloadAcceptance>>());
  const selected = useRef<number | null>(null);
  const attempt = useRef(0);
  const player = useRef<TextToSpeechSession | null>(null);
  const positions = useRef(new Map<number, number>());
  const forum = snapshot?.forum.id;
  const session = snapshot?.session_id;

  function stop() {
    attempt.current += 1;
    player.current?.stop();
    player.current = null;
    selected.current = null;
    setEntryId(null);
  }

  useEffect(() => {
    setError(null);
    setClearing(false);
    setDownloaded(new Set());
    clearingRef.current = false;
    cacheCleared.current = false;
    pending.current.clear();
    positions.current.clear();
    return stop;
    // Each player and pending request belongs to one conversation.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [client, forum, session, vaultName, deleting]);

  function isCached(entry: TranscriptEntry): boolean {
    return downloaded.has(entry.id) || Boolean(entry.has_cached_audio && !cacheCleared.current);
  }

  function toggle(entry: TranscriptEntry) {
    if (!forum || !session || !vaultName || deleting || clearingRef.current
      || entry.kind !== 'character' || entry.status !== 'complete' || !entry.text.trim()) return;
    const wasSelected = selected.current === entry.id;
    stop();
    setError(null);
    if (wasSelected) return;
    selected.current = entry.id;
    setEntryId(entry.id);
    setState('loading');
    const current = attempt.current;
    const active = () => attempt.current === current;
    const markCached = () => {
      if (active()) setDownloaded((previous) => new Set([...previous, entry.id]));
    };
    const failed = (failure: unknown) => {
      if (!active()) return;
      stop();
      positions.current.delete(entry.id);
      setError(failure instanceof DOMException && failure.name === 'NotAllowedError'
        ? 'Playback was blocked. Click Read aloud to try again.'
        : failure instanceof TextToSpeechError
          ? failure.message : chaWebMessage(failure, 'Audio could not be played. Try again.'));
    };
    function play(streaming = false) {
      if (!active()) return;
      const playback = new TextToSpeechSession(
        null, undefined, entry.text,
        () => { if (active()) stop(); },
        {
          position: positions.current.get(entry.id) ?? 0,
          onPositionChange: (position) => {
            if (position > 0) positions.current.set(entry.id, position);
            else positions.current.delete(entry.id);
          },
        },
        audioUrl(forum!, session!, entry.id),
        markCached,
        undefined, undefined, { streaming, onError: failed },
      );
      player.current = playback;
      void playback.play().then(() => {
        if (active()) setState('playing');
      }, failed);
    }
    // Cache hints in a polled snapshot can still predate the clear request.
    if (isCached(entry)) { play(); return; }
    const request = client.startAudio(forum, session, entry.id, vaultName);
    pending.current.add(request);
    void request.then((accepted) => {
      if (!active()) return;
      if (accepted.cached) markCached();
      play(!accepted.cached);
    }, failed).finally(() => pending.current.delete(request));
  }

  async function clear() {
    if (!forum || !session || !vaultName || deleting || clearingRef.current) return;
    clearingRef.current = true;
    setClearing(true);
    stop();
    positions.current.clear();
    setError(null);
    const current = attempt.current;
    try {
      // A previous generation request must be admitted before we cancel its job.
      await Promise.allSettled([...pending.current]);
      await client.clearAudio(forum, session, vaultName);
      if (attempt.current === current) {
        cacheCleared.current = true;
        setDownloaded(new Set());
      }
    } catch (failure) {
      if (attempt.current === current) {
        setError(chaWebMessage(failure, 'Audio recordings could not be cleared. Try again.'));
      }
    } finally {
      if (attempt.current === current) {
        clearingRef.current = false;
        setClearing(false);
      }
    }
  }

  return { available: configuration !== null, entryId, state, error, toggle, clearing, clear, isCached,
    clearDisabled: !forum || !session || !vaultName || deleting || clearing };
}
