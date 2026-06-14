import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

// https://vite.dev/config/
export default defineConfig({
  plugins: [react()],
  server: {
    host: '0.0.0.0',
    proxy: {
      '/ws-mgmt': {
        target: 'ws://127.0.0.1:9001',
        ws: true,
        rewrite: (path) => path.replace(/^\/ws-mgmt/, '')
      },
      '/marketdata': {
        target: 'ws://127.0.0.1:9002',
        ws: true,
        rewrite: (path) => path.replace(/^\/marketdata/, '')
      },
      '/api': {
        target: 'http://127.0.0.1:8080',
        rewrite: (path) => path.replace(/^\/api/, '')
      }
    }
  }
})
