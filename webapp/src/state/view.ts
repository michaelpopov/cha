import type { AnnotationDestination } from '../notes/annotationSource';
import type {
  Bootstrap,
  CharacterDetail,
  ForumDetail,
  PersonaDetail,
  SessionSnapshot,
  VaultDetail,
} from '../api/client';
import type { AppendEvent } from '../api/events';
import { welcomeSessionId } from './route';
import {
  applyCharacterUpdate,
  applyForumUpdate,
  applyPersonaUpdate,
  forumSummary,
  rosterSummary,
} from './entityUpdates';

export type MainView =
  | 'chat'
  | 'notes'
  | 'personas'
  | 'new-persona'
  | 'persona-detail'
  | 'persona-settings'
  | 'characters'
  | 'character-instructions'
  | 'new-character'
  | 'character-detail'
  | 'character-files'
  | 'character-file'
  | 'new-character-file'
  | 'character-settings'
  | 'forums'
  | 'new-forum'
  | 'sessions'
  | 'forum-detail'
  | 'forum-file'
  | 'new-forum-file'
  | 'forum-members'
  | 'settings'
  | 'settings-vaults'
  | 'settings-new-vault'
  | 'settings-download-vault'
  | 'settings-merge-vault'
  | 'settings-vault'
  | 'settings-providers'
  | 'settings-new-provider'
  | 'settings-provider'
  | 'settings-styles'
  | 'settings-new-style'
  | 'settings-style'
  | 'settings-voices'
  | 'settings-voice-input'
  | 'settings-jev'
  | 'settings-events'
  | 'settings-notes'
  | 'settings-web-search'
  | 'settings-new-voice'
  | 'settings-voice'
  | 'settings-api-keys'
  | 'settings-new-api-key'
  | 'settings-api-key'
  | 'settings-r2-storage';

export type BootstrapStatus = 'loading' | 'ready' | 'failed' | 'incompatible';
export type LayoutMode = 'single' | 'split';

export function workspaceVisibility(state: Pick<AppState, 'mainView' | 'layoutMode'>) {
  const workspace = state.mainView === 'notes' || state.mainView === 'chat';
  return {
    notesVisible: workspace && (state.layoutMode === 'split' || state.mainView === 'notes'),
    chatVisible: workspace && (state.layoutMode === 'split' || state.mainView === 'chat'),
  };
}
export type StreamStatus =
  | 'idle'
  | 'connecting'
  | 'connected'
  | 'reconnecting'
  | 'retry';

export interface ActiveConversation {
  forumId: string;
  sessionId: string;
}

interface EditableInspection {
  id: string | null;
  name: string | null;
  writable: boolean;
}

export interface SessionActivity {
  forumId: string;
  sessionId: string;
  status: 'generating' | 'finished';
}

export interface AppState {
  sessionActivity: readonly SessionActivity[];
  sidebarOpen: boolean;
  pictureOpen: boolean;
  notesToolbarVisible: boolean;
  chatToolbarVisible: boolean;
  notesPromptVisible: boolean;
  pictureWidth: number;
  pictureCharacterId: string | null;
  mainView: MainView;
  layoutMode: LayoutMode;
  sidebarTab: 'chat' | 'notes';
  activeNoteId: number | null;
  notesEpoch: number;
  noteRequest: number;
  noteDestination?: AnnotationDestination;
  bootstrapStatus: BootstrapStatus;
  bootstrap: Bootstrap | null;
  bootstrapMessage: string | null;
  currentForumId: string | null;
  activeConversation: ActiveConversation | null;
  inspectedCharacter: {
    id: string | null;
    file: string | null;
    settingsWritable: boolean;
    writable: boolean;
  };
  inspectedPersona: { id: string | null; writable: boolean };
  // Forum inspection describes currentForumId, also used by session browsing.
  inspectedForum: { file: string | null; writable: boolean };
  inspectedVaultName: string | null;
  inspectedProvider: EditableInspection;
  inspectedStyle: EditableInspection;
  inspectedVoice: EditableInspection;
  inspectedApiKey: { id: string | null; name: string | null };
  currentDefaultCharacterId: string | null;
  sessionOperation: 'idle' | 'pending' | 'failed';
  sessionOperationMessage: string | null;
  sessionOperationRetryable: boolean;
  activeConversationLabel: string | null;
  sessionSnapshot: SessionSnapshot | null;
  audioCacheClearCount: number;
  streamStatus: StreamStatus;
  streamMessage: string | null;
}

export const initialAppState: AppState = {
  sessionActivity: [],
  sidebarOpen: true,
  pictureOpen: false,
  notesToolbarVisible: true,
  chatToolbarVisible: true,
  notesPromptVisible: true,
  pictureWidth: 280,
  pictureCharacterId: null,
  mainView: 'chat',
  layoutMode: 'single',
  sidebarTab: 'chat',
  activeNoteId: null, noteDestination: undefined,
  notesEpoch: 0,
  noteRequest: 0,
  bootstrapStatus: 'loading',
  bootstrap: null,
  bootstrapMessage: null,
  currentForumId: null,
  activeConversation: null,
  inspectedCharacter: { id: null, file: null, settingsWritable: false, writable: false },
  inspectedPersona: { id: null, writable: false },
  inspectedForum: { file: null, writable: false },
  inspectedVaultName: null,
  inspectedProvider: { id: null, name: null, writable: false },
  inspectedStyle: { id: null, name: null, writable: false },
  inspectedVoice: { id: null, name: null, writable: false },
  inspectedApiKey: { id: null, name: null },
  currentDefaultCharacterId: null,
  sessionOperation: 'idle',
  sessionOperationMessage: null,
  sessionOperationRetryable: false,
  activeConversationLabel: null,
  sessionSnapshot: null,
  audioCacheClearCount: 0,
  streamStatus: 'idle',
  streamMessage: null,
};

