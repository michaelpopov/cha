import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';

import { App } from './App';
import { createChaWebClient } from './client';
import './styles.css';

const root = document.getElementById('root');

if (!root) {
  throw new Error('The cha application root is missing.');
}

createRoot(root).render(
  <StrictMode>
    <App client={createChaWebClient()} />
  </StrictMode>,
);
