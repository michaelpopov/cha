import { afterEach, describe, expect, it, vi } from 'vitest';

import { OpenAiVoiceInputSession } from './openAiVoiceInput';
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

afterEach(() => {
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
