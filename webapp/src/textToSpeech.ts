export interface TextToSpeechConfiguration {
  baseUrl: string;
  voiceId: string;
  outputFormat: string;
  apiKey: string;
  model: string;
}

export interface TextToSpeechVoice {
  elevenlabs_voice_id: string;
  settings: {
    stability?: number;
    similarity_boost?: number;
    style?: number;
    use_speaker_boost?: boolean;
    speed?: number;
  };
}

declare global {
  interface Window {
    chaTextToSpeech?: TextToSpeechConfiguration;
  }
}

export function getTextToSpeechConfiguration(): TextToSpeechConfiguration | null {
  const configuration = window.chaTextToSpeech;
  return configuration
      && typeof configuration.baseUrl === 'string' && configuration.baseUrl.length > 0
      && typeof configuration.voiceId === 'string' && configuration.voiceId.length > 0
      && typeof configuration.outputFormat === 'string' && configuration.outputFormat.length > 0
      && typeof configuration.apiKey === 'string' && configuration.apiKey.length > 0
      && typeof configuration.model === 'string' && configuration.model.length > 0
    ? configuration
    : null;
}

export class TextToSpeechError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'TextToSpeechError';
  }
}

const maximumCachedAudioBytes = 256 * 1024 * 1024;
const audioCache = new Map<string, Blob>();

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

// Exported so tests can isolate this app-lifetime module cache.
export function clearTextToSpeechCache(): void {
  audioCache.clear();
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
    const voiceId = this.voice?.elevenlabs_voice_id ?? this.configuration.voiceId;
    const url = `${this.configuration.baseUrl}/${encodeURIComponent(voiceId)}`
      + `?output_format=${encodeURIComponent(this.configuration.outputFormat)}`;
    const body: {
      text: string;
      model_id: string;
      voice_settings?: TextToSpeechVoice['settings'];
    } = {
      text: this.text,
      model_id: this.configuration.model,
    };
    if (this.voice && Object.keys(this.voice.settings).length > 0) {
      body.voice_settings = this.voice.settings;
    }
    const requestBody = JSON.stringify(body);
    const cacheKey = `${url}\n${requestBody}`;
    let blob = this.options.cache ? cachedAudio(cacheKey) : undefined;
    if (!blob) {
      const response = await fetch(url, {
        method: 'POST',
        headers: {
          Accept: 'audio/mpeg',
          'Content-Type': 'application/json',
          'xi-api-key': this.configuration.apiKey,
        },
        body: requestBody,
        signal: this.request.signal,
      });
      if (!response.ok) {
        throw new TextToSpeechError(await elevenLabsErrorMessage(response));
      }
      if (this.stopped) return;

      blob = await response.blob();
      if (this.stopped) return;
      if (this.options.cache) cacheAudio(cacheKey, blob);
    }
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

async function elevenLabsErrorMessage(response: Response): Promise<string> {
  const fallback = `ElevenLabs request failed (HTTP ${response.status}).`;
  try {
    const parsed: unknown = await response.json();
    if (!parsed || typeof parsed !== 'object') return fallback;
    const root = parsed as Record<string, unknown>;
    const detailValue = root.detail;
    if (typeof detailValue === 'string') {
      return detailValue.trim()
        ? `ElevenLabs: ${detailValue.trim()} (HTTP ${response.status})`
        : fallback;
    }
    const detail = detailValue && typeof detailValue === 'object'
      ? detailValue as Record<string, unknown>
      : root;
    const message = typeof detail.message === 'string' ? detail.message.trim() : '';
    return message ? `ElevenLabs: ${message} (HTTP ${response.status})` : fallback;
  } catch {
    return fallback;
  }
}
