/**
 * MacDrop Service Module for MacCompanion Middleware
 *
 * Provides push file transfer from modern web browsers to vintage Macintosh clients:
 * - Reverse Push Channel: 68k Mac connects outbound to /api/macdrop/channel,
 *   bypassing NAT, SLIRP, and firewalls with ZERO port forwarding needed.
 * - Multi-Client Management: Automatically detects and lists all connected Macs
 *   (SE/30, Classic, IIci, etc.) and allows targeting specific machines or all machines.
 * - Ingestion endpoint for drag-and-drop file uploads.
 * - Automatic MacBinary II packaging and Type/Creator assignment.
 * - Reliable Transfer ACK: Ensures file is verified and written to disk before reporting success.
 * - Live Client Status: Only shows clients that are actively online.
 */

const express = require('express');
const net = require('net');
const path = require('path');
const fs = require('fs');
const os = require('os');
const { EventEmitter } = require('events');
const { createMacBinary, isMacBinary, parseMacBinaryHeader, getTypeCreatorForFilename, sanitizeMacFilename } = require('./macbinary');
const { createModuleStore } = require('../../core/storage');
const { getLocalIpAddress } = require('../../core/discovery');

const DEFAULT_MACDROP_PORT = 6803;
const HTTP_PORT = parseInt(process.env.PORT || '6800', 10);
const TCP_CONNECT_TIMEOUT_MS = 6000;
const MAX_TRANSFER_LOGS = 30;

// Temporary staging directory for inbound 68k -> modern transfers
const INBOUND_DIR = path.join(os.tmpdir(), 'maccompanion-macdrop-inbound');
if (fs.existsSync(INBOUND_DIR)) {
  try {
    const staleFiles = fs.readdirSync(INBOUND_DIR);
    for (const file of staleFiles) {
      if (file.endsWith('.bin')) {
        try { fs.unlinkSync(path.join(INBOUND_DIR, file)); } catch (_) {}
      }
    }
  } catch (_) {}
} else {
  try { fs.mkdirSync(INBOUND_DIR, { recursive: true }); } catch (_) {}
}

const TYPE_TO_EXT = {
  'PICT': 'pict',
  'TEXT': 'txt',
  'JPEG': 'jpg',
  'GIFf': 'gif',
  'PNGf': 'png',
  'BMP ': 'bmp',
  'TIFF': 'tif',
  'Midi': 'mid',
  'AIFF': 'aif',
  'WAVE': 'wav',
  'Mp3 ': 'mp3',
  'PDF ': 'pdf',
  'ZIP ': 'zip',
  'dImg': 'dsk',
  'DDim': 'dsk',
  'rohd': 'img',
};

function getMimeType(filename, type) {
  const ext = (filename.split('.').pop() || '').toLowerCase();
  switch (ext) {
    case 'pict':
    case 'pct':  return 'image/pict';
    case 'jpg':
    case 'jpeg': return 'image/jpeg';
    case 'gif':  return 'image/gif';
    case 'png':  return 'image/png';
    case 'bmp':  return 'image/bmp';
    case 'tif':
    case 'tiff': return 'image/tiff';
    case 'txt':
    case 'text':
    case 'log':
    case 'c':
    case 'h':    return 'text/plain';
    case 'pdf':  return 'application/pdf';
    case 'aif':
    case 'aiff': return 'audio/aiff';
    case 'wav':  return 'audio/wav';
    case 'mid':
    case 'midi': return 'audio/midi';
    case 'mp3':  return 'audio/mpeg';
    case 'zip':  return 'application/zip';
    case 'dsk':
    case 'img':  return 'application/octet-stream';
    case 'bin':  return 'application/x-macbinary';
    default:
      if (type === 'PICT') return 'image/pict';
      if (type === 'TEXT') return 'text/plain';
      return 'application/octet-stream';
  }
}

class MacDropModule {
  constructor() {
    this.id = 'macdrop';
    this.name = 'MacDrop File Transfer';
    this.basePath = '/api/macdrop';
    this.router = express.Router();

    // Map of active connected 68k clients: clientId -> clientObj
    this.activeClients = new Map();
    this.selectedTargetId = 'auto'; // 'auto', 'all', or specific clientId

    // Pending transfer callbacks: transferId -> { resolve, reject, timer }
    this.pendingTransfers = new Map();

    // Map of staged inbound transfers (68k -> Modern Web / PC): transferId -> transferObj
    this.inboundTransfers = new Map();

    // Channel reconnection listeners for upload in-flight
    this.channelWaiters = new Map();

    // Active transfer progress info for real-time Web UI polling
    this.activeTransfer = null;

    // Cache of manual target ping statuses: `${ip}:${port}` -> { online: boolean, checkedAt: timestamp }
    this.pingCache = new Map();
    this.inFlightPings = new Map();
    this.lastPongByIp = new Map();
    this.peerProbeTimestamps = new Map();

    this.store = createModuleStore('macdrop', {
      targets: [],
      knownDevices: [],
      activeTarget: null,
      selectedTargetId: 'auto',
      recentTransfers: [],
    });

    const saved = this.store.load();
    if (saved && saved.selectedTargetId) {
      this.selectedTargetId = saved.selectedTargetId;
    }

    this._ensureKnownDevices();
    this.initRoutes();

    // Periodic sweep for closed/dead reverse channel sockets
    this.sweepTimer = setInterval(() => this.sweepDeadClients(), 1500);
    if (this.sweepTimer && this.sweepTimer.unref) {
      this.sweepTimer.unref();
    }
  }

  /**
   * Seeds/ensures knownDevices list is populated from targets or transfer history,
   * strictly maintaining one entry per IP with the last name and model reported/seen.
   */
  _ensureKnownDevices() {
    try {
      const data = this.store.load();
      const deviceByIp = new Map();

      // Seed from historical recent transfers (newest first, so first seen per IP is latest)
      if (Array.isArray(data.recentTransfers)) {
        for (const xfer of data.recentTransfers) {
          if (xfer && xfer.targetIp && !deviceByIp.has(xfer.targetIp)) {
            let machine = 'Vintage Macintosh';
            if (xfer.targetName && xfer.targetName.includes('SE/30')) machine = 'Macintosh SE/30';
            else if (xfer.targetName && xfer.targetName.includes('IIci')) machine = 'Macintosh IIci';
            else if (xfer.targetName && xfer.targetName.includes('Basilisk')) machine = 'Macintosh 68k (Basilisk II)';

            deviceByIp.set(xfer.targetIp, {
              id: `device_${xfer.targetIp.replace(/\./g, '_')}`,
              name: xfer.targetName || `Macintosh (${xfer.targetIp})`,
              machine,
              ip: xfer.targetIp,
              port: DEFAULT_MACDROP_PORT,
              version: '',
              lastSeen: xfer.timestamp || Date.now(),
              source: 'history',
            });
          }
        }
      }

      // Existing knownDevices take precedence if they have newer or manually configured data
      if (Array.isArray(data.knownDevices)) {
        for (const d of data.knownDevices) {
          if (d && d.ip) {
            const cleanIp = d.ip.trim();
            const existing = deviceByIp.get(cleanIp);
            if (!existing || (d.lastSeen && d.lastSeen >= (existing.lastSeen || 0))) {
              deviceByIp.set(cleanIp, {
                id: `device_${cleanIp.replace(/\./g, '_')}`,
                name: d.name || `Macintosh (${cleanIp})`,
                machine: d.machine || 'Vintage Macintosh',
                ip: cleanIp,
                port: d.port || DEFAULT_MACDROP_PORT,
                version: d.version || '',
                lastSeen: d.lastSeen || Date.now(),
                source: d.source || 'reverse_channel',
              });
            }
          }
        }
      }

      // Manual targets also merged strictly by IP
      if (Array.isArray(data.targets)) {
        for (const t of data.targets) {
          if (t && t.ip) {
            const cleanIp = t.ip.trim();
            deviceByIp.set(cleanIp, {
              id: `device_${cleanIp.replace(/\./g, '_')}`,
              name: t.name || `Macintosh (${cleanIp})`,
              machine: t.machine || 'Vintage Macintosh',
              ip: cleanIp,
              port: t.port || DEFAULT_MACDROP_PORT,
              version: t.version || '',
              lastSeen: t.lastSeen || Date.now(),
              source: 'manual',
            });
          }
        }
      }

      const cleanedList = Array.from(deviceByIp.values());
      this.store.save({ knownDevices: cleanedList });
    } catch (err) {
      console.warn('[MacDrop] Failed to initialize known devices:', err);
    }
  }

  /**
   * Records or updates a known device in the store (strictly one entry per IP,
   * retaining the latest name, model and timestamp reported).
   */
  recordKnownDevice(dev) {
    try {
      if (!dev || !dev.ip) return;
      const cleanIp = dev.ip.trim();
      const data = this.store.load();
      let knownDevices = Array.isArray(data.knownDevices) ? [...data.knownDevices] : [];

      const idx = knownDevices.findIndex(d => d.ip === cleanIp);

      const entry = {
        id: `device_${cleanIp.replace(/\./g, '_')}`,
        name: dev.name || (idx >= 0 ? knownDevices[idx].name : `Macintosh (${cleanIp})`),
        machine: dev.machine || (idx >= 0 ? knownDevices[idx].machine : 'Macintosh 68k'),
        ip: cleanIp,
        port: dev.port || (idx >= 0 ? knownDevices[idx].port : DEFAULT_MACDROP_PORT),
        version: dev.version || (idx >= 0 ? knownDevices[idx].version : ''),
        lastSeen: Date.now(),
        source: dev.source || (idx >= 0 ? knownDevices[idx].source : 'reverse_channel'),
      };

      if (idx >= 0) {
        knownDevices[idx] = entry;
      } else {
        knownDevices.push(entry);
      }

      // Strictly deduplicate by IP across the list
      const deduped = [];
      const seenIps = new Set();
      for (const d of knownDevices) {
        if (d && d.ip && !seenIps.has(d.ip)) {
          seenIps.add(d.ip);
          deduped.push(d);
        }
      }

      this.store.save({ knownDevices: deduped });
    } catch (e) {
      console.warn('[MacDrop] Failed to record known device:', e);
    }
  }

  /**
   * Waits up to timeoutMs for a client reverse channel to reconnect.
   */
  waitForClientChannel(clientId, timeoutMs = 2500) {
    return new Promise((resolve) => {
      const client = this.activeClients.get(clientId);
      if (client && client.res && !client.res.writableEnded && !client.res.destroyed) {
        return resolve(true);
      }
      const timer = setTimeout(() => {
        this.channelWaiters.delete(clientId);
        resolve(false);
      }, timeoutMs);

      this.channelWaiters.set(clientId, () => {
        clearTimeout(timer);
        this.channelWaiters.delete(clientId);
        resolve(true);
      });
    });
  }

  /**
   * Checks whether any file transfer (outbound or inbound) is currently active.
   */
  hasActiveTransfer() {
    if (this.activeTransfer && (this.activeTransfer.status === 'STREAMING' || this.activeTransfer.status === 'WRITING_DISK')) {
      return true;
    }
    for (const t of this.inboundTransfers.values()) {
      if (t.status === 'STREAMING' || t.status === 'DOWNLOADING') {
        return true;
      }
    }
    for (const c of this.activeClients.values()) {
      if (c.transferInProgress || c.writingDisk) {
        return true;
      }
    }
    return false;
  }

  /**
   * Returns online 68k Macintosh peers visible to a given requesting 68k client.
   */
  getOnlinePeersForClient(excludeId = '', excludeIp = '') {
    const peers = [];
    const seenIds = new Set();
    const seenIps = new Set();
    const cleanExcludeId = (excludeId || '').trim();
    const cleanExcludeIp = (excludeIp || '').trim();

    const sanitizeField = (str) => String(str || '').replace(/[|;\r\n]/g, ' ').trim();

    // 1. Live active reverse-channel clients
    for (const c of this.activeClients.values()) {
      if (!c) continue;
      if (cleanExcludeId && c.id === cleanExcludeId) continue;
      if (!cleanExcludeId && cleanExcludeIp && c.ip === cleanExcludeIp) continue;
      seenIds.add(c.id);
      if (c.ip) seenIps.add(c.ip.trim());
      peers.push({
        id: sanitizeField(c.id),
        name: sanitizeField(c.name || `Macintosh (${c.ip})`),
        machine: sanitizeField(c.machine || 'Macintosh 68k'),
        ip: (c.ip || '').trim(),
      });
    }

    // 2. Online manual / known LAN targets from pingCache
    try {
      const data = this.store.load();
      for (const d of [...(data.knownDevices || []), ...(data.targets || [])]) {
        if (!d || !d.ip) continue;
        const ip = d.ip.trim();
        if (seenIps.has(ip)) continue;
        if (cleanExcludeIp && ip === cleanExcludeIp) continue;
        const port = d.port || DEFAULT_MACDROP_PORT;
        const isOnline = Boolean(this.pingCache.get(`${ip}:${port}`)?.online);
        if (!isOnline) continue;

        const devId = d.id || `device_${ip.replace(/\./g, '_')}`;
        if (cleanExcludeId && devId === cleanExcludeId) continue;
        if (seenIds.has(devId)) continue;

        seenIds.add(devId);
        seenIps.add(ip);
        const pongMeta = this.lastPongByIp.get(ip);
        peers.push({
          id: sanitizeField(devId),
          name: sanitizeField((pongMeta && pongMeta.name) || d.name || `Macintosh (${ip})`),
          machine: sanitizeField((pongMeta && pongMeta.machine) || d.machine || 'Vintage Macintosh'),
          ip,
        });
      }
    } catch (_) {}

    return peers.slice(0, 6);
  }

