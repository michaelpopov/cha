import { useRef, type KeyboardEvent, type PointerEvent } from 'react';

import { PaperPlaneIcon, SpeakerIcon, StopIcon } from '../components/Icons';
import type { VoiceInput } from './useVoiceInput';

const compactHeight = 56;

export function editorPixelHeight(expanded: boolean, viewportHeight: number): number {
  if (!expanded) return compactHeight;
  const chrome = 44 + 44;
  const usable = Math.max(compactHeight, viewportHeight - chrome);
  return Math.max(compactHeight, Math.round(usable / 2));
}

function ExpandIcon() {
  return (
    <svg aria-hidden="true" fill="none" height="20" viewBox="0 0 24 24" width="20">
      <path d="M6 14l6-6 6 6" stroke="currentColor" strokeLinecap="round" strokeLinejoin="round" strokeWidth="1.7" />
    </svg>
  );
}

function ShrinkIcon() {
  return (
    <svg aria-hidden="true" fill="none" height="20" viewBox="0 0 24 24" width="20">
      <path d="M6 10l6 6 6-6" stroke="currentColor" strokeLinecap="round" strokeLinejoin="round" strokeWidth="1.7" />
    </svg>
  );
}

export function Composer({
  value,
  onChange,
  expanded,
  onExpanded,
  viewportHeight,
  onAutomaticAudio,
  automaticAudio = false,
  automaticAudioDisabled,
  deleting,
  mode,
  commandDisabled,
  onSend,
  onStop,
  voice,
}: {
  value: string;
  onChange(value: string): void;
  expanded: boolean;
  onExpanded(expanded: boolean): void;
  viewportHeight: number;
  onAutomaticAudio?(): void;
  automaticAudio?: boolean;
  automaticAudioDisabled?: boolean;
  deleting: boolean;
  mode: 'send' | 'stop';
  commandDisabled: boolean;
  onSend(): void;
  onStop(): void;
  voice?: VoiceInput;
}) {
  const editorRef = useRef<HTMLTextAreaElement | null>(null);
  const composing = useRef(false);
  const pressed = useRef(false);
  const toggledByPointer = useRef(false);
  const height = editorPixelHeight(expanded, viewportHeight);

  function insertNewline(textarea: HTMLTextAreaElement) {
    const start = textarea.selectionStart ?? value.length;
    const end = textarea.selectionEnd ?? value.length;
    const next = `${value.slice(0, start)}\n${value.slice(end)}`;
    const caret = start + 1;
    onChange(next);
    requestAnimationFrame(() => {
      textarea.selectionStart = caret;
      textarea.selectionEnd = caret;
    });
  }

  function onKeyDown(event: KeyboardEvent<HTMLTextAreaElement>) {
    if (event.key !== 'Enter') return;
    const native = event.nativeEvent;
    if (native.isComposing || composing.current || native.keyCode === 229) return;
    if (event.ctrlKey) {
      event.preventDefault();
      if (!event.currentTarget.readOnly) insertNewline(event.currentTarget);
      return;
    }
    event.preventDefault();
    if (mode === 'send' && !commandDisabled) onSend();
  }

  function toggleExpanded() {
    onExpanded(!expanded);
  }

  function onSizePointerDown(event: PointerEvent<HTMLButtonElement>) {
    if (event.button !== 0) return;
    // A pointer press would move focus and dismiss the iPhone keyboard.
    event.preventDefault();
    pressed.current = true;
  }

  // Toggle only for a press that started on this button. Its click follows.
  function onSizePointerUp(event: PointerEvent<HTMLButtonElement>) {
    if (event.button !== 0 || !pressed.current) return;
    pressed.current = false;
    toggledByPointer.current = true;
    toggleExpanded();
  }

  function onSizePointerEnd() {
    pressed.current = false;
  }

  function onSizeClick() {
    if (toggledByPointer.current) {
      toggledByPointer.current = false;
      return;
    }
    toggleExpanded();
  }

  return (
    <div className="chaweb-composer">
      <div className="chaweb-size-row">
        <button
          aria-label="Voice mode"
          aria-pressed={automaticAudio}
          title={automaticAudio ? 'Turn voice mode off' : 'Turn voice mode on'}
          className="chaweb-icon-button"
          disabled={deleting || automaticAudioDisabled || !onAutomaticAudio}
          onClick={() => {
            editorRef.current?.blur();
            onAutomaticAudio?.();
          }}
          type="button"
        >
          {automaticAudio ? <SpeakerIcon /> : <SpeakerOffIcon />}
        </button>
        {/* Voice mode makes the editor read-only, so Enter is not available on a phone. */}
        {automaticAudio && mode === 'send' && (
          <button
            aria-label="Send"
            title="Send"
            className="chaweb-icon-button"
            disabled={commandDisabled}
            onClick={() => {
              editorRef.current?.blur();
              onSend();
            }}
            type="button"
          >
            <PaperPlaneIcon className="chaweb-send-icon" />
          </button>
        )}
        {mode === 'stop' && (
          <button
            aria-label="Stop"
            title="Stop generation"
            className="chaweb-icon-button"
            disabled={commandDisabled}
            onClick={onStop}
            type="button"
          >
            <StopIcon />
          </button>
        )}
        {voice?.status && (
          <div
            className={`chaweb-voice-status is-${voice.status}`}
            role="status"
          >
            {voice.status === 'starting' ? 'Microphone starting…'
              : voice.status === 'recording' ? 'Speak now' : 'Microphone paused'}
          </div>
        )}
        <button
          aria-label={expanded ? 'Shrink editor' : 'Expand editor'}
          className="chaweb-icon-button chaweb-size-button"
          onClick={onSizeClick}
          onPointerCancel={onSizePointerEnd}
          onPointerDown={onSizePointerDown}
          onPointerLeave={onSizePointerEnd}
          onPointerUp={onSizePointerUp}
          type="button"
        >
          {expanded ? <ShrinkIcon /> : <ExpandIcon />}
        </button>
      </div>
      <textarea
        aria-label="Message"
        className="chaweb-editor"
        enterKeyHint="send"
        onChange={(event) => onChange(event.target.value)}
        onCompositionEnd={() => { composing.current = false; }}
        onCompositionStart={() => { composing.current = true; }}
        onKeyDown={onKeyDown}
        ref={editorRef}
        readOnly={voice?.enabled || voice?.finishing}
        rows={2}
        style={{ height }}
        value={value}
      />
    </div>
  );
}

function SpeakerOffIcon() {
  return (
    <svg aria-hidden="true" fill="none" height="20" viewBox="0 0 24 24" width="20">
      <path d="M5 10h3l4-3.5v11L8 14H5zM16 10l4 4m0-4-4 4"
        stroke="currentColor" strokeLinecap="round" strokeLinejoin="round" strokeWidth="1.7" />
    </svg>
  );
}
