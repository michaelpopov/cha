import {
  useEffect,
  useRef,
  useState,
  type Dispatch,
  type FormEvent,
  type KeyboardEvent,
  type MouseEvent,
} from 'react';
import { createPortal } from 'react-dom';

import { publicErrorMessage } from '../api/client';
import type { AppAction, AppState, MainView } from '../state/view';
import { CharacterIcon, ForumsIcon, MoreIcon, PersonasIcon, SettingsIcon } from './Icons';
import { TransliteratingInput } from './TransliterationMode';

interface SidebarProps {
  state: AppState;
  dispatch: Dispatch<AppAction>;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
  onDownloadSession(forumId: string, sessionId: string, label: string): Promise<void>;
  onRenameSession(forumId: string, sessionId: string, label: string): Promise<void>;
  onDeleteSession(forumId: string, sessionId: string): Promise<void>;
  onSwitchVault(vaultName: string): Promise<void>;
}

interface SelectedSession {
  forumId: string;
  sessionId: string;
  label: string;
}

interface MenuState extends SelectedSession {
  x: number;
  y: number;
  restoreFocus: HTMLElement | null;
}

interface DialogState extends SelectedSession {
  kind: 'rename' | 'delete';
  restoreFocus: HTMLElement | null;
}

const navigation = [
  { action: 'show-personas' as const, views: ['personas', 'persona-detail'] as MainView[], label: 'Personas', icon: PersonasIcon },
  { action: 'show-characters' as const, views: ['characters', 'character-detail', 'character-settings'] as MainView[], label: 'Characters', icon: CharacterIcon },
  { action: 'show-forums' as const, views: ['forums', 'new-forum', 'sessions', 'forum-detail', 'forum-members', 'new-session'] as MainView[], label: 'Forums', icon: ForumsIcon },
];

const settingsViews: MainView[] = [
  'settings',
  'settings-providers',
  'settings-new-provider',
  'settings-provider',
  'settings-styles',
  'settings-new-style',
  'settings-style',
  'settings-voices',
  'settings-new-voice',
  'settings-voice',
  'settings-api-keys',
  'settings-new-api-key',
  'settings-api-key',
];

function SessionDialog({
  kind,
  session,
  restoreFocus,
  onClose,
  onDelete,
  onRename,
}: {
  kind: 'rename' | 'delete';
  session: SelectedSession;
  restoreFocus: HTMLElement | null;
  onClose(): void;
  onDelete(): Promise<void>;
  onRename(label: string): Promise<void>;
}) {
  const dialog = useRef<HTMLDialogElement | null>(null);
  const [label, setLabel] = useState(session.label);
  const [pending, setPending] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const trimmed = label.trim();

  useEffect(() => {
    if (typeof dialog.current?.showModal === 'function') dialog.current.showModal();
    else dialog.current?.setAttribute('open', '');
    return () => {
      if (typeof dialog.current?.close === 'function') dialog.current.close();
      if (restoreFocus?.isConnected) restoreFocus.focus();
    };
  }, [restoreFocus]);

  async function submit(event: FormEvent) {
    event.preventDefault();
    if (pending) return;
    setPending(true);
    setError(null);
    try {
      if (kind === 'rename') await onRename(trimmed);
      else await onDelete();
      onClose();
    } catch (failure: unknown) {
      setError(publicErrorMessage(failure, `The session could not be ${kind === 'rename' ? 'renamed' : 'deleted'}.`));
    } finally {
      setPending(false);
    }
  }

  return createPortal(
    <dialog className="cha-dialog" onCancel={onClose} ref={dialog}>
      <form method="dialog" onSubmit={(event) => void submit(event)}>
        <h2>{kind === 'rename' ? 'Rename session' : 'Delete session?'}</h2>
        {kind === 'rename' ? (
          <TransliteratingInput
            autoFocus
            className="cha-form-control"
            disabled={pending}
            id="cha-rename-session"
            label="Session name"
            maxLength={200}
            onFocus={(event) => event.currentTarget.select()}
            onValueChange={setLabel}
            value={label}
          />
        ) : (
          <p>Delete “{session.label}”? It will be removed from CHA and cannot be reopened.</p>
        )}
        {error && <p className="cha-error-message" role="alert">{error}</p>}
        <div className="cha-dialog-actions">
          <button className="cha-button cha-button-ghost" disabled={pending} onClick={onClose} type="button">
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={pending || (kind === 'rename' && (!trimmed || trimmed === session.label))}
            type="submit"
          >
            {pending ? 'Working…' : kind === 'rename' ? 'Rename' : 'Delete'}
          </button>
        </div>
      </form>
    </dialog>,
    document.body,
  );
}

