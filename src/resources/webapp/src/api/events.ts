import type { components } from './schema';
import { sessionEventsUrl, type SessionSnapshot } from './client';

export type AppendEvent = components['schemas']['AppendEvent'];

export interface SessionStreamFailure {
  readonly kind: 'stream_failure';
}

export interface SessionEventHandlers {
  onSnapshot(snapshot: SessionSnapshot): void;
  onAppend(event: AppendEvent): void;
  onError(failure: SessionStreamFailure): void;
}

interface EventSourceLike {
  addEventListener(type: string, listener: (event: MessageEvent<string>) => void): void;
  close(): void;
  onerror: ((event: Event) => void) | null;
}

export type EventSourceFactory = (url: string) => EventSourceLike;

export interface SessionEventConnection {
  close(): void;
}

const streamFailure: SessionStreamFailure = Object.freeze({ kind: 'stream_failure' });

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function isSnapshot(value: unknown): value is SessionSnapshot {
  return isRecord(value)
    && isRecord(value.forum)
    && typeof value.session_id === 'string'
    && typeof value.session_label === 'string'
    && Array.isArray(value.characters)
    && typeof value.default_character_id === 'string'
    && Array.isArray(value.transcript)
    && isRecord(value.generation)
    && typeof value.lifecycle === 'string';
}

function isAppend(value: unknown): value is AppendEvent {
  if (!isRecord(value) || !isRecord(value.target)) return false;
  if (typeof value.text !== 'string' || typeof value.seq !== 'number') return false;
  if (value.target.kind === 'entry') return typeof value.target.entry_id === 'number';
  if (value.target.kind === 'reasoning') return typeof value.target.request_id === 'number';
  return false;
}

function parseEvent<T>(data: string, accepts: (value: unknown) => value is T): T {
  const value: unknown = JSON.parse(data);
  if (!accepts(value)) throw new TypeError('Malformed CHA session event.');
  return value;
}

export function openSessionEvents(
  forumId: string,
  sessionId: string,
  handlers: SessionEventHandlers,
  createEventSource: EventSourceFactory = (url) => new EventSource(url),
): SessionEventConnection {
  const source = createEventSource(sessionEventsUrl(forumId, sessionId));
  let closed = false;
  let failureReported = false;

  const reportFailure = () => {
    if (closed || failureReported) return;
    failureReported = true;
    handlers.onError(streamFailure);
  };

  source.addEventListener('snapshot', (event) => {
    if (closed) return;
    try {
      handlers.onSnapshot(parseEvent(event.data, isSnapshot));
    } catch {
      reportFailure();
    }
  });
  source.addEventListener('append', (event) => {
    if (closed) return;
    try {
      handlers.onAppend(parseEvent(event.data, isAppend));
    } catch {
      reportFailure();
    }
  });
  source.onerror = reportFailure;

  return {
    close() {
      if (closed) return;
      closed = true;
      source.close();
    },
  };
}
