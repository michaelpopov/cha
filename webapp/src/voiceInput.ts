export interface VoiceInputConfiguration {
  url: string;
  apiKey: string;
  model: string;
  languages?: string[];
  keywords?: string[];
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

async function connect(
  configuration: VoiceInputConfiguration,
  peer: RTCPeerConnection,
): Promise<void> {
  const offer = await peer.createOffer();
  await peer.setLocalDescription(offer);
  if (!offer.sdp) throw new Error('Voice input could not create an audio connection.');

  const body = new FormData();
  body.set('sdp', offer.sdp);
  body.set('session', JSON.stringify({
    type: 'transcription',
    audio: {
      input: {
        transcription: {
          model: configuration.model,
          languages: configuration.languages,
          keywords: configuration.keywords,
          delay: 'low',
        },
        turn_detection: null,
      },
    },
  }));
  const response = await fetch(configuration.url, {
    method: 'POST',
    headers: { Authorization: `Bearer ${configuration.apiKey}` },
    body,
  });
  if (!response.ok) throw new Error('The realtime transcription request failed.');
  await peer.setRemoteDescription({ type: 'answer', sdp: await response.text() });
}

export class VoiceInputSession {
  private readonly opened: Promise<void>;
  private readonly completed: Promise<void>;
  private openSession: (() => void) | null = null;
  private rejectSession: ((failure: unknown) => void) | null = null;
  private completeSession: (() => void) | null = null;
  private failure: unknown = null;
  private stopping = false;
  private cancelled = false;
  private transcriptCompleted = false;
  private transcript = '';

  private constructor(
    private readonly stream: MediaStream,
    private readonly peer: RTCPeerConnection,
    private readonly events: RTCDataChannel,
    private readonly onTranscription: (text: string) => void,
    private readonly onFailure: (failure: unknown) => void,
  ) {
    this.opened = new Promise((resolve, reject) => {
      this.openSession = resolve;
      this.rejectSession = reject;
    });
    this.completed = new Promise((resolve) => { this.completeSession = resolve; });
    events.addEventListener('open', () => {
      this.openSession?.();
      this.openSession = null;
      this.rejectSession = null;
    });
    events.addEventListener('message', ({ data }) => this.handleEvent(data));
    events.addEventListener('error', () => {
      this.fail(new Error('The realtime transcription connection failed.'));
    });
    events.addEventListener('close', () => {
      if (!this.cancelled && !this.transcriptCompleted) {
        this.fail(new Error('The realtime transcription connection closed.'));
      }
    });
    peer.addEventListener('connectionstatechange', () => {
      if (peer.connectionState === 'failed') {
        this.fail(new Error('The realtime transcription connection failed.'));
      }
    });
  }

  static supported(): boolean {
    return typeof RTCPeerConnection !== 'undefined'
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
    let session: VoiceInputSession | null = null;
    try {
      const peer = new RTCPeerConnection();
      for (const track of stream.getAudioTracks()) peer.addTrack(track, stream);
      const events = peer.createDataChannel('oai-events');
      session = new VoiceInputSession(
        stream, peer, events, onTranscription, onFailure,
      );
      await Promise.all([connect(configuration, peer), session.opened]);
      return session;
    } catch (failure) {
      if (session) session.cancel();
      else for (const track of stream.getTracks()) track.stop();
      throw failure;
    }
  }

  async stop(): Promise<void> {
    if (!this.stopping) {
      this.stopping = true;
      this.stopTracks();
      try {
        this.events.send(JSON.stringify({ type: 'input_audio_buffer.commit' }));
      } catch (failure) {
        this.fail(failure);
      }
    }
    await this.completed;
    this.closeTransport();
    if (this.failure) throw this.failure;
  }

  cancel(): void {
    if (this.cancelled) return;
    this.cancelled = true;
    this.stopping = true;
    this.stopTracks();
    this.transcriptCompleted = true;
    this.completeSession?.();
    this.completeSession = null;
    this.openSession?.();
    this.openSession = null;
    this.rejectSession = null;
    this.closeTransport();
  }

  private handleEvent(data: unknown): void {
    if (this.cancelled || typeof data !== 'string') return;
    let event: unknown;
    try {
      event = JSON.parse(data);
    } catch {
      return this.fail(new Error('The realtime transcription response was invalid.'));
    }
    if (!event || typeof event !== 'object' || !('type' in event)) return;
    if (event.type === 'conversation.item.input_audio_transcription.delta'
        && 'delta' in event && typeof event.delta === 'string') {
      this.transcript += event.delta;
      this.onTranscription(event.delta);
    } else if (event.type === 'conversation.item.input_audio_transcription.completed') {
      if ('transcript' in event && typeof event.transcript === 'string'
          && event.transcript.startsWith(this.transcript)) {
        const remainder = event.transcript.slice(this.transcript.length);
        if (remainder) this.onTranscription(remainder);
      }
      this.transcriptCompleted = true;
      this.completeSession?.();
      this.completeSession = null;
    } else if (event.type === 'error') {
      const message = 'error' in event && event.error && typeof event.error === 'object'
        && 'message' in event.error && typeof event.error.message === 'string'
        ? event.error.message
        : 'Realtime transcription failed.';
      this.fail(new Error(message));
    }
  }

  private fail(failure: unknown): void {
    if (this.failure || this.cancelled) return;
    this.failure = failure;
    this.stopping = true;
    this.stopTracks();
    this.completeSession?.();
    this.completeSession = null;
    this.rejectSession?.(failure);
    this.rejectSession = null;
    this.closeTransport();
    this.onFailure(failure);
  }

  private closeTransport(): void {
    if (this.events.readyState !== 'closed') this.events.close();
    if (this.peer.connectionState !== 'closed') this.peer.close();
  }

  private stopTracks(): void {
    for (const track of this.stream.getTracks()) track.stop();
  }
}
