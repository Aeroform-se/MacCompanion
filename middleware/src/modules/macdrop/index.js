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
const { createMacBinary, isMacBinary, parseMacBinaryHeader, getTypeCreatorForFilename, sanitizeMacFilename } = require('./macbinary');
const { createModuleStore } = require('../../core/storage');

const DEFAULT_MACDROP_PORT = 6803;
const TCP_CONNECT_TIMEOUT_MS = 6000;
const MAX_TRANSFER_LOGS = 30;

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

    // Cache of manual target ping statuses: `${ip}:${port}` -> { online: boolean, checkedAt: timestamp }
    this.pingCache = new Map();

    this.store = createModuleStore('macdrop', {
      targets: [],
      activeTarget: null,
      selectedTargetId: 'auto',
      recentTransfers: [],
    });

    const saved = this.store.load();
    if (saved && saved.selectedTargetId) {
      this.selectedTargetId = saved.selectedTargetId;
    }

    this.initRoutes();

    // Periodic sweep for closed/dead reverse channel sockets
    setInterval(() => this.sweepDeadClients(), 1500);
  }

  /**
   * Sweeps active clients to purge dead or closed sockets immediately.
   */
  sweepDeadClients() {
    const now = Date.now();
    for (const [id, c] of this.activeClients.entries()) {
      const isSocketDead = !c.res ||
                           c.res.destroyed ||
                           !c.res.socket ||
                           c.res.socket.destroyed ||
                           (c.req && c.req.destroyed) ||
                           c.res.writableEnded;

      // If socket is dead and client has not reconnected in > 2000ms, delete
      if (isSocketDead && !c.writingDisk && (now - c.lastSeen > 2000)) {
        this.activeClients.delete(id);
        console.log(`[MacDrop] Cleaned up dead client socket: ${c.name} (${id})`);
        continue;
      }

      // If client has not been seen in > 16 seconds (two missed poll cycles), delete
      if (!c.writingDisk && (now - c.lastSeen > 16000)) {
        this.activeClients.delete(id);
        console.log(`[MacDrop] Cleaned up timed out client: ${c.name} (${id})`);
        continue;
      }

      // If client was writing disk but timed out after 45s without ACK, delete
      if (c.writingDisk && (now - c.lastSeen > 45000)) {
        this.activeClients.delete(id);
        console.log(`[MacDrop] Cleaned up client timed out on disk write: ${c.name} (${id})`);
        continue;
      }
    }
  }

  /**
   * Waits for transfer completion ACK from the 68k Mac client.
   */
  waitForTransferAck(transferId, timeoutMs = 35000, clientId = null) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingTransfers.delete(transferId);
        reject(new Error(`Timed out waiting for Macintosh client confirmation (${timeoutMs / 1000}s)`));
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
      } else {
        pending.reject(new Error(`Macintosh reported write failure (${status})`));
      }
    }
  }

  /**
   * Fast TCP ping for manually configured vintage Mac targets on LAN.
   */
  pingTarget(ip, port = DEFAULT_MACDROP_PORT) {
    return new Promise((resolve) => {
      if (!ip) return resolve(false);
      const socket = new net.Socket();
      socket.setTimeout(1500);

      socket.on('connect', () => {
        socket.destroy();
        resolve(true);
      });

      socket.on('timeout', () => {
        socket.destroy();
        resolve(false);
      });

      socket.on('error', () => {
        socket.destroy();
        resolve(false);
      });

      socket.connect(port, ip);
    });
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
      const remoteIp = (req.socket.remoteAddress || '').replace(/^.*:/, '');
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
        awaitingReconnect: false,
      };

      this.activeClients.set(clientId, clientObj);
      console.log(`[MacDrop] Reverse Channel connected: ${name} (${machine}) from ${remoteIp} [ID: ${clientId}]`);

      // Idle poll timer: cleanly end response after 8s with 204 No Content so client reconnects and proves liveness
      const pollTimer = setTimeout(() => {
        const curr = this.activeClients.get(clientId);
        if (curr && curr.res === res && !curr.transferInProgress && !curr.writingDisk && !res.writableEnded) {
          try {
            res.writeHead(204, { 'X-MacDrop-Status': 'POLL' });
            res.end();
          } catch (_) {}
        }
      }, 8000);

      // Cleanup when connection closes or aborts
      const onDisconnect = () => {
        clearTimeout(pollTimer);
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
          } else {
            // Reverse channel closed: client may be reconnecting (poll) or quit
            // A live 68k client reconnects in ~150-250ms.
            // If after 1200ms no new connection has taken over, client is offline!
            setTimeout(() => {
              const check = this.activeClients.get(clientId);
              if (check && (!check.res || check.res === res || check.res.writableEnded || check.res.destroyed)) {
                this.activeClients.delete(clientId);
                console.log(`[MacDrop] Cleaned up offline client (no reconnect after socket close): ${name} (${clientId})`);
              }
            }, 1200);
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

    // 3. Service Status (Multi-client aware)
    this.router.get('/status', (req, res) => {
      res.json(this.getStatus());
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
      const targetName = (name || `Macintosh (${cleanIp})`).trim();

      // Test ping before saving
      const isOnline = await this.pingTarget(cleanIp, numPort);
      this.pingCache.set(`${cleanIp}:${numPort}`, { online: isOnline, checkedAt: Date.now() });

      const data = this.store.load();
      const targets = Array.isArray(data.targets) ? [...data.targets] : [];

      const targetObj = {
        id: `manual_${cleanIp.replace(/\./g, '_')}_${numPort}`,
        ip: cleanIp,
        port: numPort,
        name: targetName,
        lastSeen: Date.now(),
        online: isOnline,
      };

      const existingIdx = targets.findIndex(t => t.ip === targetObj.ip && t.port === targetObj.port);
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

      res.json({ ok: true, activeTarget: targetObj, online: isOnline });
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
          originalName = req.headers['x-filename'] || req.query.filename || 'PushedFile.bin';
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
        const primaryTarget = targetsToPush[0];

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

    if (targetQuery === 'all') {
      return activeList.length > 0 ? activeList : [];
    }

    if (targetQuery && targetQuery !== 'auto') {
      if (this.activeClients.has(targetQuery)) {
        return [this.activeClients.get(targetQuery)];
      }
      const data = this.store.load();
      const saved = (data.targets || []).find(t => t.id === targetQuery || t.ip === targetQuery);
      if (saved) {
        const isOnline = Boolean(this.pingCache.get(`${saved.ip}:${saved.port || DEFAULT_MACDROP_PORT}`)?.online);
        if (isOnline) {
          return [{ ...saved, mode: 'DIRECT' }];
        }
      }
      return [];
    }

    // 'auto' mode: prefer active reverse channel client
    if (activeList.length > 0) {
      return [activeList[0]];
    }

    // fallback to saved manual target IF online
    const data = this.store.load();
    if (data.activeTarget && data.activeTarget.ip) {
      const saved = data.activeTarget;
      const isOnline = Boolean(this.pingCache.get(`${saved.ip}:${saved.port || DEFAULT_MACDROP_PORT}`)?.online);
      if (isOnline) {
        return [{ ...saved, mode: 'DIRECT' }];
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
      if (target.mode === 'CHANNEL' && target.res && !target.res.writableEnded && !target.res.destroyed) {
        try {
          const clientRes = target.res;
          const ackPromise = this.waitForTransferAck(transferId, 35000, target.id);

          target.transferInProgress = true;

          clientRes.writeHead(200, {
            'Content-Type': 'application/x-macbinary',
            'Content-Length': buffer.length,
            'X-MacDrop-Status': 'STREAM',
            'X-Filename': macInfo.fileName,
            'X-Transfer-Id': transferId,
          });
          clientRes.write(buffer);
          clientRes.end();

          target.transferInProgress = false;
          target.writingDisk = true;
          target.lastSeen = Date.now();

          // Await confirmation from 68k client
          const ack = await ackPromise;
          target.writingDisk = false;
          target.lastSeen = Date.now();
          return resolve({ ok: true, bytesSent: buffer.length, confirmedBytes: ack.bytes });
        } catch (err) {
          if (target) {
            target.transferInProgress = false;
            target.writingDisk = false;
          }
          return reject(err);
        }
      }

      // 2. Direct TCP push fallback
      if (target.ip) {
        const socket = new net.Socket();
        socket.setTimeout(TCP_CONNECT_TIMEOUT_MS);
        let ackReceived = false;

        socket.on('connect', () => {
          socket.write(buffer);
        });

        socket.on('data', (chunk) => {
          const text = chunk.toString('utf8');
          if (text.includes('MACDROP_ACK')) {
            ackReceived = true;
            socket.destroy();
            resolve({ ok: true, bytesSent: buffer.length });
          }
        });

        socket.on('timeout', () => {
          socket.destroy();
          reject(new Error(`Timed out connecting to ${target.ip}:${target.port || DEFAULT_MACDROP_PORT}`));
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
   * Scans and verifies all Macintosh clients immediately.
   */
  async refreshAllClients() {
    this.sweepDeadClients();

    const now = Date.now();
    // 1. Probe reverse channel clients: if idle, end response to prompt immediate reconnect
    for (const [id, c] of this.activeClients.entries()) {
      if (c.mode === 'CHANNEL' && !c.transferInProgress && !c.writingDisk) {
        c.awaitingReconnect = true;
        c.probeSentAt = now;
        if (c.res && !c.res.writableEnded && !c.res.destroyed) {
          try {
            c.res.writeHead(204, { 'X-MacDrop-Status': 'POLL' });
            c.res.end();
          } catch (_) {}
        }
      }
    }

    // 2. Ping all manual LAN targets in parallel
    const data = this.store.load();
    const pingPromises = (data.targets || []).map(async (t) => {
      const isOnline = await this.pingTarget(t.ip, t.port || DEFAULT_MACDROP_PORT);
      this.pingCache.set(`${t.ip}:${t.port || DEFAULT_MACDROP_PORT}`, { online: isOnline, checkedAt: Date.now() });
      return { ip: t.ip, online: isOnline };
    });

    await Promise.all(pingPromises);

    // 3. Wait briefly for active reverse channel clients to reconnect (living clients reconnect in ~150-250ms)
    await new Promise(r => setTimeout(r, 800));

    // 4. Any client that was probed and did NOT reconnect is offline! Purge immediately!
    for (const [id, c] of this.activeClients.entries()) {
      if (c.awaitingReconnect) {
        this.activeClients.delete(id);
        console.log(`[MacDrop] Client did not reconnect after refresh probe, purged immediately: ${c.name} (${id})`);
      }
    }

    this.sweepDeadClients();
  }

  /**
   * Returns current active target metadata. Returns null if no target is online.
   */
  getActiveTarget() {
    this.sweepDeadClients();
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
    this.sweepDeadClients();

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

    return {
      id: this.id,
      name: this.name,
      ok: true,
      state: isOnline ? 'READY' : 'STANDBY',
      message: isOnline
        ? `Target: ${activeTarget.name} (${activeTarget.ip})`
        : 'Awaiting 68k Macintosh client connection...',
      activeTarget,
      selectedTargetId: this.selectedTargetId,
      clients: activeClientsList,
      savedTargets,
    };
  }
}

module.exports = new MacDropModule();
