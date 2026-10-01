import { act, renderHook } from '@testing-library/react';
import { useState } from 'react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';

import { bootstrapFixture, snapshotFixture as baseSnapshot, voiceOutputRuntimeFixture } from '../test/fixtures';
import type { AudioDownloadAcceptance, AudioDownloadStatus, SessionSnapshot } from '../api/client';
import { TextToSpeechError, TextToSpeechSession } from '../textToSpeech';
import { beginSpeechPlayback } from '../speechPlayback';
import { VoiceInputSession } from '../voiceInput';
import { ChaWebError, type ChaWebClient } from './client';
import { useReadAloud } from './useReadAloud';
import { useVoiceInput } from './useVoiceInput';
import type { TranscriptEntry } from './transcript';

vi.mock('../textToSpeech', async (importOriginal) => {
  const actual = await importOriginal<typeof import('../textToSpeech')>();
  return { ...actual, TextToSpeechSession: vi.fn() };
});

const play = vi.fn<() => Promise<void>>();
const stop = vi.fn();
let ended: () => void;
const snapshotFixture: SessionSnapshot = { ...baseSnapshot, forum: bootstrapFixture.forums[1]!, session_id: 'planning' };
const entry: TranscriptEntry = {
  id: 7, kind: 'character', participant_id: 'guide', display_name: 'Guide',
  addressed_to: '', addressed_to_name: '', text: 'Hello', status: 'complete', created_at: 1,
};

function client(overrides: Partial<ChaWebClient> = {}): ChaWebClient {
  const startAudio = overrides.startAudio
    ?? vi.fn(async (_forum: string, _session: string, id: number) => ({ entry_id: id, cached: false, state: 'queued' as const }));
  return {
    getBootstrap: vi.fn(), listSessions: vi.fn(), createSession: vi.fn(), getSession: vi.fn(),
    submitInput: vi.fn(), stopSession: vi.fn(), deleteSession: vi.fn(),
    getVoiceInputRuntime: vi.fn(async () => null),
    connectVoiceInput: vi.fn(),
    startXaiVoiceInput: vi.fn(),
    sendXaiVoiceAudio: vi.fn(),
    stopXaiVoiceInput: vi.fn(),
    cancelXaiVoiceInput: vi.fn(),
    getVoiceOutputRuntime: vi.fn(async () => voiceOutputRuntimeFixture),
    startAudio,
    startAudioBatch: vi.fn(async (forum: string, session: string, ids: number[], vault: string) => ({
      entries: await Promise.all(ids.map((id) => startAudio(forum, session, id, vault))),
    })),
    getAudioStatus: vi.fn(async () => ({ cached_entry_ids: [], downloads: [] })),
    clearAudio: vi.fn(async () => undefined),
    ...overrides,
  };
}

beforeEach(() => {
  vi.useFakeTimers();
  vi.stubGlobal('Audio', class {
    src = '';
    play = vi.fn(async () => undefined);
    pause = vi.fn();
  });
  play.mockReset().mockResolvedValue(undefined);
  stop.mockReset();
  vi.mocked(TextToSpeechSession).mockReset().mockImplementation(function (
    _configuration, _voice, _text, onEnded, playback,
  ) {
    ended = () => { playback?.onPositionChange(0); onEnded(); };
    return { play, stop } as unknown as TextToSpeechSession;
  });
});
afterEach(() => { vi.useRealTimers(); vi.restoreAllMocks(); vi.unstubAllGlobals(); });

