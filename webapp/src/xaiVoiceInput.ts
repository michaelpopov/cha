import { prepareDictationPiece } from './dictationText';
import captureUrl from './voiceInputCapture.worklet.js?url&no-inline';
import type {
  VoiceInputConfiguration,
  VoiceInputTransport,
  VoiceInputXaiBridge,
} from './voiceInput';

const captureSampleRate = 16000;
const maxWaitingBatches = 19;

function abortError(): DOMException {
  return new DOMException('The operation was aborted.', 'AbortError');
}

function stopTracks(stream: MediaStream | null): void {
  for (const track of stream?.getTracks() ?? []) track.stop();
}

function pcmBase64(samples: Int16Array): string {
  const bytes = new Uint8Array(samples.length * 2);
  for (let index = 0; index < samples.length; index += 1) {
    const value = samples[index] ?? 0;
    bytes[index * 2] = value & 0xff;
    bytes[index * 2 + 1] = (value >> 8) & 0xff;
  }
  let binary = '';
  for (let index = 0; index < bytes.length; index += 1) {
    binary += String.fromCharCode(bytes[index] ?? 0);
  }
  return btoa(binary);
}

class XaiVoiceInputSession implements VoiceInputTransport {
  private readonly live = new AbortController();
  private readonly pending: Int16Array[] = [];
  private source: MediaStreamAudioSourceNode | null = null;
  private waiting = 0;
  private sending = false;
  private stopping = false;
  private cancelled = false;
  private failed = false;
  private released = false;
  private captureStopped = false;
  private failure: unknown = null;
  private timedOut = false;
  private stopPromise: Promise<void> | null = null;
  private idle: (() => void) | null = null;
  private flushResolve: (() => void) | null = null;
  private lastCharacter = '';

  constructor(
    private readonly sessionId: string,
    private readonly stopBudgetMs: number,
    private readonly stream: MediaStream,
    private readonly context: AudioContext,
    private readonly worklet: AudioWorkletNode,
    private readonly onTranscription: (text: string) => void,
    private readonly onFailure: (failure: unknown) => void,
    private readonly xaiBridge: VoiceInputXaiBridge,
  ) {
    this.worklet.port.addEventListener('message', this.onWorkletMessage);
    this.worklet.port.start();
  }

  attachMicrophone(): void {
    this.source = this.context.createMediaStreamSource(this.stream);
    this.source.connect(this.worklet);
  }

  stop(): Promise<void> {
    if (this.cancelled) return Promise.resolve();
    if (this.stopPromise) return this.stopPromise;
    if (this.failed) return Promise.reject(this.failure);
    this.stopping = true;
    this.stopPromise = this.finish();
    return this.stopPromise;
  }

  cancel(): void {
    if (this.cancelled) return;
    this.cancelled = true;
    this.live.abort();
    this.pending.length = 0;
    this.captureStopped = true;
    this.release();
    void this.xaiBridge.cancel(this.sessionId).catch(() => undefined);
    this.notifyIdle();
    this.flushResolve?.();
    this.flushResolve = null;
  }

  private readonly onWorkletMessage = (event: MessageEvent): void => {
    const data: unknown = event.data;
    if (!data || typeof data !== 'object' || !('type' in data)) return;
    if (data.type === 'flushed') {
      this.captureStopped = true;
      this.flushResolve?.();
      this.flushResolve = null;
      return;
    }
    if (data.type !== 'batch' && data.type !== 'flush-batch') return;
    if (this.captureStopped || this.cancelled || this.failed) return;
    if (!('samples' in data) || !(data.samples instanceof Int16Array) || data.samples.length === 0) {
      return;
    }
    this.acceptBatch(data.samples, data.type === 'flush-batch');
  };

  // Nineteen completed batches may wait, including the in-flight request.
  // The final flush batch may use one extra slot.
  private acceptBatch(samples: Int16Array, flush: boolean): void {
    const limit = this.stopping && flush ? maxWaitingBatches + 1 : maxWaitingBatches;
    if (this.waiting >= limit) {
      this.fail(new Error('Voice input audio overflowed.'));
      return;
    }
    this.waiting += 1;
    this.pending.push(new Int16Array(samples));
    this.sendNext();
  }

  private sendNext(): void {
    if (this.sending || this.pending.length === 0 || this.cancelled || this.failed) return;
    const samples = this.pending.shift();
    if (!samples) return;
    this.sending = true;
    void this.xaiBridge.audio(this.sessionId, pcmBase64(samples), this.live.signal).then(
      (reply) => {
        this.sending = false;
        this.waiting -= 1;
        if (!this.cancelled && !this.failed && reply.session_id === this.sessionId) {
          this.deliver(reply.pieces);
        }
        this.sendNext();
        this.notifyIdle();
      },
      (failure: unknown) => {
        this.sending = false;
        if (this.cancelled || this.failed || (this.stopping && this.live.signal.aborted)) {
          this.notifyIdle();
          return;
        }
        this.fail(failure);
      },
    );
  }

  private deliver(pieces: readonly string[]): void {
    if (this.cancelled || this.failed) return;
    for (const piece of pieces) {
      if (this.cancelled) return;
      const text = prepareDictationPiece(piece, this.lastCharacter);
      if (!text) continue;
      this.lastCharacter = text[text.length - 1] ?? '';
      this.onTranscription(text);
    }
  }

  private notifyIdle(): void {
    if (this.sending || this.pending.length > 0) return;
    const notify = this.idle;
    this.idle = null;
    notify?.();
  }

  private drain(): Promise<void> {
    if (!this.sending && this.pending.length === 0) return Promise.resolve();
    return new Promise((resolve) => { this.idle = resolve; });
  }

