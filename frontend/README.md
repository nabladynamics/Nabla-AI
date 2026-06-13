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
`vite.config.ts`).

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
