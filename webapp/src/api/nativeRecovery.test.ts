import { describe, expect, it, vi } from 'vitest';

import type { NativeBridge } from './nativeBridge';
import { installNativeRecoveryHandlers } from './nativeRecovery';

describe('native document recovery', () => {
  it('resets the route and reloads for context changes and terminal invalidation', () => {
    const handlers = new Map<string, () => void>();
    const native: Pick<NativeBridge, 'on'> = {
      on(event, handler) {
        handlers.set(event, handler as () => void);
        return () => handlers.delete(event);
      },
    };
    const resetRoute = vi.fn();
    const reload = vi.fn();
    installNativeRecoveryHandlers(native, resetRoute, reload);

    handlers.get('app.connectionInvalidated')?.();
    expect(resetRoute).toHaveBeenCalledOnce();
    expect(reload).toHaveBeenCalledOnce();

    handlers.get('app.contextChanged')?.();
    expect(resetRoute).toHaveBeenCalledTimes(2);
    expect(reload).toHaveBeenCalledTimes(2);

    handlers.get('receiver-error')?.();
    expect(resetRoute).toHaveBeenCalledTimes(2);
    expect(reload).toHaveBeenCalledTimes(3);
  });
});
