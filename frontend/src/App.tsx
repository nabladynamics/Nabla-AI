import { Outlet } from 'react-router-dom'

import { TopBar } from './components/TopBar'

export function App() {
  return (
    <div className="app">
      <TopBar />
      <main className="app-main">
        <Outlet />
      </main>
    </div>
  )
}
