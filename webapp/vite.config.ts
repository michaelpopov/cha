import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

// Vite serves only frontend assets/HMR. Domain calls use the native bridge.
// The exact allowed development origin is http://127.0.0.1:5173.
// The Windows host serves the packaged index.html itself and adds this tag
// while doing so, then answers the request with that document's connection
// identifier. The development server's HTML is not its to serve, so add the
// tag here instead. Serve only: the packaged HTML must stay as the hosts
// expect it. The macOS host injects its bootstrap as a WKUserScript and does
// not serve this path, so under macOS development mode the request 404s
// harmlessly; the bridge there does not depend on it.
const nativeBootstrap = {
  name: 'cha-native-bootstrap',
  apply: 'serve',
  transformIndexHtml: () => [{
    tag: 'script',
    attrs: { src: '/__cha-bootstrap.js' },
    injectTo: 'head' as const,
  }],
} as const;

export default defineConfig({
  base: '/',
  plugins: [react(), nativeBootstrap],
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
  },
});
