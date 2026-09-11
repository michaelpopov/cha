import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  clearTextToSpeechCache,
  getTextToSpeechConfiguration,
  TextToSpeechError,
  TextToSpeechSession,
} from './textToSpeech';

afterEach(() => {
  clearTextToSpeechCache();
  delete window.chaTextToSpeech;
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('text to speech', () => {
  it('is available only when the native shell injects a complete configuration', () => {
    expect(getTextToSpeechConfiguration()).toBeNull();
    window.chaTextToSpeech = {
      baseUrl: 'https://example.com',
      voiceId: 'fallback',
      outputFormat: 'mp3',
      apiKey: 'key',
      model: 'model',
    };
    expect(getTextToSpeechConfiguration()).toEqual(window.chaTextToSpeech);
  });

  it('rejects the legacy configuration shape', () => {
    window.chaTextToSpeech = {
      url: 'https://example.com/speech',
      apiKey: 'key',
      model: 'model',
    } as unknown as typeof window.chaTextToSpeech;

    expect(getTextToSpeechConfiguration()).toBeNull();
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
