import {
  useCallback,
  useEffect,
  useReducer,
  useRef,
  useState,
  type Dispatch,
} from 'react';

import {
  publicErrorMessage,
  type Bootstrap,
  type ChaClient,
} from '../api/client';
import { validateBootstrap } from '../state/bootstrap';
import { saveMarkdownDownload } from '../download';
import { reloadApplication, writeAppRoute } from '../state/route';
import { useLiveSession, type SessionEventsConnector } from '../useLiveSession';
import {
  appReducer,
  initialAppState,
  navigationTitle,
  type AppAction,
  type AppState,
} from '../state/view';
import { OpenAiConnectionScreen } from './OpenAiConnection';
import { ChatScreen, type ChatActions } from './ChatScreen';
import { AppErrorBoundary } from './AppErrorBoundary';
import { TopBar } from './TopBar';
import {
  CharacterDetailScreen,
  CharacterFileScreen,
  NewCharacterFileScreen,
  CharacterSettingsScreen,
  CharactersScreen,
  ForumDetailScreen,
  ForumFileScreen,
  NewForumFileScreen,
  ForumMembersScreen,
  ForumsScreen,
  NewPersonaScreen,
  NewCharacterScreen,
  NewForumScreen,
  NewSessionScreen,
  PersonaDetailScreen,
  PersonaSettingsScreen,
  PersonasScreen,
  SessionOperationReport,
  SessionsScreen,
} from './Screens';
import { Sidebar } from './Sidebar';
import { TransliterationProvider } from './TransliterationMode';
import {
  ApiKeyScreen,
  ApiKeysScreen,
  DownloadVaultScreen,
  MergeVaultScreen,
  NewApiKeyScreen,
  NewProviderScreen,
  NewStyleScreen,
  NewVoiceScreen,
  NewVaultScreen,
  ProviderScreen,
  ProvidersScreen,
  R2StorageScreen,
  StyleScreen,
  StylesScreen,
  VoiceScreen,
  VoiceSettingsScreen,
  VoicesScreen,
  VaultScreen,
  VaultsScreen,
} from './Settings';

interface ScreenProps extends ChatActions {
  playbackPositions: Map<string, Map<number, number>>;
  state: AppState;
  dispatch: Dispatch<AppAction>;
  client: ChaClient;
  onCreateSession(forumId: string, label: string): Promise<boolean>;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
  onRetrySession(): void;
  catalogRevision: number;
  characterRevision: number;
  forumRevision: number;
  personaRevision: number;
  providerRevision: number;
  styleRevision: number;
  voiceRevision: number;
}

