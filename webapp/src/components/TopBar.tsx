import type { Dispatch } from 'react';

import type { AppAction, AppState } from '../state/view';
import { SidebarIcon } from './Icons';

export function SidebarToggle({ dispatch, sidebarOpen, className = 'cha-icon-action' }: {
  dispatch: Dispatch<AppAction>;
  sidebarOpen: boolean;
  className?: string;
}) {
  const label = sidebarOpen ? 'Hide sidebar' : 'Show sidebar';
  return (
    <button
      aria-expanded={sidebarOpen}
      aria-label={label}
      className={className}
      onClick={() => dispatch({ type: 'toggle-sidebar' })}
      title={label}
      type="button"
    ><SidebarIcon /></button>
  );
}

export function TopBar({ dispatch, state, title }: {
  dispatch: Dispatch<AppAction>;
  state: AppState;
  title: string | null;
}) {
  return (
    <header className="cha-topbar">
      <div className="cha-topbar-lead">
        <SidebarToggle dispatch={dispatch} sidebarOpen={state.sidebarOpen} />
      </div>
      <div className="cha-topbar-title">{title && <h1>{title}</h1>}</div>
      {title && <div className="cha-topbar-balance" aria-hidden="true" />}
    </header>
  );
}
