import type { ButtonHTMLAttributes, ReactNode } from 'react'
import { useEffect, useState } from 'react'
import { Check, Copy, Loader2 } from 'lucide-react'

type ButtonVariant = 'primary' | 'secondary' | 'soft' | 'ghost' | 'dark' | 'danger'

export function Button({
  variant = 'secondary',
  size = 'md',
  loading = false,
  icon,
  children,
  className = '',
  disabled,
  ...props
}: ButtonHTMLAttributes<HTMLButtonElement> & {
  variant?: ButtonVariant
  size?: 'sm' | 'md' | 'lg'
  loading?: boolean
  icon?: ReactNode
}) {
  const classes = [
    'btn',
    `btn-${variant}`,
    size === 'sm' ? 'btn-sm' : size === 'lg' ? 'btn-lg' : '',
    className
  ]
    .filter(Boolean)
    .join(' ')
  return (
    <button className={classes} disabled={disabled || loading} {...props}>
      {loading ? <Loader2 size={15} className="spin" /> : icon}
      {children}
    </button>
  )
}

export function IconButton({
  label,
  children,
  className = '',
  ...props
}: ButtonHTMLAttributes<HTMLButtonElement> & { label: string }) {
  return (
    <button className={`icon-btn ${className}`} title={label} aria-label={label} {...props}>
      {children}
    </button>
  )
}

export function Pill({
  tone = 'muted',
  dot = false,
  children,
  className = ''
}: {
  tone?: 'aqua' | 'coral' | 'amber' | 'violet' | 'blue' | 'red' | 'muted'
  dot?: boolean
  children: ReactNode
  className?: string
}) {
  return (
    <span className={`pill pill-${tone} ${className}`.trim()}>
      {dot && <span className="dot" />}
      {children}
    </span>
  )
}

export function Toggle({
  checked,
  onChange,
  title,
  sub,
  disabled,
  ariaLabel
}: {
  checked: boolean
  onChange: (next: boolean) => void
  title: string
  sub?: string
  disabled?: boolean
  ariaLabel?: string
}) {
  return (
    <label className="toggle" style={disabled ? { opacity: 0.5, cursor: 'not-allowed' } : undefined}>
      <input
        type="checkbox"
        checked={checked}
        onChange={(event) => !disabled && onChange(event.target.checked)}
        disabled={disabled}
        aria-label={ariaLabel ?? title}
      />
      <span className="toggle-track" />
      <span className="toggle-copy">
        <span className="toggle-title">{title}</span>
        {sub && <span className="toggle-sub">{sub}</span>}
      </span>
    </label>
  )
}

export function Segmented<T extends string>({
  value,
  options,
  onChange,
  className = ''
}: {
  value: T
  options: Array<{ value: T; label: string; icon?: ReactNode }>
  onChange: (next: T) => void
  className?: string
}) {
  return (
    <div className={`segmented ${className}`.trim()} role="tablist">
      {options.map((option) => (
        <button
          key={option.value}
          type="button"
          role="tab"
          aria-selected={value === option.value}
          className={value === option.value ? 'active' : ''}
          onClick={() => onChange(option.value)}
        >
          {option.icon}
          {option.label}
        </button>
      ))}
    </div>
  )
}

export function CopyButton({
  value,
  label = '复制',
  className = ''
}: {
  value: string
  label?: string
  className?: string
}) {
  const [copied, setCopied] = useState(false)
  useEffect(() => {
    if (!copied) return
    const timer = setTimeout(() => setCopied(false), 1600)
    return () => clearTimeout(timer)
  }, [copied])

  async function copy(): Promise<void> {
    try {
      await navigator.clipboard.writeText(value)
      setCopied(true)
    } catch {
      /* 剪贴板不可用时静默失败 */
    }
  }

  return (
    <button
      type="button"
      className={`copy-btn ${copied ? 'copied' : ''} ${className}`.trim()}
      onClick={() => void copy()}
      title={label}
    >
      {copied ? <Check size={11} /> : <Copy size={11} />}
      {copied ? '已复制' : label}
    </button>
  )
}

export function EmptyState({
  icon,
  title,
  description,
  actions
}: {
  icon: ReactNode
  title: string
  description?: string
  actions?: ReactNode
}) {
  return (
    <div className="empty-state">
      <div className="empty-icon">{icon}</div>
      <div className="empty-title">{title}</div>
      {description && <div className="empty-desc">{description}</div>}
      {actions && <div className="empty-actions">{actions}</div>}
    </div>
  )
}

export function SectionHeading({
  title,
  description,
  actions
}: {
  title: string
  description?: string
  actions?: ReactNode
}) {
  return (
    <div className="section-heading">
      <div>
        <h2>{title}</h2>
        {description && <p style={{ marginTop: 3 }}>{description}</p>}
      </div>
      <div className="spacer" />
      {actions}
    </div>
  )
}

export function InfoBanner({
  tone = 'info',
  icon,
  children
}: {
  tone?: 'info' | 'warn' | 'error'
  icon?: ReactNode
  children: ReactNode
}) {
  return (
    <div className={`info-banner ${tone === 'info' ? '' : tone}`.trim()}>
      {icon && <span style={{ display: 'grid', placeItems: 'center', flex: '0 0 auto' }}>{icon}</span>}
      <div>{children}</div>
    </div>
  )
}
