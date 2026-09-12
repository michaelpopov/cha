import type {
  Bootstrap,
  CharacterDetail,
  ForumDetail,
  PersonaDetail,
  SessionSnapshot,
  VaultDetail,
} from '../api/client';
import type { AppendEvent } from '../api/events';

export type MainView =
  | 'chat'
  | 'personas'
  | 'new-persona'
  | 'persona-detail'
  | 'characters'
  | 'new-character'
  | 'character-detail'
  | 'character-settings'
  | 'forums'
  | 'new-forum'
  | 'sessions'
  | 'forum-detail'
  | 'forum-members'
  | 'new-session'
  | 'settings'
  | 'settings-vaults'
  | 'settings-new-vault'
  | 'settings-download-vault'
  | 'settings-vault'
  | 'settings-providers'
  | 'settings-new-provider'
  | 'settings-provider'
  | 'settings-styles'
  | 'settings-new-style'
  | 'settings-style'
  | 'settings-voices'
  | 'settings-new-voice'
  | 'settings-voice'
  | 'settings-api-keys'
  | 'settings-new-api-key'
  | 'settings-api-key'
  | 'settings-r2-storage';

export type BootstrapStatus = 'loading' | 'ready' | 'failed' | 'incompatible';
export type StreamStatus =
  | 'idle'
  | 'connecting'
  | 'connected'
  | 'reconnecting'
  | 'retry'
  // The reader opened this session on another device, which now holds it.
  | 'moved';

export interface ActiveConversation {
  forumId: string;
  sessionId: string;
}

export interface AppState {
  sidebarOpen: boolean;
  mainView: MainView;
  bootstrapStatus: BootstrapStatus;
  bootstrap: Bootstrap | null;
  bootstrapMessage: string | null;
  currentForumId: string | null;
  activeConversation: ActiveConversation | null;
  inspectedCharacterId: string | null;
  characterSettingsAvailable: boolean;
  inspectedPersonaId: string | null;
  personaEditingAvailable: boolean;
  forumEditingAvailable: boolean;
  inspectedVaultName: string | null;
  inspectedProviderId: string | null;
  inspectedProviderName: string | null;
  providerEditingAvailable: boolean;
  inspectedStyleId: string | null;
  inspectedStyleName: string | null;
  styleEditingAvailable: boolean;
  inspectedVoiceId: string | null;
  inspectedVoiceName: string | null;
  voiceEditingAvailable: boolean;
  inspectedApiKeyId: string | null;
  inspectedApiKeyName: string | null;
  currentDefaultCharacterId: string | null;
  sessionOperation: 'idle' | 'pending' | 'failed';
  sessionOperationMessage: string | null;
  sessionOperationRetryable: boolean;
  activeConversationLabel: string | null;
  sessionSnapshot: SessionSnapshot | null;
  streamStatus: StreamStatus;
  streamMessage: string | null;
}

export const initialAppState: AppState = {
  sidebarOpen: true,
  mainView: 'chat',
  bootstrapStatus: 'loading',
  bootstrap: null,
  bootstrapMessage: null,
  currentForumId: null,
  activeConversation: null,
  inspectedCharacterId: null,
  characterSettingsAvailable: false,
  inspectedPersonaId: null,
  personaEditingAvailable: false,
  forumEditingAvailable: false,
  inspectedVaultName: null,
  inspectedProviderId: null,
  inspectedProviderName: null,
  providerEditingAvailable: false,
  inspectedStyleId: null,
  inspectedStyleName: null,
  styleEditingAvailable: false,
  inspectedVoiceId: null,
  inspectedVoiceName: null,
  voiceEditingAvailable: false,
  inspectedApiKeyId: null,
  inspectedApiKeyName: null,
  currentDefaultCharacterId: null,
  sessionOperation: 'idle',
  sessionOperationMessage: null,
  sessionOperationRetryable: false,
  activeConversationLabel: null,
  sessionSnapshot: null,
  streamStatus: 'idle',
  streamMessage: null,
};

