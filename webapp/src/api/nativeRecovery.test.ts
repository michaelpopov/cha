import { describe, expect, it, vi } from 'vitest';

import type { NativeBridge } from './nativeBridge';
import { installNativeRecoveryHandlers } from './nativeRecovery';

describe('native document recovery', () => {
  it('resets the route and reloads for context changes and terminal invalidation', () => {
    const handlers = new Map<string, (payload?: unknown) => void>();
    const native: Pick<NativeBridge, 'on'> = {
      on(event, handler) {
        handlers.set(event, handler as (payload?: unknown) => void);
        return () => handlers.delete(event);
      },
    };
    const resetRoute = vi.fn();
    const reload = vi.fn();
    installNativeRecoveryHandlers(native, resetRoute, reload);

    handlers.get('app.connectionInvalidated')?.();
    expect(resetRoute).toHaveBeenCalledOnce();
    expect(reload).toHaveBeenCalledOnce();

    handlers.get('app.contextChanged')?.({});
    expect(resetRoute).toHaveBeenCalledTimes(2);
    expect(reload).toHaveBeenCalledTimes(2);

    handlers.get('app.contextChanged')?.({ causing_request_id: 7, state: 'running' });
    expect(resetRoute).toHaveBeenCalledTimes(2);
    expect(reload).toHaveBeenCalledTimes(2);

    handlers.get('app.contextChanged')?.({ causing_request_id: 7, state: 'unavailable' });
    expect(resetRoute).toHaveBeenCalledTimes(3);
    expect(reload).toHaveBeenCalledTimes(3);

    handlers.get('receiver-error')?.();
    expect(resetRoute).toHaveBeenCalledTimes(3);
    expect(reload).toHaveBeenCalledTimes(4);
  });
});
