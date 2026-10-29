import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.{ts,tsx}'],
    setupFiles: './src/test/setup.ts',
    // Some tests, such as the large EPUB import, take several seconds. A
    // loaded machine can pause a test worker for more than a second.
    testTimeout: 15_000,
  },
});
