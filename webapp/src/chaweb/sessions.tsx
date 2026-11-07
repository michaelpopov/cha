import { useEffect, useRef, useState } from 'react';

import type { ForumSummary, SessionListing } from '../api/client';
import { CheckIcon, ChevronRightIcon, TrashIcon } from '../components/Icons';
import { welcomeSessionId } from '../state/route';
import { formatTimestamp } from './time';

export function sessionsForNavigation(sessions: SessionListing[]): SessionListing[] {
  return sessions
    .slice()
    .sort((left, right) => right.updated_at - left.updated_at);
}

export function SessionsView({
  forums,
  forumId,
  sessions,
  currentSessionId,
  error,
  onForum,
  onOpen,
  onNewSession,
  entranceForumId,
  onRetry,
  onDelete,
  canDelete,
  isDeleting,
}: {
  forums: ForumSummary[];
  forumId: string;
  entranceForumId: string;
  sessions: SessionListing[];
  currentSessionId: string | null;
  error: string | null;
  onForum(forumId: string): void;
  onOpen(sessionId: string): void;
  onNewSession(): void;
  onRetry?(): void;
  onDelete(sessionId: string): void;
  canDelete(sessionId: string): boolean;
  isDeleting(sessionId: string): boolean;
}) {
  const picker = useRef<HTMLDetailsElement>(null);
  const list = useRef<HTMLUListElement>(null);
  const [revealed, setRevealed] = useState<string | null>(null);
  const selectedForum = forums.find((forum) => forum.id === forumId);
  const rows = sessionsForNavigation(sessions);
  const canCreate = selectedForum !== undefined && forumId !== entranceForumId;

  useEffect(() => {
    function closeOutside(event: Event) {
      const row = event.target instanceof Element ? event.target.closest('li') : null;
      if (!row || !list.current?.contains(row) || row.dataset.sessionId !== revealed) {
        setRevealed(null);
      }
    }
    document.addEventListener('pointerdown', closeOutside);
    document.addEventListener('focusin', closeOutside);
    return () => {
      document.removeEventListener('pointerdown', closeOutside);
      document.removeEventListener('focusin', closeOutside);
    };
  }, [revealed]);

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
    setRevealed(null);
  }, [forumId]);

  return (
    <div className="chaweb-sessions">
      {forums.length > 0 && <details
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
        <button className="chaweb-new-session" onClick={onRetry} type="button">Retry</button>
      )}
      <ul aria-label="Sessions" className="chaweb-list" ref={list}>
        {rows.map((session) => {
          const current = session.id === currentSessionId;
          return (
            <SessionRow
              key={session.id}
              sessionId={session.id}
              label={session.label}
              revealed={revealed === session.id}
              onReveal={(open) => setRevealed(open ? session.id : null)}
              onDelete={() => onDelete(session.id)}
              canDelete={session.id !== welcomeSessionId && canDelete(session.id)}
              deleting={isDeleting(session.id)}
            >
              <button
                aria-current={current ? 'true' : undefined}
                className="chaweb-session"
                onClick={() => onOpen(session.id)}
                type="button"
              >
                <span className="chaweb-session-title">
                  {session.id === welcomeSessionId ? 'Entrance' : session.label}
                </span>
                <time
                  className="chaweb-session-time"
                  dateTime={new Date(session.updated_at * 1000).toISOString()}
                >
                  {formatTimestamp(session.updated_at)}
                </time>
                {current && <CheckIcon />}
              </button>
            </SessionRow>
          );
        })}
      </ul>
      {canCreate && <button className="chaweb-new-session" onClick={onNewSession} type="button">
        New Session
      </button>}
    </div>
  );
}

function SessionRow({ sessionId, label, revealed, onReveal, onDelete, canDelete, deleting, children }: {
  sessionId: string;
  label: string;
  revealed: boolean;
  onReveal(open: boolean): void;
  onDelete(): void;
  canDelete: boolean;
  deleting: boolean;
  children: React.ReactNode;
}) {
  const gesture = useRef<{ x: number; y: number; horizontal: boolean; vertical: boolean } | null>(null);
  const suppressClick = useRef(false);
  const [drag, setDrag] = useState<number | null>(null);
  // The first click on a hidden Delete shows it. Only a shown Delete deletes.
  function onDeleteClick() {
    if (revealed) onDelete();
    else onReveal(true);
  }
  return <li
    className={`chaweb-session-row${revealed ? ' is-revealed' : ''}${canDelete || deleting ? ' is-deletable' : ''}`}
    data-session-id={sessionId}
    onPointerDown={(event) => {
      suppressClick.current = false;
      if (event.button !== 0 || !event.isPrimary || !canDelete
          || (event.target as Element).closest('.chaweb-session-delete')) return;
      gesture.current = { x: event.clientX, y: event.clientY, horizontal: false, vertical: false };
    }}
    onPointerMove={(event) => {
      const start = gesture.current;
      if (!start || start.vertical) return;
      const dx = event.clientX - start.x;
      const dy = event.clientY - start.y;
      if (!start.horizontal && Math.max(Math.abs(dx), Math.abs(dy)) >= 10) {
        if (Math.abs(dy) >= Math.abs(dx)) { start.vertical = true; return; }
        start.horizontal = true;
        suppressClick.current = true;
        event.currentTarget.setPointerCapture(event.pointerId);
      }
      if (start.horizontal) setDrag(Math.max(-88, Math.min(0, (revealed ? -88 : 0) + dx)));
    }}
    onPointerUp={(event) => {
      if (gesture.current?.horizontal) {
        const dx = event.clientX - gesture.current.x;
        onReveal((revealed ? -88 : 0) + dx < -36);
      }
      gesture.current = null;
      setDrag(null);
    }}
    onPointerCancel={() => { gesture.current = null; setDrag(null); }}
    onClickCapture={(event) => {
      if (suppressClick.current) {
        event.preventDefault();
        event.stopPropagation();
        suppressClick.current = false;
      } else if (revealed && !(event.target as Element).closest('.chaweb-session-delete')) {
        event.preventDefault();
        event.stopPropagation();
        onReveal(false);
      }
    }}
    onKeyDown={(event) => {
      if (event.key === 'Escape') { onReveal(false); event.currentTarget.querySelector('button')?.focus(); }
    }}
  >
    <div className="chaweb-session-slide" style={drag === null ? undefined : { transform: `translateX(${drag}px)`, transition: 'none' }}>
      {children}
    </div>
    {(canDelete || deleting) && <button
      aria-label={`Delete session “${label}”`}
      className="chaweb-session-delete"
      disabled={!canDelete || deleting}
      onClick={onDeleteClick}
      type="button"
    ><TrashIcon /><span>{deleting ? 'Deleting…' : 'Delete'}</span></button>}
  </li>;
}
