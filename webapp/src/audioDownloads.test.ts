import { act, render, renderHook } from '@testing-library/react';
import { createElement } from 'react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { useAudioDownloads } from './audioDownloads';
import { ChaError, type AudioDownloadAcceptance, type AudioDownloadBatchAcceptance, type AudioDownloadStatus } from './api/client';
import { fixtureClient } from './test/fixtures';

const idle: AudioDownloadStatus = { cached_entry_ids: [], downloads: [] };
const pending: AudioDownloadStatus = { cached_entry_ids: [], downloads: [{ entry_id: 1, state: 'running' }] };
const request = { vault_name: 'Personal', reference_id: 'reader' };
function deferred<T>() {
  let resolve!: (value: T) => void;
  let reject!: (reason: unknown) => void;
  const promise = new Promise<T>((done, failed) => { resolve = done; reject = failed; });
  return { promise, resolve, reject };
}
async function settle() { await act(async () => { await Promise.resolve(); }); }
afterEach(() => { vi.useRealTimers(); });

describe('background audio observer', () => {
  it.each(['command_timeout', 'command_queue_full', 'speech_busy', 'internal_error'] as const)(
    'retries a transient %s status failure and stops once an idle result is read', async (code) => {
      vi.useFakeTimers();
      const getAudioDownloads = vi.fn().mockRejectedValueOnce(new ChaError(code, 'Try again.')).mockResolvedValue(idle);
      const client = fixtureClient({ getAudioDownloads });
      const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
      await settle();
      expect(result.current.status).toBeNull();
      await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
      expect(result.current.status).toEqual(idle);
      await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
      expect(getAudioDownloads).toHaveBeenCalledTimes(2);
    });

  it.each(['vault_changed', 'not_found', 'session_not_live', 'invalid_argument', 'application_unavailable'] as const)(
    'stops polling after %s and allows an explicit retry', async (code) => {
      vi.useFakeTimers();
      const getAudioDownloads = vi.fn().mockRejectedValue(new ChaError(code, 'Audio status is unavailable.'));
      const client = fixtureClient({ getAudioDownloads });
      const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
      await settle();
      await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
      expect(getAudioDownloads).toHaveBeenCalledOnce();
      expect(result.current.unavailable).toBe('Audio status is unavailable.');
      getAudioDownloads.mockResolvedValue(idle);
      act(() => { result.current.refresh(); });
      await settle();
      expect(result.current.unavailable).toBeNull();
    });

  it('does not render transcript children for unchanged polls but renders changed job state', async () => {
    vi.useFakeTimers();
    const getAudioDownloads = vi.fn(async (): Promise<AudioDownloadStatus> => ({ cached_entry_ids: [2], downloads: [{ entry_id: 1, state: 'running' }] }));
    const client = fixtureClient({ getAudioDownloads });
    const rendered = vi.fn();
    function Transcript({ status }: { status: AudioDownloadStatus | null }) { rendered(status); return null; }
    function Screen() {
      const { status } = useAudioDownloads(client, 'forum', 'session', 'Personal', 0);
      return createElement(Transcript, { status });
    }
    render(createElement(Screen));
    await settle();
    const first = rendered.mock.lastCall![0];
    const renders = rendered.mock.calls.length;
    await act(async () => { await vi.advanceTimersByTimeAsync(3000); });
    expect(getAudioDownloads).toHaveBeenCalledTimes(4);
    expect(rendered).toHaveBeenCalledTimes(renders);
    expect(rendered.mock.lastCall![0]).toBe(first);
    getAudioDownloads.mockResolvedValue({ cached_entry_ids: [2], downloads: [{ entry_id: 1, state: 'failed', error: 'Download failed.' }] });
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(rendered).toHaveBeenCalledTimes(renders + 1);
  });

  it('publishes batch admission as a few renders instead of one per entry', async () => {
    const accepted = deferred<AudioDownloadBatchAcceptance>();
    const getAudioDownloads = vi.fn().mockResolvedValueOnce(idle).mockResolvedValue(pending);
    const client = fixtureClient({ getAudioDownloads, startAudioDownloadBatch: () => accepted.promise });
    const rendered = vi.fn();
    let submit!: ReturnType<typeof useAudioDownloads>['submitBatch'];
    function Transcript() { rendered(); return null; }
    function Screen() {
      const downloads = useAudioDownloads(client, 'forum', 'session', 'Personal', 0);
      submit = downloads.submitBatch;
      return createElement(Transcript);
    }
    render(createElement(Screen));
    await settle();
    const renders = rendered.mock.calls.length;
    const entries = Array.from({ length: 300 }, (_, index) => ({ entry_id: index + 1, reference_id: 'reader' }));
    let submission: Promise<void> | undefined;
    act(() => { submission = submit({ vault_name: 'Personal', entries }); });
    await act(async () => {
      accepted.resolve({ entries: entries.map(({ entry_id }) => ({ entry_id, cached: false, state: 'queued' })) });
      await submission;
    });
    expect(rendered.mock.calls.length - renders).toBeLessThanOrEqual(4);
  });

  it('reads once on opening and polls only while entries are pending, preserving them on failure', async () => {
    vi.useFakeTimers();
    const getAudioDownloads = vi.fn().mockResolvedValueOnce(pending)
      .mockRejectedValueOnce(new Error('offline')).mockResolvedValue(idle);
    const client = fixtureClient({ getAudioDownloads });
    const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
    await settle();
    expect(result.current.status).toEqual(pending);
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(result.current.status).toEqual(pending);
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(result.current.status).toEqual(idle);
    await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
    expect(getAudioDownloads).toHaveBeenCalledTimes(3);
  });

  it('invalidates a pre-submission poll and refreshes after accepting a job', async () => {
    const old = deferred<AudioDownloadStatus>();
    const getAudioDownloads = vi.fn().mockReturnValueOnce(old.promise).mockResolvedValue(pending);
    const client = fixtureClient({ getAudioDownloads,
      startAudioDownload: async () => ({ entry_id: 1, cached: false, state: 'queued' }),
    });
    const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
    await act(async () => { await result.current.submit(1, request); });
    expect(result.current.status?.downloads[0].state).toBe('queued');
    await act(async () => { old.resolve(idle); });
    expect(result.current.status).toEqual(pending);
    expect(getAudioDownloads).toHaveBeenCalledTimes(2);
  });

  it('refreshes after an ambiguous submission failure to discover the accepted job', async () => {
    const getAudioDownloads = vi.fn().mockResolvedValueOnce(idle).mockResolvedValue(pending);
    const client = fixtureClient({ getAudioDownloads,
      startAudioDownload: async () => { throw new Error('connection lost'); },
    });
    const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
    await settle();
    await act(async () => { await expect(result.current.submit(1, request)).rejects.toThrow('connection lost'); });
    expect(result.current.status).toEqual(pending);
  });

  it('a cached-audio miss refreshes status without swallowing another entry admission error', async () => {
    const accepted = deferred<AudioDownloadAcceptance>();
    const getAudioDownloads = vi.fn().mockResolvedValueOnce({ cached_entry_ids: [1], downloads: [] }).mockResolvedValue(idle);
    const client = fixtureClient({ getAudioDownloads, startAudioDownload: () => accepted.promise });
    const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
    await settle();
    let submission: Promise<void> | undefined;
    act(() => { submission = result.current.submit(2, request); });
    const failed = expect(submission).rejects.toThrow('Admission failed');
    act(() => { result.current.refresh(1); });
    expect(result.current.status?.cached_entry_ids).toEqual([]);
    expect(result.current.status?.downloads[0]).toMatchObject({ entry_id: 2, state: 'queued' });
    await act(async () => { accepted.reject(new Error('Admission failed')); await failed; });
    expect(getAudioDownloads).toHaveBeenCalledTimes(2);
  });

  it('leaves retry available and stops polling when submission and its status refresh both fail', async () => {
    vi.useFakeTimers();
    const getAudioDownloads = vi.fn().mockResolvedValueOnce(idle).mockRejectedValue(new ChaError('command_timeout', 'The request timed out.'));
    const client = fixtureClient({ getAudioDownloads,
      startAudioDownload: async () => { throw new ChaError('command_timeout', 'The request timed out.'); },
    });
    const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
    await settle();
    await act(async () => { await expect(result.current.submit(1, request)).rejects.toBeInstanceOf(ChaError); });
    expect(result.current.status).toEqual(idle);
    await act(async () => { await vi.advanceTimersByTimeAsync(10000); });
    expect(getAudioDownloads).toHaveBeenCalledTimes(2);
  });

  it('reports a rejected admission only to the caller and keeps generation available after refresh', async () => {
    const refresh = deferred<AudioDownloadStatus>();
    const getAudioDownloads = vi.fn().mockResolvedValueOnce(idle).mockReturnValue(refresh.promise);
    const client = fixtureClient({ getAudioDownloads,
      startAudioDownload: async () => { throw new ChaError('not_found', 'Voice output is not configured.'); },
    });
    const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
    await settle();
    await act(async () => { await expect(result.current.submit(1, request)).rejects.toThrow('Voice output is not configured.'); });
    expect(result.current.status).toEqual(idle);
    await act(async () => { refresh.resolve(idle); });
    expect(result.current.status).toEqual(idle);
  });

  it('stops polling and enables retry on a stale-vault status error', async () => {
    vi.useFakeTimers();
    const getAudioDownloads = vi.fn().mockResolvedValueOnce(pending)
      .mockRejectedValue(new ChaError('vault_changed', 'The active vault changed.'));
    const client = fixtureClient({ getAudioDownloads });
    const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
    await settle();
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(result.current.status?.downloads[0]).toEqual({ entry_id: 1, state: 'failed', error: 'The active vault changed.' });
    await act(async () => { await vi.advanceTimersByTimeAsync(10000); });
    expect(getAudioDownloads).toHaveBeenCalledTimes(2);
  });

  it('keeps polling known jobs while maintenance temporarily closes admission', async () => {
    vi.useFakeTimers();
    const getAudioDownloads = vi.fn().mockResolvedValueOnce(pending)
      .mockRejectedValueOnce(new ChaError('speech_busy', 'Maintenance')).mockResolvedValue(idle);
    const client = fixtureClient({ getAudioDownloads });
    const { result } = renderHook(() => useAudioDownloads(client, 'forum', 'session', 'Personal', 0));
    await settle();
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(result.current.status).toEqual(pending);
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    expect(result.current.status).toEqual(idle);
    expect(getAudioDownloads).toHaveBeenCalledTimes(3);
  });

  it('sidebar clear discards an outstanding poll even when no entry was cached', async () => {
    vi.useFakeTimers();
    const old = deferred<AudioDownloadStatus>();
    const getAudioDownloads = vi.fn().mockResolvedValueOnce(pending).mockReturnValueOnce(old.promise).mockResolvedValue(idle);
    const client = fixtureClient({ getAudioDownloads });
    const { result, rerender } = renderHook(({ clear }) =>
      useAudioDownloads(client, 'forum', 'session', 'Personal', clear), { initialProps: { clear: 0 } });
    await settle();
    await act(async () => { await vi.advanceTimersByTimeAsync(1000); });
    rerender({ clear: 1 });
    expect(result.current.status).toBeNull();
    await act(async () => { old.resolve(pending); });
    expect(result.current.status).toEqual(idle);
    await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
    expect(getAudioDownloads).toHaveBeenCalledTimes(3);
  });

  it('clear ignores late admission responses and waits for submissions before refreshing', async () => {
    const accepted = deferred<AudioDownloadAcceptance>();
    const getAudioDownloads = vi.fn().mockResolvedValue(idle);
    const client = fixtureClient({ getAudioDownloads, startAudioDownload: () => accepted.promise });
    const { result, rerender } = renderHook(({ clear }) =>
      useAudioDownloads(client, 'forum', 'session', 'Personal', clear), { initialProps: { clear: 0 } });
    await settle();
    let submission: Promise<void> | undefined;
    act(() => { submission = result.current.submit(1, request); });
    rerender({ clear: 1 });
    expect(result.current.status).toBeNull();
    expect(getAudioDownloads).toHaveBeenCalledTimes(1);
    await act(async () => { accepted.resolve({ entry_id: 1, cached: true }); await submission; });
    expect(result.current.status).toEqual(idle);
    expect(getAudioDownloads).toHaveBeenCalledTimes(2);
  });

  it('does not report a rejected admission after leaving its conversation', async () => {
    const accepted = deferred<AudioDownloadAcceptance>();
    const client = fixtureClient({ startAudioDownload: () => accepted.promise });
    const { result, rerender } = renderHook(({ session }) =>
      useAudioDownloads(client, 'forum', session, 'Personal', 0), { initialProps: { session: 'first' } });
    await settle();
    let submission: Promise<void> | undefined;
    act(() => { submission = result.current.submit(1, request); });
    rerender({ session: 'second' });
    await act(async () => {
      accepted.reject(new ChaError('speech_busy', 'Maintenance'));
      await expect(submission).resolves.toBeUndefined();
    });
    expect(result.current.status).toEqual(idle);
  });

  it('ignores old conversation polls and admissions after navigation', async () => {
    const old = deferred<AudioDownloadStatus>();
    const accepted = deferred<AudioDownloadAcceptance>();
    const getAudioDownloads = vi.fn().mockReturnValueOnce(old.promise).mockResolvedValue(idle);
    const client = fixtureClient({ getAudioDownloads, startAudioDownload: () => accepted.promise });
    const { result, rerender } = renderHook(({ session }) =>
      useAudioDownloads(client, 'forum', session, 'Personal', 0), { initialProps: { session: 'first' } });
    let submission: Promise<void> | undefined;
    act(() => { submission = result.current.submit(1, request); });
    rerender({ session: 'second' });
    await settle();
    await act(async () => { old.resolve(pending); accepted.resolve({ entry_id: 1, cached: true }); await submission; });
    expect(result.current.status).toEqual(idle);
    expect(getAudioDownloads).toHaveBeenCalledTimes(2);
    expect(getAudioDownloads).toHaveBeenLastCalledWith('forum', 'second', 'Personal');
  });
});
