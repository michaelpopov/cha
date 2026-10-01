import { useEffect, useMemo, useRef, useState } from 'react';

import { ChaError, type ChaClient, type SessionSnapshot } from '../api/client';
import { useAudioDownloads } from '../audioDownloads';
import { TextToSpeechError, TextToSpeechSession, useTextToSpeechConfiguration } from '../textToSpeech';
import { audioUrl, ChaWebError, chaWebMessage, type ChaWebClient } from './client';
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
  isDownloading(entry: TranscriptEntry): boolean;
  toggle(entry: TranscriptEntry): void;
  automatic: boolean;
  automaticDisabled: boolean;
  toggleAutomatic(): void;
}

function canRead(entry: TranscriptEntry): boolean {
  return entry.kind === 'character' && entry.status === 'complete' && Boolean(entry.text.trim());
}

// A short silent WAV unlocks this element from the user's click on Safari.
const silence = 'data:audio/wav;base64,UklGRiUAAABXQVZFZm10IBAAAAABAAEARKwAAESsAAABAAgAZGF0YQEAAACA';

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
  const [automatic, setAutomatic] = useState(false);
  const [submitting, setSubmitting] = useState(false);
  const [clearCount, setClearCount] = useState(0);
  const automaticRef = useRef(false);
  const automaticPlayback = useRef(false);
  const automaticAudio = useRef<HTMLAudioElement | null>(null);
  const modeAttempt = useRef(0);
  const handledDownloads = useRef(new Set<number>());
  const handledSpeech = useRef(new Set<number>());
  const clearingRef = useRef(false);
  const cacheCleared = useRef(false);
  const pending = useRef(new Set<Promise<unknown>>());
  const selected = useRef<number | null>(null);
  const attempt = useRef(0);
  const player = useRef<TextToSpeechSession | null>(null);
  const positions = useRef(new Map<number, number>());
  const forum = snapshot?.forum.id;
  const session = snapshot?.session_id;

  const downloadsClient = useMemo<Pick<ChaClient,
    'getAudioDownloads' | 'startAudioDownload' | 'startAudioDownloadBatch'>>(() => {
    function track<T>(request: Promise<T>) {
      pending.current.add(request);
      void request.then(() => pending.current.delete(request), () => pending.current.delete(request));
      return request;
    }
    return {
      async getAudioDownloads(forum, session) {
        try { return await client.getAudioStatus(forum, session); }
        catch (failure) {
          // The shared observer distinguishes permanent errors from transient failures.
          if (failure instanceof ChaWebError) {
            const code = failure.code;
            if (code === 'not_found' || code === 'vault_changed' || code === 'invalid_argument'
              || code === 'application_unavailable' || code === 'session_not_live') {
              throw new ChaError(code, failure.message);
            }
          }
          throw failure;
        }
      },
      startAudioDownload: (forum, session, id, request) =>
        track(client.startAudio(forum, session, id, request.vault_name)),
      startAudioDownloadBatch: (forum, session, request) => track(client.startAudioBatch(
        forum, session, request.entries.map((entry) => entry.entry_id), request.vault_name)),
    };
  }, [client]);
  const downloads = useAudioDownloads(downloadsClient, forum, session, vaultName, clearCount);

  function stop() {
    attempt.current += 1;
    player.current?.stop();
    player.current = null;
    selected.current = null;
    automaticPlayback.current = false;
    setEntryId(null);
  }

  useEffect(() => {
    setError(null);
    setClearing(false);
    setDownloaded(new Set());
    disableAutomatic();
    clearingRef.current = false;
    cacheCleared.current = false;
    pending.current.clear();
    positions.current.clear();
    return () => {
      modeAttempt.current += 1;
      automaticRef.current = false;
      stop();
      automaticAudio.current?.pause();
      automaticAudio.current = null;
    };
    // Each player and pending request belongs to one conversation.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [client, forum, session, vaultName, deleting]);

  function isCached(entry: TranscriptEntry): boolean {
    return downloaded.has(entry.id) || Boolean(downloads.status?.cached_entry_ids.includes(entry.id))
      || Boolean(entry.has_cached_audio && !cacheCleared.current);
  }

  function isDownloading(entry: TranscriptEntry): boolean {
    return !isCached(entry) && (entryId === entry.id || Boolean(downloads.status?.downloads
      .some((job) => job.entry_id === entry.id && job.state !== 'failed')));
  }

  function disableAutomatic() {
    modeAttempt.current += 1;
    automaticRef.current = false;
    setAutomatic(false);
    setSubmitting(false);
    if (automaticPlayback.current) stop();
  }

  function toggleAutomatic() {
    if (automaticRef.current) { disableAutomatic(); return; }
    if (!forum || !session || !vaultName || deleting || clearingRef.current || !configuration) return;
    setError(null);
    modeAttempt.current += 1;
    handledDownloads.current.clear();
    // Old completed replies are cached; only replies completed from now on are played.
    handledSpeech.current = new Set(snapshot?.transcript
      .filter((entry) => entry.status !== 'streaming').map((entry) => entry.id));
    if (!automaticAudio.current) {
      const audio = new Audio();
      automaticAudio.current = audio;
      audio.src = silence;
      void audio.play().catch(() => {});
    }
    automaticRef.current = true;
    setAutomatic(true);
    downloads.refresh();
  }

  function toggle(entry: TranscriptEntry, automatically = false) {
    if (!forum || !session || !vaultName || deleting || clearingRef.current
      || !canRead(entry)) return;
    handledSpeech.current.add(entry.id);
    const wasSelected = selected.current === entry.id;
    stop();
    setError(null);
    if (wasSelected) return;
    selected.current = entry.id;
    automaticPlayback.current = automatically;
    setEntryId(entry.id);
    setState('loading');
    const current = attempt.current;
    const active = () => attempt.current === current;
    const markCached = () => {
      if (active()) setDownloaded((previous) => new Set([...previous, entry.id]));
    };
    const failed = (failure: unknown) => {
      if (!active()) return;
      if (automatically) disableAutomatic();
      stop();
      positions.current.delete(entry.id);
      setError(failure instanceof DOMException && failure.name === 'NotAllowedError'
        ? 'Playback was blocked. Click the audio icon to try again.'
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
        undefined, undefined, { streaming, onError: failed,
          ...(automaticAudio.current ? { audio: automaticAudio.current } : {}) },
      );
      player.current = playback;
      void playback.play().then(() => {
        if (active()) setState('playing');
      }, failed);
    }
    // Cache hints in a polled snapshot can still predate the clear request.
    if (isCached(entry)) { play(); return; }
    if (automatically) { play(true); return; }
    const request = downloadsClient.startAudioDownload(forum, session, entry.id,
      { vault_name: vaultName, reference_id: configuration?.voiceId ?? '' });
    void request.then((accepted) => {
      if (!active()) return;
      if (accepted.cached) markCached();
      downloads.refresh();
      play(!accepted.cached);
    }, failed);
  }

  useEffect(() => {
    if (!automaticRef.current || submitting || clearing || !configuration || !snapshot
      || !vaultName || !downloads.status) return;
    if (downloads.unavailable) {
      setError(downloads.unavailable);
      disableAutomatic();
      return;
    }
    const jobs = new Set(downloads.status.downloads.map((job) => job.entry_id));
    const entries = snapshot.transcript.filter((entry) => {
      if (!canRead(entry) || handledDownloads.current.has(entry.id)) return false;
      handledDownloads.current.add(entry.id);
      return !isCached(entry) && !jobs.has(entry.id);
    }).map((entry) => ({ entry_id: entry.id, reference_id: configuration.voiceId }));
    if (entries.length === 0) return;
    const current = modeAttempt.current;
    setSubmitting(true);
    void downloads.submitBatch({ vault_name: vaultName, entries })?.catch((failure: unknown) => {
      if (modeAttempt.current !== current) return;
      setError(chaWebMessage(failure, 'Audio could not be downloaded. Try again.'));
      disableAutomatic();
    }).finally(() => {
      if (modeAttempt.current === current) setSubmitting(false);
    });
  });

  useEffect(() => {
    if (!automaticRef.current || submitting || clearing || deleting || !snapshot
      || !downloads.status || downloads.unavailable || selected.current !== null) return;
    const jobs = new Map(downloads.status.downloads.map((job) => [job.entry_id, job]));
    for (const entry of snapshot.transcript) {
      if (entry.kind !== 'character' || handledSpeech.current.has(entry.id)) continue;
      const job = jobs.get(entry.id);
      if (entry.status === 'failed' || entry.status === 'cancelled' || job?.state === 'failed'
        || (entry.status === 'complete' && !entry.text.trim())) {
        handledSpeech.current.add(entry.id);
        if (job?.state === 'failed') setError(job.error ?? 'Audio could not be downloaded. Try again.');
        continue;
      }
      // Wait for an earlier reply to complete, even if a later clip is ready.
      if (!canRead(entry) || (!isCached(entry) && !job)) return;
      toggle(entry, true);
      return;
    }
  });

  async function clear() {
    if (!forum || !session || !vaultName || deleting || clearingRef.current) return;
    clearingRef.current = true;
    setClearing(true);
    disableAutomatic();
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
        setClearCount((count) => count + 1);
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

  return { available: configuration !== null, entryId, state, error, toggle, clearing, clear, isCached, isDownloading,
    automatic, toggleAutomatic,
    automaticDisabled: !forum || !session || !vaultName || deleting || clearing
      || (!configuration && !automatic),
    clearDisabled: !forum || !session || !vaultName || deleting || clearing };
}
