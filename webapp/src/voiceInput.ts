export interface VoiceInputConfiguration {
  url: string;
  apiKey: string;
  model: string;
  languages?: string[];
  keywords?: string[];
  blockDurationMs: number;
}

declare global {
  interface Window {
    chaVoiceInput?: VoiceInputConfiguration;
  }
}

export function getVoiceInputConfiguration(): VoiceInputConfiguration | null {
  const configuration = window.chaVoiceInput;
  return configuration
      && typeof configuration.url === 'string' && configuration.url.length > 0
      && typeof configuration.apiKey === 'string' && configuration.apiKey.length > 0
      && typeof configuration.model === 'string' && configuration.model.length > 0
      && isStringArray(configuration.languages)
      && isStringArray(configuration.keywords)
      && Number.isInteger(configuration.blockDurationMs)
      && configuration.blockDurationMs > 0
    ? configuration
    : null;
}

function isStringArray(value: unknown): value is string[] | undefined {
  return value === undefined
    || (Array.isArray(value)
      && value.every((item) => typeof item === 'string' && item.length > 0));
}

export function appendTranscription(current: string, transcription: string): string {
  const addition = normalizeDictationCommands(transcription).trim();
  if (!addition) return current;
  if (!current || /\s$/.test(current) || /^[,.;:!?)}\]]/.test(addition)) {
    return current + addition;
  }
  return `${current} ${addition}`;
}

const wordEdge = String.raw`[\p{L}\p{N}_]`;
const punctuation = String.raw`[,.!?;:]`;

const dictationCommands = [
  {
    phrase: 'exclamation (?:mark|point|sign)|восклицательный знак|знак восклицания',
    replacement: '!',
  },
  {
    phrase: 'question mark|вопросительный знак|знак вопроса',
    replacement: '?',
  },
  { phrase: 'comma|запятая', replacement: ',' },
  { phrase: 'period|full stop|точка', replacement: '.' },
  { phrase: 'new line|новая строка|с новой строки', replacement: '\n' },
].map(({ phrase, replacement }) => ({
  pattern: new RegExp(
    `(?:${punctuation}[ \\t]*)?[ \\t]*(?<!${wordEdge})(?:${phrase})`
      + `(?!${wordEdge})(?:[ \\t]*${punctuation})?`,
    'giu',
  ),
  replacement,
}));

function normalizeDictationCommands(transcription: string): string {
  let result = transcription;
  for (const command of dictationCommands) {
    result = result.replace(command.pattern, (match, offset: number, source: string) => {
      if (command.replacement === '\n') return '\n';
      const next = source[offset + match.length];
      return next && !/\s/.test(next) ? `${command.replacement} ` : command.replacement;
    });
  }
  return result.replace(/[ \t]*\n[ \t]*/g, '\n');
}

function recordingMimeType(): string | undefined {
  return [
    'audio/mp4',
    'audio/webm;codecs=opus',
    'audio/webm',
    'audio/ogg;codecs=opus',
  ].find((type) => MediaRecorder.isTypeSupported(type));
}

function recordingExtension(type: string): string {
  if (type.includes('mp4')) return 'mp4';
  if (type.includes('ogg')) return 'ogg';
  return 'webm';
}

async function transcribe(
  configuration: VoiceInputConfiguration,
  audio: Blob,
  signal: AbortSignal,
): Promise<string> {
  const body = new FormData();
  body.append('file', audio, `voice.${recordingExtension(audio.type)}`);
  body.append('model', configuration.model);
  body.append('response_format', 'json');
  for (const language of configuration.languages ?? []) {
    body.append('languages[]', language);
  }
  for (const keyword of configuration.keywords ?? []) {
    body.append('keywords[]', keyword);
  }
  const response = await fetch(configuration.url, {
    method: 'POST',
    headers: { Authorization: `Bearer ${configuration.apiKey}` },
    body,
    signal,
  });
  if (!response.ok) throw new Error('The transcription request failed.');
  const result: unknown = await response.json();
  if (!result || typeof result !== 'object'
      || !('text' in result) || typeof result.text !== 'string') {
    throw new TypeError('The transcription response was invalid.');
  }
  return result.text;
}

