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
  native.on('app.contextChanged', resetAndReload);
  native.on('app.connectionInvalidated', resetAndReload);
  native.on('receiver-error', reload);
}
