import type { components } from './schema';
import type { SessionSnapshot } from './client';
import { isRecord } from './guards';

export type AppendEvent = components['schemas']['AppendEvent'];

// A stream ends either because it broke, which the reconnect ladder repairs,
// or because the reader opened this session on another device and that device
// now holds it. Reconnecting after a takeover would only take the session
// back, so the two endings must not look alike here.
export interface SessionStreamFailure {
  readonly kind: 'stream_failure' | 'superseded';
}

export interface SessionEventHandlers {
  onSnapshot(snapshot: SessionSnapshot): void;
  onAppend(event: AppendEvent): void;
  onError(failure: SessionStreamFailure): void;
}

export interface SessionEventConnection {
  close(): void;
}

export function isAppendEvent(value: unknown): value is AppendEvent {
  if (!isRecord(value) || !isRecord(value.target)) return false;
  if (typeof value.text !== 'string'
      || !Number.isSafeInteger(value.seq)
      || (value.seq as number) < 0) return false;
  if (value.target.kind === 'entry') return typeof value.target.entry_id === 'number';
  if (value.target.kind === 'reasoning') return typeof value.target.request_id === 'number';
  return false;
}