export type AppAction =
  | { type: 'bootstrap-started' }
  | { type: 'bootstrap-loaded'; bootstrap: Bootstrap }
  | { type: 'bootstrap-failed'; message: string; incompatible: boolean }
  | { type: 'bootstrap-refreshed'; bootstrap: Bootstrap }
  | { type: 'toggle-sidebar' }
  | { type: 'show-personas' }
  | { type: 'show-new-persona' }
  | { type: 'inspect-persona'; personaId: string }
  | { type: 'persona-detail-loaded'; personaId: string; writable: boolean }
  | { type: 'persona-created'; persona: PersonaDetail }
  | { type: 'persona-updated'; persona: PersonaDetail }
  | { type: 'persona-deleted'; personaId: string }
  | { type: 'show-characters' }
  | { type: 'show-new-character' }
  | { type: 'inspect-character'; characterId: string }
  | { type: 'character-detail-loaded'; characterId: string; writable: boolean }
  | { type: 'character-created'; character: CharacterDetail }
  | { type: 'character-updated'; character: CharacterDetail }
  | { type: 'character-deleted'; characterId: string }
  | { type: 'show-character-settings' }
  | { type: 'show-forums' }
  | { type: 'show-new-forum' }
  | { type: 'forum-created'; forum: ForumDetail }
  | { type: 'select-forum'; forumId: string }
  | { type: 'show-sessions' }
  | { type: 'show-forum-detail' }
  | { type: 'show-forum-members' }
  | { type: 'forum-detail-loaded'; forumId: string; writable: boolean }
  | { type: 'forum-updated'; forum: ForumDetail }
  | { type: 'forum-deleted'; forumId: string }
  | { type: 'show-new-session' }
  | { type: 'show-settings' }
  | { type: 'show-settings-vaults' }
  | { type: 'show-settings-new-vault' }
  | { type: 'show-settings-download-vault' }
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
  | { type: 'conversation-opened'; snapshot: SessionSnapshot }
  | { type: 'session-snapshot'; snapshot: SessionSnapshot }
  | { type: 'session-append'; forumId: string; sessionId: string; event: AppendEvent }
  | { type: 'show-initial-conversation' }
  | { type: 'stream-state'; status: StreamStatus; message?: string };

function idleSessionOperation() {
  return {
    sessionOperation: 'idle' as const,
    sessionOperationMessage: null,
    sessionOperationRetryable: false,
  };
}

// The startup conversation and Return to Welcome land on the same place: the
// initial session named by bootstrap, in the forum that owns it.
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
  if (target.kind === 'entry') {
    const entryIndex = snapshot.transcript.findIndex(({ id }) => id === target.entry_id);
    if (entryIndex < 0) return snapshot;
    const transcript = [...snapshot.transcript];
    transcript[entryIndex] = {
      ...transcript[entryIndex],
      text: transcript[entryIndex].text + event.text,
    };
    return { ...snapshot, transcript };
  }

  if (snapshot.generation.request_id !== target.request_id) return snapshot;
  return {
    ...snapshot,
    generation: {
      ...snapshot.generation,
      reasoning_text: snapshot.generation.reasoning_text + event.text,
    },
  };
}

