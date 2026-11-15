const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFile, execFileSync, spawn } = require('child_process');
const { parseFlimFile, renderPosterPng } = require('./flim-parser');
const { generateVideoId, sanitizeFilename, titleFromFilename } = require('./library');

const PROFILES = {
  '128k': {
    id: '128k',
    label: 'Mac 128K (128k)',
    description: '512x342 • ratio 4 (~15 fps) • 380 B/t • Silent • Ordered',
    width: 512,
    height: 342,
    byterate: 380,
    filters: 'g1.6bbscz',
    fpsRatio: 4,
    group: false,
    errorStability: 0.5,
    bars: true,
    dither: 'ordered',
    errorAlgorithm: 'floyd',
    errorBidi: true,
    errorBleed: 0.95,
    silent: true,
    codecs: ['null', 'z32', 'lines:count=10', 'invert'],
  },
  '512k': {
    id: '512k',
    label: 'Mac 512K (512k)',
    description: '512x342 • ratio 4 (~15 fps) • 480 B/t • Silent • Ordered',
    width: 512,
    height: 342,
    byterate: 480,
    filters: 'g1.6bbscz',
    fpsRatio: 4,
    group: false,
    errorStability: 0.5,
    bars: true,
    dither: 'ordered',
    errorAlgorithm: 'floyd',
    errorBidi: true,
    errorBleed: 0.95,
    silent: true,
    codecs: ['null', 'z32', 'lines:count=10', 'invert'],
  },
  xl: {
    id: 'xl',
    label: 'Macintosh XL (xl)',
    description: '704x364 • ratio 4 (grp) • 580 B/t • Silent • Ordered',
    width: 704,
    height: 364,
    byterate: 580,
    filters: 'g1.6bbsc',
    fpsRatio: 4,
    group: true,
    errorStability: 0.5,
    bars: true,
    dither: 'ordered',
    errorAlgorithm: 'floyd',
    errorBidi: true,
    errorBleed: 0.95,
    silent: true,
    codecs: ['null', 'z32', 'lines:count=50', 'invert'],
  },
  plus: {
    id: 'plus',
    label: 'Mac Plus (plus)',
    description: '512x342 • ratio 2 (~30 fps) • 1500 B/t (~90 KB/s) • Audio • Ordered',
    width: 512,
    height: 342,
    byterate: 1500,
    filters: 'g1.6bbscz',
    fpsRatio: 2,
    group: false,
    errorStability: 0.5,
    bars: true,
    dither: 'ordered',
    errorAlgorithm: 'floyd',
    errorBidi: true,
    errorBleed: 0.95,
    silent: false,
    codecs: ['null', 'z32', 'lines:count=30', 'invert'],
  },
  performer: {
    id: 'performer',
    label: 'SE Performer (performer)',
    description: '512x342 • ratio 2 (~30 fps) • 5000 B/t (~300 KB/s) • Audio • Blue',
    width: 512,
    height: 342,
    byterate: 5000,
    filters: 'g1.6bsc',
    fpsRatio: 2,
    group: false,
    errorStability: 0.5,
    bars: true,
    dither: 'blue',
    errorAlgorithm: 'floyd',
    errorBidi: true,
    errorBleed: 0.95,
    silent: false,
    codecs: ['null', 'z32', 'lines:count=30', 'invert'],
  },
  portable: {
    id: 'portable',
    label: 'Mac Portable (portable)',
    description: '640x400 • ratio 2 (~30 fps) • 2500 B/t (~150 KB/s) • Audio • Error',
    width: 640,
    height: 400,
    byterate: 2500,
    filters: 'g1.6bsc',
    fpsRatio: 2,
    group: false,
    errorStability: 0.5,
    bars: true,
    dither: 'error',
    errorAlgorithm: 'floyd',
    errorBidi: true,
    errorBleed: 0.98,
    silent: false,
    codecs: ['null', 'z32', 'lines:count=50', 'invert'],
  },
  se: {
    id: 'se',
    label: 'Macintosh SE (se)',
    description: '512x342 • ratio 2 (~30 fps) • 2500 B/t (~150 KB/s) • Audio • Error',
    width: 512,
    height: 342,
    byterate: 2500,
    filters: 'g1.6bsc',
    fpsRatio: 2,
    group: false,
    errorStability: 0.5,
    bars: true,
    dither: 'error',
    errorAlgorithm: 'floyd',
    errorBidi: true,
    errorBleed: 0.98,
    silent: false,
    codecs: ['null', 'z32', 'lines:count=50', 'invert'],
  },
  se30: {
    id: 'se30',
    label: 'SE/30 & Mac II (se30)',
    description: '512x342 • ratio 1 (grp) • 6000 B/t (~360 KB/s) • Audio • Error',
    width: 512,
    height: 342,
    byterate: 6000,
    filters: 'g1.6sc',
    fpsRatio: 1,
    group: true,
    errorStability: 0.3,
    bars: false,
    dither: 'error',
    errorAlgorithm: 'floyd',
    errorBidi: true,
    errorBleed: 0.99,
    silent: false,
    codecs: ['null', 'z32', 'lines:count=70', 'invert'],
  },
  perfect: {
    id: 'perfect',
    label: 'Perfect Reference (perfect)',
    description: '512x342 • ratio 1 (grp) • 32000 B/t • Audio • Error',
    width: 512,
    height: 342,
    byterate: 32000,
    filters: 'g1.6sc',
    fpsRatio: 1,
    group: true,
    errorStability: 0.3,
    bars: false,
    dither: 'error',
    errorAlgorithm: 'floyd',
    errorBidi: true,
    errorBleed: 1.0,
    silent: false,
    codecs: ['null', 'z32', 'lines:count=70', 'invert'],
  },
};

