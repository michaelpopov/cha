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
      url: 'https://api.openai.com/v1/audio/transcriptions',
      apiKey: 'secret',
      model: 'gpt-4o-mini-transcribe',
      languages: ['ru', 'en'],
      keywords: ['запятая', 'comma'],
      blockDurationMs: 5_000,
    };
    expect(getVoiceInputConfiguration()).toEqual(window.chaVoiceInput);
    window.chaVoiceInput = {
      url: '', apiKey: 'secret', model: 'gpt-4o-mini-transcribe', blockDurationMs: 5_000,
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

  it('uploads ordered, self-contained blocks and flushes the final block', async () => {
    vi.useFakeTimers();
    const stopTrack = vi.fn();
    Object.defineProperty(navigator, 'mediaDevices', {
      configurable: true,
      value: { getUserMedia: vi.fn(async () => ({
        getTracks: () => [{ stop: stopTrack }],
      })) },
    });

    class FakeMediaRecorder {
      static isTypeSupported(type: string) { return type === 'audio/mp4'; }
      readonly mimeType: string;
      state: RecordingState = 'inactive';
      ondataavailable: ((event: BlobEvent) => void) | null = null;
      onerror: ((event: Event) => void) | null = null;
      onstop: ((event: Event) => void) | null = null;

      constructor(_stream: MediaStream, options?: MediaRecorderOptions) {
        this.mimeType = options?.mimeType ?? 'audio/mp4';
      }

      start() { this.state = 'recording'; }

      stop() {
        this.state = 'inactive';
        this.ondataavailable?.({ data: new Blob(['audio'], { type: this.mimeType }) } as BlobEvent);
        this.onstop?.(new Event('stop'));
      }
    }
    vi.stubGlobal('MediaRecorder', FakeMediaRecorder);
    const fetcher = vi.fn()
      .mockResolvedValueOnce({ ok: true, json: async () => ({ text: 'First block.' }) })
      .mockResolvedValueOnce({ ok: true, json: async () => ({ text: 'Second block.' }) });
    vi.stubGlobal('fetch', fetcher);
    const received: string[] = [];
    const session = await VoiceInputSession.start(
      {
        url: 'https://api.openai.com/v1/audio/transcriptions',
        apiKey: 'secret',
        model: 'gpt-transcribe',
        languages: ['ru', 'en'],
        keywords: ['восклицательный знак', 'exclamation sign'],
        blockDurationMs: 15_000,
      },
      (text) => received.push(text),
      () => {},
    );

    await vi.advanceTimersByTimeAsync(14_999);
    expect(received).toEqual([]);
    await vi.advanceTimersByTimeAsync(1);
    expect(received).toEqual(['First block.']);
    const firstRequest = fetcher.mock.calls[0];
    expect(firstRequest[0]).toBe('https://api.openai.com/v1/audio/transcriptions');
    expect(firstRequest[1].headers).toEqual({ Authorization: 'Bearer secret' });
    const firstBody = firstRequest[1].body as FormData;
    expect(firstBody.get('model')).toBe('gpt-transcribe');
    expect(firstBody.has('prompt')).toBe(false);
    expect(firstBody.get('response_format')).toBe('json');
    expect(firstBody.getAll('languages[]')).toEqual(['ru', 'en']);
    expect(firstBody.getAll('keywords[]')).toEqual([
      'восклицательный знак', 'exclamation sign',
    ]);
    expect((firstBody.get('file') as File).name).toBe('voice.mp4');

    await session.stop();
    expect(received).toEqual(['First block.', 'Second block.']);
    expect(fetcher).toHaveBeenCalledTimes(2);
    expect(stopTrack).toHaveBeenCalled();
  });
});
