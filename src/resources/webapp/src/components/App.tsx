import {
  useCallback,
  useEffect,
  useReducer,
  useRef,
  useState,
  type Dispatch,
} from 'react';

import {
  ChaError,
  chaClient,
  type Bootstrap,
  type ChaClient,
} from '../api/client';
import {
  openSessionEvents,
  type SessionEventConnection,
  type SessionEventHandlers,
} from '../api/events';
import { validateBootstrap } from '../state/bootstrap';
import { parseAppRoute, sessionRoute } from '../state/route';
import {
  appReducer,
  initialAppState,
  navigationTitle,
  type AppAction,
  type AppState,
} from '../state/view';
import { MenuIcon } from './Icons';
import {
  CharacterDetailScreen,
  CharactersScreen,
  ChatScreen,
  ForumsScreen,
  NewSessionScreen,
  PersonasScreen,
  SessionsScreen,
  type ChatActions,
} from './Screens';
import { Sidebar } from './Sidebar';

export const liveRetryDelays = [250, 500, 1_000, 2_000, 4_000] as const;

interface ScreenProps extends ChatActions {
  state: AppState;
  dispatch: Dispatch<AppAction>;
  client: ChaClient;
  onCreateSession(forumId: string, label: string): Promise<boolean>;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
  onRetrySession(): void;
}

function Screen({
  state,
  dispatch,
  client,
  onCreateSession,
  onOpenSession,
  onRetrySession,
  onRetryStream,
  onReturnToWelcome,
  onSetDefaultCharacter,
  onStopGeneration,
  onSubmitInput,
}: ScreenProps) {
  switch (state.mainView) {
    case 'chat': return (
      <ChatScreen
        dispatch={dispatch}
        onRetryStream={onRetryStream}
        onReturnToWelcome={onReturnToWelcome}
        onSetDefaultCharacter={onSetDefaultCharacter}
        onStopGeneration={onStopGeneration}
        onSubmitInput={onSubmitInput}
        state={state}
      />
    );
    case 'personas': return <PersonasScreen state={state} dispatch={dispatch} />;
    case 'characters': return <CharactersScreen state={state} dispatch={dispatch} />;
    case 'character-detail': return (
      <CharacterDetailScreen state={state} dispatch={dispatch} client={client} />
    );
    case 'forums': return <ForumsScreen state={state} dispatch={dispatch} />;
    case 'sessions': return (
      <SessionsScreen
        client={client}
        dispatch={dispatch}
        onOpenSession={onOpenSession}
        onRetrySession={onRetrySession}
        onReturnToWelcome={onReturnToWelcome}
        state={state}
      />
    );
    case 'new-session': return (
      <NewSessionScreen
        dispatch={dispatch}
        onCreateSession={onCreateSession}
        onRetrySession={onRetrySession}
        onReturnToWelcome={onReturnToWelcome}
        state={state}
      />
    );
  }
}

function BootstrapState({ state }: { state: AppState }) {
  if (state.bootstrapStatus === 'loading') {
    return <p className="cha-bootstrap-state" role="status">Loading workspace…</p>;
  }
  return (
    <div className="cha-bootstrap-state" role="alert">
      <h2>
        {state.bootstrapStatus === 'incompatible'
          ? 'Incompatible application response'
          : 'Workspace could not be loaded'}
      </h2>
      <p>{state.bootstrapMessage}</p>
    </div>
  );
}

