import { useCallback, useEffect, useRef, useState, type Dispatch } from 'react';

import { ChaError, publicErrorMessage, type ChaClient } from './api/client';
import type { SessionEventConnection, SessionEventHandlers } from './api/events';
import {
  currentAppRoute,
  sessionRoute,
  writeAppRoute,
} from './state/route';
import { consumeVoiceSettingsRestore } from './state/voiceSettingsReload';
import type { AppAction, AppState } from './state/view';

// Owns everything a navigation can invalidate: the address bar, the epoch
// marking which conversation the reader is on, and the open/subscribe/recover/
// close ladder under it. They belong together because each cancels the others'
// in-flight work; apart, the cancellation rules sit in a file that cannot see
// what they cancel.
const liveRetryDelays = [250, 500, 1_000, 2_000, 4_000] as const;
const reconnectingMessage = 'Reconnecting live updates…';
const movedMessage = 'This conversation moved to another device';

export type SessionEventsConnector = (
  forumId: string,
  sessionId: string,
  handlers: SessionEventHandlers,
) => SessionEventConnection;

interface AttachedStream {
  key: string;
  events: SessionEventConnection;
  generation: number;
}

// Marks the single replacement subscription as running, so another failure
// cannot start a competing replacement.
interface RecoveryRun {
  forumId: string;
  sessionId: string;
  generation: number;
}

interface SessionTarget {
  forumId: string;
  sessionId: string;
  updateHistory: boolean;
}

function isRetryableSessionOpen(failure: unknown): failure is ChaError {
  return failure instanceof ChaError && (
    failure.code === 'session_limit_reached'
    || failure.code === 'session_stopping'
    || failure.code === 'session_open_timeout'
  );
}

function isSessionLimit(failure: unknown): failure is ChaError {
  return failure instanceof ChaError && failure.code === 'session_limit_reached';
}

// One stable instance: a per-render default would rebuild connectStream and
// every callback and effect that depends on it, on every render.
const noSessionEvents: SessionEventsConnector = () => ({ close() {} });

// These redraw the current screen. Every other action is a navigation intent
// and ends the epoch, which is why the session actions dispatched below go
// straight to `dispatch`: they report on the operation the epoch is guarding,
// and routing one through `navigate` would cancel it.
const inPlaceActions = new Set<AppAction['type']>([
  'toggle-sidebar',
  'bootstrap-refreshed',
  'character-detail-loaded',
  'character-updated',
  'persona-detail-loaded',
  'persona-updated',
  'forum-detail-loaded',
  'forum-updated',
  'provider-detail-loaded',
  'provider-updated',
  'style-detail-loaded',
  'style-updated',
  'voice-detail-loaded',
  'voice-updated',
  'api-key-detail-loaded',
  'api-key-updated',
]);

interface LiveSessionOptions {
  connectSessionEvents?: SessionEventsConnector;
  retryDelays?: readonly number[];
  refreshBootstrap(): Promise<void>;
}

