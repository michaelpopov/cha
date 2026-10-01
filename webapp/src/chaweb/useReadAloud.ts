import { useEffect, useRef, useState } from 'react';

import type { SessionSnapshot } from '../api/client';
import { TextToSpeechError, TextToSpeechSession, useTextToSpeechConfiguration } from '../textToSpeech';
import { audioUrl, ChaWebError, chaWebMessage, type ChaWebClient } from './client';
import type { TranscriptEntry } from './transcript';

export interface ReadAloud {
  available: boolean;
  entryId: number | null;
  state: 'loading' | 'playing';
  error: string | null;
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
  const selected = useRef<number | null>(null);
  const attempt = useRef(0);
  const player = useRef<TextToSpeechSession | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const forum = snapshot?.forum.id;
  const session = snapshot?.session_id;

  function stop() {
    attempt.current += 1;
    clearTimeout(timer.current);
    player.current?.stop();
    player.current = null;
    selected.current = null;
    setEntryId(null);
  }

  useEffect(() => {
    setError(null);
    return stop;
    // Each player and pending request belongs to one conversation.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [client, forum, session, vaultName, deleting]);

  function toggle(entry: TranscriptEntry) {
    if (!forum || !session || !vaultName || deleting
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
    const failed = (failure: unknown) => {
      if (!active()) return;
      stop();
      setError(failure instanceof DOMException && failure.name === 'NotAllowedError'
        ? 'Playback was blocked. Click Read aloud to try again.'
        : failure instanceof TextToSpeechError
          ? failure.message : chaWebMessage(failure, 'Audio could not be played. Try again.'));
    };
    function play() {
      if (!active()) return;
      const playback = new TextToSpeechSession(
        null, undefined, entry.text,
        () => { if (active()) stop(); },
        undefined, audioUrl(forum!, session!, entry.id),
        undefined, undefined, undefined, { onError: failed },
      );
      player.current = playback;
      void playback.play().then(() => {
        if (active()) setState('playing');
      }, failed);
    }
    async function poll() {
      try {
        const status = await client.getAudioStatus(forum!, session!);
        if (!active()) return;
        if (status.cached_entry_ids.includes(entry.id)) { play(); return; }
        const job = status.downloads.find(({ entry_id }) => entry_id === entry.id);
        if (!job || job.state === 'failed') {
          failed(new ChaWebError(500, job?.error ?? 'Audio generation failed. Try again.'));
          return;
        }
        timer.current = setTimeout(() => void poll(), 1_000);
      } catch (failure) { failed(failure); }
    }
    if (entry.has_cached_audio) { play(); return; }
    void client.startAudio(forum, session, entry.id, vaultName).then((accepted) => {
      if (!active()) return;
      if (accepted.cached) play();
      else void poll();
    }, failed);
  }

  return { available: configuration !== null, entryId, state, error, toggle };
}
