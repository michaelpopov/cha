import { act, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';

import { bootstrapFixture, snapshotFixture as baseSnapshot, voiceOutputRuntimeFixture } from '../test/fixtures';
import type { SessionSnapshot } from '../api/client';
import { TextToSpeechError, TextToSpeechSession } from '../textToSpeech';
import { ChaWebError, type ChaWebClient } from './client';
import { useReadAloud } from './useReadAloud';
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
  return {
    getBootstrap: vi.fn(), listSessions: vi.fn(), createSession: vi.fn(), getSession: vi.fn(),
    submitInput: vi.fn(), stopSession: vi.fn(), deleteSession: vi.fn(),
    getVoiceOutputRuntime: vi.fn(async () => voiceOutputRuntimeFixture),
    startAudio: vi.fn(async () => ({ entry_id: 7, cached: false, state: 'queued' as const })),
    getAudioStatus: vi.fn(async () => ({ cached_entry_ids: [7], downloads: [] })),
    ...overrides,
  };
}

beforeEach(() => {
  vi.useFakeTimers();
  play.mockReset().mockResolvedValue(undefined);
  stop.mockReset();
  vi.mocked(TextToSpeechSession).mockReset().mockImplementation(function (
    _configuration, _voice, _text, onEnded, playback,
  ) {
    ended = () => { playback?.onPositionChange(0); onEnded(); };
    return { play, stop } as unknown as TextToSpeechSession;
  });
});
afterEach(() => vi.useRealTimers());

it('starts streaming immediately after acceptance without waiting for the cached clip', async () => {
  let started!: () => void;
  play.mockImplementationOnce(() => new Promise<void>((resolve) => { started = resolve; }));
  const api = client();
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle(entry));
  expect(api.startAudio).toHaveBeenCalledExactlyOnceWith('lobby', 'planning', 7, 'Personal');
  expect(result.current).toMatchObject({ entryId: 7, state: 'loading' });
  expect(play).toHaveBeenCalledOnce();
  expect(api.getAudioStatus).not.toHaveBeenCalled();
  expect(TextToSpeechSession).toHaveBeenCalledWith(
    null, undefined, 'Hello', expect.any(Function),
    { position: 0, onPositionChange: expect.any(Function) },
    '/api/cha/v1/forums/lobby/sessions/planning/entries/7/audio',
    undefined, undefined, undefined, { streaming: true, onError: expect.any(Function) },
  );
  await act(async () => started());
  expect(result.current).toMatchObject({ entryId: 7, state: 'playing' });
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
  expect(api.getAudioStatus).not.toHaveBeenCalled();
});

it('uses cached acceptance immediately without polling', async () => {
  const api = client({ startAudio: vi.fn(async () => ({ entry_id: 7, cached: true })) });
  const { result } = renderHook(() => useReadAloud(api, snapshotFixture, 'Personal', false));
  await act(async () => result.current.toggle(entry));
  expect(play).toHaveBeenCalledOnce();
  expect(api.getAudioStatus).not.toHaveBeenCalled();
  expect(vi.mocked(TextToSpeechSession).mock.calls[0][9]?.streaming).toBe(false);
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
  expect(api.getAudioStatus).not.toHaveBeenCalled();
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
  expect(result.current.error).toBe('Playback was blocked. Click Read aloud to try again.');
});
