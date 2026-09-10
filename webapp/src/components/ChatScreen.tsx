import {
  Fragment,
  useEffect,
  useLayoutEffect,
  useMemo,
  useRef,
  useState,
  type Dispatch,
  type FormEvent,
  type KeyboardEvent,
  type PointerEvent,
  type UIEvent,
} from 'react';

import {
  publicErrorMessage,
  type CharacterAppearance,
  type CommandResult,
  type SessionSnapshot,
} from '../api/client';
import type { AppAction, AppState } from '../state/view';
import {
  appendTranscription,
  getVoiceInputConfiguration,
  VoiceInputSession,
} from '../voiceInput';
import {
  MicrophoneIcon,
  SendIcon,
  StopIcon,
  TargetIcon,
} from './Icons';
import { TransliterationToggle, useTransliteration } from './TransliterationMode';
import { voiceClasses } from './characterAppearance';

// The chat controls App owns, declared once so the screen and the router that
// feeds it cannot drift apart.
export interface ChatActions {
  onRetryStream(): void;
  onReturnToWelcome(): void;
  onSetDefaultCharacter(characterId: string): Promise<CommandResult>;
  onStopGeneration(): Promise<CommandResult>;
  onSubmitInput(text: string): Promise<CommandResult>;
}

interface ChatScreenProps extends ChatActions {
  state: AppState;
  dispatch: Dispatch<AppAction>;
}

function actionMessage(failure: unknown): string {
  return publicErrorMessage(failure, 'The action could not be completed. Try again.');
}

function voiceInputMessage(failure: unknown): string {
  return failure instanceof DOMException && failure.name === 'NotAllowedError'
    ? 'Microphone access was denied. Allow it in System Settings and try again.'
    : 'Voice input stopped because transcription failed. Try again.';
}

// How close to the end still counts as following the conversation. A few pixels
// of slack absorbs sub-pixel rounding, which would otherwise unpin the view the
// first time it scrolled itself.
const followSlack = 24;

// Older stored transcripts can contain a model-echoed UTC metadata line. Entry
// creation time already has its own UI below the message, so hide that legacy
// prefix here as well.
const echoedTimestampPrefix = /^\s*\[\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d+)?Z\]\s*/;

function visibleEntryText(kind: string, text: string): string {
  return kind === 'character' ? text.replace(echoedTimestampPrefix, '') : text;
}

type VisibleTranscriptEntry = {
  entry: SessionSnapshot['transcript'][number];
  dividerBefore: boolean;
};

function visibleTranscriptEntries(entries: SessionSnapshot['transcript']) {
  // Multicast stores one addressed prompt per character. Character replies do
  // not reset this comparison, so only the first copy is shown to the reader.
  // A divider keeps the omitted prompt from making adjacent replies run
  // together.
  let lastPrompt: { participantId: string; text: string } | null = null;
  let dividerBefore = false;
  const visible: VisibleTranscriptEntry[] = [];
  for (const entry of entries) {
    if (entry.kind === 'notice' && entry.text === '') continue;
    if (entry.kind === 'human') {
      const repeated = lastPrompt?.participantId === entry.participant_id
        && lastPrompt.text === entry.text;
      lastPrompt = { participantId: entry.participant_id, text: entry.text };
      if (repeated) {
        dividerBefore = true;
        continue;
      }
    }
    visible.push({ entry, dividerBefore });
    dividerBefore = false;
  }
  return visible;
}

function activeCoverMarkerId(entries: SessionSnapshot['transcript']): number | null {
  for (let index = entries.length - 1; index >= 0; index -= 1) {
    const entry = entries[index];
    const marker = entry.kind === 'notice'
      && entry.text === ''
      && (entry.display_name === 'cover' || entry.display_name === 'uncover');
    if (marker) return entry.display_name === 'cover' ? entry.id : null;
  }
  return null;
}