  /**
   * Formats online peers into a compact header value for 68k clients:
   * id|name|machine;id2|name2|machine2
   */
  formatPeersHeaderValue(excludeId = '', excludeIp = '') {
    const peers = this.getOnlinePeersForClient(excludeId, excludeIp);
    if (peers.length === 0) return 'NONE';
    return peers.map(p => `${p.id}|${p.name}|${p.machine}`).join(';');
  }

  /**
   * Safely closes a reverse-channel response socket so the 68k client immediately
   * detects disconnect and reconnects instead of hanging on an orphaned stream.
   */
  _closeClientSocket(c) {
    if (c && c.res && !c.res.writableEnded && !c.res.destroyed) {
      try {
        c.res.writeHead(204, {
          'X-MacDrop-Status': 'POLL',
          'X-MacDrop-Peers': this.formatPeersHeaderValue(c.id, c.ip),
          'Connection': 'close',
        });
        c.res.end();
      } catch (_) {
        try { c.res.destroy(); } catch (_) {}
      }
    }
  }

  /**
   * Sweeps active clients to purge dead or closed sockets immediately.
   * Clients in active probing/scanning or active transfers are protected.
   */
  sweepDeadClients() {
    const now = Date.now();
    for (const [id, c] of this.activeClients.entries()) {
      // Never sweep or probe clients if a transfer is in progress for them or globally
      if (c.transferInProgress || c.writingDisk) {
        c.probing = false;
        c.lastSeen = now;
        continue;
      }
      for (const t of this.inboundTransfers.values()) {
        if ((t.clientId === id || t.clientName === c.name) && (t.status === 'STREAMING' || t.status === 'DOWNLOADING')) {
          c.transferInProgress = true;
          c.probing = false;
          c.lastSeen = now;
          break;
        }
      }
      if (c.transferInProgress) continue;

      // If client is currently in a probe/scan cycle, protect it until probe expires
      if (c.probing) {
        if (now - c.probeSentAt > 2500) {
          this._closeClientSocket(c);
          this.activeClients.delete(id);
          console.log(`[MacDrop] Cleaned up timed out probing client: ${c.name} (${id})`);
        }
        continue;
      }

      const isSocketDead = !c.res ||
                           c.res.destroyed ||
                           !c.res.socket ||
                           c.res.socket.destroyed ||
                           (c.req && c.req.destroyed) ||
                           c.res.writableEnded;

      // If socket is dead and client has not reconnected in > 2000ms, delete
      if (isSocketDead && !c.writingDisk && (now - c.lastSeen > 2000)) {
        this._closeClientSocket(c);
        this.activeClients.delete(id);
        console.log(`[MacDrop] Cleaned up dead client socket: ${c.name} (${id})`);
        continue;
      }

      // If client has not been seen in > 20 seconds, close socket and delete
      if (!c.writingDisk && (now - c.lastSeen > 20000)) {
        this._closeClientSocket(c);
        this.activeClients.delete(id);
        console.log(`[MacDrop] Cleaned up timed out client: ${c.name} (${id})`);
        continue;
      }

      // If client was writing disk but timed out after 120s without ACK, close socket and delete
      if (c.writingDisk && (now - c.lastSeen > 120000)) {
        this._closeClientSocket(c);
        this.activeClients.delete(id);
        console.log(`[MacDrop] Cleaned up client timed out on disk write: ${c.name} (${id})`);
        continue;
      }
    }

    // Sweep expired staged inbound transfers (30s timeout per user requirement) or stalled streams (>60s without data)
    for (const [id, t] of this.inboundTransfers.entries()) {
      if (t.expiresAt && now > t.expiresAt && t.status !== 'DOWNLOADING' && t.status !== 'COMPLETED') {
        this.cleanupInboundTransfer(id, 'TIMED_OUT_30S');
      } else if (t.status === 'STREAMING' && (now - t.lastProgressAt > 60000)) {
        this.cleanupInboundTransfer(id, 'STALLED_60S');
      }
    }
  }

  /**
   * Cleans up and deletes a staged inbound transfer from memory and disk.
   */
  cleanupInboundTransfer(id, reason = 'TIMEOUT') {
    if (!this.inboundTransfers.has(id)) return;
    const transfer = this.inboundTransfers.get(id);
    if (transfer.timer) {
      clearTimeout(transfer.timer);
      transfer.timer = null;
    }

    // Abort incoming upload from 68k Mac if active
    if (transfer.req && !transfer.req.destroyed) {
      try { transfer.req.destroy(); } catch (_) {}
    }
    if (transfer.res && !transfer.res.writableEnded && !transfer.res.destroyed) {
      try { transfer.res.destroy(); } catch (_) {}
    }

    // Abort active browser download stream if currently streaming
    if (transfer.activeDownloadRes && !transfer.activeDownloadRes.writableEnded && !transfer.activeDownloadRes.destroyed) {
      try { transfer.activeDownloadRes.destroy(); } catch (_) {}
      transfer.activeDownloadRes = null;
    }

    if (transfer.fd) {
      try { fs.closeSync(transfer.fd); } catch (_) {}
      transfer.fd = null;
    }
    if (transfer.filePath && fs.existsSync(transfer.filePath)) {
      try {
        fs.unlinkSync(transfer.filePath);
      } catch (_) {}
    }

    // Clear client busy flag
    if (transfer.clientId && this.activeClients.has(transfer.clientId)) {
      const c = this.activeClients.get(transfer.clientId);
      c.transferInProgress = false;
      c.lastSeen = Date.now();
    }

    this.inboundTransfers.delete(id);
    console.log(`[MacDrop] Cleaned up inbound transfer ${id} ("${transfer.fileName}") [Reason: ${reason}]`);
  }

