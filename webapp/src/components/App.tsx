import type { NoteImportSource } from '../notes/importSource';
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
  type SetStateAction,
} from 'react';
import { flushSync } from 'react-dom';

import {
  ChaError,
  publicErrorMessage,
  type Bootstrap,
  type ChaClient,
  type CharacterSettingsPatch,
  type NoteItem,
  type NotesClient,
  type NoteTextMode,
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
import { summaryDocument, summaryConversionError } from '../notes/summaryImport';
import { useNoteLeave } from '../notes/useNoteLeave';
import { compressAndWait, type CompressionCancellation } from '../notes/compression';
import { summarizeAndWait } from '../notes/summaries';
import { useNoteImports, type NoteImports } from '../notes/useNoteImports';
import type { NoteSaveHandle } from '../notes/useNoteSave';
import { reloadApplication, writeAppRoute } from '../state/route';
import { useLiveSession, type SessionEventsConnector } from '../useLiveSession';
import { useSessionActivity } from '../useSessionActivity';
import {
  appReducer,
  createInitialAppState,
  type ViewPreferences,
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
  ForumLibraryScreen,
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
import { NoteCompressionDialog, type NoteCompressionProgress } from './NoteCompressionDialog';
import { NotesTree, type CompressNote, type DeleteNoteItem, type NoteActions, type SetNoteReadOnly, type SummarizeNotes } from './NotesTree';
import { Sidebar } from './Sidebar';
import { TransliterationProvider } from './TransliterationMode';
import { ApiKeyScreen, ApiKeysScreen, NewApiKeyScreen } from './ApiKeySettings';
import { CloudStoreKeysScreen, CloudStoreScreen, CloudStoresScreen } from './CloudStoreSettings';
import {
  DownloadVaultScreen,
  MergeVaultScreen,
  NewVaultScreen,
  VaultScreen,
  VaultsScreen,
} from './VaultSettings';
import { NewProviderScreen, ProviderScreen, ProvidersScreen } from './ProviderSettings';
import { PromptScreen, PromptsScreen } from './PromptSettings';
import { NewStyleScreen, StyleScreen, StylesScreen } from './StyleSettings';
import { NewVoiceScreen, VoiceScreen, VoiceSettingsScreen, VoicesScreen } from './VoiceSettings';
import { R2StorageScreen } from './R2StorageSettings';
import { D1SyncScreen } from './D1SyncSettings';
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
  onNotesDraftChange: Dispatch<SetStateAction<string>>;
  noteItems: readonly NoteItem[];
  noteActions: NoteActions | null;
  noteImports: NoteImports;
  editorLiveRef: { current: Editor | null };
  leave: {
    decisionOpen: boolean;
    retry(): void;
    stay(): void;
    discard(): void;
  };
  compressionOpen: boolean;
  noteActionPending: boolean;
  onNoteTextModeChange(mode: NoteTextMode): void;
  onRegisterSave(save: NoteSaveHandle | null, documentId: number | null): void;
  onRegisterLoad(load: Promise<void>): void;
  onRegisterDispose(dispose: () => void): void;
  onDraftChange(next: string): void;
  onDeleteForum(forumId: string): Promise<void>;
  onCreateSession(forumId: string): Promise<boolean>;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
  onRetrySessionOpen(): void;
  onBrowseSessions(): void;
  onBackgroundError(message: string): void;
  catalogRevision: number;
}

function Screen(props: ScreenProps) {
  const { state } = props;
  const { notesVisible, chatVisible } = workspaceVisibility(state);
  const [ratio, setRatio] = useState(0.5);
  // Chat keeps its playback speed across sessions, as Notes keeps it across notes.
  const [chatPlaybackSpeed, setChatPlaybackSpeed] = useState(10);
  const content = { ...props, chatPlaybackSpeed, onChatPlaybackSpeedChange: setChatPlaybackSpeed };
  const workspaceRef = useRef<HTMLDivElement>(null);
  const split = notesVisible && chatVisible;
  if (!notesVisible && !chatVisible) return <ScreenContent {...content} view={state.mainView} />;
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
        <ScreenContent {...content} view="notes" />
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
        </> : <ScreenContent {...content} view="chat" />}
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
  noteImports,
  editorLiveRef,
  leave,
  compressionOpen,
  noteActionPending,
  onNoteTextModeChange,
  onRegisterSave,
  onRegisterLoad,
  onRegisterDispose,
  onRetryStream,
  onReturnToStart,
  onSetDefaultCharacter,
  onSetCharacterSettings,
  onStopGeneration,
  onSubmitInput,
  onAttachTextFile,
  onUncoverConversation,
  onBackgroundError,
  catalogRevision,
  chatPlaybackSpeed,
  onChatPlaybackSpeedChange,
  view,
}: ScreenProps & {
  view: AppState['mainView'];
  chatPlaybackSpeed: number;
  onChatPlaybackSpeedChange(speed: number): void;
}) {
  switch (view) {
    case 'chat': return (
      <ChatScreen
        key={`${state.bootstrap?.vault_name}/${state.activeConversation?.forumId}/${state.activeConversation?.sessionId}`}
        playbackPositions={playbackPositions}
        playbackSpeed={chatPlaybackSpeed}
        onPlaybackSpeedChange={onChatPlaybackSpeedChange}
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
    case 'forum-library': return (
      <ForumLibraryScreen key={`${state.bootstrap?.vault_name}:${state.currentForumId}`} client={client} dispatch={dispatch} navigate={navigate} state={state} />
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
      <VaultScreen client={client} dispatch={dispatch} navigate={navigate} state={state}
        onBackgroundError={onBackgroundError} />
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
    case 'settings-prompts': return (
      <PromptsScreen key={state.notesEpoch} client={client} navigate={navigate} />
    );
    case 'settings-prompt': return (
      <PromptScreen key={`${state.notesEpoch}:${state.inspectedPrompt.id}`}
        client={client} navigate={navigate} state={state} />
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
    case 'settings-cloud-stores': return (
      <CloudStoreKeysScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-cloud-store-key': return (
      <CloudStoresScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-cloud-store': return (
      <CloudStoreScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-r2-storage': return (
      <R2StorageScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
    );
    case 'settings-d1-sync': return (
      <D1SyncScreen client={client} dispatch={dispatch} navigate={navigate} state={state} />
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
        imports={noteImports}
        noteRequest={state.noteRequest}
        noteTextMode={state.noteTextMode}
        noteActionPending={noteActionPending}
        onNoteTextModeChange={onNoteTextModeChange}
        compressionOpen={compressionOpen}
        autoFocus={state.noteAutoFocus}
        destination={state.noteDestination}
        importSource={state.noteImportSource}
        notesEpoch={state.notesEpoch}
        onDraftChange={onNotesDraftChange}
        onRegisterDispose={onRegisterDispose}
        onRegisterSave={onRegisterSave}
        onRegisterLoad={onRegisterLoad}
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
  initialViewPreferences?: Partial<ViewPreferences>;
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
  initialViewPreferences,
  reload = defaultReload,
}: AppProps) {
  const [state, dispatch] = useReducer(appReducer, initialViewPreferences ?? {}, createInitialAppState);
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
  const reportBackgroundError = useCallback((message: string) => {
    setSubmissionErrors((errors) => [...errors, { id: ++nextSubmissionError.current, message }]);
  }, []);
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
  const documentIdRef = useRef<number | null>(null);
  const noteLoadRef = useRef<Promise<void> | null>(null);
  const editorLiveRef = useRef<Editor | null>(null);
  const disposeEditorRef = useRef<() => void>(() => undefined);
  const notesEpochRef = useRef(state.notesEpoch);
  const activeNoteIdRef = useRef(state.activeNoteId);
  notesEpochRef.current = state.notesEpoch;
  activeNoteIdRef.current = state.activeNoteId;
  const notesVisibleRef = useRef(false);
  const compressionRun = useRef<(CompressionCancellation & { notesChanged: boolean; label?: string }) | null>(null);
  const [compression, setCompression] = useState<NoteCompressionProgress | null>(null);
  const compressionInProgress = useCallback(() => compressionRun.current !== null, []);
  notesVisibleRef.current = workspaceVisibility(state).notesVisible;
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
  useLayoutEffect(() => {
    reportNativeLayout(state.layoutMode === 'split', state.sidebarOpen, state.pictureOpen,
      state.notesToolbarVisible, state.notesPromptVisible, state.chatToolbarVisible, state.russianVisible);
  }, [state.layoutMode, state.sidebarOpen, state.pictureOpen, state.notesToolbarVisible, state.notesPromptVisible, state.chatToolbarVisible, state.russianVisible]);
  useEffect(() => installNativeLayoutHandler((requested) => {
    if (compressionRun.current) return;
    if (requested === 'toggle-russian') {
      dispatch({ type: 'toggle-russian' });
      return;
    }
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
  const registerSave = useCallback((save: NoteSaveHandle | null, documentId: number | null) => {
    noteSaveRef.current = save;
    documentIdRef.current = documentId;
  }, []);
  const registerLoad = useCallback((load: Promise<void>) => {
    noteLoadRef.current = load;
    void load.then(() => { if (noteLoadRef.current === load) noteLoadRef.current = null; });
  }, []);
  const registerDispose = useCallback((dispose: () => void) => {
    disposeEditorRef.current = dispose;
  }, []);
  const [noteItems, setNoteItems] = useState<NoteItem[]>([]);
  const [noteActions, setNoteActions] = useState<NoteActions | null>(null);
  const noteActionsRef = useRef(noteActions);
  noteActionsRef.current = noteActions;
  const noteImports = useNoteImports({
    client,
    epoch: state.notesEpoch,
    leave: noteLeave,
    openNote: () => ({ noteId: activeNoteIdRef.current, documentId: documentIdRef.current,
      editor: editorLiveRef.current, save: noteSaveRef.current }),
    waitForOpen: () => noteLoadRef.current,
    updated: item => {
      setNoteItems(items => items.map(note => note.id === item.id ? item : note));
      noteActions?.updated(item);
    },
    refresh: () => { noteActions?.refresh(); },
  });

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
  const editNotes = useCallback((text: SetStateAction<string>) => {
    setDrafts((current) => editNotesDraft(current, typeof text === 'function' ? text(current.notes) : text));
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
    navigationBlocked: compressionInProgress,
  });

  useEffect(() => {
    setNoteItems([]);
  }, [state.notesEpoch]);

  const requestOpenNote = useCallback((id: number, importSource?: NoteImportSource) => {
    navigate({ type: 'open-note', noteId: id, importSource });
  }, [navigate]);

  const changeNoteTextMode = useCallback((mode: NoteTextMode) => {
    if (mode === state.noteTextMode || compressionRun.current) return;
    const context = vaultContext.current;
    const noteId = activeNoteIdRef.current;
    void noteLeave.leaveNoteThen(() => {
      if (vaultContext.current !== context) return false;
      dispatch({ type: 'set-note-text-mode', mode, noteId });
    });
  }, [noteLeave.leaveNoteThen, state.noteTextMode]);

  const sourceChanged = useCallback((parentId: number | null) => {
    const active = noteItems.find((item) => item.id === activeNoteIdRef.current);
    if (active?.kind === 'annotation' && active.parent_id === parentId) {
      navigate({ type: 'open-note', noteId: active.id });
    }
  }, [navigate, noteItems]);

  const summaryViewRef = useRef({ noteActions, sourceChanged });
  summaryViewRef.current = { noteActions, sourceChanged };
  const notesCancellation = useRef(new AbortController());
  useEffect(() => {
    const controller = new AbortController();
    notesCancellation.current = controller;
    return () => controller.abort();
  }, [state.notesEpoch]);

  const saveSummary = useCallback(async (forumId: string, sessionId: string) => {
    const epoch = client.contextEpoch();
    const editorEpoch = notesEpochRef.current;
    const signal = notesCancellation.current.signal;
    const current = () => {
      try { return !signal.aborted && notesEpochRef.current === editorEpoch && client.contextEpoch() === epoch; }
      catch { return false; }
    };
    let generated: Awaited<ReturnType<NotesClient['generateSessionSummary']>> | undefined;
    let guarded = false;
    let submitted = false;
    let replaced = false;
    let recovering = false;
    let cancelledSave = false;
    let save: NoteSaveHandle | null = null;
    let targetWasOpen = false;
    const sameTarget = () => generated?.target_note_id != null && activeNoteIdRef.current === generated.target_note_id;
    const discardBuffer = () => {
      if (!targetWasOpen || notesEpochRef.current !== editorEpoch || !sameTarget()) return;
      save?.dispose();
      disposeEditorRef.current();
    };
    // A hidden Notes panel loads saved content when it opens again, so do not switch screens.
    const reloadTarget = () => {
      if (targetWasOpen && sameTarget() && notesVisibleRef.current) dispatch({ type: 'open-note', noteId: generated!.target_note_id! });
    };
    const reloadAnnotations = () => {
      if (notesVisibleRef.current) summaryViewRef.current.sourceChanged(generated!.parent_id);
    };
    try {
      generated = await client.generateSessionSummary(forumId, sessionId);
      if (!current()) return;
      let document: Record<string, unknown>;
      try { document = summaryDocument(generated.markdown); }
      catch { throw new ChaError('invalid_argument', summaryConversionError); }
      while (!noteLeave.beginNoteAction()) {
        await noteLeave.whenIdle(signal);
        if (!current()) return;
      }
      guarded = true;
      if (!current()) return;
      targetWasOpen = sameTarget();
      if (targetWasOpen) {
        save = noteSaveRef.current;
        if (save?.isOpen) {
          cancelledSave = save.freeze();
          await save.waitInFlight();
          if (!current() || !sameTarget() || noteSaveRef.current?.waitInFlight !== save.waitInFlight) return;
        } else {
          // Cancel a pending pre-commit getNote reply before it can create a stale editor.
          disposeEditorRef.current();
        }
      }
      submitted = true;
      let item: NoteItem;
      try {
        item = await client.commitSessionSummary(generated.run_id, document);
      } catch (failure) {
        const definite = failure instanceof ChaError
          && !['command_timeout', 'application_unavailable', 'server_stopping'].includes(failure.code);
        if (!definite && !current()) {
          discardBuffer();
          replaced = true;
        } else if (!definite) {
          // This acknowledgement orders the reload after a queued commit or revokes its token.
          // If disconnected, dispose the buffer but do not reopen until authoritative reads are possible.
          try { await client.abandonSessionSummary(generated.run_id, 'commit_unknown'); }
          catch (barrierFailure) {
            discardBuffer();
            replaced = true;
            // No ordering acknowledgement means a queued commit may still run.
            // Keep note actions blocked while the existing reload path replaces
            // the connection and retires its outstanding work.
            if (current()) {
              recovering = true;
              reload();
            }
            throw barrierFailure;
          }
          discardBuffer();
          replaced = true;
          if (current()) {
            // A lost reply has no returned metadata. Read it before reopening controls.
            const items = await client.listNoteItems();
            if (!current()) return;
            setNoteItems(items);
            const item = items.find(note => note.id === generated!.target_note_id);
            if (item) summaryViewRef.current.noteActions?.updated(item);
            reloadTarget();
            summaryViewRef.current.noteActions?.refresh();
            noteLeave.endNoteAction();
            guarded = false;
            reloadAnnotations();
          }
        } else if (targetWasOpen && !save?.isOpen && current()) reloadTarget();
        throw failure;
      }
      replaced = true;
      if (!current()) return;
      discardBuffer();
      setNoteItems(items => items.some(note => note.id === item.id)
        ? items.map(note => note.id === item.id ? item : note) : [...items, item]);
      summaryViewRef.current.noteActions?.updated(item);
      reloadTarget();
      summaryViewRef.current.noteActions?.refresh();
      noteLeave.endNoteAction();
      guarded = false;
      reloadAnnotations();
      window.dispatchEvent(new CustomEvent('cha-note-audio-changed', { detail: { id: item.id, affectedNoteOnly: true } }));
    } catch (failure) {
      if (!signal.aborted) setSubmissionErrors(errors => [...errors, {
        id: ++nextSubmissionError.current,
        message: publicErrorMessage(failure, 'The summary could not be saved. Check Settings → Events and try again.'),
      }]);
    } finally {
      if (generated && !submitted) {
        try { await client.abandonSessionSummary(generated.run_id, 'conversion_failed'); }
        catch { /* Connection and vault cleanup also retire uncommitted runs. */ }
      }
      if (!current()) discardBuffer();
      else if (!replaced && save?.isOpen && sameTarget()) {
        save.unfreeze();
        if (cancelledSave) save.submitNow();
      }
      if (guarded && !recovering) noteLeave.endNoteAction();
    }
  }, [client, reload, noteLeave.beginNoteAction, noteLeave.endNoteAction, noteLeave.whenIdle]);

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
      return activeNoteIdRef.current === id && documentIdRef.current === id && editor && !editor.isDestroyed
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

  const runNoteOperation = useCallback(async (
    operation: { source: NoteItem } | { targets: Parameters<CompressNote>[0]; folder: boolean },
    epoch: number,
    refresh: () => Promise<void>,
  ) => {
    const context = notesCancellation.current.signal;
    const checkContext = () => {
      if (context.aborted || client.contextEpoch() !== epoch)
        throw new ChaError('vault_changed', 'The vault changed. Start again in the current vault.');
    };
    checkContext();
    if (!noteLeave.beginNoteAction()) throw new ChaError('invalid_argument', 'A notes action is already in progress.');
    const run: NonNullable<typeof compressionRun.current> = {
      requested: false, notesChanged: false, label: 'source' in operation ? 'Note summaries' : undefined,
    };
    compressionRun.current = run;
    setCompression({ message: 'Saving note…', cancelEnabled: true });
    const noteId = activeNoteIdRef.current;
    const save = noteSaveRef.current;
    const editor = editorLiveRef.current;
    let reload = false;
    let started = false;
    try {
      if (save?.isOpen) {
        save.freeze();
        const admitted = save.inFlightSerial();
        await save.waitInFlight();
        checkContext();
        if (save.saveError?.serial === admitted || !await save.flushRemaining()) return;
      }
      checkContext();
      if (run.requested) return;
      started = true;
      if ('source' in operation) {
        const { source } = operation;
        const result = await summarizeAndWait(client, source, run, checkContext, progress => {
          if (progress.cancelEnabled) setCompression(progress);
          // Disable Cancel before the folder commit is sent.
          else flushSync(() => setCompression(progress));
        });
        reload = (state.noteTextMode === 'l1' || state.noteTextMode === 'l2')
          && noteId !== null && result.targetIds.includes(noteId);
        if (result.folderSummaryCommitted) {
          reload ||= noteItems.some(item => item.id === noteId && item.parent_id === source.id
            && (item.folder_summary || item.kind === 'annotation'));
        }
      } else {
        const { targets, folder } = operation;
        for (const [index, target] of targets.entries()) {
          if (run.requested) break;
          checkContext();
          const progress = {
            message: `Compressing “${target.name}”…`,
            count: folder ? `Note ${index + 1} of ${targets.length}` : undefined,
            cancelEnabled: true,
          };
          setCompression(progress);
          await compressAndWait(client, target.id, epoch, folder, run, context, () => setCompression(progress));
          if (target.id === noteId && state.noteTextMode === 'compressed') reload = true;
        }
      }
    } finally {
      try {
        if (started || run.notesChanged) {
          checkContext();
          setCompression(progress => progress && { ...progress, cancelEnabled: false });
          await refresh();
          checkContext();
          if (reload && !run.notesChanged && noteId === activeNoteIdRef.current && notesVisibleRef.current) {
            try { await save?.saveScrollPosition?.(); }
            catch (failure) { console.warn('Could not save the note scroll position.', failure); }
          }
          // A sync notice can arrive while the replacement is opening.
          while ((reload || run.notesChanged) && noteId !== null
              && noteId === activeNoteIdRef.current && notesVisibleRef.current) {
            checkContext();
            reload = false;
            run.notesChanged = false;
            setCompression({ message: 'Opening note…', cancelEnabled: false });
            flushSync(() => dispatch({ type: 'reload-note' }));
            await noteLoadRef.current;
          }
        }
      } finally {
        if (save?.isOpen && !context.aborted && editor === editorLiveRef.current) save.unfreeze();
        compressionRun.current = null;
        setCompression(null);
        noteLeave.endNoteAction();
      }
    }
  }, [client, noteItems, noteLeave.beginNoteAction, noteLeave.endNoteAction, state.noteTextMode]);

  const compressNote = useCallback<CompressNote>((targets, epoch, folder, refresh) =>
    runNoteOperation({ targets, folder }, epoch, refresh), [runNoteOperation]);
  const summarizeNotes = useCallback<SummarizeNotes>((source, epoch, refresh) =>
    runNoteOperation({ source }, epoch, refresh), [runNoteOperation]);

  const deleteNoteItem = useCallback<DeleteNoteItem>(async (sourceId, listing, refresh) => {
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
      await client.deleteNoteItem(sourceId);
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

  useEffect(() => {
    if (!contextEvents) return;
    const unsubscribe = contextEvents.on<{
      context_epoch?: number;
    }>('notes.changed', (event) => {
      try {
        if (event.context_epoch !== client.contextEpoch()) return;
      } catch {
        return;
      }
      noteActionsRef.current?.refresh();
      if (compressionRun.current) compressionRun.current.notesChanged = true;
      else if (notesVisibleRef.current) dispatch({ type: 'reload-note' });
    });
    return unsubscribe;
  }, [client, contextEvents, dispatch]);

  const returnToStart = useCallback(() => {
    if (compressionRun.current) return;
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
    noteActions?.refresh();
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
  }, [clearLiveSession, client, navigate, noteActions, refreshBootstrap, runMutation,
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
      <TransliterationProvider controlsVisible={state.russianVisible}>
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
            onSaveSummary={saveSummary}
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
                onSummarize={summarizeNotes}
                compressionInProgress={compressionInProgress}
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
                noteImports={noteImports}
                notesDraft={drafts.notes}
                onDraftChange={editChat}
                onNotesDraftChange={editNotes}
                onRegisterDispose={registerDispose}
                onRegisterLoad={registerLoad}
                onNoteTextModeChange={changeNoteTextMode}
                noteActionPending={noteLeave.pendingKind !== null}
                compressionOpen={compression !== null}
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
                onBackgroundError={reportBackgroundError}
                state={state}
              />
            )}
          </main>
        </div>
        {compression && <NoteCompressionDialog label={compressionRun.current?.label} progress={compression} onCancel={() => {
          const run = compressionRun.current;
          if (!compression.cancelEnabled || !run) return;
          run.requested = true;
          setCompression({ message: 'Cancelling…', count: compression.count, cancelEnabled: false });
          run.cancelJob?.();
        }} />}
      </TransliterationProvider>
      </DetailRefreshProvider>
    </AppErrorBoundary>
  );
}
