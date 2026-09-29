import react from '@vitejs/plugin-react';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { defineConfig } from 'vite';

const directory = path.dirname(fileURLToPath(import.meta.url));

// Browser build only. The native config keeps its own bootstrap plugin.
export default defineConfig({
  root: path.resolve(directory, 'src/chaweb'),
  base: '/',
  plugins: [react()],
  build: {
    outDir: path.resolve(directory, 'dist-chaweb'),
    emptyOutDir: true,
  },
  server: {
    host: '127.0.0.1',
    port: 5174,
    strictPort: true,
    proxy: {
      '/api/cha/v1': {
        target: 'http://127.0.0.1:8087',
      },
    },
  },
});