it.each(['ended', 'failed', 'disabled'])('holds xAI capture through automatic audio admission and loading until %s', async (outcome) => {
  vi.stubGlobal('isSecureContext', true);
  vi.stubGlobal('AudioContext', class {
    resume = vi.fn(async () => undefined);
    close = vi.fn(async () => undefined);
  });
  vi.spyOn(VoiceInputSession, 'supported').mockReturnValue(true);
  const captures: Array<{ text: Parameters<typeof VoiceInputSession.start>[1]; cancel: ReturnType<typeof vi.fn> }> = [];
  vi.spyOn(VoiceInputSession, 'start').mockImplementation(async (_config, text) => {
    const capture = { text, cancel: vi.fn(), stop: vi.fn(async () => undefined) };
    captures.push(capture);
    return capture;
  });
  let accept!: (value: AudioDownloadAcceptance) => void;
  let loaded!: () => void;
  let failed!: (failure: unknown) => void;
  const audio: AudioDownloadStatus = { cached_entry_ids: [], downloads: [] };
  play.mockImplementationOnce(() => new Promise<void>((resolve, reject) => { loaded = resolve; failed = reject; }));
  const api = client({
    getVoiceInputRuntime: vi.fn(async () => ({ provider: 'xai' as const, url: 'wss://example.test/stt',
      model: 'test', delay: 'low' as const, prompt: '', send_phrase: 'over to you' })),
    startAudio: vi.fn(() => new Promise<AudioDownloadAcceptance>((resolve) => { accept = resolve; })),
    getAudioStatus: vi.fn(async () => ({ ...audio, downloads: [...audio.downloads] })),
  });
  const saved: SessionSnapshot = { ...snapshotFixture, transcript: [],
    generation: { ...snapshotFixture.generation, active: false } };
  const { result, rerender } = renderHook(({ snapshot }) => {
    const speech = useReadAloud(api, snapshot, 'Personal', false);
    const [draft, setDraft] = useState('');
    const voice = useVoiceInput(api, 'session:lobby/planning', draft, setDraft,
      vi.fn(), snapshot.generation.active, speech.busy);
    return { speech, voice, draft };
  }, { initialProps: { snapshot: saved } });
  await act(async () => {});
  await act(async () => result.current.speech.toggleAutomatic());
  await act(async () => result.current.voice.toggle());
  expect(captures).toHaveLength(1);
  await act(async () => rerender({ snapshot: { ...saved, transcript: [{ ...entry, status: 'streaming' }],
    generation: { ...saved.generation, active: true } } }));
  expect(captures[0].cancel).toHaveBeenCalledOnce();
  await act(async () => rerender({ snapshot: { ...saved, transcript: [entry] } }));
  expect(api.startAudio).toHaveBeenCalledOnce();
  expect(result.current.speech.busy).toBe(true);
  expect(captures).toHaveLength(1);
  act(() => captures[0].text('late words', true));
  expect(result.current.draft).toBe('');
  await act(async () => {
    audio.downloads.push({ entry_id: 7, state: 'running' });
    accept({ entry_id: 7, cached: false, state: 'running' });
  });
  expect(play).toHaveBeenCalledOnce();
  expect(result.current.speech).toMatchObject({ busy: true, state: 'loading' });
  expect(captures).toHaveLength(1);
  if (outcome === 'failed') {
    await act(async () => failed(new TextToSpeechError('Audio could not be played.')));
    expect(result.current.speech.automatic).toBe(false);
  } else if (outcome === 'disabled') {
    await act(async () => result.current.speech.toggleAutomatic());
    await act(async () => loaded());
    expect(result.current.speech.entryId).toBeNull();
  } else {
    let endPlayback!: () => void;
    try {
      await act(async () => { endPlayback = beginSpeechPlayback(); loaded(); });
      expect(result.current.speech.state).toBe('playing');
      expect(captures).toHaveLength(1);
      act(() => { ended(); endPlayback(); });
      expect(captures).toHaveLength(1);
      await act(async () => { await vi.advanceTimersByTimeAsync(399); });
      expect(captures).toHaveLength(1);
      await act(async () => { await vi.advanceTimersByTimeAsync(1); });
    } finally {
      act(() => endPlayback?.());
      await act(async () => { await vi.advanceTimersByTimeAsync(400); });
    }
  }
  expect(result.current.speech.busy).toBe(false);
  expect(captures).toHaveLength(2);
});

