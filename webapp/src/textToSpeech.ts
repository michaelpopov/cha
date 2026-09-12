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

interface AudioRequest {
  url: string;
  body: string;
  cacheKey: string;
  apiKey: string;
}

const pendingCachedAudio = new Map<string, Promise<Blob>>();

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

function audioRequest(
  configuration: TextToSpeechConfiguration,
  voice: TextToSpeechVoice | undefined,
  text: string,
): AudioRequest {
  const voiceId = voice?.elevenlabs_voice_id ?? configuration.voiceId;
  const url = `${configuration.baseUrl}/${encodeURIComponent(voiceId)}`
    + `?output_format=${encodeURIComponent(configuration.outputFormat)}`;
  const body: {
    text: string;
    model_id: string;
    voice_settings?: TextToSpeechVoice['settings'];
  } = {
    text,
    model_id: configuration.model,
  };
  if (voice && Object.keys(voice.settings).length > 0) {
    body.voice_settings = voice.settings;
  }
  const requestBody = JSON.stringify(body);
  return {
    url,
    body: requestBody,
    cacheKey: `${url}\n${requestBody}`,
    apiKey: configuration.apiKey,
  };
}

async function fetchAudio(request: AudioRequest, signal?: AbortSignal): Promise<Blob> {
  const response = await fetch(request.url, {
    method: 'POST',
    headers: {
      Accept: 'audio/mpeg',
      'Content-Type': 'application/json',
      'xi-api-key': request.apiKey,
    },
    body: request.body,
    signal,
  });
  if (!response.ok) {
    throw new TextToSpeechError(await elevenLabsErrorMessage(response));
  }
  return response.blob();
}

function cachedOrPendingAudio(request: AudioRequest): Promise<Blob> {
  const cached = cachedAudio(request.cacheKey);
  if (cached) return Promise.resolve(cached);

  const pending = pendingCachedAudio.get(request.cacheKey);
  if (pending) return pending;

  // Do not attach one caller's abort signal: prefetch and playback can share
  // this request, and a stopped playback should not discard the generated clip.
  const promise = fetchAudio(request).then((blob) => {
    cacheAudio(request.cacheKey, blob);
    return blob;
  }).finally(() => pendingCachedAudio.delete(request.cacheKey));
  pendingCachedAudio.set(request.cacheKey, promise);
  return promise;
}

export async function cacheTextToSpeech(
  configuration: TextToSpeechConfiguration,
  voice: TextToSpeechVoice | undefined,
  text: string,
): Promise<void> {
  await cachedOrPendingAudio(audioRequest(configuration, voice, text));
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
      : await fetchAudio(request, this.request.signal);
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
