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
    const response = await fetch(url, {
      method: 'POST',
      headers: {
        Accept: 'audio/mpeg',
        'Content-Type': 'application/json',
        'xi-api-key': this.configuration.apiKey,
      },
      body: JSON.stringify(body),
      signal: this.request.signal,
    });
    if (!response.ok) throw new Error('The speech request failed.');
    if (this.stopped) return;

    this.objectUrl = URL.createObjectURL(await response.blob());
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
