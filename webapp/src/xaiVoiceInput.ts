import { ChaError } from './api/client';
import { prepareDictationPiece } from './dictationText';
import captureUrl from './voiceInputCapture.worklet.js?url&no-inline';
import type {
  VoiceInputConfiguration,
  VoiceInputTransport,
  VoiceInputXaiBridge,
} from './voiceInput';

const captureSampleRate = 16000;
const maxWaitingBatches = 19;

export const unsupportedAudioFormat = 'The audio format is unsupported.';

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
  private sending = false;
  private stopping = false;
  private cancelled = false;
  private failed = false;
  private releasePromise: Promise<void> | null = null;
  private captureStopped = false;
  private failure: unknown = null;
  private timedOut = false;
  private stopPromise: Promise<void> | null = null;
  private idle: (() => void) | null = null;
  private flushResolve: (() => void) | null = null;
  private lastCharacter = '';
  private preview = '';

  constructor(
    private readonly sessionId: string,
    private readonly stopBudgetMs: number,
    private readonly stream: MediaStream,
    private readonly context: AudioContext,
    private readonly worklet: AudioWorkletNode,
    private readonly onTranscription: (text: string, provisional?: boolean) => void,
    private readonly onFailure: (failure: unknown) => void,
    private readonly xaiBridge: VoiceInputXaiBridge,
  ) {
    this.worklet.port.addEventListener('message', this.onWorkletMessage);
    this.worklet.addEventListener('processorerror', this.onCaptureFailure);
    for (const track of this.stream.getTracks()) {
      track.addEventListener('ended', this.onCaptureFailure);
    }
    this.worklet.port.start();
  }

  attachMicrophone(): void {
    if (this.stream.getTracks().some((track) => track.readyState === 'ended')) {
      throw new Error('Voice input audio capture failed.');
    }
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
    this.clearPreview();
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

  private readonly onCaptureFailure = (): void => {
    if (!this.captureStopped) this.fail(new Error('Voice input audio capture failed.'));
  };

  // Nineteen completed batches may wait, including the in-flight request.
  // The final flush batch may use one extra slot.
  private acceptBatch(samples: Int16Array, flush: boolean): void {
    const limit = this.stopping && flush ? maxWaitingBatches + 1 : maxWaitingBatches;
    const waiting = this.pending.length + (this.sending ? 1 : 0);
    if (waiting >= limit) {
      this.fail(new Error('Voice input audio overflowed.'));
      return;
    }
    this.pending.push(samples);
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
        if (!this.cancelled && !this.failed && reply.session_id === this.sessionId) {
          this.deliver(reply.pieces, reply.preview ?? '');
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

  private clearPreview(): void {
    if (!this.preview) return;
    this.preview = '';
    this.onTranscription('', true);
  }

  private deliver(pieces: readonly string[], rawPreview: string): void {
    if (this.cancelled || this.failed) return;
    const incomingPreview = prepareDictationPiece(rawPreview, this.lastCharacter) ?? '';
    if (this.preview && (this.preview !== incomingPreview || pieces.length > 0)) {
      this.clearPreview();
    }
    for (const piece of pieces) {
      if (this.cancelled) return;
      const text = prepareDictationPiece(piece, this.lastCharacter);
      if (!text) continue;
      this.lastCharacter = text[text.length - 1] ?? '';
      this.onTranscription(text);
    }
    const preview = prepareDictationPiece(rawPreview, this.lastCharacter) ?? '';
    if (preview && preview !== this.preview) {
      this.preview = preview;
      this.onTranscription(preview, true);
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
    await this.drain();
    if (this.cancelled) return;
    if (this.timedOut) throw new Error('Voice input timed out.');
    if (this.failed) throw this.failure ?? new Error('Voice input failed.');
    const remaining = Math.max(0, Math.floor(this.stopBudgetMs - (Date.now() - started)));
    if (this.live.signal.aborted || remaining <= 0) {
      throw new Error('Voice input timed out.');
    }
    const reply = await this.xaiBridge.stop(this.sessionId, remaining, this.live.signal);
    if (this.cancelled || this.failed) return;
    if (reply.session_id === this.sessionId) this.deliver(reply.pieces, reply.preview ?? '');
    await this.release();
  }

  private async finish(): Promise<void> {
    const started = Date.now();
    let timer: ReturnType<typeof setTimeout> | undefined;
    let onAbort = () => {};
    const aborted = new Promise<never>((_resolve, reject) => {
      onAbort = () => reject(abortError());
      this.live.signal.addEventListener('abort', onAbort, { once: true });
      if (this.live.signal.aborted) onAbort();
    });
    const work = this.flushDrainAndStop(started);
    void work.catch(() => undefined);
    try {
      await Promise.race([
        work,
        aborted,
        new Promise<never>((_resolve, reject) => {
          timer = setTimeout(() => {
            this.timedOut = true;
            // Reject before the abort so the race reports the timeout, not an
            // aborted bridge call.
            reject(new Error('Voice input timed out.'));
            this.live.abort();
          }, Math.max(0, this.stopBudgetMs));
        }),
      ]);
    } catch (failure) {
      if (this.cancelled) return;
      if (!this.failed) this.fail(failure);
      throw this.failure ?? failure;
    } finally {
      if (timer !== undefined) clearTimeout(timer);
      this.live.signal.removeEventListener('abort', onAbort);
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
    this.clearPreview();
    this.notifyIdle();
    this.flushResolve?.();
    this.flushResolve = null;
    this.onFailure(failure);
  }

  private release(): Promise<void> {
    if (this.releasePromise) return this.releasePromise;
    this.captureStopped = true;
    this.worklet.port.removeEventListener('message', this.onWorkletMessage);
    this.worklet.removeEventListener('processorerror', this.onCaptureFailure);
    for (const track of this.stream.getTracks()) {
      track.removeEventListener('ended', this.onCaptureFailure);
    }
    stopTracks(this.stream);
    this.releasePromise = this.context.close();
    // Cancel and failure initiate cleanup without waiting; graceful stop awaits
    // this same promise inside its timeout budget.
    void this.releasePromise.catch(() => undefined);
    return this.releasePromise;
  }
}

export async function startXaiVoiceInput(
  configuration: VoiceInputConfiguration,
  onTranscription: (text: string, provisional?: boolean) => void,
  onFailure: (failure: unknown) => void,
  xaiBridge: VoiceInputXaiBridge,
  signal?: AbortSignal,
): Promise<VoiceInputTransport> {
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
      throw new Error(unsupportedAudioFormat);
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
    ).catch((failure: unknown) => {
      // Native has already ended a start that it rejected. A cancel would only
      // leave a tombstone there until the connection closes.
      if (failure instanceof ChaError) started = false;
      throw failure;
    });
    if (setup.signal.aborted) throw abortError();
    if (ready.session_id !== sessionId) throw new Error('Voice input could not start.');
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
