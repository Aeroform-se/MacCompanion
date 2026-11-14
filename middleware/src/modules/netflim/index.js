const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const express = require('express');
const { SERVER_VERSION } = require('../../version');
const { formatPipeRow, formatPipeTable, sendPipeResponse } = require('../../core/format68k');
const { createLibrary, sanitizeFilename } = require('./library');
const { PROFILES, createEncoder } = require('./encoder');
const { renderPosterPng, readFlimToc } = require('./flim-parser');
const {
  createVodStreamer,
  DEFAULT_VOD_PORT,
  MAGIC_SERVER,
  stripFrameAudio,
  resolveProfileMatch,
} = require('./streamer');

function decodeHeaderFilename(val) {
  if (!val) return null;
  const raw = Array.isArray(val) ? val[0] : val;
  try {
    return decodeURIComponent(String(raw));
  } catch {
    return String(raw);
  }
}

function serializeLibraryItem(item) {
  const profileKeys = Object.keys(item.profiles || {});
  const totalFlimBytes = profileKeys.reduce(
    (sum, k) => sum + Number((item.profiles[k] && item.profiles[k].size) || 0),
    0
  );

  return {
    id: item.id,
    title: item.title,
    originalFilename: item.originalFilename,
    hasSource: Boolean(item.sourcePath),
    sourceSize: item.sourceSize || 0,
    sourceMime: item.sourceMime || null,
    durationSec: item.durationSec || 0,
    width: item.width || 0,
    height: item.height || 0,
    hasPoster: Boolean(item.hasPoster),
    posterUrl: item.hasPoster ? `/api/netflim/poster/${encodeURIComponent(item.id)}.png` : null,
    posterRawUrl: item.hasPoster ? `/api/netflim/poster/${encodeURIComponent(item.id)}/raw` : null,
    profiles: item.profiles || {},
    profileKeys,
    totalFlimBytes,
    createdAt: item.createdAt,
    updatedAt: item.updatedAt,
  };
}

/**
 * Factory creating a standalone NetFlim module instance (testable with temp dataDir).
 */