it('starts streaming immediately after acceptance without waiting for the cached clip', async () => {
  let started!: () => void;
  play.mockImplementationOnce(() => new Promise<void>((resolve) => { started = resolve; }));
  const api = client();
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle(entry));
  expect(api.startAudio).toHaveBeenCalledExactlyOnceWith('lobby', 'planning', 7, 'Personal');
  expect(result.current).toMatchObject({ entryId: 7, state: 'loading' });
  expect(result.current.isDownloading(entry)).toBe(true);
  expect(play).toHaveBeenCalledOnce();
  expect(api.getAudioStatus).toHaveBeenCalledWith('lobby', 'planning');
  expect(TextToSpeechSession).toHaveBeenCalledWith(
    null, undefined, 'Hello', expect.any(Function),
    { position: 0, onPositionChange: expect.any(Function) },
    '/api/cha/v1/forums/lobby/sessions/planning/entries/7/audio',
    expect.any(Function), undefined, undefined, { streaming: true, onError: expect.any(Function) },
  );
  await act(async () => started());
  expect(result.current).toMatchObject({ entryId: 7, state: 'playing' });
  expect(result.current.isDownloading(entry)).toBe(true);
  act(() => ended());
  expect(result.current.entryId).toBeNull();
  expect(stop).toHaveBeenCalled();
});

it('plays cached audio without voice configuration or a generation request', async () => {
  const api = client({ getVoiceOutputRuntime: vi.fn(async () => null) });
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle({ ...entry, has_cached_audio: true }));
  expect(result.current.available).toBe(false);
  expect(result.current.state).toBe('playing');
  expect(api.startAudio).not.toHaveBeenCalled();
  expect(api.getAudioStatus).toHaveBeenCalledOnce();
});

it('plays cached acceptance without waiting for a status refresh', async () => {
  const api = client({ startAudio: vi.fn(async () => ({ entry_id: 7, cached: true })) });
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle(entry));
  expect(play).toHaveBeenCalledOnce();
  expect(api.getAudioStatus).toHaveBeenCalledWith('lobby', 'planning');
  expect(vi.mocked(TextToSpeechSession).mock.calls[0][9]?.streaming).toBe(false);
  expect(result.current.isCached(entry)).toBe(true);
});

it('marks completed downloads as cached and resets the indicator when recordings are cleared', async () => {
  const api = client();
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  expect(result.current.isCached(entry)).toBe(false);
  expect(result.current.isCached({ ...entry, has_cached_audio: true })).toBe(true);
  await act(async () => result.current.toggle(entry));
  expect(result.current.isCached(entry)).toBe(false);
  const completed = vi.mocked(TextToSpeechSession).mock.calls[0][6]!;
  act(() => completed());
  expect(result.current.isCached(entry)).toBe(true);
  act(() => result.current.toggle(entry));
  await act(async () => result.current.toggle(entry));
  expect(api.startAudio).toHaveBeenCalledOnce();
  await act(async () => result.current.clear());
  expect(result.current.isCached(entry)).toBe(false);
  expect(result.current.isCached({ ...entry, has_cached_audio: true })).toBe(false);
  act(() => completed());
  expect(result.current.isCached(entry)).toBe(false);
});

it('pauses a reply, resumes at its saved position, and restarts after it ends', async () => {
  const api = client();
  const cached = { ...entry, has_cached_audio: true };
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle(cached));
  const first = vi.mocked(TextToSpeechSession).mock.calls[0][4]!;
  stop.mockImplementationOnce(() => first.onPositionChange(12.5));
  act(() => result.current.toggle(cached));
  expect(result.current.entryId).toBeNull();
  expect(play).toHaveBeenCalledOnce();
  await act(async () => result.current.toggle(cached));
  expect(vi.mocked(TextToSpeechSession).mock.calls[1][4]?.position).toBe(12.5);
  expect(result.current.state).toBe('playing');
  act(() => ended());
  await act(async () => result.current.toggle(cached));
  expect(vi.mocked(TextToSpeechSession).mock.calls[2][4]?.position).toBe(0);
  expect(api.startAudio).not.toHaveBeenCalled();
});

