import type {
  VoiceInputConfiguration,
  VoiceInputConnect,
} from './voiceInput';

async function connect(
  configuration: VoiceInputConfiguration,
  peer: RTCPeerConnection,
  nativeConnect: VoiceInputConnect,
  signal: AbortSignal,
): Promise<void> {
  const offer = await peer.createOffer();
  if (signal.aborted) throw new DOMException('The operation was aborted.', 'AbortError');
  await peer.setLocalDescription(offer);
  if (signal.aborted) throw new DOMException('The operation was aborted.', 'AbortError');
  if (!offer.sdp) throw new Error('Voice input could not create an audio connection.');

  const answer = await nativeConnect(offer.sdp, configuration.languages ?? [], signal);
  if (signal.aborted) {
    throw new DOMException('The operation was aborted.', 'AbortError');
  }
  await peer.setRemoteDescription({ type: 'answer', sdp: answer });
}

export class OpenAiVoiceInputSession {
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
    nativeConnect: VoiceInputConnect,
    signal?: AbortSignal,
  ): Promise<OpenAiVoiceInputSession> {
    if (!OpenAiVoiceInputSession.supported()) {
      throw new Error('Voice input is unavailable.');
    }
    const setup = new AbortController();
    let stream: MediaStream | null = null;
    let session: OpenAiVoiceInputSession | null = null;
    const cancelSetup = () => {
      setup.abort();
      if (session) session.cancel();
      else for (const track of stream?.getTracks() ?? []) track.stop();
    };
    if (signal?.aborted) {
      throw new DOMException('The operation was aborted.', 'AbortError');
    }
    signal?.addEventListener('abort', cancelSetup, { once: true });
    try {
      stream = await navigator.mediaDevices.getUserMedia({ audio: true });
      if (signal?.aborted) {
        for (const track of stream.getTracks()) track.stop();
        throw new DOMException('The operation was aborted.', 'AbortError');
      }
      const peer = new RTCPeerConnection();
      for (const track of stream.getAudioTracks()) peer.addTrack(track, stream);
      const events = peer.createDataChannel('oai-events');
      session = new OpenAiVoiceInputSession(
        stream, peer, events, onTranscription, onFailure,
      );
      const previousCancel = session.cancel.bind(session);
      session.cancel = () => {
        setup.abort();
        previousCancel();
      };
      await Promise.all([
        connect(configuration, peer, nativeConnect, setup.signal),
        session.opened,
      ]);
      return session;
    } catch (failure) {
      setup.abort();
      if (session) session.cancel();
      else for (const track of stream?.getTracks() ?? []) track.stop();
      throw failure;
    } finally {
      signal?.removeEventListener('abort', cancelSetup);
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
