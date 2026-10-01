import { useEffect, useState } from 'react';
import CodecParser from 'codec-parser';
import MSEAudioWrapper from 'mse-audio-wrapper';

import type { ChaClient, VoiceUpdate } from './api/client';
import { beginSpeechPlayback } from './speechPlayback';

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
  ): Promise<{ url: string; resource_id: string; streaming?: boolean }>;
  release(resourceId: string): Promise<void>;
}

export async function fetchLocalResource(url: string, signal?: AbortSignal): Promise<Blob> {
  const response = await fetch(url, { signal, cache: 'no-store' });
  if (!response.ok) {
    throw await speechError(response, url.includes('/media/') ? 'Audio' : 'Cached audio');
  }
  return response.blob();
}

function waitForAudioEvent(target: EventTarget, name: string, signal: AbortSignal,
  start?: () => void): Promise<void> {
  return new Promise((resolve, reject) => {
    const cleanup = () => {
      target.removeEventListener(name, ready);
      target.removeEventListener('error', failed);
      signal.removeEventListener('abort', aborted);
    };
    const ready = () => { cleanup(); resolve(); };
    const failed = () => { cleanup(); reject(new TextToSpeechError('Audio could not be played.')); };
    const aborted = () => { cleanup(); reject(new DOMException('Aborted', 'AbortError')); };
    target.addEventListener(name, ready, { once: true });
    target.addEventListener('error', failed, { once: true });
    signal.addEventListener('abort', aborted, { once: true });
    if (signal.aborted) { aborted(); return; }
    try { start?.(); } catch (error) { cleanup(); reject(error); }
  });
}

function waitForAudio(signal: AbortSignal): Promise<void> {
  return new Promise((resolve, reject) => {
    const aborted = () => {
      clearTimeout(timer);
      signal.removeEventListener('abort', aborted);
      reject(new DOMException('Aborted', 'AbortError'));
    };
    const timer = setTimeout(() => { signal.removeEventListener('abort', aborted); resolve(); }, 100);
    signal.addEventListener('abort', aborted, { once: true });
    if (signal.aborted) aborted();
  });
}

async function* readAudioChunks(url: string, signal: AbortSignal) {
  let offset = 0;
  for (;;) {
    const response = await fetch(url, { signal, cache: 'no-store',
      headers: { 'X-CHA-Audio-Offset': String(offset) } });
    if (response.status === 204) { await waitForAudio(signal); continue; }
    if (response.status === 502) throw new TextToSpeechError('Audio generation failed. Try again.');
    if (!response.ok) throw await speechError(response, 'Audio');
    const complete = response.headers.get('X-CHA-Audio-Complete');
    if (complete !== '0' && complete !== '1') throw new TextToSpeechError('Invalid audio stream.');
    const bytes = await response.arrayBuffer();
    offset += bytes.byteLength;
    yield { bytes, type: response.headers.get('Content-Type') ?? '', complete: complete === '1' };
    if (complete === '1') return;
    if (bytes.byteLength === 0) await waitForAudio(signal);
  }
}

