// Browser decode capability per component (spec 5.18.2 "per-component codec support"): MSE acceptance via
// MediaSource.isTypeSupported, decode quality via navigator.mediaCapabilities.decodingInfo (type 'media-source'),
// Widevine CDM presence for protected components. Results are cached per mime + resolution + bitrate + framerate
// in a reactive map so templates can call capability(c) directly.
import { reactive } from 'vue'
import type { Component } from '../api/types'

export type Verdict =
  | 'probing'      // async probe in flight
  | 'ok'           // supported and smooth (or smoothness unknown)
  | 'slow'         // supported but decodingInfo says not smooth
  | 'unsupported'  // no decoder for this codec string
  | 'drm'          // protected: playback needs a CDM and a license
  | 'renderer'     // text tracks rendered in JavaScript (IMSC1 / WebVTT), no decoder involved
  | 'na'           // not a media track (NRT, application, signaling)

export interface Capability {
  verdict: Verdict
  mime: string
  detail: string              // tooltip: how the verdict was reached
  method: 'mediaCapabilities' | 'isTypeSupported' | 'none'
  smooth: boolean | null
  powerEfficient: boolean | null
  assumedProfile: boolean     // codecs string was a bare fourcc (MMT asset_type); a typical profile was substituted
}

const cache = reactive<Record<string, Capability>>({})
let widevine: 'unknown' | 'present' | 'absent' = 'unknown'
let widevineProbe: Promise<void> | null = null

/// Bare fourcc (MMT MP-table asset_type) -> a complete codecs string MSE will evaluate.
const genericProfiles: Record<string, string> = {
  hev1: 'hev1.1.6.L120.B0',
  hvc1: 'hvc1.1.6.L120.B0',
  avc1: 'avc1.640028',
  avc3: 'avc3.640028',
  av01: 'av01.0.08M.08',
  vvc1: 'vvc1.1.L93.CQA.O0+3',
  'ac-4': 'ac-4.02.01.01',
  mp4a: 'mp4a.40.2',
  'ec-3': 'ec-3',
  'ac-3': 'ac-3',
  mhm1: 'mhm1.0x0D',
  mhm2: 'mhm2.0x0D',
  Opus: 'Opus',
}

function fullCodecs(codecs: string): { codecs: string; assumed: boolean } {
  const c = codecs.trim()
  if (!c) return { codecs: '', assumed: false }
  if (c.includes('.') || c.includes(',')) return { codecs: c, assumed: false }
  return genericProfiles[c] ? { codecs: genericProfiles[c], assumed: true } : { codecs: c, assumed: false }
}

function framerate(fr: string | null): number {
  if (!fr) return 30
  const m = /^(\d+(?:\.\d+)?)(?:\/(\d+))?$/.exec(fr.trim())
  if (!m) return 30
  const n = Number(m[1])
  const d = m[2] ? Number(m[2]) : 1
  return d > 0 ? n / d : 30
}

function container(c: Component): 'video' | 'audio' | 'text' | null {
  if (c.kind === 'video') return 'video'
  if (c.kind === 'audio') return 'audio'
  if (c.kind === 'subtitles' || c.kind === 'text') return 'text'
  if (c.mimeType.startsWith('video/')) return 'video'
  if (c.mimeType.startsWith('audio/')) return 'audio'
  return null
}

function probeWidevine(): Promise<void> {
  if (widevineProbe) return widevineProbe
  widevineProbe = (async () => {
    try {
      if (!navigator.requestMediaKeySystemAccess) {
        widevine = 'absent'
        return
      }
      await navigator.requestMediaKeySystemAccess('com.widevine.alpha', [
        {
          initDataTypes: ['cenc'],
          videoCapabilities: [{ contentType: 'video/mp4; codecs="hvc1.1.6.L120.B0"' }, { contentType: 'video/mp4; codecs="avc1.640028"' }],
          audioCapabilities: [{ contentType: 'audio/mp4; codecs="mp4a.40.2"' }],
        },
      ])
      widevine = 'present'
    } catch {
      widevine = 'absent'
    }
  })()
  return widevineProbe
}

