import react from '@vitejs/plugin-react';
import { defineConfig } from 'vitest/config';

export default defineConfig({
  plugins: [react()],
  test: {
    environment: 'jsdom',
    include: ['src/**/*.test.{ts,tsx}'],
    setupFiles: './src/test/setup.ts',
    // Voice tests wait for the real 1-second spoken-send delay. A loaded
    // machine can pause a test worker for more than a second.
    testTimeout: 15_000,
  },
});
