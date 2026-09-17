import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import {
  TextToSpeechError,
  TextToSpeechSession,
} from './textToSpeech';

const configuration = {
  baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'voice',
  outputFormat: 'mp3', model: 's2.1-pro',
};

function playText(
  config: typeof configuration,
  voice: ConstructorParameters<typeof TextToSpeechSession>[1],
  text: string,
) {
  return new TextToSpeechSession(config, voice, text, vi.fn()).play();
}

beforeEach(() => {
  vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
  vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
  vi.stubGlobal('Audio', vi.fn(function Audio() {
    return { addEventListener: vi.fn(), play: vi.fn().mockResolvedValue(undefined), pause: vi.fn() };
  }));
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
  vi.useRealTimers();
});

describe('FishAudio connection budget', () => {
  const configuration = {
    baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-voice',
    outputFormat: 'mp3', model: 's2.1-pro',
  };

  function holdRequests() {
    let holding = true;
    const responses = new Map<string, (response: Response) => void>();
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation((_url, init) => {
      if (!holding) return Promise.resolve(new Response('audio'));
      const text = JSON.parse(init!.body as string).text as string;
      return new Promise<Response>((resolve) => { responses.set(text, resolve); });
    });
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), play: vi.fn().mockResolvedValue(undefined), pause: vi.fn() };
    }));
    return {
      fetchMock,
      responses,
      finish() {
        holding = false;
        for (const resolve of responses.values()) resolve(new Response('audio'));
      },
    };
  }

  it('keeps the connection slot until the audio body finishes, rather than only the headers', async () => {
    const controllers: ReadableStreamDefaultController<Uint8Array>[] = [];
    const requests = holdRequests();
    requests.fetchMock.mockImplementation(async () => controllers.length < 3
      ? new Response(new ReadableStream<Uint8Array>({ start(controller) { controllers.push(controller); } }))
      : new Response('audio'),
    );
    const sessions = ['One', 'Two', 'Three', 'Four'].map((text) =>
      new TextToSpeechSession(configuration, undefined, text, vi.fn()),
    );
    const playing = sessions.map((session) => session.play());
    try {
      await vi.waitFor(() => expect(controllers).toHaveLength(3));
      expect(requests.fetchMock).toHaveBeenCalledTimes(3);
      controllers.shift()!.close();
      await vi.waitFor(() => expect(requests.fetchMock).toHaveBeenCalledTimes(4));
    } finally {
      for (const controller of controllers) controller.close();
      await Promise.allSettled(playing);
      for (const session of sessions) session.stop();
    }
  });


});

describe('FishAudio busy retries', () => {
  const configuration = {
    baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-voice',
    outputFormat: 'mp3', model: 's2.1-pro',
  };
  const busy = () => new Response(JSON.stringify({
    error: { code: 'speech_busy', message: 'Speech generation is busy. Try again shortly.' },
  }), { status: 503 });

  it('retries local capacity rejections and requests audio again on later playback', async () => {
    vi.useFakeTimers();
    const fetchMock = vi.spyOn(globalThis, 'fetch')
      .mockImplementationOnce(async () => busy())
      .mockImplementationOnce(async () => busy())
      .mockImplementation(async () => new Response('audio'));
    const caching = playText(configuration, undefined, 'Hello');
    await vi.advanceTimersByTimeAsync(2000);
    await caching;
    await playText(configuration, undefined, 'Hello');
    expect(fetchMock).toHaveBeenCalledTimes(4);
  });

  it('reports a persistent busy error after three retries', async () => {
    vi.useFakeTimers();
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () => busy());
    const caching = playText(configuration, undefined, 'Hello');
    const failed = expect(caching).rejects.toMatchObject({ name: 'TextToSpeechError', code: 'speech_busy' });
    await vi.advanceTimersByTimeAsync(3000);
    await failed;
    expect(fetchMock).toHaveBeenCalledTimes(4);
    expect(vi.getTimerCount()).toBe(0);
  });

  it.each([401, 402, 503])('does not retry provider failures (HTTP %i)', async (status) => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () => new Response(
      JSON.stringify({ error: { code: 'internal_error', message: 'Provider rejected the request' } }), { status },
    ));
    await expect(playText(configuration, undefined, 'Hello')).rejects.toThrow('Provider rejected the request');
    expect(fetchMock).toHaveBeenCalledOnce();
  });

  it('stops retrying when a preview is canceled during the delay', async () => {
    vi.useFakeTimers();
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () => busy());
    const session = new TextToSpeechSession(configuration, undefined, 'Hello', vi.fn());
    const playing = session.play();
    const canceled = expect(playing).rejects.toMatchObject({ name: 'AbortError' });
    await vi.advanceTimersByTimeAsync(100);
    session.stop();
    await canceled;
    await vi.advanceTimersByTimeAsync(3000);
    expect(fetchMock).toHaveBeenCalledOnce();
    expect(vi.getTimerCount()).toBe(0);
  });
});