export type AppAction =
  | { type: 'set-layout'; mode: LayoutMode }
  | { type: 'bootstrap-started' }
  | { type: 'bootstrap-loaded'; bootstrap: Bootstrap }
  | { type: 'bootstrap-failed'; message: string; incompatible: boolean }
  | { type: 'bootstrap-refreshed'; bootstrap: Bootstrap }
  | { type: 'vault-context-reset' }
  | { type: 'vault-context-refreshed'; bootstrap: Bootstrap }
  | { type: 'toggle-sidebar' }
  | { type: 'show-chat-tab' }
  | { type: 'show-notes-tab' }
  | { type: 'open-note'; noteId: number; destination?: AnnotationDestination }
  | { type: 'close-note' }
  | { type: 'toggle-picture' }
  | { type: 'toggle-notes-toolbar' }
  | { type: 'toggle-chat-toolbar' }
  | { type: 'toggle-notes-prompt' }
  | { type: 'resize-picture'; width: number }
  | { type: 'select-picture-character'; characterId: string }
  | { type: 'show-personas' }
  | { type: 'show-new-persona' }
  | { type: 'inspect-persona'; personaId: string }
  | { type: 'persona-detail-loaded'; personaId: string; writable: boolean }
  | { type: 'persona-created'; persona: PersonaDetail }
  | { type: 'persona-updated'; persona: PersonaDetail }
  | { type: 'persona-deleted'; personaId: string }
  | { type: 'show-persona-settings' }
  | { type: 'show-characters' }
  | { type: 'show-character-instructions' }
  | { type: 'show-new-character' }
  | { type: 'inspect-character'; characterId: string }
  | { type: 'inspect-character-file'; characterId: string; filename: string }
  | { type: 'show-new-character-file' }
  | { type: 'character-detail-loaded'; characterId: string; settingsWritable: boolean; writable: boolean }
  | { type: 'character-created'; character: CharacterDetail }
  | { type: 'character-updated'; character: CharacterDetail }
  | { type: 'character-deleted'; characterId: string }
  | { type: 'show-character-settings' }
  | { type: 'show-character-files' }
  | { type: 'show-forums' }
  | { type: 'show-new-forum' }
  | { type: 'forum-created'; forum: ForumDetail }
  | { type: 'select-forum'; forumId: string }
  | { type: 'show-sessions' }
  | { type: 'show-forum-detail' }
  | { type: 'inspect-forum-file'; forumId: string; filename: string }
  | { type: 'show-new-forum-file' }
  | { type: 'show-forum-members' }
  | { type: 'forum-detail-loaded'; forumId: string; writable: boolean }
  | { type: 'forum-updated'; forum: ForumDetail }
  | { type: 'forum-deleted'; forumId: string }
  | { type: 'show-settings' }
  | { type: 'show-settings-vaults' }
  | { type: 'show-settings-new-vault' }
  | { type: 'show-settings-download-vault' }
  | { type: 'show-settings-merge-vault' }
  | { type: 'inspect-vault'; vaultName: string }
  | { type: 'vault-created'; vault: VaultDetail }
  | { type: 'vault-downloaded'; vault: VaultDetail }
  | { type: 'vault-updated'; previousName: string; vault: VaultDetail }
  | { type: 'vault-deleted'; vaultName: string }
  | { type: 'show-settings-providers' }
  | { type: 'show-settings-new-provider' }
  | { type: 'inspect-provider'; providerId: string; providerName: string }
  | { type: 'provider-detail-loaded'; providerId: string; providerName: string; writable: boolean }
  | { type: 'provider-updated'; providerId: string; providerName: string; writable: boolean }
  | { type: 'show-settings-styles' }
  | { type: 'show-settings-new-style' }
  | { type: 'inspect-style'; styleId: string; styleName: string }
  | { type: 'style-detail-loaded'; styleId: string; styleName: string; writable: boolean }
  | { type: 'style-updated'; styleId: string; styleName: string; writable: boolean }
  | { type: 'show-settings-voices' }
  | { type: 'show-settings-voice-input' }
  | { type: 'show-settings-jev' }
  | { type: 'show-settings-events' }
  | { type: 'show-settings-notes' }
  | { type: 'show-settings-web-search' }
  | { type: 'show-settings-new-voice' }
  | { type: 'inspect-voice'; voiceId: string; voiceName: string }
  | { type: 'voice-detail-loaded'; voiceId: string; voiceName: string; writable: boolean }
  | { type: 'voice-updated'; voiceId: string; voiceName: string; writable: boolean }
  | { type: 'show-settings-api-keys' }
  | { type: 'show-settings-new-api-key' }
  | { type: 'inspect-api-key'; apiKeyId: string; apiKeyName: string }
  | { type: 'api-key-detail-loaded'; apiKeyId: string; apiKeyName: string }
  | { type: 'api-key-updated'; apiKeyId: string; apiKeyName: string }
  | { type: 'show-settings-r2-storage' }
  | { type: 'show-chat' }
  | { type: 'session-operation-started'; message: string }
  | { type: 'session-operation-failed'; message: string; retryable?: boolean }
  | { type: 'conversation-opened'; snapshot: SessionSnapshot; navigate?: boolean }
  | { type: 'session-snapshot'; snapshot: SessionSnapshot }
  | { type: 'session-discarded'; forumId: string; sessionId: string }
  | { type: 'session-audio-cache'; forumId: string; sessionId: string; entryId?: number; cached: boolean }
  | { type: 'session-append'; forumId: string; sessionId: string; event: AppendEvent }
  | {
    type: 'session-generation';
    forumId: string;
    sessionId: string;
    generation: SessionSnapshot['generation'];
  }
  | { type: 'show-initial-conversation' }
  // The initial session becomes the active conversation. The main panel stays.
  | { type: 'reset-active-conversation' }
  | { type: 'session-activity-ended'; forumId: string; sessionId: string }
  | { type: 'session-deleted'; forumId: string; sessionId: string }
  | { type: 'stream-state'; status: StreamStatus; message?: string };