  /**
   * Waits for transfer completion ACK from the 68k Mac client.
   */
  waitForTransferAck(transferId, timeoutMs = 60000, clientId = null) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingTransfers.delete(transferId);
        reject(new Error(`Timed out waiting for Macintosh client confirmation (${Math.round(timeoutMs / 1000)}s)`));
      }, timeoutMs);

      this.pendingTransfers.set(transferId, {
        clientId,
        resolve: (data) => {
          clearTimeout(timer);
          this.pendingTransfers.delete(transferId);
          resolve(data);
        },
        reject: (err) => {
          clearTimeout(timer);
          this.pendingTransfers.delete(transferId);
          reject(err);
        },
      });
    });
  }

  /**
   * Called when client reports transfer completion or error.
   */
  handleTransferAck(transferId, status, bytes) {
    if (!transferId) return;
    const pending = this.pendingTransfers.get(transferId);
    if (pending) {
      if (status === 'OK') {
        pending.resolve({ ok: true, bytes });
      } else if (status === 'CANCELLED') {
        if (this.activeTransfer && this.activeTransfer.transferId === transferId) {
          this.activeTransfer.status = 'CANCELLED';
          this.activeTransfer.error = 'Transfer cancelled by Macintosh client';
          this.activeTransfer.speedFormatted = 'Cancelled';
        }
        pending.reject(new Error('Transfer cancelled by Macintosh client'));
      } else {
        pending.reject(new Error(`Macintosh reported write failure (${status})`));
      }
    }
  }

  /**
   * Resolves the true LAN IPv4 of a connecting 68k Macintosh client,
   * preferring the client's self-reported LAN IP when sitting behind Docker NAT.
   */
  resolveClientIp(req) {
    const remoteIp = ((req.socket && req.socket.remoteAddress) || '').replace(/^.*:/, '').trim();
    const reportedIp = ((req.query && req.query.ip) || (req.headers && req.headers['x-client-ip']) || '').trim();

    if (/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(reportedIp) &&
        reportedIp !== '0.0.0.0' &&
        reportedIp !== '127.0.0.1' &&
        !reportedIp.startsWith('10.0.2.')) {
      const isDockerNatOrLoopback =
        !remoteIp ||
        remoteIp === '127.0.0.1' ||
        remoteIp.startsWith('192.168.97.') ||
        remoteIp.startsWith('192.168.65.') ||
        /^172\.(1[6-9]|2\d|3[0-1])\./.test(remoteIp);

      if (isDockerNatOrLoopback) {
        return reportedIp;
      }
    }
    return remoteIp || reportedIp || '127.0.0.1';
  }

  /**
   * Passively discovers 68k Macintosh peers that communicate with other MacCompanion modules
   * (e.g. SpotiMac, MacAI, /api/ping) across WiFi bridges where UDP broadcast is filtered.
   */
  notifyPeerRequest(req) {
    try {
      const peerIp = this.resolveClientIp(req);
      if (!peerIp ||
          peerIp === '127.0.0.1' ||
          peerIp === '0.0.0.0' ||
          peerIp.startsWith('10.0.2.') ||
          peerIp.startsWith('192.168.97.') ||
          peerIp.startsWith('192.168.65.') ||
          /^172\.(1[6-9]|2\d|3[0-1])\./.test(peerIp)) {
        return;
      }

      // Skip if already connected via reverse channel
      for (const c of this.activeClients.values()) {
        if (c.ip === peerIp) return;
      }

      const now = Date.now();
      const lastProbe = this.peerProbeTimestamps.get(peerIp) || 0;
      if (now - lastProbe < 15000) return;
      this.peerProbeTimestamps.set(peerIp, now);

      this.pingTarget(peerIp, DEFAULT_MACDROP_PORT, 1500).then((online) => {
        if (online) {
          this.pingCache.set(`${peerIp}:${DEFAULT_MACDROP_PORT}`, { online: true, checkedAt: Date.now() });
        }
      }).catch(() => {});
    } catch (_) {}
  }

  /**
   * Fast TCP ping & auto-discovery handshake for vintage Mac targets on LAN.
   * Sends MACDROP_HELLO|<serverIp>|<httpPort> so bridged 68k Macs automatically learn
   * the server IP and reply with MACDROP_PONG|<id>|<name>|<machine>|<ver>|<ip>.
   * Concurrent pings to the same IP:port are deduplicated to protect the 68k single-stream listener.
   */
  pingTarget(ip, port = DEFAULT_MACDROP_PORT, timeoutMs = 1800) {
    if (!ip) return Promise.resolve(false);
    const cleanIp = ip.trim();
    const key = `${cleanIp}:${port}`;

    // If client is already connected live via reverse channel, it is online
    for (const c of this.activeClients.values()) {
      if (c.ip === cleanIp) {
        this.pingCache.set(key, { online: true, checkedAt: Date.now() });
        return Promise.resolve(true);
      }
    }

    if (this.inFlightPings.has(key)) {
      return this.inFlightPings.get(key);
    }

    const pingPromise = new Promise((resolve) => {
      const socket = new net.Socket();
      let settled = false;
      let connected = false;
      let pongTimer = null;
      let rxBuf = '';

      const finish = (result) => {
        if (settled) return;
        settled = true;
        if (pongTimer) clearTimeout(pongTimer);
        try { socket.destroy(); } catch (_) {}
        this.inFlightPings.delete(key);
        this.pingCache.set(key, { online: result, checkedAt: Date.now() });
        resolve(result);
      };

      socket.setTimeout(timeoutMs);

      socket.on('connect', () => {
        connected = true;
        try {
          const serverIp = getLocalIpAddress();
          socket.write(`MACDROP_HELLO|${serverIp}|${HTTP_PORT}\n`);
        } catch (_) {}

        // Wait up to 350ms for optional MACDROP_PONG metadata from MacDrop v0.2.7+
        pongTimer = setTimeout(() => {
          finish(true);
        }, 350);
      });

      socket.on('data', (chunk) => {
        rxBuf += chunk.toString('utf8');
        if (rxBuf.includes('MACDROP_PONG|')) {
          const line = rxBuf.split(/\r?\n/).find(l => l.startsWith('MACDROP_PONG|')) || rxBuf;
          const parts = line.trim().split('|');
          // MACDROP_PONG|<id>|<name>|<machine>|<ver>|<ourIp>
          const pongId = (parts[1] || '').trim();
          const pongName = (parts[2] || '').trim();
          const pongMachine = (parts[3] || '').trim();
          const pongVer = (parts[4] || '').trim();

          const meta = {
            id: pongId || `device_${cleanIp.replace(/\./g, '_')}`,
            name: pongName || `Macintosh (${cleanIp})`,
            machine: pongMachine || 'Vintage Macintosh',
            version: pongVer || '',
            ip: cleanIp,
            port,
            lastSeen: Date.now(),
          };
          this.lastPongByIp.set(cleanIp, meta);
          this.recordKnownDevice({ ...meta, source: 'manual' });

          // Also update saved targets if present
          try {
            const data = this.store.load();
            if (Array.isArray(data.targets)) {
              let updated = false;
              const targets = data.targets.map(t => {
                if (t && t.ip === cleanIp) {
                  updated = true;
                  return {
                    ...t,
                    name: meta.name || t.name,
                    machine: meta.machine || t.machine,
                    version: meta.version || t.version,
                    lastSeen: Date.now(),
                    online: true,
                  };
                }
                return t;
              });
              if (updated) {
                this.store.save({ targets });
              }
            }
          } catch (_) {}

          finish(true);
        }
      });

      socket.on('end', () => {
        finish(connected);
      });

      socket.on('close', () => {
        finish(connected);
      });

      socket.on('timeout', () => {
        finish(connected);
      });

      socket.on('error', () => {
        finish(connected);
      });

      socket.connect(port, cleanIp);
    });

    this.inFlightPings.set(key, pingPromise);
    return pingPromise;
  }

  /**
   * Parse multipart/form-data buffer without external dependencies.
   */
  parseMultipart(reqBody, contentTypeHeader) {
    if (!reqBody || !contentTypeHeader) return null;
    const boundaryMatch = contentTypeHeader.match(/boundary=(?:"([^"]+)"|([^;]+))/i);
    if (!boundaryMatch) return null;

    const boundary = boundaryMatch[1] || boundaryMatch[2];
    const boundaryBuf = Buffer.from('--' + boundary);

    let start = 0;
    while (start < reqBody.length) {
      const bIdx = reqBody.indexOf(boundaryBuf, start);
      if (bIdx === -1) break;

      const headerStart = bIdx + boundaryBuf.length + 2; // skip \r\n
      const headerEnd = reqBody.indexOf(Buffer.from('\r\n\r\n'), headerStart);
      if (headerEnd === -1) break;

      const headersText = reqBody.slice(headerStart, headerEnd).toString('latin1');
      const nextBoundary = reqBody.indexOf(boundaryBuf, headerEnd + 4);
      if (nextBoundary === -1) break;

      const dataEnd = (nextBoundary >= 2 && reqBody[nextBoundary - 2] === 13 && reqBody[nextBoundary - 1] === 10)
        ? nextBoundary - 2
        : nextBoundary;
      const data = reqBody.slice(headerEnd + 4, dataEnd);

      const filenameMatch = headersText.match(/filename="([^"]+)"/i) || headersText.match(/filename=([^\s;]+)/i);
      if (filenameMatch) {
        return {
          filename: path.basename(filenameMatch[1]),
          data,
        };
      }

      start = nextBoundary;
    }
    return null;
  }

  initRoutes() {
    // Middleware to capture raw body for uploads
    this.router.use((req, res, next) => {
      if (req.method === 'POST' && req.path === '/upload') {
        const chunks = [];
        req.on('data', chunk => chunks.push(chunk));
        req.on('end', () => {
          req.rawBuffer = Buffer.concat(chunks);
          next();
        });
      } else {
        next();
      }
    });

    // 1. REVERSE PUSH CHANNEL: Vintage Mac connects outbound and waits for file pushes
    this.router.get('/channel', (req, res) => {
      const remoteIp = this.resolveClientIp(req);
      const rawId = (req.query.id || req.headers['x-client-id'] || '').trim();
      const name = (req.query.name || req.headers['x-client-name'] || `Macintosh (${remoteIp})`).trim();
      const machine = (req.query.machine || req.headers['x-machine-model'] || 'Macintosh 68k').trim();
      const version = (req.query.ver || req.headers['x-client-ver'] || '0.1.0').trim();

      const clientId = rawId || `mac_${remoteIp.replace(/\./g, '_')}_${machine.toLowerCase().replace(/[^a-z0-9]/g, '')}`;

      // Disable timeouts on reverse push socket and enable fast keepalive
      req.socket.setTimeout(0);
      req.socket.setKeepAlive(true, 3000);

      // If client was previously registered, close previous pending channel cleanly
      if (this.activeClients.has(clientId)) {
        const oldClient = this.activeClients.get(clientId);
        try {
          if (oldClient.res && !oldClient.res.writableEnded) {
            oldClient.res.end();
          }
        } catch (_) {}
      }

      if (req.socket) {
        req.socket.setNoDelay(true);
      }
      res.shouldKeepAlive = false;

      const clientObj = {
        id: clientId,
        name,
        machine,
        version,
        ip: remoteIp,
        mode: 'CHANNEL',
        connectedAt: Date.now(),
        lastSeen: Date.now(),
        res,
        req,
        transferInProgress: false,
        writingDisk: false,
        probing: false,
        awaitingReconnect: false,
      };

      this.activeClients.set(clientId, clientObj);
      console.log(`[MacDrop] Reverse Channel connected: ${name} (${machine}) from ${remoteIp} [ID: ${clientId}]`);

      // Persist to known devices
      this.recordKnownDevice({
        id: clientId,
        name,
        machine,
        version,
        ip: remoteIp,
        port: DEFAULT_MACDROP_PORT,
        source: 'reverse_channel',
      });

      // Notify any in-flight transfer waiting for channel reconnection
      const waiter = this.channelWaiters.get(clientId);
      if (waiter) {
        waiter();
      }

      // Idle poll timer: cleanly end response after 8s with 204 No Content so client reconnects and proves liveness.
      // Uses setInterval so that if a multi-minute transfer is active when 8s elapses, the timer keeps running
      // and refreshes lastSeen until the transfer finishes, then triggers a clean 204 POLL reconnect.
      const pollTimer = setInterval(() => {
        const curr = this.activeClients.get(clientId);
        if (!curr || curr.res !== res || res.writableEnded || res.destroyed) {
          clearInterval(pollTimer);
          return;
        }
        if (curr.transferInProgress || curr.writingDisk || this.hasActiveTransfer()) {
          curr.lastSeen = Date.now();
          return;
        }
        clearInterval(pollTimer);
        try {
          curr.probing = true;
          curr.probeSentAt = Date.now();
          res.writeHead(204, {
            'X-MacDrop-Status': 'POLL',
            'X-MacDrop-Peers': this.formatPeersHeaderValue(clientId, remoteIp),
            'Connection': 'close',
          });
          res.end();
        } catch (_) {}
      }, 8000);

      // Cleanup when connection closes or aborts
      const onDisconnect = () => {
        clearInterval(pollTimer);
        const curr = this.activeClients.get(clientId);
        if (curr && curr.res === res) {
          if (curr.transferInProgress) {
            console.log(`[MacDrop] Client socket closed while streaming data: ${name} (${clientId})`);
            for (const [xferId, pending] of this.pendingTransfers.entries()) {
              if (pending.clientId === clientId) {
                pending.reject(new Error('Connection lost while streaming file to Macintosh'));
              }
            }
            this.activeClients.delete(clientId);
          } else if (curr.writingDisk) {
            console.log(`[MacDrop] Reverse channel stream completed; awaiting disk write ACK from ${name}`);
          } else if (curr.probing) {
            // Intentionally closed for probe/poll: probe logic handles reconnection
          } else {
            // Socket closed unexpectedly; give 1500ms grace period for client reconnect
            setTimeout(() => {
              const check = this.activeClients.get(clientId);
              if (check && !check.probing && !check.transferInProgress && !this.hasActiveTransfer() && (!check.res || check.res === res || check.res.writableEnded || check.res.destroyed)) {
                this.activeClients.delete(clientId);
                console.log(`[MacDrop] Cleaned up offline client (no reconnect after socket close): ${name} (${clientId})`);
              }
            }, 1500);
          }
        }
      };

      req.on('close', onDisconnect);
      req.on('error', onDisconnect);
      res.on('close', onDisconnect);
      res.on('error', onDisconnect);
    });

    // 2. Client Transfer ACK Endpoint
    this.router.get('/ack', (req, res) => {
      res.shouldKeepAlive = false;
      res.setHeader('Connection', 'close');

      const transferId = req.query.transfer_id;
      const status = req.query.status || 'OK';
      const bytes = parseInt(req.query.bytes || '0', 10);
      const clientId = req.query.id;

      console.log(`[MacDrop] Transfer ACK: id=${transferId}, status=${status}, bytes=${bytes} from client ${clientId}`);
      this.handleTransferAck(transferId, status, bytes);

      if (clientId && this.activeClients.has(clientId)) {
        const c = this.activeClients.get(clientId);
        c.lastSeen = Date.now();
        c.writingDisk = false;
      }

      res.json({ ok: true });
    });

    // 2b. Manual Scan & Refresh Endpoint
    this.router.get('/refresh', async (req, res) => {
      await this.refreshAllClients();
      res.json(this.getStatus());
    });
    this.router.post('/refresh', async (req, res) => {
      await this.refreshAllClients();
      res.json(this.getStatus());
    });

    // 2c. Real-Time Active File Transfer Progress Endpoint
    this.router.get('/progress', (req, res) => {
      if (this.activeTransfer) {
        const now = Date.now();
        if (this.activeTransfer.status === 'STREAMING' && this.activeTransfer.drainStartedAt) {
          const drainElapsedSec = Math.max(0, (now - this.activeTransfer.drainStartedAt) / 1000);
          const drainSpeed = this.activeTransfer.drainSpeedBps || 650000;
          const totalBytes = this.activeTransfer.totalBytes || 1;
          const estBytes = Math.min(
            totalBytes,
            Math.round((this.activeTransfer.drainStartBytes || 0) + drainElapsedSec * drainSpeed)
          );
          this.activeTransfer.bytesSent = estBytes;
          this.activeTransfer.percent = Math.min(96, Math.max(this.activeTransfer.percent || 0, Math.round((estBytes / totalBytes) * 96)));
          if (estBytes >= totalBytes) {
            this.activeTransfer.status = 'WRITING_DISK';
            this.activeTransfer.writingDiskStartedAt = now;
            this.activeTransfer.percent = Math.max(this.activeTransfer.percent, 97);
            this.activeTransfer.speedFormatted = 'Finalizing on Macintosh...';
          }
        } else if (this.activeTransfer.status === 'WRITING_DISK') {
          const elapsedMs = now - (this.activeTransfer.writingDiskStartedAt || now);
          const startPct = this.activeTransfer.writingDiskStartPct || 97;
          const steppedPct = startPct + Math.floor(elapsedMs / 350);
          this.activeTransfer.percent = Math.min(99, Math.max(this.activeTransfer.percent || startPct, steppedPct));
        }
      }
      res.json({
        ok: true,
        transfer: this.activeTransfer || null,
      });
    });

    // 3. Service Status (Multi-client aware)
    this.router.get('/status', (req, res) => {
      res.json(this.getStatus());
    });

    // 3a. Compact Peer Discovery Endpoint for 68k Macintosh Clients
    this.router.get('/peers', (req, res) => {
      if (!this.hasActiveTransfer()) {
        this.sweepDeadClients();
      }
      const excludeId = (req.query.id || req.query.exclude || req.headers['x-client-id'] || '').trim();
      const excludeIp = this.resolveClientIp(req);
      const peers = this.getOnlinePeersForClient(excludeId, excludeIp);
      const headerVal = peers.length > 0
        ? peers.map(p => `${p.id}|${p.name}|${p.machine}`).join(';')
        : 'NONE';

      res.setHeader('Content-Type', 'text/plain; charset=utf-8');
      res.setHeader('X-MacDrop-Peers', headerVal);
      res.setHeader('Connection', 'close');

      const lines = ['SERVER|MacCompanion Server|Web Browser'];
      for (const p of peers) {
        lines.push(`PEER|${p.id}|${p.name}|${p.machine}`);
      }
      res.send(lines.join('\n') + '\n');
    });

    // 3b. INBOUND TRANSFER ENDPOINTS (68k Vintage Mac -> Server / Modern Web UI OR Peer 68k Mac Relay)
    // - 68k Mac uploads MacBinary payload (streaming & simultaneous pass-through or peer relay)
    this.router.post('/inbound/upload', (req, res) => {
      if (req.socket && typeof req.socket.setNoDelay === 'function') {
        req.socket.setNoDelay(true);
      }
      const remoteIp = this.resolveClientIp(req);
      const clientId = (req.query.id || req.headers['x-client-id'] || '').trim();
      const clientName = (req.query.name || req.headers['x-client-name'] || `Macintosh (${remoteIp})`).trim();
      const rawTargetId = (req.query.target_id || req.headers['x-target-id'] || '').trim();
      const isPeerRelay = Boolean(rawTargetId && rawTargetId.toUpperCase() !== 'SERVER');
      const transferId = (isPeerRelay ? 'p2p_' : 'in_') + Date.now().toString(36) + '_' + Math.random().toString(36).substring(2, 6);

      // Locate client in activeClients to protect it from probe sweeps while streaming
      let client = null;
      if (clientId && this.activeClients.has(clientId)) {
        client = this.activeClients.get(clientId);
      } else {
        for (const c of this.activeClients.values()) {
          if (c.ip === remoteIp || c.name === clientName) {
            client = c;
            break;
          }
        }
      }

      if (client) {
        client.transferInProgress = true;
        client.probing = false;
        client.lastSeen = Date.now();
      }

      const stagingPath = path.join(INBOUND_DIR, `${transferId}.bin`);
      let fd = null;
      try {
        fd = fs.openSync(stagingPath, 'w');
      } catch (err) {
        if (client) client.transferInProgress = false;
        console.error(`[MacDrop] Failed to create staging file ${stagingPath}:`, err);
        return res.status(500).json({ ok: false, error: 'Cannot create staging file' });
      }

      // If this is a 68k-to-68k peer relay, buffer the uploaded MacBinary II file and forward it to the target Mac
      if (isPeerRelay) {
        let totalReceived = 0;
        let headerBuffer = Buffer.alloc(0);
        let parsedFileName = req.headers['x-filename'] || 'MacintoshFile';
        const expectedTotal = parseInt(req.headers['content-length'] || '0', 10) || 0;
        const resolvedTargets = this.resolveTargets(rawTargetId);
        const targetLabel = resolvedTargets.length > 0 ? (resolvedTargets[0].name || resolvedTargets[0].ip) : rawTargetId;

        req.on('data', chunk => {
          const prevReceived = totalReceived;
          totalReceived += chunk.length;
          if (client) {
            client.transferInProgress = true;
            client.lastSeen = Date.now();
          }
          if (fd !== null) {
            try {
              fs.writeSync(fd, chunk, 0, chunk.length, prevReceived);
            } catch (_) {}
          }
          if (headerBuffer.length < 128) {
            headerBuffer = Buffer.concat([headerBuffer, chunk]);
            if (headerBuffer.length >= 128) {
              const info = parseMacBinaryHeader(headerBuffer.subarray(0, 128));
              if (info && info.fileName) {
                parsedFileName = sanitizeMacFilename(info.fileName);
              }
              this.activeTransfer = {
                transferId,
                fileName: parsedFileName,
                targetName: `${clientName} → ${targetLabel}`,
                totalBytes: expectedTotal || (128 + (info ? info.dataLength + info.rsrcLength : 0)),
                bytesSent: totalReceived,
                percent: 10,
                speedBps: 0,
                speedFormatted: 'Relaying...',
                status: 'STREAMING',
                startedAt: Date.now(),
                lastProgressAt: Date.now(),
              };
            }
          } else if (this.activeTransfer && this.activeTransfer.transferId === transferId) {
            this.activeTransfer.bytesSent = Math.round(totalReceived / 2);
            if (this.activeTransfer.totalBytes > 0) {
              this.activeTransfer.percent = Math.min(48, Math.round((totalReceived / this.activeTransfer.totalBytes) * 48));
            }
            this.activeTransfer.lastProgressAt = Date.now();
          }
        });

        req.on('end', () => {
          if (client) {
            client.transferInProgress = false;
            client.lastSeen = Date.now();
          }
          if (fd !== null) {
            try { fs.closeSync(fd); } catch (_) {}
            fd = null;
          }

          let macBinaryBuffer = null;
          try {
            macBinaryBuffer = fs.readFileSync(stagingPath);
            fs.unlinkSync(stagingPath);
          } catch (readErr) {
            console.error(`[MacDrop] Failed reading peer relay staging file ${stagingPath}:`, readErr);
            return res.status(500).json({ ok: false, error: 'Failed reading relay buffer' });
          }

          const targetsToPush = this.resolveTargets(rawTargetId);
          if (targetsToPush.length === 0) {
            if (this.activeTransfer && this.activeTransfer.transferId === transferId) {
              this.activeTransfer = null;
            }
            return res.status(404).json({ ok: false, error: 'Target Macintosh is offline' });
          }

          const target = targetsToPush[0];
          const macInfo = parseMacBinaryHeader(macBinaryBuffer) || {
            fileName: parsedFileName,
            type: '????',
            creator: '????',
            dataLength: macBinaryBuffer.length,
            rsrcLength: 0,
          };

          // Acknowledge upload to the sending 68k Mac immediately so its TCPRcv does not time out,
          // while streaming the MacBinary II payload down the receiving 68k Mac's channel!
          res.json({
            ok: true,
            relayed: true,
            message: `Relaying "${macInfo.fileName}" to ${target.name || target.ip}`,
            transferId,
          });

          console.log(`[MacDrop] Relaying "${macInfo.fileName}" (${macBinaryBuffer.length} B) from ${clientName} -> ${target.name} (${target.id})`);

          this.deliverToTarget(target, macBinaryBuffer, macInfo, transferId)
            .then(() => {
              console.log(`[MacDrop] Peer relay completed: "${macInfo.fileName}" delivered to ${target.name}`);
              const storeData = this.store.load();
              const transfers = Array.isArray(storeData.recentTransfers) ? [...storeData.recentTransfers] : [];
              transfers.unshift({
                id: transferId,
                fileName: macInfo.fileName,
                type: macInfo.type,
                creator: macInfo.creator,
                fileSize: (macInfo.dataLength || 0) + (macInfo.rsrcLength || 0),
                totalSent: macBinaryBuffer.length,
                targetName: `${clientName} → ${target.name || target.ip}`,
                targetIp: target.ip || 'N/A',
                timestamp: Date.now(),
                status: 'DELIVERED',
              });
              if (transfers.length > MAX_TRANSFER_LOGS) transfers.pop();
              this.store.save({ recentTransfers: transfers });
            })
            .catch((err) => {
              console.error(`[MacDrop] Peer relay to ${target.name} failed:`, err.message);
            })
            .finally(() => {
              setTimeout(() => {
                if (this.activeTransfer && this.activeTransfer.transferId === transferId) {
                  this.activeTransfer = null;
                }
              }, 3000);
            });
        });

        req.on('close', () => {
          if (!req.complete) {
            if (client) {
              client.transferInProgress = false;
              client.lastSeen = Date.now();
            }
            if (fd !== null) {
              try { fs.closeSync(fd); } catch (_) {}
              fd = null;
            }
            try { if (fs.existsSync(stagingPath)) fs.unlinkSync(stagingPath); } catch (_) {}
            if (this.activeTransfer && this.activeTransfer.transferId === transferId) {
              this.activeTransfer = null;
            }
          }
        });

        req.on('error', () => {
          if (client) {
            client.transferInProgress = false;
            client.lastSeen = Date.now();
          }
          if (fd !== null) {
            try { fs.closeSync(fd); } catch (_) {}
            fd = null;
          }
          try { if (fs.existsSync(stagingPath)) fs.unlinkSync(stagingPath); } catch (_) {}
        });
        return;
      }

      let totalReceived = 0;
      let headerBuffer = Buffer.alloc(0);
      let transferObj = null;

      let lastSpeedSampleTime = Date.now();
      let lastSpeedSampleBytes = 0;

      req.on('data', chunk => {
        const prevReceived = totalReceived;
        totalReceived += chunk.length;

        if (client) {
          client.transferInProgress = true;
          client.lastSeen = Date.now();
        }

        // Write directly to file descriptor for zero-delay disk caching
        if (fd !== null) {
          try {
            fs.writeSync(fd, chunk, 0, chunk.length, prevReceived);
          } catch (writeErr) {
            console.error(`[MacDrop] Staging write error for ${transferId}:`, writeErr);
          }
        }

        // Parse header as soon as first 128 bytes arrive so modern web UI gets alerted immediately!
        if (!transferObj) {
          headerBuffer = Buffer.concat([headerBuffer, chunk]);
          if (headerBuffer.length >= 128) {
            const parsedInfo = parseMacBinaryHeader(headerBuffer.subarray(0, 128));
            const rawName = (parsedInfo && parsedInfo.fileName) ? parsedInfo.fileName : (req.headers['x-filename'] || 'MacintoshFile');
            const cleanName = sanitizeMacFilename(rawName);
            const fileType = (parsedInfo && parsedInfo.type) ? parsedInfo.type : '????';
            const fileCreator = (parsedInfo && parsedInfo.creator) ? parsedInfo.creator : '????';
            const dataLength = parsedInfo ? parsedInfo.dataLength : 0;
            const rsrcLength = parsedInfo ? parsedInfo.rsrcLength : 0;

            // Direct data fork extraction for documents / graphics / audio (e.g. .pict, .txt, .jpg, .aif)
            const isDocType = ['PICT', 'TEXT', 'JPEG', 'GIFf', 'PNGf', 'BMP ', 'TIFF', 'Midi', 'AIFF', 'WAVE', 'Mp3 ', 'PDF '].includes(fileType);
            const isRawData = (rsrcLength === 0 || isDocType) && fileType !== 'APPL' && !cleanName.toLowerCase().endsWith('.bin');

            let downloadName = cleanName;
            if (isRawData) {
              if (!cleanName.includes('.') && TYPE_TO_EXT[fileType]) {
                downloadName = `${cleanName}.${TYPE_TO_EXT[fileType]}`;
              }
            } else {
              downloadName = cleanName.toLowerCase().endsWith('.bin') ? cleanName : `${cleanName}.bin`;
            }

            const downloadSize = isRawData ? dataLength : (128 + Math.ceil(dataLength / 128) * 128 + Math.ceil(rsrcLength / 128) * 128);
            const mimeType = getMimeType(downloadName, fileType);
            const expectedTotal = parseInt(req.headers['content-length'] || '0', 10) || downloadSize;

            const ev = new EventEmitter();
            ev.on('error', () => {}); // Prevent unhandled error throw if no active consumer

            transferObj = {
              id: transferId,
              clientId,
              clientName,
              clientRef: client,
              req,
              res,
              fileName: cleanName,
              downloadName,
              downloadSize,
              isRawData,
              mimeType,
              type: fileType,
              creator: fileCreator,
              dataLength,
              rsrcLength,
              totalExpected: expectedTotal,
              bytesReceived: totalReceived,
              speedBps: 0,
              speedFormatted: '0 kB/s',
              status: 'STREAMING',
              accepted: false,
              filePath: stagingPath,
              fd,
              events: ev,
              createdAt: Date.now(),
              lastProgressAt: Date.now(),
              expiresAt: null,
              timer: null,
            };

            this.inboundTransfers.set(transferId, transferObj);
            console.log(`[MacDrop] Inbound STREAMING alert: "${downloadName}" (${downloadSize} B) from ${clientName} [isRawData: ${isRawData}]`);
          }
        } else {
          transferObj.bytesReceived = totalReceived;
          const now = Date.now();
          const speedElapsed = (now - lastSpeedSampleTime) / 1000;
          if (speedElapsed >= 0.35) {
            const bytesDelta = totalReceived - lastSpeedSampleBytes;
            const currentBps = Math.round(bytesDelta / speedElapsed);
            transferObj.speedBps = currentBps;
            transferObj.speedFormatted = currentBps >= 1048576
              ? `${(currentBps / 1048576).toFixed(2)} MB/s`
              : `${(currentBps / 1024).toFixed(1)} kB/s`;
            lastSpeedSampleTime = now;
            lastSpeedSampleBytes = totalReceived;
          }
        }

        // Notify active downstream download stream in real time
        if (transferObj) {
          transferObj.events.emit('chunk', chunk, prevReceived, totalReceived);
        }
      });

      req.on('end', () => {
        if (client) {
          client.transferInProgress = false;
          client.lastSeen = Date.now();
        }

        if (fd !== null) {
          try { fs.closeSync(fd); } catch (_) {}
          fd = null;
        }

        if (!transferObj) {
          return res.json({ ok: false, error: 'Empty payload' });
        }

        transferObj.fd = null;
        const wasAccepted = Boolean(
          transferObj.accepted ||
          transferObj.status === 'DOWNLOADING' ||
          transferObj.status === 'COMPLETED'
        );
        transferObj.events.emit('end');

        console.log(`[MacDrop] Inbound upload stream completed: "${transferObj.downloadName}" (${totalReceived} B)`);

        if (wasAccepted || transferObj.accepted || transferObj.status === 'DOWNLOADING' || transferObj.status === 'COMPLETED') {
          console.log(`[MacDrop] Inbound transfer "${transferObj.downloadName}" already accepted (${transferObj.status}); skipping READY re-prompt.`);
        } else {
          transferObj.status = 'READY';
          transferObj.expiresAt = Date.now() + 30000;
          transferObj.timer = setTimeout(() => {
            this.cleanupInboundTransfer(transferId, 'TIMED_OUT_30S');
          }, 30000);
        }

        res.json({
          ok: true,
          message: 'File staged on server',
          transferId,
          fileName: transferObj.downloadName,
          size: totalReceived,
        });
      });

      req.on('close', () => {
        if (!req.complete) {
          console.log(`[MacDrop] Inbound upload aborted by client: "${transferObj ? transferObj.downloadName : transferId}"`);
          if (client) {
            client.transferInProgress = false;
            client.lastSeen = Date.now();
          }
          this.cleanupInboundTransfer(transferId, 'ABORTED_BY_CLIENT');
        }
      });

      req.on('error', err => {
        if (client) {
          client.transferInProgress = false;
          client.lastSeen = Date.now();
        }
        if (fd !== null) {
          try { fs.closeSync(fd); } catch (_) {}
          fd = null;
        }
        if (transferObj && transferObj.events.listenerCount('error') > 1) {
          try { transferObj.events.emit('error', err); } catch (_) {}
        }
        this.cleanupInboundTransfer(transferId, 'STREAM_ERROR');
        if (!res.headersSent && !res.writableEnded && !res.destroyed) {
          try { res.status(500).json({ ok: false, error: err.message }); } catch (_) {}
        }
      });
    });

    // - Query pending/staged inbound transfers
    this.router.get('/inbound/pending', (req, res) => {
      this.sweepDeadClients();
      const now = Date.now();
      const pending = Array.from(this.inboundTransfers.values()).map(t => ({
        id: t.id,
        clientName: t.clientName,
        clientId: t.clientId,
        fileName: t.fileName,
        downloadName: t.downloadName,
        fileSize: t.totalExpected || t.bytesReceived,
        downloadSize: t.downloadSize || t.dataLength || t.bytesReceived,
        isRawData: Boolean(t.isRawData),
        type: t.type,
        creator: t.creator,
        dataLength: t.dataLength,
        rsrcLength: t.rsrcLength,
        bytesReceived: t.bytesReceived,
        percent: t.status === 'COMPLETED' ? 100 : (t.totalExpected > 0 ? Math.min(99, Math.round((t.bytesReceived / t.totalExpected) * 100)) : 0),
        speedFormatted: t.status === 'COMPLETED' ? 'Complete' : t.speedFormatted,
        status: t.status,
        accepted: Boolean(t.accepted),
        expiresAt: t.expiresAt,
        remainingSec: t.expiresAt ? Math.max(0, Math.ceil((t.expiresAt - now) / 1000)) : null,
      }));
      res.json({ ok: true, inbound: pending });
    });

    // - Download staged file directly into modern browser (supports simultaneous pass-through streaming!)
    this.router.get('/inbound/download/:transferId', (req, res) => {
      const { transferId } = req.params;
      const transfer = this.inboundTransfers.get(transferId);

      if (!transfer || !transfer.filePath || !fs.existsSync(transfer.filePath)) {
        return res.status(404).send('File not found or transfer expired.');
      }

      // Mark transfer as accepted immediately so req.on('end') never re-prompts with a 30s READY timer
      transfer.accepted = true;
      transfer.expiresAt = null;
      if (transfer.timer) {
        clearTimeout(transfer.timer);
        transfer.timer = null;
      }

      res.setHeader('Content-Type', transfer.mimeType || 'application/octet-stream');
      res.setHeader('Content-Disposition', `attachment; filename="${encodeURIComponent(transfer.downloadName)}"`);
      if (transfer.downloadSize > 0) {
        res.setHeader('Content-Length', transfer.downloadSize);
      }

      // Helper to send slice (data fork only if raw data)
      const sendChunk = (chunk, chunkStart, chunkEnd) => {
        if (res.writableEnded || res.destroyed) return;
        if (!transfer.isRawData) {
          res.write(chunk);
        } else {
          const dataStart = 128;
          const dataEnd = 128 + transfer.dataLength;
          if (chunkEnd > dataStart && chunkStart < dataEnd) {
            const sliceFrom = Math.max(0, dataStart - chunkStart);
            const sliceTo = Math.min(chunk.length, dataEnd - chunkStart);
            if (sliceTo > sliceFrom) {
              res.write(chunk.subarray(sliceFrom, sliceTo));
            }
          }
        }
      };

      const markDownloadCompleted = (reason) => {
        transfer.status = 'COMPLETED';
        transfer.accepted = true;
        transfer.expiresAt = null;
        if (transfer.timer) {
          clearTimeout(transfer.timer);
          transfer.timer = null;
        }
        setTimeout(() => {
          this.cleanupInboundTransfer(transferId, reason);
        }, 4500);
      };

      // Mode A: File has ALREADY finished uploading to server
      if (transfer.status === 'READY' || transfer.status === 'COMPLETED' || !transfer.fd) {
        transfer.status = 'DOWNLOADING';
        let readStream;
        if (transfer.isRawData) {
          const endPos = 128 + transfer.dataLength - 1;
          readStream = fs.createReadStream(transfer.filePath, { start: 128, end: endPos });
        } else {
          readStream = fs.createReadStream(transfer.filePath);
        }

        readStream.pipe(res);

        readStream.on('end', () => {
          markDownloadCompleted('DOWNLOAD_COMPLETED');
        });

        readStream.on('error', (err) => {
          console.error(`[MacDrop] Inbound download streaming error for ${transferId}:`, err);
          this.cleanupInboundTransfer(transferId, 'STREAM_ERROR');
        });
        return;
      }

      // Mode B: File is CURRENTLY STREAMING from vintage Mac (Simultaneous Pipe!)
      transfer.status = 'DOWNLOADING';
      const snapshotReceived = transfer.bytesReceived;
      let catchupQueue = [];
      let isCatchingUp = true;

      const onChunk = (chunk, start, end) => {
        if (isCatchingUp) {
          catchupQueue.push({ chunk: Buffer.from(chunk), start, end });
        } else {
          sendChunk(chunk, start, end);
        }
      };

      const onEnd = () => {
        if (!isCatchingUp) {
          try { res.end(); } catch (_) {}
          markDownloadCompleted('SIMULTANEOUS_DOWNLOAD_COMPLETED');
        }
      };

      transfer.events.on('chunk', onChunk);
      transfer.events.on('end', onEnd);

      res.on('close', () => {
        transfer.events.off('chunk', onChunk);
        transfer.events.off('end', onEnd);
      });

      // Stream catch-up data from disk in 64KB blocks (allocating fresh buffer per block to prevent async write corruption)
      if (snapshotReceived > 0 && fs.existsSync(transfer.filePath)) {
        try {
          const readFd = fs.openSync(transfer.filePath, 'r');
          const BLOCK_SIZE = 65536;
          let pos = 0;
          while (pos < snapshotReceived) {
            const toRead = Math.min(BLOCK_SIZE, snapshotReceived - pos);
            const blkBuf = Buffer.allocUnsafe(toRead);
            const n = fs.readSync(readFd, blkBuf, 0, toRead, pos);
            if (n <= 0) break;
            sendChunk(blkBuf.subarray(0, n), pos, pos + n);
            pos += n;
          }
          fs.closeSync(readFd);
        } catch (readErr) {
          console.error(`[MacDrop] Catch-up read error for ${transferId}:`, readErr);
        }
      }

      // Flush queue accumulated during disk catch-up read
      isCatchingUp = false;
      for (const item of catchupQueue) {
        sendChunk(item.chunk, item.start, item.end);
      }
      catchupQueue = [];

      // If upload finished while reading catch-up buffer
      if (transfer.bytesReceived >= transfer.totalExpected && (transfer.status === 'READY' || transfer.status === 'COMPLETED' || !transfer.fd)) {
        try { res.end(); } catch (_) {}
        markDownloadCompleted('SIMULTANEOUS_DOWNLOAD_COMPLETED');
      }
    });

    // - Decline staged inbound transfer & delete file immediately
    this.router.post('/inbound/decline/:transferId', (req, res) => {
      const { transferId } = req.params;
      this.cleanupInboundTransfer(transferId, 'DECLINED_BY_USER');
      res.json({ ok: true, message: 'Transfer declined and staged file deleted' });
    });

    // - Cancel staged/streaming inbound transfer & abort streams immediately
    this.router.post('/inbound/cancel/:transferId', (req, res) => {
      const { transferId } = req.params;
      console.log(`[MacDrop] Inbound transfer cancel requested for ${transferId}`);
      this.cleanupInboundTransfer(transferId, 'CANCELLED_BY_USER');
      res.json({ ok: true, message: 'Transfer cancelled and staged file deleted' });
    });

    // - Cancel outbound transfer to 68k Mac & abort reverse stream immediately
    this.router.post('/cancel', (req, res) => {
      console.log('[MacDrop] Outbound transfer cancel requested by user');
      if (this.activeTransfer) {
        this.activeTransfer.status = 'CANCELLED';
        this.activeTransfer.error = 'Transfer cancelled by user';
        this.activeTransfer.speedFormatted = 'Cancelled';
      }

      for (const [xferId, pending] of this.pendingTransfers.entries()) {
        try {
          pending.reject(new Error('Transfer cancelled by user'));
        } catch (_) {}
      }
      this.pendingTransfers.clear();

      for (const c of this.activeClients.values()) {
        if (c.transferInProgress || c.writingDisk) {
          c.transferInProgress = false;
          c.writingDisk = false;
          c.lastSeen = Date.now();
          if (c.res && !c.res.writableEnded && !c.res.destroyed) {
            try {
              c.res.destroy(new Error('Transfer cancelled by user'));
            } catch (_) {}
          }
        }
      }

      setTimeout(() => {
        if (this.activeTransfer && this.activeTransfer.status === 'CANCELLED') {
          this.activeTransfer = null;
        }
      }, 3000);

      res.json({ ok: true, message: 'Outbound transfer cancelled' });
    });

    // 4. Select Target Client ('auto', 'all', or specific clientId)
    this.router.post('/select-target', (req, res) => {
      const { targetId } = req.body || {};
      if (!targetId) {
        return res.status(400).json({ ok: false, error: 'Missing targetId' });
      }

      this.selectedTargetId = targetId;
      this.store.save({ selectedTargetId: targetId });
      res.json({ ok: true, selectedTargetId: this.selectedTargetId });
    });

    // 5. Manual target setup (fallback for physical networks)
    this.router.post('/target', async (req, res) => {
      const { ip, port, name } = req.body || {};
      if (!ip) {
        return res.status(400).json({ ok: false, error: 'Missing target IP address' });
      }

      const cleanIp = ip.trim();
      const numPort = parseInt(port || DEFAULT_MACDROP_PORT, 10);

      // Check if client is already known or connected live to read its name directly
      let activeClient = Array.from(this.activeClients.values()).find(c => c.ip === cleanIp);

      // Test ping before saving (or if already connected live via reverse channel, it's online)
      // Note: pingTarget sends MACDROP_HELLO|<serverIp>|<httpPort>, which also triggers
      // bridged 68k clients (v0.2.7+) to reply with MACDROP_PONG and connect their reverse channel!
      let isOnline = Boolean(activeClient);
      if (!isOnline) {
        isOnline = await this.pingTarget(cleanIp, numPort, 2500);
        this.pingCache.set(`${cleanIp}:${numPort}`, { online: isOnline, checkedAt: Date.now() });
      }

      if (!isOnline) {
        return res.status(400).json({
          ok: false,
          error: `Ping timed out to ${cleanIp}:${numPort}. Device did not respond to MacDrop pings.`,
        });
      }

      // Give a brief window for MACDROP_HELLO auto-pairing to establish reverse channel if not yet connected
      if (!activeClient) {
        await new Promise(r => setTimeout(r, 250));
        activeClient = Array.from(this.activeClients.values()).find(c => c.ip === cleanIp);
      }

      const data = this.store.load();
      const known = (data.knownDevices || []).find(d => d.ip === cleanIp);
      const pongMeta = this.lastPongByIp.get(cleanIp);

      let targetName = (name || '').trim();
      let targetMachine = 'Vintage Macintosh';
      let targetVersion = '';

      if (activeClient) {
        targetName = activeClient.name;
        targetMachine = activeClient.machine;
        targetVersion = activeClient.version;
      } else if (pongMeta && pongMeta.name) {
        targetName = pongMeta.name;
        targetMachine = pongMeta.machine || targetMachine;
        targetVersion = pongMeta.version || targetVersion;
      } else if (known && known.name) {
        targetName = known.name;
        targetMachine = known.machine || targetMachine;
        targetVersion = known.version || targetVersion;
      }

      if (!targetName) {
        targetName = `Macintosh (${cleanIp})`;
      }

      const targets = Array.isArray(data.targets) ? [...data.targets] : [];

      const targetObj = {
        id: `manual_${cleanIp.replace(/\./g, '_')}_${numPort}`,
        ip: cleanIp,
        port: numPort,
        name: targetName,
        machine: targetMachine,
        version: targetVersion,
        lastSeen: Date.now(),
        online: true,
        source: 'manual',
      };

      const existingIdx = targets.findIndex(t => t.ip === targetObj.ip);
      if (existingIdx >= 0) {
        targets[existingIdx] = { ...targets[existingIdx], ...targetObj };
      } else {
        targets.unshift(targetObj);
      }

      this.selectedTargetId = targetObj.id;
      this.store.save({
        targets,
        activeTarget: targetObj,
        selectedTargetId: targetObj.id,
      });

      this.recordKnownDevice(targetObj);

      res.json({ ok: true, activeTarget: targetObj, online: isOnline });
    });

    // Remove a saved/known target
    this.router.delete('/target/:id', (req, res) => {
      const targetId = req.params.id;
      const data = this.store.load();
      const knownDevices = (data.knownDevices || []).filter(d => d.id !== targetId && d.ip !== targetId);
      const targets = (data.targets || []).filter(t => t.id !== targetId && t.ip !== targetId);
      this.store.save({ knownDevices, targets });
      res.json({ ok: true });
    });

    this.router.post('/target/remove', (req, res) => {
      const targetId = req.body?.id || req.query?.id;
      const targetIp = req.body?.ip || req.query?.ip;
      const data = this.store.load();
      const knownDevices = (data.knownDevices || []).filter(d =>
        d.id !== targetId && d.ip !== targetId && (!targetIp || d.ip !== targetIp)
      );
      const targets = (data.targets || []).filter(t =>
        t.id !== targetId && t.ip !== targetId && (!targetIp || t.ip !== targetIp)
      );
      this.store.save({ knownDevices, targets });
      res.json({ ok: true });
    });

    // 6. Test Ping (tests both reverse channel and direct TCP)
    this.router.get('/ping-target', async (req, res) => {
      this.sweepDeadClients();
      const targetId = req.query.id || this.selectedTargetId;

      if (targetId && this.activeClients.has(targetId)) {
        const client = this.activeClients.get(targetId);
        return res.json({
          ok: true,
          message: `Active Reverse Channel to ${client.name} (${client.ip}) is Online & Ready!`,
          target: { name: client.name, ip: client.ip, mode: 'CHANNEL', online: true },
        });
      }

      const activeList = Array.from(this.activeClients.values());
      if (activeList.length > 0) {
        const client = activeList[0];
        return res.json({
          ok: true,
          message: `Reverse Channel connected to ${client.name} (${client.ip})!`,
          target: { name: client.name, ip: client.ip, mode: 'CHANNEL', online: true },
        });
      }

      const data = this.store.load();
      const manualTarget = data.activeTarget;
      if (!manualTarget || !manualTarget.ip) {
        return res.status(404).json({
          ok: false,
          error: 'No Macintosh clients connected. Run MacDrop on your 68k Mac to connect automatically.',
        });
      }

      const isOnline = await this.pingTarget(manualTarget.ip, manualTarget.port || DEFAULT_MACDROP_PORT);
      this.pingCache.set(`${manualTarget.ip}:${manualTarget.port || DEFAULT_MACDROP_PORT}`, { online: isOnline, checkedAt: Date.now() });

      if (isOnline) {
        res.json({
          ok: true,
          message: `Connected to ${manualTarget.name} (${manualTarget.ip}:${manualTarget.port}) via direct TCP!`,
          target: { ...manualTarget, online: true },
        });
      } else {
        res.status(504).json({
          ok: false,
          error: `Connection timed out to ${manualTarget.ip}:${manualTarget.port || DEFAULT_MACDROP_PORT}`,
        });
      }
    });

    // 7. Ingestion Endpoint: Upload & Push File to Selected 68k Mac(s)
    this.router.post('/upload', async (req, res) => {
      try {
        this.sweepDeadClients();

        let fileBuffer = null;
        let originalName = '';

        const ctype = req.headers['content-type'] || '';
        if (ctype.includes('multipart/form-data')) {
          const parsed = this.parseMultipart(req.rawBuffer, ctype);
          if (parsed) {
            fileBuffer = parsed.data;
            originalName = parsed.filename;
          }
        } else if (req.rawBuffer && req.rawBuffer.length > 0) {
          fileBuffer = req.rawBuffer;
          let rawHeaderName = req.headers['x-filename'] || req.query.filename || 'PushedFile.bin';
          try {
            rawHeaderName = decodeURIComponent(rawHeaderName);
          } catch (_) {}
          originalName = rawHeaderName;
        }

        if (!fileBuffer || fileBuffer.length === 0) {
          return res.status(400).json({ ok: false, error: 'No file payload received' });
        }

        // Prepare MacBinary II payload
        let macBinaryBuffer = null;
        let macInfo = null;

        if (isMacBinary(fileBuffer)) {
          macBinaryBuffer = fileBuffer;
          macInfo = parseMacBinaryHeader(fileBuffer);
        } else {
          const sanitizedName = sanitizeMacFilename(originalName);
          const typeCreator = getTypeCreatorForFilename(sanitizedName);
          const customType = req.headers['x-file-type'] || req.query.type;
          const customCreator = req.headers['x-file-creator'] || req.query.creator;

          macBinaryBuffer = createMacBinary({
            fileName: sanitizedName,
            dataBuffer: fileBuffer,
            fileType: customType || typeCreator.type,
            fileCreator: customCreator || typeCreator.creator,
          });
          macInfo = parseMacBinaryHeader(macBinaryBuffer);
        }

        // Determine destination client(s)
        const targetQuery = req.query.target_id || req.headers['x-target-id'] || this.selectedTargetId;
        const targetsToPush = this.resolveTargets(targetQuery);

        if (targetsToPush.length === 0) {
          return res.status(400).json({
            ok: false,
            error: 'No target Macintosh is currently online and ready. Launch MacDrop on your vintage Mac to receive files.',
          });
        }

        const transferId = 'xfer_' + Date.now().toString(36);
        const primaryTarget = targetsToPush[0];

        this.activeTransfer = {
          transferId,
          fileName: macInfo.fileName,
          targetName: targetsToPush.length > 1 ? `All Macs (${targetsToPush.length})` : (primaryTarget.name || primaryTarget.ip),
          totalBytes: macBinaryBuffer.length,
          bytesSent: 0,
          percent: 0,
          speedBps: 0,
          speedFormatted: '0 kB/s',
          status: 'STREAMING',
          startedAt: Date.now(),
          lastProgressAt: Date.now(),
        };

        const pushResults = [];

        for (const target of targetsToPush) {
          try {
            await this.deliverToTarget(target, macBinaryBuffer, macInfo, transferId);
            pushResults.push({ target: target.name || target.ip, status: 'OK' });
          } catch (err) {
            console.error(`[MacDrop] Push to ${target.name || target.ip} failed:`, err.message);
            pushResults.push({ target: target.name || target.ip, status: 'ERROR', error: err.message });
          }
        }

        const anySuccess = pushResults.some(r => r.status === 'OK');
        if (anySuccess) {
          if (this.activeTransfer) {
            this.activeTransfer.percent = 100;
            this.activeTransfer.status = 'DELIVERED';
            this.activeTransfer.speedFormatted = 'Complete';
          }
        } else {
          if (this.activeTransfer) {
            this.activeTransfer.status = 'FAILED';
            this.activeTransfer.error = pushResults[0] ? pushResults[0].error : 'Transfer failed';
          }
        }

        setTimeout(() => {
          if (this.activeTransfer && this.activeTransfer.transferId === transferId) {
            this.activeTransfer = null;
          }
        }, 3500);

        // Record in transfer history
        const data = this.store.load();
        const transfers = Array.isArray(data.recentTransfers) ? [...data.recentTransfers] : [];
        const transferRecord = {
          id: transferId,
          fileName: macInfo.fileName,
          type: macInfo.type,
          creator: macInfo.creator,
          fileSize: macInfo.dataLength + macInfo.rsrcLength,
          totalSent: macBinaryBuffer.length,
          targetName: targetsToPush.length > 1 ? `All Macs (${targetsToPush.length})` : (primaryTarget.name || primaryTarget.ip),
          targetIp: primaryTarget.ip || 'N/A',
          timestamp: Date.now(),
          status: anySuccess ? 'DELIVERED' : 'FAILED',
        };

        transfers.unshift(transferRecord);
        if (transfers.length > MAX_TRANSFER_LOGS) transfers.pop();
        this.store.save({ recentTransfers: transfers });

        if (!anySuccess) {
          const firstErr = pushResults[0] ? pushResults[0].error : 'Client failed to receive file';
          return res.status(502).json({
            ok: false,
            error: `Push to Macintosh failed: ${firstErr}`,
            results: pushResults,
          });
        }

        res.json({
          ok: true,
          message: `Successfully delivered "${macInfo.fileName}" to ${transferRecord.targetName}`,
          transfer: transferRecord,
          results: pushResults,
        });
      } catch (err) {
        console.error('[MacDrop] Push failed:', err);
        res.status(502).json({
          ok: false,
          error: `Push to Macintosh failed: ${err.message}`,
        });
      }
    });

    // 8. Recent Transfers (optional API)
    this.router.get('/recent', (req, res) => {
      const data = this.store.load();
      res.json({
        ok: true,
        recentTransfers: data.recentTransfers || [],
      });
    });
  }

  /**
   * Resolves target objects to push to. Returns only targets that are online.
   */
  resolveTargets(targetQuery) {
    this.sweepDeadClients();
    const activeList = Array.from(this.activeClients.values());
    const data = this.store.load();

    if (targetQuery === 'all') {
      return activeList.length > 0 ? activeList : [];
    }

    if (targetQuery && targetQuery !== 'auto') {
      // 1. Check active reverse-channel clients by id, ip, device_<ip>, or manual_<ip>_<port>
      for (const c of activeList) {
        const cleanIp = (c.ip || '').trim();
        const ipSlug = cleanIp.replace(/\./g, '_');
        if (c.id === targetQuery ||
            cleanIp === targetQuery ||
            `device_${ipSlug}` === targetQuery ||
            targetQuery.startsWith(`manual_${ipSlug}_`)) {
          return [c];
        }
      }

      // 2. Check saved manual targets & knownDevices
      const allSaved = [...(data.targets || []), ...(data.knownDevices || [])];
      const saved = allSaved.find(t => {
        if (!t || !t.ip) return false;
        const cleanIp = t.ip.trim();
        const ipSlug = cleanIp.replace(/\./g, '_');
        return t.id === targetQuery ||
               cleanIp === targetQuery ||
               `device_${ipSlug}` === targetQuery ||
               targetQuery.startsWith(`manual_${ipSlug}_`);
      });

      if (saved && saved.ip) {
        const cleanIp = saved.ip.trim();
        // Check if an active reverse channel exists for this IP
        const activeByIp = activeList.find(c => c.ip === cleanIp);
        if (activeByIp) {
          return [activeByIp];
        }
        const isOnline = Boolean(this.pingCache.get(`${cleanIp}:${saved.port || DEFAULT_MACDROP_PORT}`)?.online);
        if (isOnline) {
          return [{ ...saved, ip: cleanIp, mode: 'DIRECT' }];
        }
      }

      // 3. Fallback: if the stored selectedTargetId is offline/stale, fall back to any online client
      if (targetQuery === this.selectedTargetId && activeList.length > 0) {
        return [activeList[0]];
      }
      return [];
    }

    // 'auto' mode: prefer active reverse channel client
    if (activeList.length > 0) {
      return [activeList[0]];
    }

    // fallback to saved manual target or any known device IF online in pingCache
    if (data.activeTarget && data.activeTarget.ip) {
      const saved = data.activeTarget;
      const isOnline = Boolean(this.pingCache.get(`${saved.ip}:${saved.port || DEFAULT_MACDROP_PORT}`)?.online);
      if (isOnline) {
        return [{ ...saved, mode: 'DIRECT' }];
      }
    }

    for (const t of [...(data.targets || []), ...(data.knownDevices || [])]) {
      if (t && t.ip) {
        const isOnline = Boolean(this.pingCache.get(`${t.ip}:${t.port || DEFAULT_MACDROP_PORT}`)?.online);
        if (isOnline) {
          return [{ ...t, mode: 'DIRECT' }];
        }
      }
    }

    return [];
  }

  /**
   * Delivers MacBinary payload to target (via reverse channel or direct TCP),
   * and awaits receipt ACK from the vintage Mac client.
   */
  deliverToTarget(target, buffer, macInfo, transferId) {
    return new Promise(async (resolve, reject) => {
      // 1. If target is connected via Reverse HTTP Channel
      if (target.mode === 'CHANNEL') {
        // If client is currently reconnecting from a poll/scan, wait up to 2.5s for its new socket
        if (!target.res || target.res.writableEnded || target.res.destroyed) {
          const reconnected = await this.waitForClientChannel(target.id, 2500);
          if (!reconnected) {
            return reject(new Error('Target client is offline'));
          }
          target = this.activeClients.get(target.id);
        }

        if (!target || !target.res || target.res.writableEnded || target.res.destroyed) {
          return reject(new Error('Target client reverse channel is unavailable'));
        }

        try {
          const clientRes = target.res;
          target.transferInProgress = true;
          target.lastSeen = Date.now();

          clientRes.shouldKeepAlive = false;
          if (clientRes.socket && typeof clientRes.socket.setNoDelay === 'function') {
            clientRes.socket.setNoDelay(true);
          }

          clientRes.writeHead(200, {
            'Content-Type': 'application/x-macbinary',
            'Content-Length': buffer.length,
            'Connection': 'close',
            'X-MacDrop-Status': 'STREAM',
            'X-Filename': encodeURIComponent(macInfo.fileName),
            'X-Transfer-Id': transferId,
          });

          const CHUNK_SIZE = 32768; // 32 KB chunk streaming for smooth progress & low socket buffer inflation
          let offset = 0;
          let firstDrainOffset = 0;
          let firstDrainAt = 0;
          let trueSpeedBps = 0;
          let lastSpeedSampleTime = Date.now();
          let lastSpeedSampleBytes = 0;

          while (offset < buffer.length) {
            if (clientRes.destroyed || (clientRes.socket && clientRes.socket.destroyed)) {
              throw new Error('Connection aborted by Macintosh client during file transfer');
            }

            const chunkEnd = Math.min(offset + CHUNK_SIZE, buffer.length);
            const chunk = buffer.subarray(offset, chunkEnd);
            const ok = clientRes.write(chunk);
            offset = chunkEnd;

            if (!ok && firstDrainOffset === 0) {
              firstDrainOffset = offset;
              firstDrainAt = Date.now();
              lastSpeedSampleTime = firstDrainAt;
              lastSpeedSampleBytes = offset;
            }

            target.lastSeen = Date.now();
            const now = Date.now();

            if (this.activeTransfer) {
              // Account for Docker/OrbStack kernel TCP send buffer (~4MB) so the Web UI
              // reports true bytes received by the 68k Mac rather than jumping ahead and
              // waiting 7s at "Finalizing on Macintosh..." while the kernel drains.
              const kernelBuffered = firstDrainOffset > 0 ? firstDrainOffset : 0;
              const estimatedMacBytes = firstDrainOffset > 0
                ? Math.max(Math.min(offset, CHUNK_SIZE), offset - kernelBuffered)
                : offset;

              this.activeTransfer.bytesSent = estimatedMacBytes;
              this.activeTransfer.percent = Math.min(95, Math.round((estimatedMacBytes / buffer.length) * 95));
              this.activeTransfer.lastProgressAt = now;

              const speedElapsed = (now - lastSpeedSampleTime) / 1000;
              if (speedElapsed >= 0.35) {
                const bytesDelta = offset - lastSpeedSampleBytes;
                const currentBps = Math.round(bytesDelta / speedElapsed);
                if (firstDrainAt > 0 && (now - firstDrainAt) >= 250) {
                  trueSpeedBps = Math.round((offset - firstDrainOffset) / ((now - firstDrainAt) / 1000));
                }
                const displayBps = trueSpeedBps > 0 ? trueSpeedBps : currentBps;
                this.activeTransfer.speedBps = displayBps;
                if (displayBps >= 1048576) {
                  this.activeTransfer.speedFormatted = `${(displayBps / 1048576).toFixed(2)} MB/s`;
                } else {
                  this.activeTransfer.speedFormatted = `${(displayBps / 1024).toFixed(1)} kB/s`;
                }
                lastSpeedSampleTime = now;
                lastSpeedSampleBytes = offset;
              }
            }

            if (!ok) {
              await new Promise((resolveDrain, rejectDrain) => {
                let drainTimer = setTimeout(() => {
                  cleanup();
                  rejectDrain(new Error('Transfer stalled: Macintosh client stopped accepting data for 30s'));
                }, 30000);

                const onDrain = () => {
                  cleanup();
                  resolveDrain();
                };
                const onError = (e) => {
                  cleanup();
                  rejectDrain(new Error(`Network error during transfer: ${e.message || e}`));
                };
                const onClose = () => {
                  cleanup();
                  rejectDrain(new Error('Macintosh client closed connection during file streaming'));
                };
                const cleanup = () => {
                  clearTimeout(drainTimer);
                  clientRes.removeListener('drain', onDrain);
                  clientRes.removeListener('error', onError);
                  clientRes.removeListener('close', onClose);
                };

                clientRes.once('drain', onDrain);
                clientRes.once('error', onError);
                clientRes.once('close', onClose);
              });
            }
          }

          // Mark disk-write phase before clientRes.end() so socket close events
          // are recognized as normal end-of-stream rather than mid-stream abort.
          target.transferInProgress = false;
          target.writingDisk = true;
          target.lastSeen = Date.now();

          if (this.activeTransfer) {
            const kernelBuffered = firstDrainOffset > 0 ? Math.min(firstDrainOffset, buffer.length) : 0;
            const estimatedMacBytes = Math.max(0, buffer.length - kernelBuffered);
            if (kernelBuffered > 65536 && estimatedMacBytes < buffer.length) {
              // Kernel TCP buffer still holds ~kernelBuffered bytes draining to the 68k Mac.
              // Keep status = 'STREAMING' in the Web UI and let /api/macdrop/progress advance smoothly
              // at the measured wire speed until all bytes reach the Mac!
              this.activeTransfer.bytesSent = estimatedMacBytes;
              this.activeTransfer.percent = Math.min(95, Math.round((estimatedMacBytes / buffer.length) * 95));
              this.activeTransfer.status = 'STREAMING';
              this.activeTransfer.drainStartedAt = Date.now();
              this.activeTransfer.drainStartBytes = estimatedMacBytes;
              this.activeTransfer.drainSpeedBps = trueSpeedBps > 0 ? trueSpeedBps : 750000;
              this.activeTransfer.lastProgressAt = Date.now();
            } else {
              this.activeTransfer.bytesSent = buffer.length;
              this.activeTransfer.percent = 97;
              this.activeTransfer.writingDiskStartPct = 97;
              this.activeTransfer.status = 'WRITING_DISK';
              this.activeTransfer.writingDiskStartedAt = Date.now();
              this.activeTransfer.speedFormatted = 'Finalizing on Macintosh...';
              this.activeTransfer.lastProgressAt = Date.now();
            }
          }

          // Complete HTTP stream and send TCP FIN immediately (Connection: close)
          await new Promise((resolveEnd) => {
            clientRes.end(resolveEnd);
          });

          // Dynamic timeout for disk write based on file size:
          // Base 45s + 1s per 3MB (e.g. 200MB gets 45 + 66 = 111s)
          const diskWriteTimeoutMs = Math.max(45000, 45000 + Math.ceil(buffer.length / (3 * 1024 * 1024)) * 1000);
          const ack = await this.waitForTransferAck(transferId, diskWriteTimeoutMs, target.id);

          target.writingDisk = false;
          target.lastSeen = Date.now();

          if (this.activeTransfer) {
            this.activeTransfer.percent = 100;
            this.activeTransfer.status = 'DELIVERED';
            this.activeTransfer.speedFormatted = 'Complete';
          }

          return resolve({ ok: true, bytesSent: buffer.length, confirmedBytes: ack.bytes });
        } catch (err) {
          if (target) {
            target.transferInProgress = false;
            target.writingDisk = false;
          }
          if (this.activeTransfer) {
            this.activeTransfer.status = 'FAILED';
            this.activeTransfer.error = err.message;
          }
          return reject(err);
        }
      }

      // 2. Direct TCP push fallback (streams in 64 KB chunks with real-time progress)
      if (target.ip) {
        const socket = new net.Socket();
        socket.setTimeout(Math.max(TCP_CONNECT_TIMEOUT_MS, 45000));
        socket.setNoDelay(true);
        let ackReceived = false;

        socket.on('connect', async () => {
          try {
            const CHUNK_SIZE = 65536;
            let offset = 0;
            let lastSpeedSampleTime = Date.now();
            let lastSpeedSampleBytes = 0;

            while (offset < buffer.length) {
              if (socket.destroyed) break;
              const chunkEnd = Math.min(offset + CHUNK_SIZE, buffer.length);
              const chunk = buffer.subarray(offset, chunkEnd);
              const ok = socket.write(chunk);
              offset = chunkEnd;

              const now = Date.now();
              if (this.activeTransfer) {
                this.activeTransfer.bytesSent = offset;
                this.activeTransfer.percent = Math.min(88, Math.round((offset / buffer.length) * 88));
                this.activeTransfer.lastProgressAt = now;

                const speedElapsed = (now - lastSpeedSampleTime) / 1000;
                if (speedElapsed >= 0.4) {
                  const bytesDelta = offset - lastSpeedSampleBytes;
                  const currentBps = Math.round(bytesDelta / speedElapsed);
                  this.activeTransfer.speedBps = currentBps;
                  this.activeTransfer.speedFormatted = currentBps >= 1048576
                    ? `${(currentBps / 1048576).toFixed(2)} MB/s`
                    : `${(currentBps / 1024).toFixed(1)} kB/s`;
                  lastSpeedSampleTime = now;
                  lastSpeedSampleBytes = offset;
                }
              }

              if (!ok) {
                await new Promise((resolveDrain, rejectDrain) => {
                  const onDrain = () => { cleanup(); resolveDrain(); };
                  const onErr = (e) => { cleanup(); rejectDrain(e); };
                  const onClose = () => { cleanup(); rejectDrain(new Error('Connection closed during transfer')); };
                  const cleanup = () => {
                    socket.removeListener('drain', onDrain);
                    socket.removeListener('error', onErr);
                    socket.removeListener('close', onClose);
                  };
                  socket.once('drain', onDrain);
                  socket.once('error', onErr);
                  socket.once('close', onClose);
                });
              }
            }

            if (this.activeTransfer) {
              this.activeTransfer.bytesSent = buffer.length;
              this.activeTransfer.percent = 89;
              this.activeTransfer.status = 'WRITING_DISK';
              this.activeTransfer.writingDiskStartedAt = Date.now();
              this.activeTransfer.speedFormatted = 'Finalizing on Macintosh...';
              this.activeTransfer.lastProgressAt = Date.now();
            }
          } catch (writeErr) {
            if (!ackReceived) {
              socket.destroy();
              reject(writeErr);
            }
          }
        });

        socket.on('data', (chunk) => {
          const text = chunk.toString('utf8');
          if (text.includes('MACDROP_ACK')) {
            ackReceived = true;
            if (this.activeTransfer) {
              this.activeTransfer.percent = 100;
              this.activeTransfer.status = 'DELIVERED';
              this.activeTransfer.speedFormatted = 'Complete';
            }
            socket.destroy();
            resolve({ ok: true, bytesSent: buffer.length });
          }
        });

        socket.on('timeout', () => {
          socket.destroy();
          reject(new Error(`Timed out communicating with ${target.ip}:${target.port || DEFAULT_MACDROP_PORT}`));
        });

        socket.on('close', () => {
          if (!ackReceived) {
            reject(new Error('Connection closed before Macintosh client acknowledged transfer'));
          }
        });

        socket.on('error', (err) => {
          socket.destroy();
          reject(new Error(`Network error (${err.code || err.message})`));
        });

        socket.connect(target.port || DEFAULT_MACDROP_PORT, target.ip);
        return;
      }

      reject(new Error('Target client is offline'));
    });
  }

  /**
   * Scans the local /24 LAN subnet for 68k Macintosh clients listening on port 6803
   * (e.g. sitting behind a WiFi bridge where UDP broadcast is blocked).
   */
  async scanSubnetForBridgedClients() {
    try {
      const now = Date.now();
      if (this.lastSubnetScanAt && (now - this.lastSubnetScanAt < 8000)) {
        return;
      }
      this.lastSubnetScanAt = now;

      const prefixes = new Set();
      const hostIp = getLocalIpAddress();
      const addPrefixFromIp = (ipStr) => {
        if (!ipStr || typeof ipStr !== 'string') return;
        const m = ipStr.trim().match(/^(\d{1,3}\.\d{1,3}\.\d{1,3})\.\d{1,3}$/);
        if (!m) return;
        const p = m[1];
        if (p === '127.0.0' || p === '0.0.0' || p === '10.0.2' ||
            p === '192.168.97' || p === '192.168.65' ||
            /^172\.(1[7-9]|2\d|3[0-1])$/.test(p.split('.').slice(0, 2).join('.'))) {
          return;
        }
        prefixes.add(p);
      };

      addPrefixFromIp(hostIp);
      const data = this.store.load();
      for (const d of [...(data.knownDevices || []), ...(data.targets || [])]) {
        if (d && d.ip) addPrefixFromIp(d.ip);
      }

      for (const prefix of prefixes) {
        const probeHost = (ip) => new Promise((resolve) => {
          if (ip === hostIp) return resolve(false);
          for (const c of this.activeClients.values()) {
            if (c.ip === ip) return resolve(true);
          }
          const s = new net.Socket();
          let done = false;
          const finish = (ok) => {
            if (done) return;
            done = true;
            try { s.destroy(); } catch (_) {}
            resolve(ok);
          };
          s.setTimeout(220);
          s.on('connect', () => {
            try {
              s.write(`MACDROP_HELLO|${hostIp}|${HTTP_PORT}\n`);
            } catch (_) {}
            setTimeout(() => finish(true), 180);
          });
          s.on('data', (chunk) => {
            const rx = chunk.toString('utf8');
            if (rx.includes('MACDROP_PONG|')) {
              const line = rx.split(/\r?\n/).find(l => l.startsWith('MACDROP_PONG|')) || rx;
              const parts = line.trim().split('|');
              const meta = {
                id: (parts[1] || '').trim() || `device_${ip.replace(/\./g, '_')}`,
                name: (parts[2] || '').trim() || `Macintosh (${ip})`,
                machine: (parts[3] || '').trim() || 'Vintage Macintosh',
                version: (parts[4] || '').trim() || '',
                ip,
                port: DEFAULT_MACDROP_PORT,
                lastSeen: Date.now(),
                source: 'manual',
              };
              this.lastPongByIp.set(ip, meta);
              this.pingCache.set(`${ip}:${DEFAULT_MACDROP_PORT}`, { online: true, checkedAt: Date.now() });
              this.recordKnownDevice(meta);
              finish(true);
            }
          });
          s.on('timeout', () => finish(false));
          s.on('error', () => finish(false));
          s.on('close', () => finish(false));
          s.connect(DEFAULT_MACDROP_PORT, ip);
        });

        // Sweep 1..254 in 3 fast parallel batches (~220ms each)
        for (let start = 1; start <= 254; start += 85) {
          const batch = [];
          for (let i = start; i < start + 85 && i <= 254; i++) {
            batch.push(probeHost(`${prefix}.${i}`));
          }
          await Promise.all(batch);
        }
      }
    } catch (_) {}
  }

  /**
   * Scans and verifies all Macintosh clients immediately.
   */
  async refreshAllClients() {
    if (this.hasActiveTransfer()) {
      return;
    }
    this.sweepDeadClients();

    const now = Date.now();
    // 1. Probe reverse channel clients: if idle, end response to prompt immediate reconnect
    for (const [id, c] of this.activeClients.entries()) {
      if (c.mode === 'CHANNEL' && !c.transferInProgress && !c.writingDisk) {
        c.probing = true;
        c.probeSentAt = now;
        if (c.res && !c.res.writableEnded && !c.res.destroyed) {
          try {
            c.res.writeHead(204, { 'X-MacDrop-Status': 'POLL' });
            c.res.end();
          } catch (_) {}
        }
      }
    }

    // 2. Ping all known & manual LAN targets in parallel
    const data = this.store.load();
    const ipsToPing = new Map();
    for (const t of [...(data.targets || []), ...(data.knownDevices || [])]) {
      if (t && t.ip) {
        const cleanIp = t.ip.trim();
        if (!cleanIp.startsWith('192.168.97.') && !cleanIp.startsWith('192.168.65.') && cleanIp !== '127.0.0.1') {
          ipsToPing.set(cleanIp, t.port || DEFAULT_MACDROP_PORT);
        }
      }
    }

    const pingPromises = Array.from(ipsToPing.entries()).map(async ([ip, port]) => {
      const isOnline = await this.pingTarget(ip, port);
      this.pingCache.set(`${ip}:${port}`, { online: isOnline, checkedAt: Date.now() });
      return { ip, online: isOnline };
    });

    await Promise.all(pingPromises);

    // If no clients are connected yet, trigger a non-blocking subnet sweep for bridged 68k Macs
    if (this.activeClients.size === 0) {
      this.scanSubnetForBridgedClients().catch(() => {});
    }

    // 3. Wait briefly for active reverse channel clients to reconnect (living clients reconnect in ~150-250ms)
    await new Promise(r => setTimeout(r, 800));

    // 4. Any client that was probed and did NOT reconnect is offline! Purge only AFTER scan finishes!
    for (const [id, c] of this.activeClients.entries()) {
      if (c.probing) {
        this.activeClients.delete(id);
        console.log(`[MacDrop] Client did not reconnect after scan finished, purged: ${c.name} (${id})`);
      }
    }

    this.sweepDeadClients();
  }

  /**
   * Returns current active target metadata. Returns null if no target is online.
   */
  getActiveTarget() {
    if (!this.hasActiveTransfer()) {
      this.sweepDeadClients();
    }
    const targets = this.resolveTargets(this.selectedTargetId);
    if (targets.length === 0) return null;

    if (targets.length > 1) {
      return { id: 'all', name: `All Connected Macs (${targets.length})`, ip: 'multiple', online: true };
    }

    const t = targets[0];
    return {
      id: t.id,
      name: t.name,
      machine: t.machine || 'Macintosh 68k',
      ip: t.ip,
      port: t.port || DEFAULT_MACDROP_PORT,
      online: true,
      mode: t.mode || 'DIRECT',
    };
  }

  /**
   * Module status reporting for central registry and Web UI.
   */
  getStatus() {
    if (!this.hasActiveTransfer()) {
      this.sweepDeadClients();
    }

    const activeClientsList = Array.from(this.activeClients.values()).map(c => ({
      id: c.id,
      name: c.name,
      machine: c.machine,
      ip: c.ip,
      version: c.version,
      online: true,
      connectedAt: c.connectedAt,
    }));

    const data = this.store.load();
    const knownDevices = Array.isArray(data.knownDevices) ? data.knownDevices : [];

    // Unified devices map: strictly one entry per IP
    const deviceByIp = new Map();

    // 1. Add known devices (check pingCache for any LAN device and refresh in background if needed)
    for (const kd of knownDevices) {
      if (!kd || !kd.ip) continue;
      const cleanIp = kd.ip.trim();
      const port = kd.port || DEFAULT_MACDROP_PORT;
      const key = `${cleanIp}:${port}`;
      const pingEntry = this.pingCache.get(key);
      const isOnline = Boolean(pingEntry && pingEntry.online);

      const isDockerInternal = cleanIp === '127.0.0.1' ||
                               cleanIp.startsWith('192.168.97.') ||
                               cleanIp.startsWith('192.168.65.');
      const isReverseConnected = activeClientsList.some(c => c.ip === cleanIp);

      if (!isDockerInternal && !isReverseConnected && !this.hasActiveTransfer()) {
        if (!pingEntry || (Date.now() - pingEntry.checkedAt > 10000)) {
          this.pingTarget(cleanIp, port).then(resOnline => {
            this.pingCache.set(key, { online: resOnline, checkedAt: Date.now() });
          }).catch(() => {});
        }
      }

      const pongMeta = this.lastPongByIp.get(cleanIp);
      deviceByIp.set(cleanIp, {
        id: `device_${cleanIp.replace(/\./g, '_')}`,
        name: (pongMeta && pongMeta.name) || kd.name,
        machine: (pongMeta && pongMeta.machine) || kd.machine || 'Vintage Macintosh',
        ip: cleanIp,
        port,
        version: (pongMeta && pongMeta.version) || kd.version || '',
        online: isOnline,
        lastSeen: kd.lastSeen || 0,
      });
    }

    // 2. Overlay live active reverse-channel clients (always online, and updates latest reported name/machine)
    for (const c of activeClientsList) {
      if (!c || !c.ip) continue;
      const cleanIp = c.ip.trim();
      deviceByIp.set(cleanIp, {
        id: `device_${cleanIp.replace(/\./g, '_')}`,
        name: c.name,
        machine: c.machine || 'Macintosh 68k',
        ip: cleanIp,
        port: DEFAULT_MACDROP_PORT,
        version: c.version || '',
        online: true,
        lastSeen: c.connectedAt || Date.now(),
      });
    }

    const allDevices = Array.from(deviceByIp.values());

    const savedTargets = (data.targets || []).map(t => {
      const isOnline = Boolean(this.pingCache.get(`${t.ip}:${t.port || DEFAULT_MACDROP_PORT}`)?.online);
      return {
        ...t,
        online: isOnline,
      };
    });

    // Check ping for saved targets asynchronously
    for (const t of data.targets || []) {
      const key = `${t.ip}:${t.port || DEFAULT_MACDROP_PORT}`;
      const entry = this.pingCache.get(key);
      if (!entry || (Date.now() - entry.checkedAt > 10000)) {
        this.pingTarget(t.ip, t.port || DEFAULT_MACDROP_PORT).then(online => {
          this.pingCache.set(key, { online, checkedAt: Date.now() });
        });
      }
    }

    const activeTarget = this.getActiveTarget();
    const isOnline = Boolean(activeTarget && activeTarget.online);

    const now = Date.now();
    const inboundTransfers = Array.from(this.inboundTransfers.values()).map(t => ({
      id: t.id,
      clientName: t.clientName,
      clientId: t.clientId,
      fileName: t.fileName,
      downloadName: t.downloadName,
      fileSize: t.totalExpected || t.bytesReceived,
      downloadSize: t.downloadSize || t.dataLength || t.bytesReceived,
      isRawData: Boolean(t.isRawData),
      type: t.type,
      creator: t.creator,
      dataLength: t.dataLength,
      rsrcLength: t.rsrcLength,
      bytesReceived: t.bytesReceived,
      percent: t.status === 'COMPLETED' ? 100 : (t.totalExpected > 0 ? Math.min(99, Math.round((t.bytesReceived / t.totalExpected) * 100)) : 0),
      speedFormatted: t.status === 'COMPLETED' ? 'Complete' : t.speedFormatted,
      status: t.status,
      accepted: Boolean(t.accepted),
      expiresAt: t.expiresAt,
      remainingSec: t.expiresAt ? Math.max(0, Math.ceil((t.expiresAt - now) / 1000)) : null,
    }));

    return {
      id: this.id,
      name: this.name,
      ok: true,
      state: isOnline ? 'READY' : 'STANDBY',
      message: isOnline
        ? `Target: ${activeTarget.name} (${activeTarget.ip})`
        : 'No clients found / online.',
      activeTarget,
      selectedTargetId: this.selectedTargetId,
      clients: activeClientsList,
      savedTargets,
      devices: allDevices,
      inboundTransfers,
    };
  }
}

module.exports = new MacDropModule();
