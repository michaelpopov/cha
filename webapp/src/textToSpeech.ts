import { useEffect, useState } from 'react';

import type { ChaClient } from './api/client';
import { audioRequest, type AudioRequest } from './textToSpeechRequest';

export interface TextToSpeechConfiguration {
  baseUrl: string;
  voiceId: string;
  outputFormat: string;
  apiKey: string;
  model: string;
}

export interface TextToSpeechVoice {
  // Legacy field name; the ID belongs to the configured speech provider.
  elevenlabs_voice_id: string;
  settings: {
    stability?: number;
    similarity_boost?: number;
    style?: number;
    use_speaker_boost?: boolean;
    speed?: number;
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
          apiKey: loaded.api_key ?? '',
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

const maximumCachedAudioBytes = 256 * 1024 * 1024;
const audioCache = new Map<string, Blob>();

interface PendingAudio {
  promise: Promise<Blob>;
  prioritize?: () => void;
  retainPrefetch?: (signal?: AbortSignal) => void;
}

interface FishAudioJob {
  prefetch: boolean;
  start(): void;
}

const pendingCachedAudio = new Map<string, PendingAudio>();
const fishAudioQueue: FishAudioJob[] = [];
let activeFishAudio = 0;
let activeFishAudioPrefetch = 0;

function pumpFishAudio() {
  // Leave browser connections for the event stream and normal CHA requests.
  while (activeFishAudio < 3) {
    let index = fishAudioQueue.findIndex((job) => !job.prefetch);
    if (index < 0) {
      // Background caching leaves one speech slot available for playback.
      if (activeFishAudioPrefetch >= 2 || fishAudioQueue.length === 0) return;
      index = 0;
    }
    const [job] = fishAudioQueue.splice(index, 1);
    job.start();
  }
}

function requestAudio(request: AudioRequest, signal?: AbortSignal, prefetch = false): PendingAudio {
  if (request.provider !== 'FishAudio') return { promise: fetchAudio(request, signal) };

  let job!: FishAudioJob;
  let transfer!: PendingAudio;
  const prefetchOwners = new Set<AbortSignal | undefined>();
  let discardPrefetch!: () => void;
  const detachPrefetch = () => {
    for (const owner of prefetchOwners) owner?.removeEventListener('abort', discardPrefetch);
    prefetchOwners.clear();
  };
  const promise = new Promise<Blob>((resolve, reject) => {
    const cancel = () => {
      const index = fishAudioQueue.indexOf(job);
      if (index >= 0) fishAudioQueue.splice(index, 1);
      detachPrefetch();
      // A new cache run can immediately request this clip again.
      if (pendingCachedAudio.get(request.cacheKey) === transfer) pendingCachedAudio.delete(request.cacheKey);
      reject(new DOMException('Speech generation cancelled.', 'AbortError'));
    };
    discardPrefetch = () => {
      if (!job.prefetch || !fishAudioQueue.includes(job)) return;
      if ([...prefetchOwners].every((owner) => owner?.aborted)) cancel();
    };
    job = {
      prefetch,
      start() {
        signal?.removeEventListener('abort', cancel);
        detachPrefetch();
        const background = job.prefetch;
        activeFishAudio += 1;
        if (background) activeFishAudioPrefetch += 1;
        // Hold the slot until the complete response body has been received.
        void fetchAudio(request, signal).then(resolve, reject).finally(() => {
          activeFishAudio -= 1;
          if (background) activeFishAudioPrefetch -= 1;
          pumpFishAudio();
        });
      },
    };
    if (signal?.aborted) return cancel();
    signal?.addEventListener('abort', cancel, { once: true });
    fishAudioQueue.push(job);
    pumpFishAudio();
  });
  transfer = {
    promise,
    prioritize() {
      if (!fishAudioQueue.includes(job)) return;
      job.prefetch = false;
      detachPrefetch();
      pumpFishAudio();
    },
    retainPrefetch(owner) {
      if (!job.prefetch || !fishAudioQueue.includes(job)) return;
      prefetchOwners.add(owner);
      owner?.addEventListener('abort', discardPrefetch, { once: true });
      discardPrefetch();
    },
  };
  return transfer;
}

function cachedAudio(key: string): Blob | undefined {
  return audioCache.get(key);
}

function cacheAudio(key: string, blob: Blob): void {
  if (blob.size === 0 || blob.size > maximumCachedAudioBytes) return;
  audioCache.set(key, blob);

  let cachedBytes = 0;
  for (const cached of audioCache.values()) cachedBytes += cached.size;
  for (const [oldestKey, oldest] of audioCache) {
    if (cachedBytes <= maximumCachedAudioBytes) break;
    audioCache.delete(oldestKey);
    cachedBytes -= oldest.size;
  }
}

// Exported so tests can clear completed audio between cases. In-flight requests
// remain shared until they settle.
export function clearTextToSpeechCache(): void {
  audioCache.clear();
}

async function fetchAudio(request: AudioRequest, signal?: AbortSignal): Promise<Blob> {
  for (let attempt = 0; ; attempt += 1) {
    const response = await fetch(request.url, {
      method: 'POST', headers: request.headers, body: request.body, signal,
    });
    if (response.ok) return response.blob();
    const error = await speechError(response, request.provider);
    if (request.provider !== 'FishAudio' || response.status !== 503
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

function cachedOrPendingAudio(request: AudioRequest, prefetch = false, signal?: AbortSignal): Promise<Blob> {
  if (prefetch && signal?.aborted) return Promise.reject(new DOMException('Speech generation cancelled.', 'AbortError'));
  const cached = cachedAudio(request.cacheKey);
  if (cached) return Promise.resolve(cached);

  const pending = pendingCachedAudio.get(request.cacheKey);
  if (pending) {
    if (!prefetch) pending.prioritize?.();
    else pending.retainPrefetch?.(signal);
    return pending.promise;
  }

  // Do not attach one caller's abort signal: prefetch and playback can share
  // this request, and a stopped playback should not discard the generated clip.
  const transfer = requestAudio(request, undefined, prefetch);
  transfer.promise = transfer.promise.then((blob) => {
    cacheAudio(request.cacheKey, blob);
    return blob;
  }).finally(() => {
    // An abandoned request must not remove its replacement from the map.
    if (pendingCachedAudio.get(request.cacheKey) === transfer) pendingCachedAudio.delete(request.cacheKey);
  });
  pendingCachedAudio.set(request.cacheKey, transfer);
  if (prefetch) transfer.retainPrefetch?.(signal);
  return transfer.promise;
}

export async function cacheTextToSpeech(
  configuration: TextToSpeechConfiguration,
  voice: TextToSpeechVoice | undefined,
  text: string,
  signal?: AbortSignal,
): Promise<void> {
  await cachedOrPendingAudio(audioRequest(configuration, voice, text), true, signal);
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
    private readonly options: { cache?: boolean } = {},
  ) {}

  async play(): Promise<void> {
    const request = audioRequest(this.configuration, this.voice, this.text);
    const blob = this.options.cache
      ? await cachedOrPendingAudio(request)
      : await requestAudio(request, this.request.signal).promise;
    if (this.stopped) return;

    this.objectUrl = URL.createObjectURL(blob);
    if (this.stopped) return this.releaseObjectUrl();
    const audio = new Audio(this.objectUrl);
    this.audio = audio;
    audio.addEventListener('ended', () => this.finish(), { once: true });
    audio.addEventListener('error', () => this.finish(), { once: true });
    await audio.play();
  }

  stop(): void {
    if (this.stopped) return;
    this.stopped = true;
    this.request.abort();
    this.audio?.pause();
    this.audio = null;
    this.releaseObjectUrl();
  }

  private finish(): void {
    if (this.stopped) return;
    this.stop();
    this.onEnded();
  }

  private releaseObjectUrl(): void {
    if (!this.objectUrl) return;
    URL.revokeObjectURL(this.objectUrl);
    this.objectUrl = null;
  }
}

async function speechError(response: Response, provider: string): Promise<TextToSpeechError> {
  const fallback = `${provider} request failed (HTTP ${response.status}).`;
  try {
    const parsed: unknown = await response.json();
    if (!parsed || typeof parsed !== 'object') return new TextToSpeechError(fallback);
    const root = parsed as Record<string, unknown>;
    const detailValue = root.detail;
    if (typeof detailValue === 'string') {
      return new TextToSpeechError(detailValue.trim()
        ? `${provider}: ${detailValue.trim()} (HTTP ${response.status})`
        : fallback);
    }
    const nested = detailValue ?? root.error;
    const detail = nested && typeof nested === 'object'
      ? nested as Record<string, unknown> : root;
    const message = typeof detail.message === 'string' ? detail.message.trim() : '';
    return new TextToSpeechError(
      message ? `${provider}: ${message} (HTTP ${response.status})` : fallback,
      typeof detail.code === 'string' ? detail.code : undefined,
    );
  } catch {
    return new TextToSpeechError(fallback);
  }
}
