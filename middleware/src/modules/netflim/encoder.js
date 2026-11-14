const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { execFile, execFileSync, spawn } = require('child_process');
const { parseFlimFile, renderPosterPng } = require('./flim-parser');
const { generateVideoId, sanitizeFilename, titleFromFilename } = require('./library');

const PROFILES = {
  se30: {
    id: 'se30',
    label: 'SE/30 & Mac II',
    description: '512x342 • ~30 fps (ratio 1) • 3800 B/t (~228 KB/s) • Audio',
    width: 512,
    height: 342,
    byterate: 3800,
  },
  se: {
    id: 'se',
    label: 'Macintosh SE',
    description: '512x342 • ~15 fps (ratio 2) • 1500 B/t (~90 KB/s) • Audio',
    width: 512,
    height: 342,
    byterate: 1500,
  },
  plus: {
    id: 'plus',
    label: 'Mac Plus / Low BW',
    description: '512x342 • ~15 fps (ratio 2) • 900 B/t (~54 KB/s) • Low BW',
    width: 512,
    height: 342,
    byterate: 900,
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
  const aspect = VALID_ASPECTS.includes(raw.aspect) ? raw.aspect : 'bars';
  const dither = VALID_DITHERS.includes(raw.dither) ? raw.dither : 'ordered';
  const silent =
    raw.silent === true || raw.silent === 'true' || raw.silent === '1' || raw.silent === 1;

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

  const fpsRatioRaw = raw.fpsRatio !== undefined && raw.fpsRatio !== '' ? Number(raw.fpsRatio) : NaN;
  const fpsRatio =
    Number.isFinite(fpsRatioRaw) && fpsRatioRaw >= 1 && fpsRatioRaw <= 12
      ? Math.round(fpsRatioRaw)
      : null;

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

          resolve({
            durationSec: Number(durationSec.toFixed(2)),
            width: videoStream ? Number(videoStream.width) || 0 : 0,
            height: videoStream ? Number(videoStream.height) || 0 : 0,
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
  const br = (parsed && parsed.info && parsed.info.byterate) || 3000;
  if (br >= 2400) return 'se30';
  if (br >= 1200) return 'se';
  return 'plus';
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

  function preScaleStretch(srcPath, destPath, ffmpegBin, job) {
    return new Promise((resolve, reject) => {
      const args = [
        '-y',
        '-i',
        srcPath,
        '-vf',
        'scale=512:342,setsar=1',
        '-c:v',
        'mpeg4',
        '-q:v',
        '2',
        '-c:a',
        'pcm_s16le',
        destPath,
      ];
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
          reject(new Error(`ffmpeg stretch pre-scale failed (exit ${code})`));
        }
      });
    });
  }

  function runFlimmaker({ flimmakerBin, inputPath, outPath, profileId, options, job, profileIndex }) {
    return new Promise((resolve, reject) => {
      const targetByterate = options.byterate || (PROFILES[profileId] && PROFILES[profileId].byterate) || 3000;
      const args = [
        inputPath,
        '--flim',
        outPath,
        '--profile',
        profileId,
        '--byterate',
        String(targetByterate),
        '--bars',
        options.aspect === 'crop' ? 'false' : 'true',
        '--dither',
        options.dither,
        '--silent',
        options.silent ? 'true' : 'false',
      ];

      if (options.fpsRatio !== null && options.fpsRatio !== undefined) {
        args.push('--fps-ratio', String(options.fpsRatio));
      }
      if (options.group !== null && options.group !== undefined) {
        args.push('--group', options.group ? 'true' : 'false');
      }
      if (options.dither === 'error') {
        if (options.errorAlgorithm) {
          args.push('--error-algorithm', options.errorAlgorithm);
        }
        /* Default SE/30 error stability to 0.45 and bleed to 0.95 to suppress 1px z32 dither boiling */
        const stab = options.errorStability !== null && options.errorStability !== undefined
          ? options.errorStability
          : (profileId === 'se30' ? 0.45 : null);
        const bleed = options.errorBleed !== null && options.errorBleed !== undefined
          ? options.errorBleed
          : (profileId === 'se30' ? 0.95 : null);
        if (stab !== null) {
          args.push('--error-stability', String(stab));
        }
        if (bleed !== null) {
          args.push('--error-bleed', String(bleed));
        }
        if (options.errorBidi !== null && options.errorBidi !== undefined) {
          args.push('--error-bidi', options.errorBidi ? 'true' : 'false');
        }
      }
      if (options.filters !== null && options.filters !== undefined) {
        args.push('--filters', options.filters);
      }
      if (options.codecMode === 'lines') {
        args.push('--codec', 'lines', '--codec', 'invert');
      } else if (options.codecMode === 'z32') {
        args.push('--codec', 'z32', '--codec', 'invert');
      } else if (options.codecMode === 'z16') {
        args.push('--codec', 'z16', '--codec', 'lines', '--codec', 'invert');
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

    if ((!item.durationSec || !item.width) && tools.ffprobe) {
      const meta = await probeMedia(srcAbsPath, tools.ffprobe);
      if (meta) {
        library.updateItem(item.id, {
          durationSec: item.durationSec || meta.durationSec,
          width: item.width || meta.width,
          height: item.height || meta.height,
        });
      }
    }

    let encodeInputPath = srcAbsPath;
    let tmpScaledPath = null;

    try {
      if (job.options.aspect === 'stretch') {
        if (!tools.ffmpeg) {
          throw new Error('ffmpeg is required for stretch aspect scaling');
        }
        tmpScaledPath = path.join(library.dirs.tmp, `${job.id}_stretch.mov`);
        job.message = 'Pre-scaling video to 512x342 (stretch)...';
        await preScaleStretch(srcAbsPath, tmpScaledPath, tools.ffmpeg, job);
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
          options: job.options,
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
          silent: parsed.info.silent,
          aspect: job.options.aspect,
          dither: job.options.dither,
          fpsRatio: job.options.fpsRatio,
          group: job.options.group,
          errorAlgorithm: job.options.errorAlgorithm,
          errorStability: job.options.errorStability,
          errorBleed: job.options.errorBleed,
          codecMode: job.options.codecMode || 'auto',
          filters: job.options.filters,
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
  isFlimFile,
  createEncoder,
};
