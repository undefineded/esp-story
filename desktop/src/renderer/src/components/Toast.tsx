import { AlertTriangle, CheckCircle2, Info, X } from 'lucide-react'
import { useAppStore } from '../store/useAppStore'

export function Toast() {
  const toast = useAppStore((s) => s.toast)
  const dismiss = useAppStore((s) => s.dismissToast)
  if (!toast) return null

  const Icon = toast.tone === 'success' ? CheckCircle2 : toast.tone === 'error' ? AlertTriangle : Info

  return (
    <div className="toast-wrap" key={toast.id}>
      <div className={`toast ${toast.tone}`} role="status">
        <span className="toast-icon">
          <Icon size={17} />
        </span>
        <div className="toast-copy">
          <div className="toast-title">{toast.title}</div>
          {toast.message && <div className="toast-msg">{toast.message}</div>}
        </div>
        <button type="button" className="toast-close" onClick={dismiss} aria-label="关闭提示">
          <X size={13} />
        </button>
      </div>
    </div>
  )
}
