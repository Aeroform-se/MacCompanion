const fs = require('fs');
const dgram = require('dgram');
const { readFlimToc } = require('./flim-parser');

const DEFAULT_VOD_PORT = 6804;
const MAGIC_CLIENT = 0x4E46564D; // 'NFVM'
const MAGIC_SERVER = 0x4E465653; // 'NFVS'

const PKT_OPEN = 0x01;
const PKT_WIN_ACK = 0x02;
const PKT_SEEK = 0x03;
const PKT_CLOSE = 0x04;

const PKT_OPEN_ACK = 0x81;
const PKT_CHUNK = 0x82;
const PKT_EOS = 0x83;

const OPEN_OK = 0;
const OPEN_NOT_FOUND = 1;
const OPEN_NO_PROFILE = 2;
const OPEN_ERROR = 3;

const CHUNK_PAYLOAD_MAX = 1152; // 22B header + 1152B = 1174B UDP payload (MTU safe)
const MAX_CHUNKS_PER_FRAME = 32; // fits in uint32 missingChunkMask
const FRAME_CACHE_WINDOW = 48;
const CHUNK_RESEND_COOLDOWN_MS = 180;
const NACK_RESEND_COOLDOWN_MS = 25;
const SESSION_TIMEOUT_MS = 15000;
const PUMP_INTERVAL_MS = 3;
const MAX_PACKETS_PER_TICK = 10;

function readCString(buf, start, len) {
  const slice = buf.subarray(start, start + len);
  const nullIdx = slice.indexOf(0);
  const end = nullIdx >= 0 ? nullIdx : slice.length;
  return slice.subarray(0, end).toString('ascii').trim();
}

/**
 * Strips the audio payload from a raw serialized .flim frame slice,
 * leaving only video deltas (`soundSize = 2`). Saves bandwidth on slow 68k links.
 */
function stripFrameAudio(frameBuf) {
  if (!Buffer.isBuffer(frameBuf) || frameBuf.length < 4) return frameBuf;
  const soundSize = frameBuf.readUInt16BE(2);
  if (soundSize <= 2) return frameBuf;

  const soundBytes = soundSize - 2;
  const videoStart = 4 + soundBytes;
  if (videoStart > frameBuf.length) return frameBuf;

  const out = Buffer.allocUnsafe(4 + (frameBuf.length - videoStart));
  frameBuf.copy(out, 0, 0, 2); // ticks
  out.writeUInt16BE(2, 2); // soundSize = 2 (no sound)
  frameBuf.copy(out, 4, videoStart, frameBuf.length);
  return out;
}

function resolveProfileMatch(item, requestedProfile) {
  const profiles = (item && item.profiles) || {};
  if (requestedProfile && profiles[requestedProfile]) {
    return requestedProfile;
  }
  if (profiles.se30) return 'se30';
  if (profiles.se) return 'se';
  if (profiles.plus) return 'plus';
  const keys = Object.keys(profiles);
  return keys[0] || null;
}

function buildOpenAck({
  sessionId,
  status,
  width = 0,
  height = 0,
  silent = false,
  frameCount = 0,
  totalTicks = 0,
  byterate = 0,
}) {
  const pkt = Buffer.alloc(28, 0);
  pkt.writeUInt32BE(MAGIC_SERVER, 0);
  pkt.writeUInt32BE(sessionId >>> 0, 4);
  pkt.writeUInt8(PKT_OPEN_ACK, 8);
  pkt.writeUInt8(status & 0xff, 9);
  pkt.writeUInt16BE(width & 0xffff, 10);
  pkt.writeUInt16BE(height & 0xffff, 12);
  pkt.writeUInt16BE(silent ? 1 : 0, 14);
  pkt.writeUInt32BE(frameCount >>> 0, 16);
  pkt.writeUInt32BE(totalTicks >>> 0, 20);
  pkt.writeUInt16BE(byterate & 0xffff, 24);
  pkt.writeUInt16BE(0, 26);
  return pkt;
}

function buildChunkPacket({
  sessionId,
  frameIdx,
  chunkIdx,
  totalChunks,
  frameBuf,
}) {
  const offset = chunkIdx * CHUNK_PAYLOAD_MAX;
  const end = Math.min(frameBuf.length, offset + CHUNK_PAYLOAD_MAX);
  const chunkLen = Math.max(0, end - offset);

  const pkt = Buffer.allocUnsafe(22 + chunkLen);
  pkt.writeUInt32BE(MAGIC_SERVER, 0);
  pkt.writeUInt32BE(sessionId >>> 0, 4);
  pkt.writeUInt8(PKT_CHUNK, 8);
  pkt.writeUInt8(0, 9); // frameFlags reserved
  pkt.writeUInt8(chunkIdx & 0xff, 10);
  pkt.writeUInt8(totalChunks & 0xff, 11);
  pkt.writeUInt32BE(frameIdx >>> 0, 12);
  pkt.writeUInt16BE(frameBuf.length & 0xffff, 16);
  pkt.writeUInt16BE(offset & 0xffff, 18);
  pkt.writeUInt16BE(chunkLen & 0xffff, 20);
  if (chunkLen > 0) {
    frameBuf.copy(pkt, 22, offset, end);
  }
  return pkt;
}