it('keeps separate positions when switching between replies', async () => {
  const api = client();
  const firstEntry = { ...entry, has_cached_audio: true };
  const secondEntry = { ...firstEntry, id: 8 };
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle(firstEntry));
  const first = vi.mocked(TextToSpeechSession).mock.calls[0][4]!;
  stop.mockImplementationOnce(() => first.onPositionChange(12.5));
  await act(async () => result.current.toggle(secondEntry));
  const second = vi.mocked(TextToSpeechSession).mock.calls[1][4]!;
  expect(second.position).toBe(0);
  stop.mockImplementationOnce(() => second.onPositionChange(6.25));
  await act(async () => result.current.toggle(firstEntry));
  expect(vi.mocked(TextToSpeechSession).mock.calls[2][4]?.position).toBe(12.5);
  await act(async () => result.current.toggle(secondEntry));
  expect(vi.mocked(TextToSpeechSession).mock.calls[3][4]?.position).toBe(6.25);
});

it.each(['forum', 'session', 'vault'])('clears positions when the %s changes', async (change) => {
  const api = client();
  const cached = { ...entry, has_cached_audio: true };
  const { result, rerender } = renderHook(
    ({ snapshot, vault }) => useReadAloud(api, snapshot, vault, false),
    { initialProps: { snapshot: snapshotFixture, vault: 'Personal' } },
  );
  await act(async () => result.current.toggle(cached));
  const first = vi.mocked(TextToSpeechSession).mock.calls[0][4]!;
  stop.mockImplementationOnce(() => first.onPositionChange(12.5));
  rerender({
    snapshot: change === 'forum' ? { ...snapshotFixture, forum: bootstrapFixture.forums[0]! }
      : change === 'session' ? { ...snapshotFixture, session_id: 'other' } : snapshotFixture,
    vault: change === 'vault' ? 'Projects' : 'Personal',
  });
  expect(stop).toHaveBeenCalledOnce();
  await act(async () => result.current.toggle(cached));
  expect(vi.mocked(TextToSpeechSession).mock.calls[1][4]?.position).toBe(0);
});

it('preserves the position when a resumed stream is cancelled before playback starts', async () => {
  const api = client();
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle(entry));
  const first = vi.mocked(TextToSpeechSession).mock.calls[0][4]!;
  stop.mockImplementationOnce(() => first.onPositionChange(12.5));
  act(() => result.current.toggle(entry));
  let resolve!: () => void;
  play.mockImplementationOnce(() => new Promise<void>((done) => { resolve = done; }));
  await act(async () => result.current.toggle(entry));
  expect(result.current.state).toBe('loading');
  act(() => result.current.toggle(entry));
  await act(async () => resolve());
  expect(result.current.entryId).toBeNull();
  await act(async () => result.current.toggle(entry));
  expect(vi.mocked(TextToSpeechSession).mock.calls[2][4]?.position).toBe(12.5);
});

it.each(['stop', 'switch', 'delete', 'unmount'])('ignores a late generation result after %s', async (action) => {
  let resolve!: (value: { entry_id: number; cached: boolean }) => void;
  const api = client({ startAudio: vi.fn(() => new Promise<{ entry_id: number; cached: boolean }>((done) => { resolve = done; })) });
  const { result, rerender, unmount } = renderHook(
    ({ snapshot, deleting }: { snapshot: SessionSnapshot; deleting: boolean }) =>
      useReadAloud(api, snapshot, 'Personal', deleting),
    { initialProps: { snapshot: snapshotFixture, deleting: false } },
  );
  await act(async () => result.current.toggle(entry));
  if (action === 'stop') act(() => result.current.toggle(entry));
  else if (action === 'switch') rerender({ snapshot: { ...snapshotFixture, session_id: 'other' }, deleting: false });
  else if (action === 'delete') rerender({ snapshot: snapshotFixture, deleting: true });
  else unmount();
  await act(async () => resolve({ entry_id: 7, cached: true }));
  expect(play).not.toHaveBeenCalled();
  expect(api.getAudioStatus).toHaveBeenCalledTimes(action === 'switch' ? 2 : 1);
});

