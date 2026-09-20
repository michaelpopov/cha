import { useReducer } from 'react';
import { act, renderHook } from '@testing-library/react';
import { beforeEach, expect, it, vi } from 'vitest';

import type { ChaClient, OpenSessionResult } from './api/client';
import type { SessionEventHandlers } from './api/events';
import { appReducer, initialAppState } from './state/view';
import { fixtureClient, snapshotFixture } from './test/fixtures';
import { useLiveSession } from './useLiveSession';

beforeEach(() => {
  window.history.replaceState(null, '', '/');
});

// Hands back the handlers so a test can drive the stream the hook owns.
function drivableSessionEvents() {
  const handlers: SessionEventHandlers[] = [];
  const closes: ReturnType<typeof vi.fn>[] = [];
  return {
    handlers,
    closes,
    connect(_forumId: string, _sessionId: string, given: SessionEventHandlers) {
      handlers.push(given);
      const close = vi.fn();
      closes.push(close);
      return { close };
    },
  };
}

// Bootstrap stays loading, so the startup route and reattach effects stand
// aside and each test drives the ladder through the calls it names.
function harness(overrides: Partial<ChaClient> = {}) {
  const events = drivableSessionEvents();
  const client = fixtureClient(overrides);
  const view = renderHook(() => {
    const [state, dispatch] = useReducer(appReducer, initialAppState);
    return {
      state,
      live: useLiveSession(client, state, dispatch, {
        connectSessionEvents: events.connect,
        refreshBootstrap: async () => undefined,
      }),
    };
  });
  return { events, view };
}

// Holds openSession open so a test can navigate while the open is in flight.
function haltedOpen() {
  let release: (result: OpenSessionResult) => void = () => undefined;
  const openSession = () => new Promise<OpenSessionResult>((resolve) => {
    release = resolve;
  });
  return {
    openSession,
    release: () => release({ forum_id: 'entrance', session_id: 'welcome' }),
  };
}

it('keeps an in-flight open alive across an in-place action', async () => {
  const halted = haltedOpen();
  const { events, view } = harness({ openSession: halted.openSession });

  let opened: Promise<boolean> | null = null;
  act(() => {
    opened = view.result.current.live.openConversation('entrance', 'welcome');
  });
  act(() => {
    view.result.current.live.navigate({ type: 'toggle-sidebar' });
  });
  await act(async () => {
    halted.release();
    expect(await opened).toBe(true);
  });

  expect(view.result.current.state.activeConversation)
    .toEqual({ forumId: 'entrance', sessionId: 'welcome' });
  expect(events.handlers).toHaveLength(1);
});

it('abandons an in-flight open when the reader navigates away', async () => {
  const halted = haltedOpen();
  const { events, view } = harness({ openSession: halted.openSession });

  let opened: Promise<boolean> | null = null;
  act(() => {
    opened = view.result.current.live.openConversation('entrance', 'welcome');
  });
  act(() => {
    view.result.current.live.navigate({ type: 'show-personas' });
  });
  await act(async () => {
    halted.release();
    expect(await opened).toBe(false);
  });

  expect(view.result.current.state.activeConversation).toBeNull();
  expect(events.handlers).toHaveLength(0);
});

it('reports the stream connected once its first snapshot arrives', async () => {
  const { events, view } = harness();

  await act(async () => {
    await view.result.current.live.openConversation('entrance', 'welcome');
  });
  expect(view.result.current.state.streamStatus).toBe('connecting');

  act(() => {
    events.handlers[0].onSnapshot(snapshotFixture);
  });
  expect(view.result.current.state.streamStatus).toBe('connected');
});

it('replaces the stream after it fails, without a second replacement', async () => {
  const { events, view } = harness();

  await act(async () => {
    await view.result.current.live.openConversation('entrance', 'welcome');
  });
  act(() => {
    events.handlers[0].onSnapshot(snapshotFixture);
  });

  await act(async () => {
    events.handlers[0].onError({ kind: 'stream_failure' });
  });
  expect(events.handlers).toHaveLength(2);

  act(() => {
    events.handlers[1].onSnapshot(snapshotFixture);
  });
  expect(view.result.current.state.streamStatus).toBe('connected');
});

it('parks instead of recovering when the session moves to another device', async () => {
  const { events, view } = harness();

  await act(async () => {
    await view.result.current.live.openConversation('entrance', 'welcome');
  });
  act(() => {
    events.handlers[0].onSnapshot(snapshotFixture);
  });

  await act(async () => {
    events.handlers[0].onError({ kind: 'superseded' });
  });

  expect(view.result.current.state.streamStatus).toBe('moved');
  expect(events.handlers).toHaveLength(1);
});

it('closes the live stream when the session is cleared', async () => {
  const { events, view } = harness();

  await act(async () => {
    await view.result.current.live.openConversation('entrance', 'welcome');
  });
  act(() => {
    view.result.current.live.clearLiveSession();
  });

  expect(events.closes[0]).toHaveBeenCalled();
});
