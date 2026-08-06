import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

const target = 'http://127.0.0.1:8080';

export default defineConfig({
  base: '/',
  plugins: [react()],
  server: {
    proxy: {
      '^/(api/|s/[^/]+/[^/]+/api/)': {
        target,
        changeOrigin: true,
        configure: (proxy) =>
          proxy.on('proxyReq', (request) => request.setHeader('origin', target)),
      },
    },
  },
});
