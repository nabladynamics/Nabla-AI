// Single source of truth for the backend base URL — every REST call and the
// telemetry WebSocket derive from here, so there is no hardcoded localhost:8000
// anywhere else.
//
// VITE_API_BASE_URL is a build-time Vite env var:
//   - unset / empty  -> same-origin. Local dev goes through the Vite proxy and
//     the compose stack through nginx, both of which serve /api on the page
//     origin (keeps `scripts/dev.sh` dynamic ports + `docker compose up`
//     working with no env set).
//   - set (Vercel)   -> absolute backend origin, e.g.
//     https://nabla-backend.up.railway.app. The WebSocket URL is derived from
//     the same value, switching http->ws / https->wss.

const API_BASE = (import.meta.env.VITE_API_BASE_URL ?? '').replace(/\/+$/, '')

/** Absolute (or same-origin relative) URL for a backend REST path like
 *  `/api/runs`. */
export function apiPath(path: string): string {
  return `${API_BASE}${path}`
}

/** WebSocket URL for a backend path like `/api/runs/{id}/telemetry`, derived
 *  from the same base (http->ws, https->wss). Falls back to the page origin
 *  when no base is configured (dev proxy / nginx). */
export function wsPath(path: string): string {
  if (API_BASE) {
    return `${API_BASE.replace(/^http/, 'ws')}${path}`
  }
  const proto = window.location.protocol === 'https:' ? 'wss' : 'ws'
  return `${proto}://${window.location.host}${path}`
}