function buildEosPacket(sessionId, frameCount) {
  const pkt = Buffer.alloc(16, 0);
  pkt.writeUInt32BE(MAGIC_SERVER, 0);
  pkt.writeUInt32BE(sessionId >>> 0, 4);
  pkt.writeUInt8(PKT_EOS, 8);
  pkt.writeUInt32BE(frameCount >>> 0, 12);
  return pkt;
}

/**
 * Creates a UDP NetFlim VOD streamer (port 6804) serving cached .flim frames.
 */
function createVodStreamer({ library, port = DEFAULT_VOD_PORT }) {
  let socket = null;
  let boundPort = null;
  let pumpTimer = null;
  const sessions = new Map();

  function closeSession(key) {
    const sess = sessions.get(key);
    if (!sess) return;
    sessions.delete(key);
    try {
      if (sess.fd !== null && sess.fd !== undefined) {
        fs.closeSync(sess.fd);
        sess.fd = null;
      }
    } catch {
      // Ignore fd close errors
    }
    if (sessions.size === 0 && pumpTimer) {
      clearInterval(pumpTimer);
      pumpTimer = null;
    }
  }

  function sendPacket(pkt, rinfo) {
    if (!socket) return;
    socket.send(pkt, 0, pkt.length, rinfo.port, rinfo.address, () => {
      // Best-effort UDP send
    });
  }

  function loadFrame(sess, frameIdx) {
    if (frameIdx < 0 || frameIdx >= sess.frames.length) return null;
    const cached = sess.frameCache.get(frameIdx);
    if (cached) return cached;

    const meta = sess.frames[frameIdx];
    const raw = Buffer.allocUnsafe(meta.size);
    const bytesRead = fs.readSync(sess.fd, raw, 0, meta.size, meta.offset);
    const slice = bytesRead < meta.size ? raw.subarray(0, bytesRead) : raw;
    const payload = sess.stripAudio ? stripFrameAudio(slice) : slice;
    const totalChunks = Math.max(
      1,
      Math.min(MAX_CHUNKS_PER_FRAME, Math.ceil(payload.length / CHUNK_PAYLOAD_MAX))
    );

    const entry = {
      frameIdx,
      buf: payload,
      totalChunks,
      sentAt: new Array(totalChunks).fill(0),
      nackedAt: new Array(totalChunks).fill(0),
    };

    sess.frameCache.set(frameIdx, entry);

    // Prune cache entries far behind playback cursor
    if (sess.frameCache.size > FRAME_CACHE_WINDOW) {
      for (const k of sess.frameCache.keys()) {
        if (k < sess.nextNeededFrame || sess.frameCache.size > FRAME_CACHE_WINDOW + 8) {
          sess.frameCache.delete(k);
        }
        if (sess.frameCache.size <= FRAME_CACHE_WINDOW) break;
      }
    }

    return entry;
  }

  function pumpSessions() {
    const now = Date.now();
    for (const [key, sess] of sessions.entries()) {
      if (now - sess.lastActivity > SESSION_TIMEOUT_MS) {
        closeSession(key);
        continue;
      }
      if (sess.paused) continue;

      if (sess.nextNeededFrame >= sess.frameCount) {
        if (!sess.eosSent || now - sess.eosSentAt > 500) {
          sendPacket(buildEosPacket(sess.sessionId, sess.frameCount), sess.rinfo);
          sess.eosSent = true;
          sess.eosSentAt = now;
        }
        continue;
      }

      let sentBudget = MAX_PACKETS_PER_TICK;

      // 1. Prioritize explicit NACK retransmits (throttled per-chunk to avoid duplicate floods)
      while (sentBudget > 0 && sess.nackQueue.length > 0) {
        const req = sess.nackQueue.shift();
        if (req.frameIdx < sess.nextNeededFrame || req.frameIdx >= sess.frameCount) continue;
        const frameObj = loadFrame(sess, req.frameIdx);
        if (!frameObj || req.chunkIdx >= frameObj.totalChunks) continue;
        if (frameObj.nackedAt[req.chunkIdx] !== 0 && now - frameObj.nackedAt[req.chunkIdx] < NACK_RESEND_COOLDOWN_MS) {
          continue;
        }

        const pkt = buildChunkPacket({
          sessionId: sess.sessionId,
          frameIdx: req.frameIdx,
          chunkIdx: req.chunkIdx,
          totalChunks: frameObj.totalChunks,
          frameBuf: frameObj.buf,
        });
        sendPacket(pkt, sess.rinfo);
        frameObj.sentAt[req.chunkIdx] = now;
        frameObj.nackedAt[req.chunkIdx] = now;
        sentBudget--;
      }

      // 2. Push un-acked frames inside a MacTCP-safe sliding window (cap at 6 to prevent UDP recvBuf overflow)
      const effectiveWin = Math.min(sess.windowFrames, 6);
      const winEnd = Math.min(sess.frameCount, sess.nextNeededFrame + effectiveWin);
      for (let f = sess.nextNeededFrame; f < winEnd && sentBudget > 0; f++) {
        const rel = f - sess.nextNeededFrame;
        if (rel < 32 && ((sess.recvMask >>> rel) & 1) !== 0) {
          continue; // Already assembled cleanly on client
        }

        const frameObj = loadFrame(sess, f);
        if (!frameObj) continue;

        for (let c = 0; c < frameObj.totalChunks && sentBudget > 0; c++) {
          if (frameObj.sentAt[c] !== 0 && now - frameObj.sentAt[c] < CHUNK_RESEND_COOLDOWN_MS) {
            continue;
          }
          const pkt = buildChunkPacket({
            sessionId: sess.sessionId,
            frameIdx: f,
            chunkIdx: c,
            totalChunks: frameObj.totalChunks,
            frameBuf: frameObj.buf,
          });
          sendPacket(pkt, sess.rinfo);
          frameObj.sentAt[c] = now;
          sentBudget--;
        }
      }
    }
  }

  function ensurePumpRunning() {
    if (!pumpTimer && sessions.size > 0) {
      pumpTimer = setInterval(pumpSessions, PUMP_INTERVAL_MS);
    }
  }

  function handleOpen(msg, rinfo, sessionId) {
    if (msg.length < 44) return;
    const flags = msg.readUInt8(9);
    const windowFrames = Math.max(1, Math.min(32, msg.readUInt16BE(10) || 6));
    const startFrame = msg.readUInt32BE(12);
    const maxFrameBytes = msg.readUInt16BE(16) || 28672;
    const videoId = readCString(msg, 20, 16);
    const reqProfile = readCString(msg, 36, 8).toLowerCase();

    const key = `${rinfo.address}:${rinfo.port}:${sessionId}`;
    // Close existing session if client re-opened with same sessionId
    closeSession(key);

    const item = library.getItem(videoId);
    if (!item) {
      sendPacket(buildOpenAck({ sessionId, status: OPEN_NOT_FOUND }), rinfo);
      return;
    }

    const chosenProfile = resolveProfileMatch(item, reqProfile);
    if (!chosenProfile) {
      sendPacket(buildOpenAck({ sessionId, status: OPEN_NO_PROFILE }), rinfo);
      return;
    }

    const found = library.getProfileFile(item.id, chosenProfile);
    if (!found) {
      sendPacket(buildOpenAck({ sessionId, status: OPEN_NOT_FOUND }), rinfo);
      return;
    }

    try {
      const toc = readFlimToc(found.absPath);
      const fd = fs.openSync(found.absPath, 'r');
      const silentFlag = Boolean(flags & 1) || Boolean(toc.parsed.info.silent);

      const sess = {
        key,
        sessionId,
        rinfo: { address: rinfo.address, port: rinfo.port },
        videoId: item.id,
        profile: chosenProfile,
        stripAudio: Boolean(flags & 1),
        maxFrameBytes,
        windowFrames,
        nextNeededFrame: Math.min(startFrame, toc.frames.length),
        recvMask: 0,
        paused: false,
        eosSent: false,
        eosSentAt: 0,
        fd,
        frames: toc.frames,
        frameCount: toc.frames.length,
        info: toc.parsed.info,
        frameCache: new Map(),
        nackQueue: [],
        lastActivity: Date.now(),
      };

      sessions.set(key, sess);
      ensurePumpRunning();

      sendPacket(
        buildOpenAck({
          sessionId,
          status: OPEN_OK,
          width: toc.parsed.info.width,
          height: toc.parsed.info.height,
          silent: silentFlag,
          frameCount: toc.frames.length,
          totalTicks: toc.parsed.info.totalTicks,
          byterate: toc.parsed.info.byterate,
        }),
        rinfo
      );

      // Immediately push initial chunks without waiting an extra tick
      pumpSessions();
    } catch {
      sendPacket(buildOpenAck({ sessionId, status: OPEN_ERROR }), rinfo);
    }
  }

  function handleWinAck(msg, rinfo, sessionId) {
    if (msg.length < 28) return;
    const key = `${rinfo.address}:${rinfo.port}:${sessionId}`;
    const sess = sessions.get(key);
    if (!sess) return;

    sess.lastActivity = Date.now();
    sess.rinfo = { address: rinfo.address, port: rinfo.port };
    sess.paused = msg.readUInt8(9) !== 0;
    sess.windowFrames = Math.max(1, Math.min(32, msg.readUInt16BE(10) || sess.windowFrames));
    const nextNeeded = msg.readUInt32BE(12);
    if (nextNeeded > sess.nextNeededFrame) {
      sess.nextNeededFrame = Math.min(nextNeeded, sess.frameCount);
      sess.eosSent = false;
    }
    sess.recvMask = msg.readUInt32BE(16);

    const missingFrame = msg.readUInt32BE(20);
    const missingChunkMask = msg.readUInt32BE(24);
    if (missingFrame !== 0xffffffff && missingFrame >= sess.nextNeededFrame && missingFrame < sess.frameCount) {
      for (let c = 0; c < MAX_CHUNKS_PER_FRAME; c++) {
        if ((missingChunkMask >>> c) & 1) {
          sess.nackQueue.push({ frameIdx: missingFrame, chunkIdx: c });
        }
      }
      if (sess.nackQueue.length > 64) {
        sess.nackQueue.splice(0, sess.nackQueue.length - 64);
      }
    }

    if (!sess.paused) {
      pumpSessions();
    }
  }

  function handleSeek(msg, rinfo, sessionId) {
    if (msg.length < 16) return;
    const key = `${rinfo.address}:${rinfo.port}:${sessionId}`;
    const sess = sessions.get(key);
    if (!sess) return;

    sess.lastActivity = Date.now();
    const targetFrame = Math.min(msg.readUInt32BE(12), sess.frameCount);
    sess.nextNeededFrame = targetFrame;
    sess.recvMask = 0;
    sess.eosSent = false;
    sess.nackQueue = [];
    sess.frameCache.clear();
    pumpSessions();
  }

  function handleClose(rinfo, sessionId) {
    const key = `${rinfo.address}:${rinfo.port}:${sessionId}`;
    closeSession(key);
  }

  function onMessage(msg, rinfo) {
    if (!Buffer.isBuffer(msg) || msg.length < 12) return;
    if (msg.readUInt32BE(0) !== MAGIC_CLIENT) return;

    const sessionId = msg.readUInt32BE(4);
    const type = msg.readUInt8(8);

    switch (type) {
      case PKT_OPEN:
        handleOpen(msg, rinfo, sessionId);
        break;
      case PKT_WIN_ACK:
        handleWinAck(msg, rinfo, sessionId);
        break;
      case PKT_SEEK:
        handleSeek(msg, rinfo, sessionId);
        break;
      case PKT_CLOSE:
        handleClose(rinfo, sessionId);
        break;
      default:
        break;
    }
  }

  function start() {
    if (socket) return Promise.resolve(boundPort);
    return new Promise((resolve, reject) => {
      const sock = dgram.createSocket('udp4');
      sock.on('error', (err) => {
        if (!boundPort) {
          reject(err);
        } else {
          console.error(`[NetFlim VOD] UDP socket error:`, err.message);
        }
      });
      sock.on('message', onMessage);
      sock.bind(port, '0.0.0.0', () => {
        socket = sock;
        boundPort = sock.address().port;
        console.log(`[NetFlim VOD] UDP streaming server listening on 0.0.0.0:${boundPort}`);
        resolve(boundPort);
      });
    });
  }

  function stop() {
    if (pumpTimer) {
      clearInterval(pumpTimer);
      pumpTimer = null;
    }
    for (const key of [...sessions.keys()]) {
      closeSession(key);
    }
    if (socket) {
      const s = socket;
      socket = null;
      boundPort = null;
      return new Promise((resolve) => s.close(resolve));
    }
    return Promise.resolve();
  }

  function getStats() {
    return {
      port: boundPort || port,
      listening: Boolean(socket),
      activeStreams: sessions.size,
      sessions: [...sessions.values()].map((s) => ({
        videoId: s.videoId,
        profile: s.profile,
        clientIp: s.rinfo.address,
        nextFrame: s.nextNeededFrame,
        frameCount: s.frameCount,
        paused: s.paused,
      })),
    };
  }

  return {
    start,
    stop,
    getStats,
  };
}

module.exports = {
  DEFAULT_VOD_PORT,
  MAGIC_CLIENT,
  MAGIC_SERVER,
  PKT_OPEN,
  PKT_WIN_ACK,
  PKT_SEEK,
  PKT_CLOSE,
  PKT_OPEN_ACK,
  PKT_CHUNK,
  PKT_EOS,
  OPEN_OK,
  OPEN_NOT_FOUND,
  OPEN_NO_PROFILE,
  OPEN_ERROR,
  CHUNK_PAYLOAD_MAX,
  stripFrameAudio,
  resolveProfileMatch,
  createVodStreamer,
};
