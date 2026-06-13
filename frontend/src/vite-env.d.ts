/// <reference types="vite/client" />

interface ImportMetaEnv {
  // Backend base URL (e.g. https://nabla-backend.up.railway.app). Empty/unset
  // means same-origin — the Vite dev proxy and the compose nginx both serve
  // /api on the page origin. Set this in the Vercel project for production.
  readonly VITE_API_BASE_URL?: string
}

interface ImportMeta {
  readonly env: ImportMetaEnv
}
