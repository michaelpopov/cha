import { generateText, type Editor } from '@tiptap/core';
import {
  useCallback,
  useEffect,
  useLayoutEffect,
  useReducer,
  useRef,
  useState,
  type CSSProperties,
  type Dispatch,
  type PointerEvent,
} from 'react';

import {
  ChaError,
  publicErrorMessage,
  type Bootstrap,
  type ChaClient,
  type CharacterSettingsPatch,
  type NoteItem,
  type NotesClient,
  type TextFileRequest,
} from '../api/client';
import type { NativeBridge } from '../api/nativeBridge';
import { installAppCloseHost } from '../api/nativeClose';
import { installNativeLayoutHandler, reportNativeLayout } from '../api/nativeLayout';
import { DetailRefreshProvider } from '../detailRefresh';
import { validateBootstrap } from '../state/bootstrap';
import {
  adoptChatDraft,
  chatDraftKey,
  editChatDraft,
  editNotesDraft,
  emptyPromptDrafts,
  visibleChatDraft,
} from '../state/promptDrafts';
import { saveMarkdownDownload } from '../download';
import { noteInSubtree } from '../notes/tree';
import { notesExtensions } from '../notes/editor';
import { useNoteLeave } from '../notes/useNoteLeave';
import type { NoteSaveHandle } from '../notes/useNoteSave';
import { reloadApplication, writeAppRoute } from '../state/route';
import { useLiveSession, type SessionEventsConnector } from '../useLiveSession';
import { useSessionActivity } from '../useSessionActivity';
import {
  appReducer,
  initialAppState,
  navigationTitle,
  workspaceVisibility,
  type DataAction,
  type NavigationAction,
  type AppState,
} from '../state/view';
import { OpenAiConnectionScreen } from './OpenAiConnection';
import { ChatScreen, type ChatActions } from './ChatScreen';
import { AppErrorBoundary } from './AppErrorBoundary';
import { TopBar } from './TopBar';
import {
  CharacterDetailScreen,
  CharacterInstructionsScreen,
  CharacterSettingsScreen,
  CharactersScreen,
  NewCharacterScreen,
} from './CharacterScreens';
import {
  CharacterFileScreen,
  CharacterFilesScreen,
  NewCharacterFileScreen,
  ForumFileScreen,
  NewForumFileScreen,
} from './FileScreens';
import {
  ForumDetailScreen,
  ForumMembersScreen,
  ForumsScreen,
  NewForumScreen,
} from './ForumScreens';
import {
  NewPersonaScreen,
  PersonaDetailScreen,
  PersonaSettingsScreen,
  PersonasScreen,
} from './PersonaScreens';
import { SessionsScreen } from './SessionsScreen';
import { NotesScreen } from './NotesScreen';
import { NotesTree, type CompressNote, type DeleteNoteItem, type NoteActions, type SetNoteReadOnly } from './NotesTree';
import { Sidebar } from './Sidebar';
import { TransliterationProvider } from './TransliterationMode';
import { ApiKeyScreen, ApiKeysScreen, NewApiKeyScreen } from './ApiKeySettings';
import {
  DownloadVaultScreen,
  MergeVaultScreen,
  NewVaultScreen,
  VaultScreen,
  VaultsScreen,
} from './VaultSettings';
import { NewProviderScreen, ProviderScreen, ProvidersScreen } from './ProviderSettings';
import { NewStyleScreen, StyleScreen, StylesScreen } from './StyleSettings';
import { NewVoiceScreen, VoiceScreen, VoiceSettingsScreen, VoicesScreen } from './VoiceSettings';
import { R2StorageScreen } from './R2StorageSettings';
import { SessionSettingsScreen } from './SessionSettings';
import { WebSearchSettingsScreen } from './WebSearchSettings';
import { NotesSettingsScreen } from './NotesSettings';
import { EventsScreen } from './EventsScreen';

interface ScreenProps extends ChatActions {
  playbackPositions: Map<string, Map<number, number>>;
  state: AppState;
  dispatch: Dispatch<DataAction>;
  navigate: Dispatch<NavigationAction>;
  client: ChaClient & NotesClient;
  draft: string;
  notesDraft: string;
  onNotesDraftChange(next: string): void;
  noteItems: readonly NoteItem[];
  noteActions: NoteActions | null;
  onSetNoteReadOnly: SetNoteReadOnly;
  editorLiveRef: { current: Editor | null };
  leave: {
    decisionOpen: boolean;
    retry(): void;
    stay(): void;
    discard(): void;
  };
  onRegisterSave(save: NoteSaveHandle | null): void;
  onRegisterDispose(dispose: () => void): void;
  onDraftChange(next: string): void;
  onDeleteForum(forumId: string): Promise<void>;
  onCreateSession(forumId: string): Promise<boolean>;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
  onRetrySessionOpen(): void;
  onBrowseSessions(): void;
  catalogRevision: number;
}

