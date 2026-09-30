import type { CharacterAppearance } from '../api/client';
import { Composer } from './composer';
import { Transcript, type TranscriptEntry } from './transcript';

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
  mode,
  commandDisabled,
  onSend,
  onStop,
  notice,
  showSending = false,
  pendingText = null,
  onRetry,
  onAllowSend,
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
  mode: 'send' | 'stop';
  commandDisabled: boolean;
  onSend(): void;
  onStop(): void;
  notice: string | null;
  showSending?: boolean;
  pendingText?: string | null;
  onRetry?(): void;
  onAllowSend?(): void;
}) {
  return (
    <div className="chaweb-conversation">
      <Transcript
        characters={characters}
        entries={entries}
        layoutKey={`${expanded}:${viewportHeight}`}
        personas={personas}
        sessionKey={sessionKey}
      />
      {notice && <p className="chaweb-alert" role="alert">{notice}</p>}
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
        expanded={expanded}
        mode={mode}
        onChange={onDraft}
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