// Shown only when the operation concerns the whole application — a deep link or
// a history entry the browser is restoring. An operation started from a
// navigation screen reports itself on that screen instead, so the session list
// or the half-typed session name survives the failure.
function SessionOperationState({
  state,
  onRetry,
  onReturnToWelcome,
}: {
  state: AppState;
  onRetry(): void;
  onReturnToWelcome(): void;
}) {
  if (state.sessionOperation === 'pending') {
    return (
      <p className="cha-state-message" role="status">
        {state.sessionOperationMessage ?? 'Opening session…'}
      </p>
    );
  }
  return (
    <div className="cha-bootstrap-state" role="alert">
      <h2>Session unavailable</h2>
      <p>{state.sessionOperationMessage}</p>
      <div className="cha-state-actions">
        {state.sessionOperationRetryable && (
          <button className="cha-button cha-button-ghost" onClick={onRetry} type="button">
            Retry
          </button>
        )}
        <button className="cha-button cha-button-primary" onClick={onReturnToWelcome} type="button">
          Return to Welcome
        </button>
      </div>
    </div>
  );
}

// Every navigation supersedes an open still in flight, so a slow one cannot
// land afterwards and pull the user into a conversation they have left. These
// actions change no view and must therefore supersede nothing.
const inPlaceActions = new Set<AppAction['type']>([
  'toggle-sidebar',
  'select-persona',
]);

export type SessionEventsConnector = (
  forumId: string,
  sessionId: string,
  handlers: SessionEventHandlers,
) => SessionEventConnection;

interface AppProps {
  client?: ChaClient;
  connectSessionEvents?: SessionEventsConnector;
  retryDelays?: readonly number[];
}

interface AttachedStream {
  key: string;
  events: SessionEventConnection;
  generation: number;
}

interface StreamRecovery {
  key: string;
  forumId: string;
  sessionId: string;
  generation: number;
  nextDelayIndex: number;
  everyProbeSucceeded: boolean;
  working: boolean;
}

interface SessionTarget {
  forumId: string;
  sessionId: string;
  updateHistory: boolean;
}

function isSessionLimit(failure: unknown): failure is ChaError {
  return failure instanceof ChaError && failure.code === 'session_limit_reached';
}

function isRetryableSessionOpen(failure: unknown): failure is ChaError {
  return failure instanceof ChaError && (
    failure.code === 'session_limit_reached'
    || failure.code === 'session_busy'
    || failure.code === 'session_stopping'
    || failure.code === 'session_open_timeout'
  );
}

function isSessionNotLive(failure: unknown): failure is ChaError {
  return failure instanceof ChaError && failure.code === 'session_not_live';
}

