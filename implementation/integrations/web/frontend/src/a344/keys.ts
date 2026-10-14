// Remote-control key routing for a hosted Broadcaster Application (spec 5.18.4; A/344 9.11 Keys APIs, 9.12 BAAppear).
// The host page owns every key, from the virtual remote and from the physical keyboard. A key the application has
// been granted through org.atsc.request.keys is delivered into its iframe as synthetic keydown/keyup events (the
// frame is same-origin); any other key is a receiver key and goes to the receiver actions (channel change, hide the
// app, ...). Keys typed while the iframe has focus are intercepted there with capture listeners so the rule holds
// wherever focus sits. Receiver-reserved keys (channel, volume, power) are never granted, as in the A/344 example.

export interface KeyDef {
  key: string       // KeyboardEvent.key delivered to the application
  code: string      // KeyboardEvent.code
  keyCode: number   // legacy keyCode, also reported by org.atsc.query.deviceInfo (apps still switch on it)
}

/// A/344 key names (request.keys strings and deviceInput names) with the DOM event each one becomes.
export const KEY_DEFS: Record<string, KeyDef> = {
  ArrowUp: { key: 'ArrowUp', code: 'ArrowUp', keyCode: 38 },
  ArrowDown: { key: 'ArrowDown', code: 'ArrowDown', keyCode: 40 },
  ArrowLeft: { key: 'ArrowLeft', code: 'ArrowLeft', keyCode: 37 },
  ArrowRight: { key: 'ArrowRight', code: 'ArrowRight', keyCode: 39 },
  Select: { key: 'Enter', code: 'Enter', keyCode: 13 },
  Back: { key: 'GoBack', code: 'GoBack', keyCode: 27 },          // W3C media-controller value; 27 is in every TV app's keyCode map
  BAAppear: { key: 'BAAppear', code: 'BAAppear', keyCode: 1001 },
  ColorF0Red: { key: 'ColorF0Red', code: 'ColorF0Red', keyCode: 403 },
  ColorF1Green: { key: 'ColorF1Green', code: 'ColorF1Green', keyCode: 404 },
  ColorF2Yellow: { key: 'ColorF2Yellow', code: 'ColorF2Yellow', keyCode: 405 },
  ColorF3Blue: { key: 'ColorF3Blue', code: 'ColorF3Blue', keyCode: 406 },
  ChannelUp: { key: 'ChannelUp', code: 'ChannelUp', keyCode: 427 },
  ChannelDown: { key: 'ChannelDown', code: 'ChannelDown', keyCode: 428 },
  AudioVolumeUp: { key: 'AudioVolumeUp', code: 'AudioVolumeUp', keyCode: 175 },
  AudioVolumeDown: { key: 'AudioVolumeDown', code: 'AudioVolumeDown', keyCode: 174 },
  AudioVolumeMute: { key: 'AudioVolumeMute', code: 'AudioVolumeMute', keyCode: 173 },
  Power: { key: 'Power', code: 'Power', keyCode: 0 },
}
for (let d = 0; d <= 9; d++) KEY_DEFS[String(d)] = { key: String(d), code: `Digit${d}`, keyCode: 48 + d }

/// Keys the receiver keeps for itself whatever the application asks (A/344 9.11.1 example).
export const RESERVED_KEYS = new Set(['ChannelUp', 'ChannelDown', 'AudioVolumeUp', 'AudioVolumeDown', 'AudioVolumeMute', 'Power'])

/// Request-key names that stand for several DOM keys.
const GROUPS: Record<string, string[]> = { Numeric: ['0', '1', '2', '3', '4', '5', '6', '7', '8', '9'] }
/// Names an application may use for the same key: granting one grants the other.
const ALIASES: Record<string, string> = { Enter: 'Select', Escape: 'Back', GoBack: 'Back', Backspace: 'Back', BrowserBack: 'Back' }

/// A/344 key name of a physical keyboard event on the host page or inside the frame, or null when it is not a
/// remote-control key (typing in a text field, shortcuts, ...).
export function keyNameFromEvent(e: KeyboardEvent): string | null {
  if (e.ctrlKey || e.metaKey || e.altKey) return null
  switch (e.key) {
    case 'ArrowUp': case 'ArrowDown': case 'ArrowLeft': case 'ArrowRight': return e.key
    case 'Enter': return 'Select'
    case 'Escape': case 'Backspace': case 'GoBack': case 'BrowserBack': return 'Back'
    case 'PageUp': case 'ChannelUp': return 'ChannelUp'
    case 'PageDown': case 'ChannelDown': return 'ChannelDown'
    case 'F2': case 'Insert': case 'BAAppear': return 'BAAppear'
    case 'ColorF0Red': case 'ColorF1Green': case 'ColorF2Yellow': case 'ColorF3Blue': return e.key
    case 'AudioVolumeUp': case 'AudioVolumeDown': case 'AudioVolumeMute': return e.key
    default:
      if (/^[0-9]$/.test(e.key)) return e.key
      return null
  }
}

function isTypingTarget(t: EventTarget | null): boolean {
  const el = t as HTMLElement | null
  if (!el || !el.tagName) return false
  const tag = el.tagName.toUpperCase()
  return tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || el.isContentEditable
}

export interface KeyRouterDeps {
  frameWindow: () => Window | null
  /// A key the application does not hold: the receiver acts on it (channel change, Back, ...). BAAppear always
  /// comes here first (launch or show the application) and is then delivered to the application as well.
  receiver: (name: string) => void
  log: (text: string) => void
}

const NAVIGATION = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Select']