it('stops active playback on navigation', async () => {
  const api = client();
  const { result, rerender } = renderHook(
    ({ snapshot }: { snapshot: SessionSnapshot | null }) => useReadAloud(api, snapshot, 'Personal', false),
    { initialProps: { snapshot: snapshotFixture as SessionSnapshot | null } },
  );
  await act(async () => result.current.toggle(entry));
  rerender({ snapshot: null });
  expect(stop).toHaveBeenCalled();
  expect(result.current.entryId).toBeNull();
});

it('reports a failed stream after playback starts and allows a new attempt', async () => {
  const api = client();
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle(entry));
  expect(result.current.state).toBe('playing');
  const options = vi.mocked(TextToSpeechSession).mock.calls[0][9]!;
  const playback = vi.mocked(TextToSpeechSession).mock.calls[0][4]!;
  stop.mockImplementationOnce(() => playback.onPositionChange(12.5));
  act(() => options.onError!(new TextToSpeechError('Audio generation failed. Try again.')));
  expect(result.current.entryId).toBeNull();
  expect(result.current.error).toBe('Audio generation failed. Try again.');
  await act(async () => result.current.toggle(entry));
  expect(api.startAudio).toHaveBeenCalledTimes(2);
  expect(result.current.error).toBeNull();
  expect(play).toHaveBeenCalledTimes(2);
  expect(vi.mocked(TextToSpeechSession).mock.calls[1][4]?.position).toBe(0);
});

it('reports admission and autoplay failures without claiming playback started', async () => {
  const api = client({ startAudio: vi.fn().mockRejectedValueOnce(
    new ChaWebError(404, 'Voice output is not configured.'))
    .mockResolvedValue({ entry_id: 7, cached: true }) });
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle(entry));
  expect(result.current.error).toBe('Voice output is not configured.');
  play.mockRejectedValueOnce(new DOMException('Denied', 'NotAllowedError'));
  await act(async () => result.current.toggle(entry));
  expect(result.current.entryId).toBeNull();
  expect(result.current.error).toBe('Playback was blocked. Click the audio icon to try again.');
});

it('clears session recordings, stops playback, and regenerates stale cached entries from the beginning', async () => {
  let finish!: () => void;
  const api = client({ clearAudio: vi.fn(() => new Promise<void>((resolve) => { finish = resolve; })) });
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  const cached = { ...entry, has_cached_audio: true };
  await act(async () => result.current.toggle(cached));
  const playback = vi.mocked(TextToSpeechSession).mock.calls[0][4]!;
  stop.mockImplementationOnce(() => playback.onPositionChange(12.5));
  let clearing!: Promise<void>;
  await act(async () => { clearing = result.current.clear(); });
  expect(stop).toHaveBeenCalledOnce();
  expect(result.current).toMatchObject({ clearing: true, clearDisabled: true, entryId: null });
  expect(api.clearAudio).toHaveBeenCalledExactlyOnceWith('lobby', 'planning', 'Personal');
  await act(async () => { void result.current.clear(); result.current.toggle(cached); });
  expect(api.clearAudio).toHaveBeenCalledOnce();
  expect(play).toHaveBeenCalledOnce();
  await act(async () => { finish(); await clearing; });
  expect(result.current.clearDisabled).toBe(false);
  await act(async () => result.current.toggle(cached));
  expect(api.startAudio).toHaveBeenCalledExactlyOnceWith('lobby', 'planning', 7, 'Personal');
  expect(vi.mocked(TextToSpeechSession).mock.calls[1][4]?.position).toBe(0);
  expect(snapshotFixture).toEqual({ ...baseSnapshot, forum: bootstrapFixture.forums[1]!, session_id: 'planning' });
});

it('waits for pending audio admissions before clearing and never plays their late responses', async () => {
  let accept!: (value: { entry_id: number; cached: boolean }) => void;
  const api = client({ startAudio: vi.fn(() => new Promise<{ entry_id: number; cached: boolean }>((resolve) => { accept = resolve; })) });
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  act(() => result.current.toggle(entry));
  let clearing!: Promise<void>;
  act(() => { clearing = result.current.clear(); });
  expect(api.clearAudio).not.toHaveBeenCalled();
  await act(async () => { accept({ entry_id: 7, cached: false }); await clearing; });
  expect(api.clearAudio).toHaveBeenCalledOnce();
  expect(play).not.toHaveBeenCalled();
});

