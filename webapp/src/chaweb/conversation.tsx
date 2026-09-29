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
