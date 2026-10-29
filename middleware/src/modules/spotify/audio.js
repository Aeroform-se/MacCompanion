const net = require('net');
const fs = require('fs');
const path = require('path');
const { spawn, execSync } = require('child_process');
const { EventEmitter } = require('events');
const { Transform } = require('stream');
const { DATA_DIR } = require('../../core/storage');
const audioHub = require('../../core/audio-hub');
const {
  generateCodeVerifier,
  generateCodeChallenge,
  generateState,
  exchangeCodeForTokens,
  refreshAccessToken,
} = require('./pkce');

const DEFAULT_AUDIO_TCP_PORT = parseInt(process.env.AUDIO_TCP_PORT || '6802', 10);
const DEVICE_NAME = process.env.SPOTIFY_DEVICE_NAME || 'Macintosh 68k';

// Official librespot OAuth Client ID & Scopes required by Spotify's spirc / Connect dealer
const LIBRESPOT_CLIENT_ID = '65b708073fc0480ea92a077233ca87bd';
const LIBRESPOT_REDIRECT_URI = process.env.LIBRESPOT_REDIRECT_URI || 'http://127.0.0.1:6800/login';
const LIBRESPOT_SCOPES = [
  'app-remote-control',
  'playlist-modify',
  'playlist-modify-private',
  'playlist-modify-public',
  'playlist-read',
  'playlist-read-collaborative',
  'playlist-read-private',
  'streaming',
  'ugc-image-upload',
  'user-follow-modify',
  'user-follow-read',
  'user-library-modify',
  'user-library-read',
  'user-modify',
  'user-modify-playback-state',
  'user-modify-private',
  'user-personalized',
  'user-read-birthdate',
  'user-read-currently-playing',
  'user-read-email',
  'user-read-play-history',
  'user-read-playback-position',
  'user-read-playback-state',
  'user-read-private',
  'user-read-recently-played',
  'user-top-read',
];

// Exact Apple Sound Chip (ASC) native sample rates in Hz:
// - 22,254.54 Hz (rate22khz = 0x56EE8BA3) -> 22255 Hz
// - 11,127.27 Hz (rate11khz = 0x2B7745D1) -> 11127 Hz
const SAMPLE_RATES = {
  '22k': 22255,
  '11k': 11127,
  22255: 22255,
  11127: 11127,
  22: 22255,
  11: 11127,
};

function resolveSampleRateHz(rateSetting) {
  const key = String(rateSetting || '22k').toLowerCase().trim();
  if (key.startsWith('11')) return 11127;
  return SAMPLE_RATES[key] || 22255;
}

/**
 * Curated 8-Bit Sound Profiles for SpotiMac:
 *  - warm:    (Default) Lowest possible noise floor in quiet passages via gentle dynamic compression + 9.5 kHz HF roll-off
 *  - hifi:    Maximum dynamic range & natural transients with Shibata psychoacoustic noise-shaped dither
 *  - compact: Tailored for the internal 2.25" speaker in a Compact Mac (140 Hz sub-cut + 2.2 kHz presence + leveling)
 */
const SOUND_PROFILES = {
  hifi: {
    id: 'hifi',
    label: 'HiFi / Pure Dynamic',
    description: 'Maximum dynamic range and natural transients with Shibata noise-shaped dither. Best at 100% client volume.',
    dsp: {
      resampler: 'soxr',
      ditherMethod: 'shibata',
      ditherScale: 1.0,
      compressorEnabled: false,
      compThresholdDb: -20,
      compRatio: 3.0,
      compMakeupDb: 4,
      highpassHz: 0,
      lowpassHz: 0,
      eqPresenceDb: 0,
      pregainDb: 0,
      limiterEnabled: false,
    },
  },
  warm: {
    id: 'warm',
    label: 'Low Noise Floor (Warm)',
    description: 'Minimizes 8-bit background hiss in quiet passages using gentle dynamic range compression, +5 dB makeup gain, and a 9.5 kHz low-pass filter.',
    dsp: {
      resampler: 'soxr',
      ditherMethod: 'none',
      ditherScale: 0.5,
      compressorEnabled: true,
      compThresholdDb: -20,
      compRatio: 3.0,
      compMakeupDb: 5,
      highpassHz: 0,
      lowpassHz: 9500,
      eqPresenceDb: 0,
      pregainDb: 0,
      limiterEnabled: true,
    },
  },
  compact: {
    id: 'compact',
    label: 'Compact Mac Speaker (1984 Punch)',
    description: 'Tailored for the internal speaker in a Plus/SE/Classic: rolls off sub-140 Hz cone rumble, boosts 2.2 kHz midrange presence, and levels dynamics.',
    dsp: {
      resampler: 'soxr',
      ditherMethod: 'shibata',
      ditherScale: 1.0,
      compressorEnabled: true,
      compThresholdDb: -18,
      compRatio: 3.5,
      compMakeupDb: 4,
      highpassHz: 140,
      lowpassHz: 0,
      eqPresenceDb: 3,
      pregainDb: 0,
      limiterEnabled: true,
    },
  },
};

