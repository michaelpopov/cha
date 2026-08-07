import type { Bootstrap, SessionSnapshot } from '../api/client';

export type MainView =
  | 'chat'
  | 'personas'
  | 'characters'
  | 'character-detail'
  | 'forums'
  | 'sessions'
  | 'new-session';

export type BootstrapStatus = 'loading' | 'ready' | 'failed' | 'incompatible';

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
  currentPersonaId: string | null;
  currentForumId: string | null;
  activeConversation: ActiveConversation | null;
  inspectedCharacterId: string | null;
  currentDefaultCharacterId: string | null;
  sessionOperation: 'idle' | 'pending' | 'failed';
  sessionOperationMessage: string | null;
  activeConversationLabel: string | null;
  streamLost: boolean;
}

export const initialAppState: AppState = {
  sidebarOpen: true,
  mainView: 'chat',
  bootstrapStatus: 'loading',
  bootstrap: null,
  bootstrapMessage: null,
  currentPersonaId: null,
  currentForumId: null,
  activeConversation: null,
  inspectedCharacterId: null,
  currentDefaultCharacterId: null,
  sessionOperation: 'idle',
  sessionOperationMessage: null,
  activeConversationLabel: null,
  streamLost: false,
};

export type AppAction =
  | { type: 'bootstrap-loaded'; bootstrap: Bootstrap }
  | { type: 'bootstrap-failed'; message: string; incompatible: boolean }
  | { type: 'bootstrap-refreshed'; bootstrap: Bootstrap }
  | { type: 'toggle-sidebar' }
  | { type: 'show-personas' }
  | { type: 'select-persona'; personaId: string }
  | { type: 'show-characters' }
  | { type: 'inspect-character'; characterId: string }
  | { type: 'show-forums' }
  | { type: 'select-forum'; forumId: string }
  | { type: 'show-sessions' }
  | { type: 'show-new-session' }
  | { type: 'show-chat' }
  | { type: 'session-operation-started'; message: string }
  | { type: 'session-operation-failed'; message: string }
  | { type: 'conversation-opened'; snapshot: SessionSnapshot }
  | { type: 'session-snapshot'; snapshot: SessionSnapshot }
  | { type: 'show-initial-conversation' }
  | { type: 'stream-lost' }
  | { type: 'set-default-character'; characterId: string };

function idleSessionOperation() {
  return { sessionOperation: 'idle' as const, sessionOperationMessage: null };
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
    streamLost: false,
    ...idleSessionOperation(),
  };
}

export function appReducer(state: AppState, action: AppAction): AppState {
  switch (action.type) {
    case 'bootstrap-loaded':
      return showInitialConversation({
        ...state,
        bootstrapStatus: 'ready',
        bootstrap: action.bootstrap,
        bootstrapMessage: null,
        currentPersonaId: action.bootstrap.initial_persona_id,
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
      return { ...state, mainView: 'personas', ...idleSessionOperation() };
    case 'select-persona':
      return { ...state, currentPersonaId: action.personaId };
    case 'show-characters':
      return {
        ...state,
        mainView: 'characters',
        inspectedCharacterId: null,
        ...idleSessionOperation(),
      };
    case 'inspect-character':
      return {
        ...state,
        mainView: 'character-detail',
        inspectedCharacterId: action.characterId,
      };
    case 'show-forums':
      return { ...state, mainView: 'forums', ...idleSessionOperation() };
    case 'select-forum':
      return {
        ...state,
        mainView: 'sessions',
        currentForumId: action.forumId,
        ...idleSessionOperation(),
      };
    case 'show-sessions':
      return { ...state, mainView: 'sessions', ...idleSessionOperation() };
    case 'show-new-session':
      return { ...state, mainView: 'new-session', ...idleSessionOperation() };
    case 'show-chat':
      return { ...state, mainView: 'chat', ...idleSessionOperation() };
    case 'session-operation-started':
      return {
        ...state,
        sessionOperation: 'pending',
        sessionOperationMessage: action.message,
      };
    case 'session-operation-failed':
      return {
        ...state,
        sessionOperation: 'failed',
        sessionOperationMessage: action.message,
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
        streamLost: false,
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
      };
    case 'show-initial-conversation':
      return state.bootstrap ? showInitialConversation(state, state.bootstrap) : state;
    case 'stream-lost':
      return { ...state, streamLost: true };
    case 'set-default-character':
      return { ...state, currentDefaultCharacterId: action.characterId };
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
    case 'characters': return 'Characters';
    case 'character-detail':
      return state.bootstrap?.characters.find(
        ({ id }) => id === state.inspectedCharacterId,
      )?.display_name ?? 'Character';
    case 'forums': return 'Forums';
    case 'sessions': return 'Sessions';
    case 'new-session': return 'New session';
    case 'chat': return null;
  }
}
