import { useEffect, useRef, useState } from 'react';

import type { NativeVoiceInputRuntime } from '../api/client';
import { appendPreparedTranscription, appendTranscription, withoutVoiceSendPhrase } from '../dictationText';
import { onSpeechPlaybackChange } from '../speechPlayback';
import { VoiceInputSession, type VoiceInputTransport } from '../voiceInput';
import { chaWebMessage, type ChaWebClient } from './client';

export function useVoiceInput(
  client: ChaWebClient,
  key: string,
  draft: string,
  onDraft: (text: string) => void,
  onSend: (text: string) => void,
  blocked: boolean,
  speechBusy = false,
) {
  const [runtime, setRuntime] = useState<NativeVoiceInputRuntime | null>(null);
  const [enabled, setEnabled] = useState(false);
  const [phase, setPhase] = useState<'idle' | 'starting' | 'recording' | 'finishing'>('idle');
  const [playing, setPlaying] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const session = useRef<VoiceInputTransport | null>(null);
  const startup = useRef<AbortController | null>(null);
  const attempt = useRef(0);
  const preview = useRef('');
  const phraseTimer = useRef<ReturnType<typeof setTimeout> | undefined>(undefined);
  const captureContext = useRef<AudioContext | undefined>(undefined);
  const previousKey = useRef(key);
  const submittedKey = useRef<string | null>(null);
  const current = useRef({ draft, onDraft, onSend, blocked, speechBusy, playing, phase, runtime });
  current.current = { draft, onDraft, onSend, blocked, speechBusy, playing, phase, runtime };

  function changePhase(value: typeof phase) {
    current.current.phase = value;
    setPhase(value);
  }

  function update(text: string) {
    current.current.draft = text;
    current.current.onDraft(text);
  }

  function clearPreview() {
    const text = current.current.draft;
    if (preview.current && text.endsWith(preview.current)) {
      update(text.slice(0, -preview.current.length));
    }
    preview.current = '';
  }

  function cancel() {
    ++attempt.current;
    clearTimeout(phraseTimer.current);
    startup.current?.abort();
    startup.current = null;
    session.current?.cancel();
    session.current = null;
    // Keep the visible draft even if the provider could not confirm its last words.
    preview.current = '';
    changePhase('idle');
  }

  function fail(failure: unknown) {
    cancel();
    setEnabled(false);
    setError(failure instanceof Error ? failure.message
      : chaWebMessage(failure, 'Voice input failed. Try again.'));
  }

  const available = window.isSecureContext === true && runtime !== null
    && VoiceInputSession.supported(runtime.provider);

  async function start() {
    const config = current.current.runtime;
    if (!config || current.current.blocked || current.current.speechBusy || current.current.playing
      || startup.current || session.current) return;
    const id = ++attempt.current;
    const controller = new AbortController();
    startup.current = controller;
    changePhase('starting');
    setError(null);
    try {
      const transport = await VoiceInputSession.start(config,
        (text, provisional) => {
          if (attempt.current !== id || current.current.blocked || current.current.speechBusy
            || current.current.playing) return;
          clearPreview();
          const base = current.current.draft;
          const next = provisional || config.provider === 'xai'
            ? appendPreparedTranscription(base, text)
            : appendTranscription(base, text);
          if (provisional) preview.current = next.slice(base.length);
          update(next);
          clearTimeout(phraseTimer.current);
          if (withoutVoiceSendPhrase(next, config.send_phrase) !== null) {
            // Finalize recognition before interpreting the phrase as a command.
            phraseTimer.current = setTimeout(() => void send(true), 1_000);
          }
        },
        (failure) => { if (attempt.current === id) fail(failure); },
        client.connectVoiceInput,
        {
          start: client.startXaiVoiceInput,
          audio: client.sendXaiVoiceAudio,
          stop: client.stopXaiVoiceInput,
          cancel: client.cancelXaiVoiceInput,
        },
        controller.signal,
        captureContext.current ? { context: captureContext.current, batchSamples: 8000 } : undefined,
      );
      if (attempt.current !== id || controller.signal.aborted) {
        transport.cancel();
        return;
      }
      startup.current = null;
      session.current = transport;
      changePhase('recording');
    } catch (failure) {
      if (attempt.current === id) fail(failure);
    }
  }

  async function finish(): Promise<boolean> {
    const transport = session.current;
    if (!transport) return !startup.current;
    const id = attempt.current;
    changePhase('finishing');
    clearTimeout(phraseTimer.current);
    let timeout: ReturnType<typeof setTimeout> | undefined;
    try {
      await Promise.race([
        transport.stop(),
        new Promise<never>((_resolve, reject) => {
          timeout = setTimeout(() => reject(new Error('Voice input timed out.')), 20_000);
        }),
      ]);
      if (attempt.current !== id) return false;
      clearTimeout(phraseTimer.current);
      clearPreview();
      ++attempt.current;
      session.current = null;
      changePhase('idle');
      return true;
    } catch (failure) {
      if (attempt.current === id) fail(failure);
      return false;
    } finally {
      clearTimeout(timeout);
    }
  }

  async function send(spoken = false) {
    if (current.current.blocked || current.current.phase === 'finishing'
      || startup.current) return;
    const wasRecording = session.current !== null;
    if (wasRecording && !await finish()) return;
    const phrase = current.current.runtime?.send_phrase ?? '';
    const text = spoken || wasRecording ? withoutVoiceSendPhrase(current.current.draft, phrase) : null;
    if (spoken && text === null) return;
    if (text !== null) update(text);
    if (!current.current.draft.trim()) return;
    submittedKey.current = previousKey.current;
    current.current.onSend(current.current.draft);
  }

  function toggle() {
    if (enabled) {
      setEnabled(false);
      if (startup.current) cancel();
      else void finish();
      return;
    }
    if (!available || blocked || speechBusy || playing || phase !== 'idle') return;
    // Unlock once in the microphone tap, then reuse this context after replies.
    if (runtime?.provider === 'xai') {
      try {
        captureContext.current ??= new AudioContext({ sampleRate: 16_000 });
        void captureContext.current.resume().catch(fail);
      } catch (failure) {
        fail(failure);
        return;
      }
    }
    setEnabled(true);
    void start();
  }

  useEffect(() => {
    let active = true;
    void client.getVoiceInputRuntime().then(
      (value) => { if (active) setRuntime(value); },
      (failure) => { if (active) setError(chaWebMessage(failure, 'Voice input settings could not be loaded.')); },
    );
    return () => { active = false; };
  }, [client, key]);

  useEffect(() => onSpeechPlaybackChange((value) => {
    current.current.playing = value;
    setPlaying(value);
    if (value) cancel();
  }), []);

  useEffect(() => {
    if (previousKey.current !== key) {
      const created = submittedKey.current === previousKey.current
        && previousKey.current.startsWith('new:') && key.startsWith('session:');
      preview.current = '';
      previousKey.current = key;
      submittedKey.current = null;
      cancel();
      setError(null);
      if (!created) setEnabled(false);
      return;
    }
    if (!available) {
      if (session.current || startup.current) cancel();
      if (enabled) setEnabled(false);
      return;
    }
    if (blocked || speechBusy || playing) {
      if (session.current || startup.current) cancel();
    } else if (enabled && available && phase === 'idle') {
      void start();
    }
  }, [key, blocked, speechBusy, playing, enabled, available, phase]);

  useEffect(() => {
    function hide() {
      if (document.visibilityState === 'hidden') {
        setEnabled(false);
        cancel();
      }
    }
    document.addEventListener('visibilitychange', hide);
    return () => {
      document.removeEventListener('visibilitychange', hide);
      cancel();
      void captureContext.current?.close().catch(() => undefined);
    };
  }, []);

  return {
    available,
    enabled,
    label: phase === 'starting' ? 'Cancel voice input setup'
      : enabled ? 'Stop voice input' : 'Start voice input',
    disabled: phase === 'finishing' || (!enabled && (blocked || speechBusy || playing)),
    finishing: phase === 'finishing',
    error,
    toggle,
    send: () => { void send(); },
  };
}

export type VoiceInput = ReturnType<typeof useVoiceInput>;