const VALID_DITHER_METHODS = new Set([
  'none',
  'rectangular',
  'triangular',
  'triangular_hp',
  'lipshitz',
  'shibata',
  'low_shibata',
  'high_shibata',
  'f_weighted',
  'modified_e_weighted',
  'improved_e_weighted',
]);

function normalizeDspConfig(rawDsp = {}, fallbackProfile = 'warm') {
  const base = (SOUND_PROFILES[fallbackProfile] || SOUND_PROFILES.warm).dsp;
  const merged = { ...base, ...(rawDsp || {}) };

  const resampler = merged.resampler === 'swr' ? 'swr' : 'soxr';
  const ditherMethod = VALID_DITHER_METHODS.has(String(merged.ditherMethod))
    ? String(merged.ditherMethod)
    : base.ditherMethod;
  const ditherScale = Math.max(0, Math.min(1, Number.isFinite(Number(merged.ditherScale)) ? Number(merged.ditherScale) : base.ditherScale));
  const compressorEnabled = Boolean(merged.compressorEnabled);
  const compThresholdDb = Math.max(-36, Math.min(-6, Number.isFinite(Number(merged.compThresholdDb)) ? Number(merged.compThresholdDb) : base.compThresholdDb));
  const compRatio = Math.max(1, Math.min(10, Number.isFinite(Number(merged.compRatio)) ? Number(merged.compRatio) : base.compRatio));
  const compMakeupDb = Math.max(0, Math.min(12, Number.isFinite(Number(merged.compMakeupDb)) ? Number(merged.compMakeupDb) : base.compMakeupDb));
  const highpassHz = Math.max(0, Math.min(400, parseInt(merged.highpassHz ?? base.highpassHz, 10) || 0));
  const lowpassHz = Math.max(0, Math.min(11000, parseInt(merged.lowpassHz ?? base.lowpassHz, 10) || 0));
  const eqPresenceDb = Math.max(-8, Math.min(8, Number.isFinite(Number(merged.eqPresenceDb)) ? Number(merged.eqPresenceDb) : base.eqPresenceDb));
  const pregainDb = Math.max(-8, Math.min(8, Number.isFinite(Number(merged.pregainDb)) ? Number(merged.pregainDb) : base.pregainDb));
  const limiterEnabled = Boolean(merged.limiterEnabled);

  return {
    resampler,
    ditherMethod,
    ditherScale,
    compressorEnabled,
    compThresholdDb,
    compRatio,
    compMakeupDb,
    highpassHz,
    lowpassHz,
    eqPresenceDb,
    pregainDb,
    limiterEnabled,
  };
}

function buildFfmpegFilterChain(hz, dspConfig) {
  const dsp = normalizeDspConfig(dspConfig);
  const chain = [];

  if (dsp.pregainDb !== 0) {
    chain.push(`volume=${dsp.pregainDb}dB`);
  }
  if (dsp.highpassHz > 0) {
    chain.push(`highpass=f=${dsp.highpassHz}`);
  }
  if (dsp.eqPresenceDb !== 0) {
    chain.push(`equalizer=f=2200:t=q:w=1.0:g=${dsp.eqPresenceDb}`);
  }
  if (dsp.compressorEnabled) {
    chain.push(
      `acompressor=threshold=${dsp.compThresholdDb}dB:ratio=${dsp.compRatio}:attack=15:release=220:makeup=${dsp.compMakeupDb}dB`
    );
  }
  if (dsp.limiterEnabled || dsp.compressorEnabled || dsp.pregainDb > 0 || dsp.eqPresenceDb > 0) {
    chain.push('alimiter=limit=0.95:attack=5:release=50');
  }
  if (dsp.lowpassHz > 0) {
    chain.push(`lowpass=f=${dsp.lowpassHz}`);
  }

  const aresampleParts = [
    `osr=${hz}`,
    `resampler=${dsp.resampler}`,
    `dither_method=${dsp.ditherMethod}`,
  ];
  if (dsp.ditherMethod !== 'none' && dsp.ditherScale !== 1.0) {
    aresampleParts.push(`dither_scale=${dsp.ditherScale.toFixed(2)}`);
  }
  chain.push(`aresample=${aresampleParts.join(':')}`);

  return chain.join(',');
}