it('reports a clear failure and allows retry without requiring voice configuration', async () => {
  const api = client({
    getVoiceOutputRuntime: vi.fn(async () => null),
    clearAudio: vi.fn().mockRejectedValueOnce(new ChaWebError(503, 'Try again.')).mockResolvedValue(undefined),
  });
  const { result, rerender } = renderHook(
    ({ snapshot }: { snapshot: SessionSnapshot | null }) => useReadAloud(api, snapshot, 'Personal', false),
    { initialProps: { snapshot: snapshotFixture as SessionSnapshot | null } },
  );
  await act(async () => result.current.clear());
  expect(result.current.error).toBe('Try again.');
  expect(result.current.clearDisabled).toBe(false);
  await act(async () => result.current.clear());
  expect(api.clearAudio).toHaveBeenCalledTimes(2);
  expect(result.current.error).toBeNull();
  rerender({ snapshot: null });
  expect(result.current.clearDisabled).toBe(true);
  await act(async () => result.current.clear());
  expect(api.clearAudio).toHaveBeenCalledTimes(2);
});

it('submits existing replies as one batch without reading them and observes their completed downloads', async () => {
  const accepted = new Map<number, (value: AudioDownloadAcceptance) => void>();
  const audio: AudioDownloadStatus = { cached_entry_ids: [1], downloads: [] };
  const api = client({
    getAudioStatus: vi.fn(async () => ({ ...audio, downloads: [...audio.downloads] })),
    startAudio: vi.fn((_forum, _session, id) => new Promise<AudioDownloadAcceptance>((resolve) => {
      accepted.set(id, resolve);
    })),
  });
  const saved = { ...snapshotFixture, transcript: [
    { ...entry, id: 1, has_cached_audio: true }, entry, { ...entry, id: 8 }, { ...entry, id: 9 },
    { ...entry, id: 10, kind: 'human' as const }, { ...entry, id: 11, status: 'failed' as const },
    { ...entry, id: 12, text: ' ' },
  ] };
  const { result, rerender } = renderHook(({ snapshot }) => useReadAloud(api, snapshot, 'Personal', false),
    { initialProps: { snapshot: saved } });
  await act(async () => {});
  await act(async () => result.current.toggleAutomatic());
  expect(api.startAudioBatch).toHaveBeenCalledExactlyOnceWith('lobby', 'planning', [7, 8, 9], 'Personal');
  expect([...accepted.keys()]).toEqual([7, 8, 9]);
  expect(vi.mocked(api.startAudio).mock.calls.map((call) => call.slice(0, 2))).toEqual([
    ['lobby', 'planning'], ['lobby', 'planning'], ['lobby', 'planning'],
  ]);
  expect(play).not.toHaveBeenCalled();
  expect(result.current.automatic).toBe(true);
  expect(result.current.isDownloading(entry)).toBe(true);
  await act(async () => {
    for (const [id, resolve] of accepted) {
      audio.downloads.push({ entry_id: id, state: 'running' });
      resolve({ entry_id: id, cached: false, state: 'running' });
    }
  });
  expect(result.current.isDownloading(entry)).toBe(true);
  audio.cached_entry_ids = [1, 7, 8, 9];
  audio.downloads = [];
  await act(async () => vi.advanceTimersByTimeAsync(1000));
  expect(result.current.isCached(entry)).toBe(true);
  expect(result.current.isDownloading(entry)).toBe(false);
  rerender({ snapshot: { ...saved, transcript: [...saved.transcript] } });
  expect(api.startAudio).toHaveBeenCalledTimes(3);
  expect(play).not.toHaveBeenCalled();
});