async function probe(key: string, c: Component, kind: 'video' | 'audio', mime: string, assumed: boolean) {
  const entry = cache[key]
  const typeOk = typeof MediaSource !== 'undefined' && MediaSource.isTypeSupported(mime)
  let method: Capability['method'] = 'isTypeSupported'
  let supported = typeOk
  let smooth: boolean | null = null
  let powerEfficient: boolean | null = null
  const mc = navigator.mediaCapabilities
  if (mc && typeof mc.decodingInfo === 'function') {
    try {
      const config: MediaDecodingConfiguration =
        kind === 'video'
          ? {
              type: 'media-source',
              video: { contentType: mime, width: c.width ?? 1920, height: c.height ?? 1080, bitrate: c.bandwidth ?? 5_000_000, framerate: framerate(c.frameRate) },
            }
          : { type: 'media-source', audio: { contentType: mime, channels: '2', samplerate: c.audioSamplingRate ?? 48000, bitrate: c.bandwidth ?? 128_000 } }
      const info = await mc.decodingInfo(config)
      method = 'mediaCapabilities'
      supported = info.supported
      smooth = info.smooth
      powerEfficient = info.powerEfficient
    } catch {
      // Firefox throws for some configurations; keep the isTypeSupported answer
    }
  }
  const parts = [mime]
  if (kind === 'video') parts.push(`${c.width ?? 1920}×${c.height ?? 1080} @ ${framerate(c.frameRate).toFixed(2)} fps, ${Math.round((c.bandwidth ?? 5_000_000) / 1000)} kbit/s`)
  parts.push(method === 'mediaCapabilities' ? `decodingInfo: supported=${supported} smooth=${smooth} powerEfficient=${powerEfficient}` : `isTypeSupported: ${typeOk}`)
  if (assumed) parts.push('profile assumed from the bare fourcc of the MMT asset type')
  let verdict: Verdict = supported ? (smooth === false ? 'slow' : 'ok') : 'unsupported'
  if (c.protected) {
    await probeWidevine()
    parts.push(`Widevine CDM ${widevine}`)
    verdict = 'drm'
  }
  cache[key] = { ...entry, verdict, detail: parts.join(' · '), method, smooth, powerEfficient }
}

/// Reactive capability for a component; probes on first use.
export function capability(c: Component): Capability {
  const kind = container(c)
  const { codecs, assumed } = fullCodecs(c.codecs)
  if (kind === 'text') {
    return { verdict: 'renderer', mime: c.codecs, detail: 'Caption track: rendered by the player (IMSC1 / WebVTT), no browser decoder involved', method: 'none', smooth: null, powerEfficient: null, assumedProfile: false }
  }
  if (!kind || !codecs) {
    return { verdict: 'na', mime: '', detail: c.codecs ? `No MSE container for codec "${c.codecs}"` : 'Not a media track', method: 'none', smooth: null, powerEfficient: null, assumedProfile: false }
  }
  const mime = `${kind}/mp4; codecs="${codecs}"`
  const key = [mime, c.width ?? '', c.height ?? '', c.frameRate ?? '', c.bandwidth ?? '', c.audioSamplingRate ?? '', c.protected ? 'drm' : ''].join('|')
  if (!cache[key]) {
    cache[key] = { verdict: 'probing', mime, detail: 'probing…', method: 'none', smooth: null, powerEfficient: null, assumedProfile: assumed }
    void probe(key, c, kind, mime, assumed)
  }
  return cache[key]
}

export function verdictLabel(cap: Capability): string {
  switch (cap.verdict) {
    case 'ok': return cap.powerEfficient ? 'playable · hw' : 'playable'
    case 'slow': return 'not smooth'
    case 'unsupported': return 'no decoder'
    case 'drm': return cap.detail.includes('CDM present') ? 'DRM · CDM' : 'DRM · no CDM'
    case 'renderer': return 'renderer'
    case 'probing': return '…'
    default: return '-'
  }
}

export function verdictClass(cap: Capability): string {
  switch (cap.verdict) {
    case 'ok': return 'ok'
    case 'slow': return 'warn'
    case 'unsupported': return 'bad'
    case 'drm': return 'warn'
    default: return ''
  }
}
