import {
  ChaError,
  ChaProtocolError,
  type ErrorCode,
} from './client';
import { isRecord } from './guards';

export const nativeProtocolVersion = 1;

export interface NativeBridge {
  invoke<T>(method: string, params?: unknown): Promise<T>;
  on<T>(event: string, handler: (payload: T) => void): () => void;
  dispose(): void;
  setContextEpoch(epoch: number): void;
  contextEpoch(): number;
}

export interface NativeDeliveryBatch {
  connection_id: string;
  delivery_id: number;
  messages: unknown[];
}

const knownErrorCodes: Record<string, true> = {
  not_found: true,
  bad_request: true,
  body_too_large: true,
  prompt_too_large: true,
  forbidden_origin: true,
  internal_error: true,
  speech_busy: true,
  vault_changed: true,
  session_stopping: true,
  session_limit_reached: true,
  session_open_timeout: true,
  server_stopping: true,
  session_not_live: true,
  command_timeout: true,
  command_queue_full: true,
  vault_password_required: true,
  source_vault_password_required: true,
  invalid_argument: true,
  operation_cancelled: true,
  application_unavailable: true,
};

function isErrorCode(value: unknown): value is ErrorCode {
  return typeof value === 'string' && Object.hasOwn(knownErrorCodes, value);
}

export function isNativeDeliveryBatch(value: unknown): value is NativeDeliveryBatch {
  return isRecord(value)
    && typeof value.connection_id === 'string'
    && Number.isSafeInteger(value.delivery_id)
    && (value.delivery_id as number) >= 0
    && Array.isArray(value.messages);
}

export function isNativeReply(value: unknown): value is {
  connection_id: string;
  id: number;
  context_epoch: number;
  ok: boolean;
  result?: unknown;
  error?: { code: string; message: string };
} {
  return isRecord(value)
    && typeof value.connection_id === 'string'
    && Number.isSafeInteger(value.id)
    && Number.isSafeInteger(value.context_epoch)
    && typeof value.ok === 'boolean';
}

type Pending = {
  resolve: (value: unknown) => void;
  reject: (error: unknown) => void;
};

type EventHandler = (payload: unknown) => void;

export function dispatchDeliveryBatch(
  batch: unknown,
  onMessage: (message: unknown) => void,
  ack: (connectionId: string, deliveryId: number) => void,
  onReceiverError?: () => void,
): void {
  if (!isNativeDeliveryBatch(batch)) {
    onReceiverError?.();
    return;
  }
  try {
    for (const message of batch.messages) {
      try {
        onMessage(message);
      } catch {
        onReceiverError?.();
      }
    }
  } finally {
    ack(batch.connection_id, batch.delivery_id);
  }
}