it('streams new replies once in transcript order, including a reply in progress when enabled', async () => {
  const audio: AudioDownloadStatus = { cached_entry_ids: [7], downloads: [] };
  const api = client({
    getAudioStatus: vi.fn(async () => ({ ...audio, downloads: [...audio.downloads] })),
    startAudio: vi.fn(async (_forum, _session, id) => {
      if (id === 9) { audio.cached_entry_ids.push(id); return { entry_id: id, cached: true }; }
      audio.downloads.push({ entry_id: id, state: 'running' });
      return { entry_id: id, cached: false, state: 'running' as const };
    }),
  });
  const saved: SessionSnapshot = { ...snapshotFixture,
    transcript: [entry, { ...entry, id: 8, status: 'streaming' }] };
  const { result, rerender } = renderHook(({ snapshot }) => useReadAloud(api, snapshot, 'Personal', false),
    { initialProps: { snapshot: saved } });
  await act(async () => {});
  await act(async () => result.current.toggleAutomatic());
  await act(async () => rerender({ snapshot: { ...saved,
    transcript: [...saved.transcript, { ...entry, id: 9 }] } }));
  expect(api.startAudio).toHaveBeenCalledExactlyOnceWith('lobby', 'planning', 9, 'Personal');
  expect(play).not.toHaveBeenCalled();
  const complete: SessionSnapshot = { ...saved, transcript: [entry, { ...entry, id: 8 },
    { ...entry, id: 9 }, { ...entry, id: 10, kind: 'human' }] };
  await act(async () => rerender({ snapshot: complete }));
  expect(result.current).toMatchObject({ entryId: 8, state: 'playing' });
  expect(vi.mocked(TextToSpeechSession).mock.calls[0][9]).toMatchObject({ streaming: true });
  const audioElement = vi.mocked(TextToSpeechSession).mock.calls[0][9]?.audio;
  expect(audioElement).toBeDefined();
  await act(async () => ended());
  expect(result.current.entryId).toBe(9);
  expect(vi.mocked(TextToSpeechSession).mock.calls[1][9]).toMatchObject({ streaming: false, audio: audioElement });
  await act(async () => ended());
  rerender({ snapshot: { ...complete, transcript: [...complete.transcript] } });
  expect(play).toHaveBeenCalledTimes(2);
  expect(vi.mocked(api.startAudio).mock.calls.map((call) => call[2])).toEqual([9, 8]);
});

it('turns automatic playback off without cancelling accepted downloads or replaying old replies', async () => {
  const audio: AudioDownloadStatus = { cached_entry_ids: [], downloads: [] };
  const api = client({
    getAudioStatus: vi.fn(async () => ({ ...audio, downloads: [...audio.downloads] })),
    startAudio: vi.fn(async (_forum, _session, id) => {
      audio.downloads.push({ entry_id: id, state: 'running' });
      return { entry_id: id, cached: false, state: 'running' as const };
    }),
  });
  const saved = { ...snapshotFixture, transcript: [] as TranscriptEntry[] };
  const { result, rerender } = renderHook(({ snapshot }) => useReadAloud(api, snapshot, 'Personal', false),
    { initialProps: { snapshot: saved } });
  await act(async () => {});
  await act(async () => result.current.toggleAutomatic());
  await act(async () => rerender({ snapshot: { ...saved, transcript: [entry, { ...entry, id: 8 }] } }));
  expect(play).toHaveBeenCalledOnce();
  await act(async () => result.current.toggleAutomatic());
  expect(result.current).toMatchObject({ automatic: false, entryId: null });
  expect(stop).toHaveBeenCalledOnce();
  expect(api.clearAudio).not.toHaveBeenCalled();
  audio.cached_entry_ids = [7, 8]; audio.downloads = [];
  await act(async () => vi.advanceTimersByTimeAsync(1000));
  expect(result.current.isCached(entry)).toBe(true);
  await act(async () => result.current.toggleAutomatic());
  expect(play).toHaveBeenCalledOnce();
  expect(api.startAudio).toHaveBeenCalledTimes(2);
});