  private flushWorklet(): Promise<void> {
    if (this.captureStopped) return Promise.resolve();
    return new Promise((resolve) => {
      this.flushResolve = resolve;
      this.worklet.port.postMessage({ type: 'flush' });
    });
  }

  private async flushDrainAndStop(started: number): Promise<void> {
    await this.flushWorklet();
    if (this.cancelled) return;
    if (this.timedOut) throw new Error('Voice input timed out.');
    if (this.source && this.context.state !== 'closed') this.source.disconnect();
    this.source = null;
    stopTracks(this.stream);
    while ((this.sending || this.pending.length > 0) && !this.failed && !this.cancelled && !this.timedOut) {
      await this.drain();
    }
    if (this.cancelled) return;
    if (this.timedOut) throw new Error('Voice input timed out.');
    if (this.failed) throw this.failure ?? new Error('Voice input failed.');
    const remaining = Math.max(0, Math.floor(this.stopBudgetMs - (Date.now() - started)));
    if (this.live.signal.aborted || remaining <= 0) {
      throw new Error('Voice input timed out.');
    }
    const reply = await this.xaiBridge.stop(this.sessionId, remaining, this.live.signal);
    if (this.cancelled || this.failed) return;
    if (reply.session_id === this.sessionId) this.deliver(reply.pieces);
  }

  private async finish(): Promise<void> {
    const started = Date.now();
    let timer: ReturnType<typeof setTimeout> | undefined;
    const work = this.flushDrainAndStop(started);
    void work.catch(() => undefined);
    try {
      await Promise.race([
        work,
        new Promise<never>((_resolve, reject) => {
          timer = setTimeout(() => {
            this.timedOut = true;
            this.live.abort();
            reject(new Error('Voice input timed out.'));
          }, Math.max(0, this.stopBudgetMs));
        }),
      ]);
    } catch (failure) {
      if (this.cancelled) return;
      const error = this.timedOut ? new Error('Voice input timed out.') : failure;
      if (!this.failed) this.fail(error);
      throw error;
    } finally {
      if (timer !== undefined) clearTimeout(timer);
      this.release();
    }
  }

  private fail(failure: unknown): void {
    if (this.failed || this.cancelled) {
      this.notifyIdle();
      return;
    }
    this.failed = true;
    this.failure = failure;
    this.live.abort();
    this.pending.length = 0;
    this.captureStopped = true;
    this.release();
    void this.xaiBridge.cancel(this.sessionId).catch(() => undefined);
    this.notifyIdle();
    this.flushResolve?.();
    this.flushResolve = null;
    this.onFailure(failure);
  }

  private release(): void {
    if (this.released) return;
    this.released = true;
    this.captureStopped = true;
    this.worklet.port.removeEventListener('message', this.onWorkletMessage);
    stopTracks(this.stream);
    void this.context.close().catch(() => undefined);
  }
}

export async function startXaiVoiceInput(
  configuration: VoiceInputConfiguration,
  onTranscription: (text: string) => void,
  onFailure: (failure: unknown) => void,
  xaiBridge: VoiceInputXaiBridge,
  signal?: AbortSignal,
): Promise<VoiceInputTransport> {
  if (signal?.aborted) throw abortError();
  if (typeof AudioContext === 'undefined' || typeof AudioWorkletNode === 'undefined'
      || typeof navigator.mediaDevices?.getUserMedia !== 'function') {
    throw new Error('Voice input is unavailable.');
  }
  const sessionId = crypto.randomUUID();
  const setup = new AbortController();
  let stream: MediaStream | null = null;
  let context: AudioContext | null = null;
  let session: XaiVoiceInputSession | null = null;
  let started = false;
  let nativeCancelled = false;
  const cancelEarly = () => {
    setup.abort();
    if (session) {
      session.cancel();
      return;
    }
    stopTracks(stream);
    void context?.close().catch(() => undefined);
    if (!started || nativeCancelled) return;
    nativeCancelled = true;
    void xaiBridge.cancel(sessionId).catch(() => undefined);
  };
  const onAbort = () => cancelEarly();
  signal?.addEventListener('abort', onAbort, { once: true });
  try {
    stream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1 } });
    if (setup.signal.aborted) throw abortError();
    context = new AudioContext({ sampleRate: captureSampleRate });
    if (context.sampleRate !== captureSampleRate) {
      throw new Error('The audio format is unsupported.');
    }
    if (context.state === 'suspended') await context.resume();
    if (setup.signal.aborted) throw abortError();
    if (context.state === 'suspended') {
      throw new Error('Voice input could not start audio capture.');
    }
    await context.audioWorklet.addModule(captureUrl);
    if (setup.signal.aborted) throw abortError();
    const worklet = new AudioWorkletNode(context, 'cha-voice-capture');
    const gain = context.createGain();
    gain.gain.value = 0;
    worklet.connect(gain);
    gain.connect(context.destination);
    started = true;
    const ready = await xaiBridge.start(
      sessionId,
      configuration.languages ?? [],
      setup.signal,
    );
    if (setup.signal.aborted) throw abortError();
    if (ready.session_id !== sessionId
        || !Number.isInteger(ready.stop_budget_ms)
        || ready.stop_budget_ms < 0) {
      throw new Error('Voice input could not start.');
    }
    session = new XaiVoiceInputSession(
      sessionId,
      ready.stop_budget_ms,
      stream,
      context,
      worklet,
      onTranscription,
      onFailure,
      xaiBridge,
    );
    session.attachMicrophone();
    if (setup.signal.aborted) throw abortError();
    return session;
  } catch (failure) {
    cancelEarly();
    throw failure;
  } finally {
    signal?.removeEventListener('abort', onAbort);
  }
}
