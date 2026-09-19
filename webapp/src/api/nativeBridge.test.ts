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

describe('native bridge', () => {
  it('correlates replies, acks deliveries after a receiver error, and rejects on dispose', async () => {
    const posts: unknown[] = [];
    const bridge = createEnvelopeNativeBridge({
      connectionId: 'view-9',
      post: (message) => posts.push(message),
    });

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
    expect(bridge.acks).toContainEqual({ connection_id: 'view-9', delivery_id: 2 });

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
    expect(window.__CHA_NATIVE_QUEUE__).toEqual([]);
    expect(window.__CHA_NATIVE_RECEIVE__).toBeTypeOf('function');
    delete window.__CHA_NATIVE_POST__;
    delete window.__CHA_NATIVE_RECEIVE__;
    delete window.__CHA_NATIVE_CONNECTION_ID__;
    delete window.__CHA_NATIVE_QUEUE__;
  });

  it('turns a real C++ error envelope into ChaError', async () => {
    const bridge = createFakeNativeBridge({
      'session.submit': () => {
        throw new ChaError(0, 'session_not_live', 'That session is not open.');
      },
    });
    await expect(bridge.invoke('session.submit', {}))
      .rejects.toMatchObject({ code: 'session_not_live' });
  });
});