describe('playback position', () => {
  const audios: Array<EventTarget & {
    currentTime: number; duration: number; readyState: number;
    play: ReturnType<typeof vi.fn>; pause: ReturnType<typeof vi.fn>;
  }> = [];

  beforeEach(() => {
    audios.length = 0;
    vi.spyOn(globalThis, 'fetch').mockImplementation(async () => new Response('audio'));
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      const audio = Object.assign(new EventTarget(), {
        currentTime: 0, duration: 60, readyState: 0,
        play: vi.fn().mockResolvedValue(undefined), pause: vi.fn(),
      });
      audios.push(audio);
      return audio;
    }));
  });

  it('saves the stop position, resumes after metadata loads, and resets after ending', async () => {
    let position = 0;
    const ended = vi.fn();
    const createSession = () => new TextToSpeechSession(
      configuration, undefined, 'Hello', ended,
      { position, onPositionChange: (next) => { position = next; } },
    );
    const first = createSession();
    await first.play();
    audios[0].readyState = 1;
    audios[0].currentTime = 12.5;
    first.stop();
    expect(position).toBe(12.5);

    const resumed = createSession();
    await resumed.play();
    expect(audios[1].currentTime).toBe(0);
    audios[1].readyState = 1;
    audios[1].dispatchEvent(new Event('loadedmetadata'));
    expect(audios[1].currentTime).toBe(12.5);
    audios[1].currentTime = 60;
    audios[1].dispatchEvent(new Event('ended'));
    expect(position).toBe(0);
    expect(ended).toHaveBeenCalledOnce();

    const restarted = createSession();
    await restarted.play();
    expect(audios[2].currentTime).toBe(0);
    restarted.stop();
  });

  it('preserves the saved position when stopped before metadata arrives', async () => {
    const onPositionChange = vi.fn();
    const session = new TextToSpeechSession(
      configuration, undefined, 'Hello', vi.fn(),
      { position: 12.5, onPositionChange },
    );
    await session.play();
    session.stop();
    audios[0].readyState = 1;
    audios[0].dispatchEvent(new Event('loadedmetadata'));
    expect(onPositionChange).not.toHaveBeenCalled();
    expect(audios[0].currentTime).toBe(0);
  });

  it('starts at zero when a remembered position is beyond the audio duration', async () => {
    const session = new TextToSpeechSession(
      configuration, undefined, 'Hello', vi.fn(),
      { position: 70, onPositionChange: vi.fn() },
    );
    await session.play();
    audios[0].readyState = 1;
    audios[0].dispatchEvent(new Event('loadedmetadata'));
    expect(audios[0].currentTime).toBe(0);
    session.stop();
  });
});

