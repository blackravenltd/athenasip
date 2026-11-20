// `vitest/config` rather than `vite`, so the `test` block below is typed.
import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

/**
 * AthenaSIP serves the console from the listener that answers `/api/v1`
 * (`http.files.path` in the server's config). The dev server proxies `/api`
 * so that development is same-origin too.
 */
export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': {
        target: process.env.ATHENASIP_API ?? 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },
  build: {
    outDir: 'build',
    emptyOutDir: true,
    rollupOptions: {
      // Two pages: the console, and the standalone softphone for the end-to-end run.
      input: {
        index: 'index.html',
        softphone: 'softphone.html',
      },
    },
  },
  test: {
    environment: 'node',
    setupFiles: './src/test/setup.ts',
    // The end-to-end specs are Playwright's (`npm run test:e2e`).
    exclude: ['node_modules/**', 'e2e/**'],
    // A file's first screen test pays for jsdom and its imports, up to 5 s on an idle machine
    // and more under load, as when the suite runs beside the fixture.
    testTimeout: 20_000,
  },
});