const VALID_ASPECTS = ['bars', 'crop', 'stretch'];
const VALID_DITHERS = ['ordered', 'error', 'blue'];
const VALID_ERROR_ALGORITHMS = [
  'floyd',
  'false-floyd',
  'jarvis',
  'stucki',
  'atkinson',
  'burkes',
  'sierra',
  'sierra2',
  'sierra-lite',
];
const VALID_CODEC_MODES = ['auto', 'lines', 'z32', 'z16'];

/**
 * Resolves a binary path from env override, known locations, or PATH.
 */
function resolveBinary(envVar, candidates) {
  const override = process.env[envVar];
  if (override && fs.existsSync(override)) {
    return override;
  }

  for (const candidate of candidates) {
    if (candidate.startsWith('/')) {
      if (fs.existsSync(candidate)) return candidate;
    } else {
      try {
        const resolved = execFileSync('which', [candidate], {
          encoding: 'utf8',
          stdio: ['ignore', 'pipe', 'ignore'],
        }).trim();
        if (resolved && fs.existsSync(resolved)) return resolved;
      } catch {
        // Binary not on PATH
      }
    }
  }
  return null;
}

function getToolsStatus() {
  const flimmaker = resolveBinary('FLIMMAKER_BIN', [
    '/usr/local/bin/flimmaker',
    '/usr/bin/flimmaker',
    'flimmaker',
  ]);
  const ffmpeg = resolveBinary('FFMPEG_BIN', [
    '/usr/bin/ffmpeg',
    '/opt/homebrew/bin/ffmpeg',
    '/usr/local/bin/ffmpeg',
    'ffmpeg',
  ]);
  const ffprobe = resolveBinary('FFPROBE_BIN', [
    '/usr/bin/ffprobe',
    '/opt/homebrew/bin/ffprobe',
    '/usr/local/bin/ffprobe',
    'ffprobe',
  ]);

  return {
    flimmaker,
    ffmpeg,
    ffprobe,
    canEncode: Boolean(flimmaker),
  };
}

function parseProfilesParam(input) {
  let list = [];
  if (Array.isArray(input)) {
    list = input.map((s) => String(s).trim().toLowerCase());
  } else if (typeof input === 'string' && input.trim()) {
    list = input
      .split(',')
      .map((s) => s.trim().toLowerCase())
      .filter(Boolean);
  }
  const uniqueValid = [...new Set(list.filter((id) => Boolean(PROFILES[id])))];
  return uniqueValid.length > 0 ? uniqueValid : ['se30'];
}

