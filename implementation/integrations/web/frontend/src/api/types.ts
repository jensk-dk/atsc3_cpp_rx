// Mirrors integrations/web/server.cpp JSON (spec 5.17.2). Field names are camelCase versions of the C++ types.
export interface Component {
  kind: string
  transport: 'ROUTE' | 'MMTP' | string
  tsi: number | null
  packetId: number | null
  id: string
  codecs: string
  mimeType: string
  lang: string
  name: string
  roles: string[]
  bandwidth: number | null
  width: number | null
  height: number | null
  frameRate: string | null
  audioSamplingRate: number | null
  timescale: number | null
  protected: boolean
  defaultAsset: boolean
  realTime: boolean
  session: string
  objects: number
  bytes: number
  packets: number
  errors: number
  lastToi: number | null
  lastMpuSequence: number | null
  lastName: string | null
}

export interface SlsFragmentInfo {
  kind: string
  contentType: string
  location: string
  version: number | null
  bytes: number
}

/// HELD HTMLEntryPackage of a service with the state of its Application Context Cache (spec 5.18.4).
export interface AppInfo {
  appContextId: string
  appId: string
  bcastEntryPackageUrl: string | null
  bcastEntryPageUrl: string | null
  bbandEntryPageUrl: string | null
  requiredCapabilities: string | null
  appRendering: boolean
  coupledServices: number[]
  lctTsiRef: number[]
  validFromNs: number | null
  validUntilNs: number | null
  ready: boolean          // the broadcast entry page is cached
  files: number
  bytes: number
  packageTsi: number | null
  packageBytes: number | null      // EFDT Content-Length of the entry package
  packageReceived: number | null   // bytes of the copy being assembled (null when nothing is pending)
}

export interface Service {
  llsGroup: number
  bsid: number
  serviceId: number
  globalServiceId: string | null
  major: number | null
  minor: number | null
  shortName: string
  usbdName: string
  category: number
  categoryName: string
  protocol: string
  protected: boolean
  hidden: boolean
  hideInGuide: boolean
  broadbandRequired: boolean
  essential: boolean
  drmSystemId: string | null
  slsAddress: string
  slsReceived: boolean
  slsUpdates: number
  slsToi: number
  selected: boolean
  languages: string[]
  mmtPackageId: string
  objects: number
  objectErrors: number
  bytes: number
  mmtpPackets: number
  mpd: {
    profiles: string
    type: string
    minBufferTime: number | null
    maxSegmentDuration: number | null
    timeShiftBufferDepth: number | null
    publishTimeNs: number | null
    availabilityStartNs: number | null
  }
  components: Component[]
  slsFragments: SlsFragmentInfo[]
  apps: AppInfo[]
  warnings: string[]
}

export interface ServiceDetail {
  service: Service
  fragments: { name: string; text: string }[]
  recentObjects: string[]
}

export interface Status {
  version: number
  esgVersion: number
  captureUnixMs: number | null
  stackVersion: string
  source: {
    open: boolean
    path: string
    name: string
    error: string
    format: string
    detection: string
    pacing: 'realtime' | 'nonrealtime'
    speed: number
    loop: boolean
    paused: boolean
    finished: boolean
    positionNs: number
    openedAtUnixMs: number
  }
  llsGroups: number[]
  bsids: number[]
  services: number
  selected: number
  stats: Record<string, Record<string, number>>
}

export interface Capture {
  path: string
  name: string
  dir: string
  size: number
  mtime: number
  format: string
  detection: string
  sidecar: boolean
}

export interface SourceList {
  directories: string[]
  captures: Capture[]
}

export interface LlsTable {
  group: number
  tableId: number
  name: string
  version: number
  sourceIp: string
  rootNamespace: string
  bytes: number
  timeNs: number
  xml: string
}

export interface LinkInfo {
  lmts: { carrierPlp: number; version: number; plps: { plpId: number; multicasts: { src: string; dst: string; srcPort: number; dstPort: number; sid: number | null; contextId: number | null }[] }[] }[]
  plpMap: { plpId: number; dst: string; dstPort: number; src: string; origin: string }[]
  l1: { lmtOrigin: string; plps: { plpId: number; llsFlag: boolean; modulation: string; codeRate: string; fecCodeLength: string; fecOuterCode: string; layer: string }[] } | null
  unrouted: { dst: string; dstPort: number; src: string; packets: number; bytes: number; plp: number }[]
}

