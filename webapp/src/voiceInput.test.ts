import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { OpenAiVoiceInputSession } from './openAiVoiceInput';
import { beginSpeechPlayback } from './speechPlayback';
import {
  appendTranscription,
  VoiceInputSession,
  type VoiceInputXaiBridge,
} from './voiceInput';

const unusedXaiBridge: VoiceInputXaiBridge = {
  async start() { throw new Error('unused'); },
  async audio() { throw new Error('unused'); },
  async stop() { throw new Error('unused'); },
  async cancel() {},
};

function installRealtimeCapture() {
  const track = { kind: 'audio', enabled: true, stop: vi.fn() };
  const sender = { replaceTrack: vi.fn(async (_track: unknown) => {}) };
  Object.defineProperty(navigator, 'mediaDevices', {
    configurable: true,
    value: { getUserMedia: vi.fn(async () => ({
      getAudioTracks: () => [track], getTracks: () => [track],
    })) },
  });
  const channel = Object.assign(new EventTarget(), {
    readyState: 'open', send: vi.fn(), close: vi.fn(),
  });
  class FakePeerConnection extends EventTarget {
    connectionState = 'new';
    addTrack() { return sender; }
    createDataChannel() { return channel; }
    async createOffer() { return { type: 'offer', sdp: 'offer' }; }
    async setLocalDescription() {}
    async setRemoteDescription() { channel.dispatchEvent(new Event('open')); }
    close() { this.connectionState = 'closed'; }
  }
  vi.stubGlobal('RTCPeerConnection', FakePeerConnection);
  const failure = vi.fn();
  const connect = vi.fn(async () => 'answer');
  const start = () => VoiceInputSession.start(
    { provider: 'openai', model: 'gpt-4o-transcribe', delay: 'low', prompt: '' },
    vi.fn(), failure, connect, unusedXaiBridge,
  );
  return { track, sender, failure, connect, start };
}

describe('OpenAI microphone during speech playback', () => {
  beforeEach(() => { vi.useFakeTimers(); });
  it('mutes immediately, detaches the sender, and restores it on the same connection', async () => {
    const { track, sender, connect, start } = installRealtimeCapture();
    const session = await start();
    const endPlayback = beginSpeechPlayback();
    try {
      expect(track.enabled).toBe(false);
      await vi.waitFor(() => expect(sender.replaceTrack).toHaveBeenCalledWith(null));
      endPlayback();
      await vi.advanceTimersByTimeAsync(400);
      expect(track.enabled).toBe(true);
      await vi.waitFor(() => expect(sender.replaceTrack).toHaveBeenLastCalledWith(track));
      expect(connect).toHaveBeenCalledOnce();
      expect(track.stop).not.toHaveBeenCalled();
    } finally {
      session.cancel();
      endPlayback();
    }
  });

  it('starts muted during playback and stays stopped if cancelled before playback ends', async () => {
    const { track, sender, start } = installRealtimeCapture();
    const endPlayback = beginSpeechPlayback();
    try {
      const session = await start();
      expect(track.enabled).toBe(false);
      await vi.waitFor(() => expect(sender.replaceTrack).toHaveBeenCalledWith(null));
      session.cancel();
      endPlayback();
      await vi.advanceTimersByTimeAsync(400);
      expect(track.enabled).toBe(false);
      expect(track.stop).toHaveBeenCalled();
      expect(sender.replaceTrack).toHaveBeenCalledTimes(1);
    } finally {
      endPlayback();
    }
  });

  it('keeps input muted between back-to-back clips without reattaching the track', async () => {
    const { track, sender, start } = installRealtimeCapture();
    const session = await start();
    let detached!: () => void;
    sender.replaceTrack.mockImplementationOnce(() => new Promise((resolve) => { detached = resolve; }));
    const firstEnded = beginSpeechPlayback();
    let secondEnded = () => {};
    try {
      await vi.waitFor(() => expect(detached).toBeDefined());
      firstEnded();
      secondEnded = beginSpeechPlayback();
      expect(track.enabled).toBe(false);
      detached();
      await vi.advanceTimersByTimeAsync(400);
      expect(sender.replaceTrack.mock.calls.map(([value]) => value)).toEqual([null]);
      expect(track.enabled).toBe(false);
      secondEnded();
      await vi.advanceTimersByTimeAsync(400);
      await vi.waitFor(() => expect(sender.replaceTrack).toHaveBeenLastCalledWith(track));
    } finally {
      session.cancel();
      firstEnded();
      secondEnded();
    }
  });

  it('releases capture if WebRTC cannot pause sending', async () => {
    const { track, sender, failure, start } = installRealtimeCapture();
    const session = await start();
    sender.replaceTrack.mockRejectedValueOnce(new Error('Cannot detach'));
    const endPlayback = beginSpeechPlayback();
    try {
      await vi.waitFor(() => expect(failure).toHaveBeenCalledOnce());
      expect(track.stop).toHaveBeenCalled();
      endPlayback();
      await vi.advanceTimersByTimeAsync(400);
      expect(track.enabled).toBe(false);
    } finally {
      session.cancel();
      endPlayback();
    }
  });
});

