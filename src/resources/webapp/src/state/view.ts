import type { Bootstrap } from '../api/client';

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
};

export type AppAction =
  | { type: 'bootstrap-loaded'; bootstrap: Bootstrap }
  | { type: 'bootstrap-failed'; message: string; incompatible: boolean }
  | { type: 'toggle-sidebar' }
  | { type: 'show-personas' }
  | { type: 'select-persona'; personaId: string }
  | { type: 'show-characters' }
  | { type: 'inspect-character'; characterId: string }
  | { type: 'show-forums' }
  | { type: 'select-forum'; forumId: string }
  | { type: 'show-new-session' }
  | { type: 'show-chat' }
  | { type: 'activate-conversation'; forumId: string; sessionId: string }
  | { type: 'set-default-character'; characterId: string };

export function appReducer(state: AppState, action: AppAction): AppState {
  switch (action.type) {
    case 'bootstrap-loaded': {
      const initialForum = action.bootstrap.forums.find(
        ({ id }) => id === action.bootstrap.initial_forum_id,
      );
      return {
        ...state,
        bootstrapStatus: 'ready',
        bootstrap: action.bootstrap,
        bootstrapMessage: null,
        currentPersonaId: action.bootstrap.initial_persona_id,
        currentForumId: action.bootstrap.initial_forum_id,
        activeConversation: {
          forumId: action.bootstrap.initial_forum_id,
          sessionId: action.bootstrap.initial_session_id,
        },
        currentDefaultCharacterId: initialForum?.default_character_id ?? null,
      };
    }
    case 'bootstrap-failed':
      return {
        ...state,
        bootstrapStatus: action.incompatible ? 'incompatible' : 'failed',
        bootstrapMessage: action.message,
      };
    case 'toggle-sidebar':
      return { ...state, sidebarOpen: !state.sidebarOpen };
    case 'show-personas':
      return { ...state, mainView: 'personas' };
    case 'select-persona':
      return { ...state, currentPersonaId: action.personaId };
    case 'show-characters':
      return { ...state, mainView: 'characters', inspectedCharacterId: null };
    case 'inspect-character':
      return {
        ...state,
        mainView: 'character-detail',
        inspectedCharacterId: action.characterId,
      };
    case 'show-forums':
      return { ...state, mainView: 'forums' };
    case 'select-forum':
      return { ...state, mainView: 'sessions', currentForumId: action.forumId };
    case 'show-new-session':
      return { ...state, mainView: 'new-session' };
    case 'show-chat':
      return { ...state, mainView: 'chat' };
    case 'activate-conversation':
      return {
        ...state,
        mainView: 'chat',
        currentForumId: action.forumId,
        activeConversation: { forumId: action.forumId, sessionId: action.sessionId },
      };
    case 'set-default-character':
      return { ...state, currentDefaultCharacterId: action.characterId };
  }
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
