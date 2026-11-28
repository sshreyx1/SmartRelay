import react from '@vitejs/plugin-react'
import { defineConfig } from 'vite'

// The backend (uvicorn) listens on IPv4. On newer Node, "localhost" can resolve to IPv6 ::1 first,
// which made the WebSocket upgrade fail or flap and the page only updated after F5. Use 127.0.0.1.
const BACKEND = 'http://127.0.0.1:8000'

export default defineConfig({
  plugins: [react()],
  server: {
    proxy: {
      '/api': BACKEND,
      '/captures': BACKEND,
      '/ws': { target: 'ws://127.0.0.1:8000', ws: true },
    },
  },
})
