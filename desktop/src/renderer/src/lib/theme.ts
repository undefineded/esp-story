let audioContext: AudioContext | null = null

/** 卡片靠近/写入成功时的轻提示音; 浏览器要求首次用户交互后才能播放 */
export function playChime(kind: 'card' | 'success' | 'error' = 'card'): void {
  try {
    const activation = navigator.userActivation
    if (activation && !activation.hasBeenActive) return
    audioContext ??= new AudioContext()
    if (audioContext.state === 'suspended') void audioContext.resume()
    const now = audioContext.currentTime
    const notes = kind === 'card' ? [880, 1320] : kind === 'success' ? [660, 880, 1320] : [220, 180]
    notes.forEach((freq, index) => {
      const osc = audioContext!.createOscillator()
      const gain = audioContext!.createGain()
      osc.type = 'sine'
      osc.frequency.value = freq
      gain.gain.setValueAtTime(0.0001, now + index * 0.09)
      gain.gain.exponentialRampToValueAtTime(0.06, now + index * 0.09 + 0.02)
      gain.gain.exponentialRampToValueAtTime(0.0001, now + index * 0.09 + 0.18)
      osc.connect(gain)
      gain.connect(audioContext!.destination)
      osc.start(now + index * 0.09)
      osc.stop(now + index * 0.09 + 0.2)
    })
  } catch {
    /* 音频不是关键路径 */
  }
}

export function resolveTheme(theme: 'light' | 'dark' | 'system'): 'light' | 'dark' {
  if (theme === 'light' || theme === 'dark') return theme
  return window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'
}

export function applyTheme(theme: 'light' | 'dark' | 'system'): 'light' | 'dark' {
  const resolved = resolveTheme(theme)
  document.documentElement.dataset.theme = resolved
  document.documentElement.style.colorScheme = resolved
  return resolved
}