/**
 * Builds a standard 44-byte RIFF/WAVE header for unsigned 8-bit mono PCM (`pcm_u8`)
 * so modern browsers and HTML5 `<audio>` elements can play the live 68k audio stream.
 */
function createWavHeader(sampleRate = 22255) {
  const header = Buffer.alloc(44);
  const dataLength = 0x7fffffff; // Streaming indeterminate length
  header.write('RIFF', 0);
  header.writeUInt32LE(dataLength + 36, 4);
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16); // PCM chunk size
  header.writeUInt16LE(1, 20);  // AudioFormat = 1 (PCM)
  header.writeUInt16LE(1, 22);  // NumChannels = 1 (Mono)
  header.writeUInt32LE(sampleRate, 24); // SampleRate
  header.writeUInt32LE(sampleRate, 28); // ByteRate (SampleRate * 1 * 1)
  header.writeUInt16LE(1, 32);  // BlockAlign = 1
  header.writeUInt16LE(8, 34);  // BitsPerSample = 8 (unsigned 8-bit)
  header.write('data', 36);
  header.writeUInt32LE(dataLength, 40);
  return header;
}

/**
 * 1.000x hardware-clock emulator for `librespot --backend pipe` with strict 4-byte
 * stereo frame alignment.
 *
 * Ensures `librespot` decodes at exact real-time speed (`176,400 bytes/sec`) while
 * guaranteeing that every slice fed into `ffmpeg` is an exact multiple of 4 bytes
 * (`[L_lo, L_hi, R_lo, R_hi]`), preventing byte-misalignment static bursts.
 */
const INPUT_SAMPLE_RATE = 44100;
const LIBRESPOT_BYTES_PER_SEC = INPUT_SAMPLE_RATE * 2 * 2; // 176,400 bytes/sec
const FRAME_BYTES = 3528; // Exact 20ms frame (882 stereo samples * 4 bytes)

class RealTimePacer extends Transform {
  constructor() {
    super({ highWaterMark: 7056 });
    this.nextFrameTimeMs = 0;
    this.timer = null;
    this.activeDoneCallback = null;
    this.stopped = false;
    this.alignRemainder = Buffer.alloc(0);
    this.flushEpoch = 0;
  }

  flush() {
    this.flushEpoch++;
    this.nextFrameTimeMs = 0;
    // NOTE: Never clear `this.alignRemainder` here — it holds the 1..3 byte sub-frame
    // remainder from the last OS pipe read on `librespot.stdout` and is required to
    // keep 4-byte S16_LE stereo sample alignment intact across pause/resume.
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    if (this.activeDoneCallback) {
      const cb = this.activeDoneCallback;
      this.activeDoneCallback = null;
      cb();
    }
  }

  _transform(chunk, encoding, callback) {
    if (this.stopped) return callback();

    const combined = this.alignRemainder.length > 0
      ? Buffer.concat([this.alignRemainder, chunk])
      : chunk;

    const alignedLen = combined.length - (combined.length % 4);
    if (alignedLen < combined.length) {
      this.alignRemainder = combined.subarray(alignedLen);
    } else {
      this.alignRemainder = Buffer.alloc(0);
    }

    if (alignedLen === 0) {
      return callback();
    }

    let offset = 0;
    const myEpoch = this.flushEpoch;
    this.activeDoneCallback = callback;

    const pumpNextFrame = () => {
      if (this.stopped || myEpoch !== this.flushEpoch) {
        this.timer = null;
        return;
      }
      if (offset >= alignedLen) {
        this.timer = null;
        if (this.activeDoneCallback === callback) {
          this.activeDoneCallback = null;
          callback();
        }
        return;
      }

      const end = Math.min(offset + FRAME_BYTES, alignedLen);
      const slice = combined.subarray(offset, end);
      offset = end;

      const durationMs = (slice.length / LIBRESPOT_BYTES_PER_SEC) * 1000;
      const now = performance.now();

      // If starting fresh or resuming after a pause (> 500ms gap), allow a 300ms pre-fill burst
      // so the 68k client's double-buffer fills immediately without initial underrun jitter
      if (now - this.nextFrameTimeMs > 500) {
        this.nextFrameTimeMs = now - 300;
      }

      const delayMs = Math.max(0, this.nextFrameTimeMs - now);
      this.nextFrameTimeMs += durationMs;

      if (delayMs <= 1) {
        this.push(slice);
        setImmediate(pumpNextFrame);
      } else {
        this.timer = setTimeout(() => {
          if (!this.stopped && myEpoch === this.flushEpoch) {
            this.push(slice);
            pumpNextFrame();
          }
        }, delayMs);
      }
    };

    pumpNextFrame();
  }

