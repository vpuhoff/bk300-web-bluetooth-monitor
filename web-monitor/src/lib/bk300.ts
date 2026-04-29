export type Bk300Status =
  | 'disconnected'
  | 'connecting'
  | 'connected'
  | 'reconnecting'

export type Bk300Frame = {
  prefix: '4040' | '2424'
  length: number
  typeHex: string // e.g. "4b0b"
  payload: Uint8Array
  crcOk: boolean
  raw: Uint8Array
}

export type Bk300VoltageSample = {
  atMs: number
  volts: number
  raw: number
}

const SERVICE_UUID = '0000fff0-0000-1000-8000-00805f9b34fb'
const NOTIFY_UUID = '0000fff1-0000-1000-8000-00805f9b34fb'
const WRITE_UUID = '0000fff2-0000-1000-8000-00805f9b34fb'

function u16le(bytes: Uint8Array, offset: number) {
  return bytes[offset] | (bytes[offset + 1] << 8)
}

// CRC-16/PPP (a.k.a. CRC-16/X25 with reflected poly 0x8408), init=0xFFFF, xorout=0xFFFF.
// This matches the app's FCS table implementation; verified by the fixed 0301 frame CRC = 0xD333 (bytes 33 D3).
export function crc16ppp(data: Uint8Array) {
  let fcs = 0xffff
  for (let i = 0; i < data.length; i++) {
    fcs ^= data[i]
    for (let b = 0; b < 8; b++) {
      if (fcs & 0x0001) fcs = (fcs >> 1) ^ 0x8408
      else fcs >>= 1
    }
  }
  return (fcs ^ 0xffff) & 0xffff
}

function hexToBytes(hex: string) {
  const clean = hex.replaceAll(/\s+/g, '').toLowerCase()
  if (clean.length % 2 !== 0) throw new Error('hex length must be even')
  const out = new Uint8Array(clean.length / 2)
  for (let i = 0; i < out.length; i++) {
    out[i] = Number.parseInt(clean.slice(i * 2, i * 2 + 2), 16)
  }
  return out
}

function bytesToHex(bytes: Uint8Array) {
  let s = ''
  for (const b of bytes) s += b.toString(16).padStart(2, '0')
  return s
}

export function buildCommandFrame(cmdHex: string, payloadHex = '') {
  if (cmdHex.length !== 4) throw new Error('cmdHex must be 4 hex chars, e.g. 0b0b')
  const cmd = hexToBytes(cmdHex)
  const payload = payloadHex ? hexToBytes(payloadHex) : new Uint8Array()

  if (cmdHex.toLowerCase() === '0301' && payload.length === 0) {
    // hard-coded by the Android app (04040 0A00 0301 33D3 0D0A)
    return hexToBytes('40400a00030133d30d0a')
  }

  const length = payload.length + 10
  const frame = new Uint8Array(length)
  frame[0] = 0x40
  frame[1] = 0x40
  frame[2] = length & 0xff
  frame[3] = (length >> 8) & 0xff
  frame[4] = cmd[0]
  frame[5] = cmd[1]
  frame.set(payload, 6)
  const crc = crc16ppp(frame.subarray(0, length - 4))
  frame[length - 4] = crc & 0xff
  frame[length - 3] = (crc >> 8) & 0xff
  frame[length - 2] = 0x0d
  frame[length - 1] = 0x0a
  return frame
}

export class Bk300Client {
  private device: BluetoothDevice | null = null
  private server: BluetoothRemoteGATTServer | null = null
  private notifyChar: BluetoothRemoteGATTCharacteristic | null = null
  private writeChar: BluetoothRemoteGATTCharacteristic | null = null
  private rx = new Uint8Array()
  private onDisconnected?: () => void
  private onNotifyBytes?: (bytes: Uint8Array) => void

  public async requestDevice() {
    const device = await navigator.bluetooth.requestDevice({
      filters: [{ name: 'BK300' }, { services: [SERVICE_UUID] }],
      optionalServices: [SERVICE_UUID],
    })
    this.device = device
    return device
  }

