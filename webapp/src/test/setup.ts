import '@testing-library/jest-dom/vitest';
import { cleanup } from '@testing-library/react';
import { afterEach } from 'vitest';

import { rememberAppUrl } from '../state/route';

// jsdom has no Range layout. ProseMirror measures the selection when it
// scrolls to it, which happens once the editor has focus.
if (typeof Range.prototype.getClientRects !== 'function') {
  Range.prototype.getClientRects = () => [] as unknown as DOMRectList;
  Range.prototype.getBoundingClientRect = () => new DOMRect();
}

afterEach(() => {
  cleanup();
  window.sessionStorage.clear();
  window.history.replaceState(null, '', '/');
  rememberAppUrl();
});
