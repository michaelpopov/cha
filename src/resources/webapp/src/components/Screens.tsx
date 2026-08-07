import { useEffect, useState, type Dispatch, type FormEvent } from 'react';

import type { ChaClient, CharacterDetail, SessionListing } from '../api/client';
import { sessionOperationState, type AppAction, type AppState } from '../state/view';
import { Markdown } from './Markdown';
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  ForumsIcon,
  MessageIcon,
  PlusIcon,
  SendIcon,
  TargetIcon,
} from './Icons';

interface DiscoveryScreenProps {
  state: AppState;
  dispatch: Dispatch<AppAction>;
}

export function ChatScreen({ state }: { state: AppState }) {
  const persona = state.bootstrap?.personas.find(({ id }) => id === state.currentPersonaId);
  const forum = state.bootstrap?.forums.find(
    ({ id }) => id === state.activeConversation?.forumId,
  );
  const character = state.bootstrap?.characters.find(
    ({ id }) => id === state.currentDefaultCharacterId,
  );

  return (
    <section className="cha-screen cha-chat" aria-label="Chat area">
      <div className="cha-transcript">
        <div className="cha-chat-welcome">
          <span className="cha-chat-kicker">Welcome</span>
          <p>Your conversation will appear here.</p>
        </div>
      </div>
      <form className="cha-composer" onSubmit={(event) => event.preventDefault()}>
        <button
          aria-label="Choose target character"
          className="cha-composer-action"
          disabled
          type="button"
        >
          <TargetIcon />
        </button>
        <input
          aria-label="Message"
          disabled
          placeholder={`Message ${character?.display_name ?? 'character'}`}
        />
        <button
          aria-label="Send message"
          className="cha-composer-action cha-send"
          disabled
          type="submit"
        >
          <SendIcon />
        </button>
      </form>
      <div className="cha-chat-status" aria-label="Current chat context">
        <span>{forum?.display_name ?? 'Unknown forum'}</span>
        <span>From: {persona?.display_name ?? 'Unknown persona'}</span>
        <span>To: {character?.display_name ?? 'Unknown character'}</span>
      </div>
    </section>
  );
}

export function PersonasScreen({ state, dispatch }: DiscoveryScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Personas navigation">
      <p className="cha-screen-instruction">Select a persona</p>
      <div className="cha-persona-list" role="radiogroup" aria-label="Persona">
        {state.bootstrap?.personas.map((persona) => (
          <label className="cha-persona-row" key={persona.id}>
            <span className="cha-persona-copy">
              <span className="cha-persona-name">{persona.display_name}</span>
              {persona.description && (
                <span className="cha-persona-description">{persona.description}</span>
              )}
            </span>
            <input
              checked={state.currentPersonaId === persona.id}
              name="persona"
              onChange={() => dispatch({ type: 'select-persona', personaId: persona.id })}
              type="radio"
              value={persona.id}
            />
          </label>
        ))}
      </div>
    </section>
  );
}

export function CharactersScreen({ state, dispatch }: DiscoveryScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Characters navigation">
      <div className="cha-character-list">
        {state.bootstrap?.characters.map((character) => (
          <button
            className="cha-character-row"
            key={character.id}
            onClick={() => dispatch({ type: 'inspect-character', characterId: character.id })}
            type="button"
          >
            <span className="cha-persona-copy">
              <span className="cha-persona-name">{character.display_name}</span>
              {character.description && (
                <span className="cha-persona-description">{character.description}</span>
              )}
            </span>
            <ChevronRightIcon className="cha-chevron" />
          </button>
        ))}
      </div>
    </section>
  );
}

interface CharacterDetailScreenProps extends DiscoveryScreenProps {
  client: ChaClient;
}

