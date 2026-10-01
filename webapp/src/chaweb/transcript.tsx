import { useLayoutEffect, useRef, type UIEvent } from 'react';

import type { CharacterAppearance, SessionSnapshot } from '../api/client';
import { voiceClasses } from '../components/characterAppearance';
import { Markdown } from '../components/Markdown';
import { formatTimestamp } from './time';
import type { ReadAloud } from './useReadAloud';

export type TranscriptEntry = SessionSnapshot['transcript'][number];

const followSlack = 24;

const statusLabel: Record<TranscriptEntry['status'], string | null> = {
  complete: null,
  streaming: 'Streaming',
  cancelled: 'Stopped',
  failed: 'Failed',
};

function appearanceFor(
  entry: TranscriptEntry,
  characters: ReadonlyArray<{ id: string; appearance: CharacterAppearance }>,
  personas: ReadonlyArray<{ id: string; appearance: CharacterAppearance }>,
): CharacterAppearance | undefined {
  const roster = entry.kind === 'human' ? personas : entry.kind === 'character' ? characters : [];
  return roster.find((item) => item.id === entry.participant_id)?.appearance;
}

export function Transcript({
  entries,
  characters,
  personas,
  layoutKey,
  sessionKey,
  speech,
  deleting = false,
}: {
  entries: readonly TranscriptEntry[];
  characters: ReadonlyArray<{ id: string; appearance: CharacterAppearance }>;
  personas: ReadonlyArray<{ id: string; appearance: CharacterAppearance }>;
  layoutKey: string;
  sessionKey: string;
  speech?: ReadAloud;
  deleting?: boolean;
}) {
  const scroller = useRef<HTMLDivElement>(null);
  const following = useRef(true);
  const seenSession = useRef(sessionKey);
  if (seenSession.current !== sessionKey) {
    seenSession.current = sessionKey;
    following.current = true;
  }

  useLayoutEffect(() => {
    const node = scroller.current;
    if (!node || !following.current) return;
    node.scrollTop = node.scrollHeight;
  }, [entries, layoutKey, sessionKey]);

  function notePosition(event: UIEvent<HTMLDivElement>) {
    const node = event.currentTarget;
    following.current = node.scrollHeight - node.scrollTop - node.clientHeight <= followSlack;
  }

  // Multicast stores a copy of the prompt for each character. Match the desktop
  // display: replies do not reset the comparison with the previous user prompt.
  let lastPrompt: TranscriptEntry | undefined;
  const visibleEntries = entries.filter((entry) => {
    if (entry.kind !== 'human') return true;
    const repeated = lastPrompt?.participant_id === entry.participant_id
      && lastPrompt.text === entry.text;
    lastPrompt = entry;
    return !repeated;
  });

  return (
    <div
      aria-label="Conversation transcript"
      className="chaweb-transcript"
      onScroll={notePosition}
      ref={scroller}
    >
      {visibleEntries.map((entry) => {
        const label = statusLabel[entry.status];
        const appearance = appearanceFor(entry, characters, personas);
        return (
          <article
            className={`chaweb-entry is-${entry.kind}`}
            data-status={entry.status}
            key={entry.id}
          >
            {entry.display_name && <div className="chaweb-speaker">{entry.display_name}</div>}
            <div className={`chaweb-entry-text${voiceClasses(appearance)}`}>
              <Markdown source={entry.text} />
            </div>
            {label && <div className="chaweb-entry-status">{label}</div>}
            {speech && entry.kind === 'character' && entry.status === 'complete'
              && entry.text.trim() && (speech.available || entry.has_cached_audio) && (
              <button
                className="chaweb-read-aloud"
                disabled={deleting || speech.clearing}
                onClick={() => speech.toggle(entry)}
                type="button"
                aria-label={speech.entryId === entry.id
                  ? speech.state === 'loading' ? 'Stop audio' : 'Pause audio'
                  : 'Read aloud'}
              >
                {speech.entryId === entry.id
                  ? speech.state === 'loading' ? 'Stop audio (loading)' : 'Pause audio'
                  : 'Read aloud'}
              </button>
            )}
            {entry.created_at !== null && (
              <time
                className="chaweb-entry-time"
                dateTime={new Date(entry.created_at * 1000).toISOString()}
              >
                {formatTimestamp(entry.created_at)}
              </time>
            )}
          </article>
        );
      })}
    </div>
  );
}
