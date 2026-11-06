import type { PromptFocus } from '../transcriptScroll';

import type { CharacterAppearance } from '../api/client';
import { Composer } from './composer';
import { Transcript, type TranscriptEntry } from './transcript';
import type { ReadAloud } from './useReadAloud';
import type { VoiceInput } from './useVoiceInput';

export function Conversation({
  entries,
  loaded,
  contextKey,
  promptFocus,
  onPromptFocused,
  characters,
  personas,
  draft,
  onDraft,
  expanded,
  onExpanded,
  viewportHeight,
  sessionKey,
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
  speech,
  voice,
}: {
  entries: readonly TranscriptEntry[];
  loaded?: boolean;
  contextKey?: string;
  promptFocus?: PromptFocus | null;
  onPromptFocused?(marker: PromptFocus): void;
  characters: ReadonlyArray<{ id: string; appearance: CharacterAppearance }>;
  personas: ReadonlyArray<{ id: string; appearance: CharacterAppearance }>;
  draft: string;
  onDraft(value: string): void;
  expanded: boolean;
  onExpanded(expanded: boolean): void;
  viewportHeight: number;
  sessionKey: string;
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
  speech?: ReadAloud;
  voice?: VoiceInput;
}) {
  return (
    <div className="chaweb-conversation">
      <Transcript
        loaded={loaded}
        contextKey={contextKey}
        promptFocus={promptFocus}
        onPromptFocused={onPromptFocused}
        speech={speech}
        deleting={deleting}
        characters={characters}
        entries={entries}
        layoutKey={`${expanded}:${viewportHeight}:${notice}:${showSending}:${pendingText}`}
        personas={personas}
        sessionKey={sessionKey}
      />
      <div className="chaweb-feedback">
        {speech?.error && <p className="chaweb-alert" role="alert">{speech.error}</p>}
        {voice?.error && <p className="chaweb-alert" role="alert">{voice.error}</p>}
        {notice && <p className={deleting ? 'chaweb-status' : 'chaweb-alert'} role={deleting ? 'status' : 'alert'}>{notice}</p>}
        {onRetry && (
          <button className="chaweb-new-session" onClick={onRetry} type="button">Retry</button>
        )}
        {(showSending || pendingText) && (
          <div className="chaweb-pending" role="status">
            <div className="chaweb-entry-status">Sending</div>
            {pendingText && <div>{pendingText}</div>}
          </div>
        )}
      </div>
      <Composer
        voice={voice}
        commandDisabled={commandDisabled || Boolean(voice?.finishing)}
        deleteDisabled={deleteDisabled}
        deleting={deleting}
        expanded={expanded}
        mode={mode}
        onChange={onDraft}
        onAutomaticAudio={speech?.toggleAutomatic}
        automaticAudio={speech?.automatic}
        automaticAudioDisabled={!speech || speech.automaticDisabled}
        onDelete={onDelete}
        onExpanded={onExpanded}
        onSend={onSend}
        onStop={onStop}
        value={draft}
        viewportHeight={viewportHeight}
      />
    </div>
  );
}
