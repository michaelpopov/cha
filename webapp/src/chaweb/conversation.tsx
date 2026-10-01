import { useEffect, useRef, useState } from 'react';

import type { CharacterAppearance } from '../api/client';
import { copyText } from './clipboard';
import { Composer } from './composer';
import { Transcript, type TranscriptEntry } from './transcript';
import type { ReadAloud } from './useReadAloud';

export function Conversation({
  entries,
  characters,
  personas,
  draft,
  onDraft,
  expanded,
  onExpanded,
  viewportHeight,
  sessionKey,
  onSessions,
  onDelete,
  deleteDisabled,
  deleting,
  mode,
  commandDisabled,
  onSend,
  onStop,
  notice,
  showSending = false,
  pendingText = null,
  onRetry,
  onAllowSend,
  speech,
}: {
  entries: readonly TranscriptEntry[];
  characters: ReadonlyArray<{ id: string; appearance: CharacterAppearance }>;
  personas: ReadonlyArray<{ id: string; appearance: CharacterAppearance }>;
  draft: string;
  onDraft(value: string): void;
  expanded: boolean;
  onExpanded(expanded: boolean): void;
  viewportHeight: number;
  sessionKey: string;
  onSessions(): void;
  onDelete(): void;
  deleteDisabled: boolean;
  deleting: boolean;
  mode: 'send' | 'stop';
  commandDisabled: boolean;
  onSend(): void;
  onStop(): void;
  notice: string | null;
  showSending?: boolean;
  pendingText?: string | null;
  onRetry?(): void;
  onAllowSend?(): void;
  speech?: ReadAloud;
}) {
  const root = useRef<HTMLDivElement>(null);
  const currentSession = useRef(sessionKey);
  currentSession.current = sessionKey;
  const [copyState, setCopyState] = useState<{
    key: string; status: 'copying' | 'copied' | 'failed';
  } | null>(null);
  const copyStatus = copyState?.key === sessionKey ? copyState.status : null;

  useEffect(() => {
    if (copyState?.status !== 'copied') return;
    const timer = window.setTimeout(() => setCopyState(null), 2_000);
    return () => window.clearTimeout(timer);
  }, [copyState]);

  function copyConversation() {
    const transcript = root.current?.querySelector<HTMLElement>('.chaweb-transcript');
    const text = Array.from(transcript?.querySelectorAll<HTMLElement>('.chaweb-entry') ?? [])
      .map((entry) => Array.from(entry.querySelectorAll<HTMLElement>(
        '.chaweb-speaker, .chaweb-entry-text, .chaweb-entry-status',
      )).map((part) => part.innerText).join('\n'))
      .join('\n\n');
    if (!text.trim() || copyStatus === 'copying') return;
    const key = sessionKey;
    setCopyState({ key, status: 'copying' });
    void copyText(text).then(
      () => {
        if (root.current && currentSession.current === key) setCopyState({ key, status: 'copied' });
      },
      () => {
        if (root.current && currentSession.current === key) setCopyState({ key, status: 'failed' });
      },
    );
  }

  return (
    <div className="chaweb-conversation" ref={root}>
      <Transcript
        speech={speech}
        deleting={deleting}
        characters={characters}
        entries={entries}
        layoutKey={`${expanded}:${viewportHeight}`}
        personas={personas}
        sessionKey={sessionKey}
      />
      {speech?.error && <p className="chaweb-alert" role="alert">{speech.error}</p>}
      {notice && <p className={deleting ? 'chaweb-status' : 'chaweb-alert'} role={deleting ? 'status' : 'alert'}>{notice}</p>}
      {copyStatus === 'copied' && <p className="chaweb-status" role="status">Copied to clipboard</p>}
      {copyStatus === 'failed' && (
        <p className="chaweb-alert" role="alert">Could not copy the conversation. Try again.</p>
      )}
      {onRetry && (
        <button className="chaweb-new-session" onClick={onRetry} type="button">Retry</button>
      )}
      {onAllowSend && (
        <button className="chaweb-new-session" onClick={onAllowSend} type="button">
          Allow another Send
        </button>
      )}
      {(showSending || pendingText) && (
        <div className="chaweb-pending" role="status">
          <div className="chaweb-entry-status">Sending</div>
          {pendingText && <div>{pendingText}</div>}
        </div>
      )}
      <Composer
        commandDisabled={commandDisabled}
        copied={copyStatus === 'copied'}
        copyDisabled={entries.length === 0 || deleting || copyStatus === 'copying'}
        deleteDisabled={deleteDisabled}
        deleting={deleting}
        expanded={expanded}
        mode={mode}
        onChange={onDraft}
        onCopy={copyConversation}
        onDelete={onDelete}
        onExpanded={onExpanded}
        onSend={onSend}
        onSessions={onSessions}
        onStop={onStop}
        value={draft}
        viewportHeight={viewportHeight}
      />
    </div>
  );
}
