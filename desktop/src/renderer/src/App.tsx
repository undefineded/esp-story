import { useEffect } from 'react'
import { Sidebar } from './components/Sidebar'
import { TopBar } from './components/TopBar'
import { Toast } from './components/Toast'
import { useAppStore } from './store/useAppStore'
import { viewFromHash } from './lib/navigation'
import { DashboardPage } from './pages/DashboardPage'
import { ReaderPage } from './pages/ReaderPage'
import { WriterPage } from './pages/WriterPage'
import { LibraryPage } from './pages/LibraryPage'
import { FileManagerPage } from './pages/FileManagerPage'
import { DevicePage } from './pages/DevicePage'
import { ExtensionsPage } from './pages/ExtensionsPage'
import { SettingsPage } from './pages/SettingsPage'

function Page({ view }: { view: ReturnType<typeof useAppStore.getState>['view'] }) {
  switch (view) {
    case 'reader':
      return <ReaderPage />
    case 'writer':
      return <WriterPage />
    case 'library':
      return <LibraryPage />
    case 'files':
      return <FileManagerPage />
    case 'device':
      return <DevicePage />
    case 'extensions':
      return <ExtensionsPage />
    case 'settings':
      return <SettingsPage />
    default:
      return <DashboardPage />
  }
}

export default function App() {
  const view = useAppStore((s) => s.view)
  const setView = useAppStore((s) => s.setView)
  const init = useAppStore((s) => s.init)

  useEffect(() => {
    void init()
    const onHash = (): void => setView(viewFromHash())
    window.addEventListener('hashchange', onHash)
    return () => window.removeEventListener('hashchange', onHash)
  }, [init, setView])

  useEffect(() => {
    window.scrollTo({ top: 0, behavior: 'auto' })
  }, [view])

  return (
    <div className="app-shell">
      <Sidebar />
      <div className="app-main">
        <TopBar />
        <main className="app-content" key={view}>
          <Page view={view} />
        </main>
      </div>
      <Toast />
    </div>
  )
}
