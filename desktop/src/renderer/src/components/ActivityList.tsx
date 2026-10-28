import { ArrowDownToLine, Cable, CircleAlert, Nfc, Sparkles } from 'lucide-react'
import type { ActivityEntry } from '@shared/types'
import { relativeTime } from '../lib/card'

function IconFor({ kind }: { kind: ActivityEntry['kind'] }) {
  switch (kind) {
    case 'card':
      return <Nfc size={15} />
    case 'write':
      return <ArrowDownToLine size={15} />
    case 'connect':
      return <Cable size={15} />
    case 'error':
      return <CircleAlert size={15} />
    default:
      return <Sparkles size={15} />
  }
}

export function ActivityList({ activities, limit = 6 }: { activities: ActivityEntry[]; limit?: number }) {
  if (activities.length === 0) {
    return <div className="empty-mini">还没有操作记录，扫描一张卡片开始。</div>
  }
  return (
    <div className="activity-list">
      {activities.slice(0, limit).map((activity) => (
        <div className="activity-row" key={activity.id}>
          <span className={`activity-icon ${activity.kind}`}>
            <IconFor kind={activity.kind} />
          </span>
          <div className="activity-copy">
            <div className="activity-title">{activity.title}</div>
            {activity.detail && <div className="activity-detail">{activity.detail}</div>}
          </div>
          <span className="activity-time">{relativeTime(activity.ts)}</span>
        </div>
      ))}
    </div>
  )
}
