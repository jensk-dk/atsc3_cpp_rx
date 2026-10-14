export function bytes(n: number | null | undefined): string {
  if (n === null || n === undefined) return '-'
  if (n < 1024) return `${n} B`
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} kB`
  if (n < 1024 * 1024 * 1024) return `${(n / 1024 / 1024).toFixed(1)} MB`
  return `${(n / 1024 / 1024 / 1024).toFixed(2)} GB`
}
export function bitrate(bps: number | null | undefined): string {
  if (bps === null || bps === undefined) return '-'
  if (bps >= 1_000_000) return `${(bps / 1_000_000).toFixed(2)} Mbit/s`
  if (bps >= 1000) return `${(bps / 1000).toFixed(0)} kbit/s`
  return `${bps} bit/s`
}
export function seconds(ns: number | null | undefined): string {
  if (ns === null || ns === undefined) return '-'
  const s = ns / 1e9
  const m = Math.floor(s / 60)
  return `${m}:${(s - m * 60).toFixed(1).padStart(4, '0')}`
}
export function channel(major: number | null, minor: number | null): string {
  if (major === null) return '-'
  return minor === null ? String(major) : `${major}.${minor}`
}
export function dateFromNs(ns: number | null | undefined): string {
  if (ns === null || ns === undefined) return '-'
  return new Date(ns / 1e6).toISOString().replace('T', ' ').replace(/\.\d+Z$/, 'Z')
}
export function prettyXml(xml: string): string {
  // light indentation for single-line XML as sent on air
  let formatted = ''
  let indent = ''
  const nodes = xml.replace(/>\s*</g, '><').split(/(?=<)/g)
  for (const node of nodes) {
    if (!node.trim()) continue
    if (/^<\/.+>/.test(node)) indent = indent.slice(2)
    formatted += indent + node.trim() + '\n'
    if (/^<[^!?/][^>]*[^/]>$/.test(node) && !/<\/[^>]+>$/.test(node)) indent += '  '
  }
  return formatted
}
