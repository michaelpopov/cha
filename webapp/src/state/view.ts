import type {
  Bootstrap,
  CharacterDetail,
  ForumDetail,
  PersonaDetail,
  SessionSnapshot,
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
  | 'settings';

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
  | { type: 'show-characters' }
  | { type: 'show-new-character' }
  | { type: 'inspect-character'; characterId: string }
  | { type: 'character-detail-loaded'; characterId: string; writable: boolean }
  | { type: 'character-created'; character: CharacterDetail }
  | { type: 'character-updated'; character: CharacterDetail }
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
  | { type: 'show-new-session' }
  | { type: 'show-settings' }
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
    case 'show-new-session':
      return { ...state, mainView: 'new-session', ...idleSessionOperation() };
    case 'show-settings':
      return { ...state, mainView: 'settings', ...idleSessionOperation() };
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
    case 'chat': return null;
  }
}