function Screen(props: ScreenProps) {
  const { state } = props;
  const { notesVisible, chatVisible } = workspaceVisibility(state);
  const [ratio, setRatio] = useState(0.5);
  const workspaceRef = useRef<HTMLDivElement>(null);
  const split = notesVisible && chatVisible;
  if (!notesVisible && !chatVisible) return <ScreenContent {...props} view={state.mainView} />;
  function resize(clientX: number) {
    const bounds = workspaceRef.current?.getBoundingClientRect();
    if (!bounds || bounds.width <= 6) return;
    const available = bounds.width - 6;
    const minimum = Math.min(240, available / 3);
    setRatio(Math.max(minimum / available, Math.min(1 - minimum / available,
      (clientX - bounds.left - 3) / available)));
  }
  return (
    <div className="cha-workspace" ref={workspaceRef}
      style={split ? { gridTemplateColumns: `minmax(0, ${ratio}fr) 6px minmax(0, ${1 - ratio}fr)` } : undefined}>
      {notesVisible && <div key="notes" className="cha-workspace-panel is-notes">
        <ScreenContent {...props} view="notes" />
      </div>}
      {split && <div key="divider" className="cha-workspace-divider" role="separator"
        aria-label="Resize notes and chat" aria-orientation="vertical" tabIndex={0}
        aria-valuemin={0} aria-valuemax={100} aria-valuenow={Math.round(ratio * 100)}
        onPointerDown={(event) => {
          if (event.button !== 0) return;
          event.preventDefault();
          event.currentTarget.setPointerCapture(event.pointerId);
        }}
        onPointerMove={(event) => {
          if (event.currentTarget.hasPointerCapture(event.pointerId)) resize(event.clientX);
        }}
        onPointerUp={(event) => {
          if (event.currentTarget.hasPointerCapture(event.pointerId)) event.currentTarget.releasePointerCapture(event.pointerId);
        }}
        onKeyDown={(event) => {
          if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
          event.preventDefault();
          const bounds = workspaceRef.current?.getBoundingClientRect();
          if (bounds) resize(bounds.left + (bounds.width - 6) * ratio + 3 + (event.key === 'ArrowLeft' ? -24 : 24));
        }} />}
      {chatVisible && <div key="chat" className="cha-workspace-panel is-chat">
        {state.sessionOperation !== 'idle' ? <>
          {state.layoutMode === 'single' && <TopBar title={navigationTitle(state)} />}
          <SessionOperationState state={state} onRetry={props.onRetrySessionOpen}
            onBrowseSessions={props.onBrowseSessions} />
        </> : <ScreenContent {...props} view="chat" />}
      </div>}
    </div>
  );
}