function TranscriptMessage({
  entry,
  appearance,
}: {
  entry: SessionSnapshot['transcript'][number];
  appearance: CharacterAppearance | undefined;
}) {
  return (
    <article
      className={`cha-message is-${entry.kind}`}
      data-status={entry.status}
    >
      {entry.kind !== 'human' && entry.display_name && (
        <div className="cha-speaker">{entry.display_name}</div>
      )}
      <div
        className={`cha-message-text${entry.kind === 'character'
          ? voiceClasses(appearance)
          : ''}`}
      >
        {visibleEntryText(entry.kind, entry.text)}
      </div>
      {entry.status === 'cancelled' && <div className="cha-entry-status">Stopped</div>}
      {entry.status === 'failed' && <div className="cha-entry-status">Failed</div>}
      {entry.created_at !== null && (
        <time
          className="cha-message-time"
          dateTime={new Date(entry.created_at * 1000).toISOString()}
          title={new Date(entry.created_at * 1000).toLocaleString()}
        >
          {formatEntryTime(entry.created_at)}
        </time>
      )}
    </article>
  );
}

// A final snapshot arrives over a healthy stream, so a session ending is not a
// stream failure and has to explain itself. Without this the composer would
// simply go dead until the reconnect ladder gave up on a session that is gone.
function endedMessage(snapshot: SessionSnapshot): string {
  switch (snapshot.shutdown_reason) {
    case 'server_stopping':
      return 'CHA is shutting down. This conversation is saved.';
    case 'session_failed':
      return 'This session stopped after a failure. Its conversation is saved.';
    case 'session_deleted':
      return 'This session was deleted.';
    case 'reloading':
      return 'Applying settings…';
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
  const draftRef = useRef('');

  function updateDraft(next: string) {
    draftRef.current = next;
    setDraft(next);
  }

  const transliteration = useTransliteration<HTMLTextAreaElement>(draft);
  const [actionError, setActionError] = useState<string | null>(null);
  const [pendingAction, setPendingAction] = useState<'send' | 'stop' | 'target' | null>(null);
  const [voiceInputState, setVoiceInputState] = useState<
    'idle' | 'starting' | 'recording' | 'finishing'
  >('idle');
  const voiceInputSession = useRef<VoiceInputSession | null>(null);
  const voiceInputAttempt = useRef(0);
  const transcriptEnd = useRef<HTMLDivElement | null>(null);
  const composerInput = transliteration.field;
  const chatArea = useRef<HTMLElement | null>(null);
  const composerResize = useRef<{
    pointerId: number;
    startHeight: number;
    startY: number;
  } | null>(null);
  const [composerHeight, setComposerHeight] = useState<number | null>(null);
  const followingLatest = useRef(true);
  const snapshot = state.sessionSnapshot;
  const generation = snapshot?.generation;
  const conversationKey = snapshot && `${snapshot.forum.id}/${snapshot.session_id}`;
  const forum = snapshot?.forum ?? state.bootstrap?.forums.find(
    ({ id }) => id === state.activeConversation?.forumId,
  );
  const character = snapshot?.characters.find(
    ({ id }) => id === state.currentDefaultCharacterId,
  ) ?? state.bootstrap?.characters.find(({ id }) => id === state.currentDefaultCharacterId);
  const recordingTarget = state.currentDefaultCharacterId === '-';
  const ended = snapshot && snapshot.lifecycle !== 'running' ? endedMessage(snapshot) : null;
  const connected = state.streamStatus === 'connected' && snapshot !== null && !ended;
  const generationActive = generation?.active === true;
  const sessionAvailable = snapshot !== null && !ended;
  const voiceConfiguration = getVoiceInputConfiguration();
  const voiceInputAvailable = voiceConfiguration !== null && VoiceInputSession.supported();
  const voiceInputActive = voiceInputState !== 'idle';
  const canSend = connected
    && pendingAction === null
    && voiceInputState !== 'starting'
    && voiceInputState !== 'finishing'
    && (draft.trim().length > 0 || voiceInputState === 'recording');
  const voices = useMemo(
    () => new Map((snapshot?.characters ?? []).map(({ id, appearance }) => [id, appearance])),
    [snapshot?.characters],
  );
  const transcriptEntries = snapshot ? visibleTranscriptEntries(snapshot.transcript) : [];
  const coverMarkerId = snapshot ? activeCoverMarkerId(snapshot.transcript) : null;
  const coveredEntries = coverMarkerId === null
    ? []
    : transcriptEntries.filter(({ entry }) => entry.id < coverMarkerId);
  const uncoveredEntries = coverMarkerId === null
    ? transcriptEntries
    : transcriptEntries.filter(({ entry }) => entry.id > coverMarkerId);

  // A different conversation starts at its own end rather than inheriting where
  // the reader had left the previous one.
  useEffect(() => {
    followingLatest.current = true;
  }, [conversationKey]);

  // A recording belongs to the conversation in which it started.
  useEffect(() => () => {
    voiceInputAttempt.current += 1;
    voiceInputSession.current?.cancel();
    voiceInputSession.current = null;
    setVoiceInputState('idle');
  }, [conversationKey, sessionAvailable]);

  // Following the newest text is the default, but a reader who has scrolled up
  // keeps their place: a stream that yanked the view back on every token would
  // make the transcript unreadable exactly while it was worth reading.
  useEffect(() => {
    if (!followingLatest.current) return;
    if (typeof transcriptEnd.current?.scrollIntoView === 'function') {
      transcriptEnd.current.scrollIntoView({ block: 'end' });
    }
  }, [conversationKey, generation?.reasoning_text, snapshot?.transcript]);

  function maximumComposerHeight(fallback = Number.POSITIVE_INFINITY) {
    const chatHeight = chatArea.current?.clientHeight ?? 0;
    return chatHeight > 0 ? chatHeight * 0.8 : fallback;
  }

  // A textarea follows its wrapped content until the reader gives it a larger
  // floor. Both automatic and manual growth stop at 80% of the chat area.
  useLayoutEffect(() => {
    const input = composerInput.current;
    if (!input) return;
    input.style.height = 'auto';
    const maximum = maximumComposerHeight();
    input.style.height = `${Math.min(maximum, Math.max(input.scrollHeight, composerHeight ?? 0))}px`;
  }, [composerHeight, draft]);

  // Growing the transcript moves the end away without moving the viewport, so
  // whether the reader is following has to be recorded when they last scrolled
  // rather than measured after new text has already landed.
  function noteReadingPosition(event: UIEvent<HTMLDivElement>) {
    const { scrollHeight, scrollTop, clientHeight } = event.currentTarget;
    followingLatest.current = scrollHeight - scrollTop - clientHeight <= followSlack;
  }

  function resizeComposer(height: number) {
    const input = composerInput.current;
    if (!input) return;
    const maximum = maximumComposerHeight(height);
    // Once the textarea is taller than its contents, browsers include that
    // assigned height in scrollHeight. Measure it without the assigned height
    // so dragging down can reach the real content height again.
    const assignedHeight = input.style.height;
    input.style.height = 'auto';
    const minimum = Math.min(input.scrollHeight, maximum);
    input.style.height = assignedHeight;
    setComposerHeight(Math.min(maximum, Math.max(minimum, height)));
  }

  function startComposerResize(event: PointerEvent<HTMLDivElement>) {
    const input = composerInput.current;
    if (!input) return;
    event.preventDefault();
    event.currentTarget.setPointerCapture?.(event.pointerId);
    composerResize.current = {
      pointerId: event.pointerId,
      startHeight: input.offsetHeight || input.scrollHeight,
      startY: event.clientY,
    };
  }

  function continueComposerResize(event: PointerEvent<HTMLDivElement>) {
    const resize = composerResize.current;
    if (!resize || resize.pointerId !== event.pointerId) return;
    resizeComposer(resize.startHeight + resize.startY - event.clientY);
  }

  function finishComposerResize(event: PointerEvent<HTMLDivElement>) {
    if (composerResize.current?.pointerId !== event.pointerId) return;
    composerResize.current = null;
    event.currentTarget.releasePointerCapture?.(event.pointerId);
  }

  function resizeComposerWithKeyboard(event: KeyboardEvent<HTMLDivElement>) {
    if (event.key === 'Home') {
      event.preventDefault();
      setComposerHeight(null);
      return;
    }
    if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
    event.preventDefault();
    const input = composerInput.current;
    if (!input) return;
    resizeComposer((input.offsetHeight || input.scrollHeight)
      + (event.key === 'ArrowUp' ? 24 : -24));
  }

  async function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (generationActive || !canSend) return;
    setPendingAction('send');
    setActionError(null);
    try {
      if (!await finishVoiceInput()) return;
      const submitted = draftRef.current;
      if (!submitted.trim()) return;
      const result = await onSubmitInput(submitted);
      // Typing may continue while the send is in flight; only the text that was
      // actually sent is cleared.
      if (result.clear_input) {
        const current = draftRef.current;
        updateDraft(current === submitted ? '' : current);
      }
    } catch (failure: unknown) {
      // A failed send deliberately leaves the draft untouched.
      setActionError(actionMessage(failure));
    } finally {
      setPendingAction(null);
    }
  }

  // Enter is the quick way to send. Ctrl+Enter inserts a line explicitly: it
  // is not a consistently native textarea shortcut, so relying on the browser
  // would make multiline drafts depend on the platform. IME composition has to
  // finish before Enter can be interpreted as the command.
  function submitOnEnter(event: KeyboardEvent<HTMLTextAreaElement>) {
    if (event.key !== 'Enter' || event.nativeEvent.isComposing) return;
    if (event.ctrlKey) {
      event.preventDefault();
      const { selectionEnd, selectionStart } = event.currentTarget;
      const current = draftRef.current;
      updateDraft(`${current.slice(0, selectionStart)}\n${current.slice(selectionEnd)}`);
      return;
    }
    event.preventDefault();
    if (!canSend || generationActive) return;
    event.currentTarget.form?.requestSubmit();
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

  async function finishVoiceInput(): Promise<boolean> {
    const session = voiceInputSession.current;
    if (!session) return true;
    setVoiceInputState('finishing');
    try {
      await session.stop();
      if (voiceInputSession.current === session) voiceInputSession.current = null;
      setVoiceInputState('idle');
      return true;
    } catch (failure: unknown) {
      if (voiceInputSession.current === session) voiceInputSession.current = null;
      setVoiceInputState('idle');
      setActionError(voiceInputMessage(failure));
      return false;
    }
  }

  async function toggleVoiceInput() {
    if (voiceInputSession.current) {
      await finishVoiceInput();
      return;
    }
    if (!voiceConfiguration || !voiceInputAvailable || !sessionAvailable) return;

    const attempt = ++voiceInputAttempt.current;
    let receivedVoiceDelta = false;
    setVoiceInputState('starting');
    setActionError(null);
    let session: VoiceInputSession | null = null;
    try {
      session = await VoiceInputSession.start(
        {
          ...voiceConfiguration,
          languages: [transliteration.enabled ? 'ru' : 'en'],
        },
        (text) => {
          if (voiceInputAttempt.current !== attempt) return;
          if (!text) return;
          updateDraft(receivedVoiceDelta
            ? draftRef.current + text
            : appendTranscription(draftRef.current, text));
          receivedVoiceDelta = true;
        },
        (failure) => {
          if (voiceInputAttempt.current !== attempt) return;
          voiceInputSession.current = null;
          setVoiceInputState('idle');
          setActionError(voiceInputMessage(failure));
        },
      );
      if (voiceInputAttempt.current !== attempt) {
        session.cancel();
        return;
      }
      voiceInputSession.current = session;
      setVoiceInputState('recording');
    } catch (failure: unknown) {
      if (voiceInputAttempt.current !== attempt) return;
      setVoiceInputState('idle');
      setActionError(voiceInputMessage(failure));
    }
  }

  const voiceInputLabel = voiceInputState === 'idle'
    ? 'Start voice input'
    : voiceInputState === 'recording'
      ? 'Stop voice input'
      : voiceInputState === 'starting'
        ? 'Starting voice input'
        : 'Finishing transcription';

  // While the stream is down its own narration is the more useful message, so
  // the ended notice speaks only for a session whose end arrived intact.
  const liveMessage = state.streamStatus === 'connected' ? ended : state.streamMessage;
  // A settings save ends the session deliberately and the ladder reopens it,
  // so that final snapshot narrates itself instead of offering recovery. Only
  // it is exempt: a ladder that has given up still needs its buttons, and its
  // stale reason must not take them away.
  const showRecoveryActions = state.streamStatus === 'retry'
    || state.streamStatus === 'moved'
    || (state.streamStatus === 'connected'
      && ended !== null
      && snapshot?.shutdown_reason !== 'reloading');

  return (
    <section className="cha-screen cha-chat" aria-label="Chat area" ref={chatArea}>
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
        {coveredEntries.length > 0 && (
          <section aria-label="Covered conversation" className="cha-covered">
            {coveredEntries.map(({ entry, dividerBefore }) => (
              <Fragment key={entry.id}>
                {dividerBefore && <hr className="cha-repeated-prompt-divider" />}
                <TranscriptMessage
                  appearance={voices.get(entry.participant_id)}
                  entry={entry}
                />
              </Fragment>
            ))}
          </section>
        )}
        {uncoveredEntries.map(({ entry, dividerBefore }) => (
          <Fragment key={entry.id}>
            {dividerBefore && <hr className="cha-repeated-prompt-divider" />}
            <TranscriptMessage
              appearance={voices.get(entry.participant_id)}
              entry={entry}
            />
          </Fragment>
        ))}
        {generation?.active && (
          <div className="cha-generation">
            {/* Only the phase is announced. Marking the region live would make
                a screen reader re-read the whole reasoning text per token. */}
            <div className="cha-speaker" aria-live="polite">
              {generation.phase === 'reasoning' && `${generation.character_display_name} is reasoning…`}
              {generation.phase === 'answering' && `${generation.character_display_name} is answering…`}
              {generation.phase === 'stopping' && `Stopping ${generation.character_display_name}…`}
              {generation.phase === 'waiting' && `Waiting for ${generation.character_display_name}…`}
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
                {state.streamStatus === 'moved' ? 'Continue here' : 'Retry'}
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
      <div className="cha-composer-area">
        <div
          aria-label="Resize message editor"
          aria-orientation="horizontal"
          className="cha-composer-resize"
          onDoubleClick={() => setComposerHeight(null)}
          onKeyDown={resizeComposerWithKeyboard}
          onPointerCancel={finishComposerResize}
          onPointerDown={startComposerResize}
          onPointerMove={continueComposerResize}
          onPointerUp={finishComposerResize}
          role="separator"
          tabIndex={0}
          title="Drag to resize; double-click to reset"
        />
        <form className="cha-composer" onSubmit={submit}>
          <textarea
            aria-label="Message"
            autoComplete="off"
            disabled={!sessionAvailable}
            onChange={(event) => {
              const next = transliteration.convert(event, draft);
              updateDraft(next);
            }}
            onKeyDown={submitOnEnter}
            placeholder={recordingTarget
              ? 'Recording — saved, not sent'
              : `Message ${character?.display_name ?? 'character'}`}
            ref={composerInput}
            rows={1}
            value={draft}
          />
          <div className="cha-composer-controls">
            <label className="cha-target-select" title="Choose target character">
              <TargetIcon />
              <select
                aria-label="Choose target character"
                disabled={!connected || pendingAction !== null}
                onChange={(event) => void chooseTarget(event.target.value)}
                value={state.currentDefaultCharacterId ?? ''}
              >
                {recordingTarget && <option value="-">Recording</option>}
                {snapshot?.characters.map((member) => (
                  <option key={member.id} value={member.id}>{member.display_name}</option>
                ))}
              </select>
            </label>
            <div className="cha-composer-actions">
              {voiceInputAvailable && (
                <button
                  aria-label={voiceInputLabel}
                  aria-pressed={voiceInputActive}
                  className={`cha-composer-action cha-voice-input${voiceInputActive ? ' is-active' : ''}`}
                  disabled={!sessionAvailable
                    || pendingAction !== null
                    || voiceInputState === 'starting'
                    || voiceInputState === 'finishing'}
                  onClick={() => void toggleVoiceInput()}
                  title={voiceInputLabel}
                  type="button"
                >
                  <MicrophoneIcon />
                </button>
              )}
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
            </div>
          </div>
        </form>
        <div className="cha-chat-status" aria-label="Current chat context">
          <span>{forum?.display_name ?? 'Unknown forum'}</span>
          <span>From: {forum?.default_persona_display_name ?? 'Unknown persona'}</span>
          <span>To: {recordingTarget ? 'Recording' : (character?.display_name ?? 'Unknown character')}</span>
          <TransliterationToggle
            disabled={!sessionAvailable}
            transliteration={transliteration}
          />
        </div>
      </div>
    </section>
  );
}

// Relative session-list labels go stale on an open transcript.
export function formatEntryTime(createdAt: number, now = Date.now()): string {
  const date = new Date(createdAt * 1000);
  const current = new Date(now);
  const time = date.toLocaleTimeString(undefined, { hour: '2-digit', minute: '2-digit' });
  const day = date.toLocaleDateString(undefined, {
    month: 'short',
    day: 'numeric',
    ...(date.getFullYear() === current.getFullYear() ? {} : { year: 'numeric' as const }),
  });
  return `${day}, ${time}`;
}
