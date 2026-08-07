import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

// The browser suite starts its own chaweb on a port it picks, and points this
// at it. A developer running `npm run dev` by hand gets the port that
// `make run-web-dev` uses; see README.md.
const target = process.env.CHA_API_TARGET ?? 'http://127.0.0.1:8888';

export default defineConfig({
  base: '/',
  plugins: [react()],
  server: {
    proxy: {
      '^/(api/|s/[^/]+/[^/]+/api/)': {
        target,
        changeOrigin: true,
        configure: (proxy) => {
          proxy.on('proxyReq', (request) => request.setHeader('origin', target));
          // Vite is only a development hop. When a tab or browser context
          // closes an EventSource, promptly tear down its upstream response as
          // well so chaweb releases the session's one stream slot.
          proxy.on('proxyRes', (upstream, _request, downstream) => {
            downstream.on('close', () => upstream.destroy());
          });
        },
      },
    },
  },
});
