import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  appendTranscription,
  getVoiceInputConfiguration,
  VoiceInputSession,
} from './voiceInput';

afterEach(() => {
  delete window.chaVoiceInput;
  Reflect.deleteProperty(navigator, 'mediaDevices');
  vi.useRealTimers();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('voice input', () => {
  it('reads only a complete native configuration', () => {
    expect(getVoiceInputConfiguration()).toBeNull();
    window.chaVoiceInput = {
      url: 'https://api.openai.com/v1/realtime/calls',
      apiKey: 'secret',
      model: 'gpt-live-transcribe',
      languages: ['ru', 'en'],
      keywords: ['запятая', 'comma'],
    };
    expect(getVoiceInputConfiguration()).toEqual(window.chaVoiceInput);
    window.chaVoiceInput = {
      url: '', apiKey: 'secret', model: 'gpt-live-transcribe',
    };
    expect(getVoiceInputConfiguration()).toBeNull();
  });

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
    const fetcher = vi.fn().mockResolvedValue({
      ok: true,
      text: async () => 'test answer',
    });
    vi.stubGlobal('fetch', fetcher);
    const received: string[] = [];
    const session = await VoiceInputSession.start(
      {
        url: 'https://api.openai.com/v1/realtime/calls',
        apiKey: 'secret',
        model: 'gpt-live-transcribe',
        languages: ['ru', 'en'],
        keywords: ['восклицательный знак', 'exclamation sign'],
      },
      (text) => received.push(text),
      () => {},
    );

    expect(fetcher).toHaveBeenCalledOnce();
    const request = fetcher.mock.calls[0];
    expect(request[0]).toBe('https://api.openai.com/v1/realtime/calls');
    expect(request[1].headers).toEqual({ Authorization: 'Bearer secret' });
    const body = request[1].body as FormData;
    expect(body.get('sdp')).toBe('test offer');
    expect(JSON.parse(body.get('session') as string)).toEqual({
      type: 'transcription',
      audio: {
        input: {
          transcription: {
            model: 'gpt-live-transcribe',
            languages: ['ru', 'en'],
            keywords: ['восклицательный знак', 'exclamation sign'],
            delay: 'low',
          },
          turn_detection: null,
        },
      },
    });

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
});
