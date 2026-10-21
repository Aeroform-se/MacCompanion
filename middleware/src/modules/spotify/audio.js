const net = require('net');
const fs = require('fs');
const path = require('path');
const { spawn, execSync } = require('child_process');
const { EventEmitter } = require('events');
const { Transform } = require('stream');
const { DATA_DIR } = require('../../core/storage');
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
    this.stopped = false;
    this.alignRemainder = Buffer.alloc(0);
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
    const pumpNextFrame = () => {
      if (this.stopped || offset >= alignedLen) {
        this.timer = null;
        return callback();
      }

      const end = Math.min(offset + FRAME_BYTES, alignedLen);
      const slice = combined.subarray(offset, end);
      offset = end;

      const durationMs = (slice.length / LIBRESPOT_BYTES_PER_SEC) * 1000;
      const now = performance.now();

      // If starting fresh or resuming after a pause (> 200ms gap), reset clock
      if (now - this.nextFrameTimeMs > 200) {
        this.nextFrameTimeMs = now;
      }

      const delayMs = Math.max(0, this.nextFrameTimeMs - now);
      this.nextFrameTimeMs += durationMs;

      if (delayMs <= 1) {
        this.push(slice);
        setImmediate(pumpNextFrame);
      } else {
        this.timer = setTimeout(() => {
          if (!this.stopped) {
            this.push(slice);
          }
          pumpNextFrame();
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
    this.currentRateHz = 22255;
    this.state = 'STOPPED'; // STOPPED | AWAITING_SPEAKER_AUTH | STARTING | READY | PLAYING | UNAVAILABLE
    this.lastError = null;
    this.bytesBroadcast = 0;
    this.hasBinaries = this.checkBinaries();
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
   * Spawns (or hot-restarts) the low-latency FFmpeg resampler with triangular dithering
   * without restarting `librespot`.
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

    this.currentRateHz = hz;

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
      '-af', `aresample=osr=${hz}:dither_method=triangular`,
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
      this.bytesBroadcast += u8Chunk.length;
      if (this.state !== 'PLAYING') {
        this.state = 'PLAYING';
      }
      for (const sock of this.tcpClients) {
        if (!sock.destroyed && sock.writable) {
          sock.write(u8Chunk);
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
      if (!this.ffmpegProc || this.currentRateHz !== hz) {
        console.log(`[AudioEngine] Switching FFmpeg transcoder to ${hz} Hz`);
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
      '--system-cache', cacheDir,
      '--disable-audio-cache',
      '--initial-volume', '85',
      '--access-token', speakerToken,
    ];

    console.log(`[AudioEngine] Starting librespot ("${DEVICE_NAME}") -> FFmpeg (8-bit unsigned mono @ ${hz} Hz, triangular dither, direct flush)`);
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
   * Changes the active sample rate ('22k' or '11k') and hot-swaps FFmpeg without restarting librespot.
   */
  async setSampleRate(rateMode) {
    const normalized = String(rateMode).toLowerCase().startsWith('11') ? '11k' : '22k';
    this.store.save({ audioSampleRate: normalized });
    const hz = resolveSampleRateHz(normalized);
    if (this.librespotProc && this.pacer) {
      this.spawnFfmpegTranscoder(hz);
    } else {
      await this.ensureStarted();
    }
    return this.getStatus();
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
    return {
      deviceName: DEVICE_NAME,
      available: this.hasBinaries,
      paired: this.isPaired(),
      running: Boolean(this.librespotProc),
      state: this.state,
      sampleRateMode: mode,
      sampleRateHz: hz,
      tcpPort: DEFAULT_AUDIO_TCP_PORT,
      activeTcpClients: this.tcpClients.size,
      activeHttpClients: this.httpClients.size,
      bytesBroadcast: this.bytesBroadcast,
    };
  }
}

module.exports = {
  DEFAULT_AUDIO_TCP_PORT,
  DEVICE_NAME,
  LIBRESPOT_CLIENT_ID,
  LIBRESPOT_REDIRECT_URI,
  resolveSampleRateHz,
  createWavHeader,
  AudioStreamEngine,
};
