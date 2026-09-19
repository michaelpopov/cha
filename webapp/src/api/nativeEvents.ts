import {
  isSessionSnapshot,
  type SessionSnapshot,
} from './client';
import {
  isAppendEvent,
  type AppendEvent,
  type SessionEventConnection,
  type SessionEventHandlers,
  type SessionStreamFailure,
} from './events';
import { isRecord } from './guards';
import type { NativeBridge } from './nativeBridge';

const streamFailure: SessionStreamFailure = Object.freeze({ kind: 'stream_failure' });

export interface NativeSessionEvent {
  connection_id: string;
  context_epoch: number;
  subscription_id: string;
  event: 'session.snapshot' | 'session.append';
  forum_id: string;
  session_id: string;
  seq: number;
  payload: unknown;
}

export function isNativeSessionEvent(value: unknown): value is NativeSessionEvent {
  if (!isRecord(value)
      || typeof value.connection_id !== 'string'
      || !Number.isSafeInteger(value.context_epoch)
      || typeof value.subscription_id !== 'string'
      || (value.event !== 'session.snapshot' && value.event !== 'session.append')
      || typeof value.forum_id !== 'string'
      || typeof value.session_id !== 'string'
      || !Number.isSafeInteger(value.seq)
      || (value.seq as number) < 0) {
    return false;
  }
  if (value.event === 'session.snapshot') return isSessionSnapshot(value.payload);
  return isRecord(value.payload)
    && isRecord(value.payload.target)
    && typeof value.payload.text === 'string';
}

function knownTarget(snapshot: SessionSnapshot, event: AppendEvent): boolean {
  if (event.target.kind === 'entry') {
    const entryId = event.target.entry_id;
    return snapshot.transcript.some((entry) => entry.id === entryId);
  }
  return snapshot.generation.request_id === event.target.request_id;
}

export function createNativeEventProjection(
  scope: {
    connectionId: string;
    contextEpoch: number;
    subscriptionId: string;
    forumId: string;
    sessionId: string;
  },
  handlers: SessionEventHandlers,
): {
  push(event: unknown): void;
  invalidated(): boolean;
} {
  let ready = false;
  let nextSeq = 0;
  let invalidated = false;
  let snapshot: SessionSnapshot | undefined;

  const fail = () => {
    if (invalidated) return;
    invalidated = true;
    handlers.onError(streamFailure);
  };

  return {
    invalidated: () => invalidated,
    push(event: unknown) {
      if (invalidated) return;
      if (!isNativeSessionEvent(event)) {
        fail();
        return;
      }
      if (event.connection_id !== scope.connectionId
          || event.context_epoch !== scope.contextEpoch
          || event.subscription_id !== scope.subscriptionId
          || event.forum_id !== scope.forumId
          || event.session_id !== scope.sessionId) {
        // Stale-epoch events are dropped. Maintenance should later send
        // app.contextChanged and recover with bootstrap + a new subscription.
        return;
      }
      if (event.event === 'session.snapshot') {
        if (ready && event.seq < nextSeq) return;
        snapshot = event.payload as SessionSnapshot;
        nextSeq = event.seq + 1;
        ready = true;
        handlers.onSnapshot(snapshot);
        return;
      }
      if (!ready) {
        fail();
        return;
      }
      if (event.seq < nextSeq) return;
      if (event.seq !== nextSeq) {
        fail();
        return;
      }
      const append = {
        ...(event.payload as object),
        seq: event.seq,
      };
      if (!isAppendEvent(append) || !snapshot || !knownTarget(snapshot, append)) {
        fail();
        return;
      }
      nextSeq += 1;
      handlers.onAppend(append);
    },
  };
}

export function createNativeSessionEvents(
  bridge: NativeBridge,
  options: {
    connectionId: string;
    contextEpoch(): number;
  },
): (
  forumId: string,
  sessionId: string,
  handlers: SessionEventHandlers,
) => SessionEventConnection {
  let nextSubscription = 1;
  return (forumId, sessionId, handlers) => {
    const subscriptionId = `sub-${nextSubscription}`;
    nextSubscription += 1;
    let closed = false;
    const projection = createNativeEventProjection(
      {
        connectionId: options.connectionId,
        contextEpoch: options.contextEpoch(),
        subscriptionId,
        forumId,
        sessionId,
      },
      {
        onSnapshot: (snapshot) => {
          if (!closed) handlers.onSnapshot(snapshot);
        },
        onAppend: (event) => {
          if (!closed) handlers.onAppend(event);
        },
        onError: (failure) => {
          if (!closed) handlers.onError(failure);
        },
      },
    );
    const off = bridge.on('session', (event) => projection.push(event));
    void bridge.invoke('session.subscribe', {
      forum_id: forumId,
      session_id: sessionId,
      subscription_id: subscriptionId,
    }).catch(() => {
      if (!closed) handlers.onError(streamFailure);
    });
    return {
      close() {
        if (closed) return;
        closed = true;
        off();
        void bridge.invoke('session.unsubscribe', {
          forum_id: forumId,
          session_id: sessionId,
          subscription_id: subscriptionId,
        }).catch(() => undefined);
      },
    } satisfies SessionEventConnection;
  };
}