// Navigation intents start a new navigation epoch and discard an unused
// session. All except vault-context-reset first leave the open note.
export type NavigationAction = Extract<AppAction, {
  type:
    | 'vault-context-reset'
    | 'open-note'
    | 'show-personas'
    | 'show-new-persona'
    | 'inspect-persona'
    | 'persona-created'
    | 'persona-deleted'
    | 'show-persona-settings'
    | 'show-characters'
    | 'show-character-instructions'
    | 'show-new-character'
    | 'inspect-character'
    | 'inspect-character-file'
    | 'show-new-character-file'
    | 'character-created'
    | 'character-deleted'
    | 'show-character-settings'
    | 'show-character-files'
    | 'show-forums'
    | 'show-new-forum'
    | 'forum-created'
    | 'select-forum'
    | 'show-sessions'
    | 'show-forum-detail'
    | 'inspect-forum-file'
    | 'show-new-forum-file'
    | 'show-forum-members'
    | 'forum-deleted'
    | 'show-settings'
    | 'show-settings-vaults'
    | 'show-settings-new-vault'
    | 'show-settings-download-vault'
    | 'show-settings-merge-vault'
    | 'inspect-vault'
    | 'vault-created'
    | 'vault-deleted'
    | 'show-settings-providers'
    | 'show-settings-new-provider'
    | 'inspect-provider'
    | 'show-settings-styles'
    | 'show-settings-new-style'
    | 'inspect-style'
    | 'show-settings-voices'
    | 'show-settings-voice-input'
    | 'show-settings-jev'
    | 'show-settings-events'
    | 'show-settings-notes'
    | 'show-settings-web-search'
    | 'show-settings-new-voice'
    | 'inspect-voice'
    | 'show-settings-api-keys'
    | 'show-settings-new-api-key'
    | 'inspect-api-key'
    | 'show-settings-r2-storage'
    | 'show-chat'
    | 'show-initial-conversation'
}>;

// Screens send navigation intents only through navigate.
export type DataAction = Exclude<AppAction, NavigationAction>;

function idleSessionOperation() {
  return {
    sessionOperation: 'idle' as const,
    sessionOperationMessage: null,
    sessionOperationRetryable: false,
  };
}

// The startup conversation and Return to start use the initial session named
// by bootstrap, in the forum that owns it.
function showInitialConversation(state: AppState, bootstrap: Bootstrap): AppState {
  const initialForum = bootstrap.forums.find(({ id }) => id === bootstrap.initial_forum_id);
  const initialRecent = bootstrap.recent_sessions.find(
    ({ forum_id, session_id }) => forum_id === bootstrap.initial_forum_id
      && session_id === bootstrap.initial_session_id,
  );
  return {
    ...state,
    mainView: 'chat',
    currentForumId: bootstrap.initial_forum_id,
    activeConversation: {
      forumId: bootstrap.initial_forum_id,
      sessionId: bootstrap.initial_session_id,
    },
    activeConversationLabel: initialRecent?.session_label ?? null,
    currentDefaultCharacterId: initialForum?.default_character_id ?? null,
    sessionSnapshot: null,
    pictureCharacterId: null,
    streamStatus: 'idle',
    streamMessage: null,
    ...idleSessionOperation(),
  };
}

function appendSessionEvent(
  snapshot: SessionSnapshot,
  event: AppendEvent,
): SessionSnapshot {
  const target = event.target;
  const entryIndex = snapshot.transcript.findIndex(({ id }) => id === target.entry_id);
  if (entryIndex < 0) return snapshot;
  const transcript = [...snapshot.transcript];
  transcript[entryIndex] = {
    ...transcript[entryIndex],
    text: transcript[entryIndex].text + event.text,
  };
  return { ...snapshot, transcript };
}

// Recover only newly observed turns: edits and unchanged history must not undo
// a later recipient choice. The existing snapshot is the sole history store.
function selectPictureCharacter(state: AppState, snapshot: SessionSnapshot, opened: boolean) {
  let selected = opened ? null : state.pictureCharacterId;
  if (opened) {
    if (snapshot.default_character_id === '*') {
      selected = [...snapshot.transcript].reverse().find((entry) => entry.kind === 'character')?.participant_id ?? null;
    } else {
      selected = snapshot.default_character_id;
    }
  } else {
    const previousIds = new Set(state.sessionSnapshot?.transcript.map(({ id }) => id));
    for (const entry of snapshot.transcript) {
      if (previousIds.has(entry.id) || entry.request_id === undefined) continue;
      if (entry.kind === 'character' || entry.kind === 'error') selected = entry.participant_id;
      else if (entry.kind === 'human') selected = entry.addressed_to;
    }
    if (snapshot.default_character_id !== state.currentDefaultCharacterId
        && snapshot.default_character_id !== '*') selected = snapshot.default_character_id;
  }
  if (snapshot.generation.active && snapshot.generation.character_id) {
    selected = snapshot.generation.character_id;
  }
  return snapshot.characters.some(({ id }) => id === selected) ? selected : null;
}

