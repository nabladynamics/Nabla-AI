import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// https://vite.dev/config/
// BACKEND_PORT lets dev.sh relocate the backend when :8000 is already taken
// (the proxy must follow it, or every /api call 404s).
const backendPort = Number(process.env.BACKEND_PORT ?? 8000)

export default defineConfig({
  plugins: [react()],
  server: {
    port: 5173,
    // Proxy API calls to the orchestration backend during development.
    // The backend serves run endpoints natively under /api (no rewrite),
    // including the telemetry WebSocket.
    proxy: {
      '/api': {
        target: `http://localhost:${backendPort}`,
        changeOrigin: true,
        ws: true,
      },
    },
  },
})