  _destroy(err, callback) {
    this.stopped = true;
    if (this.timer) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    callback(err);
  }
}

function cleanIpAddress(rawIp = '') {
  const str = String(rawIp || '').trim();
  if (str.startsWith('::ffff:')) return str.slice(7);
  return str;
}

class AudioStreamEngine extends EventEmitter {
  constructor({ store, getValidAccessToken, getFetchImpl }) {
    super();
    this.store = store;
    this.getValidAccessToken = getValidAccessToken;
    this.getFetchImpl = getFetchImpl || (() => global.fetch);
    this.librespotProc = null;
    this.ffmpegProc = null;
    this.pacer = null;
    this.tcpServer = null;
    this.tcpClients = new Set();
    this.httpClients = new Set();
    this.tcpPendingChunks = [];
    this.tcpPendingBytes = 0;
    this.tcpFlushTimer = null;
    this.currentRateHz = 22255;
    this.currentFilterChain = '';
    this.state = 'STOPPED'; // STOPPED | AWAITING_SPEAKER_AUTH | STARTING | READY | PLAYING | UNAVAILABLE
    this.lastError = null;
    this.bytesBroadcast = 0;
    this.hasBinaries = this.checkBinaries();

    // Multi-Mac network speaker registry: Map<clientId, { id, ip, baseName, lastSeen }>
    this.macClients = new Map();
    this.activeMacClientId = null;
  }

  checkBinaries() {
    try {
      execSync('which librespot && which ffmpeg', { stdio: 'ignore' });
      return true;
    } catch {
      return false;
    }
  }

  getSampleRateConfig() {
    const data = this.store.load();
    const mode = data.audioSampleRate === '11k' ? '11k' : '22k';
    const hz = resolveSampleRateHz(mode);
    return { mode, hz };
  }

  getDspConfig() {
    const data = this.store.load();
    const profile = (data.audioProfile && (SOUND_PROFILES[data.audioProfile] || data.audioProfile === 'custom'))
      ? data.audioProfile
      : 'warm';
    const fallbackProfile = SOUND_PROFILES[profile] ? profile : 'warm';
    const dsp = normalizeDspConfig(data.audioDsp || {}, fallbackProfile);
    const { hz } = this.getSampleRateConfig();
    const filterChain = buildFfmpegFilterChain(hz, dsp);
    return { profile, dsp, filterChain };
  }

  /**
   * Registers or refreshes an active 68k Mac client on the network so multiple Macs
   * each appear with their own speaker name in the speaker chooser.
   */
  registerMacClient({ id, ip, name }) {
    const cleanIp = cleanIpAddress(ip);
    const baseName = String(name || '').trim() || DEVICE_NAME;
    const clientId = String(id || cleanIp || baseName)
      .replace(/[^A-Za-z0-9._-]/g, '_')
      .slice(0, 48);

    if (!clientId) return null;

    const now = Date.now();
    // Prune stale Mac entries older than 10 minutes (except the currently active Mac)
    for (const [k, entry] of this.macClients.entries()) {
      if (k !== this.activeMacClientId && now - entry.lastSeen > 10 * 60 * 1000) {
        this.macClients.delete(k);
      }
    }

    this.macClients.set(clientId, {
      id: clientId,
      ip: cleanIp,
      baseName,
      lastSeen: now,
    });

    if (!this.activeMacClientId || !this.macClients.has(this.activeMacClientId)) {
      this.activeMacClientId = clientId;
    }

    return this.getMacClientList().find((m) => m.id === clientId) || null;
  }

  /**
   * Returns all registered 68k Macs with disambiguated speaker names if two Macs
   * share the same base name on different IPs.
   */
  getMacClientList() {
    const entries = Array.from(this.macClients.values());
    const nameCounts = new Map();
    for (const e of entries) {
      const key = e.baseName.toLowerCase();
      nameCounts.set(key, (nameCounts.get(key) || 0) + 1);
    }

    return entries.map((e) => {
      const duplicate = (nameCounts.get(e.baseName.toLowerCase()) || 0) > 1;
      let suffix = e.id;
      if (e.ip && e.ip.includes('.')) {
        suffix = `.${e.ip.split('.').pop()}`;
      }
      const displayName = duplicate ? `${e.baseName} (${suffix})` : e.baseName;
      return {
        id: e.id,
        deviceId: `mac68k:${e.id}`,
        ip: e.ip,
        name: displayName,
        baseName: e.baseName,
        isActive: e.id === this.activeMacClientId,
        lastSeen: e.lastSeen,
      };
    });
  }

