import { withoutVoiceSendPhrase } from '../dictationText';
import {
  useEffect,
  useLayoutEffect,
  useRef,
  useState,
  type FormEvent,
  type KeyboardEvent,
  type PointerEvent,
  type ReactElement,
  type ReactNode,
} from 'react';

import {
  publicErrorMessage,
  type ChaClient,
  type NativeVoiceInputRuntime,
} from '../api/client';
import {
  appendPreparedTranscription,
  appendTranscription,
  unsupportedAudioFormat,
  VoiceInputSession,
  type VoiceInputConfiguration,
  type VoiceInputTransport,
  type VoiceInputXaiBridge,
} from '../voiceInput';
import { MicrophoneIcon, SendIcon } from './Icons';
import { TransliterationToggle, useTransliteration } from './TransliterationMode';

export interface PromptVoiceControls {
  available: boolean;
  state: 'idle' | 'starting' | 'recording' | 'finishing';
  toggle(): Promise<void>;
}

export interface PromptInputProps {
  value: string;
  onChange(next: string): void;
  enabled: boolean;
  submitEnabled: boolean;
  microphoneEnabled: boolean;
  inputLabel: string;
  placeholder?: string;
  submitLabel: string;
  client: ChaClient;
  onSubmit(text: string): Promise<{ clear_input: boolean }>;
  onSubmittingChange?(submitting: boolean): void;
  onError(message: string | null): void;
  stopAction?: ReactElement;
  formControls?: ReactNode;
  statusRow: (controls: {
    transliteration: ReactNode;
    voice: PromptVoiceControls;
  }) => ReactNode;
}

function actionMessage(failure: unknown): string {
  return publicErrorMessage(failure, 'The action could not be completed. Try again.');
}

function voiceInputMessage(failure: unknown): string {
  if (failure instanceof DOMException && failure.name === 'NotAllowedError') {
    return 'Microphone access was denied. Allow it in System Settings and try again.';
  }
  if (failure instanceof Error && failure.message === unsupportedAudioFormat) {
    return failure.message;
  }
  return 'Voice input stopped because transcription failed. Try again.';
}

function xaiBridgeFromClient(client: ChaClient): VoiceInputXaiBridge {
  return {
    start: (sessionId, languages, signal) => client.startXaiVoiceInput(sessionId, languages, signal),
    audio: (sessionId, pcmBase64, signal) => client.sendXaiVoiceAudio(sessionId, pcmBase64, signal),
    stop: (sessionId, remainingMs, signal) => (
      client.stopXaiVoiceInput(sessionId, remainingMs, signal)
    ),
    cancel: (sessionId) => client.cancelXaiVoiceInput(sessionId),
  };
}

