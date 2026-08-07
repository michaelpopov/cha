import type { Dispatch } from 'react';

import type { AppAction, AppState, MainView } from '../state/view';
import { CharacterIcon, ForumsIcon, PersonasIcon } from './Icons';

interface SidebarProps {
  state: AppState;
  dispatch: Dispatch<AppAction>;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
}

const navigation = [
  { action: 'show-personas' as const, views: ['personas'] as MainView[], label: 'Personas', icon: PersonasIcon },
  {
    action: 'show-characters' as const,
    views: ['characters', 'character-detail'] as MainView[],
    label: 'Characters',
    icon: CharacterIcon,
  },
  {
    action: 'show-forums' as const,
    views: ['forums', 'sessions', 'new-session'] as MainView[],
    label: 'Forums',
    icon: ForumsIcon,
  },
];

export function Sidebar({ state, dispatch, onOpenSession }: SidebarProps) {
  const forums = new Map(state.bootstrap?.forums.map((forum) => [forum.id, forum]));
  // The server returns Recent newest first across every forum.
  const recents = state.bootstrap?.recent_sessions;

  return (
    <aside className="cha-sidebar" aria-label="Sidebar">
      <div className="cha-brand">cha</div>
      <nav className="cha-sidebar-nav" aria-label="Primary">
        {navigation.map(({ action, views, label, icon: NavigationIcon }) => (
          <button
            className={`cha-side-action ${views.includes(state.mainView) ? 'is-current' : ''}`}
            disabled={state.bootstrapStatus !== 'ready'}
            key={action}
            onClick={() => dispatch({ type: action })}
            type="button"
          >
            <NavigationIcon />
            <span>{label}</span>
          </button>
        ))}
      </nav>
      <div className="cha-section-label">Recent</div>
      <div className="cha-recents" aria-label="Recent sessions">
        {recents?.length === 0 && <p className="cha-empty-list">No recent sessions</p>}
        {recents?.map((session) => {
          const current = state.activeConversation?.forumId === session.forum_id
            && state.activeConversation.sessionId === session.session_id;
          const content = (
            <>
              <span className="cha-primary-line">{session.session_label}</span>
              <span className="cha-secondary-line">
                {forums.get(session.forum_id)?.display_name ?? session.forum_id}
              </span>
            </>
          );
          return (
            <button
              aria-current={current ? 'page' : undefined}
              className={`cha-recent-row ${current ? 'is-current' : ''}`}
              key={`${session.forum_id}/${session.session_id}`}
              onClick={() => void onOpenSession(session.forum_id, session.session_id)}
              type="button"
            >
              {content}
            </button>
          );
        })}
      </div>
    </aside>
  );
}
