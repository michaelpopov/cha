import {
  Fragment,
  useCallback,
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
  ChaError,
  publicErrorMessage,
  type AudioDownloadBatchEntry,
  type ChaClient,
  type CharacterAppearance,
  type CommandResult,
  type SessionSnapshot,
} from '../api/client';
import { useAudioDownloads } from '../audioDownloads';
import type { AppAction, AppState } from '../state/view';
import {
  nativeSpeechFromClient,
  TextToSpeechError,
  TextToSpeechSession,
  useTextToSpeechConfiguration,
} from '../textToSpeech';
import {
  appendTranscription,
  VoiceInputSession,
  type VoiceInputConfiguration,
} from '../voiceInput';
import { ConfirmDialog } from './ConfirmDialog';
import {
  EyeIcon,
  EyeOffIcon,
  MicrophoneIcon,
  SendIcon,
  SpeakerIcon,
  StopIcon,
  TargetIcon,
  TrashIcon,
} from './Icons';
import { TransliterationToggle, useTransliteration } from './TransliterationMode';
import { voiceClasses } from './characterAppearance';

// The chat controls App owns, declared once so the screen and the router that
// feeds it cannot drift apart.
export interface ChatActions {
  onCoverConversation(throughEntryId: number): Promise<CommandResult>;
  onDeleteTurn(responseEntryId: number): Promise<CommandResult>;
  onRetryStream(): void;
  onReturnToWelcome(): void;
  onSetDefaultCharacter(characterId: string): Promise<CommandResult>;
  onStopGeneration(): Promise<CommandResult>;
  onSubmitInput(text: string): Promise<CommandResult>;
  onUncoverConversation(): Promise<CommandResult>;
}