  setActiveMacClient(targetIdOrName) {
    const needle = String(targetIdOrName || '').trim();
    if (!needle) return null;
    const stripped = needle.startsWith('mac68k:') ? needle.slice(7) : needle;
    const list = this.getMacClientList();

    const found = list.find(
      (m) =>
        m.id === stripped ||
        m.deviceId === needle ||
        (m.ip && m.ip === stripped) ||
        m.name.toLowerCase() === stripped.toLowerCase() ||
        m.baseName.toLowerCase() === stripped.toLowerCase()
    );

    if (found) {
      this.activeMacClientId = found.id;
      return found;
    }
    return null;
  }

  getActiveMacClient() {
    const list = this.getMacClientList();
    if (list.length === 0) return null;
    return list.find((m) => m.id === this.activeMacClientId) || list[0];
  }

  getActiveMacSpeakerName() {
    const active = this.getActiveMacClient();
    return active ? active.name : DEVICE_NAME;
  }

  clearTcpPending() {
    this.tcpPendingChunks = [];
    this.tcpPendingBytes = 0;
    if (this.tcpFlushTimer) {
      clearTimeout(this.tcpFlushTimer);
      this.tcpFlushTimer = null;
    }
  }

  flushTcpPending() {
    if (this.tcpFlushTimer) {
      clearTimeout(this.tcpFlushTimer);
      this.tcpFlushTimer = null;
    }
    if (this.tcpPendingBytes <= 0 || this.tcpPendingChunks.length === 0) {
      return;
    }
    const payload = this.tcpPendingChunks.length === 1
      ? this.tcpPendingChunks[0]
      : Buffer.concat(this.tcpPendingChunks, this.tcpPendingBytes);
    this.tcpPendingChunks = [];
    this.tcpPendingBytes = 0;

    const activeMac = this.getActiveMacClient();
    const targetIp = activeMac && activeMac.ip ? activeMac.ip : null;
    const hasMatchingSocket = targetIp
      ? Array.from(this.tcpClients).some((s) => !s.destroyed && cleanIpAddress(s.remoteAddress) === targetIp)
      : false;

    for (const sock of this.tcpClients) {
      if (!sock.destroyed && sock.writable) {
        if (hasMatchingSocket && cleanIpAddress(sock.remoteAddress) !== targetIp) {
          continue;
        }
        sock.write(payload);
      }
    }
  }

  onPause() {
    this.clearTcpPending();
    if (this.pacer && typeof this.pacer.flush === 'function') {
      this.pacer.flush();
    }
  }

  isPaired() {
    const data = this.store.load();
    return Boolean(data.librespotRefreshToken || data.librespotAccessToken);
  }

  /**
   * Builds the Spotify OAuth URL specifically for `librespot` speaker registration
   * (`client_id = 65b708073fc0480ea92a077233ca87bd`, `redirect_uri = http://127.0.0.1:6800/login`).
   */
  buildSpeakerAuthorizeUrl() {
    const codeVerifier = generateCodeVerifier();
    const codeChallenge = generateCodeChallenge(codeVerifier);
    const state = generateState();

    this.store.save({
      librespotCodeVerifier: codeVerifier,
      librespotOauthState: state,
    });

    const params = new URLSearchParams({
      response_type: 'code',
      client_id: LIBRESPOT_CLIENT_ID,
      state,
      code_challenge: codeChallenge,
      code_challenge_method: 'S256',
      redirect_uri: LIBRESPOT_REDIRECT_URI,
      scope: LIBRESPOT_SCOPES.join(' '),
    });

    return `https://accounts.spotify.com/authorize?${params.toString()}`;
  }

  /**
   * Exchanges the authorization code from `http://127.0.0.1:6800/login?code=...`
   * for a `librespot`-compatible token and immediately starts the `"Macintosh 68k"` receiver.
   */
  async handleSpeakerCallback({ code, state, error }) {
    if (error) {
      throw new Error(`Speaker authorization denied: ${error}`);
    }
    if (!code) {
      throw new Error('Missing authorization code for speaker');
    }

    const stored = this.store.load();
    if (!stored.librespotCodeVerifier) {
      throw new Error('No pending speaker login found. Please click Activate Speaker again.');
    }

    const tokens = await exchangeCodeForTokens({
      clientId: LIBRESPOT_CLIENT_ID,
      code,
      redirectUri: LIBRESPOT_REDIRECT_URI,
      codeVerifier: stored.librespotCodeVerifier,
      fetchImpl: this.getFetchImpl(),
    });

    this.clearCachedCredentials();

    this.store.save({
      librespotAccessToken: tokens.accessToken,
      librespotRefreshToken: tokens.refreshToken,
      librespotExpiresAt: tokens.expiresAt,
      librespotCodeVerifier: null,
      librespotOauthState: null,
    });

    await this.ensureStarted({ forceRestart: true });
    return { ok: true };
  }

