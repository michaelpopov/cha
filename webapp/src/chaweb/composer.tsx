import { useRef, type KeyboardEvent, type PointerEvent } from 'react';

import { CheckIcon, ClearAudioIcon, CopyIcon, ForumsIcon, PaperPlaneIcon, SpeakerIcon, StopIcon, TrashIcon } from '../components/Icons';

const compactHeight = 56;

export function editorPixelHeight(expanded: boolean, viewportHeight: number): number {
  if (!expanded) return compactHeight;
  const chrome = 44 + 44;
  const usable = Math.max(compactHeight, viewportHeight - chrome);
  return Math.max(compactHeight, Math.round(usable / 2));
}

function coarsePointer(): boolean {
  return window.matchMedia?.('(pointer: coarse)')?.matches === true;
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
  onSessions,
  onDelete,
  onCopy,
  onClearAudio,
  clearAudioDisabled,
  onAutomaticAudio,
  automaticAudio = false,
  automaticAudioDisabled,
  copyDisabled,
  copied,
  deleteDisabled,
  deleting,
  mode,
  commandDisabled,
  onSend,
  onStop,
}: {
  value: string;
  onChange(value: string): void;
  expanded: boolean;
  onExpanded(expanded: boolean): void;
  viewportHeight: number;
  onSessions(): void;
  onDelete(): void;
  onCopy(): void;
  onClearAudio?(): void;
  clearAudioDisabled?: boolean;
  onAutomaticAudio?(): void;
  automaticAudio?: boolean;
  automaticAudioDisabled?: boolean;
  copyDisabled: boolean;
  copied: boolean;
  deleteDisabled: boolean;
  deleting: boolean;
  mode: 'send' | 'stop';
  commandDisabled: boolean;
  onSend(): void;
  onStop(): void;
}) {
  const editorRef = useRef<HTMLTextAreaElement | null>(null);
  const composing = useRef(false);
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
      insertNewline(event.currentTarget);
      return;
    }
    if (coarsePointer()) return;
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
  }

  function onSizePointerUp(event: PointerEvent<HTMLButtonElement>) {
    if (event.button !== 0) return;
    toggledByPointer.current = true;
    toggleExpanded();
  }

  function onSizeClick() {
    if (toggledByPointer.current) {
      toggledByPointer.current = false;
      return;
    }
    toggleExpanded();
  }

  function showSessions() {
    editorRef.current?.blur();
    onSessions();
  }

  const commandLabel = mode === 'stop' ? 'Stop' : 'Send';

  return (
    <div className="chaweb-composer">
      <div className="chaweb-size-row">
        <button
          aria-label={expanded ? 'Shrink editor' : 'Expand editor'}
          className="chaweb-icon-button"
          onClick={onSizeClick}
          onPointerDown={onSizePointerDown}
          onPointerUp={onSizePointerUp}
          type="button"
        >
          {expanded ? <ShrinkIcon /> : <ExpandIcon />}
        </button>
      </div>
      <textarea
        aria-label="Message"
        className="chaweb-editor"
        onChange={(event) => onChange(event.target.value)}
        onCompositionEnd={() => { composing.current = false; }}
        onCompositionStart={() => { composing.current = true; }}
        onKeyDown={onKeyDown}
        ref={editorRef}
        rows={2}
        style={{ height }}
        value={value}
      />
      <div className="chaweb-controls">
        <button aria-label="Sessions" className="chaweb-icon-button" disabled={deleting} onClick={showSessions} type="button">
          <ForumsIcon />
        </button>
        <button
          aria-label="Delete session"
          className="chaweb-icon-button"
          disabled={deleteDisabled}
          onClick={onDelete}
          type="button"
        >
          <TrashIcon />
        </button>
        <button
          aria-label="Clear audio recordings"
          title="Clear audio recordings"
          className="chaweb-icon-button"
          disabled={deleting || clearAudioDisabled || !onClearAudio}
          onClick={onClearAudio}
          type="button"
        >
          <ClearAudioIcon />
        </button>
        <button
          aria-label="Cache audio and play new responses automatically"
          aria-pressed={automaticAudio}
          title="Cache audio and play new responses automatically"
          className="chaweb-icon-button"
          disabled={deleting || automaticAudioDisabled || !onAutomaticAudio}
          onClick={onAutomaticAudio}
          type="button"
        >
          <SpeakerIcon />
        </button>
        <button
          aria-label={copied ? 'Copied conversation' : 'Copy conversation'}
          className="chaweb-icon-button"
          disabled={copyDisabled}
          onClick={onCopy}
          type="button"
        >
          {copied ? <CheckIcon /> : <CopyIcon />}
        </button>
        <button
          aria-label={commandLabel}
          className="chaweb-icon-button"
          disabled={commandDisabled}
          onClick={mode === 'stop' ? onStop : onSend}
          type="button"
        >
          {mode === 'stop' ? <StopIcon /> : <PaperPlaneIcon className="chaweb-send-icon" />}
        </button>
      </div>
    </div>
  );
}
