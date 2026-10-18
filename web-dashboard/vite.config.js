import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import { readFileSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const dashboardDir = dirname(fileURLToPath(import.meta.url));
const projectDir = resolve(dashboardDir, '..');

function readBackendApiKey() {
  try {
    const envFile = readFileSync(resolve(projectDir, 'backend/.env'), 'utf8');
    const line = envFile.split(/\r?\n/).find((entry) =>
      /^\s*IOT_API_KEY\s*=/.test(entry)
    );
    if (!line) return '';

    let value = line.slice(line.indexOf('=') + 1).trim();
    if (
      value.length >= 2 &&
      ((value.startsWith('"') && value.endsWith('"')) ||
        (value.startsWith("'") && value.endsWith("'")))
    ) {
      value = value.slice(1, -1);
    }
    return value;
  } catch {
    return '';
  }
}

const apiKey = readBackendApiKey();

export default defineConfig({
  plugins: [react()],
  server: {
    host: '127.0.0.1',
    port: 5173,
    strictPort: true,
    proxy: {
      '/api/v1': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: true,
        configure(proxy) {
          proxy.on('proxyReq', (proxyReq) => {
            if (apiKey) {
              proxyReq.setHeader('X-Api-Key', apiKey);
            }
          });
        }
      }
    }
  },
  build: {
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (id.includes('/node_modules/recharts/')) return 'charts';
          if (
            id.includes('/node_modules/react/') ||
            id.includes('/node_modules/react-dom/') ||
            id.includes('/node_modules/scheduler/')
          ) return 'react-vendor';
        }
      }
    }
  },
  preview: {
    host: '127.0.0.1',
    port: 4173,
    strictPort: true
  }
});
