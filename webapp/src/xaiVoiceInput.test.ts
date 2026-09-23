import { afterEach, describe, expect, it, vi } from 'vitest';

import { OpenAiVoiceInputSession } from './openAiVoiceInput';
import {
  appendPreparedTranscription,
  prepareDictationPiece,
} from './dictationText';
import {
  VoiceInputSession,
  type VoiceInputXaiBridge,
  type VoiceInputXaiPieces,
  type VoiceInputXaiStartResult,
} from './voiceInput';

class FakePort extends EventTarget {
  static flushSamples: Int16Array | null = null;

  start(): void {}

  postMessage(data: { type?: string }): void {
    if (data.type !== 'flush') return;
    const samples = FakePort.flushSamples;
    if (samples && samples.length > 0) {
      this.dispatchEvent(new MessageEvent('message', {
        data: { type: 'flush-batch', samples },
      }));
    }
    this.dispatchEvent(new MessageEvent('message', { data: { type: 'flushed' } }));
  }
}

class FakeWorklet {
  static latest: FakeWorklet | null = null;

  readonly port = new FakePort();

  constructor(_context: unknown, readonly name: string) {
    FakeWorklet.latest = this;
  }

  connect(): this { return this; }

  disconnect(): void {}

  emit(samples: Int16Array): void {
    this.port.dispatchEvent(new MessageEvent('message', {
      data: { type: 'batch', samples },
    }));
  }
}

class FakeContext {
  static latest: FakeContext | null = null;
  static moduleUrl = '';
  static rate = 16000;
  static suspended = false;

  sampleRate = FakeContext.rate;
  state: AudioContextState = FakeContext.suspended ? 'suspended' : 'running';
  destination = {};
  sourceConnected = false;
  gainValue = 1;
  closed = false;
  readonly audioWorklet = {
    addModule: vi.fn(async (url: string) => { FakeContext.moduleUrl = url; }),
  };

  constructor(_options?: AudioContextOptions) {
    FakeContext.latest = this;
  }

  createGain(): { gain: { value: number }; connect: () => void; disconnect: () => void } {
    return {
      gain: {
        set value(value: number) { FakeContext.latest!.gainValue = value; },
        get value() { return FakeContext.latest!.gainValue; },
      },
      connect: () => undefined,
      disconnect: () => undefined,
    };
  }

  createMediaStreamSource(): { connect: () => void; disconnect: () => void } {
    return {
      connect: () => { FakeContext.latest!.sourceConnected = true; },
      disconnect: () => undefined,
    };
  }

  async resume(): Promise<void> {
    if (!FakeContext.suspended) this.state = 'running';
  }

  async close(): Promise<void> {
    this.closed = true;
    this.state = 'closed';
  }
}

const xaiConfiguration = {
  provider: 'xai' as const,
  model: 'grok-voice-transcribe-2.0',
  delay: 'low' as const,
  prompt: 'Do not send this prompt.',
};

function installCapture(): { stopTrack: ReturnType<typeof vi.fn> } {
  const stopTrack = vi.fn();
  Object.defineProperty(navigator, 'mediaDevices', {
    configurable: true,
    value: {
      getUserMedia: vi.fn(async () => ({
        getAudioTracks: () => [{ stop: stopTrack }],
        getTracks: () => [{ stop: stopTrack }],
      })),
    },
  });
  vi.stubGlobal('AudioContext', FakeContext);
  vi.stubGlobal('AudioWorkletNode', FakeWorklet);
  vi.stubGlobal('RTCPeerConnection', class {
    constructor() { throw new Error('OpenAI WebRTC should not start'); }
  });
  return { stopTrack };
}

function pieces(sessionId: string, text: string[]): VoiceInputXaiPieces {
  return { session_id: sessionId, pieces: text };
}