export class KeyRouter {
  /// Granted A/344 names, as requested ("Numeric" stays a group, "Select" covers "Enter").
  readonly granted = new Set<string>()
  private hostListening = false
  private frameWin: Window | null = null
  private readonly onHostKey = (e: KeyboardEvent) => this.hostKey(e)
  private readonly onFrameKey = (e: KeyboardEvent) => this.frameKey(e)

  constructor(private readonly deps: KeyRouterDeps) {}

  /// org.atsc.request.keys: returns the accepted names (unknown names are ignored, reserved ones refused).
  request(keys: string[]): string[] {
    const accepted: string[] = []
    for (const raw of keys) {
      const name = ALIASES[raw] ?? raw
      if (RESERVED_KEYS.has(name)) continue
      if (!(name in KEY_DEFS) && !(name in GROUPS)) continue
      this.granted.add(name)
      accepted.push(raw)
    }
    if (accepted.length) this.deps.log(`keys granted: ${accepted.join(', ')}`)
    return accepted
  }

  /// org.atsc.relinquish.keys
  relinquish(keys: string[]): void {
    for (const raw of keys) this.granted.delete(ALIASES[raw] ?? raw)
    this.deps.log(`keys relinquished: ${keys.join(', ')}`)
  }

  reset(): void {
    this.granted.clear()
  }

  isGranted(name: string): boolean {
    if (this.granted.has(name)) return true
    for (const [group, members] of Object.entries(GROUPS)) if (this.granted.has(group) && members.includes(name)) return true
    return false
  }

  /// True when the application holds no key other than BAAppear (its launch screen, A/344 9.11).
  holdsOnlyBAAppear(): boolean {
    for (const g of this.granted) if (g !== 'BAAppear') return false
    return true
  }

  /// A key from the virtual remote, the host keyboard or the frame's own keyboard events (A/344 9.11, 9.12):
  /// BAAppear is the key dedicated to the application — the receiver shows it and it is delivered whether requested
  /// or not; a granted key goes to the application; while the application holds nothing beyond BAAppear, the
  /// navigation keys the receiver does not use are issued to it as BAAppear; everything else is a receiver key.
  press(name: string): void {
    if (name === 'BAAppear') {
      this.deps.receiver(name)
      if (this.frameWin) this.deliver(name)
      return
    }
    if (this.isGranted(name) && this.frameWin) {
      this.deliver(name)
      return
    }
    if (this.frameWin && NAVIGATION.includes(name) && this.holdsOnlyBAAppear()) {
      this.deps.log(`${name} issued as BAAppear (the application holds no keys)`)
      this.deps.receiver('BAAppear')
      this.deliver('BAAppear')
      return
    }
    this.deps.receiver(name)
  }

  /// Synthetic keydown + keyup inside the application (isTrusted is false, which its handlers do not check; keyCode
  /// and which are defined explicitly because the Sinclair navigation code switches on keyCode).
  deliver(name: string): void {
    const win = this.frameWin
    const def = KEY_DEFS[name]
    if (!win || !def) return
    const doc = win.document
    const target: EventTarget = doc.activeElement ?? doc.body ?? win
    for (const type of ['keydown', 'keyup'] as const) {
      const Ctor = (win as Window & { KeyboardEvent: typeof KeyboardEvent }).KeyboardEvent   // the frame's own class: its handlers may check instanceof
      const ev = new Ctor(type, { key: def.key, code: def.code, bubbles: true, cancelable: true, composed: true })
      Object.defineProperty(ev, 'keyCode', { get: () => def.keyCode })
      Object.defineProperty(ev, 'which', { get: () => def.keyCode })
      target.dispatchEvent(ev)
    }
    this.deps.log(`key → app: ${name}`)
  }

  /// Physical keyboard on the host page: remote keys are routed, everything else (typing) is left alone.
  attachHost(): void {
    if (this.hostListening) return
    window.addEventListener('keydown', this.onHostKey, true)
    this.hostListening = true
  }
  detachHost(): void {
    if (!this.hostListening) return
    window.removeEventListener('keydown', this.onHostKey, true)
    this.hostListening = false
  }
  /// Capture listeners inside the application's window (same origin): keys it was not granted never reach it.
  attachFrame(win: Window | null): void {
    this.detachFrame()
    this.frameWin = win
    if (!win) return
    try {
      win.addEventListener('keydown', this.onFrameKey, true)
      win.addEventListener('keyup', this.onFrameKey, true)
    } catch {
      // cross-origin (broadband entry page on another host): keys can only come from the virtual remote
    }
  }
  detachFrame(): void {
    const win = this.frameWin
    this.frameWin = null
    if (!win) return
    try {
      win.removeEventListener('keydown', this.onFrameKey, true)
      win.removeEventListener('keyup', this.onFrameKey, true)
    } catch {
      // window already gone
    }
  }

  private hostKey(e: KeyboardEvent) {
    if (e.repeat || isTypingTarget(e.target)) return
    if (this.frameWin && e.view === this.frameWin) return   // handled by the frame listener
    const name = keyNameFromEvent(e)
    if (!name) return
    e.preventDefault()
    e.stopPropagation()
    this.press(name)
  }
  private frameKey(e: KeyboardEvent) {
    if (!e.isTrusted) return   // our own synthetic delivery
    const name = keyNameFromEvent(e)
    if (!name) return
    if (this.isGranted(name)) return   // a trusted key the app holds passes through untouched
    e.preventDefault()
    e.stopImmediatePropagation()
    if (e.type === 'keydown' && !e.repeat) this.press(name)
  }
}
