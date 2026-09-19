import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

// Vite serves only frontend assets/HMR. Domain calls use the native bridge.
// The exact allowed development origin is http://127.0.0.1:5173.
export default defineConfig({
  base: '/',
  plugins: [react()],
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
  },
});
