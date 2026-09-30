import { ChaProtocolError } from '../api/client';
import { ChaWebError } from './client';

export const reconnectingNotice = 'Reconnecting';
export const stopRequestedNotice = 'Stop requested';
export const unknownSendNotice =
  'Send status unknown. Check the conversation before sending again.';

const readDelaysMs = [1_000, 2_000, 4_000, 10_000] as const;

export interface Draft {
  text: string;
  revision: number;
}

export type DraftMap = Record<string, Draft>;

export function newDraftKey(forumId: string): string {
  return `new:${forumId}`;
}

export function sessionDraftKey(forumId: string, sessionId: string): string {
  return `session:${forumId}/${sessionId}`;
}

export function editDraft(drafts: DraftMap, key: string, text: string): DraftMap {
  const current = drafts[key] ?? { text: '', revision: 0 };
  if (current.text === text) return drafts;
  return { ...drafts, [key]: { text, revision: current.revision + 1 } };
}

// An acknowledgement clears only the revision that was submitted.
export function applyAcknowledgement(
  drafts: DraftMap,
  key: string,
  revision: number,
  nextKey?: string,
): DraftMap {
  const source = drafts[key] ?? { text: '', revision: 0 };
  const settled = source.revision === revision
    ? { text: '', revision: source.revision + 1 }
    : source;
  if (!nextKey || nextKey === key) return { ...drafts, [key]: settled };
  // The new row can be opened and edited before its creation response arrives.
  if (drafts[nextKey]) {
    const next = { ...drafts };
    if (source.revision === revision) delete next[key];
    return next;
  }
  const next = { ...drafts, [nextKey]: settled };
  delete next[key];
  return next;
}

/** Delay after `failures` failed reads. `null` means the retries are exhausted. */
export function readRetryDelayMs(failures: number): number | null {
  if (failures < 1 || failures > readDelaysMs.length) return null;
  return readDelaysMs[failures - 1];
}

export type ReadKind = 'retry' | 'missing' | 'terminal';
export type WriteKind = 'rejected' | 'missing' | 'unknown';

export function classifyReadFailure(error: unknown): ReadKind {
  if (error instanceof ChaProtocolError) return 'terminal';
  if (error instanceof ChaWebError) {
    if (error.status === 404 || error.code === 'not_found') return 'missing';
    if (error.message.includes('incompatible response')) return 'terminal';
    if (error.status === 0 || error.status === 408 || error.status === 429
        || error.status === 500 || error.status === 502
        || error.status === 503 || error.status === 504) {
      return 'retry';
    }
    return 'terminal';
  }
  return 'retry';
}

export function classifyWriteFailure(error: unknown): WriteKind {
  if (error instanceof ChaWebError) {
    if (error.status === 404 && error.code === 'not_found') return 'missing';
    if (((error.status === 400 || error.status === 415 || error.status === 422)
          && error.code === 'invalid_argument')
        || (error.status === 400 && error.code === 'prompt_too_large')
        || error.status === 413) {
      return 'rejected';
    }
  }
  return 'unknown';
}

export interface CommandInput {
  kind: 'draft' | 'session';
  forumValid: boolean;
  text: string;
  createPending: boolean;
  inputPending: boolean;
  stopPending: boolean;
  stopping: boolean;
  generationActive: boolean;
  snapshotReady: boolean;
  stateUnknown: boolean;
  sendBlocked: boolean;
}

export function commandControl(input: CommandInput): { mode: 'send' | 'stop'; disabled: boolean } {
  const textReady = input.text.trim().length > 0;
  if (input.kind === 'draft') {
    return {
      mode: 'send',
      disabled: !input.forumValid || !textReady || input.createPending || input.sendBlocked,
    };
  }
  const showStop = input.inputPending
    || input.stopPending
    || input.stopping
    || input.generationActive
    || input.stateUnknown
    || input.sendBlocked;
  if (showStop) return { mode: 'stop', disabled: input.stopPending };
  return { mode: 'send', disabled: !input.snapshotReady || !textReady };
}
