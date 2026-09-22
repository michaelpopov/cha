import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import { installNativeHostBridge } from './api/nativeBridge';
import { createNativeChaClient } from './api/nativeClient';
import { createNativeSessionEvents } from './api/nativeEvents';
import { installNativeRecoveryHandlers } from './api/nativeRecovery';
import { App } from './components/App';
import './styles/app.css';

const root = document.getElementById('root');

if (!root) {
  throw new Error('The cha application root is missing.');
}

const native = installNativeHostBridge();
if (!native) {
  throw new Error('CHA requires the native host bridge.');
}
installNativeRecoveryHandlers(native);
const app = (
  <App
    client={createNativeChaClient(native)}
    contextEvents={native}
    connectSessionEvents={createNativeSessionEvents(native, {
      connectionId: window.__CHA_NATIVE_CONNECTION_ID__ ?? 'view-1',
      contextEpoch: () => native.contextEpoch(),
    })}
  />
);

createRoot(root).render(
  <StrictMode>
    {app}
  </StrictMode>,
);