describe('text to speech', () => {
  it('fetches committed audio on every playback without a preview request and releases temporary audio', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () => new Response('audio'));
    const url = '/api/v1/forums/lobby/sessions/chat/entries/2/audio?vault_name=Personal';
    const onCached = vi.fn();
    for (let i = 0; i < 2; i += 1) {
      const session = new TextToSpeechSession(null, undefined, '', vi.fn(), undefined, url, onCached);
      await session.play();
      session.stop();
    }
    expect(fetchMock).toHaveBeenCalledTimes(2);
    for (const [input, init] of fetchMock.mock.calls) {
      expect(input).toBe(url);
      expect(init?.method).toBeUndefined();
      expect(init?.body).toBeUndefined();
    }
    expect(onCached).toHaveBeenCalledTimes(2);
    expect(URL.revokeObjectURL).toHaveBeenCalledTimes(2);
  });

  it('labels cached-playback errors without referring to FishAudio', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify({
      error: { code: 'vault_changed', message: 'The active vault changed.' },
    }), { status: 409 }));
    const session = new TextToSpeechSession(null, undefined, '', vi.fn(), undefined, '/cached-audio');
    await expect(session.play()).rejects.toMatchObject({
      message: 'Cached audio: The active vault changed. (HTTP 409)', status: 409, code: 'vault_changed',
    });
  });

  it('uses a cached-playback fallback message for a non-JSON error', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response('Not found', { status: 404 }));
    const session = new TextToSpeechSession(null, undefined, '', vi.fn(), undefined, '/cached-audio');
    await expect(session.play()).rejects.toMatchObject({ message: 'Cached audio request failed (HTTP 404).', status: 404 });
  });

  it('aborts a pending FishAudio preview when stopped', async () => {
    let signal: AbortSignal | undefined;
    vi.spyOn(globalThis, 'fetch').mockImplementation((_url, init) => {
      signal = init?.signal as AbortSignal;
      return new Promise((_resolve, reject) => {
        signal!.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
      });
    });
    const session = new TextToSpeechSession({
      baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'voice',
      outputFormat: 'mp3', model: 's2.1-pro',
    }, undefined, 'Hello', vi.fn());
    const playing = session.play();
    session.stop();
    expect(signal?.aborted).toBe(true);
    await expect(playing).rejects.toMatchObject({ name: 'AbortError' });
  });
  it('forwards FishAudio synthesis through CHA with the assigned voice', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      new Response(new TextEncoder().encode('audio'), { headers: { 'Content-Type': 'audio/mpeg' } }),
    );
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:fish');
    const play = vi.fn().mockResolvedValue(undefined);
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), play };
    }));
    await new TextToSpeechSession({
      baseUrl: 'https://api.fish.audio/v1/tts',
      voiceId: 'fallback', outputFormat: 'mp3', model: 's2.1-pro',
    }, { elevenlabs_voice_id: 'fish-voice', settings: {
      speed: 0.9,
    } }, 'Hello', vi.fn()).play();
    expect(fetchMock).toHaveBeenCalledWith('/api/v1/voice-output/audio', expect.objectContaining({
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: 'Hello', reference_id: 'fish-voice', settings: { speed: 0.9 } }),
    }));
    expect(play).toHaveBeenCalledOnce();
  });

  it.each([
    { status: 402, message: 'Insufficient credits', reason: 'balance' },
    { error: { message: 'Insufficient credits' } },
  ])('exposes FishAudio and CHA error messages (%j)', async (error) => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify(error), { status: 402 }));
    await expect(playText({
      baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-default',
      outputFormat: 'mp3', model: 's2.1-pro',
    }, undefined, 'Hello')).rejects.toThrow('FishAudio: Insufficient credits (HTTP 402)');
  });

  it('shows the authentication fallback supplied by the FishAudio proxy', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify({
      error: { message: 'Authentication failed. Check the FishAudio API key.' },
    }), { status: 401 }));
    await expect(playText({
      baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-default',
      outputFormat: 'mp3', model: 's2.1-pro',
    }, undefined, 'Hello')).rejects.toThrow(
      'FishAudio: Authentication failed. Check the FishAudio API key. (HTTP 401)',
    );
  });

  it('uses a useful fallback when speech output does not return JSON', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response('Bad gateway', { status: 502 }));
    await expect(playText({
      baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-default',
      outputFormat: 'mp3', model: 's2.1-pro',
    }, undefined, 'Hello')).rejects.toThrow('FishAudio request failed (HTTP 502).');
  });

  it('requests FishAudio audio and plays it', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      new Response(new TextEncoder().encode('audio'), {
        headers: { 'Content-Type': 'audio/mpeg' },
      }),
    );
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    const play = vi.fn().mockResolvedValue(undefined);
    const pause = vi.fn();
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), play, pause };
    }));
    const session = new TextToSpeechSession(
      {
        baseUrl: 'https://api.fish.audio/v1/tts',
        voiceId: 'fallback/voice',
        outputFormat: 'mp3 44',
        model: 's2.1-pro',
      },
      undefined,
      'Read this',
      vi.fn(),
    );

    await session.play();

    expect(fetchMock).toHaveBeenCalledWith(
      '/api/v1/voice-output/audio',
      expect.objectContaining({
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ text: 'Read this', reference_id: 'fallback/voice', settings: {} }),
      }),
    );
    expect(play).toHaveBeenCalledOnce();
  });

  it('always requests fresh previews without transcript identity', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () =>
      new Response(new TextEncoder().encode('audio')),
    );
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return {
        addEventListener: vi.fn(),
        play: vi.fn().mockResolvedValue(undefined),
      };
    }));
    const configuration = {
      baseUrl: 'https://api.fish.audio/v1/tts',
      voiceId: 'preview-voice',
      outputFormat: 'mp3',
      model: 's2.1-pro',
    };

    await new TextToSpeechSession(
      configuration, undefined, 'Preview this', vi.fn(),
    ).play();
    await new TextToSpeechSession(
      configuration, undefined, 'Preview this', vi.fn(),
    ).play();

    expect(fetchMock).toHaveBeenCalledTimes(2);
    for (const [, init] of fetchMock.mock.calls) {
      expect(JSON.parse(init!.body as string)).not.toHaveProperty('entry');
    }
  });

  it('omits voice settings for an assigned voice with no overrides', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      new Response(new TextEncoder().encode('audio')),
    );
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), play: vi.fn().mockResolvedValue(undefined) };
    }));
    const session = new TextToSpeechSession(
      {
        baseUrl: 'https://api.fish.audio/v1/tts',
        voiceId: 'fallback',
        outputFormat: 'mp3',
        model: 's2.1-pro',
      },
      { elevenlabs_voice_id: 'plain-voice', settings: {} },
      'Read this',
      vi.fn(),
    );

    await session.play();

    expect(fetchMock).toHaveBeenCalledWith(
      '/api/v1/voice-output/audio',
      expect.objectContaining({
        body: JSON.stringify({ text: 'Read this', reference_id: 'plain-voice', settings: {} }),
      }),
    );
  });

});
