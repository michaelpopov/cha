import type { SessionSnapshot } from './api/client';

type Entry = SessionSnapshot['transcript'][number];
export interface PromptFocus {
  key: string;
  afterId: number;
}

export interface TranscriptScroll {
  key: string;
  entries: Map<number, boolean>;
  transcript: readonly Entry[];
  layout: string;
  following: boolean;
  consumed?: PromptFocus;
}

// Called after the articles commit. Both chat renderers use the same arrival
// rules; only the transcript container is allowed to move.
export function scrollTranscript(
  container: HTMLElement,
  previous: TranscriptScroll | null,
  key: string,
  transcript: readonly Entry[],
  layout: string,
  automatic: boolean,
  prompt: PromptFocus | null,
): TranscriptScroll {
  const before = previous?.key === key ? previous : null;
  const entries = new Map<number, boolean>();
  let target: HTMLElement | null = null;
  let localTarget: HTMLElement | null = null;
  let representative: Entry | null = null;
  let consumed = before?.consumed;
  for (const entry of transcript) {
    const node = container.querySelector<HTMLElement>(`[data-entry-id="${entry.id}"]`);
    const readable = entry.kind === 'character'
      && Boolean(node?.querySelector('[data-message-body]')?.textContent?.trim());
    const seen = before?.entries.get(entry.id) ?? false;
    entries.set(entry.id, seen || readable);
    if (before && node) {
      if (entry.kind === 'human' && !before.entries.has(entry.id)) target = node;
      if (readable && !seen && automatic
          && (entry.status === 'streaming' || entry.status === 'complete')) target = node;
    }
    if (entry.kind !== 'human') continue;
    if (!representative || representative.participant_id !== entry.participant_id
        || representative.text !== entry.text) representative = entry;
    if (!localTarget && prompt?.key === key && prompt !== consumed && entry.id > prompt.afterId) {
      localTarget = container.querySelector<HTMLElement>(`[data-entry-id="${representative.id}"]`);
    }
  }
  if (localTarget && prompt) {
    target = localTarget;
    consumed = prompt;
  }
  let following = before?.following ?? true;
  if (target) {
    const desired = container.scrollTop + target.getBoundingClientRect().top
      - container.getBoundingClientRect().top - container.clientTop;
    container.scrollTop = Math.max(0, Math.min(desired,
      Math.max(0, container.scrollHeight - container.clientHeight)));
    following = isFollowing(container);
  } else if (following && (!before || before.transcript !== transcript || before.layout !== layout)) {
    container.scrollTop = container.scrollHeight;
  }
  return { key, entries, transcript, layout, following, consumed };
}

export function isFollowing(container: HTMLElement): boolean {
  return container.scrollHeight - container.scrollTop - container.clientHeight <= 24;
}

export function lastEntryId(entries: readonly Entry[]): number {
  return entries.reduce((last, entry) => Math.max(last, entry.id), 0);
}