export function appReducer(state: AppState, action: AppAction): AppState {
  switch (action.type) {
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
    case 'toggle-sidebar':
      return { ...state, sidebarOpen: !state.sidebarOpen };
    case 'show-personas':
      return {
        ...state,
        mainView: 'personas',
        inspectedPersonaId: null,
        personaEditingAvailable: false,
        ...idleSessionOperation(),
      };
    case 'show-new-persona':
      return { ...state, mainView: 'new-persona', ...idleSessionOperation() };
    case 'inspect-persona':
      return {
        ...state,
        mainView: 'persona-detail',
        inspectedPersonaId: action.personaId,
        personaEditingAvailable: action.personaId === state.inspectedPersonaId
          ? state.personaEditingAvailable
          : false,
        ...idleSessionOperation(),
      };
    case 'persona-detail-loaded':
      if (state.inspectedPersonaId !== action.personaId) return state;
      return { ...state, personaEditingAvailable: action.writable };
    case 'persona-created': {
      if (!state.bootstrap) return state;
      const persona = action.persona;
      const personas = [
        ...state.bootstrap.personas,
        {
          id: persona.id,
          display_name: persona.display_name,
          ...(persona.description === undefined
            ? {} : { description: persona.description }),
        },
      ].sort((left, right) => left.display_name.localeCompare(right.display_name));
      return {
        ...state,
        mainView: 'persona-detail',
        bootstrap: { ...state.bootstrap, personas },
        inspectedPersonaId: persona.id,
        personaEditingAvailable: persona.writable,
        ...idleSessionOperation(),
      };
    }
    case 'persona-updated': {
      if (!state.bootstrap) return state;
      const persona = action.persona;
      const bootstrap = {
        ...state.bootstrap,
        personas: state.bootstrap.personas.map((current) => (
          current.id === persona.id
            ? {
              id: persona.id,
              display_name: persona.display_name,
              ...(persona.description === undefined
                ? {} : { description: persona.description }),
            }
            : current
        )),
        forums: state.bootstrap.forums.map((forum) => (
          forum.default_persona_id === persona.id
            ? { ...forum, default_persona_display_name: persona.display_name }
            : forum
        )),
      };
      const sessionSnapshot = state.sessionSnapshot
        && state.sessionSnapshot.forum.default_persona_id === persona.id
        ? {
          ...state.sessionSnapshot,
          forum: {
            ...state.sessionSnapshot.forum,
            default_persona_display_name: persona.display_name,
          },
        }
        : state.sessionSnapshot;
      return { ...state, bootstrap, sessionSnapshot };
    }
    case 'persona-deleted':
      return {
        ...state,
        mainView: 'personas',
        bootstrap: state.bootstrap ? {
          ...state.bootstrap,
          personas: state.bootstrap.personas.filter(({ id }) => id !== action.personaId),
        } : null,
        inspectedPersonaId: null,
        personaEditingAvailable: false,
        ...idleSessionOperation(),
      };
    case 'show-characters':
      return {
        ...state,
        mainView: 'characters',
        inspectedCharacterId: null,
        characterSettingsAvailable: false,
        ...idleSessionOperation(),
      };
    case 'show-new-character':
      return { ...state, mainView: 'new-character', ...idleSessionOperation() };
    case 'inspect-character':
      return {
        ...state,
        mainView: 'character-detail',
        inspectedCharacterId: action.characterId,
        characterSettingsAvailable: action.characterId === state.inspectedCharacterId
          ? state.characterSettingsAvailable
          : false,
        ...idleSessionOperation(),
      };
    case 'character-detail-loaded':
      // A reply for a character the reader has already left must not decide
      // whether the one now on screen offers its settings.
      if (action.characterId !== state.inspectedCharacterId) return state;
      return { ...state, characterSettingsAvailable: action.writable };
    case 'character-created': {
      if (!state.bootstrap) return state;
      const character = action.character;
      const characters = [
        ...state.bootstrap.characters,
        {
          id: character.id,
          display_name: character.display_name,
          ...(character.description === undefined
            ? {} : { description: character.description }),
          appearance: character.appearance,
          ...(character.voice === undefined ? {} : { voice: character.voice }),
        },
      ].sort((left, right) => left.display_name.localeCompare(right.display_name));
      return {
        ...state,
        mainView: 'character-detail',
        bootstrap: { ...state.bootstrap, characters },
        inspectedCharacterId: character.id,
        characterSettingsAvailable: character.writable,
        ...idleSessionOperation(),
      };
    }
    case 'character-updated': {
      if (!state.bootstrap) return state;
      const character = action.character;
      const summary = {
        id: character.id,
        display_name: character.display_name,
        ...(character.description === undefined
          ? {} : { description: character.description }),
        appearance: character.appearance,
        ...(character.voice === undefined ? {} : { voice: character.voice }),
      };
      const updateMembers = <T extends { id: string }>(members: T[]) => (
        members.map((member) => member.id === character.id
          ? { ...member, ...summary }
          : member)
      );
      const bootstrap = {
        ...state.bootstrap,
        characters: updateMembers(state.bootstrap.characters),
        forums: state.bootstrap.forums.map((forum) => ({
          ...forum,
          members: updateMembers(forum.members),
        })),
      };
      const sessionSnapshot = state.sessionSnapshot ? {
        ...state.sessionSnapshot,
        characters: updateMembers(state.sessionSnapshot.characters),
        forum: {
          ...state.sessionSnapshot.forum,
          members: updateMembers(state.sessionSnapshot.forum.members),
        },
        generation: state.sessionSnapshot.generation.character_id === character.id
          ? {
            ...state.sessionSnapshot.generation,
            character_display_name: character.display_name,
          }
          : state.sessionSnapshot.generation,
      } : null;
      return { ...state, bootstrap, sessionSnapshot };
    }
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
        inspectedCharacterId: null,
        characterSettingsAvailable: false,
        ...idleSessionOperation(),
      };
    case 'show-character-settings':
      return { ...state, mainView: 'character-settings', ...idleSessionOperation() };
    case 'show-forums':
      return { ...state, mainView: 'forums', ...idleSessionOperation() };
    case 'show-new-forum':
      return { ...state, mainView: 'new-forum', ...idleSessionOperation() };
    case 'forum-created': {
      if (!state.bootstrap) return state;
      const { forum_markdown: _markdown, writable, ...summary } = action.forum;
      const forums = [...state.bootstrap.forums, summary].sort(
        (left, right) => left.display_name.localeCompare(right.display_name),
      );
      return {
        ...state,
        mainView: 'forum-detail',
        bootstrap: { ...state.bootstrap, forums },
        currentForumId: summary.id,
        forumEditingAvailable: writable,
        ...idleSessionOperation(),
      };
    }
    case 'select-forum':
      return {
        ...state,
        mainView: 'sessions',
        currentForumId: action.forumId,
        forumEditingAvailable: action.forumId === state.currentForumId
          ? state.forumEditingAvailable
          : false,
        ...idleSessionOperation(),
      };
    case 'show-sessions':
      return { ...state, mainView: 'sessions', ...idleSessionOperation() };
    // The detail always describes the current forum: it is reachable only from
    // that forum's Sessions screen, so it needs no subject of its own.
    case 'show-forum-detail':
      return { ...state, mainView: 'forum-detail', ...idleSessionOperation() };
    case 'show-forum-members':
      return { ...state, mainView: 'forum-members', ...idleSessionOperation() };
    case 'forum-detail-loaded':
      if (state.currentForumId !== action.forumId) return state;
      return { ...state, forumEditingAvailable: action.writable };
    case 'forum-updated': {
      if (!state.bootstrap) return state;
      const { forum_markdown: _markdown, writable, ...summary } = action.forum;
      const bootstrap = {
        ...state.bootstrap,
        forums: state.bootstrap.forums.map((forum) => (
          forum.id === summary.id ? summary : forum
        )),
      };
      const sessionSnapshot = state.sessionSnapshot?.forum.id === summary.id
        ? {
          ...state.sessionSnapshot,
          forum: { ...state.sessionSnapshot.forum, ...summary },
        }
        : state.sessionSnapshot;
      return {
        ...state,
        bootstrap,
        sessionSnapshot,
        forumEditingAvailable: writable,
      };
    }
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
        forumEditingAvailable: false,
        ...(activeDeleted ? {
          activeConversation: null,
          activeConversationLabel: null,
          currentDefaultCharacterId: null,
          sessionSnapshot: null,
          streamStatus: 'idle' as const,
          streamMessage: null,
        } : {}),
        ...idleSessionOperation(),
      };
    }
    case 'show-new-session':
      return { ...state, mainView: 'new-session', ...idleSessionOperation() };
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
        providerEditingAvailable: false,
        ...idleSessionOperation(),
      };
    case 'show-settings-new-provider':
      return { ...state, mainView: 'settings-new-provider', ...idleSessionOperation() };
    case 'inspect-provider':
      return {
        ...state,
        mainView: 'settings-provider',
        inspectedProviderId: action.providerId,
        inspectedProviderName: action.providerName,
        providerEditingAvailable: action.providerId === state.inspectedProviderId
          ? state.providerEditingAvailable
          : false,
        ...idleSessionOperation(),
      };
    case 'provider-detail-loaded':
      if (action.providerId !== state.inspectedProviderId) return state;
      return {
        ...state,
        inspectedProviderName: action.providerName,
        providerEditingAvailable: action.writable,
      };
    case 'provider-updated':
      if (action.providerId !== state.inspectedProviderId) return state;
      return {
        ...state,
        inspectedProviderName: action.providerName,
        providerEditingAvailable: action.writable,
      };
    case 'show-settings-styles':
      return {
        ...state,
        mainView: 'settings-styles',
        styleEditingAvailable: false,
        ...idleSessionOperation(),
      };
    case 'show-settings-new-style':
      return { ...state, mainView: 'settings-new-style', ...idleSessionOperation() };
    case 'inspect-style':
      return {
        ...state,
        mainView: 'settings-style',
        inspectedStyleId: action.styleId,
        inspectedStyleName: action.styleName,
        styleEditingAvailable: action.styleId === state.inspectedStyleId
          ? state.styleEditingAvailable
          : false,
        ...idleSessionOperation(),
      };
    case 'style-detail-loaded':
      if (action.styleId !== state.inspectedStyleId) return state;
      return {
        ...state,
        inspectedStyleName: action.styleName,
        styleEditingAvailable: action.writable,
      };
    case 'style-updated':
      if (action.styleId !== state.inspectedStyleId) return state;
      return {
        ...state,
        inspectedStyleName: action.styleName,
        styleEditingAvailable: action.writable,
      };
    case 'show-settings-voices':
      return {
        ...state,
        mainView: 'settings-voices',
        voiceEditingAvailable: false,
        ...idleSessionOperation(),
      };
    case 'show-settings-new-voice':
      return { ...state, mainView: 'settings-new-voice', ...idleSessionOperation() };
    case 'inspect-voice':
      return {
        ...state,
        mainView: 'settings-voice',
        inspectedVoiceId: action.voiceId,
        inspectedVoiceName: action.voiceName,
        voiceEditingAvailable: action.voiceId === state.inspectedVoiceId
          ? state.voiceEditingAvailable
          : false,
        ...idleSessionOperation(),
      };
    case 'voice-detail-loaded':
    case 'voice-updated':
      if (action.voiceId !== state.inspectedVoiceId) return state;
      return {
        ...state,
        inspectedVoiceName: action.voiceName,
        voiceEditingAvailable: action.writable,
      };
    case 'show-settings-api-keys':
      return { ...state, mainView: 'settings-api-keys', ...idleSessionOperation() };
    case 'show-settings-new-api-key':
      return { ...state, mainView: 'settings-new-api-key', ...idleSessionOperation() };
    case 'inspect-api-key':
      return {
        ...state,
        mainView: 'settings-api-key',
        inspectedApiKeyId: action.apiKeyId,
        inspectedApiKeyName: action.apiKeyName,
        ...idleSessionOperation(),
      };
    case 'api-key-detail-loaded':
    case 'api-key-updated':
      if (action.apiKeyId !== state.inspectedApiKeyId) return state;
      return { ...state, inspectedApiKeyName: action.apiKeyName };
    case 'show-settings-r2-storage':
      return { ...state, mainView: 'settings-r2-storage', ...idleSessionOperation() };
    case 'show-chat':
      return { ...state, mainView: 'chat', ...idleSessionOperation() };
    case 'session-operation-started':
      return {
        ...state,
        sessionOperation: 'pending',
        sessionOperationMessage: action.message,
        sessionOperationRetryable: false,
      };
    case 'session-operation-failed':
      return {
        ...state,
        sessionOperation: 'failed',
        sessionOperationMessage: action.message,
        sessionOperationRetryable: action.retryable ?? false,
      };
    case 'conversation-opened':
      return {
        ...state,
        mainView: 'chat',
        currentForumId: action.snapshot.forum.id,
        activeConversation: {
          forumId: action.snapshot.forum.id,
          sessionId: action.snapshot.session_id,
        },
        activeConversationLabel: action.snapshot.session_label,
        currentDefaultCharacterId: action.snapshot.default_character_id,
        sessionSnapshot: action.snapshot,
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
        activeConversationLabel: action.snapshot.session_label,
        currentDefaultCharacterId: action.snapshot.default_character_id,
        sessionSnapshot: action.snapshot,
      };
    case 'session-append':
      if (!state.sessionSnapshot
          || state.activeConversation?.forumId !== action.forumId
          || state.activeConversation.sessionId !== action.sessionId
          || state.sessionSnapshot.forum.id !== action.forumId
          || state.sessionSnapshot.session_id !== action.sessionId) {
        return state;
      }
      return {
        ...state,
        sessionSnapshot: appendSessionEvent(state.sessionSnapshot, action.event),
      };
    case 'show-initial-conversation':
      return state.bootstrap ? showInitialConversation(state, state.bootstrap) : state;
    case 'stream-state':
      return {
        ...state,
        streamStatus: action.status,
        streamMessage: action.message ?? null,
      };
  }
}

