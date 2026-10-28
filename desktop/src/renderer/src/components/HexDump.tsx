import { useMemo } from 'react'

function parse(hex: string): number[] {
  const clean = hex.replace(/[^0-9a-fA-F]/g, '')
  const out: number[] = []
  for (let i = 0; i + 1 < clean.length; i += 2) {
    out.push(parseInt(clean.slice(i, i + 2), 16))
  }
  return out
}

export function HexDump({
  hex,
  start = 0,
  unit = 'page',
  bytesPerRow = 16
}: {
  hex: string
  start?: number
  unit?: 'page' | 'block' | 'byte'
  bytesPerRow?: number
}) {
  const bytes = useMemo(() => parse(hex), [hex])
  const rows = useMemo(() => {
    const result: Array<{ offset: number; values: number[] }> = []
    for (let i = 0; i < bytes.length; i += bytesPerRow) {
      const offset =
        unit === 'page'
          ? start + Math.floor(i / 4)
          : unit === 'block'
            ? start + Math.floor(i / 16)
            : start + i
      result.push({ offset, values: bytes.slice(i, i + bytesPerRow) })
    }
    return result
  }, [bytes, start, bytesPerRow])

  if (bytes.length === 0) {
    return <div className="empty-mini">没有读取到数据</div>
  }

  function offsetLabel(index: number): string {
    if (unit === 'page') return `P${String(index).padStart(2, '0')}`
    if (unit === 'block') return `B${String(index).padStart(3, '0')}`
    return `0x${index.toString(16).toUpperCase().padStart(4, '0')}`
  }

  return (
    <div className="hex-scroll">
      <table className="hex-table">
        <thead>
          <tr>
            <th style={{ width: 62 }}>{unit === 'block' ? 'Block' : unit === 'page' ? 'Page' : 'Offset'}</th>
            <th>Bytes 0 – {bytesPerRow - 1}</th>
            <th style={{ width: 150 }}>ASCII</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((row) => {
            const ascii = row.values
              .map((b) => (b >= 32 && b < 127 ? String.fromCharCode(b) : '.'))
              .join('')
            return (
              <tr key={row.offset}>
                <td className="hex-offset">{offsetLabel(row.offset)}</td>
                <td className="hex-bytes">
                  {row.values.map((b) => b.toString(16).padStart(2, '0').toUpperCase()).join(' ')}
                </td>
                <td className="hex-ascii">{ascii}</td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}
