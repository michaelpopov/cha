import type { Dispatch } from 'react';

import type { AppAction, AppState } from '../state/view';
import { SidebarIcon } from './Icons';

export function TopBar({ dispatch, state, title }: {
  dispatch: Dispatch<AppAction>;
  state: AppState;
  title: string | null;
}) {
  return (
    <header className="cha-topbar">
      <div className="cha-topbar-lead">
        <button
          aria-expanded={state.sidebarOpen}
          aria-label={state.sidebarOpen ? 'Hide sidebar' : 'Show sidebar'}
          className="cha-icon-action"
          onClick={() => dispatch({ type: 'toggle-sidebar' })}
          type="button"
        ><SidebarIcon /></button>
      </div>
      <div className="cha-topbar-title">{title && <h1>{title}</h1>}</div>
      {title && <div className="cha-topbar-balance" aria-hidden="true" />}
    </header>
  );
}
