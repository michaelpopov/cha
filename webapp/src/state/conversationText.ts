import type { SessionSnapshot } from '../api/client';

function normalizeLineEndings(value: string): string {
  return value.replace(/\r\n?/g, '\n');
}

export function hasCopyableConversation(snapshot: SessionSnapshot): boolean {
  return snapshot.transcript.some((entry) => entry.kind !== 'notice' && entry.text.length > 0);
}

export function conversationText(snapshot: SessionSnapshot): string {
  const blocks = snapshot.transcript.flatMap((entry) => {
    if (entry.kind === 'notice' || entry.text.length === 0) return [];
    let speaker: string;
    if (entry.kind === 'human') {
      speaker = `${entry.display_name} -> ${entry.addressed_to_name}:`;
    } else if (entry.kind === 'error') {
      speaker = 'Error:';
    } else {
      speaker = `${entry.display_name}:`;
    }
    if (entry.status === 'cancelled') speaker += ' [stopped]';
    if (entry.status === 'streaming') speaker += ' [in progress]';
    return [`${speaker}\n${normalizeLineEndings(entry.text)}`];
  });

  const header = `Session: ${snapshot.session_label}\nForum: ${snapshot.forum.display_name}`;
  return `${[header, ...blocks].join('\n\n')}\n`;
}