afterEach(() => {
  FakePort.flushSamples = null;
  FakeWorklet.latest = null;
  FakeContext.latest = null;
  FakeContext.moduleUrl = '';
  FakeContext.rate = 16000;
  FakeContext.suspended = false;
  Reflect.deleteProperty(navigator, 'mediaDevices');
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('dictation piece formatting', () => {
  it('prepares commands, newlines, and separators one piece at a time', () => {
    expect(prepareDictationPiece('Hello', '')).toBe('Hello');
    expect(prepareDictationPiece(' new line', 'o')).toBe('\n');
    expect(prepareDictationPiece(' world', '\n')).toBe('world');
    expect(['Hello', ' new line', ' world'].reduce((text, piece) => {
      const next = prepareDictationPiece(piece, text.slice(-1));
      return next ? text + next : text;
    }, '')).toBe('Hello\nworld');

    expect(prepareDictationPiece(' comma', 'o')).toBe(',');
    expect(prepareDictationPiece(' точка', 'р')).toBe('.');
    expect(prepareDictationPiece(' новая строка', 'т')).toBe('\n');
    expect(prepareDictationPiece('Hello comma there', '')).toBe('Hello, there');
    expect(prepareDictationPiece('\n', '')).toBe('\n');
    expect(prepareDictationPiece(' \n ', '')).toBe('\n');
    expect(prepareDictationPiece('   ', '')).toBeNull();
    expect(prepareDictationPiece('new', '')).toBe('new');
    expect(prepareDictationPiece('line', 'w')).toBe(' line');
  });

  it('keeps a prepared newline when appending to a draft', () => {
    expect(appendPreparedTranscription('Draft', 'Hello')).toBe('Draft Hello');
    expect(appendPreparedTranscription('Draft ', 'Hello')).toBe('Draft Hello');
    expect(appendPreparedTranscription('Draft', '\n')).toBe('Draft\n');
    expect(appendPreparedTranscription('Draft\n', 'world')).toBe('Draft\nworld');
    expect(appendPreparedTranscription('Hello', ',')).toBe('Hello,');
    expect(appendPreparedTranscription('Hello', '')).toBe('Hello');
  });
});

describe('xAI voice capture', () => {
  it('starts capture after native readiness and forwards languages', async () => {
    installCapture();
    const OpenAiStart = vi.spyOn(OpenAiVoiceInputSession, 'start');
    let releaseStart: ((value: { session_id: string; stop_budget_ms: number }) => void) | undefined;
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn((sessionId: string, languages: string[]) => {
        expect(languages).toEqual(['ru']);
        expect(FakeContext.latest?.sourceConnected).toBe(false);
        return new Promise<VoiceInputXaiStartResult>((resolve) => {
          releaseStart = () => resolve({ session_id: sessionId, stop_budget_ms: 20000 });
        });
      }),
      audio: vi.fn(async (sessionId: string) => pieces(sessionId, [])),
      stop: vi.fn(async (sessionId: string) => pieces(sessionId, [])),
      cancel: vi.fn(async () => {}),
    };
    const starting = VoiceInputSession.start(
      { ...xaiConfiguration, languages: ['ru'] },
      () => {},
      () => {},
      vi.fn(),
      bridge,
    );
    await vi.waitFor(() => expect(bridge.start).toHaveBeenCalledOnce());
    expect(FakeContext.latest?.sourceConnected).toBe(false);
    expect(FakeWorklet.latest?.name).toBe('cha-voice-capture');
    expect(FakeContext.moduleUrl.startsWith('data:')).toBe(false);
    releaseStart?.({ session_id: 'late', stop_budget_ms: 20000 });
    const session = await starting;
    expect(FakeContext.latest?.sourceConnected).toBe(true);
    expect(FakeContext.latest?.gainValue).toBe(0);
    expect(OpenAiStart).not.toHaveBeenCalled();
    const media = navigator.mediaDevices.getUserMedia as ReturnType<typeof vi.fn>;
    expect(media).toHaveBeenCalledWith({ audio: { channelCount: 1 } });
    session.cancel();
  });

  it('forwards an empty language list and English without hardcoding', async () => {
    installCapture();
    const seen: string[][] = [];
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (sessionId: string, languages: string[]) => {
        seen.push(languages);
        return { session_id: sessionId, stop_budget_ms: 20000 };
      }),
      audio: vi.fn(async (sessionId: string) => pieces(sessionId, [])),
      stop: vi.fn(async (sessionId: string) => pieces(sessionId, [])),
      cancel: vi.fn(async () => {}),
    };
    const english = await VoiceInputSession.start(
      { ...xaiConfiguration, languages: ['en'] },
      () => {}, () => {}, vi.fn(), bridge,
    );
    english.cancel();
    const empty = await VoiceInputSession.start(
      { ...xaiConfiguration, languages: [] },
      () => {}, () => {}, vi.fn(), bridge,
    );
    empty.cancel();
    expect(seen).toEqual([['en'], []]);
  });

  it('rejects an unsupported sample rate before native start', async () => {
    const { stopTrack } = installCapture();
    FakeContext.rate = 48000;
    const start = vi.fn();
    await expect(VoiceInputSession.start(
      xaiConfiguration,
      () => {},
      () => {},
      vi.fn(),
      {
        start,
        audio: vi.fn(),
        stop: vi.fn(),
        cancel: vi.fn(async () => {}),
      },
    )).rejects.toThrow('The audio format is unsupported.');
    expect(start).not.toHaveBeenCalled();
    expect(stopTrack).toHaveBeenCalled();
    expect(FakeContext.latest?.closed).toBe(true);
  });

  it('keeps one audio request outstanding and preserves batch order', async () => {
    installCapture();
    const sent: string[] = [];
    const pending: Array<(value: VoiceInputXaiPieces) => void> = [];
    let sessionId = '';
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (id: string) => {
        sessionId = id;
        return { session_id: id, stop_budget_ms: 20000 };
      }),
      audio: vi.fn((_id: string, pcm: string) => {
        sent.push(pcm);
        return new Promise<VoiceInputXaiPieces>((resolve) => { pending.push(resolve); });
      }),
      stop: vi.fn(async (id: string) => pieces(id, [])),
      cancel: vi.fn(async () => {}),
    };
    const otherUi = vi.fn(async () => 'settings-saved');
    const session = await VoiceInputSession.start(
      xaiConfiguration, () => {}, () => {}, vi.fn(), bridge,
    );
    const node = FakeWorklet.latest;
    if (!node) throw new Error('missing worklet');
    node.emit(Int16Array.from([1, 2]));
    node.emit(Int16Array.from([3, 4]));
    expect(bridge.audio).toHaveBeenCalledOnce();
    await expect(otherUi()).resolves.toBe('settings-saved');
    pending[0]?.(pieces(sessionId, []));
    await vi.waitFor(() => expect(bridge.audio).toHaveBeenCalledTimes(2));
    expect(sent[0]?.startsWith(btoa(String.fromCharCode(1, 0, 2, 0)))).toBe(true);
    const second = atob(sent[1] ?? '');
    expect(second.charCodeAt(0)).toBe(3);
    expect(second.charCodeAt(1)).toBe(0);
    session.cancel();
  });

  it('keeps a full batch envelope below the bridge limit', async () => {
    installCapture();
    let pcm = '';
    let sessionId = '';
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (id: string) => {
        sessionId = id;
        return { session_id: id, stop_budget_ms: 20000 };
      }),
      audio: vi.fn(async (id: string, body: string) => {
        pcm = body;
        return pieces(id, []);
      }),
      stop: vi.fn(async (id: string) => pieces(id, [])),
      cancel: vi.fn(async () => {}),
    };
    const session = await VoiceInputSession.start(
      xaiConfiguration, () => {}, () => {}, vi.fn(), bridge,
    );
    FakeWorklet.latest?.emit(new Int16Array(1600).fill(1));
    await vi.waitFor(() => expect(pcm).not.toBe(''));
    expect(pcm).toHaveLength(4268);
    const bytes = Uint8Array.from(atob(pcm), (char) => char.charCodeAt(0));
    expect(bytes).toHaveLength(3200);
    expect(bytes[0]).toBe(1);
    expect(bytes[1]).toBe(0);
    const envelope = JSON.stringify({
      connection_id: 'c'.repeat(64),
      id: Number.MAX_SAFE_INTEGER,
      context_epoch: Number.MAX_SAFE_INTEGER,
      method: 'voiceInput.xai.audio',
      params: { session_id: 's'.repeat(64), pcm_base64: pcm },
    });
    expect(envelope.length).toBeLessThan(65536);
    expect(sessionId).not.toBe('');
    session.cancel();
  });

  it('stops on the 20th waiting batch and does not send it', async () => {
    const failures: unknown[] = [];
    const received: string[] = [];
    let releaseAudio: ((value: VoiceInputXaiPieces) => void) | undefined;
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (sessionId: string) => (
        { session_id: sessionId, stop_budget_ms: 20000 }
      )),
      audio: vi.fn((_sessionId: string) => new Promise<VoiceInputXaiPieces>((resolve) => {
        releaseAudio = () => resolve(pieces(_sessionId, ['late']));
      })),
      stop: vi.fn(async (sessionId: string) => pieces(sessionId, [])),
      cancel: vi.fn(async () => {}),
    };
    const { stopTrack } = installCapture();
    const session = await VoiceInputSession.start(
      xaiConfiguration,
      (text) => received.push(text),
      (failure) => failures.push(failure),
      vi.fn(),
      bridge,
    );
    const node = FakeWorklet.latest;
    if (!node) throw new Error('missing worklet');
    for (let index = 0; index < 19; index += 1) node.emit(new Int16Array(8));
    expect(failures).toEqual([]);
    expect(bridge.audio).toHaveBeenCalledOnce();
    node.emit(new Int16Array(8));
    expect(failures).toHaveLength(1);
    expect(failures[0]).toMatchObject({ message: 'Voice input audio overflowed.' });
    expect(bridge.audio).toHaveBeenCalledOnce();
    expect(stopTrack).toHaveBeenCalled();
    expect(FakeContext.latest?.closed).toBe(true);
    releaseAudio?.(pieces('stale', ['late']));
    await Promise.resolve();
    expect(bridge.audio).toHaveBeenCalledOnce();
    expect(received).toEqual([]);
    session.cancel();
  });

  it('accepts the flush batch as the twentieth slot and sends it before stop', async () => {
    const { stopTrack } = installCapture();
    const pcms: string[] = [];
    const pending: Array<(value: VoiceInputXaiPieces) => void> = [];
    let sessionId = '';
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (id: string) => {
        sessionId = id;
        return { session_id: id, stop_budget_ms: 20000 };
      }),
      audio: vi.fn((_id: string, pcm: string) => {
        pcms.push(pcm);
        return new Promise<VoiceInputXaiPieces>((resolve) => { pending.push(resolve); });
      }),
      stop: vi.fn(async (id: string, remaining: number) => {
        expect(remaining).toBeGreaterThan(0);
        expect(remaining).toBeLessThanOrEqual(20000);
        return pieces(id, []);
      }),
      cancel: vi.fn(async () => {}),
    };
    const session = await VoiceInputSession.start(
      xaiConfiguration, () => {}, () => {}, vi.fn(), bridge,
    );
    const node = FakeWorklet.latest;
    if (!node) throw new Error('missing worklet');
    for (let index = 0; index < 19; index += 1) node.emit(new Int16Array(4));
    FakePort.flushSamples = Int16Array.from([7]);
    const stopping = session.stop();
    expect(bridge.audio).toHaveBeenCalledOnce();
    expect(bridge.stop).not.toHaveBeenCalled();
    while (pending.length > 0 || pcms.length < 20) {
      const resolve = pending.shift();
      if (!resolve) break;
      resolve(pieces(sessionId, []));
      await Promise.resolve();
      await Promise.resolve();
    }
    await vi.waitFor(() => expect(pcms).toHaveLength(20));
    const last = Uint8Array.from(atob(pcms[19] ?? ''), (char) => char.charCodeAt(0));
    expect(last).toHaveLength(2);
    expect(last[0]).toBe(7);
    await vi.waitFor(() => expect(bridge.stop).toHaveBeenCalledOnce());
    await stopping;
    expect(stopTrack).toHaveBeenCalled();
    expect(FakeContext.latest?.closed).toBe(true);
  });

  it('formats each reply piece once and delivers stop pieces before stop resolves', async () => {
    installCapture();
    const order: string[] = [];
    let sessionId = '';
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (id: string) => {
        sessionId = id;
        return { session_id: id, stop_budget_ms: 20000 };
      }),
      audio: vi.fn(async (id: string) => pieces(id, ['Hello', ' new line', 'ignored stale'])),
      stop: vi.fn(async (id: string) => {
        order.push('stop-returned');
        return pieces(id, [' world', '']);
      }),
      cancel: vi.fn(async () => {}),
    };
    const session = await VoiceInputSession.start(
      xaiConfiguration,
      (text) => order.push(text),
      () => {},
      vi.fn(),
      bridge,
    );
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    await vi.waitFor(() => expect(order).toEqual(['Hello', '\n', 'ignored stale']));
    bridge.audio = vi.fn(async () => pieces('other-session', ['nope']));
    FakeWorklet.latest?.emit(Int16Array.from([2]));
    await vi.waitFor(() => expect(bridge.audio).toHaveBeenCalled());
    await Promise.resolve();
    expect(order).toEqual(['Hello', '\n', 'ignored stale']);
    await session.stop();
    order.push('stop-resolved');
    expect(order).toEqual([
      'Hello', '\n', 'ignored stale', 'stop-returned', ' world', 'stop-resolved',
    ]);
    expect(sessionId).not.toBe('');
  });

  it('formats later commands and preserves repeated text', async () => {
    installCapture();
    const received: string[] = [];
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (sessionId: string) => (
        { session_id: sessionId, stop_budget_ms: 20000 }
      )),
      audio: vi.fn(async (sessionId: string) => pieces(sessionId, [
        'Hello',
        ' comma',
        'Мир',
        ' точка',
        'Привет',
        ' новая строка',
        'мир',
        'new',
        'line',
        'Try again.',
        'Try again.',
        'very very useful',
      ])),
      stop: vi.fn(async (sessionId: string) => pieces(sessionId, ['\n', 'Next'])),
      cancel: vi.fn(async () => {}),
    };
    const session = await VoiceInputSession.start(
      xaiConfiguration,
      (text) => received.push(text),
      () => {},
      vi.fn(),
      bridge,
    );
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    await vi.waitFor(() => expect(received[0]).toBe('Hello'));
    await session.stop();
    expect(received.join('')).toBe(
      'Hello, Мир. Привет\nмир new line Try again. Try again. very very useful\nNext',
    );
  });

  it('fails a short stop budget and cancels while stop is pending', async () => {
    installCapture();
    const failures: unknown[] = [];
    let releaseStop: ((value: VoiceInputXaiPieces) => void) | undefined;
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (sessionId: string) => ({ session_id: sessionId, stop_budget_ms: 40 })),
      audio: vi.fn(async (sessionId: string) => pieces(sessionId, [])),
      stop: vi.fn(() => new Promise<VoiceInputXaiPieces>((resolve) => { releaseStop = resolve; })),
      cancel: vi.fn(async () => {}),
    };
    const session = await VoiceInputSession.start(
      xaiConfiguration, () => {}, (failure) => failures.push(failure), vi.fn(), bridge,
    );
    await expect(session.stop()).rejects.toThrow('Voice input timed out.');
    expect(failures).toEqual([expect.objectContaining({ message: 'Voice input timed out.' })]);
    expect(bridge.cancel).toHaveBeenCalledOnce();
    expect(FakeContext.latest?.closed).toBe(true);
    releaseStop?.(pieces('done', ['too late']));
    await expect(session.stop()).rejects.toThrow('Voice input timed out.');
    expect(bridge.stop).toHaveBeenCalledOnce();
  });

  it('settles stop when dictation is cancelled and ignores a late reply', async () => {
    const { stopTrack } = installCapture();
    const received: string[] = [];
    let releaseAudio: ((value: VoiceInputXaiPieces) => void) | undefined;
    let releaseStop: ((value: VoiceInputXaiPieces) => void) | undefined;
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (sessionId: string) => (
        { session_id: sessionId, stop_budget_ms: 20000 }
      )),
      audio: vi.fn((_id: string, _pcm: string, signal: AbortSignal) => new Promise<VoiceInputXaiPieces>((resolve, reject) => {
        releaseAudio = resolve;
        signal.addEventListener('abort', () => {
          reject(new DOMException('The operation was aborted.', 'AbortError'));
        }, { once: true });
      })),
      stop: vi.fn((_id: string, _remaining: number, signal: AbortSignal) => new Promise<VoiceInputXaiPieces>((resolve, reject) => {
        releaseStop = resolve;
        signal.addEventListener('abort', () => {
          reject(new DOMException('The operation was aborted.', 'AbortError'));
        }, { once: true });
      })),
      cancel: vi.fn(async () => {}),
    };
    const session = await VoiceInputSession.start(
      xaiConfiguration,
      (text) => received.push(text),
      () => {},
      vi.fn(),
      bridge,
    );
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    const stopping = session.stop();
    await vi.waitFor(() => expect(releaseAudio).toBeTypeOf('function'));
    releaseAudio?.(pieces('in-flight', []));
    await vi.waitFor(() => expect(bridge.stop).toHaveBeenCalledOnce());
    session.cancel();
    session.cancel();
    await stopping;
    expect(stopTrack).toHaveBeenCalled();
    expect(FakeContext.latest?.closed).toBe(true);
    expect(bridge.cancel).toHaveBeenCalledOnce();
    releaseAudio?.(pieces('old', ['late audio']));
    releaseStop?.(pieces('old', ['late stop']));
    await Promise.resolve();
    expect(received).toEqual([]);
    await session.stop();
  });

  it('releases the microphone when startup is cancelled', async () => {
    const stopTrack = vi.fn();
    let releaseMedia: ((stream: object) => void) | undefined;
    Object.defineProperty(navigator, 'mediaDevices', {
      configurable: true,
      value: {
        getUserMedia: vi.fn(() => new Promise((resolve) => {
          releaseMedia = () => resolve({
            getAudioTracks: () => [{ stop: stopTrack }],
            getTracks: () => [{ stop: stopTrack }],
          });
        })),
      },
    });
    vi.stubGlobal('AudioContext', FakeContext);
    vi.stubGlobal('AudioWorkletNode', FakeWorklet);
    const start = vi.fn();
    const controller = new AbortController();
    const starting = VoiceInputSession.start(
      xaiConfiguration, () => {}, () => {}, vi.fn(),
      { start, audio: vi.fn(), stop: vi.fn(), cancel: vi.fn(async () => {}) },
      controller.signal,
    );
    await vi.waitFor(() => expect(navigator.mediaDevices.getUserMedia).toHaveBeenCalled());
    controller.abort();
    releaseMedia?.({});
    await expect(starting).rejects.toMatchObject({ name: 'AbortError' });
    expect(stopTrack).toHaveBeenCalled();
    expect(start).not.toHaveBeenCalled();
  });

  it('cancels a native start that is still in flight', async () => {
    const { stopTrack } = installCapture();
    let releaseStart: (() => void) | undefined;
    const cancel = vi.fn(async () => {});
    const controller = new AbortController();
    const starting = VoiceInputSession.start(
      xaiConfiguration, () => {}, () => {}, vi.fn(),
      {
        start: vi.fn((sessionId: string) => new Promise<VoiceInputXaiStartResult>((resolve) => {
          releaseStart = () => resolve({ session_id: sessionId, stop_budget_ms: 20000 });
        })),
        audio: vi.fn(),
        stop: vi.fn(),
        cancel,
      },
      controller.signal,
    );
    await vi.waitFor(() => expect(releaseStart).toBeTypeOf('function'));
    controller.abort();
    releaseStart?.();
    await expect(starting).rejects.toMatchObject({ name: 'AbortError' });
    expect(cancel).toHaveBeenCalledOnce();
    expect(stopTrack).toHaveBeenCalled();
    expect(FakeContext.latest?.sourceConnected).toBe(false);
  });

  it('ends the session when an audio request fails', async () => {
    const { stopTrack } = installCapture();
    const failures: unknown[] = [];
    const bridge: VoiceInputXaiBridge = {
      start: vi.fn(async (sessionId: string) => (
        { session_id: sessionId, stop_budget_ms: 20000 }
      )),
      audio: vi.fn(async () => { throw new Error('native bridge failed'); }),
      stop: vi.fn(async (sessionId: string) => pieces(sessionId, [])),
      cancel: vi.fn(async () => {}),
    };
    const session = await VoiceInputSession.start(
      xaiConfiguration, () => {}, (failure) => failures.push(failure), vi.fn(), bridge,
    );
    FakeWorklet.latest?.emit(Int16Array.from([1]));
    await vi.waitFor(() => expect(failures).toHaveLength(1));
    expect(stopTrack).toHaveBeenCalled();
    expect(FakeContext.latest?.closed).toBe(true);
    expect(bridge.cancel).toHaveBeenCalledOnce();
    FakeWorklet.latest?.emit(Int16Array.from([2]));
    expect(bridge.audio).toHaveBeenCalledOnce();
    await expect(session.stop()).rejects.toThrow('native bridge failed');
  });
});