function releaseClosedNote(previous: AppState, state: AppState, action: AppAction): AppState {
  const { notesVisible, chatVisible } = workspaceVisibility(state);
  if (state.activeNoteId === null || notesVisible) return state;
  const leftVisibleNote = workspaceVisibility(previous).notesVisible
    && action.type !== 'set-layout';
  if (chatVisible && !leftVisibleNote) return state;
  return { ...state, activeNoteId: null, noteDestination: undefined };
}

function reduceApp(state: AppState, action: AppAction): AppState {
  switch (action.type) {
    case 'set-layout':
      return state.layoutMode === action.mode ? state : { ...state, layoutMode: action.mode };
    case 'bootstrap-started':
      return {
        ...state,
        bootstrapStatus: 'loading',
        bootstrapMessage: null,
      };
    case 'bootstrap-loaded':
      return showInitialConversation({
        ...state,
        bootstrapStatus: 'ready',
        bootstrap: action.bootstrap,
        bootstrapMessage: null,
      }, action.bootstrap);
    case 'bootstrap-failed':
      return {
        ...state,
        bootstrapStatus: action.incompatible ? 'incompatible' : 'failed',
        bootstrapMessage: action.message,
      };
    case 'bootstrap-refreshed':
      return { ...state, bootstrap: action.bootstrap };
    case 'vault-context-reset':
      return {
        ...state,
        activeConversation: null,
        activeConversationLabel: null,
        currentForumId: null,
        currentDefaultCharacterId: null,
        sessionSnapshot: null,
        pictureCharacterId: null,
        streamStatus: 'idle',
        streamMessage: null,
        activeNoteId: null, noteDestination: undefined,
        notesEpoch: state.notesEpoch + 1,
        ...idleSessionOperation(),
      };
    case 'vault-context-refreshed': {
      const updated = state.sessionSnapshot
        ? state : showInitialConversation(state, action.bootstrap);
      return {
        ...updated,
        mainView: state.mainView,
        bootstrap: action.bootstrap,
        inspectedVaultName: state.mainView === 'settings-vault'
          && state.inspectedVaultName === state.bootstrap?.vault_name
          ? action.bootstrap.vault_name : state.inspectedVaultName,
      };
    }
    case 'toggle-picture':
      return { ...state, pictureOpen: !state.pictureOpen };
    case 'toggle-chat-toolbar':
      return { ...state, chatToolbarVisible: !state.chatToolbarVisible };
    case 'toggle-notes-toolbar':
      return { ...state, notesToolbarVisible: !state.notesToolbarVisible };
    case 'toggle-notes-prompt':
      return { ...state, notesPromptVisible: !state.notesPromptVisible };
    case 'resize-picture':
      return { ...state, pictureWidth: action.width };
    case 'select-picture-character':
      return {
        ...state,
        pictureCharacterId: state.sessionSnapshot?.characters.some(({ id }) => id === action.characterId)
          ? action.characterId : null,
      };
    case 'toggle-sidebar':
      return { ...state, sidebarOpen: !state.sidebarOpen };
    case 'show-chat-tab':
      return { ...state, sidebarTab: 'chat' };
    case 'show-notes-tab':
      return { ...state, sidebarTab: 'notes' };
    case 'open-note': {
      // The visible chat panel keeps its pending operation.
      const { notesVisible, chatVisible } = workspaceVisibility(state);
      return {
        ...state,
        mainView: 'notes',
        activeNoteId: action.noteId,
        noteDestination: action.destination,
        noteRequest: state.noteRequest + 1,
        ...(notesVisible && chatVisible ? {} : idleSessionOperation()),
      };
    }
    case 'close-note':
      return { ...state, activeNoteId: null, noteDestination: undefined };
    case 'show-personas':
      return {
        ...state,
        mainView: 'personas',
        inspectedPersona: { id: null, writable: false },
        ...idleSessionOperation(),
      };
    case 'show-new-persona':
      return { ...state, mainView: 'new-persona', ...idleSessionOperation() };
    case 'inspect-persona':
      return {
        ...state,
        mainView: 'persona-detail',
        inspectedPersona: {
          id: action.personaId,
          writable: action.personaId === state.inspectedPersona.id
            ? state.inspectedPersona.writable
            : false,
        },
        ...idleSessionOperation(),
      };
    case 'persona-detail-loaded':
      if (state.inspectedPersona.id !== action.personaId) return state;
      return {
        ...state,
        inspectedPersona: { ...state.inspectedPersona, writable: action.writable },
      };
    case 'show-persona-settings':
      return state.inspectedPersona.id
        ? { ...state, mainView: 'persona-settings', ...idleSessionOperation() }
        : state;
    case 'persona-created': {
      if (!state.bootstrap) return state;
      const persona = action.persona;
      const personas = [
        ...state.bootstrap.personas,
        rosterSummary(persona),
      ].sort((left, right) => left.display_name.localeCompare(right.display_name));
      return {
        ...state,
        mainView: 'persona-detail',
        bootstrap: { ...state.bootstrap, personas },
        inspectedPersona: { id: persona.id, writable: persona.writable },
        ...idleSessionOperation(),
      };
    }
    case 'persona-updated':
      return applyPersonaUpdate(state, action.persona);
    case 'persona-deleted':
      return {
        ...state,
        mainView: 'personas',
        bootstrap: state.bootstrap ? {
          ...state.bootstrap,
          personas: state.bootstrap.personas.filter(({ id }) => id !== action.personaId),
        } : null,
        inspectedPersona: { id: null, writable: false },
        ...idleSessionOperation(),
      };
    case 'show-characters':
      return {
        ...state,
        mainView: 'characters',
        inspectedCharacter: {
          id: null,
          file: null,
          settingsWritable: false,
          writable: false,
        },
        ...idleSessionOperation(),
      };
    case 'show-character-instructions':
      return { ...state, mainView: 'character-instructions', ...idleSessionOperation() };
    case 'show-new-character':
      return { ...state, mainView: 'new-character', ...idleSessionOperation() };
    case 'inspect-character':
      return {
        ...state,
        mainView: 'character-detail',
        inspectedCharacter: {
          id: action.characterId,
          file: null,
          settingsWritable: action.characterId === state.inspectedCharacter.id
            ? state.inspectedCharacter.settingsWritable
            : false,
          writable: action.characterId === state.inspectedCharacter.id
            ? state.inspectedCharacter.writable
            : false,
        },
        ...idleSessionOperation(),
      };
    case 'inspect-character-file':
      return state.inspectedCharacter.id === action.characterId
        ? {
          ...state,
          mainView: 'character-file',
          inspectedCharacter: { ...state.inspectedCharacter, file: action.filename },
          ...idleSessionOperation(),
        }
        : state;
    case 'show-new-character-file':
      return state.inspectedCharacter.id && state.inspectedCharacter.settingsWritable
        ? { ...state, mainView: 'new-character-file', ...idleSessionOperation() }
        : state;
    case 'character-detail-loaded':
      // A reply for a character the reader has already left must not decide
      // whether the one now on screen offers its settings.
      if (action.characterId !== state.inspectedCharacter.id) return state;
      return {
        ...state,
        inspectedCharacter: {
          ...state.inspectedCharacter,
          settingsWritable: action.settingsWritable,
          writable: action.writable,
        },
      };
    case 'character-created': {
      if (!state.bootstrap) return state;
      const character = action.character;
      const characters = [
        ...state.bootstrap.characters,
        rosterSummary(character),
      ].sort((left, right) => left.display_name.localeCompare(right.display_name));
      return {
        ...state,
        mainView: 'character-detail',
        bootstrap: { ...state.bootstrap, characters },
        inspectedCharacter: {
          id: character.id,
          file: null,
          settingsWritable: character.settings_writable,
          writable: character.writable,
        },
        ...idleSessionOperation(),
      };
    }
    case 'character-updated':
      return applyCharacterUpdate(state, action.character);
    case 'character-deleted':
      return {
        ...state,
        mainView: 'characters',
        bootstrap: state.bootstrap ? {
          ...state.bootstrap,
          characters: state.bootstrap.characters.filter(
            ({ id }) => id !== action.characterId,
          ),
        } : null,
        inspectedCharacter: {
          id: null,
          file: null,
          settingsWritable: false,
          writable: false,
        },
        ...idleSessionOperation(),
      };
    case 'show-character-settings':
      return { ...state, mainView: 'character-settings', ...idleSessionOperation() };
    case 'show-character-files':
      return state.inspectedCharacter.id
        ? { ...state, mainView: 'character-files', inspectedCharacter: { ...state.inspectedCharacter, file: null }, ...idleSessionOperation() }
        : state;
    case 'show-forums':
      return { ...state, mainView: 'forums', ...idleSessionOperation() };
    case 'show-new-forum':
      return { ...state, mainView: 'new-forum', ...idleSessionOperation() };
    case 'forum-created': {
      if (!state.bootstrap) return state;
      const summary = forumSummary(action.forum);
      const forums = [...state.bootstrap.forums, summary].sort(
        (left, right) => left.display_name.localeCompare(right.display_name),
      );
      return {
        ...state,
        mainView: 'forum-detail',
        bootstrap: { ...state.bootstrap, forums },
        currentForumId: summary.id,
        inspectedForum: { file: null, writable: action.forum.writable },
        ...idleSessionOperation(),
      };
    }
    case 'select-forum':
      return {
        ...state,
        mainView: 'sessions',
        currentForumId: action.forumId,
        inspectedForum: {
          file: null,
          writable: action.forumId === state.currentForumId
            ? state.inspectedForum.writable
            : false,
        },
        ...idleSessionOperation(),
      };
    case 'show-sessions':
      return { ...state, mainView: 'sessions', ...idleSessionOperation() };
    // The detail always describes the current forum: it is reachable only from
    // that forum's Sessions screen, so it needs no subject of its own.
    case 'show-forum-detail':
      return {
        ...state,
        mainView: 'forum-detail',
        inspectedForum: { ...state.inspectedForum, file: null },
        ...idleSessionOperation(),
      };
    case 'inspect-forum-file':
      return state.currentForumId === action.forumId
        ? {
          ...state,
          mainView: 'forum-file',
          inspectedForum: { ...state.inspectedForum, file: action.filename },
          ...idleSessionOperation(),
        }
        : state;
    case 'show-new-forum-file':
      return state.currentForumId && state.inspectedForum.writable
        ? { ...state, mainView: 'new-forum-file', ...idleSessionOperation() }
        : state;
    case 'show-forum-members':
      return { ...state, mainView: 'forum-members', ...idleSessionOperation() };
    case 'forum-detail-loaded':
      if (state.currentForumId !== action.forumId) return state;
      return {
        ...state,
        inspectedForum: { ...state.inspectedForum, writable: action.writable },
      };
    case 'forum-updated':
      return applyForumUpdate(state, action.forum);
    case 'forum-deleted': {
      const activeDeleted = state.activeConversation?.forumId === action.forumId;
      return {
        ...state,
        mainView: 'forums',
        bootstrap: state.bootstrap ? {
          ...state.bootstrap,
          forums: state.bootstrap.forums.filter(({ id }) => id !== action.forumId),
          recent_sessions: state.bootstrap.recent_sessions.filter(
            ({ forum_id }) => forum_id !== action.forumId,
          ),
        } : null,
        currentForumId: null,
        inspectedForum: { file: null, writable: false },
        ...(activeDeleted ? {
          activeConversation: null,
          activeConversationLabel: null,
          currentDefaultCharacterId: null,
          sessionSnapshot: null,
          pictureCharacterId: null,
          streamStatus: 'idle' as const,
          streamMessage: null,
        } : {}),
        ...idleSessionOperation(),
      };
    }
    case 'show-settings':
      return { ...state, mainView: 'settings', ...idleSessionOperation() };
    case 'show-settings-vaults':
      return {
        ...state,
        mainView: 'settings-vaults',
        inspectedVaultName: null,
        ...idleSessionOperation(),
      };
    case 'show-settings-new-vault':
      return { ...state, mainView: 'settings-new-vault', ...idleSessionOperation() };
    case 'show-settings-download-vault':
      return { ...state, mainView: 'settings-download-vault', ...idleSessionOperation() };
    case 'show-settings-merge-vault':
      return { ...state, mainView: 'settings-merge-vault', ...idleSessionOperation() };
    case 'inspect-vault':
      return {
        ...state,
        mainView: 'settings-vault',
        inspectedVaultName: action.vaultName,
        ...idleSessionOperation(),
      };
    case 'vault-created':
      return {
        ...state,
        mainView: 'settings-vault',
        inspectedVaultName: action.vault.display_name,
        bootstrap: state.bootstrap ? {
          ...state.bootstrap,
          vaults: [...state.bootstrap.vaults, action.vault.display_name]
            .sort((left, right) => left.localeCompare(right)),
        } : null,
        ...idleSessionOperation(),
      };
    case 'vault-downloaded':
      return {
        ...state,
        bootstrap: state.bootstrap ? {
          ...state.bootstrap,
          vaults: [...state.bootstrap.vaults, action.vault.display_name]
            .sort((left, right) => left.localeCompare(right)),
        } : null,
        ...idleSessionOperation(),
      };
    case 'vault-updated':
      return {
        ...state,
        inspectedVaultName: action.vault.display_name,
        bootstrap: state.bootstrap ? {
          ...state.bootstrap,
          vault_name: state.bootstrap.vault_name === action.previousName
            ? action.vault.display_name : state.bootstrap.vault_name,
          vaults: state.bootstrap.vaults.map((name) => (
            name === action.previousName ? action.vault.display_name : name
          )).sort((left, right) => left.localeCompare(right)),
        } : null,
      };
    case 'vault-deleted':
      return {
        ...state,
        mainView: 'settings-vaults',
        inspectedVaultName: null,
        bootstrap: state.bootstrap ? {
          ...state.bootstrap,
          vaults: state.bootstrap.vaults.filter((name) => name !== action.vaultName),
        } : null,
        ...idleSessionOperation(),
      };
    case 'show-settings-providers':
      return {
        ...state,
        mainView: 'settings-providers',
        inspectedProvider: { ...state.inspectedProvider, writable: false },
        ...idleSessionOperation(),
      };
    case 'show-settings-new-provider':
      return { ...state, mainView: 'settings-new-provider', ...idleSessionOperation() };
    case 'inspect-provider':
      return {
        ...state,
        mainView: 'settings-provider',
        inspectedProvider: {
          id: action.providerId,
          name: action.providerName,
          writable: action.providerId === state.inspectedProvider.id
            ? state.inspectedProvider.writable
            : false,
        },
        ...idleSessionOperation(),
      };
    case 'provider-detail-loaded':
    case 'provider-updated':
      if (action.providerId !== state.inspectedProvider.id) return state;
      return {
        ...state,
        inspectedProvider: {
          ...state.inspectedProvider,
          name: action.providerName,
          writable: action.writable,
        },
      };
    case 'show-settings-styles':
      return {
        ...state,
        mainView: 'settings-styles',
        inspectedStyle: { ...state.inspectedStyle, writable: false },
        ...idleSessionOperation(),
      };
    case 'show-settings-new-style':
      return { ...state, mainView: 'settings-new-style', ...idleSessionOperation() };
    case 'inspect-style':
      return {
        ...state,
        mainView: 'settings-style',
        inspectedStyle: {
          id: action.styleId,
          name: action.styleName,
          writable: action.styleId === state.inspectedStyle.id
            ? state.inspectedStyle.writable
            : false,
        },
        ...idleSessionOperation(),
      };
    case 'style-detail-loaded':
    case 'style-updated':
      if (action.styleId !== state.inspectedStyle.id) return state;
      return {
        ...state,
        inspectedStyle: {
          ...state.inspectedStyle,
          name: action.styleName,
          writable: action.writable,
        },
      };
    case 'show-settings-voices':
      return {
        ...state,
        mainView: 'settings-voices',
        inspectedVoice: { ...state.inspectedVoice, writable: false },
        ...idleSessionOperation(),
      };
    case 'show-settings-jev':
      return { ...state, mainView: 'settings-jev', ...idleSessionOperation() };
    case 'show-settings-events':
      return { ...state, mainView: 'settings-events', ...idleSessionOperation() };
    case 'show-settings-notes':
      return { ...state, mainView: 'settings-notes', ...idleSessionOperation() };
    case 'show-settings-web-search':
      return { ...state, mainView: 'settings-web-search', ...idleSessionOperation() };
    case 'show-settings-voice-input':
      return {
        ...state,
        mainView: 'settings-voice-input',
        ...idleSessionOperation(),
      };
    case 'show-settings-new-voice':
      return { ...state, mainView: 'settings-new-voice', ...idleSessionOperation() };
    case 'inspect-voice':
      return {
        ...state,
        mainView: 'settings-voice',
        inspectedVoice: {
          id: action.voiceId,
          name: action.voiceName,
          writable: action.voiceId === state.inspectedVoice.id
            ? state.inspectedVoice.writable
            : false,
        },
        ...idleSessionOperation(),
      };
    case 'voice-detail-loaded':
    case 'voice-updated':
      if (action.voiceId !== state.inspectedVoice.id) return state;
      return {
        ...state,
        inspectedVoice: {
          ...state.inspectedVoice,
          name: action.voiceName,
          writable: action.writable,
        },
      };
    case 'show-settings-api-keys':
      return { ...state, mainView: 'settings-api-keys', ...idleSessionOperation() };
    case 'show-settings-new-api-key':
      return { ...state, mainView: 'settings-new-api-key', ...idleSessionOperation() };
    case 'inspect-api-key':
      return {
        ...state,
        mainView: 'settings-api-key',
        inspectedApiKey: { id: action.apiKeyId, name: action.apiKeyName },
        ...idleSessionOperation(),
      };
    case 'api-key-detail-loaded':
    case 'api-key-updated':
      if (action.apiKeyId !== state.inspectedApiKey.id) return state;
      return { ...state, inspectedApiKey: { ...state.inspectedApiKey, name: action.apiKeyName } };
    case 'show-settings-r2-storage':
      return { ...state, mainView: 'settings-r2-storage', ...idleSessionOperation() };
    case 'show-chat':
      return { ...state, mainView: 'chat', ...idleSessionOperation() };
    case 'session-operation-started':
      return {
        ...state,
        mainView: state.mainView === 'notes' ? 'notes' : 'chat',
        sessionOperation: 'pending',
        sessionOperationMessage: action.message,
        sessionOperationRetryable: false,
      };
    case 'session-operation-failed':
      return {
        ...state,
        mainView: state.mainView === 'notes' ? 'notes' : 'chat',
        sessionOperation: 'failed',
        sessionOperationMessage: action.message,
        sessionOperationRetryable: action.retryable ?? false,
      };
    case 'session-discarded': {
      if (state.activeConversation?.forumId !== action.forumId
          || state.activeConversation.sessionId !== action.sessionId) return state;
      return {
        ...state,
        bootstrap: state.bootstrap
            && state.bootstrap.initial_forum_id === action.forumId
            && state.bootstrap.initial_session_id === action.sessionId
          ? {
            ...state.bootstrap,
            initial_forum_id: state.bootstrap.entrance_forum_id,
            initial_session_id: welcomeSessionId,
          }
          : state.bootstrap,
        activeConversation: null,
        activeConversationLabel: null,
        sessionSnapshot: null,
        pictureCharacterId: null,
        ...idleSessionOperation(),
      };
    }
    case 'conversation-opened':
      return {
        ...state,
        mainView: action.navigate === false ? state.mainView : 'chat',
        currentForumId: action.snapshot.forum.id,
        inspectedForum: { ...state.inspectedForum, file: null },
        activeConversation: {
          forumId: action.snapshot.forum.id,
          sessionId: action.snapshot.session_id,
        },
        activeConversationLabel: action.snapshot.session_label,
        currentDefaultCharacterId: action.snapshot.default_character_id,
        sessionSnapshot: action.snapshot,
        pictureCharacterId: selectPictureCharacter(state, action.snapshot, true),
        streamStatus: 'connecting',
        streamMessage: 'Connecting live updates…',
        ...idleSessionOperation(),
      };
    case 'session-snapshot':
      if (state.activeConversation?.forumId !== action.snapshot.forum.id
          || state.activeConversation.sessionId !== action.snapshot.session_id) {
        return state;
      }
      return {
        ...state,
        bootstrap: state.bootstrap && state.bootstrap.recent_sessions.some((session) => (
          session.forum_id === action.snapshot.forum.id
            && session.session_id === action.snapshot.session_id
            && session.session_label !== action.snapshot.session_label
        ))
          ? {
            ...state.bootstrap,
            recent_sessions: state.bootstrap.recent_sessions.map((session) => (
              session.forum_id === action.snapshot.forum.id
                && session.session_id === action.snapshot.session_id
                ? { ...session, session_label: action.snapshot.session_label }
                : session
            )),
          }
          : state.bootstrap,
        activeConversationLabel: action.snapshot.session_label,
        currentDefaultCharacterId: action.snapshot.default_character_id,
        sessionSnapshot: action.snapshot,
        pictureCharacterId: selectPictureCharacter(state, action.snapshot, false),
      };
    case 'session-audio-cache':
      if (!state.sessionSnapshot
          || state.sessionSnapshot.forum.id !== action.forumId
          || state.sessionSnapshot.session_id !== action.sessionId) return state;
      return {
        ...state,
        audioCacheClearCount: !action.cached && action.entryId === undefined
          ? state.audioCacheClearCount + 1 : state.audioCacheClearCount,
        sessionSnapshot: {
          ...state.sessionSnapshot,
          transcript: state.sessionSnapshot.transcript.map((entry) => (
            action.entryId === undefined || entry.id === action.entryId
              ? { ...entry, has_cached_audio: action.cached }
              : entry
          )),
        },
      };
    case 'session-append':
    case 'session-generation':
      if (!state.sessionSnapshot
          || state.activeConversation?.forumId !== action.forumId
          || state.activeConversation.sessionId !== action.sessionId
          || state.sessionSnapshot.forum.id !== action.forumId
          || state.sessionSnapshot.session_id !== action.sessionId) {
        return state;
      }
      return {
        ...state,
        sessionSnapshot: action.type === 'session-append'
          ? appendSessionEvent(state.sessionSnapshot, action.event)
          : { ...state.sessionSnapshot, generation: action.generation },
      };
    case 'show-initial-conversation':
      return state.bootstrap ? showInitialConversation(state, state.bootstrap) : state;
    case 'reset-active-conversation':
      return state.bootstrap
        ? { ...showInitialConversation(state, state.bootstrap), mainView: state.mainView }
        : state;
    case 'session-activity-ended':
    case 'session-deleted':
      return state;
    case 'stream-state':
      return {
        ...state,
        streamStatus: action.status,
        streamMessage: action.message ?? null,
      };
  }
}

