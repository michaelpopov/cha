// The bounded reconnect ladder from webapp.md, kept free of React and of the
// HTTP client so a test can drive it with fake steps and no clock.
//
// Attaching is what gives the ladder its shape. A new EventSource reports
// success only when its first snapshot arrives, so `attach` resolves true then
// and false when the stream fails instead. A stream that connects and drops
// later therefore ends this ladder successfully and a fresh one begins, which
// is what stops a long conversation from spending its retries one rung at a
// time across an afternoon.

import { ChaError, type ChaClient, type SessionSnapshot } from '../api/client';

export const reconnectingMessage = 'Reconnecting live updates…';
export const waitingForCapacityMessage = 'Waiting for another session to close';

export function isSessionLimit(failure: unknown): failure is ChaError {
  return failure instanceof ChaError && failure.code === 'session_limit_reached';
}

function isSessionNotLive(failure: unknown): failure is ChaError {
  return failure instanceof ChaError && failure.code === 'session_not_live';
}

// What one snapshot probe learned about the session behind a dead stream. The
// caller classifies the API failures; this module only routes the answers.
export type ProbeOutcome =
  | 'live' // The session answered, so its one stream slot is the open question.
  | 'recovered' // It had been unloaded, and was opened again.
  | 'waiting-for-capacity' // Re-opening needs a place the server has not freed.
  | 'unavailable'; // The probe itself failed.

export type RecoveryOutcome = 'connected' | 'other-window' | 'retry' | 'cancelled';

export interface RecoverySteps {
  probe(): Promise<ProbeOutcome>;
  report(message: string): void;
  // False once the wait was cancelled, which ends the ladder.
  wait(milliseconds: number): Promise<boolean>;
  // True once a new stream has delivered its first snapshot.
  attach(): Promise<boolean>;
  cancelled(): boolean;
}

export interface SessionProbeOptions {
  client: ChaClient;
  forumId: string;
  sessionId: string;
  cancelled(): boolean;
  onSnapshot(snapshot: SessionSnapshot): void;
}

// The probe a rung runs before it attaches. A snapshot is the only way to
// learn why a stream died, because EventSource exposes neither the status nor
// the error body of the response that refused it.
export function sessionProbe({
  client,
  forumId,
  sessionId,
  cancelled,
  onSnapshot,
}: SessionProbeOptions): () => Promise<ProbeOutcome> {
  return async () => {
    try {
      const snapshot = await client.getSessionSnapshot(forumId, sessionId);
      if (cancelled()) return 'unavailable';
      onSnapshot(snapshot);
      return 'live';
    } catch (failure: unknown) {
      if (!isSessionNotLive(failure)) return 'unavailable';
    }
    // The runtime unloaded this conversation while nothing was watching it.
    // The stored session is still there to be opened again.
    try {
      await client.openSession(forumId, sessionId);
      if (cancelled()) return 'unavailable';
      const snapshot = await client.getSessionSnapshot(forumId, sessionId);
      if (cancelled()) return 'unavailable';
      onSnapshot(snapshot);
      return 'recovered';
    } catch (failure: unknown) {
      return isSessionLimit(failure) ? 'waiting-for-capacity' : 'unavailable';
    }
  };
}

export async function recoverSessionStream(
  delays: readonly number[],
  steps: RecoverySteps,
): Promise<RecoveryOutcome> {
  // Every probe answering while every stream attempt fails is what a session
  // whose one stream is held by another window looks like from here:
  // EventSource never exposes the 409 that would say so directly.
  let everyProbeSucceeded = true;

  for (const delay of delays) {
    if (steps.cancelled()) return 'cancelled';
    const outcome = await steps.probe();
    if (steps.cancelled()) return 'cancelled';
    if (outcome !== 'live') everyProbeSucceeded = false;

    steps.report(
      outcome === 'waiting-for-capacity' ? waitingForCapacityMessage : reconnectingMessage,
    );
    if (!await steps.wait(delay)) return 'cancelled';

    if (outcome === 'live' || outcome === 'recovered') {
      if (await steps.attach()) return 'connected';
      if (steps.cancelled()) return 'cancelled';
    }
  }

  return everyProbeSucceeded ? 'other-window' : 'retry';
}