function normalizeEncodeOptions(raw = {}) {
  const rawAspect = typeof raw.aspect === 'string' ? raw.aspect.trim().toLowerCase() : '';
  const aspect = VALID_ASPECTS.includes(rawAspect) ? rawAspect : null;

  const rawDither = typeof raw.dither === 'string' ? raw.dither.trim().toLowerCase() : '';
  const dither = VALID_DITHERS.includes(rawDither) ? rawDither : null;

  let silent = null;
  if (raw.silent === true || raw.silent === 'true' || raw.silent === '1' || raw.silent === 1) {
    silent = true;
  } else if (raw.silent === false || raw.silent === 'false' || raw.silent === '0' || raw.silent === 0) {
    silent = false;
  }

  const fromSec =
    raw.from !== undefined && raw.from !== '' && Number.isFinite(Number(raw.from))
      ? Math.max(0, Number(raw.from))
      : null;
  const durationSec =
    raw.duration !== undefined && raw.duration !== '' && Number.isFinite(Number(raw.duration)) && Number(raw.duration) > 0
      ? Number(raw.duration)
      : null;

  const byterateRaw = raw.byterate !== undefined && raw.byterate !== '' ? Number(raw.byterate) : NaN;
  const byterate =
    Number.isFinite(byterateRaw) && byterateRaw >= 300 && byterateRaw <= 32000
      ? Math.round(byterateRaw)
      : null;

  let targetFps = null;
  let fpsRatio = null;

  if (raw.targetFps !== undefined && raw.targetFps !== '') {
    const tf = Number(raw.targetFps);
    if (Number.isFinite(tf) && tf >= 5 && tf <= 60) {
      targetFps = Math.round(tf);
    }
  }

  if (typeof raw.fpsRatio === 'string' && raw.fpsRatio.trim().toLowerCase().startsWith('fps:')) {
    const tf = Number(raw.fpsRatio.trim().slice(4));
    if (Number.isFinite(tf) && tf >= 5 && tf <= 60) {
      targetFps = Math.round(tf);
    }
  } else if (raw.fpsRatio !== undefined && raw.fpsRatio !== '') {
    const fpsRatioRaw = Number(raw.fpsRatio);
    if (Number.isFinite(fpsRatioRaw)) {
      if (fpsRatioRaw >= 10 && fpsRatioRaw <= 60 && targetFps === null) {
        targetFps = Math.round(fpsRatioRaw);
      } else if (fpsRatioRaw >= 1 && fpsRatioRaw <= 12) {
        fpsRatio = Math.round(fpsRatioRaw);
      }
    }
  }

  let group = null;
  if (raw.group === true || raw.group === 'true' || raw.group === '1') {
    group = true;
  } else if (raw.group === false || raw.group === 'false' || raw.group === '0') {
    group = false;
  }

  const errorAlgorithm = VALID_ERROR_ALGORITHMS.includes(raw.errorAlgorithm)
    ? raw.errorAlgorithm
    : null;

  const stabRaw = raw.errorStability !== undefined && raw.errorStability !== '' ? Number(raw.errorStability) : NaN;
  const errorStability =
    Number.isFinite(stabRaw) && stabRaw >= 0 && stabRaw <= 1
      ? Number(stabRaw.toFixed(2))
      : null;

  const bleedRaw = raw.errorBleed !== undefined && raw.errorBleed !== '' ? Number(raw.errorBleed) : NaN;
  const errorBleed =
    Number.isFinite(bleedRaw) && bleedRaw >= 0 && bleedRaw <= 1
      ? Number(bleedRaw.toFixed(2))
      : null;

  let errorBidi = null;
  if (raw.errorBidi === true || raw.errorBidi === 'true' || raw.errorBidi === '1') {
    errorBidi = true;
  } else if (raw.errorBidi === false || raw.errorBidi === 'false' || raw.errorBidi === '0') {
    errorBidi = false;
  }

  const codecMode = VALID_CODEC_MODES.includes(raw.codecMode) ? raw.codecMode : 'auto';

  let filters = null;
  if (typeof raw.filters === 'string' && raw.filters.trim() !== '' && raw.filters.trim() !== 'auto') {
    const cleanFilters = raw.filters.trim().replace(/[^a-zA-Z0-9._-]/g, '').slice(0, 32);
    filters = cleanFilters === 'none' ? '' : cleanFilters;
  }

  return {
    aspect,
    dither,
    silent,
    from: fromSec,
    duration: durationSec,
    byterate,
    targetFps,
    fpsRatio,
    group,
    errorAlgorithm,
    errorStability,
    errorBleed,
    errorBidi,
    codecMode,
    filters,
    comment: raw.comment ? String(raw.comment).slice(0, 180) : null,
  };
}

function isFlimFile(filePath) {
  const fd = fs.openSync(filePath, 'r');
  try {
    const header = Buffer.alloc(5);
    const bytesRead = fs.readSync(fd, header, 0, 5, 0);
    return bytesRead >= 4 && header.toString('ascii', 0, 4) === 'FLIM';
  } finally {
    fs.closeSync(fd);
  }
}

function moveFileSafe(srcPath, destPath) {
  try {
    fs.renameSync(srcPath, destPath);
  } catch (err) {
    if (err.code === 'EXDEV') {
      fs.copyFileSync(srcPath, destPath);
      fs.unlinkSync(srcPath);
    } else {
      throw err;
    }
  }
}

