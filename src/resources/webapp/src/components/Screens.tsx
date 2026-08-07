import {
  useEffect,
  useRef,
  useState,
  type Dispatch,
  type FormEvent,
  type UIEvent,
} from 'react';

import {
  publicErrorMessage,
  type ChaClient,
  type CharacterDetail,
  type CommandResult,
  type SessionListing,
  type SessionSnapshot,
} from '../api/client';
import { sessionOperationState, type AppAction, type AppState } from '../state/view';
import { Markdown } from './Markdown';
import {
  ChevronLeftIcon,
  ChevronRightIcon,
  ForumsIcon,
  MessageIcon,
  PlusIcon,
  SendIcon,
  StopIcon,
  TargetIcon,
} from './Icons';

interface DiscoveryScreenProps {
  state: AppState;
  dispatch: Dispatch<AppAction>;
}

// The chat controls App owns, declared once so the screen and the router that
// feeds it cannot drift apart.
export interface ChatActions {
  onRetryStream(): void;
  onReturnToWelcome(): void;
  onSetDefaultCharacter(characterId: string): Promise<CommandResult>;
  onStopGeneration(): Promise<CommandResult>;
  onSubmitInput(text: string): Promise<CommandResult>;
}

type ChatScreenProps = DiscoveryScreenProps & ChatActions;

function actionMessage(failure: unknown): string {
  return publicErrorMessage(failure, 'The action could not be completed. Try again.');
}

// How close to the end still counts as following the conversation. A few pixels
// of slack absorbs sub-pixel rounding, which would otherwise unpin the view the
// first time it scrolled itself.
const followSlack = 24;

// A final snapshot arrives over a healthy stream, so a session ending is not a
// stream failure and has to explain itself. Without this the composer would
// simply go dead until the reconnect ladder gave up on a session that is gone.
function endedMessage(snapshot: SessionSnapshot): string {
  switch (snapshot.shutdown_reason) {
    case 'server_stopping':
      return 'CHA is shutting down. This conversation is saved.';
    case 'session_failed':
      return 'This session stopped after a failure. Its conversation is saved.';
    case 'browser_disconnected':
      return 'This session was released because the browser disconnected.';
    default:
      return 'This session is closing. Its conversation is saved.';
  }
}

