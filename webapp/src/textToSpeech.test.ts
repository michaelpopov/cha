import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  cacheTextToSpeech,
  clearTextToSpeechCache,
  TextToSpeechError,
  TextToSpeechSession,
} from './textToSpeech';

afterEach(() => {
  clearTextToSpeechCache();
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
  vi.useRealTimers();
});

describe('FishAudio connection budget', () => {
  const configuration = {
    baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-voice',
    outputFormat: 'mp3', apiKey: '', model: 's2.1-pro',
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

  it('shares two prefetch slots across cache runs and promotes a queued clip for playback', async () => {
    const requests = holdRequests();
    const caching = ['Old 1', 'Old 2', 'New 1', 'New 2'].map((text) =>
      cacheTextToSpeech(configuration, undefined, text),
    );
    const session = new TextToSpeechSession(configuration, undefined, 'New 2', vi.fn(), { cache: true });
    const preview = new TextToSpeechSession(configuration, undefined, 'Preview', vi.fn());
    let playing: Promise<void> | undefined;
    try {
      expect(requests.fetchMock).toHaveBeenCalledTimes(2);
      playing = session.play();
      expect([...requests.responses.keys()]).toEqual(['Old 1', 'Old 2', 'New 2']);
      const previewing = preview.play();
      expect(requests.fetchMock).toHaveBeenCalledTimes(3);
      preview.stop();
      await expect(previewing).rejects.toMatchObject({ name: 'AbortError' });
      expect(requests.responses.has('Preview')).toBe(false);
    } finally {
      requests.finish();
      await Promise.allSettled([...caching, ...(playing ? [playing] : [])]);
      session.stop();
      preview.stop();
    }
  });

  it('starts queued playback before background caching and releases capacity after errors', async () => {
    const requests = holdRequests();
    const caching = ['Cache 1', 'Cache 2', 'Cache 3'].map((text) =>
      cacheTextToSpeech(configuration, undefined, text),
    );
    const first = new TextToSpeechSession(configuration, undefined, 'First preview', vi.fn());
    const second = new TextToSpeechSession(configuration, undefined, 'Second preview', vi.fn());
    const failing = first.play();
    const failed = expect(failing).rejects.toThrow('FishAudio: No credits (HTTP 402)');
    const playing = second.play();
    try {
      expect(requests.fetchMock).toHaveBeenCalledTimes(3);
      requests.responses.get('First preview')!(new Response(JSON.stringify({ message: 'No credits' }), { status: 402 }));
      await failed;
      await vi.waitFor(() => expect(requests.responses.has('Second preview')).toBe(true));
      expect(requests.responses.has('Cache 3')).toBe(false);
      expect(requests.fetchMock).toHaveBeenCalledTimes(4);
    } finally {
      requests.finish();
      await Promise.allSettled([...caching, failing, playing]);
      first.stop();
      second.stop();
    }
  });

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

  it('discards abandoned queued prefetch, leaves active requests running, and allows a fresh attempt', async () => {
    const requests = holdRequests();
    const controller = new AbortController();
    const caching = ['Active 1', 'Active 2', 'Queued'].map((text) =>
      cacheTextToSpeech(configuration, undefined, text, controller.signal),
    );
    const discarded = expect(caching[2]).rejects.toMatchObject({ name: 'AbortError' });
    let fresh: Promise<void> | undefined;
    try {
      controller.abort();
      fresh = cacheTextToSpeech(configuration, undefined, 'Queued');
      await discarded;
      expect(requests.fetchMock).toHaveBeenCalledTimes(2);
      requests.responses.get('Active 1')!(new Response('audio'));
      await vi.waitFor(() => expect(requests.responses.has('Queued')).toBe(true));
      expect(requests.fetchMock).toHaveBeenCalledTimes(3);
    } finally {
      requests.finish();
      await Promise.allSettled([...caching, ...(fresh ? [fresh] : [])]);
    }
  });

  it('keeps queued prefetch shared with another cache run after its first owner leaves', async () => {
    const requests = holdRequests();
    const first = new AbortController();
    const second = new AbortController();
    const caching = ['Active 1', 'Active 2', 'Shared'].map((text) =>
      cacheTextToSpeech(configuration, undefined, text, first.signal),
    );
    caching.push(cacheTextToSpeech(configuration, undefined, 'Shared', second.signal));
    try {
      first.abort();
      requests.responses.get('Active 1')!(new Response('audio'));
      await vi.waitFor(() => expect(requests.responses.has('Shared')).toBe(true));
      expect(requests.fetchMock).toHaveBeenCalledTimes(3);
    } finally {
      requests.finish();
      await Promise.allSettled(caching);
    }
  });

  it('keeps queued playback when its cache run stops', async () => {
    const requests = holdRequests();
    const previews = ['One', 'Two', 'Three'].map((text) =>
      new TextToSpeechSession(configuration, undefined, text, vi.fn()),
    );
    const playing = previews.map((session) => session.play());
    const controller = new AbortController();
    const caching = cacheTextToSpeech(configuration, undefined, 'Shared', controller.signal);
    const playback = new TextToSpeechSession(configuration, undefined, 'Shared', vi.fn(), { cache: true });
    playing.push(playback.play());
    try {
      controller.abort();
      requests.responses.get('One')!(new Response('audio'));
      await vi.waitFor(() => expect(requests.responses.has('Shared')).toBe(true));
      expect(requests.fetchMock).toHaveBeenCalledTimes(4);
    } finally {
      requests.finish();
      await Promise.allSettled([...playing, caching]);
      for (const session of [...previews, playback]) session.stop();
    }
  });
});

describe('FishAudio busy retries', () => {
  const configuration = {
    baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-voice',
    outputFormat: 'mp3', apiKey: '', model: 's2.1-pro',
  };
  const busy = () => new Response(JSON.stringify({
    error: { code: 'speech_busy', message: 'Speech generation is busy. Try again shortly.' },
  }), { status: 503 });

  it('retries local capacity rejections and caches the successful result', async () => {
    vi.useFakeTimers();
    const fetchMock = vi.spyOn(globalThis, 'fetch')
      .mockImplementationOnce(async () => busy())
      .mockImplementationOnce(async () => busy())
      .mockImplementation(async () => new Response('audio'));
    const caching = cacheTextToSpeech(configuration, undefined, 'Hello');
    await vi.advanceTimersByTimeAsync(2000);
    await caching;
    await cacheTextToSpeech(configuration, undefined, 'Hello');
    expect(fetchMock).toHaveBeenCalledTimes(3);
  });

  it('reports a persistent busy error after three retries', async () => {
    vi.useFakeTimers();
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () => busy());
    const caching = cacheTextToSpeech(configuration, undefined, 'Hello');
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
    await expect(cacheTextToSpeech(configuration, undefined, 'Hello')).rejects.toThrow('Provider rejected the request');
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

describe('text to speech', () => {
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
      outputFormat: 'mp3', apiKey: 'secret', model: 's2.1-pro',
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
      voiceId: 'fallback', outputFormat: 'mp3_44100_128',
      apiKey: 'secret', model: 'eleven_multilingual_v2',
    }, { elevenlabs_voice_id: 'fish-voice', settings: {
      speed: 0.9, stability: 0.5, similarity_boost: 0.7, style: 0.8, use_speaker_boost: true,
    } }, 'Hello', vi.fn()).play();
    expect(fetchMock).toHaveBeenCalledWith('/api/v1/voice-output/audio', expect.objectContaining({
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: 'Hello', reference_id: 'fish-voice', settings: { speed: 0.9 } }),
    }));
    expect(play).toHaveBeenCalledOnce();
  });

  it('invalidates FishAudio cached clips when model or format changes', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () =>
      new Response(new TextEncoder().encode('audio')),
    );
    const configuration = {
      baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-default',
      outputFormat: 'mp3', apiKey: 'secret', model: 's2.1-pro',
    };
    await cacheTextToSpeech(configuration, undefined, 'Hello');
    await cacheTextToSpeech(configuration, undefined, 'Hello');
    await cacheTextToSpeech({ ...configuration, model: 's2-pro' }, undefined, 'Hello');
    await cacheTextToSpeech({ ...configuration, outputFormat: 'wav' }, undefined, 'Hello');
    expect(fetchMock).toHaveBeenCalledTimes(3);
    expect(JSON.parse(fetchMock.mock.calls[0][1]!.body as string).reference_id).toBe('fish-default');
  });

  it('ignores ElevenLabs-only settings in FishAudio requests and cache keys', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () => new Response('audio'));
    const configuration = {
      baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fallback',
      outputFormat: 'mp3', apiKey: '', model: 's2.1-pro',
    };
    await cacheTextToSpeech(configuration, {
      elevenlabs_voice_id: 'voice', settings: { stability: 0.5, use_speaker_boost: true },
    }, 'Hello');
    await cacheTextToSpeech(configuration, {
      elevenlabs_voice_id: 'voice', settings: { stability: 0.8, use_speaker_boost: false },
    }, 'Hello');
    expect(fetchMock).toHaveBeenCalledOnce();
    expect(JSON.parse(fetchMock.mock.calls[0][1]!.body as string).settings).toEqual({});
  });

  it.each([
    { status: 402, message: 'Insufficient credits', reason: 'balance' },
    { error: { message: 'Insufficient credits' } },
  ])('exposes FishAudio and CHA error messages (%j)', async (error) => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify(error), { status: 402 }));
    await expect(cacheTextToSpeech({
      baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-default',
      outputFormat: 'mp3', apiKey: 'secret', model: 's2.1-pro',
    }, undefined, 'Hello')).rejects.toThrow('FishAudio: Insufficient credits (HTTP 402)');
  });

  it('shows the authentication fallback supplied by the FishAudio proxy', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify({
      error: { message: 'Authentication failed. Check the FishAudio API key.' },
    }), { status: 401 }));
    await expect(cacheTextToSpeech({
      baseUrl: 'https://api.fish.audio/v1/tts', voiceId: 'fish-default',
      outputFormat: 'mp3', apiKey: '', model: 's2.1-pro',
    }, undefined, 'Hello')).rejects.toThrow(
      'FishAudio: Authentication failed. Check the FishAudio API key. (HTTP 401)',
    );
  });

  it('requests ElevenLabs audio and plays it', async () => {
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
        baseUrl: 'https://example.com/speech',
        voiceId: 'fallback/voice',
        outputFormat: 'mp3 44',
        apiKey: 'secret',
        model: 'multilingual',
      },
      undefined,
      'Read this',
      vi.fn(),
    );

    await session.play();

    expect(fetchMock).toHaveBeenCalledWith(
      'https://example.com/speech/fallback%2Fvoice?output_format=mp3%2044',
      expect.objectContaining({
        method: 'POST',
        headers: expect.objectContaining({ 'xi-api-key': 'secret' }),
        body: JSON.stringify({ text: 'Read this', model_id: 'multilingual' }),
      }),
    );
    expect(play).toHaveBeenCalledOnce();
  });

  it('reuses cached audio for the same synthesis request', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () =>
      new Response(new TextEncoder().encode('audio')),
    );
    vi.spyOn(URL, 'createObjectURL')
      .mockReturnValueOnce('blob:first')
      .mockReturnValueOnce('blob:second');
    const play = vi.fn().mockResolvedValue(undefined);
    const pause = vi.fn();
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), play, pause };
    }));
    const configuration = {
      baseUrl: 'https://example.com/speech',
      voiceId: 'cached-voice',
      outputFormat: 'mp3',
      apiKey: 'secret',
      model: 'multilingual',
    };

    const first = new TextToSpeechSession(
      configuration, undefined, 'Read this again', vi.fn(), { cache: true },
    );
    await first.play();
    first.stop();
    const second = new TextToSpeechSession(
      configuration, undefined, 'Read this again', vi.fn(), { cache: true },
    );
    await second.play();

    expect(fetchMock).toHaveBeenCalledOnce();
    expect(URL.createObjectURL).toHaveBeenCalledTimes(2);
    expect(play).toHaveBeenCalledTimes(2);
  });

  it('shares an in-flight cached request with playback', async () => {
    let resolveResponse!: (response: Response) => void;
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(() => (
      new Promise<Response>((resolve) => { resolveResponse = resolve; })
    ));
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    const play = vi.fn().mockResolvedValue(undefined);
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), play, pause: vi.fn() };
    }));
    const configuration = {
      baseUrl: 'https://example.com/speech',
      voiceId: 'cached-voice',
      outputFormat: 'mp3',
      apiKey: 'secret',
      model: 'multilingual',
    };

    const warming = cacheTextToSpeech(configuration, undefined, 'Same clip');
    const session = new TextToSpeechSession(
      configuration, undefined, 'Same clip', vi.fn(), { cache: true },
    );
    const playing = session.play();

    expect(fetchMock).toHaveBeenCalledOnce();
    resolveResponse(new Response(new TextEncoder().encode('audio')));
    await Promise.all([warming, playing]);
    expect(play).toHaveBeenCalledOnce();
  });

  it('does not cache audio unless requested', async () => {
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
      baseUrl: 'https://example.com/speech',
      voiceId: 'preview-voice',
      outputFormat: 'mp3',
      apiKey: 'secret',
      model: 'multilingual',
    };

    await new TextToSpeechSession(
      configuration, undefined, 'Preview this', vi.fn(),
    ).play();
    await new TextToSpeechSession(
      configuration, undefined, 'Preview this', vi.fn(),
    ).play();

    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it('generates new audio when synthesis settings change', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () =>
      new Response(new TextEncoder().encode('audio')),
    );
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return {
        addEventListener: vi.fn(),
        play: vi.fn().mockResolvedValue(undefined),
        pause: vi.fn(),
      };
    }));
    const configuration = {
      baseUrl: 'https://example.com/speech',
      voiceId: 'fallback',
      outputFormat: 'mp3',
      apiKey: 'secret',
      model: 'multilingual',
    };

    await new TextToSpeechSession(
      configuration,
      { elevenlabs_voice_id: 'warm-voice', settings: { speed: 0.9 } },
      'Read this',
      vi.fn(),
      { cache: true },
    ).play();
    await new TextToSpeechSession(
      configuration,
      { elevenlabs_voice_id: 'warm-voice', settings: { speed: 1.1 } },
      'Read this',
      vi.fn(),
      { cache: true },
    ).play();

    expect(fetchMock).toHaveBeenCalledTimes(2);
  });

  it('evicts the oldest audio when the cache exceeds 256 MiB', async () => {
    const largeBlob = { size: 128 * 1024 * 1024 } as Blob;
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockImplementation(async () => ({
      ok: true,
      blob: async () => largeBlob,
    }) as Response);
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return {
        addEventListener: vi.fn(),
        play: vi.fn().mockResolvedValue(undefined),
        pause: vi.fn(),
      };
    }));
    const configuration = {
      baseUrl: 'https://example.com/speech',
      voiceId: 'cached-voice',
      outputFormat: 'mp3',
      apiKey: 'secret',
      model: 'multilingual',
    };

    for (let index = 0; index < 3; index += 1) {
      const session = new TextToSpeechSession(
        configuration, undefined, `Clip ${index}`, vi.fn(), { cache: true },
      );
      await session.play();
      session.stop();
    }
    const retained = new TextToSpeechSession(
      configuration, undefined, 'Clip 1', vi.fn(), { cache: true },
    );
    await retained.play();
    expect(fetchMock).toHaveBeenCalledTimes(3);

    const replay = new TextToSpeechSession(
      configuration, undefined, 'Clip 0', vi.fn(), { cache: true },
    );
    await replay.play();

    expect(fetchMock).toHaveBeenCalledTimes(4);
  });

  it('uses an assigned voice and its configured settings', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(
      new Response(new TextEncoder().encode('audio')),
    );
    vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
    vi.stubGlobal('Audio', vi.fn(function Audio() {
      return { addEventListener: vi.fn(), play: vi.fn().mockResolvedValue(undefined) };
    }));
    const session = new TextToSpeechSession(
      {
        baseUrl: 'https://example.com/speech',
        voiceId: 'fallback',
        outputFormat: 'mp3',
        apiKey: 'secret',
        model: 'multilingual',
      },
      {
        elevenlabs_voice_id: 'warm voice',
        settings: { stability: 0.45, speed: 0.95 },
      },
      'Read this',
      vi.fn(),
    );

    await session.play();

    expect(fetchMock).toHaveBeenCalledWith(
      'https://example.com/speech/warm%20voice?output_format=mp3',
      expect.objectContaining({
        body: JSON.stringify({
          text: 'Read this',
          model_id: 'multilingual',
          voice_settings: { stability: 0.45, speed: 0.95 },
        }),
      }),
    );
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
        baseUrl: 'https://example.com/speech',
        voiceId: 'fallback',
        outputFormat: 'mp3',
        apiKey: 'secret',
        model: 'multilingual',
      },
      { elevenlabs_voice_id: 'plain-voice', settings: {} },
      'Read this',
      vi.fn(),
    );

    await session.play();

    expect(fetchMock).toHaveBeenCalledWith(
      'https://example.com/speech/plain-voice?output_format=mp3',
      expect.objectContaining({
        body: JSON.stringify({ text: 'Read this', model_id: 'multilingual' }),
      }),
    );
  });

  it('exposes the ElevenLabs error message and HTTP status', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify({
      detail: {
        message: 'The selected voice cannot process this request.',
      },
    }), {
      status: 422,
      headers: { 'Content-Type': 'application/json' },
    }));
    const session = new TextToSpeechSession(
      {
        baseUrl: 'https://example.com/speech',
        voiceId: 'fallback',
        outputFormat: 'mp3',
        apiKey: 'secret',
        model: 'multilingual',
      },
      { elevenlabs_voice_id: 'assigned-voice', settings: {} },
      'Read this',
      vi.fn(),
    );

    await expect(session.play()).rejects.toEqual(new TextToSpeechError(
      'ElevenLabs: The selected voice cannot process this request. (HTTP 422)',
    ));
  });

  it('uses a useful fallback when ElevenLabs does not return JSON', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response('Bad gateway', {
      status: 502,
      headers: { 'Content-Type': 'text/plain' },
    }));
    const session = new TextToSpeechSession(
      {
        baseUrl: 'https://example.com/speech',
        voiceId: 'fallback',
        outputFormat: 'mp3',
        apiKey: 'secret',
        model: 'multilingual',
      },
      undefined,
      'Read this',
      vi.fn(),
    );

    await expect(session.play()).rejects.toThrow(
      'ElevenLabs request failed (HTTP 502).',
    );
  });
});