  async getValidLibrespotToken() {
    const data = this.store.load();
    if (!data.librespotRefreshToken && !data.librespotAccessToken) {
      return null;
    }

    const now = Date.now();
    if (data.librespotAccessToken && data.librespotExpiresAt && now < data.librespotExpiresAt - 5 * 60 * 1000) {
      return data.librespotAccessToken;
    }

    if (!data.librespotRefreshToken) {
      return data.librespotAccessToken || null;
    }

    console.log('[AudioEngine] Refreshing librespot speaker access token...');
    const refreshed = await refreshAccessToken({
      clientId: LIBRESPOT_CLIENT_ID,
      refreshToken: data.librespotRefreshToken,
      fetchImpl: this.getFetchImpl(),
    });

    this.store.save({
      librespotAccessToken: refreshed.accessToken,
      librespotRefreshToken: refreshed.refreshToken,
      librespotExpiresAt: refreshed.expiresAt,
    });

    return refreshed.accessToken;
  }

  clearCachedCredentials() {
    try {
      const credFile = path.join(DATA_DIR, 'librespot', 'credentials.json');
      if (fs.existsSync(credFile)) {
        fs.unlinkSync(credFile);
      }
    } catch {}
  }

  /**
   * Spawns (or hot-restarts on the fly) the low-latency FFmpeg resampler + DSP filter chain
   * without restarting `librespot` or dropping active TCP/HTTP audio clients.
   */
  spawnFfmpegTranscoder(hz) {
    if (this.ffmpegProc) {
      if (this.pacer && this.ffmpegProc.stdin) {
        try {
          this.pacer.unpipe(this.ffmpegProc.stdin);
        } catch {}
      }
      try {
        this.ffmpegProc.kill('SIGTERM');
      } catch {}
      this.ffmpegProc = null;
    }

    const { profile, dsp } = this.getDspConfig();
    const filterChain = buildFfmpegFilterChain(hz, dsp);

    this.currentRateHz = hz;
    this.currentFilterChain = filterChain;

    console.log(`[AudioEngine] FFmpeg DSP active (profile="${profile}", ${hz} Hz): -af "${filterChain}"`);

    const ffmpegArgs = [
      '-hide_banner',
      '-loglevel', 'error',
      '-probesize', '32',
      '-analyzeduration', '0',
      '-fflags', 'nobuffer',
      '-flags', 'low_delay',
      '-f', 's16le',
      '-ar', '44100',
      '-ac', '2',
      '-i', 'pipe:0',
      '-af', filterChain,
      '-f', 'u8',
      '-acodec', 'pcm_u8',
      '-ac', '1',
      '-ar', String(hz),
      '-avioflags', 'direct',
      '-flush_packets', '1',
      'pipe:1',
    ];

    const proc = spawn('ffmpeg', ffmpegArgs, {
      stdio: ['pipe', 'pipe', 'pipe'],
    });
    this.ffmpegProc = proc;

    proc.stdin.on('error', () => {});

    proc.stdout.on('data', (u8Chunk) => {
      if (audioHub.isTtsActive()) {
        return;
      }
      this.bytesBroadcast += u8Chunk.length;
      if (this.state !== 'PLAYING') {
        this.state = 'PLAYING';
      }

      // Coalesce outgoing TCP chunks to ~1460 bytes (1 Ethernet MSS) or 60ms max delay
      // to cut MacTCP / Ethernet receive interrupt overhead on the 68k Mac by 3-4x
      if (this.tcpClients.size > 0) {
        this.tcpPendingChunks.push(u8Chunk);
        this.tcpPendingBytes += u8Chunk.length;
        if (this.tcpPendingBytes >= 1460) {
          this.flushTcpPending();
        } else if (!this.tcpFlushTimer) {
          this.tcpFlushTimer = setTimeout(() => this.flushTcpPending(), 60);
        }
      }

      for (const res of this.httpClients) {
        if (!res.writableEnded) {
          res.write(u8Chunk);
        }
      }
    });

    proc.stderr.on('data', (data) => {
      const msg = data.toString().trim();
      if (msg) console.warn('[AudioEngine:ffmpeg]', msg);
    });

    proc.on('exit', () => {
      if (this.ffmpegProc === proc) {
        this.ffmpegProc = null;
      }
    });

    if (this.pacer) {
      this.pacer.pipe(proc.stdin);
    }
  }

