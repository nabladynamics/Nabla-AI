import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import { RouterProvider } from 'react-router-dom'

import { RunProvider } from './state/RunContext'
import { router } from './router'
import './index.css'

const rootElement = document.getElementById('root')
if (!rootElement) {
  throw new Error('Root element #root not found')
}

createRoot(rootElement).render(
  <StrictMode>
    <RunProvider>
      <RouterProvider router={router} />
    </RunProvider>
  </StrictMode>,
)