export function CharacterDetailScreen({
  state,
  dispatch,
  client,
}: CharacterDetailScreenProps) {
  const [detail, setDetail] = useState<CharacterDetail | null>(null);
  const [error, setError] = useState<string | null>(null);
  const characterId = state.inspectedCharacterId;

  useEffect(() => {
    if (!characterId) return;
    let current = true;
    setDetail(null);
    setError(null);
    void client.getCharacter(characterId).then(
      (loaded) => {
        if (current) setDetail(loaded);
      },
      (failure: unknown) => {
        if (current) {
          setError(failure instanceof Error ? failure.message : 'Character detail could not be loaded.');
        }
      },
    );
    return () => {
      current = false;
    };
  }, [characterId, client]);

  return (
    <section className="cha-screen cha-navigation" aria-label="Character detail navigation">
      <button
        className="cha-back-row"
        onClick={() => dispatch({ type: 'show-characters' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Characters</span>
      </button>
      {!characterId && <p className="cha-state-message">No character is selected.</p>}
      {characterId && !detail && !error && (
        <p className="cha-state-message" role="status">Loading character…</p>
      )}
      {error && <p className="cha-state-message cha-error-message" role="alert">{error}</p>}
      {detail && <Markdown source={detail.character_markdown} />}
    </section>
  );
}

export function ForumsScreen({ state, dispatch }: DiscoveryScreenProps) {
  return (
    <section className="cha-screen cha-navigation" aria-label="Forums navigation">
      <p className="cha-screen-instruction">Choose a forum</p>
      <div className="cha-list">
        {state.bootstrap?.forums.map((forum) => (
          <button
            className="cha-list-action"
            key={forum.id}
            onClick={() => dispatch({ type: 'select-forum', forumId: forum.id })}
            type="button"
          >
            <span className="cha-list-icon"><ForumsIcon /></span>
            <span className="cha-list-copy">
              <span className="cha-primary-line">{forum.display_name}</span>
              <span className="cha-secondary-line">
                {forum.members.map(({ display_name }) => display_name).join(', ') || 'No characters'}
              </span>
            </span>
            <ChevronRightIcon className="cha-chevron" />
          </button>
        ))}
      </div>
    </section>
  );
}

interface SessionsScreenProps extends DiscoveryScreenProps {
  client: ChaClient;
  onOpenSession(forumId: string, sessionId: string): Promise<boolean>;
}

// Opening or creating a session is reported by the screen that started it, so
// the list stays on screen and a half-typed session name is not thrown away.
function SessionOperationReport({
  pending,
  failure,
  state,
}: {
  pending: boolean;
  failure: string | null;
  state: AppState;
}) {
  if (pending) {
    return (
      <p className="cha-state-message" role="status">
        {state.sessionOperationMessage ?? 'Opening session…'}
      </p>
    );
  }
  if (!failure) return null;
  return (
    <p className="cha-state-message cha-error-message" role="alert">{failure}</p>
  );
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
  dispatch,
  client,
  onOpenSession,
}: SessionsScreenProps) {
  const [sessions, setSessions] = useState<SessionListing[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [requestVersion, setRequestVersion] = useState(0);
  const forumId = state.currentForumId;
  const { pending: sessionPending, failure: sessionFailure } = sessionOperationState(state);

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
          setError(failure instanceof Error ? failure.message : 'Sessions could not be loaded.');
        }
      },
    );
    return () => {
      current = false;
    };
  }, [client, forumId, requestVersion]);

  return (
    <section className="cha-screen cha-navigation" aria-label="Forum sessions navigation">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-forums' })} type="button">
        <ChevronLeftIcon />
        <span>All forums</span>
      </button>
      {!forumId && <p className="cha-state-message">No forum is selected.</p>}
      <SessionOperationReport pending={sessionPending} failure={sessionFailure} state={state} />
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
      {forumId && sessions && (
        <div className="cha-list">
          <button
            className="cha-list-action"
            disabled={sessionPending}
            onClick={() => dispatch({ type: 'show-new-session' })}
            type="button"
          >
            <span className="cha-list-icon"><PlusIcon /></span>
            <span className="cha-list-copy">
              <span className="cha-primary-line">New session</span>
              <span className="cha-secondary-line">Enter a name to begin</span>
            </span>
            <ChevronRightIcon className="cha-chevron" />
          </button>
          {sessions.map((session) => {
            const active = state.activeConversation?.forumId === forumId
              && state.activeConversation.sessionId === session.id;
            return (
              <button
                aria-current={active ? 'page' : undefined}
                className={`cha-list-action ${active ? 'is-current' : ''}`}
                disabled={sessionPending}
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

interface NewSessionScreenProps extends DiscoveryScreenProps {
  onCreateSession(forumId: string, label: string): Promise<boolean>;
}

export function NewSessionScreen({
  state,
  dispatch,
  onCreateSession,
}: NewSessionScreenProps) {
  const [name, setName] = useState('');
  const trimmedName = name.trim();
  const { pending: sessionPending, failure: sessionFailure } = sessionOperationState(state);

  function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (!state.currentForumId || !trimmedName || sessionPending) return;
    void onCreateSession(state.currentForumId, trimmedName);
  }

  return (
    <section className="cha-screen cha-navigation" aria-label="New session navigation">
      <button
        className="cha-back-row"
        disabled={sessionPending}
        onClick={() => dispatch({ type: 'show-sessions' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Sessions</span>
      </button>
      <SessionOperationReport pending={sessionPending} failure={sessionFailure} state={state} />
      <form className="cha-new-session" onSubmit={submit}>
        <label htmlFor="cha-session-name">Session name</label>
        <input
          autoComplete="off"
          autoFocus
          className="cha-form-control"
          disabled={sessionPending}
          id="cha-session-name"
          onChange={(event) => setName(event.target.value)}
          placeholder="e.g. Architecture review"
          type="text"
          value={name}
        />
        <div className="cha-new-session-actions">
          <button
            className="cha-button cha-button-ghost"
            disabled={sessionPending}
            onClick={() => dispatch({ type: 'show-sessions' })}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={!trimmedName || sessionPending}
            type="submit"
          >
            Start session
          </button>
        </div>
      </form>
    </section>
  );
}