function Screen({
  playbackPositions,
  state,
  dispatch,
  client,
  onCreateSession,
  onOpenSession,
  onRetrySession,
  onCoverConversation,
  onDeleteTurn,
  onRetryStream,
  onReturnToWelcome,
  onSetDefaultCharacter,
  onStopGeneration,
  onSubmitInput,
  onUncoverConversation,
  catalogRevision,
  characterRevision,
  forumRevision,
  personaRevision,
  providerRevision,
  styleRevision,
  voiceRevision,
}: ScreenProps) {
  // A session can be opened from the sidebar while any navigation screen is
  // showing, so each one carries the report rather than only the two screens
  // that start an open themselves.
  const sessionReport = (
    <SessionOperationReport
      onRetrySession={onRetrySession}
      onReturnToWelcome={onReturnToWelcome}
      state={state}
    />
  );

  switch (state.mainView) {
    case 'chat': return (
      <ChatScreen
        playbackPositions={playbackPositions}
        client={client}
        dispatch={dispatch}
        onCoverConversation={onCoverConversation}
        onDeleteTurn={onDeleteTurn}
        onRetryStream={onRetryStream}
        onReturnToWelcome={onReturnToWelcome}
        onSetDefaultCharacter={onSetDefaultCharacter}
        onStopGeneration={onStopGeneration}
        onSubmitInput={onSubmitInput}
        onUncoverConversation={onUncoverConversation}
        state={state}
      />
    );
    case 'personas': return (
      <PersonasScreen state={state} dispatch={dispatch} sessionReport={sessionReport} />
    );
    case 'new-persona': return (
      <NewPersonaScreen
        client={client}
        dispatch={dispatch}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'persona-detail': return (
      <PersonaDetailScreen
        client={client}
        dispatch={dispatch}
        reloadVersion={personaRevision}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'persona-settings': return (
      <PersonaSettingsScreen
        client={client}
        dispatch={dispatch}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'characters': return (
      <CharactersScreen state={state} dispatch={dispatch} sessionReport={sessionReport} />
    );
    case 'new-character': return (
      <NewCharacterScreen
        client={client}
        dispatch={dispatch}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'character-detail': return (
      <CharacterDetailScreen
        client={client}
        dispatch={dispatch}
        reloadVersion={characterRevision}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'character-file': return (
      <CharacterFileScreen
        client={client}
        dispatch={dispatch}
        reloadVersion={characterRevision}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'new-character-file': return (
      <NewCharacterFileScreen
        client={client}
        dispatch={dispatch}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'character-settings': return (
      <CharacterSettingsScreen
        client={client}
        dispatch={dispatch}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'forums': return (
      <ForumsScreen state={state} dispatch={dispatch} sessionReport={sessionReport} />
    );
    case 'new-forum': return (
      <NewForumScreen
        client={client}
        dispatch={dispatch}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'sessions': return (
      <SessionsScreen
        catalogRevision={catalogRevision}
        client={client}
        dispatch={dispatch}
        onOpenSession={onOpenSession}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'forum-detail': return (
      <ForumDetailScreen
        client={client}
        dispatch={dispatch}
        reloadVersion={forumRevision}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'forum-file': return (
      <ForumFileScreen
        client={client}
        dispatch={dispatch}
        reloadVersion={forumRevision}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'new-forum-file': return (
      <NewForumFileScreen
        client={client}
        dispatch={dispatch}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'forum-members': return (
      <ForumMembersScreen
        client={client}
        dispatch={dispatch}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'new-session': return (
      <NewSessionScreen
        dispatch={dispatch}
        onCreateSession={onCreateSession}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'settings': return (
      <OpenAiConnectionScreen
        client={client}
        dispatch={dispatch}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'settings-vaults': return (
      <VaultsScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-new-vault': return (
      <NewVaultScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-download-vault': return (
      <DownloadVaultScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-merge-vault': return (
      <MergeVaultScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-vault': return (
      <VaultScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-providers': return (
      <ProvidersScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-new-provider': return (
      <NewProviderScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-provider': return (
      <ProviderScreen
        client={client}
        dispatch={dispatch}
        reloadVersion={providerRevision}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'settings-styles': return (
      <StylesScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-new-style': return (
      <NewStyleScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-style': return (
      <StyleScreen
        client={client}
        dispatch={dispatch}
        reloadVersion={styleRevision}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'settings-voices': return (
      <VoicesScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-voice-input': return (
      <VoiceSettingsScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-new-voice': return (
      <NewVoiceScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-voice': return (
      <VoiceScreen
        client={client}
        dispatch={dispatch}
        reloadVersion={voiceRevision}
        sessionReport={sessionReport}
        state={state}
      />
    );
    case 'settings-api-keys': return (
      <ApiKeysScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-new-api-key': return (
      <NewApiKeyScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-api-key': return (
      <ApiKeyScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
    case 'settings-r2-storage': return (
      <R2StorageScreen client={client} dispatch={dispatch} sessionReport={sessionReport} state={state} />
    );
  }
}

function BootstrapState({ state, onRetry }: { state: AppState; onRetry(): void }) {
  if (state.bootstrapStatus === 'loading') {
    return <p className="cha-bootstrap-state" role="status">Loading workspace…</p>;
  }
  return (
    <div className="cha-bootstrap-state" role="alert">
      <h2>
        {state.bootstrapStatus === 'incompatible'
          ? 'Incompatible application response'
          : 'Application API unavailable'}
      </h2>
      <p>{state.bootstrapMessage}</p>
      <button className="cha-button cha-button-primary" onClick={onRetry} type="button">
        Retry
      </button>
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
function defaultReload() {
  reloadApplication();
}

interface AppProps {
  client: ChaClient;
  connectSessionEvents?: SessionEventsConnector;
  retryDelays?: readonly number[];
  reload?: () => void;
}

export function App({
  client,
  connectSessionEvents,
  retryDelays,
  reload = defaultReload,
}: AppProps) {
  const [state, dispatch] = useReducer(appReducer, initialAppState);
  const [bootstrapAttempt, setBootstrapAttempt] = useState(0);
  const [catalogRevision, setCatalogRevision] = useState(0);
  const [characterRevision, setCharacterRevision] = useState(0);
  const [forumRevision, setForumRevision] = useState(0);
  const [personaRevision, setPersonaRevision] = useState(0);
  const [providerRevision, setProviderRevision] = useState(0);
  const [styleRevision, setStyleRevision] = useState(0);
  const [voiceRevision, setVoiceRevision] = useState(0);
  const request = useRef<{ client: ChaClient; promise: Promise<Bootstrap> } | null>(null);
  const playbackPositions = useRef(new Map<string, Map<number, number>>());
  const pendingMutations = useRef(new Set<string>());

  useEffect(() => {
    const vaultName = state.bootstrap?.vault_name;
    document.title = vaultName ? `CHA: ${vaultName}` : 'CHA';
  }, [state.bootstrap?.vault_name]);

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
          // The screen stays deliberately generic: validation errors describe
          // the response shape, not a recovery action. Keep that detail in the
          // local developer console so a mismatched package remains diagnosable.
          console.error('CHA bootstrap validation failed.', failure);
          dispatch({
            type: 'bootstrap-failed',
            incompatible: true,
            message: 'CHA returned an incompatible response. Restart CHA with matching browser files.',
          });
        }
      },
      (failure: unknown) => {
        if (!current) return;
        dispatch({
          type: 'bootstrap-failed',
          incompatible: false,
          message: publicErrorMessage(
            failure,
            'CHA’s application API is unavailable. Check that CHA is running and try again.',
          ),
        });
      },
    );
    return () => {
      current = false;
    };
  }, [bootstrapAttempt, client]);

  const retryBootstrap = useCallback(() => {
    request.current = null;
    dispatch({ type: 'bootstrap-started' });
    setBootstrapAttempt((attempt) => attempt + 1);
  }, []);

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

  const {
    navigate,
    openConversation,
    createConversation,
    retrySessionOpen,
    retryStream,
    clearLiveSession,
  } = useLiveSession(client, state, dispatch, {
    connectSessionEvents,
    retryDelays,
    refreshBootstrap,
  });

  const returnToWelcome = useCallback(() => {
    clearLiveSession();
    navigate({ type: 'show-initial-conversation' });
    writeAppRoute('/');
  }, [clearLiveSession, navigate]);

  const switchVault = useCallback(async (vaultName: string, password?: string) => {
    await client.switchVault(vaultName, password);
    writeAppRoute('/', 'replace');
    reload();
  }, [client, reload]);

  // Keyed by conversation as well as action: the server gives a mutation up to
  // its command deadline, and a request left behind in one conversation must
  // not refuse the same action in the one the reader has moved to.
  const runMutation = useCallback(async <T,>(
    { forumId, sessionId }: { forumId: string; sessionId: string },
    action: string,
    operation: () => Promise<T>,
  ) => {
    const key = `${forumId}/${sessionId}/${action}`;
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
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation(active, 'submit', () => client.submitInput(
      active.forumId,
      active.sessionId,
      { text },
    ));
  }, [client, runMutation, state.activeConversation]);

  const coverConversation = useCallback((throughEntryId: number) => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation(active, 'cover', () => client.coverConversation(
      active.forumId,
      active.sessionId,
      { through_entry_id: throughEntryId },
    ));
  }, [client, runMutation, state.activeConversation]);

  const uncoverConversation = useCallback(() => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation(active, 'cover', () => client.uncoverConversation(
      active.forumId,
      active.sessionId,
    ));
  }, [client, runMutation, state.activeConversation]);

  const deleteTurn = useCallback((responseEntryId: number) => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation(active, 'delete-turn', () => client.deleteTurn(
      active.forumId,
      active.sessionId,
      { response_entry_id: responseEntryId },
    ));
  }, [client, runMutation, state.activeConversation]);

  const stopGeneration = useCallback(() => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation(active, 'stop', () => client.stopGeneration(
      active.forumId,
      active.sessionId,
    ));
  }, [client, runMutation, state.activeConversation]);

  const setDefaultCharacter = useCallback((characterId: string) => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation(active, 'target', () => client.setDefaultCharacter(
      active.forumId,
      active.sessionId,
      characterId,
    ));
  }, [client, runMutation, state.activeConversation]);

  const renameSession = useCallback(async (
    forumId: string,
    sessionId: string,
    label: string,
  ) => {
    await runMutation({ forumId, sessionId }, 'catalog', () => (
      client.renameSession(forumId, sessionId, label)
    ));
    await refreshBootstrap();
    setCatalogRevision((revision) => revision + 1);
  }, [client, refreshBootstrap, runMutation]);

  const downloadSession = useCallback(async (
    forumId: string,
    sessionId: string,
    label: string,
  ) => {
    await saveMarkdownDownload(
      label,
      forumId,
      sessionId,
      () => client.downloadSession(forumId, sessionId),
    );
  }, [client]);

  const clearSessionAudioCache = useCallback(async (forumId: string, sessionId: string) => {
    await client.clearSessionAudioCache(forumId, sessionId);
    playbackPositions.current.delete(JSON.stringify([state.bootstrap?.vault_name, forumId, sessionId]));
    dispatch({ type: 'session-audio-cache', forumId, sessionId, cached: false });
  }, [client, state.bootstrap?.vault_name]);

  const deleteSession = useCallback(async (forumId: string, sessionId: string) => {
    await runMutation({ forumId, sessionId }, 'catalog', () => (
      client.deleteSession(forumId, sessionId)
    ));
    const active = state.activeConversation?.forumId === forumId
      && state.activeConversation.sessionId === sessionId;
    if (active) {
      clearLiveSession();
      navigate({ type: 'show-initial-conversation' });
      writeAppRoute('/', 'replace');
    }
    await refreshBootstrap();
    setCatalogRevision((revision) => revision + 1);
  }, [clearLiveSession, client, navigate, refreshBootstrap, runMutation,
    state.activeConversation]);

  const deletePersona = useCallback(async (personaId: string) => {
    await client.deletePersona(personaId);
    navigate({ type: 'persona-deleted', personaId });
  }, [client, navigate]);

  const deleteCharacter = useCallback(async (characterId: string) => {
    await client.deleteCharacter(characterId);
    navigate({ type: 'character-deleted', characterId });
  }, [client, navigate]);

  const deleteForum = useCallback(async (forumId: string) => {
    await client.deleteForum(forumId);
    if (state.activeConversation?.forumId === forumId) {
      clearLiveSession();
      writeAppRoute('/', 'replace');
    }
    navigate({ type: 'forum-deleted', forumId });
    setCatalogRevision((revision) => revision + 1);
  }, [clearLiveSession, client, navigate, state.activeConversation]);

  const title = navigationTitle(state);
  const ready = state.bootstrapStatus === 'ready';
  const wholeApplication = state.sessionOperation !== 'idle' && state.mainView === 'chat';

  return (
    <AppErrorBoundary onReload={reload}>
      <TransliterationProvider>
        <div
          className={`cha-app ${state.sidebarOpen ? 'is-sidebar-open' : ''}`}
          data-sidebar={state.sidebarOpen ? 'open' : 'closed'}
        >
          <Sidebar
            onClearSessionAudioCache={clearSessionAudioCache}
            dispatch={navigate}
            onDeleteSession={deleteSession}
            onDownloadSession={downloadSession}
            onOpenSession={openConversation}
            onRenameSession={renameSession}
            onSwitchVault={switchVault}
            state={state}
          />
          <main className="cha-main" data-view={state.mainView}>
            <TopBar
              client={client}
              dispatch={navigate}
              onDeleteCharacter={deleteCharacter}
              onDeleteForum={deleteForum}
              onDeletePersona={deletePersona}
              onCharacterDefinitionUpdated={() => (
                setCharacterRevision((revision) => revision + 1)
              )}
              onForumDefinitionUpdated={() => setForumRevision((revision) => revision + 1)}
              onPersonaDefinitionUpdated={() => setPersonaRevision((revision) => revision + 1)}
              onProviderUpdated={() => setProviderRevision((revision) => revision + 1)}
              onStyleUpdated={() => setStyleRevision((revision) => revision + 1)}
              onVoiceUpdated={() => setVoiceRevision((revision) => revision + 1)}
              state={state}
              title={title}
            />
            {!ready && <BootstrapState onRetry={retryBootstrap} state={state} />}
            {ready && wholeApplication && (
              <SessionOperationState
                onRetry={retrySessionOpen}
                onReturnToWelcome={returnToWelcome}
                state={state}
              />
            )}
            {ready && !wholeApplication && (
              <Screen
                playbackPositions={playbackPositions.current}
                catalogRevision={catalogRevision}
                characterRevision={characterRevision}
                forumRevision={forumRevision}
                personaRevision={personaRevision}
                providerRevision={providerRevision}
                styleRevision={styleRevision}
                voiceRevision={voiceRevision}
                client={client}
                dispatch={navigate}
                onCoverConversation={coverConversation}
                onDeleteTurn={deleteTurn}
                onCreateSession={createConversation}
                onOpenSession={openConversation}
                onRetryStream={retryStream}
                onRetrySession={retrySessionOpen}
                onReturnToWelcome={returnToWelcome}
                onSetDefaultCharacter={setDefaultCharacter}
                onStopGeneration={stopGeneration}
                onSubmitInput={submitInput}
                onUncoverConversation={uncoverConversation}
                state={state}
              />
            )}
          </main>
        </div>
      </TransliterationProvider>
    </AppErrorBoundary>
  );
}
