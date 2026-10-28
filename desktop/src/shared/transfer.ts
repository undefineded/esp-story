/** IEEE CRC32 and bounded, drained request windows for the serial file protocol. */
export function crc32(data: Uint8Array): number {
  let crc = 0xffffffff
  for (const byte of data) {
    crc ^= byte
    for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1))
  }
  return (~crc) >>> 0
}

export async function transferWindow<T>(jobs: (() => Promise<T>)[]): Promise<T[]> {
  // Drain every pending response before caller cancels/reuses the device session.
  const results = await Promise.allSettled(jobs.map((job) => Promise.resolve().then(job)))
  const error = results.find((r) => r.status === 'rejected')
  if (error?.status === 'rejected') throw error.reason
  return results.map((r) => (r as PromiseFulfilledResult<T>).value)
}
