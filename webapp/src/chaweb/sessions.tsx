import type { SessionListing } from '../api/client';
import { CheckIcon } from '../components/Icons';
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
  error,
  onForum,
  onOpen,
  onNewSession,
}: {
  forums: Array<{ id: string; display_name: string }>;
  forumId: string;
  sessions: SessionListing[];
  currentSessionId: string | null;
  error: string | null;
  onForum(forumId: string): void;
  onOpen(sessionId: string): void;
  onNewSession(): void;
}) {
  const rows = sessionsForNavigation(sessions);
  return (
    <div className="chaweb-sessions">
      <select
        aria-label="Forum"
        className="chaweb-forum"
        onChange={(event) => onForum(event.target.value)}
        value={forumId}
      >
        {forums.map((forum) => (
          <option key={forum.id} value={forum.id}>{forum.display_name}</option>
        ))}
      </select>
      {error && <p className="chaweb-alert" role="alert">{error}</p>}
      <ul aria-label="Sessions" className="chaweb-list">
        {rows.map((session) => {
          const current = session.id === currentSessionId;
          return (
            <li key={session.id}>
              <button
                aria-current={current ? 'true' : undefined}
                className="chaweb-session"
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
      <button className="chaweb-new-session" onClick={onNewSession} type="button">
        New Session
      </button>
    </div>
  );
}