export function Sidebar({
  state,
  dispatch,
  onDownloadSession,
  onOpenSession,
  onRenameSession,
  onDeleteSession,
  onSwitchVault,
}: SidebarProps) {
  const forums = new Map(state.bootstrap?.forums.map((forum) => [forum.id, forum]));
  const recents = state.bootstrap?.recent_sessions;
  const [menu, setMenu] = useState<MenuState | null>(null);
  const [dialog, setDialog] = useState<DialogState | null>(null);
  const [downloadError, setDownloadError] = useState<string | null>(null);
  const [vaultPending, setVaultPending] = useState(false);
  const [vaultError, setVaultError] = useState<string | null>(null);
  const menuRef = useRef<HTMLDivElement | null>(null);

  function closeMenu(restore = true) {
    if (restore) menu?.restoreFocus?.focus();
    setMenu(null);
  }

  function openMenu(event: MouseEvent, session: SelectedSession, anchor: HTMLElement) {
    event.preventDefault();
    setDownloadError(null);
    const rect = anchor.getBoundingClientRect();
    const x = event.type === 'contextmenu' ? event.clientX : rect.right;
    const y = event.type === 'contextmenu' ? event.clientY : rect.bottom;
    const target = event.target as HTMLElement;
    setMenu({
      ...session,
      x,
      y,
      restoreFocus: target.closest<HTMLElement>('button') ?? anchor,
    });
  }

  useEffect(() => {
    if (!menu) return;
    const dismiss = (event: globalThis.MouseEvent) => {
      if (!menuRef.current?.contains(event.target as Node)) closeMenu(false);
    };
    window.addEventListener('pointerdown', dismiss);
    return () => window.removeEventListener('pointerdown', dismiss);
  }, [menu]);

  useEffect(() => {
    if (menu) menuRef.current?.querySelector<HTMLButtonElement>('button')?.focus();
  }, [menu]);

  function menuKeys(event: KeyboardEvent<HTMLDivElement>) {
    const items = [...event.currentTarget.querySelectorAll<HTMLButtonElement>('[role="menuitem"]')];
    const current = items.indexOf(document.activeElement as HTMLButtonElement);
    let next = current;
    if (event.key === 'ArrowDown') next = (current + 1) % items.length;
    else if (event.key === 'ArrowUp') next = (current - 1 + items.length) % items.length;
    else if (event.key === 'Home') next = 0;
    else if (event.key === 'End') next = items.length - 1;
    else if (event.key === 'Escape') return closeMenu();
    else return;
    event.preventDefault();
    items[next]?.focus();
  }

  async function switchVault(name: string) {
    if (name === state.bootstrap?.vault_name || vaultPending) return;
    setVaultPending(true);
    setVaultError(null);
    try {
      await onSwitchVault(name);
    } catch (failure: unknown) {
      setVaultError(publicErrorMessage(failure, 'The vault could not be switched.'));
      setVaultPending(false);
    }
  }

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
          const mutable = session.forum_id !== state.bootstrap?.initial_forum_id
            || session.session_id !== state.bootstrap?.initial_session_id;
          const selected = {
            forumId: session.forum_id,
            sessionId: session.session_id,
            label: session.session_label,
          };
          return (
            <div
              className={`cha-recent-row ${current ? 'is-current' : ''}`}
              key={`${session.forum_id}/${session.session_id}`}
              onContextMenu={mutable ? (event) => openMenu(event, selected, event.currentTarget) : undefined}
            >
              <button
                aria-current={current ? 'page' : undefined}
                className="cha-recent-open"
                onClick={() => void onOpenSession(session.forum_id, session.session_id)}
                type="button"
              >
                <span className="cha-primary-line">{session.session_label}</span>
                <span className="cha-secondary-line">
                  {forums.get(session.forum_id)?.display_name ?? session.forum_id}
                </span>
              </button>
              {mutable && (
                <button
                  aria-expanded={menu?.forumId === session.forum_id
                    && menu.sessionId === session.session_id}
                  aria-haspopup="menu"
                  aria-label={`Actions for ${session.session_label}`}
                  className="cha-recent-more"
                  onClick={(event) => openMenu(event, selected, event.currentTarget)}
                  type="button"
                >
                  <MoreIcon />
                </button>
              )}
            </div>
          );
        })}
      </div>
      {downloadError && <p className="cha-error-message" role="alert">{downloadError}</p>}
      {vaultError && <p className="cha-error-message" role="alert">{vaultError}</p>}
      <div className="cha-sidebar-footer">
        <select
          aria-label="Vault"
          className="cha-vault-select"
          disabled={state.bootstrapStatus !== 'ready' || vaultPending}
          onChange={(event) => void switchVault(event.target.value)}
          value={state.bootstrap?.vault_name ?? ''}
        >
          {state.bootstrap?.vaults.map((name) => (
            <option key={name} value={name}>{name}</option>
          ))}
        </select>
        <button
          aria-current={settingsViews.includes(state.mainView) ? 'page' : undefined}
          aria-label="Settings"
          className={`cha-sidebar-settings ${settingsViews.includes(state.mainView) ? 'is-current' : ''}`}
          disabled={state.bootstrapStatus !== 'ready'}
          onClick={() => dispatch({ type: 'show-settings' })}
          title="Settings"
          type="button"
        >
          <SettingsIcon />
        </button>
      </div>
      {menu && createPortal(
        <div
          className="cha-session-menu"
          onKeyDown={menuKeys}
          ref={menuRef}
          role="menu"
          style={{
            left: Math.max(8, Math.min(menu.x, window.innerWidth - 168)),
            top: Math.max(8, Math.min(menu.y, window.innerHeight - 140)),
          }}
        >
          <button
            onClick={() => {
              const selected = menu;
              closeMenu(false);
              void onDownloadSession(
                selected.forumId, selected.sessionId, selected.label,
              ).catch((failure: unknown) => {
                setDownloadError(publicErrorMessage(
                  failure,
                  'The session could not be downloaded.',
                ));
              }).finally(() => {
                if (selected.restoreFocus?.isConnected) selected.restoreFocus.focus();
              });
            }}
            role="menuitem"
            type="button"
          >Download</button>
          <button onClick={() => { setDialog({ kind: 'rename', ...menu }); closeMenu(false); }} role="menuitem" type="button">Rename…</button>
          <button onClick={() => { setDialog({ kind: 'delete', ...menu }); closeMenu(false); }} role="menuitem" type="button">Delete…</button>
        </div>,
        document.body,
      )}
      {dialog && (
        <SessionDialog
          kind={dialog.kind}
          onClose={() => setDialog(null)}
          onDelete={() => onDeleteSession(dialog.forumId, dialog.sessionId)}
          onRename={(label) => onRenameSession(dialog.forumId, dialog.sessionId, label)}
          restoreFocus={dialog.restoreFocus}
          session={dialog}
        />
      )}
    </aside>
  );
}
