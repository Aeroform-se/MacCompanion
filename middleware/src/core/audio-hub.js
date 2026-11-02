const { spawn, execSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

/**
 * Shared 8-Bit PCM Audio Hub (TCP 6802 & HTTP PCM)
 *
 * Coordinates audio streaming across multiple MacCompanion modules (SpotiMac, MacAI, etc.)
 * with two user-selectable modes:
 *   1. Server-Side Audio Ducking / Mixing (Default, `pauseSpotifyOnSpeech = false`):
 *      When SpotiMac is actively streaming music over TCP 6802, MacAI TTS speech is mixed
 *      directly into the live 8-bit PCM music stream on the server while smoothly ducking
 *      music volume to ~22% (-13 dB) and fading music back to 100% when speech ends.
 *      Only a single TCP 6802 stream and Sound Manager channel are used on the 68k Mac!
 *   2. Pause & Auto-Resume SpotiMac on Speech (`pauseSpotifyOnSpeech = true`):
 *      When enabled via MacAI's Speech menu ("Pause SpotiMac on Speech"), SpotiMac playback
 *      is paused before speech starts and automatically resumed when speech finishes.
 */
class SharedAudioHub {
  constructor() {
    this.spotifyEngine = null;
    this.pauseSpotifyFn = null;
    this.resumeSpotifyFn = null;
    this.pauseSpotifyOnSpeech = false; // Default: false = Server-Side Audio Ducking / Mixing
    this.wasSpotifyPausedForSpeech = false;
    this.ttsActiveUntil = 0;
    this.voiceGateActive = false;
    this.activeTtsTimer = null;
    this.ttsDrainTimer = null;
    this.pendingTts = null;

    // Server-Side Audio Ducking / Mixing state
    this.duckQueue = null;
    this.duckOffset = 0;
    this.duckGain = 1.0;
  }

  registerSpotifyEngine(engine, pauseFn, resumeFn) {
    this.spotifyEngine = engine;
    this.pauseSpotifyFn = pauseFn;
    this.resumeSpotifyFn = resumeFn || null;
  }

  setPauseSpotifyOnSpeech(enabled) {
    this.pauseSpotifyOnSpeech = Boolean(enabled);
  }

  getPauseSpotifyOnSpeech() {
    return Boolean(this.pauseSpotifyOnSpeech);
  }

  /**
   * Returns true ONLY when the voice gate is actively blocking Spotify music
   * (i.e. during microphone recording or when `pauseSpotifyOnSpeech` mode is enabled).
   * In default Audio Ducking mode, Spotify music is NEVER blocked or dropped!
   */
  isVoiceGateBlockingSpotify() {
    return Boolean(this.voiceGateActive && Date.now() < this.ttsActiveUntil);
  }

  isTtsActive() {
    return this.isVoiceGateBlockingSpotify();
  }

  hasActiveDuckMix() {
    return Boolean(this.duckQueue && this.duckOffset < this.duckQueue.length);
  }

  /**
   * Mixes any queued TTS speech (`this.duckQueue`) into a live unsigned 8-bit PCM
   * music chunk from `AudioStreamEngine`, smoothly ducking the music to 22% volume
   * during speech and ramping back to 100% after speech completes.
   */
  mixSpotifyChunk(musicU8Chunk) {
    if (!musicU8Chunk || musicU8Chunk.length === 0) return musicU8Chunk;

    const hasTts = Boolean(this.duckQueue && this.duckOffset < this.duckQueue.length);
    if (!hasTts && this.duckGain >= 0.999) {
      return musicU8Chunk;
    }

    const out = Buffer.allocUnsafe(musicU8Chunk.length);
    const targetDuckGain = 0.22; // ~ -13 dB ducked background music during speech
    const attackStep = 0.0025;   // ~14ms smooth duck-down at 22.255 kHz
    const releaseStep = 0.00045; // ~80ms smooth fade-up after speech finishes

    for (let i = 0; i < musicU8Chunk.length; i++) {
      const musicSigned = musicU8Chunk[i] - 128;
      let ttsSigned = 0;

      if (this.duckQueue && this.duckOffset < this.duckQueue.length) {
        if (this.duckGain > targetDuckGain) {
          this.duckGain = Math.max(targetDuckGain, this.duckGain - attackStep);
        }
        ttsSigned = this.duckQueue[this.duckOffset++] - 128;
        if (this.duckOffset >= this.duckQueue.length) {
          this.duckQueue = null;
          this.duckOffset = 0;
        }
      } else if (this.duckGain < 1.0) {
        this.duckGain = Math.min(1.0, this.duckGain + releaseStep);
      }

      const mixed = Math.round(musicSigned * this.duckGain + ttsSigned * 1.05);
      const clamped = mixed < -128 ? -128 : (mixed > 127 ? 127 : mixed);
      out[i] = clamped + 128;
    }

    return out;
  }

  /**
   * Prepares the audio pipe before MacAI starts voice recording or TTS speech.
   * - In default Audio Ducking mode (`pauseSpotifyOnSpeech = false` and `forcePause = false`),
   *   SpotiMac is NOT paused so music continues playing smoothly under the ducked TTS voice.
   * - When `pauseSpotifyOnSpeech = true` (or `forcePause = true` for mic recording),
   *   SpotiMac is paused and will automatically resume after speech finishes.
   */
  async pauseSpotifyForVoice({ forcePause = false } = {}) {
    const shouldPause = Boolean(forcePause || this.pauseSpotifyOnSpeech);
    if (!shouldPause) {
      return;
    }

    this.voiceGateActive = true;
    this.ttsActiveUntil = Date.now() + 15000;

    if (this.spotifyEngine && typeof this.spotifyEngine.onPause === 'function') {
      this.spotifyEngine.onPause({ discardMs: 0 });
    }

    if (typeof this.pauseSpotifyFn === 'function') {
      try {
        const paused = await this.pauseSpotifyFn();
        if (paused) {
          this.wasSpotifyPausedForSpeech = true;
        }
      } catch {}
    }

    if (this.spotifyEngine && typeof this.spotifyEngine.onPause === 'function') {
      this.spotifyEngine.onPause({ discardMs: 0 });
    }
  }

  /**
   * Called whenever SpotiMac playback is resumed (/play, /toggle-play, /next, /previous, /transfer).
   * Immediately cancels any active voice gate so SpotiMac has 100% priority.
   */
  onSpotifyResume() {
    this.wasSpotifyPausedForSpeech = false;
    this.stopTtsStream({ closeTtsSockets: true });
    if (this.spotifyEngine && typeof this.spotifyEngine.onResume === 'function') {
      this.spotifyEngine.onResume();
    }
  }

  /**
   * Stops any in-progress standalone TTS stream.
   */
  stopTtsStream({ closeTtsSockets = false } = {}) {
    this.ttsActiveUntil = 0;
    this.voiceGateActive = false;
    this.pendingTts = null;
    if (this.activeTtsTimer) {
      clearTimeout(this.activeTtsTimer);
      this.activeTtsTimer = null;
    }
    if (this.ttsDrainTimer) {
      clearTimeout(this.ttsDrainTimer);
      this.ttsDrainTimer = null;
    }
    if (closeTtsSockets && this.spotifyEngine && this.spotifyEngine.tcpClients) {
      for (const sock of Array.from(this.spotifyEngine.tcpClients)) {
        if (sock._isTemporaryTtsSocket) {
          this.spotifyEngine.tcpClients.delete(sock);
          try {
            sock.end();
            sock.destroy();
          } catch {}
        }
      }
    }
  }

  /**
   * Called when a 68k client connects to TCP port 6802.
   * If standalone TTS speech audio was synthesized while MacAI was connecting, immediately flush it!
   */
  onClientConnected(socket) {
    if (this.pendingTts && Date.now() < this.pendingTts.expiresAt && this.pendingTts.buffer && this.pendingTts.buffer.length > 0) {
      const { buffer, hz } = this.pendingTts;
      this.pendingTts = null;
      if (socket) {
        socket._isTemporaryTtsSocket = true;
      }
      console.log(`[AudioHub] 68k TCP client connected — streaming ${buffer.length} buffered TTS bytes`);
      this.streamPcmU8ToTcpClients(buffer, { hz, targetSocket: socket });
    }
  }

  /**
   * Streams an unsigned 8-bit mono PCM buffer (`pcm_u8` at `hz` sample rate)
   * to the 68k Mac:
   * - If SpotiMac is actively streaming music and `pauseSpotifyOnSpeech` is false (default),
   *   queues `u8Buffer` into `this.duckQueue` so `mixSpotifyChunk()` mixes it directly into
   *   SpotiMac's live TCP 6802 stream with smooth audio ducking!
   * - Otherwise streams `u8Buffer` at real-time speed over TCP port 6802 to MacAI's socket
   *   (and auto-resumes SpotiMac afterwards if `wasSpotifyPausedForSpeech` is true).
   */
  streamPcmU8ToTcpClients(u8Buffer, { hz = 22255, targetSocket = null } = {}) {
    if (!u8Buffer || u8Buffer.length === 0) {
      this.voiceGateActive = false;
      this.ttsActiveUntil = 0;
      return { streamedBytes: 0, durationMs: 0, mixedIntoSpotify: false };
    }

    const durationMs = Math.ceil((u8Buffer.length / hz) * 1000);

    // 1. Default Server-Side Audio Ducking / Mixing Mode:
    // If SpotiMac is actively streaming music right now and `pauseSpotifyOnSpeech` is false,
    // mix the TTS directly into SpotiMac's live audio stream!
    if (
      !targetSocket &&
      !this.pauseSpotifyOnSpeech &&
      this.spotifyEngine &&
      typeof this.spotifyEngine.isActivelyStreamingMusic === 'function' &&
      this.spotifyEngine.isActivelyStreamingMusic()
    ) {
      this.stopTtsStream();
      if (typeof this.spotifyEngine.clearTcpPending === 'function') {
        this.spotifyEngine.clearTcpPending();
      }
      this.duckQueue = u8Buffer;
      this.duckOffset = 0;
      console.log(`[AudioHub] Mixing ${u8Buffer.length} TTS bytes (${durationMs}ms) into live SpotiMac stream with audio ducking`);
      return { streamedBytes: u8Buffer.length, durationMs, mixedIntoSpotify: true };
    }

    // 2. Standalone TTS Stream over TCP 6802 (when SpotiMac is not playing, or when Pause SpotiMac on Speech is enabled)
    this.stopTtsStream();
    this.duckQueue = null;
    this.duckOffset = 0;

    if (this.pauseSpotifyOnSpeech || this.wasSpotifyPausedForSpeech) {
      this.voiceGateActive = true;
      this.ttsActiveUntil = Date.now() + durationMs + 600;
    }

    // Choose target socket(s): if targetSocket is provided (MacAI just connected), stream to it;
    // otherwise check if there is a connected socket that is NOT an idle background SpotiMac socket when MacAI is about to connect.
    const activeSockets = targetSocket
      ? [targetSocket]
      : (this.spotifyEngine && this.spotifyEngine.tcpClients
          ? Array.from(this.spotifyEngine.tcpClients).filter((s) => !s.destroyed && s.writable && !s._receivedSpotifyBytes)
          : []);

    if (activeSockets.length === 0) {
      console.log(`[AudioHub] Buffering ${u8Buffer.length} TTS bytes (${durationMs}ms) for MacAI TCP :6802 arrival...`);
      this.pendingTts = {
        buffer: u8Buffer,
        hz,
        expiresAt: Date.now() + 12000,
      };
      this.ttsDrainTimer = setTimeout(() => {
        this.ttsDrainTimer = null;
        this.pendingTts = null;
        this.voiceGateActive = false;
        this.ttsActiveUntil = 0;
        if (this.wasSpotifyPausedForSpeech && typeof this.resumeSpotifyFn === 'function') {
          this.wasSpotifyPausedForSpeech = false;
          Promise.resolve(this.resumeSpotifyFn()).catch(() => {});
        }
      }, 12000);
      if (typeof this.ttsDrainTimer.unref === 'function') this.ttsDrainTimer.unref();
      return { streamedBytes: u8Buffer.length, durationMs, mixedIntoSpotify: false };
    }

    // Pre-burst the first 4096 bytes (~184ms) immediately so MacAI's double-buffer starts
    // playing with zero prebuffer latency, then pace remaining frames at ~90ms intervals.
    const initialBurstBytes = Math.min(4096, u8Buffer.length);
    const frameBytes = Math.max(512, Math.round(hz * 0.09)); // ~2000 bytes @ 22k (90ms)
    let offset = 0;

    // If SpotiMac was paused for speech, fire `resumeSpotifyFn()` (`PUT /me/player/play`)
    // ~350ms before the TTS utterance finishes draining on the 68k Mac so Spotify's cloud
    // API latency (~350-450ms) is hidden and music resumes right as speech finishes!
    let resumeTriggered = false;
    const triggerSpotifyResume = () => {
      if (resumeTriggered) return;
      resumeTriggered = true;
      if (this.wasSpotifyPausedForSpeech && typeof this.resumeSpotifyFn === 'function') {
        this.wasSpotifyPausedForSpeech = false;
        Promise.resolve(this.resumeSpotifyFn()).catch(() => {});
      }
    };

    if (this.wasSpotifyPausedForSpeech && durationMs > 450) {
      const earlyResumeDelayMs = Math.max(80, durationMs - 350);
      const earlyTimer = setTimeout(() => {
        if (this.wasSpotifyPausedForSpeech) {
          triggerSpotifyResume();
        }
      }, earlyResumeDelayMs);
      if (typeof earlyTimer.unref === 'function') earlyTimer.unref();
    }

    const finishTtsHandover = () => {
      this.activeTtsTimer = null;
      this.ttsDrainTimer = setTimeout(() => {
        this.ttsDrainTimer = null;
        this.voiceGateActive = false;
        this.ttsActiveUntil = 0;

        // Cleanly close temporary MacAI TTS sockets so MacAI releases its Sound Manager
        // channel and never double-plays when SpotiMac plays music!
        if (this.spotifyEngine && this.spotifyEngine.tcpClients) {
          for (const sock of Array.from(this.spotifyEngine.tcpClients)) {
            if (sock._isTemporaryTtsSocket) {
              this.spotifyEngine.tcpClients.delete(sock);
              try {
                sock.end();
                const destroyTimer = setTimeout(() => {
                  try { sock.destroy(); } catch {}
                }, 150);
                if (typeof destroyTimer.unref === 'function') destroyTimer.unref();
              } catch {}
            }
          }
        }

        // Ensure Spotify resumes if it wasn't already triggered by the early resume timer
        triggerSpotifyResume();
      }, 180);
      if (typeof this.ttsDrainTimer.unref === 'function') this.ttsDrainTimer.unref();
    };

    const pump = () => {
      if (offset >= u8Buffer.length || !this.spotifyEngine) {
        finishTtsHandover();
        return;
      }
      const step = offset === 0 ? initialBurstBytes : frameBytes;
      const end = Math.min(offset + step, u8Buffer.length);
      const chunk = u8Buffer.subarray(offset, end);
      offset = end;

      for (const sock of activeSockets) {
        if (!sock.destroyed && sock.writable) {
          sock._isTemporaryTtsSocket = true;
          sock.write(chunk);
        }
      }

      if (offset < u8Buffer.length) {
        this.activeTtsTimer = setTimeout(pump, 85);
      } else {
        finishTtsHandover();
      }
    };

    pump();
    return { streamedBytes: u8Buffer.length, durationMs, mixedIntoSpotify: false };
  }

  /**
   * Converts any input audio buffer (MP3, WAV, OGG, AIFF, or raw PCM) into
   * unsigned 8-bit mono PCM (`pcm_u8`) at 22,255 Hz (or `hz`) using FFmpeg.
   */
  transcodeToPcmU8(inputBuffer, { hz = 22255, inputFormat = null, extraFilter = null } = {}) {
    return new Promise((resolve, reject) => {
      const args = [
        '-hide_banner',
        '-loglevel', 'error',
      ];
      if (inputFormat) {
        args.push('-f', inputFormat);
      }
      const filterChain = extraFilter
        ? `${extraFilter},acompressor=threshold=-18dB:ratio=3:makeup=4dB,alimiter=limit=0.95,aresample=osr=${hz}`
        : `highpass=f=120,acompressor=threshold=-18dB:ratio=3:makeup=4dB,alimiter=limit=0.95,aresample=osr=${hz}`;
      args.push(
        '-i', 'pipe:0',
        '-af', filterChain,
        '-f', 'u8',
        '-acodec', 'pcm_u8',
        '-ac', '1',
        '-ar', String(hz),
        'pipe:1'
      );

      let proc;
      try {
        proc = spawn('ffmpeg', args, { stdio: ['pipe', 'pipe', 'pipe'] });
      } catch (err) {
        return reject(err);
      }

      const chunks = [];
      proc.stdout.on('data', (c) => chunks.push(c));
      proc.on('error', (err) => reject(err));
      proc.on('close', (code) => {
        if (code === 0 && chunks.length > 0) {
          resolve(Buffer.concat(chunks));
        } else {
          reject(new Error(`ffmpeg exited with code ${code}`));
        }
      });

      proc.stdin.on('error', () => {});
      proc.stdin.end(inputBuffer);
    });
  }

  /**
   * Converts unsigned 8-bit mono PCM recorded on the 68k Mac (`pcm_u8` at `inputHz`)
   * into a standard 16-bit 16 kHz mono WAV buffer suitable for Whisper / Gemini STT.
   */
  pcmU8ToWav16k(u8Buffer, inputHz = 11127) {
    const numSamples = u8Buffer.length;
    const pcm16 = Buffer.alloc(numSamples * 2);
    for (let i = 0; i < numSamples; i++) {
      const u8 = u8Buffer[i];
      const s16 = (u8 - 128) << 8;
      pcm16.writeInt16LE(s16, i * 2);
    }

    const header = Buffer.alloc(44);
    const dataLen = pcm16.length;
    header.write('RIFF', 0);
    header.writeUInt32LE(dataLen + 36, 4);
    header.write('WAVE', 8);
    header.write('fmt ', 12);
    header.writeUInt32LE(16, 16);
    header.writeUInt16LE(1, 20); // PCM
    header.writeUInt16LE(1, 22); // Mono
    header.writeUInt32LE(inputHz, 24);
    header.writeUInt32LE(inputHz * 2, 28);
    header.writeUInt16LE(2, 32);
    header.writeUInt16LE(16, 34);
    header.write('data', 36);
    header.writeUInt32LE(dataLen, 40);

    return Buffer.concat([header, pcm16]);
  }
}

module.exports = new SharedAudioHub();