// ---- ESG / EPG (spec 5.17.5, integrations/web/server.cpp esgRoutes) ----
export interface TimeInfo {
  wallUnixMs: number
  captureUnixMs: number | null
  positionNs: number
  open: boolean
  paused: boolean
  finished: boolean
}

export interface EsgIcon {
  uri: string
  url: string
  mime: string
  width: number | null
  height: number | null
  dataSize: number | null
}

export interface EsgRating {
  description: string
  lang: string
  scheme: string
  region: number | null
  dimensions: { dimension: number; value: string }[]
}

export interface EsgService {
  id: string
  version: number
  globalServiceId: string | null
  name: string
  description: string
  serviceType: number | null
  major: number | null
  minor: number | null
  emergency: boolean
  validFrom: number | null
  validTo: number | null
  icon: string
  icons: EsgIcon[]
  genres: string[]
  audioLanguages: string[]
  textLanguages: string[]
  ratings: EsgRating[]
  bsid: number | null        // SLT service this guide entry resolves to (globalServiceID, else major.minor)
  serviceId: number | null
  shortName: string
  selected: boolean
  programmes: number
}

export interface Programme {
  serviceId: string          // ESG Service fragment id
  contentId: string
  scheduleId: string
  start: number              // Unix seconds
  end: number
  bsid: number | null
  sltServiceId: number | null
  title: string
  description: string
  lengthS: number | null
  genres: string[]
  icon: string
  rating: string
}

export interface EsgContent {
  id: string
  version: number
  validFrom: number | null
  validTo: number | null
  title: string
  description: string
  names: { lang: string; text: string }[]
  descriptions: { lang: string; text: string }[]
  lengthS: number | null
  serviceIds: string[]
  genres: string[]
  audioLanguages: string[]
  textLanguages: string[]
  icon: string
  icons: EsgIcon[]
  ratings: EsgRating[]
  components: { kind: string; language: string; text: string }[]
  capabilities: string[]
  xml: string
}

export interface EsgSummary {
  version: number
  stats: Record<string, number>
  spanStart: number | null
  spanEnd: number | null
  services: EsgService[]
  esgServices: { bsid: number; serviceId: number; shortName: string; slsReceived: boolean; objects: number; bytes: number }[]
  descriptors: {
    id: string
    version: number | null
    location: string
    bsid: number
    serviceId: number
    tsi: number
    toi: number
    receivedNs: number
    entries: {
      start: number | null
      end: number | null
      tsi: number | null
      ip: string | null
      port: number | null
      units: { toi: number | null; contentLocation: string; fragments: number; service: number; schedule: number; content: number; other: number }[]
    }[]
  }[]
  files: { name: string; url: string; contentType: string; bytes: number; tsi: number; toi: number; receivedNs: number }[]
}

export interface EsgSchedule {
  from: number
  to: number
  programmes: Programme[]
}

// ---- media origin (spec 5.17.3, integrations/web/server.cpp mediaRoutes) ----
export interface MediaRepresentation {
  id: string
  kind: string
  codecs: string
  mimeType: string
  lang: string
  bandwidth: number | null
  width: number | null
  height: number | null
  timescale: number | null
  segmentDuration: number | null
  startNumber: number
  addressing: 'number' | 'time' | 'none'
  initName: string
  mediaTemplate: string
  protected: boolean
  init: boolean
  cachedSegments: number
  firstKey: number | null
  lastKey: number | null
}
export interface MediaSegment {
  seq: number
  name: string
  repId: string
  init: boolean
  key: number
  bytes: number
  tsi: number
  toi: number
  receivedNs: number
  contentType: string
  url: string
}
export interface MediaIndex {
  bsid: number
  serviceId: number
  selected: boolean
  nextSeq: number
  mpdType: string
  maxSegmentDuration: number | null
  timeShiftBufferDepth: number | null
  cachedBytes: number
  cacheLimit: number
  evicted: number
  availabilityStartMs: number | null
  periodStartS: number
  liveEdgeMs: number | null      // availability of the newest cached segment: the player's "now"
  captureMs: number
  representations: MediaRepresentation[]
  segments: MediaSegment[]
}
export interface PlayerSettings {
  mpdUrl: string
  timeUrl: string
  liveDelay: number
  liveCatchup: boolean
  autoSwitchBitrate: boolean
  protected: boolean
  selected: boolean
  minSegments: number            // segments per A/V representation to hold before starting dash.js
}

