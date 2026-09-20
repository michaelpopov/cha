import { useEffect, useState } from 'react';

import type { ChaClient, VoiceUpdate } from './api/client';

export function nativeSpeechFromClient(client: ChaClient): NativeSpeech {
  return {
    preview(text, voice, signal) {
      return client.previewSpeech(
        text,
        voice?.elevenlabs_voice_id,
        voice?.settings,
        signal,
      );
    },
    release(resourceId) {
      return client.releaseResource(resourceId);
    },
  };
}

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
  constructor(message: string, readonly code?: string, readonly status?: number) {
    super(message);
    this.name = 'TextToSpeechError';
  }
}

export interface NativeSpeech {
  preview(
    text: string,
    voice: TextToSpeechVoice | undefined,
    signal: AbortSignal,
  ): Promise<{ url: string; resource_id: string }>;
  release(resourceId: string): Promise<void>;
}

export async function fetchLocalResource(url: string, signal?: AbortSignal): Promise<Blob> {
  const response = await fetch(url, { signal, cache: 'no-store' });
  if (!response.ok) {
    throw await speechError(response, url.includes('/media/') ? 'Audio' : 'Cached audio');
  }
  return response.blob();
}

export class TextToSpeechSession {
  private readonly request = new AbortController();
  private audio: HTMLAudioElement | null = null;
  private objectUrl: string | null = null;
  private stopped = false;
  private nativeResourceId: string | null = null;

  constructor(
    _configuration: TextToSpeechConfiguration | null,
    private readonly voice: TextToSpeechVoice | undefined,
    private readonly text: string,
    private readonly onEnded: () => void,
    private readonly playback?: {
      position: number;
      onPositionChange(position: number): void;
    },
    private readonly cachedUrl?: string,
    private readonly onCached?: () => void,
    private readonly nativeSpeech?: NativeSpeech,
    private readonly onDispose?: () => void,
  ) {}

  async play(): Promise<void> {
    let blob: Blob;
    if (this.cachedUrl) {
      blob = await fetchLocalResource(this.cachedUrl, this.request.signal);
    } else if (this.nativeSpeech) {
      const resource = await this.nativeSpeech.preview(
        this.text, this.voice, this.request.signal,
      );
      this.nativeResourceId = resource.resource_id;
      if (this.stopped) {
        this.releaseNative();
        return;
      }
      blob = await fetchLocalResource(resource.url, this.request.signal);
    } else throw new TextToSpeechError('Voice output is unavailable.');
    if (this.stopped) {
      this.releaseNative();
      return;
    }
    if (this.cachedUrl) this.onCached?.();

    this.objectUrl = URL.createObjectURL(blob);
    if (this.stopped) {
      this.releaseObjectUrl();
      this.releaseNative();
      this.onDispose?.();
      return;
    }
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
    this.releaseNative();
    this.onDispose?.();
  }

  private releaseNative(): void {
    const id = this.nativeResourceId;
    this.nativeResourceId = null;
    if (id && this.nativeSpeech) void this.nativeSpeech.release(id);
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

async function speechError(response: Response, source = 'FishAudio'): Promise<TextToSpeechError> {
  const fallback = `${source} request failed (HTTP ${response.status}).`;
  try {
    const parsed: unknown = await response.json();
    if (!parsed || typeof parsed !== 'object') return new TextToSpeechError(fallback, undefined, response.status);
    const root = parsed as Record<string, unknown>;
    const nested = root.error;
    const detail = nested && typeof nested === 'object'
      ? nested as Record<string, unknown> : root;
    const message = typeof detail.message === 'string' ? detail.message.trim() : '';
    return new TextToSpeechError(
      message ? `${source}: ${message} (HTTP ${response.status})` : fallback,
      typeof detail.code === 'string' ? detail.code : undefined,
      response.status,
    );
  } catch {
    return new TextToSpeechError(fallback, undefined, response.status);
  }
}