interface ChatScreenProps extends ChatActions {
  playbackPositions: Map<string, Map<number, number>>;
  client: ChaClient;
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
const allCharactersTarget = '*';
function multicastSubmission(text: string): string {
  if (text.startsWith('/')) return text;
  const firstText = text.search(/\S/);
  const escaped = firstText >= 0 && text[firstText] === '@'
    ? `${text.slice(0, firstText)}@${text.slice(firstText)}`
    : text;
  return `/mcast ${escaped}`;
}

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

function canReadEntry(entry: SessionSnapshot['transcript'][number]): boolean {
  return (entry.kind === 'human' || entry.kind === 'character')
    && entry.status === 'complete' && entry.created_at !== null;
}

function TranscriptMessage({
  entry,
  appearance,
  speechState,
  speechAvailable,
  speechError,
  onToggleSpeech,
  actionDisabled,
  onCover,
  onUncover,
  onDelete,
}: {
  entry: SessionSnapshot['transcript'][number];
  appearance: CharacterAppearance | undefined;
  speechState: 'idle' | 'queued' | 'running' | 'failed' | 'loading' | 'playing';
  speechError?: string;
  speechAvailable: boolean;
  onToggleSpeech(entry: SessionSnapshot['transcript'][number]): void;
  actionDisabled: boolean;
  onCover?: (entry: SessionSnapshot['transcript'][number]) => void;
  onUncover?: () => void;
  onDelete?: (entry: SessionSnapshot['transcript'][number]) => void;
}) {
  const canRead = canReadEntry(entry);
  const contextTokens = entry.kind === 'character'
    && entry.input_tokens != null
    && entry.output_tokens != null
    ? entry.input_tokens + entry.output_tokens
    : null;
  const canCover = entry.kind === 'character'
    && (entry.status === 'complete' || entry.status === 'cancelled')
    && entry.created_at !== null;
  const canDelete = canCover && entry.request_id !== undefined;
  const spokenItem = entry.kind === 'human'
    ? 'your prompt'
    : `${entry.display_name}'s response`;
  const speechLabel = speechState === 'queued'
    ? `Queued audio for ${spokenItem}`
    : speechState === 'running' ? `Generating audio for ${spokenItem}`
    : speechState === 'failed' ? `Retry audio for ${spokenItem}`
    : speechState === 'loading'
    ? `${entry.has_cached_audio ? 'Loading' : 'Generating'} audio for ${spokenItem}`
    : speechState === 'playing'
      ? `Stop reading ${spokenItem}`
      : `${entry.has_cached_audio ? 'Play cached audio' : 'Generate audio'} for ${spokenItem}`;
  const speechTitle = speechState === 'idle'
    ? entry.has_cached_audio ? 'Play cached audio' : 'Generate audio'
    : speechLabel;
  const coverLabel = onUncover
    ? 'Uncover transcript'
    : `Cover transcript through ${entry.display_name}'s response`;
  return (
    <article
      className={`cha-message is-${entry.kind}`}
      data-status={entry.status}
    >
      {entry.kind !== 'human' && entry.display_name && (
        <div className="cha-speaker">{entry.display_name}</div>
      )}
      <div
        className={`cha-message-text${voiceClasses(
          entry.kind === 'human' || entry.kind === 'character' ? appearance : undefined,
        )}`}
      >
        {visibleEntryText(entry.kind, entry.text)}
      </div>
      {entry.status === 'cancelled' && <div className="cha-entry-status">Stopped</div>}
      {entry.status === 'failed' && <div className="cha-entry-status">Failed</div>}
      {(entry.created_at !== null || contextTokens !== null) && (
        <div className="cha-message-meta">
          {entry.created_at !== null && (
            <time
              className="cha-message-time"
              dateTime={new Date(entry.created_at * 1000).toISOString()}
              title={new Date(entry.created_at * 1000).toLocaleString()}
            >
              {formatEntryTime(entry.created_at)}
            </time>
          )}
          {contextTokens !== null && (
            <span
              className="cha-message-tokens"
              title={`${contextTokens.toLocaleString()} context tokens`}
            >
              {formatTokenUsage(contextTokens)}
            </span>
          )}
          {canRead && speechAvailable && (
            <button
              aria-label={speechLabel}
              className={`cha-message-action${speechState !== 'idle' ? ' is-active' : ''}${entry.has_cached_audio && speechState !== 'playing' ? ' has-cached-audio' : ''}`}
              data-speech-state={speechState}
              disabled={speechState === 'loading' || speechState === 'queued' || speechState === 'running'}
              onClick={() => onToggleSpeech(entry)}
              title={speechTitle}
              type="button"
            >
              {speechState === 'playing' ? <StopIcon /> : <SpeakerIcon />}
            </button>
          )}
          {speechError && <span role="alert">{speechError}</span>}
          {canCover && (onCover || onUncover) && (
            <button
              aria-label={coverLabel}
              className={`cha-message-action${onUncover ? ' is-active' : ''}`}
              disabled={actionDisabled}
              onClick={() => onUncover ? onUncover() : onCover?.(entry)}
              title={coverLabel}
              type="button"
            >
              {onUncover ? <EyeIcon /> : <EyeOffIcon />}
            </button>
          )}
          {canDelete && onDelete && (
            <button
              aria-label={`Delete ${entry.display_name}'s response and its prompt`}
              className="cha-message-action cha-danger-icon-action"
              disabled={actionDisabled}
              onClick={() => onDelete(entry)}
              title={`Delete ${entry.display_name}'s response and its prompt`}
              type="button"
            >
              <TrashIcon />
            </button>
          )}
        </div>
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
    case 'session_closed':
      return 'This session has closed. Its conversation is saved.';
    default:
      return 'This session is closing. Its conversation is saved.';
  }
}

export function ChatScreen({
  playbackPositions,
  client,
  state,
  dispatch,
  onRetryStream,
  onReturnToWelcome,
  onCoverConversation,
  onDeleteTurn,
  onSetDefaultCharacter,
  onStopGeneration,
  onSubmitInput,
  onUncoverConversation,
}: ChatScreenProps) {
  const [draft, setDraft] = useState('');
  const [sendToAll, setSendToAll] = useState(false);
  const draftRef = useRef('');

  function updateDraft(next: string) {
    draftRef.current = next;
    setDraft(next);
  }

  const transliteration = useTransliteration<HTMLTextAreaElement>(draft);
  const [actionError, setActionError] = useState<string | null>(null);
  const [pendingAction, setPendingAction] = useState<
    'send' | 'stop' | 'target' | 'cover' | 'delete' | null
  >(null);
  const [voiceInputState, setVoiceInputState] = useState<
    'idle' | 'starting' | 'recording' | 'finishing'
  >('idle');
  const [voiceConfiguration, setVoiceConfiguration] =
    useState<VoiceInputConfiguration | null>(null);
  const voiceInputSession = useRef<VoiceInputSession | null>(null);
  const voiceInputStartup = useRef<AbortController | null>(null);
  const voiceInputAttempt = useRef(0);
  const transcriptEnd = useRef<HTMLDivElement | null>(null);
  const textToSpeechSession = useRef<TextToSpeechSession | null>(null);
  const [speechCacheEnabled, setSpeechCacheEnabled] = useState(false);
  const [speechCacheSubmitting, setSpeechCacheSubmitting] = useState(false);
  const handledAudioEntries = useRef(new Set<number>());
  const audioCacheContext = useRef<{ key: string | null; clearCount: number }>({
    key: null, clearCount: state.audioCacheClearCount,
  });
  const [spokenEntry, setSpokenEntry] = useState<{
    id: number;
    state: 'loading' | 'playing';
  } | null>(null);
  const [turnToDelete, setTurnToDelete] = useState<{
    id: number;
    displayName: string;
  } | null>(null);
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
  const audioConversationKey = snapshot && JSON.stringify([
    state.bootstrap?.vault_name, snapshot.forum.id, snapshot.session_id,
  ]);
  const forum = snapshot?.forum ?? state.bootstrap?.forums.find(
    ({ id }) => id === state.activeConversation?.forumId,
  );
  const character = snapshot?.characters.find(
    ({ id }) => id === state.currentDefaultCharacterId,
  ) ?? state.bootstrap?.characters.find(({ id }) => id === state.currentDefaultCharacterId);
  const recordingDefault = state.currentDefaultCharacterId === '-';
  const recordingTarget = recordingDefault && !sendToAll;
  const ended = snapshot && snapshot.lifecycle !== 'running' ? endedMessage(snapshot) : null;
  const connected = state.streamStatus === 'connected' && snapshot !== null && !ended;
  const generationActive = generation?.active === true;
  const sessionAvailable = snapshot !== null && !ended;
  const voiceInputAvailable = voiceConfiguration !== null && VoiceInputSession.supported();
  const textToSpeechConfiguration = useTextToSpeechConfiguration(client);
  const downloads = useAudioDownloads(client, snapshot?.forum.id, snapshot?.session_id,
    state.bootstrap?.vault_name, state.audioCacheClearCount);
  const speechSelection = useRef<number | null>(null);
  const speechAttempt = useRef(0);
  const voiceInputActive = voiceInputState !== 'idle';
  const canSend = connected
    && pendingAction === null
    && voiceInputState !== 'starting'
    && voiceInputState !== 'finishing'
    && (draft.trim().length > 0 || voiceInputState === 'recording');
  const appearances = useMemo(
    () => new Map((snapshot?.characters ?? []).map(({ id, appearance }) => [id, appearance])),
    [snapshot?.characters],
  );
  const speechVoices = useMemo(
    () => new Map((snapshot?.characters ?? []).map(({ id, voice }) => [id, voice])),
    [snapshot?.characters],
  );
  const personas = useMemo(
    () => new Map((state.bootstrap?.personas ?? []).map((persona) => [persona.id, persona])),
    [state.bootstrap?.personas],
  );
  const cachedAudioIds = useMemo(() => downloads.status && new Set(downloads.status.cached_entry_ids), [downloads.status]);
  const audioJobs = useMemo(() => new Map(downloads.status?.downloads.map((job) => [job.entry_id, job]) ?? []), [downloads.status]);
  const transcriptEntries = useMemo(
    () => snapshot ? visibleTranscriptEntries(snapshot.transcript.map((entry) => cachedAudioIds
      ? { ...entry, has_cached_audio: cachedAudioIds.has(entry.id) }
      : entry)) : [],
    [snapshot?.transcript, cachedAudioIds],
  );
  const coveredUntil = snapshot?.covered_until ?? null;
  const coveredEntries = coveredUntil === null
    ? []
    : transcriptEntries.filter(({ entry }) => entry.id < coveredUntil);
  const uncoveredEntries = coveredUntil === null
    ? transcriptEntries
    : transcriptEntries.filter(({ entry }) => entry.id >= coveredUntil);
  const boundaryEntryId = coveredEntries.length > 0
    ? coveredEntries[coveredEntries.length - 1].entry.id
    : null;

  useEffect(() => {
    let current = true;
    void client.getVoiceInputRuntime().then(
      (configuration) => {
        if (!current) return;
        setVoiceConfiguration(configuration ? {
          model: configuration.model,
          delay: configuration.delay,
          prompt: configuration.prompt,
        } : null);
      },
      () => { if (current) setVoiceConfiguration(null); },
    );
    return () => { current = false; };
  }, [client]);

  // A different conversation starts with a fresh composer at its own end.
  useEffect(() => {
    followingLatest.current = true;
    setSendToAll(false);
    setTurnToDelete(null);
    updateDraft('');
  }, [conversationKey]);

  // A recording belongs to the conversation in which it started.
  useEffect(() => () => {
    voiceInputAttempt.current += 1;
    voiceInputStartup.current?.abort();
    voiceInputStartup.current = null;
    voiceInputSession.current?.cancel();
    voiceInputSession.current = null;
    setVoiceInputState('idle');
  }, [conversationKey, sessionAvailable]);

  useEffect(() => () => {
    speechAttempt.current += 1;
    textToSpeechSession.current?.stop();
    textToSpeechSession.current = null;
    setSpokenEntry(null);
    speechSelection.current = null;
  }, [conversationKey, state.audioCacheClearCount]);

  const speechRequest = useCallback((entry: SessionSnapshot['transcript'][number]): AudioDownloadBatchEntry => {
    const voice = entry.kind === 'character'
      ? speechVoices.get(entry.participant_id) : personas.get(entry.participant_id)?.voice;
    return { entry_id: entry.id,
      reference_id: voice?.elevenlabs_voice_id ?? textToSpeechConfiguration!.voiceId,
      settings: voice?.settings ?? {},
    };
  }, [speechVoices, personas, textToSpeechConfiguration]);

  useEffect(() => {
    // Reset once when this observer changes sessions/vaults or clears audio.
    const context = audioCacheContext.current;
    if (context.key !== audioConversationKey || context.clearCount !== state.audioCacheClearCount) {
      audioCacheContext.current = { key: audioConversationKey, clearCount: state.audioCacheClearCount };
      handledAudioEntries.current.clear();
      setSpeechCacheEnabled(false);
      return;
    }
    if (downloads.unavailable) { setSpeechCacheEnabled(false); return; }
    if (!speechCacheEnabled || speechCacheSubmitting || !textToSpeechConfiguration || !downloads.status) return;
    const entries: AudioDownloadBatchEntry[] = [];
    for (const { entry } of transcriptEntries) {
      if (handledAudioEntries.current.has(entry.id) || !canReadEntry(entry)) continue;
      handledAudioEntries.current.add(entry.id);
      if (cachedAudioIds?.has(entry.id) || audioJobs.has(entry.id) || !visibleEntryText(entry.kind, entry.text).trim()) continue;
      entries.push(speechRequest(entry));
    }
    if (entries.length === 0) return;
    // One short acceptance request for the batch. The core owns the queue,
    // three concurrent downloads and retries; no browser transfer is awaited.
    setSpeechCacheSubmitting(true);
    void downloads.submitBatch({ vault_name: state.bootstrap!.vault_name, entries })
      ?.catch((failure: unknown) => {
        setSpeechCacheEnabled(false);
        setActionError(actionMessage(failure));
      })
      .finally(() => setSpeechCacheSubmitting(false));
  }, [audioConversationKey, state.audioCacheClearCount, speechCacheEnabled, speechCacheSubmitting,
    textToSpeechConfiguration, transcriptEntries, cachedAudioIds, audioJobs, downloads.status,
    downloads.unavailable, downloads.submitBatch, speechRequest, state.bootstrap?.vault_name]);

  function toggleSpeechCache() {
    setActionError(null);
    if (!speechCacheEnabled) handledAudioEntries.current.clear();
    setSpeechCacheEnabled(!speechCacheEnabled);
  }

  function toggleSpeech(entry: SessionSnapshot['transcript'][number]) {
    if (!snapshot || (!entry.has_cached_audio && !textToSpeechConfiguration)) return;
    speechAttempt.current += 1;
    if (speechSelection.current === entry.id && textToSpeechSession.current) {
      textToSpeechSession.current?.stop();
      textToSpeechSession.current = null;
      speechSelection.current = null;
      setSpokenEntry(null);
      return;
    }
    textToSpeechSession.current?.stop();
    textToSpeechSession.current = null;
    speechSelection.current = entry.id;
    setSpokenEntry(null);
    setActionError(null);
    if (entry.has_cached_audio) { playCached(entry); return; }
    if (!textToSpeechConfiguration) return;
    const { entry_id, ...request } = speechRequest(entry);
    void downloads.submit(entry_id, { vault_name: state.bootstrap!.vault_name, ...request })?.catch((failure: unknown) => {
      if (speechSelection.current === entry.id) {
        speechSelection.current = null;
      }
      setActionError(actionMessage(failure));
    });
  }

  function playCached(entry: SessionSnapshot['transcript'][number]) {
    if (!snapshot) return;
    const attempt = speechAttempt.current;
    const playbackKey = JSON.stringify([state.bootstrap?.vault_name, snapshot.forum.id, snapshot.session_id]);
    let positions = playbackPositions.get(playbackKey);
    if (!positions) {
      positions = new Map<number, number>();
      playbackPositions.set(playbackKey, positions);
    }
    const entryPositions = positions;
    const begin = (cachedUrl: string, resourceId?: string) => {
      const session = new TextToSpeechSession(
        textToSpeechConfiguration,
        entry.kind === 'character'
          ? speechVoices.get(entry.participant_id)
          : personas.get(entry.participant_id)?.voice,
        visibleEntryText(entry.kind, entry.text),
        () => {
          if (textToSpeechSession.current !== session) return;
          textToSpeechSession.current = null;
          setSpokenEntry(null);
          speechSelection.current = null;
        },
        {
          position: entryPositions.get(entry.id) ?? 0,
          onPositionChange: (position) => {
            if (position > 0) entryPositions.set(entry.id, position);
            else entryPositions.delete(entry.id);
          },
        },
        cachedUrl,
        () => dispatch({
          type: 'session-audio-cache', forumId: snapshot.forum.id,
          sessionId: snapshot.session_id, entryId: entry.id, cached: true,
        }),
        undefined,
        resourceId
          ? () => { void client.releaseResource(resourceId); }
          : undefined,
      );
      textToSpeechSession.current = session;
      setSpokenEntry({ id: entry.id, state: 'loading' });
      setActionError(null);
      void session.play().then(() => {
        if (textToSpeechSession.current === session) {
          setSpokenEntry({ id: entry.id, state: 'playing' });
        }
      }).catch((failure: unknown) => {
        if (textToSpeechSession.current !== session) return;
        session.stop();
        textToSpeechSession.current = null;
        setSpokenEntry(null);
        speechSelection.current = null;
        if (failure instanceof TextToSpeechError && failure.status === 404) {
          dispatch({ type: 'session-audio-cache', forumId: snapshot.forum.id,
            sessionId: snapshot.session_id, entryId: entry.id, cached: false });
          downloads.refresh(entry.id);
          return;
        }
        if (!(failure instanceof DOMException && failure.name === 'AbortError')) {
          setActionError(failure instanceof TextToSpeechError
            ? failure.message
            : 'This message could not be read aloud. Try again.');
        }
      });
    };
    void client.resolveAudioSource(
      snapshot.forum.id, snapshot.session_id, entry.id, state.bootstrap!.vault_name,
    ).then((resource) => {
      if (speechAttempt.current !== attempt || speechSelection.current !== entry.id) {
        void client.releaseResource(resource.resource_id);
        return;
      }
      begin(resource.url, resource.resource_id);
    }).catch((failure: unknown) => {
      if (speechAttempt.current !== attempt || speechSelection.current !== entry.id) return;
      speechSelection.current = null;
      setSpokenEntry(null);
      if ((failure instanceof TextToSpeechError && failure.status === 404)
          || (failure instanceof ChaError && failure.code === 'not_found')) {
        dispatch({ type: 'session-audio-cache', forumId: snapshot.forum.id,
          sessionId: snapshot.session_id, entryId: entry.id, cached: false });
        downloads.refresh(entry.id);
        return;
      }
      setActionError(publicErrorMessage(
        failure, 'This message could not be read aloud. Try again.'));
    });
  }

  useEffect(() => {
    const id = speechSelection.current;
    if (id === null || textToSpeechSession.current || !downloads.status) return;
    const entry = snapshot?.transcript.find((entry) => entry.id === id);
    if (!entry) { speechSelection.current = null; return; }
    if (cachedAudioIds?.has(id)) playCached(entry);
    else {
      const job = audioJobs.get(id);
      if (!job || job.state === 'failed') speechSelection.current = null;
    }
  }, [downloads.status]);

  async function changeCover(throughEntryId?: number) {
    if (!connected || generationActive || pendingAction) return;
    setPendingAction('cover');
    setActionError(null);
    try {
      if (throughEntryId === undefined) await onUncoverConversation();
      else await onCoverConversation(throughEntryId);
    } catch (failure: unknown) {
      setActionError(actionMessage(failure));
    } finally {
      setPendingAction(null);
    }
  }

  async function deleteTurn(responseEntryId: number) {
    if (!connected || generationActive || pendingAction) return;
    setPendingAction('delete');
    setActionError(null);
    try {
      if (spokenEntry?.id === responseEntryId) {
        textToSpeechSession.current?.stop();
        textToSpeechSession.current = null;
        setSpokenEntry(null);
      }
      await onDeleteTurn(responseEntryId);
    } catch (failure: unknown) {
      setActionError(actionMessage(failure));
    } finally {
      setPendingAction(null);
    }
  }

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
      const result = await onSubmitInput(
        sendToAll ? multicastSubmission(submitted) : submitted,
      );
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
    if (!characterId || pendingAction) return;
    if (characterId === allCharactersTarget) {
      setSendToAll(true);
      return;
    }
    if (characterId === state.currentDefaultCharacterId) {
      setSendToAll(false);
      return;
    }
    setPendingAction('target');
    setActionError(null);
    try {
      await onSetDefaultCharacter(characterId);
      setSendToAll(false);
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
    if (voiceInputState === 'starting') {
      voiceInputAttempt.current += 1;
      voiceInputStartup.current?.abort();
      voiceInputStartup.current = null;
      setVoiceInputState('idle');
      return;
    }
    if (voiceInputSession.current) {
      await finishVoiceInput();
      return;
    }
    if (!voiceConfiguration || !voiceInputAvailable || !sessionAvailable) return;

    const attempt = ++voiceInputAttempt.current;
    const startup = new AbortController();
    voiceInputStartup.current = startup;
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
        client.connectVoiceInput,
        startup.signal,
      );
      if (voiceInputAttempt.current !== attempt) {
        session.cancel();
        return;
      }
      voiceInputSession.current = session;
      voiceInputStartup.current = null;
      setVoiceInputState('recording');
    } catch (failure: unknown) {
      if (voiceInputAttempt.current !== attempt) return;
      voiceInputStartup.current = null;
      setVoiceInputState('idle');
      setActionError(voiceInputMessage(failure));
    }
  }

  const voiceInputLabel = voiceInputState === 'idle'
    ? 'Start voice input'
    : voiceInputState === 'recording'
      ? 'Stop voice input'
      : voiceInputState === 'starting'
        ? 'Cancel voice input setup'
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
          </div>
        )}
        {coveredEntries.length > 0 && (
          <section aria-label="Covered conversation" className="cha-covered">
            {coveredEntries.map(({ entry, dividerBefore }) => (
              <Fragment key={entry.id}>
                {dividerBefore && <hr className="cha-repeated-prompt-divider" />}
                <TranscriptMessage
                  appearance={entry.kind === 'human'
                    ? personas.get(entry.participant_id)?.appearance
                    : entry.kind === 'character'
                      ? appearances.get(entry.participant_id)
                      : undefined}
                  actionDisabled={!connected || generationActive || pendingAction !== null}
                  entry={entry}
                  onCover={entry.id === boundaryEntryId
                    ? undefined
                    : (coveredEntry) => changeCover(coveredEntry.id)}
                  onToggleSpeech={toggleSpeech}
                  onDelete={(response) => setTurnToDelete({
                    id: response.id,
                    displayName: response.display_name,
                  })}
                  onUncover={entry.id === boundaryEntryId ? () => changeCover() : undefined}
                  speechState={spokenEntry?.id === entry.id ? spokenEntry.state
                    : audioJobs.get(entry.id)?.state ?? 'idle'}
                  speechError={audioJobs.get(entry.id)?.error}
                  speechAvailable={textToSpeechConfiguration !== null || (entry.has_cached_audio === true && downloads.status !== null)}
                />
              </Fragment>
            ))}
          </section>
        )}
        {uncoveredEntries.map(({ entry, dividerBefore }) => (
          <Fragment key={entry.id}>
            {dividerBefore && <hr className="cha-repeated-prompt-divider" />}
            <TranscriptMessage
              appearance={entry.kind === 'human'
                ? personas.get(entry.participant_id)?.appearance
                : entry.kind === 'character'
                  ? appearances.get(entry.participant_id)
                  : undefined}
              actionDisabled={!connected || generationActive || pendingAction !== null}
              entry={entry}
              onCover={(coveredEntry) => changeCover(coveredEntry.id)}
              onToggleSpeech={toggleSpeech}
              onDelete={(response) => setTurnToDelete({
                id: response.id,
                displayName: response.display_name,
              })}
              speechState={spokenEntry?.id === entry.id ? spokenEntry.state
                : audioJobs.get(entry.id)?.state ?? 'idle'}
              speechError={audioJobs.get(entry.id)?.error}
              speechAvailable={textToSpeechConfiguration !== null || (entry.has_cached_audio === true && downloads.status !== null)}
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
            placeholder={sendToAll
              ? 'Message all characters'
              : recordingTarget
              ? 'Self-notes — saved, not sent'
              : `Message ${character?.display_name ?? 'character'}`}
            ref={composerInput}
            rows={1}
            value={draft}
          />
          <div className="cha-composer-controls">
            <label className="cha-target-select" title="Choose message target">
              <TargetIcon />
              <select
                aria-label="Choose message target"
                disabled={!connected || pendingAction !== null}
                onChange={(event) => void chooseTarget(event.target.value)}
                value={sendToAll ? allCharactersTarget : (state.currentDefaultCharacterId ?? '')}
              >
                <option value={allCharactersTarget}>All characters</option>
                <option value="-">Self-notes</option>
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
          <span>To: {sendToAll
            ? 'All characters'
            : recordingTarget
              ? 'Self-notes'
              : (character?.display_name ?? 'Unknown character')}</span>
          {textToSpeechConfiguration && (
            <button
              aria-label="Cache conversation audio automatically"
              aria-pressed={speechCacheEnabled}
              className="cha-speech-cache-toggle"
              disabled={!snapshot || !downloads.status || downloads.unavailable !== null}
              onClick={toggleSpeechCache}
              title={downloads.unavailable ?? "Cache conversation audio automatically"}
              type="button"
            >
              <SpeakerIcon />
            </button>
          )}
          <TransliterationToggle
            disabled={!sessionAvailable}
            transliteration={transliteration}
          />
        </div>
      </div>
      {turnToDelete && (
        <ConfirmDialog
          confirmLabel="Delete response"
          message={`Delete ${turnToDelete.displayName}'s response and the prompt that generated it? This cannot be undone.`}
          onCancel={() => setTurnToDelete(null)}
          onConfirm={() => {
            const responseEntryId = turnToDelete.id;
            setTurnToDelete(null);
            void deleteTurn(responseEntryId);
          }}
          title="Delete response?"
        />
      )}
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

export function formatTokenUsage(tokens: number): string {
  return tokens < 1000 ? tokens.toLocaleString() : `${Math.round(tokens / 1000)}K`;
}