function probeMedia(filePath, ffprobeBin) {
  if (!ffprobeBin) return Promise.resolve(null);
  return new Promise((resolve) => {
    execFile(
      ffprobeBin,
      ['-v', 'error', '-print_format', 'json', '-show_format', '-show_streams', filePath],
      { timeout: 15000, maxBuffer: 1024 * 1024 },
      (err, stdout) => {
        if (err || !stdout) return resolve(null);
        try {
          const data = JSON.parse(stdout);
          const streams = Array.isArray(data.streams) ? data.streams : [];
          const videoStream = streams.find((s) => s.codec_type === 'video');
          const audioStream = streams.find((s) => s.codec_type === 'audio');
          const fmtDuration = data.format && Number(data.format.duration);
          const streamDuration = videoStream && Number(videoStream.duration);
          const durationSec = Number.isFinite(fmtDuration)
            ? fmtDuration
            : Number.isFinite(streamDuration)
              ? streamDuration
              : 0;

          let fps = 0;
          const rateStr = videoStream && (videoStream.avg_frame_rate || videoStream.r_frame_rate);
          if (typeof rateStr === 'string' && rateStr.includes('/')) {
            const [num, den] = rateStr.split('/').map(Number);
            if (Number.isFinite(num) && Number.isFinite(den) && den > 0) {
              fps = Number((num / den).toFixed(3));
            }
          } else if (rateStr && Number.isFinite(Number(rateStr))) {
            fps = Number(Number(rateStr).toFixed(3));
          }

          resolve({
            durationSec: Number(durationSec.toFixed(2)),
            width: videoStream ? Number(videoStream.width) || 0 : 0,
            height: videoStream ? Number(videoStream.height) || 0 : 0,
            fps,
            hasAudio: Boolean(audioStream),
            codec: videoStream ? videoStream.codec_name : null,
          });
        } catch {
          resolve(null);
        }
      }
    );
  });
}

function inferProfileFromFlim(parsed, explicitProfile) {
  if (explicitProfile && PROFILES[explicitProfile]) {
    return explicitProfile;
  }
  const w = (parsed && parsed.info && parsed.info.width) || 512;
  const h = (parsed && parsed.info && parsed.info.height) || 342;
  if (w === 704 && h === 364) return 'xl';
  if (w === 640 && h === 400) return 'portable';

  const br = (parsed && parsed.info && parsed.info.byterate) || 3000;
  if (br >= 16000) return 'perfect';
  if (br >= 5500) return 'se30';
  if (br >= 4000) return 'performer';
  if (br >= 2000) return 'se';
  if (br >= 1000) return 'plus';
  if (br >= 430) return '512k';
  return '128k';
}

function resolveProfileEncodeSettings(profileId, options = {}) {
  const prof = PROFILES[profileId] || PROFILES.se30;
  const effectiveAspect = options.aspect || (prof.bars ? 'bars' : 'crop');
  const effectiveBars = effectiveAspect === 'crop' ? false : (options.aspect ? true : prof.bars);
  const effectiveDither = options.dither || prof.dither;
  const effectiveSilent =
    options.silent !== null && options.silent !== undefined ? Boolean(options.silent) : Boolean(prof.silent);
  const effectiveByterate =
    options.byterate !== null && options.byterate !== undefined ? options.byterate : prof.byterate;
  const effectiveFpsRatio =
    options.fpsRatio !== null && options.fpsRatio !== undefined ? options.fpsRatio : prof.fpsRatio;
  const effectiveGroup =
    options.group !== null && options.group !== undefined ? Boolean(options.group) : Boolean(prof.group);
  const effectiveErrorAlgorithm = options.errorAlgorithm || prof.errorAlgorithm;
  const effectiveErrorStability =
    options.errorStability !== null && options.errorStability !== undefined
      ? options.errorStability
      : prof.errorStability;
  const effectiveErrorBleed =
    options.errorBleed !== null && options.errorBleed !== undefined
      ? options.errorBleed
      : prof.errorBleed;
  const effectiveErrorBidi =
    options.errorBidi !== null && options.errorBidi !== undefined
      ? Boolean(options.errorBidi)
      : Boolean(prof.errorBidi);
  const effectiveFilters =
    options.filters !== null && options.filters !== undefined ? options.filters : prof.filters;
  const effectiveCodecMode = options.codecMode || 'auto';

  let cliCodecs;
  if (effectiveCodecMode === 'lines') {
    cliCodecs = ['lines', 'invert'];
  } else if (effectiveCodecMode === 'z32') {
    cliCodecs = ['z32', 'invert'];
  } else if (effectiveCodecMode === 'z16') {
    cliCodecs = ['z16', 'lines', 'invert'];
  } else {
    cliCodecs = (prof.codecs || ['null', 'z32', 'lines:count=50', 'invert']).filter((c) => c !== 'null');
  }

  return {
    aspect: effectiveAspect,
    bars: effectiveBars,
    dither: effectiveDither,
    silent: effectiveSilent,
    byterate: effectiveByterate,
    fpsRatio: effectiveFpsRatio,
    group: effectiveGroup,
    errorAlgorithm: effectiveErrorAlgorithm,
    errorStability: effectiveErrorStability,
    errorBleed: effectiveErrorBleed,
    errorBidi: effectiveErrorBidi,
    filters: effectiveFilters,
    codecMode: effectiveCodecMode,
    cliCodecs,
  };
}

