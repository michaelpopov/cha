import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

// HTTP development still proxies API calls to chaweb. Native development
// (`CHA_NATIVE_DEV=1`) serves only frontend assets/HMR; domain calls use the
// host bridge. The exact allowed origin is http://127.0.0.1:5173.
const nativeDev = process.env.CHA_NATIVE_DEV === '1';
const target = process.env.CHA_API_TARGET ?? 'http://127.0.0.1:8888';

export default defineConfig({
  base: '/',
  plugins: [react()],
  server: nativeDev
    ? {
      host: '127.0.0.1',
      port: 5173,
      strictPort: true,
    }
    : {
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
