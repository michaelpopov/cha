import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import { installNativeHostBridge } from './api/nativeBridge';
import { createNativeChaClient } from './api/nativeClient';
import { createNativeSessionEvents } from './api/nativeEvents';
import { App } from './components/App';
import './styles/app.css';

const root = document.getElementById('root');

if (!root) {
  throw new Error('The cha application root is missing.');
}

const native = installNativeHostBridge();
const app = native
  ? (
    <App
      client={createNativeChaClient(native)}
      connectSessionEvents={createNativeSessionEvents(native, {
        connectionId: window.__CHA_NATIVE_CONNECTION_ID__ ?? 'view-1',
        contextEpoch: () => native.contextEpoch(),
      })}
      streamRecovery="replace"
    />
  )
  : <App />;

createRoot(root).render(
  <StrictMode>
    {app}
  </StrictMode>,
);