export class TextToSpeechSession {
  private readonly request = new AbortController();
  private audio: HTMLAudioElement | null = null;
  private objectUrl: string | null = null;
  private stopped = false;
  private nativeResourceId: string | null = null;
  private endPlayback: (() => void) | null = null;
  private playbackWaitTimer: ReturnType<typeof setTimeout> | undefined;

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
    private readonly options?: { streaming?: boolean; onError?(error: Error): void; audio?: HTMLAudioElement },
  ) {}

  async play(): Promise<void> {
    let url = this.cachedUrl;
    let streaming = this.options?.streaming ?? false;
    if (!url && this.nativeSpeech) {
      const resource = await this.nativeSpeech.preview(this.text, this.voice, this.request.signal);
      this.nativeResourceId = resource.resource_id;
      if (this.stopped) { this.releaseNative(); return; }
      url = resource.url;
      streaming = resource.streaming ?? false;
    }
    if (!url) throw new TextToSpeechError('Voice output is unavailable.');
    if (streaming) return this.playStream(url);
    const blob = await fetchLocalResource(url, this.request.signal);
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
    await this.startAudio(this.createAudio(this.objectUrl));
  }

  private async startAudio(audio: HTMLAudioElement): Promise<void> {
    if (this.stopped) return;
    // Mute input before play(), so the first spoken samples cannot enter dictation.
    this.endPlayback ??= beginSpeechPlayback();
    try {
      await audio.play();
    } catch (error) {
      this.stop();
      throw error;
    }
  }

  private createAudio(url: string, managed = false): HTMLAudioElement {
    const audio = this.options?.audio ?? new Audio();
    // Safari needs this before attaching a ManagedMediaSource without an AirPlay URL.
    if (managed) audio.disableRemotePlayback = true;
    audio.src = url;
    this.audio = audio;
    const signal = this.request.signal;
    audio.addEventListener('ended', () => this.finish(true), { once: true, signal });
    audio.addEventListener('pause', () => {
      // Media controls can pause the clip without ending it or advancing the queue.
      this.clearPlaybackWait();
      this.releaseInput();
    }, { signal });
    audio.addEventListener('play', () => {
      if (!this.stopped) this.endPlayback ??= beginSpeechPlayback();
    }, { signal });
    audio.addEventListener('playing', () => this.clearPlaybackWait(), { signal });
    audio.addEventListener('waiting', () => {
      if (this.stopped || audio.paused || this.playbackWaitTimer !== undefined) return;
      this.playbackWaitTimer = setTimeout(() => {
        this.options?.onError?.(new TextToSpeechError('Audio playback stalled. Try again.'));
        this.finish();
      }, 10_000);
    }, { signal });
    audio.addEventListener('error', () => {
      if (this.stopped) return;
      this.options?.onError?.(new TextToSpeechError('Audio could not be played.'));
      this.finish(true);
    }, { once: true, signal });
    if (this.playback && this.playback.position > 0) {
      const position = this.playback.position;
      const resume = () => {
        if (this.stopped) return;
        audio.currentTime = Number.isFinite(audio.duration) && position >= audio.duration ? 0 : position;
      };
      if (audio.readyState >= 1) resume();
      else audio.addEventListener('loadedmetadata', resume, { once: true, signal });
    }
    return audio;
  }

  private async playStream(url: string): Promise<void> {
    const signal = this.request.signal;
    const chunks = readAudioChunks(url, signal);
    const first = await chunks.next();
    if (first.done || this.stopped) return;
    const mime = first.value.type.split(';')[0].trim().toLowerCase();
    const StreamingSource = (globalThis as typeof globalThis & {
      ManagedMediaSource?: typeof MediaSource;
    }).ManagedMediaSource ?? globalThis.MediaSource;
    const mp4Mime = 'audio/mp4;codecs="mp3"';
    const rawMp3 = mime === 'audio/mpeg' && StreamingSource?.isTypeSupported(mime);
    const wrappedMp3 = mime === 'audio/mpeg' && !rawMp3 && StreamingSource?.isTypeSupported(mp4Mime);
    if ((!rawMp3 && !wrappedMp3) || (this.playback?.position ?? 0) > 0) {
      const parts = [first.value.bytes];
      for await (const chunk of chunks) parts.push(chunk.bytes);
      if (this.stopped) return;
      this.onCached?.();
      this.objectUrl = URL.createObjectURL(new Blob(parts, { type: first.value.type }));
      await this.startAudio(this.createAudio(this.objectUrl));
      return;
    }
    const source = new StreamingSource() as MediaSource & { readonly streaming?: boolean };
    const opened = waitForAudioEvent(source, 'sourceopen', signal);
    this.objectUrl = URL.createObjectURL(source);
    const audio = this.createAudio(this.objectUrl, 'streaming' in source);
    await opened;
    const buffer = source.addSourceBuffer(wrappedMp3 ? mp4Mime : mime);
    if (wrappedMp3) buffer.mode = 'sequence';
    // Firefox accepts MP3 frames inside MP4, but not the raw MP3 byte stream.
    const parser = wrappedMp3 ? new CodecParser(mime, { enableFrameCRC32: false }) : null;
    const wrapper = wrappedMp3 ? new MSEAudioWrapper(mime, {
      codec: 'mpeg', minFramesPerSegment: 1, minBytesPerSegment: 1,
    }) : null;
    const append = async (bytes: ArrayBuffer) => {
      if (bytes.byteLength === 0) return;
      // ManagedMediaSource lets Safari request data only when it needs more audio.
      while (source.streaming === false
        || (buffer.buffered.length && buffer.buffered.end(buffer.buffered.length - 1) - audio.currentTime > 30)) {
        await waitForAudio(signal);
      }
      if (buffer.buffered.length && audio.currentTime - buffer.buffered.start(0) > 20) {
        await waitForAudioEvent(buffer, 'updateend', signal, () => buffer.remove(0, audio.currentTime - 10));
      }
      await waitForAudioEvent(buffer, 'updateend', signal, () => buffer.appendBuffer(bytes));
    };
    const appendFrames = async (frames: IterableIterator<unknown>) => {
      for (const segment of wrapper!.iterator([...frames])) await append(segment.buffer);
    };
    const appendChunk = async (bytes: ArrayBuffer) => {
      if (parser) await appendFrames(parser.parseChunk(new Uint8Array(bytes)));
      else await append(bytes);
    };
    const transfer = (async () => {
      await appendChunk(first.value.bytes);
      for await (const chunk of chunks) await appendChunk(chunk.bytes);
      if (parser) await appendFrames(parser.flush());
      if (this.stopped) return;
      source.endOfStream();
      this.onCached?.();
    })();
    // Resolve when playback starts; later transfer failures still reach the screen.
    await new Promise<void>((resolve, reject) => {
      let started = false;
      void transfer.catch((error: unknown) => {
        if (!started) { this.stop(); reject(error); return; }
        if (this.stopped) return;
        this.options?.onError?.(error instanceof Error ? error : new TextToSpeechError('Audio generation failed.'));
        this.finish(true);
      });
      void this.startAudio(audio).then(() => { started = true; resolve(); }, reject);
    });
  }

  stop(resetPosition = false): void {
    if (this.stopped) return;
    this.stopped = true;
    this.clearPlaybackWait();
    this.request.abort();
    this.audio?.pause();
    if (this.playback && this.audio) {
      if (resetPosition) this.playback.onPositionChange(0);
      else if (this.audio.readyState >= 1 && Number.isFinite(this.audio.currentTime)) {
        this.playback.onPositionChange(this.audio.currentTime);
      }
    }
    this.audio?.removeAttribute('src');
    this.audio?.load();
    this.audio = null;
    this.releaseInput();
    this.releaseObjectUrl();
    this.releaseNative();
    this.onDispose?.();
  }

  private releaseNative(): void {
    const id = this.nativeResourceId;
    this.nativeResourceId = null;
    if (id && this.nativeSpeech) void this.nativeSpeech.release(id);
  }

  private releaseInput(): void {
    this.endPlayback?.();
    this.endPlayback = null;
  }

  private clearPlaybackWait(): void {
    if (this.playbackWaitTimer === undefined) return;
    clearTimeout(this.playbackWaitTimer);
    this.playbackWaitTimer = undefined;
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