afterEach(async () => {
  if (vi.isFakeTimers()) await vi.advanceTimersByTimeAsync(400);
  Reflect.deleteProperty(navigator, 'mediaDevices');
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('voice input', () => {
  it('joins blocks without damaging punctuation or existing whitespace', () => {
    expect(appendTranscription('', '  Hello  ')).toBe('Hello');
    expect(appendTranscription('Hello', 'world.')).toBe('Hello world.');
    expect(appendTranscription('Hello ', 'world.')).toBe('Hello world.');
    expect(appendTranscription('Hello', ', world.')).toBe('Hello, world.');
    expect(appendTranscription('Hello', '   ')).toBe('Hello');
  });

  it('turns English and Russian dictation commands into punctuation', () => {
    expect(appendTranscription('', 'Hello! exclamation sign.')).toBe('Hello!');
    expect(appendTranscription('', 'Hello comma how are you question mark')).toBe(
      'Hello, how are you?',
    );
    expect(appendTranscription('', 'Привет запятая как дела знак вопроса')).toBe(
      'Привет, как дела?',
    );
    expect(appendTranscription('', 'First new line Second')).toBe('First\nSecond');
  });

  it('streams transcript deltas and commits the final turn before stopping', async () => {
    const stopTrack = vi.fn();
    const audioTrack = { kind: 'audio', stop: stopTrack } as unknown as MediaStreamTrack;
    Object.defineProperty(navigator, 'mediaDevices', {
      configurable: true,
      value: { getUserMedia: vi.fn(async () => ({
        getAudioTracks: () => [audioTrack],
        getTracks: () => [audioTrack],
      })) },
    });

    class FakeDataChannel extends EventTarget {
      readyState: RTCDataChannelState = 'connecting';
      readonly sent: string[] = [];

      send(data: string) { this.sent.push(data); }
      open() {
        this.readyState = 'open';
        this.dispatchEvent(new Event('open'));
      }
      message(data: object) {
        this.dispatchEvent(new MessageEvent('message', { data: JSON.stringify(data) }));
      }
      close() {
        this.readyState = 'closed';
        this.dispatchEvent(new Event('close'));
      }
    }
    const channel = new FakeDataChannel();

    class FakePeerConnection extends EventTarget {
      connectionState: RTCPeerConnectionState = 'new';
      addTrack = vi.fn();
      createDataChannel() {
        return channel as unknown as RTCDataChannel;
      }
      async createOffer() {
        return { type: 'offer', sdp: 'test offer' } as RTCSessionDescriptionInit;
      }
      async setLocalDescription() {}
      async setRemoteDescription() { channel.open(); }
      close() { this.connectionState = 'closed'; }
    }
    vi.stubGlobal('RTCPeerConnection', FakePeerConnection);
    const connect = vi.fn(async (_sdp: string, languages: string[]) => {
      expect(languages).toEqual(['ru', 'en']);
      return 'test answer';
    });
    const received: string[] = [];
    const session = await VoiceInputSession.start(
      {
        provider: 'openai',
        model: 'gpt-live-transcribe',
        delay: 'xhigh',
        prompt: 'A discussion about software architecture.',
        languages: ['ru', 'en'],
      },
      (text) => received.push(text),
      () => {},
      connect,
      unusedXaiBridge,
    );

    expect(connect).toHaveBeenCalledWith(
      'test offer', ['ru', 'en'], expect.any(AbortSignal),
    );

    channel.message({
      type: 'conversation.item.input_audio_transcription.delta', delta: 'Hello',
    });
    channel.message({
      type: 'conversation.item.input_audio_transcription.delta', delta: ' world.',
    });
    expect(received).toEqual(['Hello', ' world.']);

    const stopping = session.stop();
    expect(channel.sent.map((event) => JSON.parse(event))).toEqual([
      { type: 'input_audio_buffer.commit' },
    ]);
    let stopped = false;
    void stopping.then(() => { stopped = true; });
    await Promise.resolve();
    expect(stopped).toBe(false);
    channel.message({
      type: 'conversation.item.input_audio_transcription.completed',
      transcript: 'Hello world.',
    });
    await stopping;
    expect(stopped).toBe(true);
    expect(stopTrack).toHaveBeenCalled();
  });

  it('uses a native connect callback instead of a credential-bearing fetch', async () => {
    const stopTrack = vi.fn();
    const audioTrack = { kind: 'audio', stop: stopTrack } as unknown as MediaStreamTrack;
    Object.defineProperty(navigator, 'mediaDevices', {
      configurable: true,
      value: { getUserMedia: vi.fn(async () => ({
        getAudioTracks: () => [audioTrack],
        getTracks: () => [audioTrack],
      })) },
    });
    class FakeDataChannel extends EventTarget {
      readyState: RTCDataChannelState = 'connecting';
      send() {}
      open() {
        this.readyState = 'open';
        this.dispatchEvent(new Event('open'));
      }
      close() { this.readyState = 'closed'; }
    }
    const channel = new FakeDataChannel();
    class FakePeerConnection extends EventTarget {
      connectionState: RTCPeerConnectionState = 'new';
      addTrack = vi.fn();
      createDataChannel() { return channel as unknown as RTCDataChannel; }
      async createOffer() {
        return { type: 'offer', sdp: 'native offer' } as RTCSessionDescriptionInit;
      }
      async setLocalDescription() {}
      async setRemoteDescription() { channel.open(); }
      close() { this.connectionState = 'closed'; }
    }
    vi.stubGlobal('RTCPeerConnection', FakePeerConnection);
    const fetcher = vi.fn();
    vi.stubGlobal('fetch', fetcher);
    const connect = vi.fn(async (sdp: string, languages: string[]) => {
      expect(sdp).toBe('native offer');
      expect(languages).toEqual([]);
      return 'native answer';
    });
    const session = await VoiceInputSession.start(
      {
        provider: 'openai',
        model: 'gpt-4o-transcribe',
        delay: 'low',
        prompt: '',
      },
      () => {},
      () => {},
      connect,
      unusedXaiBridge,
    );
    expect(fetcher).not.toHaveBeenCalled();
    expect(connect).toHaveBeenCalledOnce();
    session.cancel();
    expect(stopTrack).toHaveBeenCalled();
  });

  it('cancels native setup and microphone capture before start resolves', async () => {
    const stopTrack = vi.fn();
    const audioTrack = { kind: 'audio', stop: stopTrack } as unknown as MediaStreamTrack;
    Object.defineProperty(navigator, 'mediaDevices', {
      configurable: true,
      value: { getUserMedia: vi.fn(async () => ({
        getAudioTracks: () => [audioTrack],
        getTracks: () => [audioTrack],
      })) },
    });
    class FakeDataChannel extends EventTarget {
      readyState: RTCDataChannelState = 'connecting';
      send() {}
      close() { this.readyState = 'closed'; }
    }
    const closePeer = vi.fn();
    class FakePeerConnection extends EventTarget {
      connectionState: RTCPeerConnectionState = 'new';
      addTrack = vi.fn();
      createDataChannel() { return new FakeDataChannel() as unknown as RTCDataChannel; }
      async createOffer() {
        return { type: 'offer', sdp: 'pending offer' } as RTCSessionDescriptionInit;
      }
      async setLocalDescription() {}
      async setRemoteDescription() {}
      close() { closePeer(); this.connectionState = 'closed'; }
    }
    vi.stubGlobal('RTCPeerConnection', FakePeerConnection);
    const connect = vi.fn((_sdp: string, _languages: string[], signal: AbortSignal) => (
      new Promise<string>((_resolve, reject) => {
        signal.addEventListener('abort', () => (
          reject(new DOMException('Aborted', 'AbortError'))
        ));
      })
    ));
    const controller = new AbortController();
    const starting = VoiceInputSession.start(
      { provider: 'openai', model: 'gpt-4o-transcribe', delay: 'low', prompt: '' },
      () => {},
      () => {},
      connect,
      unusedXaiBridge,
      controller.signal,
    );
    await vi.waitFor(() => expect(connect).toHaveBeenCalledOnce());
    controller.abort();
    await expect(starting).rejects.toMatchObject({ name: 'AbortError' });
    expect(stopTrack).toHaveBeenCalledOnce();
    expect(closePeer).toHaveBeenCalledOnce();
  });

  it('checks microphone support for both providers and WebRTC only for OpenAI', () => {
    Object.defineProperty(navigator, 'mediaDevices', {
      configurable: true,
      value: { getUserMedia: vi.fn() },
    });
    vi.stubGlobal('RTCPeerConnection', class {});
    vi.stubGlobal('AudioContext', class {});
    vi.stubGlobal('AudioWorkletNode', class {});
    expect(VoiceInputSession.supported('openai')).toBe(true);
    expect(VoiceInputSession.supported('xai')).toBe(true);

    vi.stubGlobal('RTCPeerConnection', undefined);
    expect(VoiceInputSession.supported('openai')).toBe(false);
    expect(VoiceInputSession.supported('xai')).toBe(true);

    vi.stubGlobal('AudioWorkletNode', undefined);
    expect(VoiceInputSession.supported('xai')).toBe(false);
  });

  it('does not use the OpenAI WebRTC session when xAI capture is unavailable', async () => {
    const getUserMedia = vi.fn();
    Object.defineProperty(navigator, 'mediaDevices', {
      configurable: true,
      value: { getUserMedia },
    });
    const OpenAiStart = vi.spyOn(OpenAiVoiceInputSession, 'start');
    vi.stubGlobal('RTCPeerConnection', class {
      constructor() { throw new Error('OpenAI WebRTC should not start'); }
    });
    vi.stubGlobal('AudioContext', undefined);
    vi.stubGlobal('AudioWorkletNode', undefined);

    await expect(VoiceInputSession.start(
      {
        provider: 'xai',
        model: 'grok-voice-transcribe-2.0',
        delay: 'low',
        prompt: 'Do not send this prompt.',
      },
      () => {},
      () => {},
      vi.fn(),
      unusedXaiBridge,
    )).rejects.toThrow('Voice input is unavailable.');
    expect(OpenAiStart).not.toHaveBeenCalled();
    expect(getUserMedia).not.toHaveBeenCalled();
  });
});