// A fresh recorder for each block makes every upload a self-contained audio
// file. MediaRecorder time slices are pieces of one larger file and later
// pieces are not guaranteed to be independently decodable.
export class VoiceInputSession {
  private readonly abort = new AbortController();
  private readonly mimeType = recordingMimeType();
  private recorder: MediaRecorder | null = null;
  private blockDone: Promise<void> = Promise.resolve();
  private finishBlock: (() => void) | null = null;
  private timer: number | null = null;
  private pending: Promise<void> = Promise.resolve();
  private failure: unknown = null;
  private stopping = false;
  private cancelled = false;

  private constructor(
    private readonly configuration: VoiceInputConfiguration,
    private readonly stream: MediaStream,
    private readonly onTranscription: (text: string) => void,
    private readonly onFailure: (failure: unknown) => void,
  ) {}

  static supported(): boolean {
    return typeof MediaRecorder !== 'undefined'
      && typeof navigator.mediaDevices?.getUserMedia === 'function';
  }

  static async start(
    configuration: VoiceInputConfiguration,
    onTranscription: (text: string) => void,
    onFailure: (failure: unknown) => void,
  ): Promise<VoiceInputSession> {
    if (!VoiceInputSession.supported()) {
      throw new Error('Voice input is unavailable.');
    }
    const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
    const session = new VoiceInputSession(
      configuration, stream, onTranscription, onFailure,
    );
    try {
      session.startBlock();
      return session;
    } catch (failure) {
      session.stopTracks();
      throw failure;
    }
  }

  async stop(): Promise<void> {
    if (!this.stopping) {
      this.stopping = true;
      this.clearTimer();
      await this.stopBlock();
      this.stopTracks();
    } else {
      await this.blockDone;
    }
    await this.pending;
    if (this.failure) throw this.failure;
  }

  cancel(): void {
    if (this.cancelled) return;
    this.cancelled = true;
    this.stopping = true;
    this.clearTimer();
    this.abort.abort();
    if (this.recorder?.state === 'recording') this.recorder.stop();
    this.stopTracks();
  }

  private startBlock(): void {
    const chunks: Blob[] = [];
    const recorder = this.mimeType
      ? new MediaRecorder(this.stream, { mimeType: this.mimeType })
      : new MediaRecorder(this.stream);
    this.recorder = recorder;
    this.blockDone = new Promise((resolve) => { this.finishBlock = resolve; });
    recorder.ondataavailable = ({ data }) => {
      if (data.size > 0) chunks.push(data);
    };
    recorder.onerror = () => this.fail(new Error('Audio recording failed.'));
    recorder.onstop = () => {
      this.recorder = null;
      if (!this.cancelled && chunks.length > 0) {
        this.enqueue(new Blob(chunks, { type: recorder.mimeType || chunks[0].type }));
      }
      this.finishBlock?.();
      this.finishBlock = null;
    };
    recorder.start();
    this.timer = window.setTimeout(
      () => void this.rotateBlock(), this.configuration.blockDurationMs,
    );
  }

  private async rotateBlock(): Promise<void> {
    this.timer = null;
    try {
      await this.stopBlock();
      if (!this.stopping && !this.failure) this.startBlock();
    } catch (failure) {
      this.fail(failure);
    }
  }

  private async stopBlock(): Promise<void> {
    if (this.recorder?.state === 'recording') this.recorder.stop();
    await this.blockDone;
  }

  private enqueue(audio: Blob): void {
    // Start every upload as soon as its block closes. The small ordering chain
    // delays only editor updates, so a slower response cannot make recording
    // fall further and further behind while results still append in order.
    const request = transcribe(this.configuration, audio, this.abort.signal).then(
      (text) => ({ text } as const),
      (failure: unknown) => ({ failure } as const),
    );
    this.pending = this.pending
      .then(async () => {
        if (this.cancelled || this.failure) return;
        const result = await request;
        if ('failure' in result) throw result.failure;
        if (!this.cancelled) this.onTranscription(result.text);
      })
      .catch((failure: unknown) => this.fail(failure));
  }

  private fail(failure: unknown): void {
    if (this.failure || this.cancelled) return;
    this.failure = failure;
    this.stopping = true;
    this.clearTimer();
    this.abort.abort();
    if (this.recorder?.state === 'recording') this.recorder.stop();
    this.stopTracks();
    this.onFailure(failure);
  }

  private clearTimer(): void {
    if (this.timer === null) return;
    window.clearTimeout(this.timer);
    this.timer = null;
  }

  private stopTracks(): void {
    for (const track of this.stream.getTracks()) track.stop();
  }
}
