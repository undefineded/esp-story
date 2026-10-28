import type { NfcBridge } from '@shared/types'
import { MockBridge } from './mock-bridge'

const params = new URLSearchParams(window.location.search)
const forceDemo = params.get('demo') === '1'
const native = window.nfcBridge
const useDemo = forceDemo || !native || native.isDemo

export const isDemo = useDemo
export const bridge: NfcBridge = useDemo ? (new MockBridge() as NfcBridge) : (native as NfcBridge)
export const mockBridge = useDemo ? (bridge as unknown as MockBridge) : null
