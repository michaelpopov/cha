import { act, renderHook, waitFor } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';

import { ChaError, type ChaClient } from './api/client';
import { fixtureClient } from './test/fixtures';
import { useLoad } from './useLoad';

function deferred<Value>() {
  let resolve!: (value: Value) => void;
  let reject!: (failure: unknown) => void;
  const promise = new Promise<Value>((accept, fail) => {
    resolve = accept;
    reject = fail;
  });
  return { promise, resolve, reject };
}

describe('useLoad', () => {
  it('keeps loaded data on ordinary renders and clears it when retrying', async () => {
    const client = fixtureClient();
    const refreshed = deferred<string[]>();
    const load = vi.fn((_client: ChaClient) => Promise.resolve(['First']))
      .mockImplementationOnce(() => Promise.resolve(['First']))
      .mockImplementationOnce(() => refreshed.promise);
    const { result, rerender } = renderHook(() => useLoad(client, load, 'Load failed.'));

    expect(result.current.data).toBeNull();
    await waitFor(() => expect(result.current.data).toEqual(['First']));
    rerender();
    expect(load).toHaveBeenCalledTimes(1);
    expect(load).toHaveBeenCalledWith(client);
    expect(result.current.data).toEqual(['First']);

    act(() => result.current.retry());
    expect(result.current.data).toBeNull();
    expect(result.current.error).toBeNull();
    expect(load).toHaveBeenCalledTimes(2);
    await act(async () => { refreshed.resolve(['Refreshed']); });
    expect(result.current.data).toEqual(['Refreshed']);
  });

  it.each([
    { failure: new Error('Internal details'), expected: 'Load failed.' },
    { failure: new ChaError('application_unavailable', 'Try again shortly.'), expected: 'Try again shortly.' },
  ])('reports "$expected" and clears the error on retry', async ({ failure, expected }) => {
    const client = fixtureClient();
    const retried = deferred<string[]>();
    const load = vi.fn((_client: ChaClient) => retried.promise)
      .mockRejectedValueOnce(failure);
    const { result } = renderHook(() => useLoad(client, load, 'Load failed.'));

    await waitFor(() => expect(result.current.error).toBe(expected));
    expect(result.current.data).toBeNull();
    act(() => result.current.retry());
    expect(result.current.error).toBeNull();
    await act(async () => { retried.resolve([]); });
    expect(result.current.data).toEqual([]);
    expect(result.current.error).toBeNull();
  });

  it.each([
    { change: 'client', late: 'success' },
    { change: 'client', late: 'failure' },
    { change: 'retry', late: 'success' },
    { change: 'retry', late: 'failure' },
  ])('ignores a late $late after a $change change', async ({ change, late }) => {
    const original = deferred<string[]>();
    const replacement = deferred<string[]>();
    const firstClient = fixtureClient();
    const secondClient = fixtureClient();
    const load = vi.fn((_client: ChaClient) => replacement.promise)
      .mockImplementationOnce(() => original.promise);
    const { result, rerender } = renderHook(
      ({ client }) => useLoad(client, load, 'Load failed.'),
      { initialProps: { client: firstClient } },
    );

    if (change === 'client') rerender({ client: secondClient });
    else act(() => result.current.retry());
    expect(load).toHaveBeenCalledTimes(2);
    expect(load).toHaveBeenLastCalledWith(change === 'client' ? secondClient : firstClient);
    await act(async () => { replacement.resolve(['Current']); });
    await act(async () => {
      if (late === 'success') original.resolve(['Obsolete']);
      else original.reject(new Error('Obsolete failure'));
    });
    expect(result.current.data).toEqual(['Current']);
    expect(result.current.error).toBeNull();
  });
});