function ScreenContent({
  playbackPositions,
  state,
  dispatch,
  navigate,
  client,
  onDeleteForum,
  onCreateSession,
  onOpenSession,
  onRetrySessionOpen,
  onCoverConversation,
  onDeleteTurn,
  draft,
  onDraftChange,
  notesDraft,
  onNotesDraftChange,
  noteItems,
  noteActions,
  onSetNoteReadOnly,
  editorLiveRef,
  leave,
  onRegisterSave,
  onRegisterDispose,
  onRetryStream,
  onReturnToStart,
  onSetDefaultCharacter,
  onSetCharacterSettings,
  onStopGeneration,
  onSubmitInput,
  onAttachTextFile,
  onUncoverConversation,
  catalogRevision,
  view,
}: ScreenProps & { view: AppState['mainView'] }) {
  switch (view) {
    case 'chat': return (
      <ChatScreen
        key={`${state.bootstrap?.vault_name}/${state.activeConversation?.forumId}/${state.activeConversation?.sessionId}`}
        playbackPositions={playbackPositions}
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        onCoverConversation={onCoverConversation}
        onDeleteTurn={onDeleteTurn}
        draft={draft}
        onDraftChange={onDraftChange}
        onRetryStream={onRetryStream}
        onReturnToStart={onReturnToStart}
        onSetDefaultCharacter={onSetDefaultCharacter}
        onSetCharacterSettings={onSetCharacterSettings}
        onStopGeneration={onStopGeneration}
        onSubmitInput={onSubmitInput}
        onAttachTextFile={onAttachTextFile}
        onUncoverConversation={onUncoverConversation}
        state={state}
      />
    );
    case 'personas': return (
      <PersonasScreen state={state} dispatch={dispatch} navigate={navigate} />
    );
    case 'new-persona': return (
      <NewPersonaScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'persona-detail': return (
      <PersonaDetailScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'persona-settings': return (
      <PersonaSettingsScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'characters': return (
      <CharactersScreen state={state} dispatch={dispatch} navigate={navigate} />
    );
    case 'character-instructions': return (
      <CharacterInstructionsScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'new-character': return (
      <NewCharacterScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'character-detail': return (
      <CharacterDetailScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'character-file': return (
      <CharacterFileScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'character-files': return (
      <CharacterFilesScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'new-character-file': return (
      <NewCharacterFileScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'character-settings': return (
      <CharacterSettingsScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'forums': return (
      <ForumsScreen state={state} dispatch={dispatch} navigate={navigate} />
    );
    case 'new-forum': return (
      <NewForumScreen
        onForumsChanged={() => noteActions?.refresh()}
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'sessions': return (
      <SessionsScreen
        catalogRevision={catalogRevision}
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        onCreateSession={onCreateSession}
        onOpenSession={onOpenSession}
        state={state}
      />
    );
    case 'forum-detail': return (
      <ForumDetailScreen
        onForumsChanged={() => noteActions?.refresh()}
        onDelete={onDeleteForum}
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'forum-file': return (
      <ForumFileScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'new-forum-file': return (
      <NewForumFileScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'forum-members': return (
      <ForumMembersScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'settings': return (
      <OpenAiConnectionScreen
        client={client}
        navigate={navigate}
        state={state}
      />
    );
    case 'settings-vaults': return (
      <VaultsScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-new-vault': return (
      <NewVaultScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-download-vault': return (
      <DownloadVaultScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-merge-vault': return (
      <MergeVaultScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-vault': return (
      <VaultScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-providers': return (
      <ProvidersScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-new-provider': return (
      <NewProviderScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-provider': return (
      <ProviderScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'settings-styles': return (
      <StylesScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-new-style': return (
      <NewStyleScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-style': return (
      <StyleScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'settings-voices': return (
      <VoicesScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-jev': return <SessionSettingsScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />;
    case 'settings-notes': return <NotesSettingsScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />;
    case 'settings-events': return <EventsScreen key={state.notesEpoch} client={client} navigate={navigate} />;
    case 'settings-web-search': return <WebSearchSettingsScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />;
    case 'settings-voice-input': return (
      <VoiceSettingsScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-new-voice': return (
      <NewVoiceScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-voice': return (
      <VoiceScreen
        client={client}
        dispatch={dispatch}
        navigate={navigate}
        state={state}
      />
    );
    case 'settings-api-keys': return (
      <ApiKeysScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-new-api-key': return (
      <NewApiKeyScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-api-key': return (
      <ApiKeyScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-r2-storage': return (
      <R2StorageScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'notes': return (
      <NotesScreen
        toolbarVisible={state.notesToolbarVisible}
        promptVisible={state.notesPromptVisible}
        activeNoteId={state.activeNoteId}
        client={client}
        navigate={navigate}
        draft={notesDraft}
        editorLiveRef={editorLiveRef}
        leave={leave}
        noteItems={noteItems}
        noteActions={noteActions}
        onSetNoteReadOnly={onSetNoteReadOnly}
        noteRequest={state.noteRequest}
        destination={state.noteDestination}
        notesEpoch={state.notesEpoch}
        onDraftChange={onNotesDraftChange}
        onRegisterDispose={onRegisterDispose}
        onRegisterSave={onRegisterSave}
        sessionStatus={!workspaceVisibility(state).chatVisible && state.sessionOperation !== 'idle' && (
          <div className="cha-notes-status" role={state.sessionOperation === 'failed' ? 'alert' : 'status'}>
            <p>{state.sessionOperationMessage}</p>
            {state.sessionOperation === 'failed' && state.sessionOperationRetryable && (
              <button className="cha-button" onClick={onRetrySessionOpen} type="button">Retry</button>
            )}
          </div>
        )}
      />
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

const minimumSidebarWidth = 192;

function maximumSidebarWidth() {
  return Math.max(minimumSidebarWidth, window.innerWidth - 320);
}

function clampSidebarWidth(width: number) {
  return Math.min(maximumSidebarWidth(), Math.max(minimumSidebarWidth, width));
}

interface AppProps {
  client: ChaClient & NotesClient;
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
  useSessionActivity(client, state, dispatch);
  const [bootstrapAttempt, setBootstrapAttempt] = useState(0);
  const [catalogRevision, setCatalogRevision] = useState(0);
  const [detailEpoch, setDetailEpoch] = useState(0);
  const [detailRefreshing, setDetailRefreshing] = useState(false);
  const [detailFailed, setDetailFailed] = useState(false);
  const [sidebarWidth, setSidebarWidth] = useState(() => clampSidebarWidth(264));
  const [resizingSidebar, setResizingSidebar] = useState(false);
  const sidebarResize = useRef<{ pointerId: number; startX: number; startWidth: number } | null>(null);

  useEffect(() => {
    const resize = () => setSidebarWidth((width) => clampSidebarWidth(width));
    window.addEventListener('resize', resize);
    return () => window.removeEventListener('resize', resize);
  }, []);

  function finishSidebarResize(event: PointerEvent<HTMLDivElement>) {
    if (sidebarResize.current?.pointerId !== event.pointerId) return;
    sidebarResize.current = null;
    setResizingSidebar(false);
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
  }
  const request = useRef<{ client: ChaClient; promise: Promise<Bootstrap> } | null>(null);
  const playbackPositions = useRef(new Map<string, Map<number, number>>());
  const pendingMutations = useRef(new Set<string>());
  const [drafts, setDrafts] = useState(emptyPromptDrafts);
  const [submissionErrors, setSubmissionErrors] = useState<{ id: number; message: string }[]>([]);
  const nextSubmissionError = useRef(0);
  const { chatVisible: chatSlotVisible } = workspaceVisibility(state);
  const composerPresent = chatSlotVisible && state.bootstrapStatus === 'ready' && state.sessionOperation === 'idle';
  const composerKey = `${state.bootstrap?.vault_name}/${state.activeConversation?.forumId}/${state.activeConversation?.sessionId}/${composerPresent}`;
  const composerVisit = useRef(0);
  useLayoutEffect(() => {
    composerVisit.current += 1;
  }, [composerKey]);

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

  const bootstrapRefresh = useRef(0);
  const appliedBootstrapRefresh = useRef(0);
  const vaultContext = useRef(0);
  const refreshBootstrap = useCallback(async () => {
    const refresh = ++bootstrapRefresh.current;
    const context = vaultContext.current;
    try {
      const bootstrap = validateBootstrap(await client.getBootstrap());
      if (context !== vaultContext.current) return true;
      if (refresh < appliedBootstrapRefresh.current) return true;
      appliedBootstrapRefresh.current = refresh;
      dispatch({
        type: 'bootstrap-refreshed',
        bootstrap,
      });
      setDetailFailed(false);
      return true;
    } catch {
      // The live snapshot remains usable. Discovery refresh is non-critical.
      return false;
    }
  }, [client]);

  const welcomeRefresh = useRef(0);
  const refreshAfterWelcome = useCallback(async () => {
    const refresh = ++welcomeRefresh.current;
    setDetailRefreshing(true);
    setDetailFailed(false);
    setDetailEpoch((epoch) => epoch + 1);
    setCatalogRevision((revision) => revision + 1);
    const ok = await refreshBootstrap();
    // Only the newest refresh ends the refreshing state.
    if (refresh !== welcomeRefresh.current) return;
    setDetailRefreshing(false);
    if (!ok) setDetailFailed(true);
  }, [refreshBootstrap]);

  const sessionDiscarded = useCallback(() => setCatalogRevision((revision) => revision + 1), []);
  const noteSaveRef = useRef<NoteSaveHandle | null>(null);
  const editorLiveRef = useRef<Editor | null>(null);
  const disposeEditorRef = useRef<() => void>(() => undefined);
  const notesEpochRef = useRef(state.notesEpoch);
  const activeNoteIdRef = useRef(state.activeNoteId);
  notesEpochRef.current = state.notesEpoch;
  activeNoteIdRef.current = state.activeNoteId;
  const noteLeave = useNoteLeave(() => noteSaveRef.current, {
    disposeEditor: () => {
      disposeEditorRef.current();
      dispatch({ type: 'close-note' });
    },
  });
  const closeHost = useRef(noteLeave);
  closeHost.current = noteLeave;
  const closeInstalled = useRef(false);
  if (!closeInstalled.current) {
    closeInstalled.current = true;
    installAppCloseHost(() => closeHost.current);
  }
  useEffect(() => {
    reportNativeLayout(state.layoutMode === 'split', state.sidebarOpen, state.pictureOpen,
      state.notesToolbarVisible, state.notesPromptVisible, state.chatToolbarVisible);
  }, [state.layoutMode, state.sidebarOpen, state.pictureOpen, state.notesToolbarVisible, state.notesPromptVisible, state.chatToolbarVisible]);
  useEffect(() => installNativeLayoutHandler((requested) => {
    if (requested === 'toggle-notes-prompt') {
      dispatch({ type: 'toggle-notes-prompt' });
      return;
    }
    if (requested === 'toggle-chat-toolbar') {
      dispatch({ type: 'toggle-chat-toolbar' });
      return;
    }
    if (requested === 'toggle-toolbar') {
      dispatch({ type: 'toggle-notes-toolbar' });
      return;
    }
    if (requested === 'toggle-pictures') {
      dispatch({ type: 'toggle-picture' });
      return;
    }
    if (requested === 'toggle-sidebar') {
      dispatch({ type: 'toggle-sidebar' });
      return;
    }
    const mode = requested === 'toggle' ? (state.layoutMode === 'split' ? 'single' : 'split') : requested;
    if (mode !== 'single' && mode !== 'split') return;
    // A chat open started in Split View did not leave the note. If it
    // completed in Single View, it would replace the note without saving it.
    if (state.bootstrapStatus !== 'ready' || state.sessionOperation === 'pending'
      || closeHost.current.isPending() || mode === state.layoutMode) return;
    const commit = () => { dispatch({ type: 'set-layout', mode }); };
    if (workspaceVisibility(state).notesVisible && !workspaceVisibility({ ...state, layoutMode: mode }).notesVisible) {
      const context = vaultContext.current;
      void noteLeave.leaveNoteThen(() => {
        if (vaultContext.current !== context) return false;
        commit();
      });
    } else commit();
  }), [state.bootstrapStatus, state.layoutMode, state.mainView, state.sessionOperation, noteLeave.leaveNoteThen]);
  const registerSave = useCallback((save: NoteSaveHandle | null) => {
    noteSaveRef.current = save;
  }, []);
  const registerDispose = useCallback((dispose: () => void) => {
    disposeEditorRef.current = dispose;
  }, []);
  const [noteItems, setNoteItems] = useState<NoteItem[]>([]);
  const [noteActions, setNoteActions] = useState<NoteActions | null>(null);

  const activeChatKey = state.activeConversation
    ? chatDraftKey(
      state.bootstrap?.vault_name,
      state.activeConversation.forumId,
      state.activeConversation.sessionId,
    )
    : '';
  const chatDraftText = visibleChatDraft(drafts, activeChatKey);
  const editChat = useCallback((text: string) => {
    setDrafts((current) => editChatDraft(current, activeChatKey, text));
  }, [activeChatKey]);
  const editNotes = useCallback((text: string) => {
    setDrafts((current) => editNotesDraft(current, text));
  }, []);

  const {
    navigate,
    openConversation,
    createConversation,
    retrySessionOpen,
    retryStream,
    clearLiveSession,
    noteDraftAdoption,
    clearVaultContext,
    retainSession,
    abandonUnusedSession,
    noteWelcomeSubmission,
  } = useLiveSession(client, state, dispatch, {
    connectSessionEvents,
    retryDelays,
    refreshBootstrap,
    onSessionDiscarded: sessionDiscarded,
    onWelcomeRefresh: () => { void refreshAfterWelcome(); },
    onConversationOpened: (forumId, sessionId) => {
      setDrafts((current) => adoptChatDraft(
        current,
        chatDraftKey(state.bootstrap?.vault_name, forumId, sessionId),
      ));
    },
    leaveNoteThen: noteLeave.leaveNoteThen,
  });

  useEffect(() => {
    setNoteItems([]);
  }, [state.notesEpoch]);

  const requestOpenNote = useCallback((id: number) => {
    const live = editorLiveRef.current;
    if (
      workspaceVisibility(state).notesVisible
      && state.activeNoteId === id
      && live
      && !live.isDestroyed
      && !noteSaveRef.current?.frozen
    ) {
      live.view.focus();
      return;
    }
    navigate({ type: 'open-note', noteId: id });
  }, [navigate, state.activeNoteId, state.mainView, state.layoutMode]);

  const sourceChanged = useCallback((parentId: number | null) => {
    const active = noteItems.find((item) => item.id === activeNoteIdRef.current);
    if (active?.kind === 'annotation' && active.parent_id === parentId) {
      navigate({ type: 'open-note', noteId: active.id });
    }
  }, [navigate, noteItems]);

  const beforeCreateNote = useCallback(async () => {
    const result = await noteLeave.leaveNoteThen(() => true);
    if (result === false) return false;
    // The leave only saved the open note. It stays editable until a new note opens.
    noteSaveRef.current?.unfreeze();
    return true;
  }, [noteLeave.leaveNoteThen]);

  const getNoteLength = useCallback(async (id: number) => {
    const openText = () => {
      const editor = editorLiveRef.current;
      return activeNoteIdRef.current === id && editor && !editor.isDestroyed
        ? editor.getText({ blockSeparator: '\n' }) : null;
    };
    let text = openText();
    if (text === null) {
      const note = await client.getNote(id);
      text = openText() ?? generateText(note.document, notesExtensions(), { blockSeparator: '\n' });
    }
    return [...text].length;
  }, [client]);

  const setNoteReadOnly = useCallback<SetNoteReadOnly>(async (sourceId, readOnly) => {
    if (!noteLeave.beginNoteAction()) {
      throw new ChaError('invalid_argument', 'A notes action is already in progress.');
    }
    const epoch = notesEpochRef.current;
    const save = sourceId === activeNoteIdRef.current ? noteSaveRef.current : null;
    let updated: NoteItem | undefined;
    try {
      if (save?.isOpen) {
        save.freeze();
        await save.waitInFlight();
        if (!await save.flushRemaining()) {
          throw new ChaError('internal_error', save.saveError?.message ?? 'The note could not be saved.');
        }
      }
      updated = await client.setNoteReadOnly(sourceId, readOnly);
      return updated;
    } finally {
      if (save?.isOpen && notesEpochRef.current === epoch && activeNoteIdRef.current === sourceId) {
        save.unfreeze(updated ? !updated.read_only : undefined);
      }
      noteLeave.endNoteAction();
    }
  }, [client, noteLeave.beginNoteAction, noteLeave.endNoteAction]);

  const compressNote = useCallback<CompressNote>(async (sourceId, epoch, skipIneligible) => {
    const checkContext = () => {
      if (client.contextEpoch() !== epoch) throw new ChaError('vault_changed', 'The vault changed. Start again in the current vault.');
    };
    checkContext();
    if (!noteLeave.beginNoteAction()) throw new ChaError('invalid_argument', 'A notes action is already in progress.');
    const editorEpoch = notesEpochRef.current;
    const noteId = activeNoteIdRef.current;
    const save = sourceId === noteId ? noteSaveRef.current : null;
    let canceledUncovered = false;
    try {
      if (save?.isOpen) {
        canceledUncovered = save.freeze();
        const admitted = save.inFlightSerial();
        if (admitted !== null || save.hasUnsaved()) {
          await save.waitInFlight();
          checkContext();
          if (save.saveError?.serial === admitted) throw new ChaError('internal_error', save.saveError.message);
          const saved = await save.flushRemaining();
          checkContext();
          if (!saved) throw new ChaError('internal_error', save.saveError?.message ?? 'The note could not be saved.');
        }
      }
      checkContext();
      const result = await client.compressNote(sourceId, epoch, skipIneligible);
      checkContext();
      return result;
    } finally {
      if (save?.isOpen && notesEpochRef.current === editorEpoch && activeNoteIdRef.current === noteId) {
        save.unfreeze();
        if (canceledUncovered && save.hasUnsaved() && !save.saveError) save.submitNow();
      }
      noteLeave.endNoteAction();
    }
  }, [client, noteLeave.beginNoteAction, noteLeave.endNoteAction]);

  const deleteNoteItem = useCallback<DeleteNoteItem>(async (sourceId, listing, refresh, remove) => {
    if (!noteLeave.beginNoteAction()) return 'busy';
    const epoch = notesEpochRef.current;
    const noteId = activeNoteIdRef.current;
    const affected = noteInSubtree(listing, sourceId, noteId);
    const save = noteSaveRef.current;
    let canceledUncovered = false;
    let deleted = false;
    const sameNote = () => (
      notesEpochRef.current === epoch && activeNoteIdRef.current === noteId
    );
    try {
      if (affected && save?.isOpen && sameNote()) {
        canceledUncovered = save.freeze();
        await save.waitInFlight();
      }
      if (notesEpochRef.current !== epoch) return 'failed';
      if (remove) {
        if (!await remove()) return 'failed';
      } else await client.deleteNoteItem(sourceId);
      deleted = true;
      if (affected && sameNote()) {
        noteSaveRef.current?.dispose();
        disposeEditorRef.current();
        dispatch({ type: 'close-note' });
      }
      if (notesEpochRef.current !== epoch) return 'deleted';
      try {
        await refresh();
        return 'deleted';
      } catch {
        return 'refresh-failed';
      }
    } finally {
      if (!deleted && affected && sameNote()) {
        const current = noteSaveRef.current;
        current?.unfreeze();
        if (canceledUncovered) current?.submitNow();
      }
      noteLeave.endNoteAction();
    }
  }, [client, dispatch, noteLeave.beginNoteAction, noteLeave.endNoteAction]);

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
      vaultContext.current += 1;
      welcomeRefresh.current += 1;
      clearVaultContext();
      setDetailRefreshing(false);
      setDetailFailed(false);
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
    const action = () => {
      const bootstrap = state.bootstrap;
      if (bootstrap) noteDraftAdoption(bootstrap.initial_forum_id, bootstrap.initial_session_id);
      clearLiveSession();
      navigate({ type: 'show-initial-conversation' });
      writeAppRoute('/');
    };
    const { notesVisible, chatVisible } = workspaceVisibility(state);
    if (notesVisible && chatVisible) action();
    else void noteLeave.leaveNoteThen(action);
  }, [clearLiveSession, navigate, noteDraftAdoption, noteLeave.leaveNoteThen, state.bootstrap, state.layoutMode, state.mainView]);

  const browseSessions = useCallback(() => {
    void noteLeave.leaveNoteThen(() => {
      clearLiveSession();
      navigate({ type: 'show-forums' });
    });
  }, [clearLiveSession, navigate, noteLeave.leaveNoteThen]);

  const switchVault = useCallback(async (vaultName: string, password?: string) => {
    await noteLeave.leaveNoteThen(async () => {
      await abandonUnusedSession({ refresh: false });
      await client.switchVault(vaultName, password);
      setDrafts(emptyPromptDrafts());
      writeAppRoute('/', 'replace');
      if (!contextEvents) reload();
    });
  }, [abandonUnusedSession, client, contextEvents, noteLeave.leaveNoteThen, reload]);

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
    noteWelcomeSubmission(active.forumId, active.sessionId);
    if (text.trim()) retainSession(active.forumId, active.sessionId);
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
  }, [client, noteWelcomeSubmission, retainSession, runMutation, state.activeConversation, state.activeConversationLabel, state.sessionSnapshot, state.bootstrap]);

  const attachTextFile = useCallback((file: TextFileRequest) => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    retainSession(active.forumId, active.sessionId);
    return runMutation(active, 'upload', () => client.attachTextFile(
      active.forumId, active.sessionId, file));
  }, [client, retainSession, runMutation, state.activeConversation]);

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
    noteWelcomeSubmission(active.forumId, active.sessionId);
    return runMutation(active, 'stop', () => client.stopGeneration(
      active.forumId,
      active.sessionId,
    ));
  }, [client, noteWelcomeSubmission, runMutation, state.activeConversation]);

  const setDefaultCharacter = useCallback((characterId: string) => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation(active, 'target', () => client.setDefaultCharacter(
      active.forumId,
      active.sessionId,
      characterId,
    ));
  }, [client, runMutation, state.activeConversation]);

  const setCharacterSettings = useCallback((characterId: string, patch: CharacterSettingsPatch) => {
    const active = state.activeConversation;
    if (!active) return Promise.reject(new Error('No live conversation is selected.'));
    return runMutation(active, 'settings', () => client.setCharacterSettings(
      active.forumId, active.sessionId, { character_id: characterId, ...patch },
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
    dispatch({ type: 'session-deleted', forumId, sessionId });
    await refreshBootstrap();
    const active = state.activeConversation?.forumId === forumId
      && state.activeConversation.sessionId === sessionId;
    if (active) {
      clearLiveSession();
      // A deleted background conversation does not close the open note.
      if (workspaceVisibility(state).notesVisible) dispatch({ type: 'reset-active-conversation' });
      else navigate({ type: 'show-initial-conversation' });
      writeAppRoute('/', 'replace');
    }
    setCatalogRevision((revision) => revision + 1);
  }, [clearLiveSession, client, navigate, refreshBootstrap, runMutation,
    state.activeConversation, state.mainView, state.layoutMode]);

  const deleteForum = useCallback(async (forumId: string) => {
    await client.deleteForum(forumId);
    noteActions?.refresh();
    if (state.activeConversation?.forumId === forumId) {
      clearLiveSession();
      writeAppRoute('/', 'replace');
    }
    navigate({ type: 'forum-deleted', forumId });
    setCatalogRevision((revision) => revision + 1);
  }, [clearLiveSession, client, navigate, noteActions, state.activeConversation]);

  const title = navigationTitle(state);
  const ready = state.bootstrapStatus === 'ready';
  // Bootstrap states keep the application-wide top bar.
  const visibility = workspaceVisibility(state);
  const chatVisible = ready && visibility.chatVisible;
  const notesVisible = ready && visibility.notesVisible;
  const windowItemName = ready && state.mainView === 'notes'
    ? noteItems.find(({ id }) => id === state.activeNoteId)?.name
    : ready && state.mainView === 'chat' && state.sessionOperation === 'idle'
      ? state.activeConversationLabel : null;

  useEffect(() => {
    const vaultName = state.bootstrap?.vault_name;
    document.title = vaultName
      ? `CHA: ${vaultName}${windowItemName ? ` — ${windowItemName}` : ''}`
      : 'CHA';
  }, [state.bootstrap?.vault_name, windowItemName]);

  const detailRefresh = {
    epoch: detailEpoch,
    refreshing: detailRefreshing,
    failed: detailFailed,
    stale: false,
    retry: () => { void refreshAfterWelcome(); },
  };

  return (
    <AppErrorBoundary onReload={reload}>
      <DetailRefreshProvider value={detailRefresh}>
      <TransliterationProvider>
        <div
          className={`cha-app ${state.sidebarOpen ? 'is-sidebar-open' : ''}${resizingSidebar ? ' is-resizing-sidebar' : ''}`}
          data-sidebar={state.sidebarOpen ? 'open' : 'closed'}
          style={{ '--cha-sidebar-width': `${sidebarWidth}px` } as CSSProperties}
        >
          <Sidebar
            onCreateSession={createConversation}
            onClearSessionAudioCache={clearSessionAudioCache}
            dispatch={dispatch}
            navigate={navigate}
            onDeleteSession={deleteSession}
            onDownloadSession={downloadSession}
            onOpenSession={openConversation}
            onRenameSession={renameSession}
            notesTree={(
              <NotesTree
                key={state.notesEpoch}
                activeNoteId={state.activeNoteId}
                browsing={state.sidebarTab === 'notes' && state.sidebarOpen}
                client={client}
                getNoteLength={getNoteLength}
                onBeforeCreateNote={beforeCreateNote}
                onDeleteItem={deleteNoteItem}
                onCompressNote={compressNote}
                onSetNoteReadOnly={setNoteReadOnly}
                onNoteItems={setNoteItems}
                onSourceChanged={sourceChanged}
                onOpenNote={requestOpenNote}
                vaultName={state.bootstrap?.vault_name ?? ''}
                onRegisterNoteActions={setNoteActions}
              />
            )}
            onSwitchVault={switchVault}
            state={state}
          />
          {state.sidebarOpen && (
            <div
              aria-label="Resize sidebar"
              aria-orientation="vertical"
              aria-valuemax={maximumSidebarWidth()}
              aria-valuemin={minimumSidebarWidth}
              aria-valuenow={sidebarWidth}
              className="cha-sidebar-resize"
              onKeyDown={(event) => {
                if (event.key !== 'ArrowLeft' && event.key !== 'ArrowRight') return;
                event.preventDefault();
                setSidebarWidth((width) => clampSidebarWidth(width + (event.key === 'ArrowRight' ? 16 : -16)));
              }}
              onLostPointerCapture={finishSidebarResize}
              onPointerCancel={finishSidebarResize}
              onPointerDown={(event) => {
                if (event.button !== 0) return;
                event.preventDefault();
                event.currentTarget.setPointerCapture(event.pointerId);
                sidebarResize.current = { pointerId: event.pointerId, startX: event.clientX, startWidth: sidebarWidth };
                setResizingSidebar(true);
              }}
              onPointerMove={(event) => {
                const resize = sidebarResize.current;
                if (resize?.pointerId !== event.pointerId) return;
                setSidebarWidth(clampSidebarWidth(resize.startWidth + event.clientX - resize.startX));
              }}
              onPointerUp={finishSidebarResize}
              role="separator"
              tabIndex={0}
            />
          )}
          <main className={`cha-main${chatVisible ? ' is-chat' : ''}${notesVisible ? ' is-notes' : ''}`} data-view={state.mainView}>
            {!chatVisible && !notesVisible && <TopBar title={title} />}
            {submissionErrors.map((error) => <div className="cha-submission-error" role="alert" key={error.id}>
              <span>{error.message}</span>
              <button type="button" aria-label="Dismiss message error" onClick={() => setSubmissionErrors((errors) => errors.filter(({ id }) => id !== error.id))}>Dismiss</button>
            </div>)}
            {!ready && <BootstrapState onRetry={retryBootstrap} state={state} />}
            {ready && (
              <Screen
                playbackPositions={playbackPositions.current}
                catalogRevision={catalogRevision}
                client={client}
                dispatch={dispatch}
                navigate={navigate}
                draft={chatDraftText}
                editorLiveRef={editorLiveRef}
                leave={noteLeave}
                noteItems={noteItems}
                noteActions={noteActions}
                onSetNoteReadOnly={setNoteReadOnly}
                notesDraft={drafts.notes}
                onDraftChange={editChat}
                onNotesDraftChange={editNotes}
                onRegisterDispose={registerDispose}
                onRegisterSave={registerSave}
                onCoverConversation={coverConversation}
                onDeleteTurn={deleteTurn}
                onDeleteForum={deleteForum}
                onCreateSession={createConversation}
                onOpenSession={openConversation}
                onRetrySessionOpen={retrySessionOpen}
                onBrowseSessions={browseSessions}
                onRetryStream={retryStream}
                onReturnToStart={returnToStart}
                onSetDefaultCharacter={setDefaultCharacter}
                onSetCharacterSettings={setCharacterSettings}
                onStopGeneration={stopGeneration}
                onSubmitInput={submitInput}
                onAttachTextFile={attachTextFile}
                onUncoverConversation={uncoverConversation}
                state={state}
              />
            )}
          </main>
        </div>
      </TransliterationProvider>
      </DetailRefreshProvider>
    </AppErrorBoundary>
  );
}
