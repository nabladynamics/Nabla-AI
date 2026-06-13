# nabla frontend

React + TypeScript + Vite single-page app. The top (presentation) layer of the
3-layer architecture; it talks only to the backend over HTTP and never to the
solver core directly.

## Develop

```bash
npm install
npm run dev          # http://localhost:5173
```

`/api/*` is proxied to the backend on `http://localhost:8000` (see
`vite.config.ts`), so no env var is needed for local dev.

## Backend URL (`VITE_API_BASE_URL`)

Every REST call and the telemetry WebSocket derive from one config module
([`src/config.ts`](src/config.ts)). It reads the build-time env var
`VITE_API_BASE_URL` (see [`.env.example`](.env.example)); unset means
same-origin (dev proxy / compose nginx). On **Vercel**, set
`VITE_API_BASE_URL` in the project's Environment Variables to the deployed
Railway backend URL (e.g. `https://nabla-backend.up.railway.app`); the
WebSocket URL is derived from it automatically (`https` → `wss`).

## Routes

| Path    | View            |
| ------- | --------------- |
| `/pre`  | Pre-processing  |
| `/sim`  | Simulation      |
| `/post` | Post-processing |

`/` redirects to `/pre`.

## Quality gates

```bash
npm run lint         # ESLint (flat config)
npm run typecheck    # tsc project references
npm run build        # production build
```