it('clearing recordings disables automatic mode and waits for pending batch admission', async () => {
  const accepted: Array<() => void> = [];
  const api = client({ startAudio: vi.fn((_forum, _session, id) =>
    new Promise<AudioDownloadAcceptance>((resolve) => accepted.push(() =>
      resolve({ entry_id: id, cached: false, state: 'queued' })))) });
  const saved = { ...snapshotFixture, transcript: [entry, { ...entry, id: 8 }, { ...entry, id: 9 }] };
  const { result } = renderHook(() => useReadAloud(api, saved, 'Personal', false));
  await act(async () => {});
  await act(async () => result.current.toggleAutomatic());
  expect(accepted).toHaveLength(3);
  let clear!: Promise<void>;
  act(() => { clear = result.current.clear(); });
  expect(result.current).toMatchObject({ automatic: false, clearing: true });
  await act(async () => { accepted[0]!(); accepted[1]!(); });
  expect(api.clearAudio).not.toHaveBeenCalled();
  await act(async () => { accepted[2]!(); await clear; });
  expect(api.clearAudio).toHaveBeenCalledOnce();
  expect(play).not.toHaveBeenCalled();
  expect(result.current.automatic).toBe(false);
  expect(api.startAudio).toHaveBeenCalledTimes(3);
});

it.each(['disable', 'navigation'])('ignores a late automatic admission failure after %s', async (action) => {
  let reject!: (failure: unknown) => void;
  const api = client({ startAudio: vi.fn(() => new Promise<AudioDownloadAcceptance>((_resolve, failed) => { reject = failed; })) });
  const saved = { ...snapshotFixture, transcript: [entry] };
  const { result, rerender } = renderHook(({ snapshot }) => useReadAloud(api, snapshot, 'Personal', false),
    { initialProps: { snapshot: saved } });
  await act(async () => {});
  await act(async () => result.current.toggleAutomatic());
  if (action === 'disable') act(() => result.current.toggleAutomatic());
  else rerender({ snapshot: { ...saved, session_id: 'other' } });
  await act(async () => reject(new ChaWebError(503, 'Old admission failed.')));
  expect(result.current).toMatchObject({ automatic: false, error: null });
  expect(play).not.toHaveBeenCalled();
});

it('reports automatic admission errors and allows an explicit retry', async () => {
  const api = client({ startAudio: vi.fn().mockRejectedValueOnce(new ChaWebError(503, 'Try again.'))
    .mockResolvedValue({ entry_id: 7, cached: true }) });
  const { result } = renderHook(() => useReadAloud(api, { ...snapshotFixture, transcript: [entry] }, 'Personal', false));
  await act(async () => {});
  await act(async () => result.current.toggleAutomatic());
  expect(result.current).toMatchObject({ automatic: false, error: 'Try again.' });
  await act(async () => result.current.toggleAutomatic());
  expect(result.current).toMatchObject({ automatic: true, error: null });
  expect(api.startAudio).toHaveBeenCalledTimes(2);
  expect(play).not.toHaveBeenCalled();
});

it('disables automatic playback when the browser blocks it and retains manual playback', async () => {
  const cached: number[] = [];
  const api = client({
    getAudioStatus: vi.fn(async () => ({ cached_entry_ids: [...cached], downloads: [] })),
    startAudio: vi.fn(async (_forum, _session, id) => { cached.push(id); return { entry_id: id, cached: true }; }),
  });
  const saved = { ...snapshotFixture, transcript: [] as TranscriptEntry[] };
  const { result, rerender } = renderHook(({ snapshot }) => useReadAloud(api, snapshot, 'Personal', false),
    { initialProps: { snapshot: saved } });
  await act(async () => {});
  await act(async () => result.current.toggleAutomatic());
  play.mockRejectedValueOnce(new DOMException('Blocked', 'NotAllowedError'));
  await act(async () => rerender({ snapshot: { ...saved, transcript: [entry] } }));
  expect(result.current).toMatchObject({ automatic: false, entryId: null,
    error: 'Playback was blocked. Click the audio icon to try again.' });
  await act(async () => result.current.toggle(entry));
  expect(result.current).toMatchObject({ state: 'playing', entryId: 7, error: null });
});