export function PromptInput({
  value,
  onChange,
  enabled,
  submitEnabled,
  microphoneEnabled,
  inputLabel,
  placeholder,
  submitLabel,
  client,
  onSubmit,
  onSubmittingChange,
  onError,
  stopAction,
  formControls,
  statusRow,
}: PromptInputProps) {
  const draftRef = useRef(value);
  const committedValue = useRef(value);
  if (committedValue.current !== value) {
    committedValue.current = value;
    draftRef.current = value;
  }

  const onChangeRef = useRef(onChange);
  const onSubmitRef = useRef(onSubmit);
  const onErrorRef = useRef(onError);
  const onSubmittingChangeRef = useRef(onSubmittingChange);
  onChangeRef.current = onChange;
  onSubmitRef.current = onSubmit;
  onErrorRef.current = onError;
  onSubmittingChangeRef.current = onSubmittingChange;

  function updateDraft(next: string) {
    draftRef.current = next;
    onChangeRef.current(next);
  }

  const transliteration = useTransliteration<HTMLTextAreaElement>(value);
  const [submitting, setSubmitting] = useState(false);
  const sending = useRef(false);
  const composerEpoch = useRef(0);
  const [voiceInputState, setVoiceInputState] = useState<
    'idle' | 'starting' | 'recording' | 'finishing'
  >('idle');
  const [voiceSendPending, setVoiceSendPending] = useState(false);
  const voiceSendPendingRef = useRef(false);
  const [voiceConfiguration, setVoiceConfiguration] =
    useState<VoiceInputConfiguration | null>(null);
  const voiceInputSession = useRef<VoiceInputTransport | null>(null);
  const voiceSendPhrase = useRef('over to you');
  const voiceInputStartup = useRef<AbortController | null>(null);
  const voiceInputAttempt = useRef(0);
  const voiceSendTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const voiceLanguage = useRef<'en' | 'ru'>('en');
  voiceLanguage.current = transliteration.enabled ? 'ru' : 'en';
  const composerArea = useRef<HTMLDivElement | null>(null);
  const composerInput = transliteration.field;
  const composerResize = useRef<{
    pointerId: number;
    startHeight: number;
    startY: number;
  } | null>(null);
  const [composerHeight, setComposerHeight] = useState<number | null>(null);
  const voiceInputAvailable = voiceConfiguration !== null
    && VoiceInputSession.supported(voiceConfiguration.provider);
  const voiceInputActive = voiceInputState !== 'idle';
  const canSubmit = submitEnabled
    && !submitting
    && !stopAction
    && voiceInputState !== 'starting'
    && voiceInputState !== 'finishing'
    && (value.trim().length > 0 || voiceInputState === 'recording');

  function changeVoiceSendPending(next: boolean) {
    voiceSendPendingRef.current = next;
    setVoiceSendPending(next);
  }

  function stopVoiceWork() {
    const session = voiceInputSession.current;
    voiceInputSession.current = null;
    voiceInputStartup.current?.abort();
    voiceInputStartup.current = null;
    // Cancel while this attempt is still current so a preview callback can
    // remove only the unconfirmed suffix. Later callbacks see the new attempt.
    session?.cancel();
    voiceInputAttempt.current += 1;
    if (voiceSendTimer.current !== null) {
      clearTimeout(voiceSendTimer.current);
      voiceSendTimer.current = null;
    }
    changeVoiceSendPending(false);
    setVoiceInputState('idle');
  }

  useEffect(() => {
    let current = true;
    void client.getVoiceInputRuntime().then(
      (configuration) => {
        if (!current) return;
        setVoiceConfiguration(configuration ? {
          provider: configuration.provider,
          model: configuration.model,
          delay: configuration.delay,
          prompt: configuration.prompt,
        } : null);
      },
      () => { if (current) setVoiceConfiguration(null); },
    );
    return () => { current = false; };
  }, [client]);

  useEffect(() => () => {
    stopVoiceWork();
  }, [client, enabled]);

  useEffect(() => () => {
    composerEpoch.current += 1;
    onSubmittingChangeRef.current?.(false);
  }, []);

  function maximumComposerHeight(fallback = Number.POSITIVE_INFINITY) {
    const chatHeight = composerArea.current?.parentElement?.clientHeight ?? 0;
    return chatHeight > 0 ? chatHeight * 0.8 : fallback;
  }

  function followTranscript() {
    const transcript = composerArea.current?.parentElement?.querySelector('.cha-transcript');
    if (!transcript || transcript.getAttribute('data-following') === 'false') return;
    const end = transcript.lastElementChild;
    if (end instanceof HTMLElement) end.scrollIntoView?.({ block: 'end' });
  }

  // A textarea follows its wrapped content until the reader gives it a larger
  // floor. Both automatic and manual growth stop at 80% of the chat area.
  function fitComposer() {
    const input = composerInput.current;
    if (!input) return;
    input.style.height = 'auto';
    const maximum = maximumComposerHeight();
    input.style.height = `${Math.min(maximum, Math.max(input.scrollHeight, composerHeight ?? 0))}px`;
    followTranscript();
  }
  useLayoutEffect(fitComposer, [composerHeight, value]);

  // Picture/sidebar resizing and window changes resize the column without a
  // draft change. fitComposer reads the draft from the element, so only the
  // manual height renews this observer, not every keystroke.
  useLayoutEffect(() => {
    const area = composerArea.current?.parentElement;
    if (!area || typeof ResizeObserver === 'undefined') return;
    const observer = new ResizeObserver(fitComposer);
    observer.observe(area);
    return () => observer.disconnect();
  }, [composerHeight]);

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

  async function finishVoiceInput(): Promise<boolean> {
    const session = voiceInputSession.current;
    if (!session) return true;
    const epoch = composerEpoch.current;
    setVoiceInputState('finishing');
    try {
      await session.stop();
      if (composerEpoch.current !== epoch) return false;
      if (voiceInputSession.current === session) voiceInputSession.current = null;
      setVoiceInputState('idle');
      return true;
    } catch (failure: unknown) {
      if (composerEpoch.current !== epoch) return false;
      if (voiceInputSession.current === session) voiceInputSession.current = null;
      setVoiceInputState('idle');
      onErrorRef.current(voiceInputMessage(failure));
      return false;
    }
  }

  async function send(voiceCommand = false) {
    if (stopAction || !canSubmit || sending.current) return;
    const epoch = composerEpoch.current;
    sending.current = true;
    const wasRecording = voiceInputSession.current !== null;
    changeVoiceSendPending(false);
    setSubmitting(true);
    onSubmittingChangeRef.current?.(true);
    onErrorRef.current(null);
    try {
      // A spoken command leaves the microphone on for the next prompt.
      if (!voiceCommand && !await finishVoiceInput()) return;
      if (composerEpoch.current !== epoch) return;
      const withoutCommand = voiceCommand || wasRecording
        ? withoutVoiceSendPhrase(draftRef.current, voiceSendPhrase.current) : null;
      if (voiceCommand && withoutCommand === null) return;
      const submitted = withoutCommand ?? draftRef.current;
      if (withoutCommand !== null) {
        updateDraft(submitted.trim() ? submitted : '');
      }
      if (!submitted.trim()) return;
      const result = await onSubmitRef.current(submitted);
      if (composerEpoch.current !== epoch) return;
      if (result.clear_input) {
        // Speech or typing may continue during the send. Clear only its prefix.
        const current = draftRef.current;
        updateDraft(current.startsWith(submitted)
          ? current.slice(submitted.length).replace(/^[ \t]+/, '') : current);
      } else {
        changeVoiceSendPending(false);
      }
    } catch (failure: unknown) {
      if (composerEpoch.current !== epoch) return;
      changeVoiceSendPending(false);
      onErrorRef.current(actionMessage(failure));
    } finally {
      if (composerEpoch.current === epoch) {
        sending.current = false;
        setSubmitting(false);
        onSubmittingChangeRef.current?.(false);
      }
    }
  }

  function submit(event: FormEvent<HTMLFormElement>) {
    event.preventDefault();
    if (stopAction) return;
    void send();
  }

  useEffect(() => {
    if (!voiceSendPendingRef.current) return;
    if (withoutVoiceSendPhrase(value, voiceSendPhrase.current) === null) {
      changeVoiceSendPending(false);
      return;
    }
    // Wait until Stop is gone and the prompt can be sent.
    if (!canSubmit) return;
    const timer = setTimeout(() => void send(true), 1000);
    voiceSendTimer.current = timer;
    return () => {
      clearTimeout(timer);
      if (voiceSendTimer.current === timer) voiceSendTimer.current = null;
    };
  }, [voiceSendPending, value, canSubmit]);

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
    if (stopAction || !canSubmit) return;
    event.currentTarget.form?.requestSubmit();
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
    if (!voiceConfiguration || !voiceInputAvailable || !enabled || !microphoneEnabled) return;

    const attempt = ++voiceInputAttempt.current;
    const startup = new AbortController();
    voiceInputStartup.current = startup;
    let receivedVoiceDelta = false;
    let voicePreview = '';
    let openAiTranscript = '';
    changeVoiceSendPending(false);
    setVoiceInputState('starting');
    onErrorRef.current(null);
    let runtime: NativeVoiceInputRuntime | null;
    try {
      runtime = await client.getVoiceInputRuntime();
    } catch (failure: unknown) {
      if (voiceInputAttempt.current !== attempt) return;
      voiceInputStartup.current = null;
      setVoiceInputState('idle');
      onErrorRef.current(voiceInputMessage(failure));
      return;
    }
    if (voiceInputAttempt.current !== attempt || startup.signal.aborted) return;
    if (!runtime) {
      voiceInputStartup.current = null;
      setVoiceInputState('idle');
      onErrorRef.current('Voice input is unavailable.');
      return;
    }
    voiceSendPhrase.current = runtime.send_phrase;
    const configuration: VoiceInputConfiguration = {
      provider: runtime.provider,
      model: runtime.model,
      delay: runtime.delay,
      prompt: runtime.prompt,
      languages: [voiceLanguage.current],
    };
    let session: VoiceInputTransport | null = null;
    try {
      session = await VoiceInputSession.start(
        configuration,
        (text, provisional) => {
          if (voiceInputAttempt.current !== attempt) return;
          if (configuration.provider === 'openai') {
            // Native spoken sends keep recording. Consume only new text so
            // cumulative previews cannot restore an already submitted prompt.
            if (provisional && !text) return;
            const transcript = text;
            text = transcript.startsWith(openAiTranscript)
              ? transcript.slice(openAiTranscript.length) : '';
            openAiTranscript = provisional ? transcript : '';
            provisional = false;
          }
          if (provisional) {
            const current = draftRef.current;
            const at = voicePreview ? current.lastIndexOf(voicePreview) : -1;
            const base = at < 0 ? current
              : current.slice(0, at) + current.slice(at + voicePreview.length);
            voicePreview = '';
            if (base !== current || text) {
              const next = appendPreparedTranscription(base, text);
              voicePreview = next.slice(base.length);
              updateDraft(next);
              if (!text && withoutVoiceSendPhrase(next, voiceSendPhrase.current) !== null) {
                changeVoiceSendPending(true);
              }
            }
            return;
          }
          if (!text) return;
          const current = draftRef.current;
          updateDraft(receivedVoiceDelta && current ? current + text
            : configuration.provider === 'xai'
              ? appendPreparedTranscription(current, text)
              : appendTranscription(current, text));
          if (withoutVoiceSendPhrase(draftRef.current, voiceSendPhrase.current) !== null) {
            changeVoiceSendPending(true);
          }
          receivedVoiceDelta = true;
        },
        (failure) => {
          if (voiceInputAttempt.current !== attempt) return;
          voiceInputSession.current = null;
          changeVoiceSendPending(false);
          setVoiceInputState('idle');
          onErrorRef.current(voiceInputMessage(failure));
        },
        client.connectVoiceInput,
        xaiBridgeFromClient(client),
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
      onErrorRef.current(voiceInputMessage(failure));
    }
  }

  const voiceInputLabel = voiceInputState === 'idle'
    ? 'Start voice input'
    : voiceInputState === 'recording'
      ? 'Stop voice input'
      : voiceInputState === 'starting'
        ? 'Cancel voice input setup'
        : 'Finishing transcription';

  return (
    <div className="cha-composer-area" ref={composerArea}>
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
          aria-label={inputLabel}
          autoComplete="off"
          disabled={!enabled}
          onChange={(event) => {
            const next = transliteration.convert(event, draftRef.current);
            updateDraft(next);
          }}
          onKeyDown={submitOnEnter}
          placeholder={placeholder}
          ref={composerInput}
          rows={1}
          value={value}
        />
        <div className="cha-composer-controls">
          {formControls}
          <div className="cha-composer-actions">
            {voiceInputAvailable && (
              <button
                aria-label={voiceInputLabel}
                aria-pressed={voiceInputActive}
                className={`cha-composer-action cha-voice-input${voiceInputState === 'starting' ? ' is-starting' : voiceInputActive ? ' is-active' : ''}`}
                disabled={!enabled
                  || !microphoneEnabled
                  || submitting
                  || voiceInputState === 'finishing'}
                onClick={() => void toggleVoiceInput()}
                title={voiceInputLabel}
                type="button"
              >
                <MicrophoneIcon />
              </button>
            )}
            {stopAction ?? (
              <button
                aria-label={submitLabel}
                className="cha-composer-action cha-send"
                disabled={!canSubmit}
                type="submit"
              >
                <SendIcon />
              </button>
            )}
          </div>
        </div>
      </form>
      {statusRow({
        transliteration: (
          <TransliterationToggle
            disabled={!enabled}
            transliteration={transliteration}
          />
        ),
        voice: {
          available: voiceInputAvailable,
          state: voiceInputState,
          toggle: toggleVoiceInput,
        },
      })}
    </div>
  );
}