export function createEnvelopeNativeBridge(options: {
  connectionId: string;
  post(message: unknown): void;
  onAck?(connectionId: string, deliveryId: number): void;
}): NativeBridge & {
  receive(batch: unknown): void;
  acks: Array<{ connection_id: string; delivery_id: number }>;
} {
  let epoch = 0;
  let nextId = 1;
  let disposed = false;
  const pending = new Map<number, Pending>();
  const listeners = new Map<string, Set<EventHandler>>();
  const acks: Array<{ connection_id: string; delivery_id: number }> = [];

  const emit = (event: string, payload: unknown) => {
    listeners.get(event)?.forEach((handler) => handler(payload));
  };

  const rejectAll = (error: unknown) => {
    for (const request of pending.values()) request.reject(error);
    pending.clear();
  };

  const handleMessage = (message: unknown) => {
    if (isNativeReply(message)) {
      const request = pending.get(message.id);
      if (!request) return;
      pending.delete(message.id);
      if (message.ok) {
        request.resolve(message.result);
        return;
      }
      const code = message.error && isErrorCode(message.error.code)
        ? message.error.code
        : 'internal_error';
      const text = message.error?.message ?? 'The request could not be completed.';
      request.reject(new ChaError(0, code, text));
      return;
    }
    if (isRecord(message) && typeof message.event === 'string') {
      emit(message.event as string, message);
      emit('session', message);
    }
  };

  const receive = (batch: unknown) => {
    if (disposed) return;
    dispatchDeliveryBatch(
      batch,
      handleMessage,
      (connectionId, deliveryId) => {
        acks.push({ connection_id: connectionId, delivery_id: deliveryId });
        options.onAck?.(connectionId, deliveryId);
        options.post({ connection_id: connectionId, delivery_id: deliveryId });
      },
      () => emit('receiver-error', undefined),
    );
  };

  return {
    acks,
    receive,
    setContextEpoch(value: number) {
      epoch = value;
    },
    contextEpoch() {
      return epoch;
    },
    invoke<T>(method: string, params?: unknown): Promise<T> {
      if (disposed) return Promise.reject(new ChaProtocolError());
      const id = nextId;
      nextId += 1;
      // Dev leftover: the spec replaces the connection before exhausting
      // JS-safe integers. Dispose is enough until the host does that.
      if (nextId > Number.MAX_SAFE_INTEGER) {
        rejectAll(new ChaProtocolError());
        disposed = true;
        return Promise.reject(new ChaProtocolError());
      }
      // info/bootstrap ignore the value; 0 until the first bootstrap result.
      return new Promise<T>((resolve, reject) => {
        pending.set(id, {
          resolve: (value) => resolve(value as T),
          reject,
        });
        options.post({
          connection_id: options.connectionId,
          id,
          context_epoch: epoch,
          method,
          params: params ?? {},
        });
      });
    },
    on<T>(event: string, handler: (payload: T) => void): () => void {
      const set = listeners.get(event) ?? new Set<EventHandler>();
      const wrapped: EventHandler = (payload) => handler(payload as T);
      set.add(wrapped);
      listeners.set(event, set);
      return () => {
        set.delete(wrapped);
      };
    },
    dispose() {
      if (disposed) return;
      disposed = true;
      listeners.clear();
      rejectAll(new ChaProtocolError());
    },
  };
}

export function createFakeNativeBridge(
  handlers: Record<string, (params: unknown) => unknown | Promise<unknown>> = {},
): NativeBridge & {
  receive: (batch: unknown) => void;
  posts: unknown[];
  acks: Array<{ connection_id: string; delivery_id: number }>;
} {
  const posts: unknown[] = [];
  const envelope = createEnvelopeNativeBridge({
    connectionId: 'view-9',
    post(message) {
      posts.push(message);
      if (!isRecord(message) || typeof message.method !== 'string') return;
      const handler = handlers[message.method];
      if (!handler) return;
      void Promise.resolve(handler(message.params)).then((result) => {
        envelope.receive({
          connection_id: 'view-9',
          delivery_id: envelope.acks.length + 1,
          messages: [{
            connection_id: 'view-9',
            id: message.id,
            context_epoch: message.context_epoch,
            ok: true,
            result,
          }],
        });
      }).catch((failure: unknown) => {
        const error = failure instanceof ChaError
          ? { code: failure.code, message: failure.message }
          : { code: 'internal_error', message: 'The request could not be completed.' };
        envelope.receive({
          connection_id: 'view-9',
          delivery_id: envelope.acks.length + 1,
          messages: [{
            connection_id: 'view-9',
            id: message.id,
            context_epoch: message.context_epoch,
            ok: false,
            error,
          }],
        });
      });
    },
  });
  return Object.assign(envelope, { posts });
}

declare global {
  interface Window {
    __CHA_NATIVE_POST__?: (message: string) => void;
    __CHA_NATIVE_CONNECTION_ID__?: string;
    __CHA_NATIVE_RECEIVE__?: (batch: unknown) => void;
  }
}

export function installNativeHostBridge(): NativeBridge | null {
  if (typeof window === 'undefined' || !window.__CHA_NATIVE_POST__) return null;
  const post = window.__CHA_NATIVE_POST__;
  const connectionId = window.__CHA_NATIVE_CONNECTION_ID__ ?? 'view-1';
  const bridge = createEnvelopeNativeBridge({
    connectionId,
    post(message) {
      post(JSON.stringify(message));
    },
  });
  window.__CHA_NATIVE_RECEIVE__ = (batch) => bridge.receive(batch);
  return bridge;
}