export function ChatScreen({
  state,
  dispatch,
  onRetryStream,
  onReturnToWelcome,
  onSetDefaultCharacter,
  onStopGeneration,
  onSubmitInput,
}: ChatScreenProps) {
  const [draft, setDraft] = useState('');
  const [actionError, setActionError] = useState<string | null>(null);
  const [pendingAction, setPendingAction] = useState<'send' | 'stop' | 'target' | null>(null);
  const transcriptEnd = useRef<HTMLDivElement | null>(null);
  const followingLatest = useRef(true);
  const snapshot = state.sessionSnapshot;
  const generation = snapshot?.generation;
  const conversationKey = snapshot && `${snapshot.forum.id}/${snapshot.session_id}`;
  const persona = state.bootstrap?.personas.find(({ id }) => id === state.currentPersonaId);
  const forum = snapshot?.forum ?? state.bootstrap?.forums.find(
    ({ id }) => id === state.activeConversation?.forumId,
  );
  const character = snapshot?.characters.find(
    ({ id }) => id === state.currentDefaultCharacterId,
  ) ?? state.bootstrap?.characters.find(({ id }) => id === state.currentDefaultCharacterId);
  const ended = snapshot && snapshot.lifecycle !== 'running' ? endedMessage(snapshot) : null;
  const connected = state.streamStatus === 'connected' && snapshot !== null && !ended;
  const generationActive = generation?.active === true;
  const sessionAvailable = snapshot !== null && !ended;
  const canSend = connected && pendingAction === null && draft.trim().length > 0;

  // A different conversation starts at its own end rather than inheriting where
  // the reader had left the previous one.
  useEffect(() => {
    followingLatest.current = true;
  }, [conversationKey]);

  // Following the newest text is the default, but a reader who has scrolled up
  // keeps their place: a stream that yanked the view back on every token would
  // make the transcript unreadable exactly while it was worth reading.
  useEffect(() => {
    if (!followingLatest.current) return;
    if (typeof transcriptEnd.current?.scrollIntoView === 'function') {
      transcriptEnd.current.scrollIntoView({ block: 'end' });
    }
  }, [conversationKey, generation?.reasoning_text, snapshot?.transcript]);

  // Growing the transcript moves the end away without moving the viewport, so
  // whether the reader is following has to be recorded when they last scrolled
  // rather than measured after new text has already landed.
  function noteReadingPosition(event: UIEvent<HTMLDivElement>) {
    const { scrollHeight, scrollTop, clientHeight } = event.currentTarget;
    followingLatest.current = scrollHeight - scrollTop - clientHeight <= followSlack;
  }

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (generationActive || !canSend) return;
    setPendingAction('send');
    setActionError(null);
    try {
      const result = await onSubmitInput(draft);
      if (result.clear_input) setDraft('');
    } catch (failure: unknown) {
      // A failed send deliberately leaves the draft untouched.
      setActionError(actionMessage(failure));
    } finally {
      setPendingAction(null);
    }
  }

  async function stop() {
    if (!generationActive || !sessionAvailable || pendingAction) return;
    setPendingAction('stop');
    setActionError(null);
    try {
      await onStopGeneration();
    } catch (failure: unknown) {
      setActionError(actionMessage(failure));
    } finally {
      setPendingAction(null);
    }
  }

  async function chooseTarget(characterId: string) {
    if (!characterId || characterId === state.currentDefaultCharacterId || pendingAction) return;
    setPendingAction('target');
    setActionError(null);
    try {
      await onSetDefaultCharacter(characterId);
    } catch (failure: unknown) {
      setActionError(actionMessage(failure));
    } finally {
      setPendingAction(null);
    }
  }

  // While the stream is down its own narration is the more useful message, so
  // the ended notice speaks only for a session whose end arrived intact.
  const liveMessage = state.streamStatus === 'connected' ? ended : state.streamMessage;
  const showRecoveryActions = state.streamStatus === 'retry'
    || state.streamStatus === 'other-window'
    || (state.streamStatus === 'connected' && ended !== null);

  return (
    <section className="cha-screen cha-chat" aria-label="Chat area">
      <div
        aria-label="Conversation transcript"
        className="cha-transcript"
        onScroll={noteReadingPosition}
      >
        {(!snapshot || (snapshot.transcript.length === 0 && !generation?.reasoning_text)) && (
          <div className="cha-chat-welcome">
            <span className="cha-chat-kicker">
              {snapshot?.session_label ?? state.activeConversationLabel ?? 'Chat'}
            </span>
            <p>Start the conversation below.</p>
          </div>
        )}
        {snapshot?.transcript.map((entry) => (
          <article
            className={`cha-message is-${entry.kind}`}
            data-status={entry.status}
            key={entry.id}
          >
            {entry.kind !== 'human' && entry.display_name && (
              <div className="cha-speaker">{entry.display_name}</div>
            )}
            <div className="cha-message-text">{entry.text}</div>
            {entry.status === 'cancelled' && <div className="cha-entry-status">Stopped</div>}
            {entry.status === 'failed' && <div className="cha-entry-status">Failed</div>}
          </article>
        ))}
        {generation?.active && (
          <div className="cha-generation">
            {/* Only the phase is announced. Marking the region live would make
                a screen reader re-read the whole reasoning text per token. */}
            <div className="cha-speaker" aria-live="polite">
              {generation.phase === 'reasoning' && `${generation.agent_name} is reasoning…`}
              {generation.phase === 'answering' && `${generation.agent_name} is answering…`}
              {generation.phase === 'stopping' && `Stopping ${generation.agent_name}…`}
              {generation.phase === 'waiting' && `Waiting for ${generation.agent_name}…`}
            </div>
            {generation.reasoning_text && (
              <div className="cha-reasoning-text">{generation.reasoning_text}</div>
            )}
          </div>
        )}
        {snapshot?.notice && <p className="cha-session-notice">{snapshot.notice}</p>}
        <div ref={transcriptEnd} />
      </div>
      {liveMessage && (
        <div
          className={`cha-live-state ${showRecoveryActions ? 'cha-error-message' : ''}`}
          role={showRecoveryActions ? 'alert' : 'status'}
        >
          <span>{liveMessage}</span>
          {showRecoveryActions && (
            <span className="cha-live-actions">
              <button className="cha-button cha-button-ghost" onClick={onRetryStream} type="button">
                Retry
              </button>
              <button
                className="cha-button cha-button-ghost"
                onClick={() => {
                  if (snapshot) dispatch({ type: 'select-forum', forumId: snapshot.forum.id });
                  else dispatch({ type: 'show-forums' });
                }}
                type="button"
              >
                Browse sessions
              </button>
              <button className="cha-button cha-button-ghost" onClick={onReturnToWelcome} type="button">
                Return to Welcome
              </button>
            </span>
          )}
        </div>
      )}
      {actionError && (
        <p className="cha-chat-action-error cha-error-message" role="alert">{actionError}</p>
      )}
      <form className="cha-composer" onSubmit={submit}>
        <label className="cha-target-select" title="Choose target character">
          <TargetIcon />
          <select
            aria-label="Choose target character"
            disabled={!connected || pendingAction !== null}
            onChange={(event) => void chooseTarget(event.target.value)}
            value={state.currentDefaultCharacterId ?? ''}
          >
            {snapshot?.characters.map((member) => (
              <option key={member.id} value={member.id}>{member.display_name}</option>
            ))}
          </select>
        </label>
        <input
          aria-label="Message"
          autoComplete="off"
          disabled={!sessionAvailable}
          onChange={(event) => setDraft(event.target.value)}
          placeholder={`Message ${character?.display_name ?? 'character'}`}
          value={draft}
        />
        <button
          aria-label={generationActive ? 'Stop generation' : 'Send message'}
          className={`cha-composer-action ${generationActive ? 'cha-stop' : 'cha-send'}`}
          disabled={generationActive
            ? pendingAction !== null || !sessionAvailable
            : !canSend}
          onClick={generationActive ? () => void stop() : undefined}
          type={generationActive ? 'button' : 'submit'}
        >
          {generationActive ? <StopIcon /> : <SendIcon />}
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
  const [requestVersion, setRequestVersion] = useState(0);
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
          setError(publicErrorMessage(failure, 'Character detail could not be loaded.'));
        }
      },
    );
    return () => {
      current = false;
    };
  }, [characterId, client, requestVersion]);

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
      {error && (
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
  onRetrySession(): void;
  onReturnToWelcome(): void;
}

// Opening or creating a session is reported by the screen that started it, so
// the list stays on screen and a half-typed session name is not thrown away.
function SessionOperationReport({
  pending,
  failure,
  state,
  onRetrySession,
  onReturnToWelcome,
}: {
  pending: boolean;
  failure: string | null;
  state: AppState;
  onRetrySession(): void;
  onReturnToWelcome(): void;
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
    <div className="cha-state-message cha-error-message" role="alert">
      <p>{failure}</p>
      {state.sessionOperationRetryable && (
        <div className="cha-state-actions">
          <button className="cha-button cha-button-ghost" onClick={onRetrySession} type="button">
            Retry
          </button>
          <button className="cha-button cha-button-ghost" onClick={onReturnToWelcome} type="button">
            Return to Welcome
          </button>
        </div>
      )}
    </div>
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
  onRetrySession,
  onReturnToWelcome,
}: SessionsScreenProps) {
  const [sessions, setSessions] = useState<SessionListing[] | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [requestVersion, setRequestVersion] = useState(0);
  const forumId = state.currentForumId;
  const { pending: sessionPending, failure: sessionFailure } = sessionOperationState(state);
  // The forum bootstrap starts in is the built-in one, whose single session the
  // server synthesizes; it stores no forum of its own, so a create there fails
  // as not-found. Offering the action would only produce that error.
  const canCreateSessions = forumId !== state.bootstrap?.initial_forum_id;

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
  }, [client, forumId, requestVersion]);

  return (
    <section className="cha-screen cha-navigation" aria-label="Forum sessions navigation">
      <button className="cha-back-row" onClick={() => dispatch({ type: 'show-forums' })} type="button">
        <ChevronLeftIcon />
        <span>All forums</span>
      </button>
      {!forumId && <p className="cha-state-message">No forum is selected.</p>}
      <SessionOperationReport
        failure={sessionFailure}
        onRetrySession={onRetrySession}
        onReturnToWelcome={onReturnToWelcome}
        pending={sessionPending}
        state={state}
      />
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

interface NewSessionScreenProps extends DiscoveryScreenProps {
  onCreateSession(forumId: string, label: string): Promise<boolean>;
  onRetrySession(): void;
  onReturnToWelcome(): void;
}

export function NewSessionScreen({
  state,
  dispatch,
  onCreateSession,
  onRetrySession,
  onReturnToWelcome,
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
        onClick={() => dispatch({ type: 'show-sessions' })}
        type="button"
      >
        <ChevronLeftIcon />
        <span>Sessions</span>
      </button>
      <SessionOperationReport
        failure={sessionFailure}
        onRetrySession={onRetrySession}
        onReturnToWelcome={onReturnToWelcome}
        pending={sessionPending}
        state={state}
      />
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
            onClick={() => dispatch({ type: 'show-sessions' })}
            type="button"
          >
            Cancel
          </button>
          <button
            className="cha-button cha-button-primary"
            disabled={!trimmedName || sessionPending || state.sessionOperationRetryable}
            type="submit"
          >
            Start session
          </button>
        </div>
      </form>
    </section>
  );
}
