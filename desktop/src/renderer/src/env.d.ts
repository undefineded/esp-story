import type { NfcBridge } from '@shared/types'

declare global {
  interface Window {
    nfcBridge?: NfcBridge
  }
}

export {}
