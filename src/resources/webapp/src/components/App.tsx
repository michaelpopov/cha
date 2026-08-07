import { useEffect, useReducer, useRef, type Dispatch } from 'react';

import { chaClient, type Bootstrap, type ChaClient } from '../api/client';
import { validateBootstrap } from '../state/bootstrap';
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
}

function Screen({ state, dispatch, client }: ScreenProps) {
  switch (state.mainView) {
    case 'chat': return <ChatScreen state={state} />;
    case 'personas': return <PersonasScreen state={state} dispatch={dispatch} />;
    case 'characters': return <CharactersScreen state={state} dispatch={dispatch} />;
    case 'character-detail': return (
      <CharacterDetailScreen state={state} dispatch={dispatch} client={client} />
    );
    case 'forums': return <ForumsScreen state={state} dispatch={dispatch} />;
    case 'sessions': return <SessionsScreen state={state} dispatch={dispatch} />;
    case 'new-session': return <NewSessionScreen dispatch={dispatch} />;
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

export function App({ client = chaClient }: { client?: ChaClient }) {
  const [state, dispatch] = useReducer(appReducer, initialAppState);
  const request = useRef<{ client: ChaClient; promise: Promise<Bootstrap> } | null>(null);

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

  const title = navigationTitle(state);
  const ready = state.bootstrapStatus === 'ready';

  return (
    <div
      className={`cha-app ${state.sidebarOpen ? 'is-sidebar-open' : ''}`}
      data-sidebar={state.sidebarOpen ? 'open' : 'closed'}
    >
      <Sidebar state={state} dispatch={dispatch} />
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
        {ready ? <Screen state={state} dispatch={dispatch} client={client} /> : (
          <BootstrapState state={state} />
        )}
      </main>
    </div>
  );
}
