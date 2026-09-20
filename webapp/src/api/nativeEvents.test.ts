import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it, vi } from 'vitest';

import { isSessionSnapshot } from './client';
import { createEnvelopeNativeBridge } from './nativeBridge';
import {
  createNativeEventProjection,
  createNativeSessionEvents,
  isNativeSessionEvent,
} from './nativeEvents';
import { snapshotFixture } from '../test/fixtures';

const wireDirectory = join(
  dirname(fileURLToPath(import.meta.url)),
  '../../../tests/fixtures/wire',
);

function loadFixture(name: string): unknown {
  return JSON.parse(readFileSync(join(wireDirectory, name), 'utf8'));
}

const snapshot = loadFixture('snapshot.json');

function event(overrides: Record<string, unknown> = {}) {
  return {
    connection_id: 'view-9',
    context_epoch: 3,
    subscription_id: 'sub-5',
    event: 'session.snapshot',
    forum_id: 'history',
    session_id: 'session-1',
    seq: 0,
    payload: snapshot,
    ...overrides,
  };
}

describe('native session projection', () => {
  it('accepts a C++ snapshot fixture and consecutive appends', () => {
    expect(isSessionSnapshot(snapshot)).toBe(true);
    expect(isNativeSessionEvent(event())).toBe(true);
    expect(isNativeSessionEvent(loadFixture('native-event-append.json'))).toBe(true);

    const onSnapshot = vi.fn();
    const onAppend = vi.fn();
    const onError = vi.fn();
    const projection = createNativeEventProjection(
      {
        connectionId: 'view-9',
        contextEpoch: 3,
        subscriptionId: 'sub-5',
        forumId: 'history',
        sessionId: 'session-1',
      },
      { onSnapshot, onAppend, onError },
    );

    projection.push(event());
    projection.push(event({
      event: 'session.append',
      seq: 1,
      payload: { target: { kind: 'entry', entry_id: 1 }, text: 'Hello' },
    }));
    expect(onSnapshot).toHaveBeenCalledOnce();
    expect(onAppend).toHaveBeenCalledWith({
      target: { kind: 'entry', entry_id: 1 },
      text: 'Hello',
      seq: 1,
    });
    expect(onError).not.toHaveBeenCalled();
  });

  it('lets a newer snapshot set the baseline and ignores stale subscriptions', () => {
    const onSnapshot = vi.fn();
    const onAppend = vi.fn();
    const onError = vi.fn();
    const projection = createNativeEventProjection(
      {
        connectionId: 'view-9',
        contextEpoch: 3,
        subscriptionId: 'sub-5',
        forumId: 'history',
        sessionId: 'session-1',
      },
      { onSnapshot, onAppend, onError },
    );

    projection.push(event());
    projection.push(event({ seq: 4 }));
    projection.push(event({
      event: 'session.append',
      seq: 5,
      payload: { target: { kind: 'entry', entry_id: 1 }, text: 'later' },
    }));
    projection.push(event({ subscription_id: 'sub-old', seq: 6 }));
    projection.push(event({ subscription_id: 'sub-old', payload: null }));
    expect(onSnapshot).toHaveBeenCalledTimes(2);
    expect(onAppend).toHaveBeenCalledOnce();
    expect(onError).not.toHaveBeenCalled();
  });

  it('invalidates on gaps, unknown targets, and appends before a snapshot', () => {
    const handlers = {
      onSnapshot: vi.fn(),
      onAppend: vi.fn(),
      onError: vi.fn(),
    };
    const gap = createNativeEventProjection(
      {
        connectionId: 'view-9',
        contextEpoch: 3,
        subscriptionId: 'sub-5',
        forumId: 'history',
        sessionId: 'session-1',
      },
      handlers,
    );
    gap.push(event());
    gap.push(event({
      event: 'session.append',
      seq: 2,
      payload: { target: { kind: 'entry', entry_id: 1 }, text: 'gap' },
    }));
    expect(handlers.onError).toHaveBeenCalledWith({ kind: 'stream_failure' });
    gap.push(event({ seq: 3 }));
    expect(handlers.onSnapshot).toHaveBeenCalledOnce();

    const unknown = createNativeEventProjection(
      {
        connectionId: 'view-9',
        contextEpoch: 3,
        subscriptionId: 'sub-5',
        forumId: 'history',
        sessionId: 'session-1',
      },
      { onSnapshot: vi.fn(), onAppend: vi.fn(), onError: vi.fn() },
    );
    unknown.push(event());
    unknown.push(event({
      event: 'session.append',
      seq: 1,
      payload: { target: { kind: 'entry', entry_id: 99 }, text: 'missing' },
    }));
    expect(unknown.invalidated()).toBe(true);

    const early = createNativeEventProjection(
      {
        connectionId: 'view-9',
        contextEpoch: 3,
        subscriptionId: 'sub-5',
        forumId: 'history',
        sessionId: 'session-1',
      },
      { onSnapshot: vi.fn(), onAppend: vi.fn(), onError: vi.fn() },
    );
    early.push(event({
      event: 'session.append',
      seq: 1,
      payload: { target: { kind: 'entry', entry_id: 1 }, text: 'early' },
    }));
    expect(early.invalidated()).toBe(true);
  });

  it('installs the handler before subscribe and accepts snapshot without a reply', () => {
    const posts: unknown[] = [];
    const bridge = createEnvelopeNativeBridge({
      connectionId: 'view-9',
      post: (message) => posts.push(message),
    });
    bridge.setContextEpoch(3);
    const onSnapshot = vi.fn();
    const connect = createNativeSessionEvents(bridge, {
      connectionId: 'view-9',
      contextEpoch: () => bridge.contextEpoch(),
    });
    const connection = connect('history', 'session-1', {
      onSnapshot,
      onAppend: vi.fn(),
      onError: vi.fn(),
    });
    expect(posts[0]).toMatchObject({
      method: 'session.subscribe',
      params: { subscription_id: 'sub-1', forum_id: 'history', session_id: 'session-1' },
    });
    bridge.receive({
      connection_id: 'view-9',
      delivery_id: 1,
      messages: [event({ subscription_id: 'sub-1' })],
    });
    expect(onSnapshot).toHaveBeenCalledOnce();
    connection.close();
    expect(posts.at(-1)).toMatchObject({
      method: 'session.unsubscribe',
      params: { subscription_id: 'sub-1' },
    });
  });
});

describe('native events unused HTTP snapshot fixture shape', () => {
  it('keeps the HTTP snapshot fixture compatible with native payload checks', () => {
    expect(isSessionSnapshot(snapshotFixture)).toBe(true);
  });
});
