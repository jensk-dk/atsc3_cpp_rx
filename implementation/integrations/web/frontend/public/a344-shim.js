// A/344 receiver bridge for Broadcaster Applications hosted in the frontend's service view (spec 5.18.4).
// The daemon injects this as the first script of a broadcast entry page it serves from the Application Context
// Cache. The page is launched with the ?wsURL=...&rev=... query terms of A/344 8.2; a `new WebSocket(url)` whose url
// starts with that wsURL value is not a network socket but a channel to the hosting page (window.parent) over
// postMessage, where the JSON-RPC host in src/a344/host.ts answers org.atsc.* requests and sends org.atsc.notify.
// Other URLs get the browser's WebSocket. Uncaught errors and console.error lines are forwarded to the host's app
// console. Plain ES5 on purpose: it runs inside the application, before any of its own scripts.
(function () {
  'use strict'
  var params = new URLSearchParams(window.location.search)
  var wsBase = params.get('wsURL') || ''
  var host = window.parent
  if (!wsBase || !host || host === window) return
  var origin = window.location.origin
  var Native = window.WebSocket
  var nextId = 1
  var sockets = {}

  function post(msg) {
    try { host.postMessage(msg, origin) } catch (e) { /* host gone */ }
  }

  function Bridged(url, protocols) {
    this.url = url
    this.protocol = ''
    this.extensions = ''
    this.binaryType = 'blob'
    this.bufferedAmount = 0
    this.readyState = 0
    this.onopen = null
    this.onmessage = null
    this.onerror = null
    this.onclose = null
    this._id = nextId++
    this._listeners = {}
    sockets[this._id] = this
    post({ a344: 'open', id: this._id, url: url, protocols: protocols === undefined ? null : protocols })
  }
  Bridged.CONNECTING = Bridged.prototype.CONNECTING = 0
  Bridged.OPEN = Bridged.prototype.OPEN = 1
  Bridged.CLOSING = Bridged.prototype.CLOSING = 2
  Bridged.CLOSED = Bridged.prototype.CLOSED = 3
  Bridged.prototype.addEventListener = function (type, fn) {
    if (!fn) return
    ;(this._listeners[type] = this._listeners[type] || []).push(fn)
  }
  Bridged.prototype.removeEventListener = function (type, fn) {
    var l = this._listeners[type]
    if (!l) return
    var i = l.indexOf(fn)
    if (i >= 0) l.splice(i, 1)
  }
  Bridged.prototype.dispatchEvent = function (ev) {
    try { Object.defineProperty(ev, 'target', { value: this }) } catch (e) { /* read-only in some engines */ }
    var handler = this['on' + ev.type]
    if (typeof handler === 'function') handler.call(this, ev)
    var l = (this._listeners[ev.type] || []).slice()
    for (var i = 0; i < l.length; i++) {
      var f = l[i]
      if (typeof f === 'function') f.call(this, ev)
      else if (f && typeof f.handleEvent === 'function') f.handleEvent(ev)
    }
    return true
  }
  Bridged.prototype.send = function (data) {
    if (this.readyState !== 1) throw new DOMException('WebSocket is not open', 'InvalidStateError')
    post({ a344: 'send', id: this._id, data: typeof data === 'string' ? data : String(data) })
  }
  Bridged.prototype.close = function (code, reason) {
    if (this.readyState >= 2) return
    this.readyState = 2
    post({ a344: 'close', id: this._id, code: code === undefined ? 1000 : code, reason: reason || '' })
  }
  function finish(s, code, reason, clean) {
    if (s.readyState === 3) return
    s.readyState = 3
    delete sockets[s._id]
    var ev
    try { ev = new CloseEvent('close', { code: code || 1000, reason: reason || '', wasClean: clean !== false }) } catch (e) { ev = new Event('close') }
    s.dispatchEvent(ev)
  }

  window.addEventListener('message', function (ev) {
    if (ev.source !== host || ev.origin !== origin) return
    var m = ev.data
    if (!m || m.a344host !== true) return
    var s = sockets[m.id]
    if (!s) return
    if (m.type === 'opened') {
      if (s.readyState === 0) {
        s.readyState = 1
        s.protocol = m.protocol || ''
        s.dispatchEvent(new Event('open'))
      }
    } else if (m.type === 'message') {
      if (s.readyState === 1) s.dispatchEvent(new MessageEvent('message', { data: m.data, origin: origin }))
    } else if (m.type === 'error') {
      s.dispatchEvent(new Event('error'))
      finish(s, 1006, m.reason || '', false)
    } else if (m.type === 'closed') {
      finish(s, m.code, m.reason, true)
    }
  })

  function Shim(url, protocols) {
    var u = String(url)
    if (u.indexOf(wsBase) === 0) return new Bridged(u, protocols)
    return protocols === undefined ? new Native(url) : new Native(url, protocols)
  }
  Shim.prototype = Native.prototype   // native sockets keep passing instanceof checks
  Shim.CONNECTING = 0
  Shim.OPEN = 1
  Shim.CLOSING = 2
  Shim.CLOSED = 3
  window.WebSocket = Shim

  function report(level, text) { post({ a344: 'log', level: level, text: String(text) }) }
  window.addEventListener('error', function (e) {
    report('error', (e.message || 'error') + (e.filename ? ' @ ' + String(e.filename).replace(/^.*\//, '') + ':' + e.lineno : ''))
  })
  window.addEventListener('unhandledrejection', function (e) {
    var r = e.reason
    report('error', 'unhandled rejection: ' + (r && r.message ? r.message : String(r)))
  })
  var nativeError = console.error.bind(console)
  console.error = function () {
    var parts = []
    for (var i = 0; i < arguments.length; i++) {
      var a = arguments[i]
      parts.push(typeof a === 'string' ? a : a && a.message ? a.message : String(a))
    }
    report('error', parts.join(' '))
    nativeError.apply(console, arguments)
  }
  window.addEventListener('load', function () { report('info', 'entry page loaded' + (document.title ? ': ' + document.title : '')) })
  post({ a344: 'hello', wsUrl: wsBase, rev: params.get('rev') || '' })
})()