function sameSession(session: SessionActivity, forumId: string, sessionId: string) {
  return session.forumId === forumId && session.sessionId === sessionId;
}

// Sidebar activity of every session this page has followed. Reading an open
// conversation acknowledges its finished response. Notes and Settings keep it.
function reduceSessionActivity(state: AppState, action: AppAction): readonly SessionActivity[] {
  let activity = state.sessionActivity;
  switch (action.type) {
    case 'vault-context-reset':
      return [];
    case 'forum-deleted':
      activity = activity.filter((session) => session.forumId !== action.forumId);
      break;
    case 'session-discarded':
    case 'session-deleted':
      activity = activity.filter((session) => !sameSession(session, action.forumId, action.sessionId));
      break;
    case 'session-activity-ended':
      activity = activity.map((session) => (
        sameSession(session, action.forumId, action.sessionId) && session.status === 'generating'
          ? { ...session, status: 'finished' } : session
      ));
      break;
    case 'conversation-opened':
    case 'session-snapshot':
    case 'session-generation': {
      const forumId = action.type === 'session-generation' ? action.forumId : action.snapshot.forum.id;
      const sessionId = action.type === 'session-generation' ? action.sessionId : action.snapshot.session_id;
      const generating = action.type === 'session-generation'
        ? action.generation.active : action.snapshot.generation.active;
      const existing = activity.find((session) => sameSession(session, forumId, sessionId));
      const status = generating ? 'generating' : existing && 'finished';
      if (!status || status === existing?.status) break;
      activity = existing
        ? activity.map((session) => (session === existing ? { ...session, status } : session))
        : [...activity, { forumId, sessionId, status }];
      break;
    }
  }
  const active = state.activeConversation;
  if (!active || !workspaceVisibility(state).chatVisible || state.sessionOperation !== 'idle') return activity;
  return activity.filter((session) => (
    session.status !== 'finished' || !sameSession(session, active.forumId, active.sessionId)
  ));
}

