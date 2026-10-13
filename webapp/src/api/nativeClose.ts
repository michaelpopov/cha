export type CloseReplyResult = 'ready' | 'cancelled';

export interface CloseReplyMessage {
  native_action: 'close_reply';
  connection_id: string;
  attempt_id: number;
  result: CloseReplyResult;
}

export interface PageCloseMessage {
  native_action: 'close';
  connection_id: string;
}

export interface PageCloseHost {
  prepareClose(attemptId: number): void;
  closeAborted(attemptId: number): void;
}

export function isSafeAttemptId(value: unknown): value is number {
  return typeof value === 'number' && Number.isSafeInteger(value) && value > 0;
}

function postNativeMessage(message: CloseReplyMessage | PageCloseMessage) {
  const post = window.__CHA_NATIVE_POST__;
  const connectionId = window.__CHA_NATIVE_CONNECTION_ID__;
  if (!post || !connectionId || message.connection_id !== connectionId) return;
  post(JSON.stringify(message));
}

export function requestNativeClose() {
  const connectionId = window.__CHA_NATIVE_CONNECTION_ID__;
  if (!connectionId) return;
  postNativeMessage({ native_action: 'close', connection_id: connectionId });
}

export function replyNativeClose(attemptId: number, result: CloseReplyResult) {
  const connectionId = window.__CHA_NATIVE_CONNECTION_ID__;
  if (!connectionId || !isSafeAttemptId(attemptId)) return;
  postNativeMessage({
    native_action: 'close_reply',
    connection_id: connectionId,
    attempt_id: attemptId,
    result,
  });
}

// The injected document-start script uses this same ready reply until App
// replaces the two host entry points. No editor can exist before that.
export function defaultPrepareClose(attemptId: number) {
  replyNativeClose(attemptId, 'ready');
}

export function installAppCloseHost(readHost: () => PageCloseHost) {
  window.__CHA_NATIVE_PREPARE_CLOSE__ = (attemptId) => {
    if (!isSafeAttemptId(attemptId)) return;
    readHost().prepareClose(attemptId);
  };
  window.__CHA_NATIVE_CLOSE_ABORTED__ = (attemptId) => {
    if (!isSafeAttemptId(attemptId)) return;
    readHost().closeAborted(attemptId);
  };
}

declare global {
  interface Window {
    __CHA_NATIVE_PREPARE_CLOSE__?: (attemptId: number) => void;
    __CHA_NATIVE_CLOSE_ABORTED__?: (attemptId: number) => void;
  }
}