// ---- MMT push origin (spec 5.17.4, integrations/web/server.cpp mmtRoutes) ----
// player.json carries `mode`: "dash" (ROUTE, the fields above) or "push" (MMTP, polled index + plain fragment GETs).
export interface PushPlayerSettings {
  mode: 'push'
  indexUrl: string               // /media/{bsid}/{sid}/mmt/index.json (polled with ?since=<seq>)
  liveDelay: number              // seconds behind liveEdgeS the player keeps
  minFragments: number           // fragments per chosen component to hold before play()
  protected: boolean
  selected: boolean
}
export type AnyPlayerSettings = (PlayerSettings & { mode?: 'dash' }) | PushPlayerSettings

export interface MmtInit {
  seq: number
  bytes: number
  url: string
}
export interface MmtComponent {
  packetId: number
  assetId: string
  kind: 'video' | 'audio' | 'subtitles' | 'data' | string
  sampleEntry: string
  codecs: string                 // RFC 6381 string derived from the init (hvcC / esds / dac4), never the MP-table asset_type
  mime: string                   // video/mp4, audio/mp4, application/mp4
  timescale: number
  lang: string
  width: number | null
  height: number | null
  channels: number | null
  sampleRate: number | null
  defaultAsset: boolean
  init: MmtInit | null           // null until the MPU metadata (FT=0) of the asset was seen
  timestampOffsetS: number | null   // SourceBuffer.timestampOffset, constant per component; null until the first fragment fixed it
  mode: 'keep' | 'rewrite' | string
  fragments: number
  lastMpu: number | null
  lastEndS: number | null
  damaged: number
  dropped: number
}
export interface MmtFragment {
  seq: number                    // per service, monotonic, shared with init segments
  packetId: number
  mpu: number
  url: string
  bytes: number
  startS: number                 // position on the shared MSE timeline (timestampOffsetS already included)
  durationS: number
  endS: number
  damaged: boolean
  discontinuity: boolean
  tfdtRewritten: boolean
  presentationUnixMs: number | null
}
export interface MmtIndex {
  bsid: number
  serviceId: number
  selected: boolean
  generation?: number            // changes when the origin restarted its timeline (capture loop wrap, encoder restart,
                                 // source reopen); seqs may restart with it. Missing on an older daemon: treated as 0.
  nextSeq: number
  anchorNtp: string | null
  anchorUnixMs: number | null
  components: MmtComponent[]
  fragments: MmtFragment[]       // cached fragments with seq > since, in arrival order
  liveEdgeS: number
  cachedBytes: number
  cacheLimit: number
  evicted: number
}

// Application Context Cache origin (/apps/{bsid}/{sid}/index.json, spec 5.18.4)
export interface AppsIndexApp {
  index: number
  appContextId: string
  appId: string
  bcastEntryPackageUrl: string | null
  bcastEntryPageUrl: string | null
  bbandEntryPageUrl: string | null
  requiredCapabilities: string | null
  appRendering: boolean
  coupledServices: number[]
  validFromNs: number | null
  validUntilNs: number | null
  ready: boolean
  files: number
  bytes: number
  packageTsi: number | null
  packageBytes: number | null
  packageReceived: number | null
  baseUrl: string            // /apps/{bsid}/{sid}/{index}/
  entryUrl: string | null    // baseUrl + entry page + ?wsURL=&rev= (null for broadband-only apps)
}
export interface AppsIndexFile {
  path: string
  contentType: string
  bytes: number
  version: number | null
  package: string
  appContextIds: string[]
  tsi: number
  toi: number
  receivedNs: number
}
export interface AppsIndex {
  bsid: number
  serviceId: number
  selected: boolean
  wsUrl: string
  rev: string
  apps: AppsIndexApp[]
  files: AppsIndexFile[]
  cachedBytes: number
  cacheLimit: number
  objects: number
  packages: number
  rejected: number
}