export function appReducer(state: AppState, action: AppAction): AppState {
  const next = releaseClosedNote(state, reduceApp(state, action), action);
  const activity = reduceSessionActivity(next, action);
  // Keep the same array when nothing changed. Background checks restart on a new one.
  if (activity.length === next.sessionActivity.length
      && activity.every((session, index) => session === next.sessionActivity[index])) return next;
  return { ...next, sessionActivity: activity };
}

export function navigationTitle(state: AppState): string | null {
  switch (state.mainView) {
    case 'personas': return 'Personas';
    case 'new-persona': return 'New persona';
    case 'persona-settings': return 'Settings';
    case 'characters': return 'Characters';
    case 'character-instructions': return 'Instructions';
    case 'new-character': return 'New character';
    case 'character-settings': return 'Character settings';
    case 'character-files': return 'Files';
    case 'character-file': return state.inspectedCharacter.file ?? 'File';
    case 'new-character-file': return 'New file';
    case 'forums': return 'Forums';
    case 'new-forum': return 'New forum';
    case 'sessions': return 'Sessions';
    case 'forum-file': return state.inspectedForum.file ?? 'File';
    case 'new-forum-file': return 'New file';
    case 'forum-members': return 'Members';
    case 'settings': return 'Settings';
    case 'settings-vaults': return 'Vaults';
    case 'settings-new-vault': return 'New vault';
    case 'settings-download-vault': return 'Download vault';
    case 'settings-merge-vault': return 'Merge vault';
    case 'settings-providers': return 'Providers';
    case 'settings-new-provider': return 'New provider';
    case 'settings-styles': return 'Styles';
    case 'settings-new-style': return 'New style';
    case 'settings-voices': return 'Voices';
    case 'settings-jev': return 'Session settings';
    case 'settings-events': return 'Events';
    case 'settings-notes': return 'Notes settings';
    case 'settings-web-search': return 'Search API';
    case 'settings-voice-input': return 'Voice settings';
    case 'settings-new-voice': return 'New voice';
    case 'settings-api-keys': return 'API Keys';
    case 'settings-new-api-key': return 'New API key';
    case 'settings-r2-storage': return 'R2 storage';
    case 'persona-detail':
    case 'character-detail':
    case 'forum-detail':
    case 'settings-vault':
    case 'settings-provider':
    case 'settings-style':
    case 'settings-voice':
    case 'settings-api-key':
    case 'chat':
    case 'notes': return null;
  }
}
