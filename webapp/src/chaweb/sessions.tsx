import { useEffect, useRef, type ReactNode } from 'react';

import type { ForumSummary, SessionListing } from '../api/client';
import { CheckIcon, ChevronRightIcon } from '../components/Icons';
import { welcomeSessionId } from '../state/route';
import { formatTimestamp } from './time';

export function sessionsForNavigation(sessions: SessionListing[]): SessionListing[] {
  return sessions
    .filter((session) => session.id !== welcomeSessionId)
    .sort((left, right) => right.updated_at - left.updated_at);
}

export function SessionsView({
  forums,
  forumId,
  sessions,
  currentSessionId,
  welcomeCurrent = false,
  error,
  onForum,
  onOpen,
  onOpenWelcome,
  onNewSession,
  onRetry,
  disabled = false,
  vaultActions,
}: {
  forums: ForumSummary[];
  forumId: string;
  sessions: SessionListing[];
  currentSessionId: string | null;
  welcomeCurrent?: boolean;
  error: string | null;
  onForum(forumId: string): void;
  onOpen(sessionId: string): void;
  onOpenWelcome(): void;
  onNewSession(): void;
  onRetry?(): void;
  disabled?: boolean;
  vaultActions?: ReactNode;
}) {
  const picker = useRef<HTMLDetailsElement>(null);
  const selectedForum = forums.find((forum) => forum.id === forumId);
  const rows = sessionsForNavigation(sessions);
  const canCreate = forums.length > 0;

  useEffect(() => {
    const closeOutside = (event: Event) => {
      if (event.target instanceof Node && !picker.current?.contains(event.target)) {
        if (picker.current) picker.current.open = false;
      }
    };
    document.addEventListener('pointerdown', closeOutside);
    document.addEventListener('focusin', closeOutside);
    return () => {
      document.removeEventListener('pointerdown', closeOutside);
      document.removeEventListener('focusin', closeOutside);
    };
  }, []);

  useEffect(() => {
    if (picker.current) picker.current.open = false;
  }, [forumId]);

  return (
    <div className="chaweb-sessions">
      {forums.length > 0 && <details
        inert={disabled}
        className="chaweb-forum-picker"
        onKeyDown={(event) => {
          if (event.key === 'Escape') {
            event.preventDefault();
            event.currentTarget.open = false;
            event.currentTarget.querySelector('summary')?.focus();
          }
        }}
        ref={picker}
      >
        <summary
          aria-describedby="chaweb-current-forum"
          aria-label="Forum"
          className="chaweb-forum"
          role="button"
        >
          <span className="chaweb-forum-text" id="chaweb-current-forum">
            <span className="chaweb-forum-name">{selectedForum?.display_name}</span>
            {' '}
            <span className="chaweb-forum-characters">
              {selectedForum?.members.map((member) => member.display_name).join(' · ')}
            </span>
          </span>
          <ChevronRightIcon className="chaweb-forum-chevron" />
        </summary>
        <div aria-label="Forums" className="chaweb-forum-menu" role="group">
          {forums.map((forum) => (
            <button
              aria-pressed={forum.id === forumId}
              className="chaweb-forum-option"
              key={forum.id}
              onClick={() => {
                if (picker.current) {
                  picker.current.open = false;
                  picker.current.querySelector('summary')?.focus();
                }
                onForum(forum.id);
              }}
              type="button"
            >
              <span className="chaweb-forum-text">
                <span className="chaweb-forum-name">{forum.display_name}</span>
                {' '}
                <span className="chaweb-forum-characters">
                  {forum.members.map((member) => member.display_name).join(' · ')}
                </span>
              </span>
              <CheckIcon />
            </button>
          ))}
        </div>
      </details>}
      {error && <p className="chaweb-alert" role="alert">{error}</p>}
      {onRetry && (
        <button className="chaweb-new-session" disabled={disabled} onClick={onRetry} type="button">Retry</button>
      )}
      <button
        aria-current={welcomeCurrent ? 'true' : undefined}
        className="chaweb-session"
        disabled={disabled}
        onClick={onOpenWelcome}
        type="button"
      >
        <span className="chaweb-session-title">Welcome</span>
        {welcomeCurrent && <CheckIcon />}
      </button>
      <ul aria-label="Sessions" className="chaweb-list">
        {rows.map((session) => {
          const current = session.id === currentSessionId;
          return (
            <li key={session.id}>
              <button
                aria-current={current ? 'true' : undefined}
                className="chaweb-session"
                disabled={disabled}
                onClick={() => onOpen(session.id)}
                type="button"
              >
                <span className="chaweb-session-title">{session.label}</span>
                <time
                  className="chaweb-session-time"
                  dateTime={new Date(session.updated_at * 1000).toISOString()}
                >
                  {formatTimestamp(session.updated_at)}
                </time>
                {current && <CheckIcon />}
              </button>
            </li>
          );
        })}
      </ul>
      <button className="chaweb-new-session" disabled={disabled || !canCreate} onClick={onNewSession} type="button">
        New Session
      </button>
      {vaultActions}
    </div>
  );
}
