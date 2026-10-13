import { afterEach, describe, expect, it, vi } from 'vitest';

import {
  defaultPrepareClose,
  installAppCloseHost,
  replyNativeClose,
  requestNativeClose,
} from './nativeClose';

afterEach(() => {
  delete window.__CHA_NATIVE_POST__;
  delete window.__CHA_NATIVE_CONNECTION_ID__;
  delete window.__CHA_NATIVE_PREPARE_CLOSE__;
  delete window.__CHA_NATIVE_CLOSE_ABORTED__;
});

describe('native close messages', () => {
  it('posts a typed ready reply and a page close on the current connection', () => {
    const post = vi.fn();
    window.__CHA_NATIVE_POST__ = post;
    window.__CHA_NATIVE_CONNECTION_ID__ = 'view-4';
    defaultPrepareClose(3);
    requestNativeClose();
    expect(post.mock.calls.map((call) => JSON.parse(call[0] as string))).toEqual([
      {
        native_action: 'close_reply',
        connection_id: 'view-4',
        attempt_id: 3,
        result: 'ready',
      },
      { native_action: 'close', connection_id: 'view-4' },
    ]);
  });

  it('rejects an unsafe attempt and a missing connection', () => {
    const post = vi.fn();
    window.__CHA_NATIVE_POST__ = post;
    replyNativeClose(1, 'cancelled');
    window.__CHA_NATIVE_CONNECTION_ID__ = 'view-4';
    replyNativeClose(0, 'ready');
    replyNativeClose(1.5, 'ready');
    expect(post).not.toHaveBeenCalled();
  });

  it('replaces the injected ready responder before an editor exists', () => {
    const post = vi.fn();
    window.__CHA_NATIVE_POST__ = post;
    window.__CHA_NATIVE_CONNECTION_ID__ = 'view-4';
    window.__CHA_NATIVE_PREPARE_CLOSE__ = defaultPrepareClose;
    const host = {
      prepareClose: vi.fn(),
      closeAborted: vi.fn(),
    };
    installAppCloseHost(() => host);
    window.__CHA_NATIVE_PREPARE_CLOSE__?.(8);
    window.__CHA_NATIVE_CLOSE_ABORTED__?.(8);
    window.__CHA_NATIVE_PREPARE_CLOSE__?.(0);
    expect(host.prepareClose).toHaveBeenCalledTimes(1);
    expect(host.prepareClose).toHaveBeenCalledWith(8);
    expect(host.closeAborted).toHaveBeenCalledWith(8);
    expect(post).not.toHaveBeenCalled();
  });
});
