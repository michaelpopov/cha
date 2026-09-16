import { useEffect, useState } from 'react';

import type { ChaClient, VoiceUpdate } from './api/client';
import { audioRequest, type AudioRequest, type AudioEntry } from './textToSpeechRequest';

export interface TextToSpeechConfiguration {
  baseUrl: string;
  voiceId: string;
  outputFormat: string;
  model: string;
}

export interface TextToSpeechVoice {
  // Legacy field name; contains a FishAudio reference ID.
  elevenlabs_voice_id: string;
  settings: {
    speed?: number;
  };
}

export function speechVoice(voice: Pick<VoiceUpdate, 'elevenlabs_voice_id' | 'speed'>): TextToSpeechVoice {
  return {
    elevenlabs_voice_id: voice.elevenlabs_voice_id,
    settings: voice.speed === null ? {} : { speed: voice.speed },
  };
}

export function useTextToSpeechConfiguration(
  client: Pick<ChaClient, 'getVoiceOutputRuntime'>,
): TextToSpeechConfiguration | null {
  const [configuration, setConfiguration] =
    useState<TextToSpeechConfiguration | null>(null);
  useEffect(() => {
    let current = true;
    setConfiguration(null);
    void client.getVoiceOutputRuntime().then(
      (loaded) => {
        if (!current) return;
        setConfiguration(loaded && {
          baseUrl: loaded.url,
          voiceId: loaded.default_voice_id,
          outputFormat: loaded.output_format,
          model: loaded.model,
        });
      },
      () => { if (current) setConfiguration(null); },
    );
    return () => { current = false; };
  }, [client]);
  return configuration;
}

export class TextToSpeechError extends Error {
  constructor(message: string, readonly code?: string) {
    super(message);
    this.name = 'TextToSpeechError';
  }
}

interface FishAudioJob {
  start(): void;
}

const fishAudioQueue: FishAudioJob[] = [];
let activeFishAudio = 0;

function pumpFishAudio() {
  // Leave browser connections for the event stream and normal CHA requests.
  while (activeFishAudio < 3 && fishAudioQueue.length > 0) {
    fishAudioQueue.shift()!.start();
  }
}

interface AudioResult {
  blob: Blob;
  cached: boolean;
}

function requestAudio(request: AudioRequest, signal: AbortSignal): Promise<AudioResult> {
  return new Promise<AudioResult>((resolve, reject) => {
    const cancel = () => {
      const index = fishAudioQueue.indexOf(job);
      if (index >= 0) fishAudioQueue.splice(index, 1);
      reject(new DOMException('Speech generation cancelled.', 'AbortError'));
    };
    const job: FishAudioJob = {
      start() {
        signal.removeEventListener('abort', cancel);
        activeFishAudio += 1;
        // Hold the slot until the complete response body has been received.
        void fetchAudio(request, signal).then(resolve, reject).finally(() => {
          activeFishAudio -= 1;
          pumpFishAudio();
        });
      },
    };
    if (signal.aborted) return cancel();
    signal.addEventListener('abort', cancel, { once: true });
    fishAudioQueue.push(job);
    pumpFishAudio();
  });
}

async function fetchAudio(request: AudioRequest, signal?: AbortSignal): Promise<AudioResult> {
  for (let attempt = 0; ; attempt += 1) {
    const response = await fetch(request.url, {
      method: 'POST', headers: request.headers, body: request.body, signal,
    });
    if (response.ok) return {
      blob: await response.blob(),
      cached: response.headers.get('X-CHA-Audio-Cached') === 'true',
    };
    const error = await speechError(response);
    if (response.status !== 503
      || error.code !== 'speech_busy' || attempt >= 3) throw error;
    await waitForSpeechRetry(signal);
  }
}

function waitForSpeechRetry(signal?: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const cancel = () => {
      clearTimeout(timer);
      reject(new DOMException('Speech generation cancelled.', 'AbortError'));
    };
    const timer = setTimeout(() => {
      signal?.removeEventListener('abort', cancel);
      resolve();
    }, 1000);
    if (signal?.aborted) return cancel();
    signal?.addEventListener('abort', cancel, { once: true });
  });
}

export class TextToSpeechSession {
  private readonly request = new AbortController();
  private audio: HTMLAudioElement | null = null;
  private objectUrl: string | null = null;
  private stopped = false;

  constructor(
    private readonly configuration: TextToSpeechConfiguration,
    private readonly voice: TextToSpeechVoice | undefined,
    private readonly text: string,
    private readonly onEnded: () => void,
    private readonly entry?: AudioEntry,
    private readonly onCacheStatus?: (cached: boolean) => void,
    private readonly playback?: {
      position: number;
      onPositionChange(position: number): void;
    },
  ) {}

  async play(): Promise<void> {
    const request = audioRequest(this.configuration, this.voice, this.text, this.entry);
    const { blob, cached } = await requestAudio(request, this.request.signal);
    if (this.stopped) return;
    if (this.entry) this.onCacheStatus?.(cached);

    this.objectUrl = URL.createObjectURL(blob);
    if (this.stopped) return this.releaseObjectUrl();
    const audio = new Audio(this.objectUrl);
    this.audio = audio;
    audio.addEventListener('ended', () => this.finish(true), { once: true });
    audio.addEventListener('error', () => this.finish(), { once: true });
    if (this.playback && this.playback.position > 0) {
      const position = this.playback.position;
      const resume = () => {
        if (this.stopped) return;
        audio.currentTime = Number.isFinite(audio.duration) && position >= audio.duration ? 0 : position;
      };
      if (audio.readyState >= 1) resume();
      else audio.addEventListener('loadedmetadata', resume, { once: true });
    }
    await audio.play();
  }

  stop(resetPosition = false): void {
    if (this.stopped) return;
    this.stopped = true;
    this.request.abort();
    this.audio?.pause();
    if (this.playback && this.audio) {
      if (resetPosition) this.playback.onPositionChange(0);
      else if (this.audio.readyState >= 1 && Number.isFinite(this.audio.currentTime)) {
        this.playback.onPositionChange(this.audio.currentTime);
      }
    }
    this.audio = null;
    this.releaseObjectUrl();
  }

  private finish(completed = false): void {
    if (this.stopped) return;
    this.stop(completed);
    this.onEnded();
  }

  private releaseObjectUrl(): void {
    if (!this.objectUrl) return;
    URL.revokeObjectURL(this.objectUrl);
    this.objectUrl = null;
  }
}

async function speechError(response: Response): Promise<TextToSpeechError> {
  const fallback = `FishAudio request failed (HTTP ${response.status}).`;
  try {
    const parsed: unknown = await response.json();
    if (!parsed || typeof parsed !== 'object') return new TextToSpeechError(fallback);
    const root = parsed as Record<string, unknown>;
    const nested = root.error;
    const detail = nested && typeof nested === 'object'
      ? nested as Record<string, unknown> : root;
    const message = typeof detail.message === 'string' ? detail.message.trim() : '';
    return new TextToSpeechError(
      message ? `FishAudio: ${message} (HTTP ${response.status})` : fallback,
      typeof detail.code === 'string' ? detail.code : undefined,
    );
  } catch {
    return new TextToSpeechError(fallback);
  }
}