export function useLiveSession(
  client: ChaClient,
  state: AppState,
  dispatch: Dispatch<AppAction>,
  {
    connectSessionEvents = noSessionEvents,
    retryDelays = liveRetryDelays,
    refreshBootstrap,
  }: LiveSessionOptions,
) {
  // The epoch this render was built from. The ref below is what asynchronous
  // work compares against; this is what a render can compare against without
  // reading that ref while rendering.
  const [renderedEpoch, setRenderedEpoch] = useState(0);
  // The address the page started on is adopted once, before the reattach
  // effect below is allowed to act on the reducer's initial conversation.
  const [initialRouteReady, setInitialRouteReady] = useState(false);
  const initialRouteHandled = useRef(false);
  // Bumped by every navigation intent. An open that finishes after the epoch
  // moved on belongs to a conversation the user has already left.
  const navigation = useRef(0);
  const pendingTarget = useRef<{ key: string } | null>(null);
  const retryTarget = useRef<SessionTarget | null>(null);
  const connection = useRef<AttachedStream | null>(null);
  const recovery = useRef<RecoveryRun | null>(null);
  const retryTimerCancellation = useRef<(() => void) | null>(null);
  // Connection resets invalidate recovery without changing navigation intent.
  const liveGeneration = useRef(0);
  // A stream that has already delivered a snapshot can fail at any later
  // moment, and the handler that notices it was built before the ladder that
  // will answer exists. This ref publishes the current starter to those
  // closures; nothing else needs it.
  const recoveryStarter = useRef<(
    forumId: string,
    sessionId: string,
    generation: number,
    reopen?: boolean,
  ) => void>(() => undefined);

  // Every navigation intent goes through here so the ref and the rendered copy
  // of the epoch can never disagree.
  const beginNavigation = useCallback(() => {
    navigation.current += 1;
    setRenderedEpoch(navigation.current);
    return navigation.current;
  }, []);

  const navigate = useCallback((action: AppAction) => {
    if (!inPlaceActions.has(action.type)) beginNavigation();
    dispatch(action);
  }, [beginNavigation, dispatch]);

  const cancelRetryTimer = useCallback(() => {
    retryTimerCancellation.current?.();
    retryTimerCancellation.current = null;
  }, []);

  const waitForRetry = useCallback((milliseconds: number, generation: number) => (
    new Promise<boolean>((resolve) => {
      let settled = false;
      const finish = (completed: boolean) => {
        if (settled) return;
        settled = true;
        retryTimerCancellation.current = null;
        resolve(completed && liveGeneration.current === generation);
      };
      const timer = window.setTimeout(() => finish(true), milliseconds);
      retryTimerCancellation.current = () => {
        window.clearTimeout(timer);
        finish(false);
      };
    })
  ), []);

  const detachStream = useCallback((events: SessionEventConnection) => {
    events.close();
    if (connection.current?.events === events) connection.current = null;
  }, []);

  const resetLiveSession = useCallback(() => {
    cancelRetryTimer();
    recovery.current = null;
    connection.current?.events.close();
    connection.current = null;
    liveGeneration.current += 1;
    return liveGeneration.current;
  }, [cancelRetryTimer]);

  // Welcome and deletion also discard the last open target.
  const clearLiveSession = useCallback(() => {
    resetLiveSession();
    retryTarget.current = null;
  }, [resetLiveSession]);

  const clearVaultContext = useCallback(() => {
    clearLiveSession();
    pendingTarget.current = null;
  }, [clearLiveSession]);

  // `onSettled` belongs to a replacement attempt, which needs to know whether
  // this stream reached its first snapshot. Without one, a failure starts one.
  const connectStream = useCallback((
    forumId: string,
    sessionId: string,
    generation: number,
    reconnecting: boolean,
    onSettled?: (connected: boolean) => void,
  ) => {
    if (liveGeneration.current !== generation) {
      onSettled?.(false);
      return;
    }
    const key = `${forumId}/${sessionId}`;
    let events: SessionEventConnection | null = null;
    const failed = () => {
      if (onSettled) onSettled(false);
      else recoveryStarter.current(forumId, sessionId, generation);
    };
    try {
      events = connectSessionEvents(forumId, sessionId, {
        onSnapshot: (snapshot) => {
          if (!events || connection.current?.events !== events) return;
          dispatch({ type: 'session-snapshot', snapshot });
          if (snapshot.lifecycle !== 'running' && snapshot.shutdown_reason === 'reloading') {
            detachStream(events);
            recoveryStarter.current(forumId, sessionId, generation, true);
            onSettled?.(false);
            return;
          }
          dispatch({ type: 'stream-state', status: 'connected' });
          onSettled?.(true);
        },
        onAppend: (event) => {
          if (!events || connection.current?.events !== events) return;
          dispatch({ type: 'session-append', forumId, sessionId, event });
        },
        onError: (failure) => {
          if (!events || connection.current?.events !== events) return;
          detachStream(events);
          // The reader picked this session up on another device. Recovering
          // here would take it straight back, so this page parks instead and
          // waits for the reader to ask for it again. Ending the generation is
          // what parks it: a replacement in flight, and the late failure callback of
          // a stream that had already connected, both belong to that
          // generation and would otherwise reconnect behind the notice.
          if (failure.kind === 'superseded') {
            cancelRetryTimer();
            recovery.current = null;
            liveGeneration.current += 1;
            dispatch({
              type: 'stream-state',
              status: 'moved',
              message: movedMessage,
            });
            onSettled?.(false);
            return;
          }
          failed();
        },
      });
      connection.current = { key, events, generation };
      dispatch({
        type: 'stream-state',
        status: reconnecting ? 'reconnecting' : 'connecting',
        message: reconnecting ? reconnectingMessage : 'Connecting live updates…',
      });
    } catch {
      if (events) detachStream(events);
      failed();
    }
  }, [cancelRetryTimer, connectSessionEvents, detachStream, dispatch]);

  // One replacement attach: resolves true when the new stream delivers its
  // first snapshot, false when it fails first. A failure after that belongs to
  // a conversation that was working, so it starts recovery afresh.
  const attachStream = useCallback((
    forumId: string,
    sessionId: string,
    generation: number,
  ) => new Promise<boolean>((resolve) => {
    let settled = false;
    connectStream(forumId, sessionId, generation, true, (connected) => {
      if (!settled) {
        settled = true;
        resolve(connected);
      } else if (!connected) {
        recoveryStarter.current(forumId, sessionId, generation);
      }
    });
  }), [connectStream]);

  const beginRecovery = useCallback((
    forumId: string,
    sessionId: string,
    generation: number,
    reopen = false,
  ) => {
    // A reload supersedes a pending reattach, including one whose first
    // snapshot and terminal snapshot arrived in the same delivery turn.
    if (liveGeneration.current !== generation || (recovery.current && !reopen)) return;
    if (reopen) cancelRetryTimer();
    const run: RecoveryRun = { forumId, sessionId, generation };
    recovery.current = run;
    const cancelled = () => recovery.current !== run
      || liveGeneration.current !== generation;
    dispatch({
      type: 'stream-state', status: 'reconnecting',
      message: reopen ? 'Applying settings…' : reconnectingMessage,
    });

    void (async () => {
      for (let attempt = 0; !cancelled(); attempt += 1) {
        try {
          if (reopen) await client.openSession(forumId, sessionId);
          if (cancelled()) return false;
          return await attachStream(forumId, sessionId, generation);
        } catch (failure: unknown) {
          if (!isRetryableSessionOpen(failure)) return false;
        }
        if (cancelled() || attempt >= retryDelays.length) return false;
        if (!await waitForRetry(retryDelays[attempt], generation)) return false;
      }
      return false;
    })().then((connected) => {
      if (cancelled()) return;
      recovery.current = null;
      if (!connected) {
        dispatch({
          type: 'stream-state',
          status: 'retry',
          message: 'Live updates could not be restored.',
        });
      }
    });
  }, [attachStream, cancelRetryTimer, client, dispatch, retryDelays, waitForRetry]);

  // Publish the starter before stream callbacks can run, keeping render free
  // of writes to refs.
  useEffect(() => {
    recoveryStarter.current = beginRecovery;
  }, [beginRecovery]);

  const openWithCapacityRetry = useCallback(async (
    epoch: number,
    generation: number,
    forumId: string,
    sessionId: string,
  ) => {
    for (let attempt = 0; ; attempt += 1) {
      if (navigation.current !== epoch || liveGeneration.current !== generation) return false;
      try {
        await client.openSession(forumId, sessionId);
        return navigation.current === epoch && liveGeneration.current === generation;
      } catch (failure: unknown) {
        if (!isSessionLimit(failure) || attempt >= retryDelays.length) throw failure;
        // The reader may have left while the request was in flight, in which
        // case this attempt must not announce itself on their new screen.
        if (navigation.current !== epoch || liveGeneration.current !== generation) return false;
        dispatch({
          type: 'session-operation-started',
          message: 'Waiting for another session to close',
        });
        if (!await waitForRetry(retryDelays[attempt], generation)) return false;
      }
    }
  }, [client, dispatch, retryDelays, waitForRetry]);

  const performOpen = useCallback(async (
    epoch: number,
    forumId: string,
    sessionId: string,
    updateHistory: boolean,
    refreshRecent: boolean,
  ) => {
    // React StrictMode deliberately replays effect cleanup during development.
    // That cleanup advances the live generation while this asynchronous open
    // is in flight. Retry that one interrupted attempt without asking the user
    // to recover from a framework lifecycle probe.
    for (let interruption = 0; interruption < 2; interruption += 1) {
      const generation = resetLiveSession();
      if (navigation.current !== epoch) return false;
      if (!await openWithCapacityRetry(epoch, generation, forumId, sessionId)) {
        if (navigation.current === epoch) continue;
        return false;
      }
      const snapshot = await client.getSessionSnapshot(forumId, sessionId);
      if (navigation.current !== epoch) return false;
      if (liveGeneration.current !== generation) continue;

      dispatch({ type: 'conversation-opened', snapshot });
      if (updateHistory) {
        writeAppRoute(sessionRoute(snapshot.forum.id, snapshot.session_id));
      }
      connectStream(forumId, sessionId, generation, false);
      if (refreshRecent) void refreshBootstrap();
      return true;
    }
    return false;
  }, [client, connectStream, dispatch, openWithCapacityRetry, refreshBootstrap, resetLiveSession]);

  const openConversation = useCallback(async (
    forumId: string,
    sessionId: string,
    updateHistory = true,
    refreshRecent = true,
  ) => {
    const target = `${forumId}/${sessionId}`;
    retryTarget.current = { forumId, sessionId, updateHistory };
    if (connection.current?.key === target) {
      dispatch({ type: 'show-chat' });
      return true;
    }
    if (pendingTarget.current?.key === target) return false;
    const pending = { key: target };
    pendingTarget.current = pending;
    const epoch = beginNavigation();
    dispatch({ type: 'session-operation-started', message: 'Opening session…' });
    try {
      const opened = await performOpen(
        epoch, forumId, sessionId, updateHistory, refreshRecent,
      );
      if (!opened && navigation.current === epoch) {
        dispatch({
          type: 'session-operation-failed',
          retryable: true,
          message: 'Opening the session was interrupted. Try again.',
        });
      }
      return opened;
    } catch (failure: unknown) {
      if (navigation.current === epoch) {
        const limited = isSessionLimit(failure);
        const retryable = isRetryableSessionOpen(failure);
        dispatch({
          type: 'session-operation-failed',
          retryable,
          message: limited
            ? 'Another session has not closed yet. Try again.'
            : publicErrorMessage(failure, 'The requested session could not be opened.'),
        });
      }
      return false;
    } finally {
      if (pendingTarget.current === pending) pendingTarget.current = null;
    }
  }, [beginNavigation, dispatch, performOpen]);

  const createConversation = useCallback(async (forumId: string, label: string) => {
    const target = `${forumId}/new/${label}`;
    if (pendingTarget.current?.key === target) return false;
    const pending = { key: target };
    pendingTarget.current = pending;
    retryTarget.current = null;
    const epoch = beginNavigation();
    dispatch({ type: 'session-operation-started', message: 'Creating session…' });
    try {
      const created = await client.createSession(forumId, label);
      // Cancelling does not un-create the session the server already wrote, so
      // it has to appear in Recent rather than becoming a session nobody sees.
      if (navigation.current !== epoch) {
        void refreshBootstrap();
        return false;
      }
      retryTarget.current = { forumId, sessionId: created.id, updateHistory: true };
      dispatch({ type: 'session-operation-started', message: 'Opening session…' });
      const opened = await performOpen(epoch, forumId, created.id, true, true);
      if (!opened && navigation.current === epoch) {
        dispatch({
          type: 'session-operation-failed',
          retryable: true,
          message: 'Opening the new session was interrupted. Try again.',
        });
      }
      return opened;
    } catch (failure: unknown) {
      if (navigation.current === epoch) {
        const limited = isSessionLimit(failure);
        const retryable = isRetryableSessionOpen(failure);
        dispatch({
          type: 'session-operation-failed',
          retryable: retryable && retryTarget.current !== null,
          message: limited
            ? 'Another session has not closed yet. Try again.'
            : publicErrorMessage(failure, 'The session could not be created.'),
        });
      }
      return false;
    } finally {
      if (pendingTarget.current === pending) pendingTarget.current = null;
    }
  }, [beginNavigation, client, dispatch, performOpen, refreshBootstrap]);

  const retrySessionOpen = useCallback(() => {
    const target = retryTarget.current;
    if (target) void openConversation(target.forumId, target.sessionId, target.updateHistory);
  }, [openConversation]);

  const retryStream = useCallback(() => {
    const active = state.activeConversation;
    if (!active) return;
    cancelRetryTimer();
    connection.current?.events.close();
    connection.current = null;
    recovery.current = null;
    beginRecovery(active.forumId, active.sessionId, liveGeneration.current,
      state.sessionSnapshot?.shutdown_reason === 'reloading');
  }, [beginRecovery, cancelRetryTimer, state.activeConversation, state.sessionSnapshot]);

  useEffect(() => {
    if (state.bootstrapStatus !== 'ready' || initialRouteHandled.current) return;
    initialRouteHandled.current = true;
    const restoreVoiceSettings = consumeVoiceSettingsRestore();
    const route = currentAppRoute();
    if (route.kind === 'root') {
      if (restoreVoiceSettings) navigate({ type: 'show-settings-voice-input' });
      setInitialRouteReady(true);
    } else if (route.kind === 'session') {
      void openConversation(route.forumId, route.sessionId, false)
        .finally(() => {
          if (restoreVoiceSettings) navigate({ type: 'show-settings-voice-input' });
          setInitialRouteReady(true);
        });
    } else {
      dispatch({
        type: 'session-operation-failed',
        message: 'This address does not identify a CHA session.',
      });
      setInitialRouteReady(true);
    }
  }, [dispatch, navigate, openConversation, state.bootstrapStatus]);

  // Startup and Return to Welcome both adopt the initial IDs without an open
  // request. A matching snapshot without a connection is also attachable: this
  // matters when React StrictMode replays the unmount cleanup after the
  // snapshot was committed but before the stream can remain attached.
  useEffect(() => {
    const active = state.activeConversation;
    // A click can land after this render but before its effect runs. In that
    // case the captured Chat view is stale and must not reopen over the user's
    // new navigation screen.
    if (navigation.current !== renderedEpoch) return;
    if (!initialRouteReady || state.bootstrapStatus !== 'ready' || !active) return;
    if (state.mainView !== 'chat') return;
    if (state.sessionOperation !== 'idle') return;
    if (pendingTarget.current || connection.current || recovery.current) return;
    const snapshotMatches = state.sessionSnapshot?.forum.id === active.forumId
      && state.sessionSnapshot.session_id === active.sessionId;
    if (snapshotMatches) {
      if (state.sessionSnapshot?.lifecycle !== 'running'
          || state.streamStatus === 'retry'
          || state.streamStatus === 'moved') return;
      connectStream(
        active.forumId,
        active.sessionId,
        liveGeneration.current,
        state.streamStatus !== 'connecting',
      );
      return;
    }
    void openConversation(active.forumId, active.sessionId, false, false);
  }, [connectStream, initialRouteReady, openConversation, renderedEpoch,
    state.activeConversation, state.bootstrapStatus, state.mainView,
    state.sessionOperation, state.sessionSnapshot, state.streamStatus]);

  useEffect(() => {
    const visitHistoryRoute = () => {
      const route = currentAppRoute();
      if (route.kind === 'root') {
        clearLiveSession();
        navigate({ type: 'show-initial-conversation' });
      } else if (route.kind === 'session') {
        void openConversation(route.forumId, route.sessionId, false);
      } else {
        navigate({
          type: 'session-operation-failed',
          message: 'This address does not identify a CHA session.',
        });
      }
    };
    window.addEventListener('popstate', visitHistoryRoute);
    window.addEventListener('hashchange', visitHistoryRoute);
    return () => {
      window.removeEventListener('popstate', visitHistoryRoute);
      window.removeEventListener('hashchange', visitHistoryRoute);
    };
  }, [clearLiveSession, navigate, openConversation]);

  useEffect(() => () => {
    resetLiveSession();
  }, [resetLiveSession]);

  return {
    navigate,
    openConversation,
    createConversation,
    retrySessionOpen,
    retryStream,
    clearLiveSession,
    clearVaultContext,
  };
}
