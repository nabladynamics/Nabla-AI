import { createBrowserRouter, Navigate } from 'react-router-dom'

import { App } from './App'
import { Post } from './routes/Post'
import { Pre } from './routes/Pre'
import { Sim } from './routes/Sim'

// The three top-level stages of a CFD workflow: pre-process, simulate, post-process.
export const router = createBrowserRouter([
  {
    path: '/',
    element: <App />,
    children: [
      { index: true, element: <Navigate to="/pre" replace /> },
      { path: 'pre', element: <Pre /> },
      { path: 'sim', element: <Sim /> },
      { path: 'post', element: <Post /> },
    ],
  },
])
