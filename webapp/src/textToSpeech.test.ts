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
      pause: vi.fn(), removeAttribute: vi.fn(), load: vi.fn(),
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

describe('streaming playback', () => {
  const appended: string[] = [];
  let source: TestMediaSource;
  class TestBuffer extends EventTarget {
    buffered = { length: 0 };
    appendBuffer(bytes: ArrayBuffer) {
      appended.push(new TextDecoder().decode(bytes));
      queueMicrotask(() => this.dispatchEvent(new Event('updateend')));
    }
  }
  class TestMediaSource extends EventTarget {
    static isTypeSupported = () => true;
    endOfStream = vi.fn();
    constructor() {
      super();
      source = this;
      queueMicrotask(() => this.dispatchEvent(new Event('sourceopen')));
    }
    addSourceBuffer = vi.fn(() => new TestBuffer());
  }
  const response = (body: string, complete = false, type = 'audio/mpeg') => new Response(body, {
    headers: { 'Content-Type': type, 'X-CHA-Audio-Complete': complete ? '1' : '0' },
  });
  const start = (onCached = vi.fn(), onError = vi.fn()) => {
    const release = vi.fn(async () => undefined);
    const session = new TextToSpeechSession(null, undefined, 'Hello', vi.fn(),
      undefined, undefined, onCached,
      { preview: async () => ({ url: '/media/r1', resource_id: 'r1', streaming: true }), release },
      undefined, { onError });
    return { session, release };
  };
  beforeEach(() => {
    appended.length = 0;
    vi.stubGlobal('MediaSource', TestMediaSource);
  });

  it('starts before the final bytes and marks the cache only on successful completion', async () => {
    let finish!: (response: Response) => void;
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValueOnce(response('first'))
      .mockImplementationOnce(() => new Promise((resolve) => { finish = resolve; }));
    const cached = vi.fn();
    const { session, release } = start(cached);
    await session.play();
    await vi.waitFor(() => expect(fetchMock).toHaveBeenCalledTimes(2));
    expect(appended).toEqual(['first']);
    expect(audios[0].play).toHaveBeenCalledOnce();
    expect(cached).not.toHaveBeenCalled();
    expect(source.endOfStream).not.toHaveBeenCalled();
    expect(fetchMock.mock.calls[1][1]?.headers).toEqual({ 'X-CHA-Audio-Offset': '5' });
    finish(response('second', true));
    await vi.waitFor(() => expect(source.endOfStream).toHaveBeenCalledOnce());
    expect(appended).toEqual(['first', 'second']);
    expect(cached).toHaveBeenCalledOnce();
    session.stop();
    expect(release).toHaveBeenCalledOnce();
  });

  it('reports a failure after playback starts, releases the preview, and never marks it cached', async () => {
    let fail!: (response: Response) => void;
    vi.spyOn(globalThis, 'fetch').mockResolvedValueOnce(response('first'))
      .mockImplementationOnce(() => new Promise((resolve) => { fail = resolve; }));
    const cached = vi.fn();
    const error = vi.fn();
    const { session, release } = start(cached, error);
    await session.play();
    await vi.waitFor(() => expect(fail).toBeDefined());
    fail(new Response('', { status: 502 }));
    await vi.waitFor(() => expect(error).toHaveBeenCalledOnce());
    expect(cached).not.toHaveBeenCalled();
    expect(source.endOfStream).not.toHaveBeenCalled();
    expect(release).toHaveBeenCalledOnce();
    expect(audios[0].pause).toHaveBeenCalledOnce();
  });

  it('aborts waiting for the first bytes and stops polling on disposal', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValue(new Response(null, { status: 204 }));
    const { session, release } = start();
    const playing = session.play();
    const rejection = expect(playing).rejects.toMatchObject({ name: 'AbortError' });
    await vi.waitFor(() => expect(fetchMock).toHaveBeenCalled());
    session.stop();
    await rejection;
    const calls = fetchMock.mock.calls.length;
    await new Promise((resolve) => setTimeout(resolve, 150));
    expect(fetchMock).toHaveBeenCalledTimes(calls);
    expect(release).toHaveBeenCalledOnce();
    expect(audios).toHaveLength(0);
  });

  it.each(['audio/wav', 'audio/mpeg'])('collects %s when streaming decoding is unavailable', async (type) => {
    vi.stubGlobal('MediaSource', undefined);
    let finish!: (response: Response) => void;
    vi.spyOn(globalThis, 'fetch').mockResolvedValueOnce(response('first', false, type))
      .mockImplementationOnce(() => new Promise((resolve) => { finish = resolve; }));
    const { session } = start();
    const playing = session.play();
    await vi.waitFor(() => expect(finish).toBeDefined());
    expect(audios).toHaveLength(0);
    finish(response('last', true, type));
    await playing;
    expect(audios[0].play).toHaveBeenCalledOnce();
    expect((vi.mocked(URL.createObjectURL).mock.calls[0][0] as Blob).size).toBe(9);
    session.stop();
  });
});
