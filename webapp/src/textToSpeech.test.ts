import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import { TextToSpeechSession } from './textToSpeech';

type TestAudio = EventTarget & {
  currentTime: number;
  duration: number;
  readyState: number;
  play: ReturnType<typeof vi.fn>;
  pause: ReturnType<typeof vi.fn>;
};

const audios: TestAudio[] = [];

beforeEach(() => {
  audios.length = 0;
  vi.spyOn(URL, 'createObjectURL').mockReturnValue('blob:audio');
  vi.spyOn(URL, 'revokeObjectURL').mockImplementation(() => {});
  vi.stubGlobal('Audio', vi.fn(function Audio() {
    const audio = Object.assign(new EventTarget(), {
      currentTime: 0,
      duration: 60,
      readyState: 1,
      play: vi.fn().mockResolvedValue(undefined),
      pause: vi.fn(),
    });
    audios.push(audio);
    return audio;
  }));
});

afterEach(() => {
  vi.unstubAllGlobals();
  vi.restoreAllMocks();
});

describe('text to speech', () => {
  it('loads native speech through a local resource and releases it', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch')
      .mockImplementation(async () => new Response('audio'));
    const release = vi.fn(async () => undefined);
    const preview = vi.fn(async () => ({ url: '/media/r9', resource_id: 'r9' }));
    const voice = { elevenlabs_voice_id: 'voice', settings: { speed: 0.9 } };
    const session = new TextToSpeechSession(
      null, voice, 'Hello', vi.fn(), undefined, undefined, undefined, { preview, release },
    );

    await session.play();

    expect(preview).toHaveBeenCalledWith('Hello', voice, expect.any(AbortSignal));
    expect(fetchMock).toHaveBeenCalledWith('/media/r9', expect.objectContaining({
      cache: 'no-store',
    }));
    expect(audios[0].play).toHaveBeenCalledOnce();
    session.stop();
    expect(release).toHaveBeenCalledWith('r9');
    expect(URL.revokeObjectURL).toHaveBeenCalledWith('blob:audio');
  });

  it('aborts a pending native preview when stopped', async () => {
    let signal: AbortSignal | undefined;
    const preview = vi.fn((_text, _voice, pending: AbortSignal) => {
      signal = pending;
      return new Promise<{ url: string; resource_id: string }>((_resolve, reject) => {
        pending.addEventListener('abort', () => reject(new DOMException('Aborted', 'AbortError')));
      });
    });
    const session = new TextToSpeechSession(
      null, undefined, 'Hello', vi.fn(), undefined, undefined, undefined,
      { preview, release: vi.fn(async () => undefined) },
    );

    const playing = session.play();
    session.stop();

    expect(signal?.aborted).toBe(true);
    await expect(playing).rejects.toMatchObject({ name: 'AbortError' });
  });

  it('fetches committed audio on every playback without requesting a preview', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch')
      .mockImplementation(async () => new Response('audio'));
    const onCached = vi.fn();
    for (let index = 0; index < 2; index += 1) {
      const session = new TextToSpeechSession(
        null, undefined, '', vi.fn(), undefined, '/media/cached-r2', onCached,
      );
      await session.play();
      session.stop();
    }

    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(fetchMock).toHaveBeenCalledWith('/media/cached-r2', expect.objectContaining({
      cache: 'no-store',
    }));
    expect(onCached).toHaveBeenCalledTimes(2);
  });

  it('reports local resource errors without referring to a provider', async () => {
    vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(JSON.stringify({
      error: { code: 'vault_changed', message: 'The active vault changed.' },
    }), { status: 409 }));
    const session = new TextToSpeechSession(
      null, undefined, '', vi.fn(), undefined, '/media/cached-r2',
    );

    await expect(session.play()).rejects.toMatchObject({
      message: 'Audio: The active vault changed. (HTTP 409)',
      status: 409,
      code: 'vault_changed',
    });
  });

  it('reports unavailable speech without committed or native audio', async () => {
    const session = new TextToSpeechSession(null, undefined, 'Hello', vi.fn());
    await expect(session.play()).rejects.toMatchObject({
      name: 'TextToSpeechError',
      message: 'Voice output is unavailable.',
    });
  });
});

describe('playback position', () => {
  beforeEach(() => {
    vi.spyOn(globalThis, 'fetch').mockImplementation(async () => new Response('audio'));
  });

  it('saves the stop position, resumes, and resets after ending', async () => {
    let position = 0;
    const ended = vi.fn();
    const createSession = () => new TextToSpeechSession(
      null, undefined, '', ended,
      { position, onPositionChange: (next) => { position = next; } },
      '/media/cached-r2',
    );
    const first = createSession();
    await first.play();
    audios[0].currentTime = 12.5;
    first.stop();
    expect(position).toBe(12.5);

    const resumed = createSession();
    await resumed.play();
    expect(audios[1].currentTime).toBe(12.5);
    audios[1].dispatchEvent(new Event('ended'));
    expect(position).toBe(0);
    expect(ended).toHaveBeenCalledOnce();
  });

  it('starts at zero when a remembered position exceeds the duration', async () => {
    const session = new TextToSpeechSession(
      null, undefined, '', vi.fn(),
      { position: 70, onPositionChange: vi.fn() },
      '/media/cached-r2',
    );
    await session.play();
    expect(audios[0].currentTime).toBe(0);
    session.stop();
  });
});
