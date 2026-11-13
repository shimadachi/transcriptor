import react from '@vitejs/plugin-react';
import {defineConfig} from 'vite';

// The page is loaded from the app's own files (app://transcriptor/, served by
// electron/main.js out of dist/), so every asset is addressed relative to
// index.html and nothing is fetched from anywhere else.
export default defineConfig({
  base: './',
  plugins: [react()],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    // Electron pins its own Chromium, so there is one browser to build for.
    target: 'esnext',
  },
});
