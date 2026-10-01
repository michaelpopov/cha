import { useEffect, useRef, useState } from 'react';

import type { SessionSnapshot } from '../api/client';
import { TextToSpeechError, TextToSpeechSession, useTextToSpeechConfiguration } from '../textToSpeech';
import { audioUrl, chaWebMessage, type ChaWebClient } from './client';
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
    function play(streaming = false) {
      if (!active()) return;
      const playback = new TextToSpeechSession(
        null, undefined, entry.text,
        () => { if (active()) stop(); },
        undefined, audioUrl(forum!, session!, entry.id),
        undefined, undefined, undefined, { streaming, onError: failed },
      );
      player.current = playback;
      void playback.play().then(() => {
        if (active()) setState('playing');
      }, failed);
    }
    if (entry.has_cached_audio) { play(); return; }
    void client.startAudio(forum, session, entry.id, vaultName).then((accepted) => {
      if (!active()) return;
      play(!accepted.cached);
    }, failed);
  }

  return { available: configuration !== null, entryId, state, error, toggle };
}
