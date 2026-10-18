import type { components } from './schema';
import type { SessionSnapshot } from './client';
import { isRecord } from './guards';

export type AppendEvent = components['schemas']['AppendEvent'];

export interface SessionEventHandlers {
  onSnapshot(snapshot: SessionSnapshot): void;
  onAppend(event: AppendEvent): void;
  onGeneration(generation: SessionSnapshot['generation']): void;
  onError(): void;
}

export interface SessionEventConnection {
  close(): void;
}

export function isAppendEvent(value: unknown): value is AppendEvent {
  if (!isRecord(value) || !isRecord(value.target)) return false;
  if (typeof value.text !== 'string'
      || !Number.isSafeInteger(value.seq)
      || (value.seq as number) < 0) return false;
  return value.target.kind === 'entry' && typeof value.target.entry_id === 'number';
}
