import type { ReactNode } from 'react'
import { VIEW_META, type View } from '../lib/navigation'

export function PageHeader({
  view,
  actions,
  title,
  description
}: {
  view: View
  actions?: ReactNode
  title?: string
  description?: string
}) {
  const meta = VIEW_META[view]
  return (
    <div className="page-header">
      <div className="page-header-copy">
        <div className="page-eyebrow">{meta.eyebrow}</div>
        <h1 className="page-title">{title ?? meta.title}</h1>
        <p className="page-desc">{description ?? meta.description}</p>
      </div>
      {actions && <div className="page-actions">{actions}</div>}
    </div>
  )
}