function createNetflimModule(opts = {}) {
  const library = createLibrary(opts.dataDir);
  const encoder = createEncoder({ library });
  const vodPort = opts.udpPort !== undefined
    ? opts.udpPort
    : (Number(process.env.NETFLIM_VOD_PORT) || DEFAULT_VOD_PORT);
  const vodStreamer = createVodStreamer({ library, port: vodPort });
  if (opts.startVod === true) {
    vodStreamer.start().catch(() => {});
  }
  const activeTcpStreams = new Set();
  const router = express.Router();

  function getStatus() {
    const tools = encoder.getToolsStatus();
    const summary = library.getSummary();
    const jobs = encoder.listJobs();
    const activeJobs = jobs.filter((j) => j.status === 'queued' || j.status === 'running').length;
    const vodStats = vodStreamer.getStats();

    let state = 'READY';
    let message = `${summary.videoCount} video${summary.videoCount === 1 ? '' : 's'} (${summary.flimCount} .flim)`;
    if (!tools.canEncode) {
      state = 'DEGRADED';
      message = summary.videoCount > 0
        ? `${summary.videoCount} .flim ready (flimmaker missing on host)`
        : 'flimmaker not found on host (direct .flim upload ready)';
    }

    return {
      id: 'netflim',
      name: 'NetFlim Video & Sync',
      ok: true,
      state,
      message,
      canEncode: tools.canEncode,
      tools: {
        flimmaker: Boolean(tools.flimmaker),
        ffmpeg: Boolean(tools.ffmpeg),
        ffprobe: Boolean(tools.ffprobe),
      },
      summary,
      activeJobs,
      activeStreams: vodStats.activeStreams + activeTcpStreams.size,
      vod: {
        ...vodStats,
        tcpStreams: activeTcpStreams.size,
      },
      udpPorts: {
        vod: vodStats.port,
        sync: 5050,
      },
      profiles: PROFILES,
    };
  }

  // Status & metadata
  router.get('/status', (req, res) => {
    res.json(getStatus());
  });

  // Lightweight 68k ping
  router.get('/ping', (req, res) => {
    const summary = library.getSummary();
    sendPipeResponse(
      req,
      res,
      formatPipeRow(['OK', 'netflim', SERVER_VERSION, summary.videoCount, summary.flimCount])
    );
  });

  // 68k pipe-delimited catalog (?profile=se30&encoding=macroman)
  router.get('/catalog', (req, res) => {
    const reqProfile = req.query.profile ? String(req.query.profile).trim().toLowerCase() : null;
    const items = library.listItems();
    const rows = [];

    for (const item of items) {
      const profMap = item.profiles || {};
      const keys = Object.keys(profMap);
      if (keys.length === 0) continue;

      if (reqProfile && !profMap[reqProfile]) continue;

      const selected =
        (reqProfile && profMap[reqProfile]) ||
        profMap.se30 ||
        profMap.se ||
        profMap.plus ||
        profMap[keys[0]];

      rows.push([
        item.id,
        item.title || item.originalFilename || item.id,
        Math.round(selected.durationSec || item.durationSec || 0),
        keys.join(','),
        selected.size || 0,
        item.hasPoster ? 1 : 0,
        selected.frameCount || 0,
        selected.silent ? 1 : 0,
      ]);
    }

    sendPipeResponse(req, res, formatPipeTable(rows));
  });

  // Web UI full library view
  router.get('/library', (req, res) => {
    const items = library.listItems().map(serializeLibraryItem);
    res.json({
      ok: true,
      items,
      summary: library.getSummary(),
      tools: getStatus().tools,
      canEncode: getStatus().canEncode,
      profiles: PROFILES,
    });
  });

  // Job queue monitoring
  router.get('/jobs', (req, res) => {
    res.json({
      ok: true,
      busy: encoder.isQueueBusy(),
      jobs: encoder.listJobs(),
    });
  });

  router.get('/jobs/:jobId', (req, res) => {
    const job = encoder.getJob(req.params.jobId);
    if (!job) {
      return res.status(404).json({ ok: false, error: 'Job not found' });
    }
    return res.json({ ok: true, job });
  });

  // Streaming file upload (.mp4/.mov/etc. -> flimmaker queue, or direct .flim ingest)
  router.post('/upload', (req, res) => {
    const contentType = String(req.headers['content-type'] || '').toLowerCase();
    if (contentType.includes('multipart/form-data')) {
      return res.status(415).json({
        ok: false,
        error: 'Send raw binary body with X-File-Name header instead of multipart/form-data',
      });
    }

    library.ensureDirs();
    const originalFilename = sanitizeFilename(
      decodeHeaderFilename(req.headers['x-file-name']) || req.query.filename || 'upload.bin'
    );
    const title = decodeHeaderFilename(req.headers['x-video-title']) || req.query.title || '';
    const profilesRaw = decodeHeaderFilename(req.headers['x-profiles']) || req.query.profiles || '';
    const aspect = decodeHeaderFilename(req.headers['x-aspect']) || req.query.aspect || 'bars';
    const dither = decodeHeaderFilename(req.headers['x-dither']) || req.query.dither || 'ordered';
    const silent = req.headers['x-silent'] || req.query.silent || 'false';
    const from = req.headers['x-from'] || req.query.from;
    const duration = req.headers['x-duration'] || req.query.duration;
    const comment = decodeHeaderFilename(req.headers['x-comment']) || req.query.comment || '';

    const tmpPath = path.join(
      library.dirs.tmp,
      `upload_${Date.now()}_${crypto.randomBytes(4).toString('hex')}`
    );

    const writeStream = fs.createWriteStream(tmpPath);
    let cleanedUp = false;
    let totalWritten = 0;

    const cleanupTmp = () => {
      if (cleanedUp) return;
      cleanedUp = true;
      try {
        if (fs.existsSync(tmpPath)) fs.unlinkSync(tmpPath);
      } catch {
        // Ignore cleanup failures
      }
    };

    req.on('data', (chunk) => {
      totalWritten += chunk.length;
    });

    req.on('aborted', () => {
      writeStream.destroy();
      cleanupTmp();
    });

    writeStream.on('error', (err) => {
      cleanupTmp();
      if (!res.headersSent) {
        res.status(500).json({ ok: false, error: err.message });
      }
    });

    writeStream.on('finish', async () => {
      if (req.aborted) {
        cleanupTmp();
        return;
      }

      if (totalWritten === 0) {
        cleanupTmp();
        return res.status(400).json({ ok: false, error: 'Empty upload body' });
      }

      try {
        const result = await encoder.ingestUpload({
          tmpPath,
          originalFilename,
          mimeType: contentType || 'application/octet-stream',
          title,
          profiles: profilesRaw,
          options: { aspect, dither, silent, from, duration, comment },
        });

        cleanedUp = true; // ingestUpload moved or deleted tmpPath
        const statusCode = result.mode === 'encode' ? 202 : 201;
        return res.status(statusCode).json({
          ok: true,
          mode: result.mode,
          item: serializeLibraryItem(result.item),
          job: result.job,
        });
      } catch (err) {
        cleanupTmp();
        const codeMap = {
          FLIMMAKER_MISSING: 503,
          INVALID_FLIM: 400,
        };
        const status = codeMap[err.code] || 500;
        return res.status(status).json({
          ok: false,
          code: err.code || 'UPLOAD_FAILED',
          error: err.message || 'Upload ingestion failed',
        });
      }
    });

    req.pipe(writeStream);
  });

  // Trigger encoding additional profile(s) from retained source video
  router.post('/library/:id/encode', (req, res) => {
    try {
      const body = req.body || {};
      const job = encoder.enqueueEncode({
        videoId: req.params.id,
        profiles: body.profiles,
        options: body,
      });
      return res.status(202).json({ ok: true, job });
    } catch (err) {
      const codeMap = {
        FLIMMAKER_MISSING: 503,
        NOT_FOUND: 404,
        NO_SOURCE: 400,
      };
      return res.status(codeMap[err.code] || 500).json({
        ok: false,
        code: err.code || 'ENCODE_FAILED',
        error: err.message,
      });
    }
  });

  // Rename / update video metadata
  router.patch('/library/:id', (req, res) => {
    const body = req.body || {};
    const title = body.title !== undefined ? String(body.title).trim().slice(0, 80) : null;
    if (!title) {
      return res.status(400).json({ ok: false, error: 'Non-empty title is required' });
    }

    const updated = library.updateItem(req.params.id, { title });
    if (!updated) {
      return res.status(404).json({ ok: false, error: 'Video not found' });
    }
    return res.json({ ok: true, item: serializeLibraryItem(updated) });
  });

  // Delete video and all associated files
  router.delete('/library/:id', (req, res) => {
    const removed = library.deleteItem(req.params.id);
    if (!removed) {
      return res.status(404).json({ ok: false, error: 'Video not found' });
    }
    return res.json({ ok: true, deletedId: removed.id });
  });

  // Delete one encoded profile
  router.delete('/library/:id/profiles/:profile', (req, res) => {
    const updated = library.deleteProfile(req.params.id, req.params.profile);
    if (!updated) {
      return res.status(404).json({ ok: false, error: 'Video not found' });
    }
    return res.json({ ok: true, item: serializeLibraryItem(updated) });
  });

  // Poster PNG for Web UI
  router.get(['/poster/:id.png', '/poster/:id'], async (req, res) => {
    const id = req.params.id.replace(/\.png$/i, '');
    const paths = library.getPosterPaths(id);
    if (!paths) {
      return res.status(404).json({ ok: false, error: 'Video not found' });
    }

    if (paths.pngPath) {
      res.setHeader('Content-Type', 'image/png');
      res.setHeader('Cache-Control', 'public, max-age=3600');
      return res.sendFile(paths.pngPath);
    }

    if (paths.binPath) {
      try {
        const binBuf = fs.readFileSync(paths.binPath);
        const pngBuf = await renderPosterPng(binBuf, 2);
        if (pngBuf) {
          res.setHeader('Content-Type', 'image/png');
          return res.send(pngBuf);
        }
      } catch {
        // Fall through to 404
      }
    }

    return res.status(404).json({ ok: false, error: 'Poster not available' });
  });

  // Raw 128x86 1-bit MSB-first Mac bitmap (1376 bytes) for 68k CopyBits
  router.get('/poster/:id/raw', (req, res) => {
    const paths = library.getPosterPaths(req.params.id);
    if (!paths || !paths.binPath) {
      return res.status(404).send('NOT_FOUND');
    }
    res.setHeader('Content-Type', 'application/octet-stream');
    res.setHeader('Cache-Control', 'public, max-age=3600');
    return res.sendFile(paths.binPath);
  });

  // Framed binary TCP VOD stream (port 6800) with hardware-friendly backpressure
  router.get('/stream/:id', (req, res) => {
    const item = library.getItem(req.params.id);
    if (!item) {
      res.setHeader('X-NetFlim-Status', 'NOT_FOUND');
      return res.status(404).send('NOT_FOUND');
    }

    const reqProfile = req.query.profile ? String(req.query.profile).trim().toLowerCase() : null;
    const chosenProfile = resolveProfileMatch(item, reqProfile);
    if (!chosenProfile) {
      res.setHeader('X-NetFlim-Status', 'NO_PROFILE');
      return res.status(404).send('NO_PROFILE');
    }

    const found = library.getProfileFile(item.id, chosenProfile);
    if (!found) {
      res.setHeader('X-NetFlim-Status', 'NOT_FOUND');
      return res.status(404).send('NOT_FOUND');
    }

    let toc;
    let fd;
    try {
      toc = readFlimToc(found.absPath);
      fd = fs.openSync(found.absPath, 'r');
    } catch {
      res.setHeader('X-NetFlim-Status', 'ERROR');
      return res.status(500).send('STREAM_ERROR');
    }

    const stripAudio = req.query.stripAudio === '1' || req.query.stripAudio === 'true';
    const rawStart = parseInt(req.query.start || '0', 10);
    const startFrame = Number.isFinite(rawStart)
      ? Math.max(0, Math.min(rawStart, toc.frames.length))
      : 0;
    const maxBytes = Math.min(65534, Math.max(4096, parseInt(req.query.maxBytes || '28672', 10) || 28672));
    const silent = stripAudio || Boolean(toc.parsed.info.silent);

    if (req.socket && typeof req.socket.setNoDelay === 'function') {
      req.socket.setNoDelay(true);
    }
    res.useChunkedEncodingByDefault = false;
    res.sendDate = false;
    // Pad HTTP response header so the 24B NFVS header and all 8B-framed records start on a 32-bit (0 mod 4) boundary
    const hdrPadLen = (4 - ((155 + chosenProfile.length) % 4)) % 4;
    res.writeHead(200, {
      'Content-Type': 'application/octet-stream',
      'Cache-Control': 'no-store',
      'Connection': 'close',
      'X-NetFlim-Status': 'OK',
      'X-NetFlim-Profile': chosenProfile,
      'X-Pad': '0'.repeat(hdrPadLen),
    });

    const ZERO_PAD = Buffer.alloc(4, 0);

    const hdr = Buffer.alloc(24);
    hdr.writeUInt32BE(MAGIC_SERVER, 0);
    hdr.writeUInt16BE(toc.parsed.info.width & 0xffff, 4);
    hdr.writeUInt16BE(toc.parsed.info.height & 0xffff, 6);
    hdr.writeUInt16BE(silent ? 1 : 0, 8);
    hdr.writeUInt16BE(toc.parsed.info.byterate & 0xffff, 10);
    hdr.writeUInt32BE(toc.frames.length >>> 0, 12);
    hdr.writeUInt32BE(toc.parsed.info.totalTicks >>> 0, 16);
    hdr.writeUInt32BE(startFrame >>> 0, 20);

    const streamToken = {
      videoId: item.id,
      profile: chosenProfile,
      clientIp: req.ip || (req.socket && req.socket.remoteAddress) || '',
    };
    activeTcpStreams.add(streamToken);

    let closed = false;
    let sentHeader = false;
    let nextFrame = startFrame;
    const BATCH_TARGET_BYTES = 32768;

    const cleanup = () => {
      if (closed) return;
      closed = true;
      activeTcpStreams.delete(streamToken);
      if (fd !== null && fd !== undefined) {
        try {
          fs.closeSync(fd);
        } catch {
          // Ignore close error
        }
        fd = null;
      }
    };

    req.on('close', cleanup);
    res.on('close', cleanup);
    res.on('error', cleanup);

    const pumpBatch = () => {
      if (closed) return;
      try {
        const chunks = [];
        let queued = 0;

        if (!sentHeader) {
          chunks.push(hdr);
          queued += hdr.length;
          sentHeader = true;
        }

        while (nextFrame < toc.frames.length && queued < BATCH_TARGET_BYTES) {
          const meta = toc.frames[nextFrame];
          const raw = Buffer.allocUnsafe(meta.size);
          const bytesRead = fs.readSync(fd, raw, 0, meta.size, meta.offset);
          const rawSlice = bytesRead < meta.size ? raw.subarray(0, bytesRead) : raw;
          let payload = stripAudio ? stripFrameAudio(rawSlice) : rawSlice;
          if (payload.length > maxBytes) {
            payload = payload.subarray(0, maxBytes);
          }

          const padBytes = (4 - (payload.length & 3)) & 3;
          const fHdr = Buffer.allocUnsafe(8);
          fHdr.writeUInt32BE(nextFrame >>> 0, 0);
          fHdr.writeUInt16BE(payload.length & 0xffff, 4);
          fHdr.writeUInt16BE(padBytes, 6);
          if (padBytes > 0) {
            chunks.push(fHdr, payload, ZERO_PAD.subarray(0, padBytes));
          } else {
            chunks.push(fHdr, payload);
          }
          queued += 8 + payload.length + padBytes;
          nextFrame++;
        }

        const isEos = nextFrame >= toc.frames.length;
        if (isEos) {
          const eos = Buffer.alloc(8);
          eos.writeUInt32BE(toc.frames.length >>> 0, 0);
          eos.writeUInt16BE(0xffff, 4);
          eos.writeUInt16BE(0, 6);
          chunks.push(eos);
        }

        const batchBuf = chunks.length === 1 ? chunks[0] : Buffer.concat(chunks);
        const canContinue = res.write(batchBuf);
        if (isEos) {
          cleanup();
          res.end();
          return;
        }
        if (canContinue) {
          setImmediate(pumpBatch);
        } else {
          res.once('drain', pumpBatch);
        }
      } catch {
        cleanup();
        try {
          res.end();
        } catch {
          // Ignore socket end error
        }
      }
    };

    setImmediate(pumpBatch);
  });

  // Direct .flim file download
  router.get('/download/:id/:profile', (req, res) => {
    const found = library.getProfileFile(req.params.id, req.params.profile);
    if (!found) {
      return res.status(404).json({ ok: false, error: 'Encoded profile not found' });
    }
    const baseName = sanitizeFilename(`${found.item.title}_${req.params.profile}.flim`);
    res.setHeader('Content-Type', 'application/octet-stream');
    res.setHeader('Content-Disposition', `attachment; filename="${baseName}"`);
    return res.sendFile(found.absPath);
  });

  return {
    id: 'netflim',
    name: 'NetFlim Video & Sync',
    basePath: '/api/netflim',
    router,
    getStatus,
    library,
    encoder,
    vodStreamer,
  };
}

const defaultModule = createNetflimModule();

module.exports = {
  ...defaultModule,
  createNetflimModule,
};
