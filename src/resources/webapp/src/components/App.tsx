import { useCallback, useEffect, useReducer, useRef, type Dispatch } from 'react';

import { chaClient, type Bootstrap, type ChaClient } from '../api/client';
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
} from './Screens';
import { Sidebar } from './Sidebar';

interface ScreenProps {
  state: AppState;
  dispatch: Dispatch<AppAction>;
  client: ChaClient;
  onCreateSession(forumId: string, label: string): Promise<boolean>;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
}

function Screen({
  state,
  dispatch,
  client,
  onCreateSession,
  onOpenSession,
}: ScreenProps) {
  switch (state.mainView) {
    case 'chat': return <ChatScreen state={state} />;
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
        state={state}
      />
    );
    case 'new-session': return (
      <NewSessionScreen
        dispatch={dispatch}
        onCreateSession={onCreateSession}
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
  onReturnToWelcome,
}: {
  state: AppState;
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
      <button className="cha-button cha-button-primary" onClick={onReturnToWelcome} type="button">
        Return to Welcome
      </button>
    </div>
  );
}

// Every navigation supersedes an open still in flight, so a slow one cannot
// land afterwards and pull the user into a conversation they have left. These
// are the actions that change no view and must therefore supersede nothing.
const inPlaceActions = new Set<AppAction['type']>([
  'toggle-sidebar',
  'select-persona',
  'set-default-character',
]);

export type SessionEventsConnector = (
  forumId: string,
  sessionId: string,
  handlers: SessionEventHandlers,
) => SessionEventConnection;

interface AppProps {
  client?: ChaClient;
  connectSessionEvents?: SessionEventsConnector;
}

