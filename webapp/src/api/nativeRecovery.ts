import type { NativeBridge } from './nativeBridge';
import { reloadApplication, writeAppRoute } from '../state/route';

export function installNativeRecoveryHandlers(
  native: Pick<NativeBridge, 'on'>,
  resetRoute: () => void = () => writeAppRoute('/', 'replace'),
  reload: () => void = () => reloadApplication(),
): void {
  const resetAndReload = () => {
    resetRoute();
    reload();
  };
  native.on<{ causing_request_id?: number; state?: string }>('app.contextChanged', (event) => {
    if (!Number.isSafeInteger(event.causing_request_id) || event.state !== 'running') {
      resetAndReload();
    }
  });
  native.on('app.connectionInvalidated', resetAndReload);
  native.on('receiver-error', reload);
}
