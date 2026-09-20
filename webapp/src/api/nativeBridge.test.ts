import { readFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { describe, expect, it, vi } from 'vitest';

import { ChaError, ChaProtocolError } from './client';
import {
  createEnvelopeNativeBridge,
  createFakeNativeBridge,
  dispatchDeliveryBatch,
  installNativeHostBridge,
} from './nativeBridge';

const wireDirectory = join(
  dirname(fileURLToPath(import.meta.url)),
  '../../../tests/fixtures/wire',
);

function loadFixture(name: string): unknown {
  return JSON.parse(readFileSync(join(wireDirectory, name), 'utf8'));
}

const methodPolicies = loadFixture('native-method-policies.json') as Record<string, {
  requires_context_epoch: boolean;
  changes_context: boolean;
}>;

describe('native bridge', () => {
  it.each(Object.entries(methodPolicies))(
    'applies the shared reply epoch requirement for %s',
    async (method, policy) => {
      const bridge = createEnvelopeNativeBridge({ connectionId: 'current', post: () => {} });
      bridge.setContextEpoch(7);
      const resolved = vi.fn();
      const pending = bridge.invoke(method).then(resolved);
      const reply = { connection_id: 'current', id: 1, ok: true, result: 'completed' };
      bridge.receive({ connection_id: 'current', delivery_id: 1, messages: [
        { ...reply, context_epoch: 6 },
      ] });
      await Promise.resolve();
      expect(resolved).toHaveBeenCalledTimes(policy.requires_context_epoch ? 0 : 1);
      bridge.receive({ connection_id: 'current', delivery_id: 2, messages: [
        { ...reply, context_epoch: 7 },
      ] });
      await pending;
      expect(resolved).toHaveBeenCalledExactlyOnceWith('completed');
      bridge.dispose();
    },
  );

  it.each(Object.entries(methodPolicies))(
    'honors the shared context policy for %s',
    async (method, policy) => {
      const bridge = createEnvelopeNativeBridge({ connectionId: 'current', post: () => {} });
      bridge.setContextEpoch(7);
      const pending = bridge.invoke(method);
      const completed = policy.requires_context_epoch && !policy.changes_context
        ? expect(pending).rejects.toMatchObject({ code: 'vault_changed' })
        : expect(pending).resolves.toEqual('completed');

      bridge.receive({ connection_id: 'current', delivery_id: 1, messages: [
        { connection_id: 'current', event: 'app.contextChanged', context_epoch: 8, state: 'running' },
        { connection_id: 'current', id: 1, context_epoch: 8, ok: true, result: 'completed' },
      ] });
      try {
        await completed;
      } finally {
        bridge.dispose();
      }
    },
  );

  it('does not post an invocation that was already aborted', async () => {
    const posts: unknown[] = [];
    const bridge = createEnvelopeNativeBridge({
      connectionId: 'view-9',
      post: (message) => posts.push(message),
    });
    const cancellation = new AbortController();
    cancellation.abort();

    await expect(bridge.invoke('session.submit', {}, {
      signal: cancellation.signal,
      cancelMethod: 'request.cancel',
    })).rejects.toMatchObject({ name: 'AbortError' });
    expect(posts).toEqual([]);
  });

  it('replays an early receiver failure and only aliases session events', () => {
    const bridge = createEnvelopeNativeBridge({
      connectionId: 'view-9',
      post: () => {},
    });
    bridge.receive({ invalid: true });
    const onReceiverError = vi.fn();
    bridge.on('receiver-error', onReceiverError);
    expect(onReceiverError).toHaveBeenCalledOnce();

    const onSession = vi.fn();
    const onContextChanged = vi.fn();
    bridge.on('session', onSession);
    bridge.on('app.contextChanged', onContextChanged);
    bridge.receive({
      connection_id: 'view-9',
      delivery_id: 1,
      messages: [{ event: 'app.contextChanged', connection_id: 'view-9', context_epoch: 3 }],
    });
    expect(onContextChanged).toHaveBeenCalledOnce();
    expect(onSession).not.toHaveBeenCalled();
    bridge.receive({
      connection_id: 'view-9',
      delivery_id: 2,
      messages: [{ event: 'session.snapshot', connection_id: 'view-9', context_epoch: 3 }],
    });
    expect(onSession).toHaveBeenCalledOnce();
  });

  it('correlates replies, acks deliveries after a receiver error, and rejects on dispose', async () => {
    const posts: unknown[] = [];
    const bridge = createEnvelopeNativeBridge({
      connectionId: 'view-9',
      post: (message) => posts.push(message),
    });
    bridge.setContextEpoch(3);
    const pending = bridge.invoke('session.submit', { forum_id: 'history' });
    expect(posts).toHaveLength(1);
    expect(posts[0]).toMatchObject({
      connection_id: 'view-9',
      id: 1,
      method: 'session.submit',
    });

    const onMessage = vi.fn(() => {
      throw new Error('dispatch failed');
    });
    const ack = vi.fn();
    const onReceiverError = vi.fn();
    dispatchDeliveryBatch(
      {
        connection_id: 'view-9',
        delivery_id: 1,
        messages: [loadFixture('native-reply-command.json')],
      },
      onMessage,
      ack,
      onReceiverError,
    );
    expect(onReceiverError).toHaveBeenCalledOnce();
    expect(ack).toHaveBeenCalledWith('view-9', 1);

    const posted = posts[0] as { id: number };
    const reply = loadFixture('native-reply-command.json') as Record<string, unknown>;
    bridge.receive({
      connection_id: 'view-9',
      delivery_id: 2,
      messages: [{ ...reply, id: posted.id }],
    });
    await expect(pending).resolves.toEqual({ clear_input: true, notice: 'Saved' });
    expect(posts).toContainEqual({ connection_id: 'view-9', delivery_id: 2 });

    const leftover = bridge.invoke('session.stop', {});
    bridge.dispose();
    await expect(leftover).rejects.toBeInstanceOf(ChaProtocolError);
  });

  it('drains host-queued deliveries when the receiver is installed', () => {
    window.__CHA_NATIVE_CONNECTION_ID__ = 'view-9';
    window.__CHA_NATIVE_POST__ = () => {};
    window.__CHA_NATIVE_QUEUE__ = [{
      connection_id: 'view-9',
      delivery_id: 7,
      messages: [{
        connection_id: 'view-9',
        context_epoch: 1,
        subscription_id: 'sub-1',
        event: 'session.snapshot',
        forum_id: 'history',
        session_id: 'session-1',
        seq: 0,
        payload: loadFixture('snapshot.json'),
      }],
    }];
    const bridge = installNativeHostBridge();
    expect(bridge).not.toBeNull();
    bridge?.setContextEpoch(7);
    expect(window.__CHA_NATIVE_CONTEXT_EPOCH__).toBe(7);
    expect(window.__CHA_NATIVE_QUEUE__).toEqual([]);
    expect(window.__CHA_NATIVE_RECEIVE__).toBeTypeOf('function');
    delete window.__CHA_NATIVE_POST__;
    delete window.__CHA_NATIVE_RECEIVE__;
    delete window.__CHA_NATIVE_CONNECTION_ID__;
    delete window.__CHA_NATIVE_CONTEXT_EPOCH__;
    delete window.__CHA_NATIVE_QUEUE__;
  });

  it('turns a real C++ error envelope into ChaError', async () => {
    const bridge = createFakeNativeBridge({
      'session.submit': () => {
        throw new ChaError('session_not_live', 'That session is not open.');
      },
    });
    await expect(bridge.invoke('session.submit', {}))
      .rejects.toMatchObject({ code: 'session_not_live' });
  });

  it('ignores foreign documents and stale epochs without resolving or losing a live request', async () => {
    const post = vi.fn();
    const bridge = createEnvelopeNativeBridge({ connectionId: 'current', post });
    bridge.setContextEpoch(7);
    const resolved = vi.fn();
    const pending = bridge.invoke('session.submit').then(resolved);
    const reply = { connection_id: 'current', context_epoch: 7, id: 1, ok: true, result: 'current' };
    bridge.receive({ connection_id: 'old', delivery_id: 1, messages: [reply] });
    bridge.receive({ connection_id: 'current', delivery_id: 2,
      messages: [{ ...reply, connection_id: 'old' }] });
    bridge.receive({ connection_id: 'current', delivery_id: 3,
      messages: [{ ...reply, context_epoch: 6 }] });
    await Promise.resolve();
    expect(resolved).not.toHaveBeenCalled();
    expect(post).toHaveBeenCalledWith({ connection_id: 'old', delivery_id: 1 });
    bridge.receive({ connection_id: 'current', delivery_id: 4, messages: [reply] });
    await pending;
    expect(resolved).toHaveBeenCalledWith('current');
  });

  it('rejects old-context work but preserves the initiating maintenance result', async () => {
    const bridge = createEnvelopeNativeBridge({ connectionId: 'current', post: () => {} });
    bridge.setContextEpoch(7);
    const old = bridge.invoke('provider.list');
    const changed = expect(old).rejects.toMatchObject({ code: 'vault_changed' });
    const maintenance = bridge.invoke('vault.switch');
    bridge.receive({ connection_id: 'current', delivery_id: 1, messages: [
      { connection_id: 'current', event: 'app.contextChanged', context_epoch: 8, state: 'running' },
      { connection_id: 'current', id: 1, context_epoch: 7, ok: true, result: ['old'] },
      { connection_id: 'current', id: 2, context_epoch: 8, ok: true, result: { context_epoch: 8 } },
    ] });
    await changed;
    await expect(maintenance).resolves.toEqual({ context_epoch: 8 });
    bridge.setContextEpoch(7);
    expect(bridge.contextEpoch()).toBe(8);
    expect(bridge).not.toHaveProperty('acks');
  });
});
