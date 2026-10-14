import { useEffect, useState } from 'react';
import { publicErrorMessage, type ChaClient, type SessionListing } from '../api/client';
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  MessageIcon,
  PlusIcon,
} from './Icons';
import { forumMemberNames } from './ForumScreens';
import { type NavigationScreenProps } from './RosterScreens';

interface SessionsScreenProps extends NavigationScreenProps {
  client: ChaClient;
  catalogRevision: number;
  onCreateSession(forumId: string): Promise<boolean>;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
}

export function formatSessionTime(updatedAt: number, now = Date.now()): string {
  const elapsed = Math.max(0, Math.floor(now / 1000) - updatedAt);
  if (elapsed < 60) return 'Now';
  if (elapsed < 60 * 60) return `${Math.floor(elapsed / 60)}m`;
  if (elapsed < 24 * 60 * 60) return `${Math.floor(elapsed / (60 * 60))}h`;
  if (elapsed < 7 * 24 * 60 * 60) return `${Math.floor(elapsed / (24 * 60 * 60))}d`;

  const date = new Date(updatedAt * 1000);
  const current = new Date(now);
  return new Intl.DateTimeFormat(undefined, {
    month: 'short',
    day: 'numeric',
    ...(date.getFullYear() === current.getFullYear() ? {} : { year: 'numeric' }),
  }).format(date);
}

export function SessionsScreen({
  state,
  navigate,
  client,
  catalogRevision,
  onCreateSession,
  onOpenSession,
}: SessionsScreenProps) {
  const [sessions, setSessions] = useState<SessionListing[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [requestVersion, setRequestVersion] = useState(0);
  const forumId = state.currentForumId;
  const forum = state.bootstrap?.forums.find(({ id }) => id === forumId);
  // Entrance has only the server's built-in Welcome session.
  const canCreateSessions = forumId !== state.bootstrap?.entrance_forum_id;

  useEffect(() => {
    if (!forumId) return;
    let current = true;
    setSessions(null);
    setError(null);
    void client.listSessions(forumId).then(
      (loaded) => {
        // The listing is not ordered by the server.
        if (current) setSessions([...loaded].sort((left, right) => right.updated_at - left.updated_at));
      },
      (failure: unknown) => {
        if (current) {
          setError(publicErrorMessage(failure, 'Sessions could not be loaded.'));
        }
      },
    );
    return () => {
      current = false;
    };
  }, [catalogRevision, client, forumId, requestVersion]);

  return (
    <section className="cha-screen cha-navigation" aria-label="Forum sessions navigation">
      <button className="cha-back-row" onClick={() => navigate({ type: 'show-forums' })} type="button">
        <ChevronLeftIcon />
        <span>All forums</span>
      </button>
      {/* The screen is titled Sessions, so without this the forum being listed
          is named nowhere once the sidebar is collapsed. It reads as a pushable
          row because that is what it is: the way into the forum's description. */}
      {forum && (
        <button
          className="cha-header-row"
          onClick={() => navigate({ type: 'show-forum-detail' })}
          type="button"
        >
          <span className="cha-list-copy">
            <span className="cha-primary-line">{forum.display_name}</span>
            <span className="cha-secondary-line">{forumMemberNames(forum)}</span>
          </span>
          <ChevronRightIcon className="cha-chevron" />
        </button>
      )}
      {!forumId && <p className="cha-state-message">No forum is selected.</p>}
      {forumId && !sessions && !error && (
        <p className="cha-state-message" role="status">Loading sessions…</p>
      )}
      {forumId && error && (
        <div className="cha-state-message cha-error-message" role="alert">
          <p>{error}</p>
          <button
            className="cha-button cha-button-ghost"
            onClick={() => setRequestVersion((version) => version + 1)}
            type="button"
          >
            Try again
          </button>
        </div>
      )}
      {forumId && sessions?.length === 0 && (
        <p className="cha-state-message">
          {canCreateSessions
            ? 'No sessions in this forum yet. Create the first one below.'
            : 'This forum has no sessions.'}
        </p>
      )}
      {forumId && sessions && (
        <div className="cha-list">
          {canCreateSessions && (
            <button
              className="cha-list-action"
              onClick={() => void onCreateSession(forumId)}
              type="button"
            >
              <span className="cha-list-icon"><PlusIcon /></span>
              <span className="cha-list-copy">
                <span className="cha-primary-line">New session</span>
              </span>
              <ChevronRightIcon className="cha-chevron" />
            </button>
          )}
          {sessions.map((session) => {
            const active = state.activeConversation?.forumId === forumId
              && state.activeConversation.sessionId === session.id;
            return (
              <button
                aria-current={active ? 'page' : undefined}
                className={`cha-list-action ${active ? 'is-current' : ''}`}
                key={session.id}
                onClick={() => void onOpenSession(forumId, session.id)}
                type="button"
              >
                <span className="cha-list-icon"><MessageIcon /></span>
                <span className="cha-list-copy">
                  <span className="cha-primary-line">{session.label}</span>
                </span>
                <time className="cha-secondary-line" dateTime={new Date(session.updated_at * 1000).toISOString()}>
                  {formatSessionTime(session.updated_at)}
                </time>
              </button>
            );
          })}
        </div>
      )}
    </section>
  );
}
