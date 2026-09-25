import {
  useCallback,
  useEffect,
  useLayoutEffect,
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
import type { NativeBridge } from '../api/nativeBridge';
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
  JevSettingsScreen,
  VoicesScreen,
  VaultScreen,
  VaultsScreen,
} from './Settings';

interface ScreenProps extends ChatActions {
  playbackPositions: Map<string, Map<number, number>>;
  state: AppState;
  dispatch: Dispatch<AppAction>;
  client: ChaClient;
  onDeleteForum(forumId: string): Promise<void>;
  onCreateSession(forumId: string, label: string): Promise<boolean>;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
  catalogRevision: number;
}

function Screen({
  playbackPositions,
  state,
  dispatch,
  client,
  onDeleteForum,
  onCreateSession,
  onOpenSession,
  onCoverConversation,
  onDeleteTurn,
  onRetryStream,
  onReturnToStart,
  onSetDefaultCharacter,
  onStopGeneration,
  onSubmitInput,
  onUncoverConversation,
  catalogRevision,
}: ScreenProps) {
  switch (state.mainView) {
    case 'chat': return (
      <ChatScreen
        key={`${state.bootstrap?.vault_name}/${state.activeConversation?.forumId}/${state.activeConversation?.sessionId}`}
        playbackPositions={playbackPositions}
        client={client}
        dispatch={dispatch}
        onCoverConversation={onCoverConversation}
        onDeleteTurn={onDeleteTurn}
        onRetryStream={onRetryStream}
        onReturnToStart={onReturnToStart}
        onSetDefaultCharacter={onSetDefaultCharacter}
        onStopGeneration={onStopGeneration}
        onSubmitInput={onSubmitInput}
        onUncoverConversation={onUncoverConversation}
        state={state}
      />
    );
    case 'personas': return (
      <PersonasScreen state={state} dispatch={dispatch} />
    );
    case 'new-persona': return (
      <NewPersonaScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'persona-detail': return (
      <PersonaDetailScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'persona-settings': return (
      <PersonaSettingsScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'characters': return (
      <CharactersScreen state={state} dispatch={dispatch} />
    );
    case 'new-character': return (
      <NewCharacterScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'character-detail': return (
      <CharacterDetailScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'character-file': return (
      <CharacterFileScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'new-character-file': return (
      <NewCharacterFileScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'character-settings': return (
      <CharacterSettingsScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'forums': return (
      <ForumsScreen state={state} dispatch={dispatch} />
    );
    case 'new-forum': return (
      <NewForumScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'sessions': return (
      <SessionsScreen
        catalogRevision={catalogRevision}
        client={client}
        dispatch={dispatch}
        onOpenSession={onOpenSession}
        state={state}
      />
    );
    case 'forum-detail': return (
      <ForumDetailScreen
        onDelete={onDeleteForum}
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'forum-file': return (
      <ForumFileScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'new-forum-file': return (
      <NewForumFileScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'forum-members': return (
      <ForumMembersScreen
        client={client}
        dispatch={dispatch}
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
    case 'settings': return (
      <OpenAiConnectionScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'settings-vaults': return (
      <VaultsScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-new-vault': return (
      <NewVaultScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-download-vault': return (
      <DownloadVaultScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-merge-vault': return (
      <MergeVaultScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-vault': return (
      <VaultScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-providers': return (
      <ProvidersScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-new-provider': return (
      <NewProviderScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-provider': return (
      <ProviderScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'settings-styles': return (
      <StylesScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-new-style': return (
      <NewStyleScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-style': return (
      <StyleScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'settings-voices': return (
      <VoicesScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-jev': return <JevSettingsScreen client={client} dispatch={dispatch} state={state} />;
    case 'settings-voice-input': return (
      <VoiceSettingsScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-new-voice': return (
      <NewVoiceScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-voice': return (
      <VoiceScreen
        client={client}
        dispatch={dispatch}
        state={state}
      />
    );
    case 'settings-api-keys': return (
      <ApiKeysScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-new-api-key': return (
      <NewApiKeyScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-api-key': return (
      <ApiKeyScreen client={client} dispatch={dispatch} state={state} />
    );
    case 'settings-r2-storage': return (
      <R2StorageScreen client={client} dispatch={dispatch} state={state} />
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

// Opening a session replaces chat; navigating elsewhere dismisses this state.
function SessionOperationState({
  state,
  onRetry,
  onBrowseSessions,
}: {
  state: AppState;
  onRetry(): void;
  onBrowseSessions(): void;
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
        <button className="cha-button cha-button-primary" onClick={onBrowseSessions} type="button">
          Browse sessions
        </button>
      </div>
    </div>
  );
}

function defaultReload() {
  reloadApplication();
}

interface AppProps {
  client: ChaClient;
  contextEvents?: Pick<NativeBridge, 'on'>;
  connectSessionEvents?: SessionEventsConnector;
  retryDelays?: readonly number[];
  reload?: () => void;
}

export function App({
  client,
  contextEvents,
  connectSessionEvents,
  retryDelays,
  reload = defaultReload,
}: AppProps) {
  const [state, dispatch] = useReducer(appReducer, initialAppState);
  const [bootstrapAttempt, setBootstrapAttempt] = useState(0);
  const [catalogRevision, setCatalogRevision] = useState(0);
  const request = useRef<{ client: ChaClient; promise: Promise<Bootstrap> } | null>(null);
  const playbackPositions = useRef(new Map<string, Map<number, number>>());
  const pendingMutations = useRef(new Set<string>());
  const [submissionErrors, setSubmissionErrors] = useState<{ id: number; message: string }[]>([]);
  const nextSubmissionError = useRef(0);
  const composerKey = `${state.bootstrap?.vault_name}/${state.activeConversation?.forumId}/${state.activeConversation?.sessionId}/${state.mainView}`;
  const composerVisit = useRef(0);
  useLayoutEffect(() => {
    composerVisit.current += 1;
  }, [composerKey]);

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
    clearVaultContext,
  } = useLiveSession(client, state, dispatch, {
    connectSessionEvents,
    retryDelays,
    refreshBootstrap,
  });

  useEffect(() => {
    if (!contextEvents) return;
    let current = true;
    let generation = 0;
    const unsubscribe = contextEvents.on<{
      causing_request_id?: number;
      state?: string;
    }>('app.contextChanged', (event) => {
      if (!Number.isSafeInteger(event.causing_request_id) || event.state !== 'running') return;
      const started = ++generation;
      clearVaultContext();
      playbackPositions.current.clear();
      navigate({ type: 'vault-context-reset' });
      void (async () => {
        try {
          const bootstrap = validateBootstrap(await client.getBootstrap());
          if (current && started === generation) {
            dispatch({
              type: 'vault-context-refreshed',
              bootstrap,
            });
          }
        } catch (failure: unknown) {
          if (current && started === generation) {
            dispatch({
              type: 'bootstrap-failed',
              incompatible: false,
              message: publicErrorMessage(
                failure, 'CHA could not refresh the active vault. Try again.',
              ),
            });
          }
        }
      })();
    });
    return () => {
      current = false;
      generation += 1;
      unsubscribe();
    };
  }, [clearVaultContext, client, contextEvents, navigate]);

  const returnToStart = useCallback(() => {
    clearLiveSession();
    navigate({ type: 'show-initial-conversation' });
    writeAppRoute('/');
  }, [clearLiveSession, navigate]);

  const browseSessions = useCallback(() => {
    clearLiveSession();
    navigate({ type: 'show-forums' });
  }, [clearLiveSession, navigate]);

  const switchVault = useCallback(async (vaultName: string, password?: string) => {
    await client.switchVault(vaultName, password);
    writeAppRoute('/', 'replace');
    if (!contextEvents) reload();
  }, [client, contextEvents, reload]);

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

  const submitInput = useCallback(async (text: string) => {
    const active = state.activeConversation;
    if (!active) throw new Error('No live conversation is selected.');
    const visit = composerVisit.current;
    const forumName = state.sessionSnapshot?.forum.display_name
      ?? state.bootstrap?.forums.find((forum) => forum.id === active.forumId)?.display_name
      ?? active.forumId;
    const sessionName = state.activeConversationLabel ?? active.sessionId;
    try {
      return await runMutation(active, 'submit', () => client.submitInput(active.forumId, active.sessionId, { text }));
    } catch (failure: unknown) {
      if (composerVisit.current !== visit) {
        const message = publicErrorMessage(failure, 'The message could not be sent.');
        setSubmissionErrors((errors) => [...errors, {
          id: ++nextSubmissionError.current,
          message: `Message to ${forumName} / ${sessionName} was not sent: ${message}`,
        }]);
      }
      throw failure;
    }
  }, [client, runMutation, state.activeConversation, state.activeConversationLabel, state.sessionSnapshot, state.bootstrap]);

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
    await refreshBootstrap();
    const active = state.activeConversation?.forumId === forumId
      && state.activeConversation.sessionId === sessionId;
    if (active) {
      clearLiveSession();
      navigate({ type: 'show-initial-conversation' });
      writeAppRoute('/', 'replace');
    }
    setCatalogRevision((revision) => revision + 1);
  }, [clearLiveSession, client, navigate, refreshBootstrap, runMutation,
    state.activeConversation]);

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
  const chatVisible = ready && !wholeApplication && state.mainView === 'chat';

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
          <main className={`cha-main${chatVisible ? ' is-chat' : ''}`} data-view={state.mainView}>
            {!chatVisible && <TopBar dispatch={navigate} state={state} title={title} />}
            {submissionErrors.map((error) => <div className="cha-submission-error" role="alert" key={error.id}>
              <span>{error.message}</span>
              <button type="button" aria-label="Dismiss message error" onClick={() => setSubmissionErrors((errors) => errors.filter(({ id }) => id !== error.id))}>Dismiss</button>
            </div>)}
            {!ready && <BootstrapState onRetry={retryBootstrap} state={state} />}
            {ready && wholeApplication && (
              <SessionOperationState
                onBrowseSessions={browseSessions}
                onRetry={retrySessionOpen}
                state={state}
              />
            )}
            {ready && !wholeApplication && (
              <Screen
                playbackPositions={playbackPositions.current}
                catalogRevision={catalogRevision}
                client={client}
                dispatch={navigate}
                onCoverConversation={coverConversation}
                onDeleteTurn={deleteTurn}
                onDeleteForum={deleteForum}
                onCreateSession={createConversation}
                onOpenSession={openConversation}
                onRetryStream={retryStream}
                onReturnToStart={returnToStart}
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