  /**
   * Starts or restarts the `librespot` -> `RealTimePacer` -> `ffmpeg` pipeline.
   */
  async ensureStarted({ forceRestart = false } = {}) {
    if (!this.hasBinaries) {
      this.state = 'UNAVAILABLE';
      return false;
    }

    const { hz } = this.getSampleRateConfig();
    const { filterChain } = this.getDspConfig();

    let speakerToken = null;
    try {
      speakerToken = await this.getValidLibrespotToken();
    } catch (err) {
      console.warn('[AudioEngine] Failed to refresh speaker token:', err.message);
    }

    if (!speakerToken) {
      this.clearCachedCredentials();
      this.stopProcesses();
      this.state = 'AWAITING_SPEAKER_AUTH';
      return false;
    }

    if (!forceRestart && this.librespotProc && this.pacer) {
      if (!this.ffmpegProc || this.currentRateHz !== hz || this.currentFilterChain !== filterChain) {
        this.spawnFfmpegTranscoder(hz);
      }
      return true;
    }

    this.stopProcesses();
    this.state = 'STARTING';

    const cacheDir = path.join(DATA_DIR, 'librespot');
    if (!fs.existsSync(cacheDir)) {
      fs.mkdirSync(cacheDir, { recursive: true });
    }

    const librespotArgs = [
      '--name', DEVICE_NAME,
      '--device-type', 'computer',
      '--backend', 'pipe',
      '--bitrate', '320',
      '--format', 'S16',
      '--dither', 'none',
      '--volume-ctrl', 'cubic',
      '--volume-range', '30',
      '--system-cache', cacheDir,
      '--disable-audio-cache',
      '--initial-volume', '100',
      '--access-token', speakerToken,
    ];

    console.log(`[AudioEngine] Starting librespot ("${DEVICE_NAME}") -> FFmpeg (8-bit unsigned mono @ ${hz} Hz)`);
    const lsProc = spawn('librespot', librespotArgs, {
      stdio: ['ignore', 'pipe', 'pipe'],
    });
    this.librespotProc = lsProc;

    this.pacer = new RealTimePacer();
    this.pacer.on('error', () => {});
    lsProc.stdout.pipe(this.pacer);

    this.spawnFfmpegTranscoder(hz);

    lsProc.stderr.on('data', (data) => {
      const line = data.toString().trim();
      if (line.includes('Authenticated as') || line.includes('Country:')) {
        this.state = 'READY';
        this.lastError = null;
      }
      if (line.includes('INVALID_CREDENTIALS') || line.includes('Bad credentials')) {
        console.warn('[AudioEngine:librespot] Credentials rejected; clearing cache:', line);
        this.clearCachedCredentials();
        this.lastError = 'Speaker credentials rejected by Spotify';
      } else if (line.includes('ERROR') || line.includes('error')) {
        console.warn('[AudioEngine:librespot]', line);
      }
    });

    lsProc.on('exit', (code) => {
      console.log(`[AudioEngine] librespot exited with code ${code}`);
      if (this.librespotProc === lsProc) {
        this.librespotProc = null;
        if (code !== 0 && this.lastError) {
          this.state = 'AWAITING_SPEAKER_AUTH';
        } else if (this.state !== 'STOPPED') {
          // Auto-restart librespot if it exited unexpectedly
          setTimeout(() => {
            if (this.state !== 'STOPPED') {
              this.ensureStarted({ forceRestart: true }).catch(() => {});
            }
          }, 1500);
        }
      }
    });

    this.state = 'READY';
    return true;
  }

  /**
   * Updates sample rate ('22k' | '11k'), preset profile ('hifi' | 'warm' | 'compact' | 'custom'),
   * and/or granular DSP settings, hot-swapping FFmpeg on the fly without interrupting playback.
   */
  async updateAudioConfig(patch = {}) {
    const currentData = this.store.load();
    const updates = {};

    if (patch.rate !== undefined) {
      updates.audioSampleRate = String(patch.rate).toLowerCase().startsWith('11') ? '11k' : '22k';
    }

    let targetProfile = currentData.audioProfile || 'hifi';
    let baseDsp = currentData.audioDsp || SOUND_PROFILES.hifi.dsp;

    if (patch.profile && SOUND_PROFILES[patch.profile]) {
      targetProfile = patch.profile;
      baseDsp = { ...SOUND_PROFILES[patch.profile].dsp };
    } else if (patch.profile === 'custom') {
      targetProfile = 'custom';
    }

    if (patch.dsp && typeof patch.dsp === 'object') {
      if (!patch.profile) {
        targetProfile = 'custom';
      }
      baseDsp = { ...baseDsp, ...patch.dsp };
    }

    updates.audioProfile = targetProfile;
    updates.audioDsp = normalizeDspConfig(baseDsp, SOUND_PROFILES[targetProfile] ? targetProfile : 'hifi');

    this.store.save(updates);

    const { hz } = this.getSampleRateConfig();
    if (this.librespotProc && this.pacer) {
      this.spawnFfmpegTranscoder(hz);
    } else {
      await this.ensureStarted();
    }
    return this.getStatus();
  }