export function App({
  client = chaClient,
  connectSessionEvents = openSessionEvents,
}: AppProps) {
  const [state, dispatch] = useReducer(appReducer, initialAppState);
  const request = useRef<{ client: ChaClient; promise: Promise<Bootstrap> } | null>(null);
  // What the operation in flight is working towards. A second request for the
  // same thing is the duplicate click this guards against; a request for a
  // different one is a navigation and supersedes it.
  const pendingTarget = useRef<string | null>(null);
  const connection = useRef<{ key: string; events: SessionEventConnection } | null>(null);
  const initialRouteHandled = useRef(false);
  // Bumped by every navigation intent. An open that finishes after the epoch
  // moved on belongs to a conversation the user has already left, so it must
  // not attach its stream, its state, or its URL.
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
      // The active snapshot remains usable. A later Stage 4 recovery or reload
      // will retry discovery if this non-critical Recent refresh failed.
    }
  }, [client]);

  // The dispatch every view receives. App's own bookkeeping uses the raw one,
  // because reporting an operation is not a navigation away from it.
  const navigate = useCallback((action: AppAction) => {
    if (!inPlaceActions.has(action.type)) navigation.current += 1;
    dispatch(action);
  }, []);

  const closeStream = useCallback(() => {
    connection.current?.events.close();
    connection.current = null;
  }, []);

  // Closes one particular stream. A superseded open must not close whatever is
  // attached now, because that belongs to the navigation that replaced it.
  const detachStream = useCallback((events: SessionEventConnection) => {
    events.close();
    if (connection.current?.events === events) connection.current = null;
  }, []);

  const performOpen = useCallback(async (
    epoch: number,
    forumId: string,
    sessionId: string,
    updateHistory: boolean,
  ) => {
    closeStream();
    // Opening is what makes a session live, and the contract has no operation
    // that releases one, so an abandoned open holds a slot until the server's
    // idle grace expires. Checking here cannot close that window — the epoch
    // can move while the request is in flight — but it does keep a navigation
    // that already happened from starting one more.
    if (navigation.current !== epoch) return false;
    await client.openSession(forumId, sessionId);
    const snapshot = await client.getSessionSnapshot(forumId, sessionId);
    if (navigation.current !== epoch) return false;

    const key = `${snapshot.forum.id}/${snapshot.session_id}`;
    let events: SessionEventConnection | null = null;
    events = connectSessionEvents(forumId, sessionId, {
      onSnapshot: (nextSnapshot) => dispatch({ type: 'session-snapshot', snapshot: nextSnapshot }),
      onAppend: () => {
        // Stage 4 adds transcript projection. Stage 3 establishes ownership of
        // the live stream so an opened browser session remains attached.
      },
      onError: () => {
        if (!events) return;
        // Only the attached stream's failure is the user's problem; a stream
        // already replaced by a later navigation is not.
        const attached = connection.current?.events === events;
        detachStream(events);
        if (attached) dispatch({ type: 'stream-lost' });
      },
    });
    connection.current = { key, events };
    await refreshBootstrap();
    if (navigation.current !== epoch) {
      detachStream(events);
      return false;
    }

    dispatch({ type: 'conversation-opened', snapshot });
    if (updateHistory) {
      window.history.pushState(null, '', sessionRoute(snapshot.forum.id, snapshot.session_id));
    }
    return true;
  }, [client, closeStream, connectSessionEvents, detachStream, refreshBootstrap]);

  const openConversation = useCallback(async (
    forumId: string,
    sessionId: string,
    updateHistory = true,
  ) => {
    const target = `${forumId}/${sessionId}`;
    // The session already holds this browser's stream, so returning to it is a
    // view change rather than a reattach.
    if (connection.current?.key === target) {
      dispatch({ type: 'show-chat' });
      return true;
    }
    if (pendingTarget.current === target) return false;
    pendingTarget.current = target;
    const epoch = (navigation.current += 1);
    dispatch({ type: 'session-operation-started', message: 'Opening session…' });
    try {
      return await performOpen(epoch, forumId, sessionId, updateHistory);
    } catch (failure: unknown) {
      if (navigation.current === epoch) {
        dispatch({
          type: 'session-operation-failed',
          message: failure instanceof Error
            ? failure.message
            : 'The requested session could not be opened.',
        });
      }
      return false;
    } finally {
      if (pendingTarget.current === target) pendingTarget.current = null;
    }
  }, [performOpen]);

  const createConversation = useCallback(async (forumId: string, label: string) => {
    // A session that does not exist yet has no identity to compare, so the
    // forum plus the label is what a repeated submission would name.
    const target = `${forumId}/new/${label}`;
    if (pendingTarget.current === target) return false;
    pendingTarget.current = target;
    const epoch = (navigation.current += 1);
    dispatch({ type: 'session-operation-started', message: 'Creating session…' });
    try {
      const created = await client.createSession(forumId, label);
      if (navigation.current !== epoch) return false;
      dispatch({ type: 'session-operation-started', message: 'Opening session…' });
      return await performOpen(epoch, forumId, created.id, true);
    } catch (failure: unknown) {
      if (navigation.current === epoch) {
        dispatch({
          type: 'session-operation-failed',
          message: failure instanceof Error
            ? failure.message
            : 'The session could not be created.',
        });
      }
      return false;
    } finally {
      if (pendingTarget.current === target) pendingTarget.current = null;
    }
  }, [client, performOpen]);

  const returnToWelcome = useCallback(() => {
    closeStream();
    navigate({ type: 'show-initial-conversation' });
    window.history.pushState(null, '', '/');
  }, [closeStream, navigate]);

  useEffect(() => {
    if (state.bootstrapStatus !== 'ready' || initialRouteHandled.current) return;
    initialRouteHandled.current = true;
    const route = parseAppRoute(window.location.pathname);
    if (route.kind === 'session') {
      void openConversation(route.forumId, route.sessionId, false);
    } else if (route.kind === 'invalid') {
      dispatch({
        type: 'session-operation-failed',
        message: 'This address does not identify a CHA session.',
      });
    }
  }, [openConversation, state.bootstrapStatus]);

  useEffect(() => {
    const visitHistoryRoute = () => {
      const route = parseAppRoute(window.location.pathname);
      if (route.kind === 'root') {
        closeStream();
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
  }, [closeStream, navigate, openConversation]);

  useEffect(() => () => closeStream(), [closeStream]);

  const title = navigationTitle(state);
  const ready = state.bootstrapStatus === 'ready';
  // Chat is the view a deep link or a restored history entry arrives on; every
  // other view owns the operation it started and reports it in place.
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
          <SessionOperationState state={state} onReturnToWelcome={returnToWelcome} />
        )}
        {ready && !wholeApplication && (
          <Screen
            client={client}
            dispatch={navigate}
            onCreateSession={createConversation}
            onOpenSession={openConversation}
            state={state}
          />
        )}
      </main>
    </div>
  );
}
