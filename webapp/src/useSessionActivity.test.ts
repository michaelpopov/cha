import { useReducer } from 'react';
import { act, cleanup, renderHook } from '@testing-library/react';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';

import { ChaError, type ChaClient, type SessionSnapshot } from './api/client';
import { appReducer, initialAppState, type AppState } from './state/view';
import { bootstrapFixture, fixtureClient, snapshotFixture } from './test/fixtures';
import { useSessionActivity } from './useSessionActivity';

beforeEach(() => { vi.useFakeTimers(); });
afterEach(() => { cleanup(); vi.useRealTimers(); vi.restoreAllMocks(); });

const generating = { ...snapshotFixture, generation: { ...snapshotFixture.generation, active: true } };
const welcome = { forumId: 'entrance', sessionId: 'welcome' };
const planning = { ...snapshotFixture, forum: bootstrapFixture.forums[1], session_id: 'planning' };

function backgroundState() {
  let state = appReducer(initialAppState, { type: 'bootstrap-loaded', bootstrap: bootstrapFixture });
  state = appReducer(state, { type: 'conversation-opened', snapshot: generating });
  state = appReducer(state, { type: 'conversation-opened', snapshot: planning });
  return appReducer(state, { type: 'stream-state', status: 'connected' });
}

function harness(client: ChaClient, initial: AppState = backgroundState()) {
  return renderHook(() => {
    const [state, dispatch] = useReducer(appReducer, initial);
    useSessionActivity(client, state, dispatch);
    return { state, dispatch };
  });
}

it('checks a generating background session until it retires, without opening it', async () => {
  const getSessionSnapshot = vi.fn().mockResolvedValueOnce(generating)
    .mockRejectedValueOnce(new ChaError('session_not_live', 'Retired'));
  const client = fixtureClient({ getSessionSnapshot });
  const open = vi.spyOn(client, 'openSession');
  const hook = harness(client);
  await act(async () => {});
  expect(getSessionSnapshot).toHaveBeenCalledExactlyOnceWith('entrance', 'welcome');
  expect(hook.result.current.state.sessionActivity).toEqual([{ ...welcome, status: 'generating' }]);
  await act(async () => { await vi.advanceTimersByTimeAsync(1_000); });
  expect(hook.result.current.state.sessionActivity).toEqual([{ ...welcome, status: 'finished' }]);
  expect(open).not.toHaveBeenCalled();
  await act(async () => { await vi.advanceTimersByTimeAsync(5_000); });
  expect(getSessionSnapshot).toHaveBeenCalledTimes(2);
});

it('does not treat a transient failure as completion and retries the check', async () => {
  vi.spyOn(console, 'warn').mockImplementation(() => {});
  const getSessionSnapshot = vi.fn().mockRejectedValueOnce(new Error('Disconnected'))
    .mockResolvedValueOnce(snapshotFixture);
  const hook = harness(fixtureClient({ getSessionSnapshot }));
  await act(async () => {});
  expect(hook.result.current.state.sessionActivity[0].status).toBe('generating');
  await act(async () => { await vi.advanceTimersByTimeAsync(1_000); });
  expect(hook.result.current.state.sessionActivity[0].status).toBe('finished');
});

it('warns once while checks keep failing', async () => {
  const warn = vi.spyOn(console, 'warn').mockImplementation(() => {});
  const getSessionSnapshot = vi.fn().mockRejectedValue(new Error('Disconnected'));
  harness(fixtureClient({ getSessionSnapshot }));
  await act(async () => { await vi.advanceTimersByTimeAsync(3_000); });
  expect(getSessionSnapshot.mock.calls.length).toBeGreaterThan(1);
  expect(warn).toHaveBeenCalledOnce();
});

it('ignores a late background check after the session is reopened', async () => {
  let resolve!: (snapshot: SessionSnapshot) => void;
  const getSessionSnapshot = vi.fn(() => new Promise<SessionSnapshot>((done) => { resolve = done; }));
  const hook = harness(fixtureClient({ getSessionSnapshot }));
  act(() => {
    hook.result.current.dispatch({ type: 'conversation-opened', snapshot: generating });
    hook.result.current.dispatch({ type: 'stream-state', status: 'connected' });
  });
  await act(async () => { resolve(snapshotFixture); });
  expect(hook.result.current.state.sessionActivity).toEqual([{ ...welcome, status: 'generating' }]);
});

it('ignores a late check after vault replacement, including reused session IDs', async () => {
  let resolve!: (snapshot: SessionSnapshot) => void;
  const getSessionSnapshot = vi.fn(() => new Promise<SessionSnapshot>((done) => { resolve = done; }));
  const hook = harness(fixtureClient({ getSessionSnapshot }));
  act(() => {
    hook.result.current.dispatch({ type: 'vault-context-reset' });
    hook.result.current.dispatch({ type: 'conversation-opened', snapshot: generating });
    hook.result.current.dispatch({ type: 'stream-state', status: 'connected' });
  });
  await act(async () => { resolve(snapshotFixture); });
  expect(hook.result.current.state.sessionActivity).toEqual([{ ...welcome, status: 'generating' }]);
});

it('uses the live stream for the selected session, including while in Notes', async () => {
  let state = appReducer(backgroundState(), { type: 'conversation-opened', snapshot: generating });
  state = appReducer(state, { type: 'stream-state', status: 'connected' });
  state = appReducer(state, { type: 'open-note', noteId: 4 });
  const getSessionSnapshot = vi.fn();
  const hook = harness(fixtureClient({ getSessionSnapshot }), state);
  await act(async () => { await vi.advanceTimersByTimeAsync(3_000); });
  expect(getSessionSnapshot).not.toHaveBeenCalled();
  act(() => hook.result.current.dispatch({ type: 'session-generation', ...welcome, generation: snapshotFixture.generation }));
  expect(hook.result.current.state.sessionActivity).toEqual([{ ...welcome, status: 'finished' }]);
});