export function App({
  client = chaClient,
  connectSessionEvents = openSessionEvents,
  retryDelays = liveRetryDelays,
}: AppProps) {
  const [state, dispatch] = useReducer(appReducer, initialAppState);
  const [initialRouteReady, setInitialRouteReady] = useState(false);
  // The epoch this render was built from. The ref below is what asynchronous
  // work compares against; this is what a render can compare against without
  // reading that ref while rendering.
  const [renderedEpoch, setRenderedEpoch] = useState(0);
  const request = useRef<{ client: ChaClient; promise: Promise<Bootstrap> } | null>(null);
  const pendingTarget = useRef<string | null>(null);
  const retryTarget = useRef<SessionTarget | null>(null);
  const pendingMutations = useRef(new Set<string>());
  const connection = useRef<AttachedStream | null>(null);
  const recovery = useRef<StreamRecovery | null>(null);
  const retryTimerCancellation = useRef<(() => void) | null>(null);
  const liveGeneration = useRef(0);
  const recoveryStarter = useRef<(
    forumId: string,
    sessionId: string,
    generation: number,
  ) => void>(() => undefined);
  const initialRouteHandled = useRef(false);
  // Bumped by every navigation intent. An open that finishes after the epoch
  // moved on belongs to a conversation the user has already left.
  const navigation = useRef(0);

  useEffect(() => {
    if (request.current?.client !== client) {
      request.current = { client, promise: client.getBootstrap() };
    }
    let current = true;
    void request.current.promise.then(
      (response) => {
        if (!current) return;
        try {
          dispatch({ type: 'bootstrap-loaded', bootstrap: validateBootstrap(response) });
        } catch (failure: unknown) {
          dispatch({
            type: 'bootstrap-failed',
            incompatible: true,
            message: failure instanceof Error
              ? failure.message
              : 'CHA returned an incompatible bootstrap response.',
          });
        }
      },
      (failure: unknown) => {
        if (!current) return;
        dispatch({
          type: 'bootstrap-failed',
          incompatible: false,
          message: failure instanceof Error ? failure.message : 'The request failed.',
        });
      },
    );
    return () => {
      current = false;
    };
  }, [client]);

  const refreshBootstrap = useCallback(async () => {
    try {
      dispatch({
        type: 'bootstrap-refreshed',
        bootstrap: validateBootstrap(await client.getBootstrap()),
      });
    } catch {
      // The live snapshot remains usable. Discovery refresh is non-critical.
    }
  }, [client]);

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
  }, [beginNavigation]);

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

  const connectStream = useCallback((
    forumId: string,
    sessionId: string,
    generation: number,
    reconnecting: boolean,
  ) => {
    if (liveGeneration.current !== generation) return false;
    const key = `${forumId}/${sessionId}`;
    let events: SessionEventConnection | null = null;
    try {
      events = connectSessionEvents(forumId, sessionId, {
        onSnapshot: (snapshot) => {
          if (!events || connection.current?.events !== events) return;
          recovery.current = null;
          cancelRetryTimer();
          dispatch({ type: 'session-snapshot', snapshot });
          dispatch({ type: 'stream-state', status: 'connected' });
        },
        onAppend: (event) => {
          if (!events || connection.current?.events !== events) return;
          dispatch({ type: 'session-append', forumId, sessionId, event });
        },
        onError: () => {
          if (!events || connection.current?.events !== events) return;
          detachStream(events);
          recoveryStarter.current(forumId, sessionId, generation);
        },
      });
      connection.current = { key, events, generation };
      dispatch({
        type: 'stream-state',
        status: reconnecting ? 'reconnecting' : 'connecting',
        message: reconnecting ? 'Reconnecting live updates…' : 'Connecting live updates…',
      });
      return true;
    } catch {
      if (events) detachStream(events);
      recoveryStarter.current(forumId, sessionId, generation);
      return false;
    }
  }, [cancelRetryTimer, connectSessionEvents, detachStream]);

  const finishRecovery = useCallback((current: StreamRecovery) => {
    if (recovery.current !== current || liveGeneration.current !== current.generation) return;
    recovery.current = null;
    if (current.everyProbeSucceeded) {
      dispatch({
        type: 'stream-state',
        status: 'other-window',
        message: 'This session is open in another window',
      });
    } else {
      dispatch({
        type: 'stream-state',
        status: 'retry',
        message: 'Live updates could not be restored.',
      });
    }
  }, []);

  const runRecoveryAttempt = useCallback(async (current: StreamRecovery) => {
    if (recovery.current !== current || liveGeneration.current !== current.generation) return;
    if (current.nextDelayIndex >= retryDelays.length) {
      finishRecovery(current);
      return;
    }

    current.working = true;
    const delay = retryDelays[current.nextDelayIndex];
    current.nextDelayIndex += 1;
    let readyToConnect = false;
    let waitingForCapacity = false;

    try {
      const snapshot = await client.getSessionSnapshot(current.forumId, current.sessionId);
      if (recovery.current !== current || liveGeneration.current !== current.generation) return;
      dispatch({ type: 'session-snapshot', snapshot });
      readyToConnect = true;
    } catch (failure: unknown) {
      current.everyProbeSucceeded = false;
      if (isSessionNotLive(failure)) {
        try {
          await client.openSession(current.forumId, current.sessionId);
          if (recovery.current !== current || liveGeneration.current !== current.generation) return;
          const snapshot = await client.getSessionSnapshot(current.forumId, current.sessionId);
          if (recovery.current !== current || liveGeneration.current !== current.generation) return;
          dispatch({ type: 'session-snapshot', snapshot });
          readyToConnect = true;
        } catch (openFailure: unknown) {
          waitingForCapacity = isSessionLimit(openFailure);
        }
      }
    }

    if (recovery.current !== current || liveGeneration.current !== current.generation) return;
    dispatch({
      type: 'stream-state',
      status: 'reconnecting',
      message: waitingForCapacity
        ? 'Waiting for another session to close'
        : 'Reconnecting live updates…',
    });
    if (!await waitForRetry(delay, current.generation)) return;
    if (recovery.current !== current || liveGeneration.current !== current.generation) return;
    current.working = false;

    if (readyToConnect) {
      connectStream(current.forumId, current.sessionId, current.generation, true);
    } else {
      recoveryStarter.current(current.forumId, current.sessionId, current.generation);
    }
  }, [client, connectStream, finishRecovery, retryDelays, waitForRetry]);

  const startRecovery = useCallback((
    forumId: string,
    sessionId: string,
    generation: number,
  ) => {
    if (liveGeneration.current !== generation) return;
    const key = `${forumId}/${sessionId}`;
    let current = recovery.current;
    if (!current || current.key !== key || current.generation !== generation) {
      current = {
        key,
        forumId,
        sessionId,
        generation,
        nextDelayIndex: 0,
        everyProbeSucceeded: true,
        working: false,
      };
      recovery.current = current;
      dispatch({
        type: 'stream-state',
        status: 'reconnecting',
        message: 'Reconnecting live updates…',
      });
    }
    if (current.working) return;
    if (current.nextDelayIndex >= retryDelays.length) {
      finishRecovery(current);
      return;
    }
    void runRecoveryAttempt(current);
  }, [finishRecovery, retryDelays.length, runRecoveryAttempt]);

  // A stream error is the only caller, and a stream cannot exist before the
  // effects of the commit that created it have run, so publishing the current
  // starter here is early enough and keeps the render itself free of writes.
  useEffect(() => {
    recoveryStarter.current = startRecovery;
  }, [startRecovery]);

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
        dispatch({
          type: 'session-operation-started',
          message: 'Waiting for another session to close',
        });
        if (!await waitForRetry(retryDelays[attempt], generation)) return false;
      }
    }
  }, [client, retryDelays, waitForRetry]);

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
        window.history.pushState(null, '', sessionRoute(snapshot.forum.id, snapshot.session_id));
      }
      connectStream(forumId, sessionId, generation, false);
      if (refreshRecent) void refreshBootstrap();
      return true;
    }
    return false;
  }, [client, connectStream, openWithCapacityRetry, refreshBootstrap, resetLiveSession]);

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
    if (pendingTarget.current === target) return false;
    pendingTarget.current = target;
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
            : failure instanceof Error
              ? failure.message
              : 'The requested session could not be opened.',
        });
      }
      return false;
    } finally {
      if (pendingTarget.current === target) pendingTarget.current = null;
    }
  }, [beginNavigation, performOpen]);

  const createConversation = useCallback(async (forumId: string, label: string) => {
    const target = `${forumId}/new/${label}`;
    if (pendingTarget.current === target) return false;
    pendingTarget.current = target;
    const epoch = beginNavigation();
    dispatch({ type: 'session-operation-started', message: 'Creating session…' });
    try {
      const created = await client.createSession(forumId, label);
      if (navigation.current !== epoch) return false;
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
          retryable,
          message: limited
            ? 'Another session has not closed yet. Try again.'
            : failure instanceof Error
              ? failure.message
              : 'The session could not be created.',
        });
      }
      return false;
    } finally {
      if (pendingTarget.current === target) pendingTarget.current = null;
    }
  }, [beginNavigation, client, performOpen]);

  const returnToWelcome = useCallback(() => {
    resetLiveSession();
    retryTarget.current = null;
    navigate({ type: 'show-initial-conversation' });
    window.history.pushState(null, '', '/');
  }, [navigate, resetLiveSession]);

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
    startRecovery(active.forumId, active.sessionId, liveGeneration.current);
  }, [cancelRetryTimer, startRecovery, state.activeConversation]);

  const runMutation = useCallback(async <T,>(key: string, operation: () => Promise<T>) => {
    if (pendingMutations.current.has(key)) {
      throw new Error('That action is already in progress.');
    }
    pendingMutations.current.add(key);
    try {
      return await operation();
    } finally {
      pendingMutations.current.delete(key);
    }
  }, []);

  const submitInput = useCallback((text: string) => {
    const active = state.activeConversation;
    const persona = state.currentPersonaId;
    if (!active || !persona) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation('submit', () => client.submitInput(
      active.forumId,
      active.sessionId,
      { persona, text },
    ));
  }, [client, runMutation, state.activeConversation, state.currentPersonaId]);

  const stopGeneration = useCallback(() => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation('stop', () => client.stopGeneration(active.forumId, active.sessionId));
  }, [client, runMutation, state.activeConversation]);

  const setDefaultCharacter = useCallback((characterId: string) => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation('target', () => client.setDefaultCharacter(
      active.forumId,
      active.sessionId,
      characterId,
    ));
  }, [client, runMutation, state.activeConversation]);

  useEffect(() => {
    if (state.bootstrapStatus !== 'ready' || initialRouteHandled.current) return;
    initialRouteHandled.current = true;
    const route = parseAppRoute(window.location.pathname);
    if (route.kind === 'root') {
      setInitialRouteReady(true);
    } else if (route.kind === 'session') {
      void openConversation(route.forumId, route.sessionId, false)
        .finally(() => setInitialRouteReady(true));
    } else {
      dispatch({
        type: 'session-operation-failed',
        message: 'This address does not identify a CHA session.',
      });
      setInitialRouteReady(true);
    }
  }, [openConversation, state.bootstrapStatus]);

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
          || state.streamStatus === 'other-window') return;
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
      const route = parseAppRoute(window.location.pathname);
      if (route.kind === 'root') {
        resetLiveSession();
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
    return () => window.removeEventListener('popstate', visitHistoryRoute);
  }, [navigate, openConversation, resetLiveSession]);

  useEffect(() => () => {
    resetLiveSession();
  }, [resetLiveSession]);

  const title = navigationTitle(state);
  const ready = state.bootstrapStatus === 'ready';
  const wholeApplication = state.sessionOperation !== 'idle' && state.mainView === 'chat';

  return (
    <div
      className={`cha-app ${state.sidebarOpen ? 'is-sidebar-open' : ''}`}
      data-sidebar={state.sidebarOpen ? 'open' : 'closed'}
    >
      <Sidebar dispatch={navigate} onOpenSession={openConversation} state={state} />
      <main className="cha-main">
        <header className="cha-topbar">
          <button
            aria-expanded={state.sidebarOpen}
            aria-label={state.sidebarOpen ? 'Hide sidebar' : 'Show sidebar'}
            className="cha-icon-action"
            onClick={() => dispatch({ type: 'toggle-sidebar' })}
            type="button"
          >
            <MenuIcon />
          </button>
          <div className="cha-topbar-title">{title && <h1>{title}</h1>}</div>
          <div className="cha-empty-action" aria-hidden="true" />
        </header>
        {!ready && <BootstrapState state={state} />}
        {ready && wholeApplication && (
          <SessionOperationState
            onRetry={retrySessionOpen}
            onReturnToWelcome={returnToWelcome}
            state={state}
          />
        )}
        {ready && !wholeApplication && (
          <Screen
            client={client}
            dispatch={navigate}
            onCreateSession={createConversation}
            onOpenSession={openConversation}
            onRetryStream={retryStream}
            onRetrySession={retrySessionOpen}
            onReturnToWelcome={returnToWelcome}
            onSetDefaultCharacter={setDefaultCharacter}
            onStopGeneration={stopGeneration}
            onSubmitInput={submitInput}
            state={state}
          />
        )}
      </main>
    </div>
  );
}