// Pending disables the actions that would start a second operation; a failure
// carries the message the responsible view reports.
export function sessionOperationState(state: AppState): {
  pending: boolean;
  failure: string | null;
} {
  return {
    pending: state.sessionOperation === 'pending',
    failure: state.sessionOperation === 'failed' ? state.sessionOperationMessage : null,
  };
}

export function navigationTitle(state: AppState): string | null {
  switch (state.mainView) {
    case 'personas': return 'Personas';
    case 'new-persona': return 'New persona';
    case 'persona-detail':
      return state.bootstrap?.personas.find(
        ({ id }) => id === state.inspectedPersonaId,
      )?.display_name ?? 'Persona';
    case 'characters': return 'Characters';
    case 'new-character': return 'New character';
    case 'character-detail':
      return state.bootstrap?.characters.find(
        ({ id }) => id === state.inspectedCharacterId,
      )?.display_name ?? 'Character';
    case 'character-settings': return 'Settings';
    case 'forums': return 'Forums';
    case 'new-forum': return 'New forum';
    case 'sessions': return 'Sessions';
    case 'forum-detail':
      return state.bootstrap?.forums.find(
        ({ id }) => id === state.currentForumId,
      )?.display_name ?? 'Forum';
    case 'forum-members': return 'Members';
    case 'new-session': return 'New session';
    case 'settings': return 'Settings';
    case 'settings-vaults': return 'Vaults';
    case 'settings-new-vault': return 'New vault';
    case 'settings-download-vault': return 'Download vault';
    case 'settings-vault': return state.inspectedVaultName ?? 'Vault';
    case 'settings-providers': return 'Providers';
    case 'settings-new-provider': return 'New provider';
    case 'settings-provider': return state.inspectedProviderName ?? 'Provider';
    case 'settings-styles': return 'Styles';
    case 'settings-new-style': return 'New style';
    case 'settings-style': return state.inspectedStyleName ?? 'Style';
    case 'settings-voices': return 'Voices';
    case 'settings-new-voice': return 'New voice';
    case 'settings-voice': return state.inspectedVoiceName ?? 'Voice';
    case 'settings-api-keys': return 'API Keys';
    case 'settings-new-api-key': return 'New API key';
    case 'settings-api-key': return state.inspectedApiKeyName ?? 'API Key';
    case 'settings-r2-storage': return 'R2 storage';
    case 'chat': return null;
  }
}