async function savePosterArtifacts(library, videoId, posterBitmap) {
  if (!Buffer.isBuffer(posterBitmap) || posterBitmap.length < 1376) {
    return { hasPoster: false, posterBin: null, posterPng: null };
  }

  library.ensureDirs();
  const binAbs = path.join(library.dirs.posters, `${videoId}.bin`);
  const pngAbs = path.join(library.dirs.posters, `${videoId}.png`);

  fs.writeFileSync(binAbs, posterBitmap.subarray(0, 1376));
  const pngBuf = await renderPosterPng(posterBitmap, 2);
  if (pngBuf) {
    fs.writeFileSync(pngAbs, pngBuf);
  }

  return {
    hasPoster: true,
    posterBin: library.toRelativePath(binAbs),
    posterPng: pngBuf ? library.toRelativePath(pngAbs) : null,
  };
}

/**
 * Creates an async NetFlim encoder & job queue manager bound to `library`.
 */
function createEncoder({ library }) {
  const jobs = [];
  const queue = [];
  let activeJob = null;

  function trimJobsHistory() {
    while (jobs.length > 50) {
      const idx = jobs.findIndex((j) => j.status === 'done' || j.status === 'error');
      if (idx >= 0) jobs.splice(idx, 1);
      else break;
    }
  }

  function appendJobLog(job, line) {
    const clean = String(line || '').trim();
    if (!clean) return;
    job.logTail.push(clean.slice(0, 240));
    if (job.logTail.length > 30) {
      job.logTail.shift();
    }
  }

  function listJobs() {
    return [...jobs].sort((a, b) => Date.parse(b.createdAt) - Date.parse(a.createdAt));
  }

  function getJob(jobId) {
    return jobs.find((j) => j.id === jobId) || null;
  }

  function preProcessMedia(srcPath, destPath, ffmpegBin, job, { stretch = false, targetFps = null } = {}) {
    return new Promise((resolve, reject) => {
      const vfParts = [];
      if (targetFps && Number.isFinite(targetFps) && targetFps >= 5 && targetFps <= 60) {
        vfParts.push(`fps=${Math.round(targetFps)}`);
      }
      if (stretch) {
        vfParts.push('scale=512:342,setsar=1');
      }
      const args = [
        '-y',
        '-i',
        srcPath,
      ];
      if (vfParts.length > 0) {
        args.push('-vf', vfParts.join(','));
      }
      args.push(
        '-c:v',
        'mpeg4',
        '-q:v',
        '2',
        '-c:a',
        'pcm_s16le',
        destPath
      );
      const proc = spawn(ffmpegBin, args, { stdio: ['ignore', 'pipe', 'pipe'] });
      proc.stderr.on('data', (chunk) => {
        const lines = chunk.toString('utf8').split(/\r?\n|\r/);
        for (const l of lines) appendJobLog(job, l);
      });
      proc.on('error', reject);
      proc.on('close', (code) => {
        if (code === 0 && fs.existsSync(destPath)) {
          resolve(destPath);
        } else {
          reject(new Error(`ffmpeg pre-process failed (exit ${code})`));
        }
      });
    });
  }

  function runFlimmaker({ flimmakerBin, inputPath, outPath, profileId, options, job, profileIndex }) {
    return new Promise((resolve, reject) => {
      const cfg = resolveProfileEncodeSettings(profileId, options);
      const args = [
        inputPath,
        '--flim',
        outPath,
        '--profile',
        profileId,
      ];

      if (options.aspect === 'stretch') {
        args.push('--width', '512', '--height', '342');
      }

      args.push(
        '--byterate',
        String(cfg.byterate),
        '--fps-ratio',
        String(cfg.fpsRatio),
        '--group',
        cfg.group ? 'true' : 'false',
        '--bars',
        cfg.bars ? 'true' : 'false',
        '--dither',
        cfg.dither,
        '--silent',
        cfg.silent ? 'true' : 'false'
      );

      if (cfg.dither === 'error') {
        args.push(
          '--error-algorithm',
          cfg.errorAlgorithm,
          '--error-stability',
          String(cfg.errorStability),
          '--error-bleed',
          String(cfg.errorBleed),
          '--error-bidi',
          cfg.errorBidi ? 'true' : 'false'
        );
      }
      if (cfg.filters !== null && cfg.filters !== undefined) {
        args.push('--filters', cfg.filters);
      }
      for (const spec of cfg.cliCodecs) {
        args.push('--codec', spec);
      }

      if (options.from !== null) {
        args.push('--from', String(options.from));
      }
      if (options.duration !== null) {
        args.push('--duration', String(options.duration));
      }
      if (options.comment) {
        args.push('--comment', options.comment);
      }

      const proc = spawn(flimmakerBin, args, { stdio: ['ignore', 'pipe', 'pipe'] });
      let stderrBuf = '';

      const handleChunk = (chunk, isErr) => {
        const text = chunk.toString('utf8');
        if (isErr) stderrBuf = (stderrBuf + text).slice(-2048);
        const lines = text.split(/\r?\n|\r/);
        for (const line of lines) {
          appendJobLog(job, line);
          const pctMatch = line.match(/(\d{1,3}(?:\.\d+)?)\s*%/);
          if (pctMatch) {
            const localPct = Math.max(0, Math.min(100, Number(pctMatch[1])));
            const base = (profileIndex / job.totalProfiles) * 100;
            const slice = (1 / job.totalProfiles) * 100;
            job.progressPct = Math.min(99, Math.round(base + (slice * localPct) / 100));
          }
        }
      };

      proc.stdout.on('data', (c) => handleChunk(c, false));
      proc.stderr.on('data', (c) => handleChunk(c, true));
      proc.on('error', reject);
      proc.on('close', (code) => {
        if (code === 0 && fs.existsSync(outPath)) {
          const st = fs.statSync(outPath);
          if (st.size > 0) return resolve();
        }
        const detail = stderrBuf.trim().split(/\r?\n/).slice(-3).join(' | ');
        reject(new Error(`flimmaker exited with code ${code}${detail ? `: ${detail}` : ''}`));
      });
    });
  }

  async function executeJob(job) {
    const tools = getToolsStatus();
    if (!tools.flimmaker) {
      throw Object.assign(
        new Error('flimmaker binary not found on this host'),
        { code: 'FLIMMAKER_MISSING' }
      );
    }

    const item = library.getItem(job.videoId);
    if (!item || !item.sourcePath) {
      throw new Error('Source video file not found for encoding');
    }

    const srcAbsPath = library.resolvePath(item.sourcePath);
    if (!srcAbsPath || !fs.existsSync(srcAbsPath)) {
      throw new Error(`Missing source file on disk: ${item.sourcePath}`);
    }

    job.status = 'running';
    job.startedAt = new Date().toISOString();
    job.message = 'Inspecting source media...';
    job.progressPct = 2;

    let probedMeta = null;
    if (tools.ffprobe) {
      probedMeta = await probeMedia(srcAbsPath, tools.ffprobe);
      if (probedMeta && (!item.durationSec || !item.width)) {
        library.updateItem(item.id, {
          durationSec: item.durationSec || probedMeta.durationSec,
          width: item.width || probedMeta.width,
          height: item.height || probedMeta.height,
        });
      }
    }

    let encodeInputPath = srcAbsPath;
    let tmpScaledPath = null;
    const effectiveOptions = { ...job.options };

    try {
      const wantStretch = effectiveOptions.aspect === 'stretch';
      let needFfmpegFps = false;

      if (effectiveOptions.targetFps) {
        const srcFps = probedMeta && probedMeta.fps > 0 ? probedMeta.fps : 0;
        const targetFps = effectiveOptions.targetFps;
        const nearestRatio = srcFps > 0 ? Math.max(1, Math.round(srcFps / targetFps)) : 0;
        if (srcFps > 0 && nearestRatio >= 1 && Math.abs(srcFps / nearestRatio - targetFps) <= 0.6) {
          effectiveOptions.fpsRatio = nearestRatio;
        } else if (tools.ffmpeg) {
          needFfmpegFps = true;
          effectiveOptions.fpsRatio = 1;
        } else if (srcFps > 0) {
          effectiveOptions.fpsRatio = Math.max(1, Math.round(srcFps / targetFps));
        } else {
          effectiveOptions.fpsRatio = Math.max(1, Math.round(60 / targetFps));
        }
      }

      if (wantStretch || needFfmpegFps) {
        if (!tools.ffmpeg) {
          throw new Error('ffmpeg is required for stretch aspect scaling or frame rate conversion');
        }
        tmpScaledPath = path.join(library.dirs.tmp, `${job.id}_prep.mov`);
        job.message = wantStretch && needFfmpegFps
          ? `Pre-scaling 512x342 & converting to ${effectiveOptions.targetFps} fps...`
          : needFfmpegFps
            ? `Converting frame rate to ${effectiveOptions.targetFps} fps...`
            : 'Pre-scaling video to 512x342 (stretch)...';
        await preProcessMedia(srcAbsPath, tmpScaledPath, tools.ffmpeg, job, {
          stretch: wantStretch,
          targetFps: needFfmpegFps ? effectiveOptions.targetFps : null,
        });
        encodeInputPath = tmpScaledPath;
      }

      for (let i = 0; i < job.profiles.length; i++) {
        const profileId = job.profiles[i];
        const profileSpec = PROFILES[profileId];
        job.currentProfile = profileId;
        job.message = `Encoding ${profileSpec.label} (${profileId})...`;
        job.progressPct = Math.max(5, Math.round((i / job.totalProfiles) * 100));

        const outFilename = `${item.id}__${profileId}.flim`;
        const outAbsPath = path.join(library.dirs.flims, outFilename);
        const outTmpPath = path.join(library.dirs.tmp, `${job.id}__${profileId}.flim`);

        await runFlimmaker({
          flimmakerBin: tools.flimmaker,
          inputPath: encodeInputPath,
          outPath: outTmpPath,
          profileId,
          options: effectiveOptions,
          job,
          profileIndex: i,
        });

        moveFileSafe(outTmpPath, outAbsPath);

        const parsed = parseFlimFile(outAbsPath);
        const currentItem = library.getItem(item.id) || item;

        if (!currentItem.hasPoster && parsed.posterBitmap) {
          job.message = 'Rendering 1-bit poster artwork...';
          const posterData = await savePosterArtifacts(library, item.id, parsed.posterBitmap);
          if (posterData.hasPoster) {
            library.updateItem(item.id, posterData);
          }
        }

        const encodedFps =
          parsed.info.totalTicks > 0 && parsed.info.frameCount > 0
            ? Number(((parsed.info.frameCount * 60) / parsed.info.totalTicks).toFixed(1))
            : null;

        const cfg = resolveProfileEncodeSettings(profileId, effectiveOptions);

        library.addProfileToItem(item.id, profileId, {
          profile: profileId,
          file: library.toRelativePath(outAbsPath),
          size: parsed.fileSize,
          width: parsed.info.width,
          height: parsed.info.height,
          frameCount: parsed.info.frameCount,
          totalTicks: parsed.info.totalTicks,
          durationSec: parsed.durationSec,
          byterate: parsed.info.byterate,
          fps: encodedFps,
          targetFps: job.options.targetFps || null,
          silent: parsed.info.silent,
          aspect: cfg.aspect,
          dither: cfg.dither,
          fpsRatio: cfg.fpsRatio,
          group: cfg.group,
          errorAlgorithm: cfg.errorAlgorithm,
          errorStability: cfg.errorStability,
          errorBleed: cfg.errorBleed,
          errorBidi: cfg.errorBidi,
          codecMode: cfg.codecMode,
          codecs: cfg.cliCodecs,
          filters: cfg.filters,
          createdAt: new Date().toISOString(),
        });

        job.completedProfiles.push(profileId);
        job.progressPct = Math.round(((i + 1) / job.totalProfiles) * 100);
      }

      job.status = 'done';
      job.currentProfile = null;
      job.progressPct = 100;
      job.message = `Encoded ${job.completedProfiles.join(', ')}`;
      job.finishedAt = new Date().toISOString();
    } finally {
      if (tmpScaledPath && fs.existsSync(tmpScaledPath)) {
        try {
          fs.unlinkSync(tmpScaledPath);
        } catch {
          // Ignore temp cleanup failure
        }
      }
    }
  }

  function processQueue() {
    if (activeJob || queue.length === 0) return;
    const nextJob = queue.shift();
    activeJob = nextJob;

    executeJob(nextJob)
      .catch((err) => {
        nextJob.status = 'error';
        nextJob.currentProfile = null;
        nextJob.error = err.message || 'Encode failed';
        nextJob.message = `Error: ${nextJob.error}`;
        nextJob.finishedAt = new Date().toISOString();
      })
      .finally(() => {
        activeJob = null;
        processQueue();
      });
  }

  function enqueueEncode({ videoId, profiles, options }) {
    const tools = getToolsStatus();
    if (!tools.flimmaker) {
      const err = new Error(
        'flimmaker binary not found on this host. Run MacCompanion inside Docker/OrbStack or upload a pre-encoded .flim file.'
      );
      err.code = 'FLIMMAKER_MISSING';
      throw err;
    }

    const item = library.getItem(videoId);
    if (!item) {
      const err = new Error(`Library item "${videoId}" not found`);
      err.code = 'NOT_FOUND';
      throw err;
    }
    if (!item.sourcePath) {
      const err = new Error('This item was imported directly as a .flim without a source video to re-encode');
      err.code = 'NO_SOURCE';
      throw err;
    }

    const targetProfiles = parseProfilesParam(profiles);
    const normOpts = normalizeEncodeOptions(options);

    const job = {
      id: `job_${crypto.randomBytes(4).toString('hex')}`,
      videoId: item.id,
      title: item.title,
      profiles: targetProfiles,
      options: normOpts,
      status: 'queued',
      currentProfile: null,
      completedProfiles: [],
      totalProfiles: targetProfiles.length,
      progressPct: 0,
      message: `Queued (${targetProfiles.join(', ')})`,
      error: null,
      logTail: [],
      createdAt: new Date().toISOString(),
      startedAt: null,
      finishedAt: null,
    };

    jobs.push(job);
    queue.push(job);
    trimJobsHistory();
    setImmediate(processQueue);
    return job;
  }

  async function ingestFlimUpload({ tmpPath, originalFilename, title, profileHint }) {
    let parsed;
    try {
      parsed = parseFlimFile(tmpPath);
    } catch (err) {
      if (fs.existsSync(tmpPath)) fs.unlinkSync(tmpPath);
      const parseErr = new Error(`Invalid .flim file: ${err.message}`);
      parseErr.code = 'INVALID_FLIM';
      throw parseErr;
    }

    library.ensureDirs();
    const videoId = generateVideoId();
    const profileKey = inferProfileFromFlim(parsed, profileHint);
    const cleanOrig = sanitizeFilename(originalFilename, `${videoId}.flim`);
    const flimAbsPath = path.join(library.dirs.flims, `${videoId}__${profileKey}.flim`);

    moveFileSafe(tmpPath, flimAbsPath);

    const posterData = parsed.posterBitmap
      ? await savePosterArtifacts(library, videoId, parsed.posterBitmap)
      : { hasPoster: false, posterBin: null, posterPng: null };

    const item = library.upsertItem({
      id: videoId,
      title: (title && String(title).trim()) || titleFromFilename(cleanOrig),
      originalFilename: cleanOrig,
      sourcePath: null,
      sourceSize: 0,
      sourceMime: 'application/x-macflim',
      durationSec: parsed.durationSec,
      width: parsed.info.width,
      height: parsed.info.height,
      ...posterData,
      profiles: {
        [profileKey]: {
          profile: profileKey,
          file: library.toRelativePath(flimAbsPath),
          size: parsed.fileSize,
          width: parsed.info.width,
          height: parsed.info.height,
          frameCount: parsed.info.frameCount,
          totalTicks: parsed.info.totalTicks,
          durationSec: parsed.durationSec,
          byterate: parsed.info.byterate,
          silent: parsed.info.silent,
          aspect: 'bars',
          dither: 'ordered',
          createdAt: new Date().toISOString(),
        },
      },
    });

    return { mode: 'flim', item, job: null };
  }

  async function ingestUpload({ tmpPath, originalFilename, mimeType, title, profiles, options }) {
    if (!fs.existsSync(tmpPath)) {
      throw new Error('Uploaded temporary file is missing');
    }

    const cleanOrig = sanitizeFilename(originalFilename, 'upload.bin');

    if (isFlimFile(tmpPath)) {
      const hintedProfiles = profiles ? parseProfilesParam(profiles) : [];
      return ingestFlimUpload({
        tmpPath,
        originalFilename: cleanOrig,
        title,
        profileHint: hintedProfiles[0] || null,
      });
    }

    const tools = getToolsStatus();
    if (!tools.flimmaker) {
      fs.unlinkSync(tmpPath);
      const err = new Error(
        'flimmaker binary not found on this host. Run MacCompanion inside Docker/OrbStack or upload a pre-encoded .flim file.'
      );
      err.code = 'FLIMMAKER_MISSING';
      throw err;
    }

    library.ensureDirs();
    const videoId = generateVideoId();
    const ext = path.extname(cleanOrig) || '.mp4';
    const srcFilename = `${videoId}${ext}`;
    const srcAbsPath = path.join(library.dirs.sources, srcFilename);

    moveFileSafe(tmpPath, srcAbsPath);
    const stat = fs.statSync(srcAbsPath);
    const probed = await probeMedia(srcAbsPath, tools.ffprobe);

    const item = library.upsertItem({
      id: videoId,
      title: (title && String(title).trim()) || titleFromFilename(cleanOrig),
      originalFilename: cleanOrig,
      sourcePath: library.toRelativePath(srcAbsPath),
      sourceSize: stat.size,
      sourceMime: mimeType || 'video/mp4',
      durationSec: probed ? probed.durationSec : 0,
      width: probed ? probed.width : 0,
      height: probed ? probed.height : 0,
      hasPoster: false,
      posterBin: null,
      posterPng: null,
      profiles: {},
    });

    const job = enqueueEncode({
      videoId: item.id,
      profiles,
      options,
    });

    return { mode: 'encode', item, job };
  }

  return {
    getToolsStatus,
    listJobs,
    getJob,
    enqueueEncode,
    ingestUpload,
    isQueueBusy: () => Boolean(activeJob) || queue.length > 0,
  };
}

module.exports = {
  PROFILES,
  VALID_ASPECTS,
  VALID_DITHERS,
  getToolsStatus,
  parseProfilesParam,
  normalizeEncodeOptions,
  resolveProfileEncodeSettings,
  isFlimFile,
  createEncoder,
};