  /**
   * Changes the active sample rate ('22k' or '11k') and hot-swaps FFmpeg without restarting librespot.
   */
  async setSampleRate(rateMode) {
    return this.updateAudioConfig({ rate: rateMode });
  }

  /**
   * Starts the raw TCP audio server on port 6802 for 68k Macintosh clients.
   */
  startTcpServer(port = DEFAULT_AUDIO_TCP_PORT) {
    if (this.tcpServer) return this.tcpServer;

    this.tcpServer = net.createServer((socket) => {
      socket.setNoDelay(true);
      this.tcpClients.add(socket);
      console.log(`[AudioEngine] 68k TCP client connected (${socket.remoteAddress}:${socket.remotePort}) — active clients: ${this.tcpClients.size}`);

      this.ensureStarted().catch(() => {});

      socket.on('close', () => {
        this.tcpClients.delete(socket);
        console.log(`[AudioEngine] 68k TCP client disconnected — active clients: ${this.tcpClients.size}`);
      });

      socket.on('error', () => {
        this.tcpClients.delete(socket);
      });
    });

    this.tcpServer.listen(port, '0.0.0.0', () => {
      console.log(`[AudioEngine] Raw 8-bit PCM TCP server listening on 0.0.0.0:${port}`);
    });

    return this.tcpServer;
  }

  /**
   * Attaches an Express HTTP response as a live streaming client:
   * - `format = 'wav'`: Prepends a 44-byte WAV header so modern browsers can play the exact 8-bit stream
   * - `format = 'raw'`: Streams pure headerless unsigned 8-bit PCM bytes
   */
  async attachHttpClient(req, res, { format = 'wav' } = {}) {
    await this.ensureStarted();
    const { hz } = this.getSampleRateConfig();

    res.setHeader('Cache-Control', 'no-store');
    res.setHeader('Connection', 'keep-alive');
    res.setHeader('X-Audio-Sample-Rate', String(hz));
    res.setHeader('X-Audio-Bits', '8');
    res.setHeader('X-Audio-Channels', '1');

    if (res.socket && res.socket.setNoDelay) {
      res.socket.setNoDelay(true);
    }

    if (format === 'wav') {
      res.setHeader('Content-Type', 'audio/wav');
      res.write(createWavHeader(hz));
    } else {
      res.setHeader('Content-Type', 'application/octet-stream');
    }

    this.httpClients.add(res);
    req.on('close', () => {
      this.httpClients.delete(res);
    });
  }

  stopProcesses() {
    this.clearTcpPending();
    if (this.pacer) {
      try {
        this.pacer.destroy();
      } catch {}
      this.pacer = null;
    }
    if (this.ffmpegProc) {
      try {
        this.ffmpegProc.kill('SIGTERM');
      } catch {}
      this.ffmpegProc = null;
    }
    if (this.librespotProc) {
      try {
        this.librespotProc.kill('SIGTERM');
      } catch {}
      this.librespotProc = null;
    }
  }

  close() {
    this.state = 'STOPPED';
    this.stopProcesses();
    if (this.tcpServer) {
      this.tcpServer.close();
      this.tcpServer = null;
    }
  }

  getStatus() {
    const { mode, hz } = this.getSampleRateConfig();
    const { profile, dsp, filterChain } = this.getDspConfig();
    const macClients = this.getMacClientList();
    return {
      deviceName: this.getActiveMacSpeakerName(),
      receiverDeviceName: DEVICE_NAME,
      available: this.hasBinaries,
      paired: this.isPaired(),
      running: Boolean(this.librespotProc),
      state: this.state,
      sampleRateMode: mode,
      sampleRateHz: hz,
      profile,
      profiles: Object.values(SOUND_PROFILES).map(({ id, label, description, dsp: presetDsp }) => ({
        id,
        label,
        description,
        dsp: presetDsp,
      })),
      dsp,
      filterChain,
      tcpPort: DEFAULT_AUDIO_TCP_PORT,
      activeTcpClients: this.tcpClients.size,
      activeHttpClients: this.httpClients.size,
      bytesBroadcast: this.bytesBroadcast,
      macClients,
      activeMacClientId: this.activeMacClientId,
    };
  }
}

module.exports = {
  DEFAULT_AUDIO_TCP_PORT,
  DEVICE_NAME,
  LIBRESPOT_CLIENT_ID,
  LIBRESPOT_REDIRECT_URI,
  SOUND_PROFILES,
  resolveSampleRateHz,
  createWavHeader,
  buildFfmpegFilterChain,
  AudioStreamEngine,
};