  public setHandlers(opts: {
    onDisconnected?: () => void
    onNotifyBytes?: (bytes: Uint8Array) => void
  }) {
    this.onDisconnected = opts.onDisconnected
    this.onNotifyBytes = opts.onNotifyBytes
  }

  public getDevice() {
    return this.device
  }

  public isConnected() {
    return !!this.server?.connected
  }

  public async connect() {
    if (!this.device) throw new Error('No device. Call requestDevice() first.')
    this.device.removeEventListener('gattserverdisconnected', this.handleDisconnected)
    this.device.addEventListener('gattserverdisconnected', this.handleDisconnected)

    this.server = await this.device.gatt!.connect()
    const service = await this.server.getPrimaryService(SERVICE_UUID)
    this.notifyChar = await service.getCharacteristic(NOTIFY_UUID)
    this.writeChar = await service.getCharacteristic(WRITE_UUID)

    await this.notifyChar.startNotifications()
    this.notifyChar.addEventListener('characteristicvaluechanged', this.handleNotify)
  }

  public async disconnect() {
    try {
      this.notifyChar?.removeEventListener('characteristicvaluechanged', this.handleNotify)
      if (this.notifyChar?.service?.device) {
        // best-effort
      }
    } finally {
      this.server?.disconnect()
    }
  }

  public async writeCommand(cmdHex: string, payloadHex = '') {
    if (!this.writeChar) throw new Error('Not connected')
    const frame = buildCommandFrame(cmdHex, payloadHex)
    await this.writeChar.writeValueWithoutResponse(frame)
    return frame
  }

  private handleDisconnected = () => {
    this.server = null
    this.notifyChar = null
    this.writeChar = null
    this.rx = new Uint8Array()
    this.onDisconnected?.()
  }

  private handleNotify = (ev: Event) => {
    const chr = ev.target as BluetoothRemoteGATTCharacteristic
    const dv = chr.value
    if (!dv) return
    const bytes = new Uint8Array(dv.buffer.slice(dv.byteOffset, dv.byteOffset + dv.byteLength))
    this.onNotifyBytes?.(bytes)
  }

  public pushNotifyBytes(bytes: Uint8Array) {
    // append
    const next = new Uint8Array(this.rx.length + bytes.length)
    next.set(this.rx, 0)
    next.set(bytes, this.rx.length)
    this.rx = next
  }

  public popFrames(): Bk300Frame[] {
    const frames: Bk300Frame[] = []
    while (true) {
      const idx = indexOfSubarray(this.rx, new Uint8Array([0x0d, 0x0a]))
      if (idx < 0) break
      const candidate = this.rx.subarray(0, idx + 2)

      // remove from rx now; if candidate is junk, we drop it like the Android app does
      this.rx = this.rx.subarray(idx + 2)

      if (candidate.length < 10) continue
      const p0 = candidate[0]
      const p1 = candidate[1]
      const prefix =
        p0 === 0x40 && p1 === 0x40 ? ('4040' as const) : p0 === 0x24 && p1 === 0x24 ? ('2424' as const) : null
      if (!prefix) continue

      const length = u16le(candidate, 2)
      if (length !== candidate.length) continue

      const typeHex = bytesToHex(candidate.subarray(4, 6))
      const payload = candidate.subarray(6, length - 4)
      const crcIn = u16le(candidate, length - 4)
      const crcCalc = crc16ppp(candidate.subarray(0, length - 4))
      frames.push({
        prefix,
        length,
        typeHex,
        payload,
        crcOk: crcIn === crcCalc,
        raw: candidate,
      })
    }
    return frames
  }
}

function indexOfSubarray(haystack: Uint8Array, needle: Uint8Array) {
  if (needle.length === 0) return 0
  outer: for (let i = 0; i <= haystack.length - needle.length; i++) {
    for (let j = 0; j < needle.length; j++) {
      if (haystack[i + j] !== needle[j]) continue outer
    }
    return i
  }
  return -1
}

export function parseVoltageFrom4b0b(frame: Bk300Frame): Bk300VoltageSample | null {
  if (frame.typeHex.toLowerCase() !== '4b0b') return null
  if (frame.payload.length < 2) return null
  const raw = u16le(frame.payload, 0)
  return { atMs: Date.now(), raw, volts: raw / 100 }
}

