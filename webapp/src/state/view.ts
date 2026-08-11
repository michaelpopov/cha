import type { Bootstrap, SessionSnapshot } from '../api/client';
import type { AppendEvent } from '../api/events';

export type MainView =
  | 'chat'
  | 'personas'
  | 'characters'
  | 'character-detail'
  | 'forums'
  | 'sessions'
  | 'new-session';

export type BootstrapStatus = 'loading' | 'ready' | 'failed' | 'incompatible';
export type StreamStatus =
  | 'idle'
  | 'connecting'
  | 'connected'
  | 'reconnecting'
  | 'retry'
  | 'other-window';

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
  | { type: 'show-characters' }
  | { type: 'inspect-character'; characterId: string }
  | { type: 'show-forums' }
  | { type: 'select-forum'; forumId: string }
  | { type: 'show-sessions' }
  | { type: 'show-new-session' }
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
      return { ...state, mainView: 'personas', ...idleSessionOperation() };
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
        ...idleSessionOperation(),
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
