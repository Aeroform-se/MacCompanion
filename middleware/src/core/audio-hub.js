const { spawn, execSync } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

/**
 * Shared 8-Bit PCM Audio Hub (TCP 6802 & HTTP PCM)
 *
 * Coordinates audio streaming across multiple MacCompanion modules (SpotiMac, MacAI, etc.)
 * so that:
 *   1. When MacAI starts voice recording or TTS spoken read-back, active SpotiMac playback
 *      is automatically paused (`pauseSpotifyForVoice()`).
 *   2. Any module can transcode/synthesize audio into unsigned 8-bit mono PCM (`pcm_u8` at
 *      22,255 Hz or 11,127 Hz) and stream it over the shared TCP 6802 socket to the 68k Mac's
 *      Sound Manager.
 */
class SharedAudioHub {
  constructor() {
    this.spotifyEngine = null;
    this.pauseSpotifyFn = null;
    this.ttsActiveUntil = 0;
    this.activeTtsTimer = null;
  }

  registerSpotifyEngine(engine, pauseFn) {
    this.spotifyEngine = engine;
    this.pauseSpotifyFn = pauseFn;
  }

  isTtsActive() {
    return Date.now() < this.ttsActiveUntil;
  }

  /**
   * Automatically pauses SpotiMac playback and flushes the librespot pacer
   * whenever MacAI starts recording voice input or speaking an AI response.
   */
  async pauseSpotifyForVoice() {
    if (this.spotifyEngine && typeof this.spotifyEngine.onPause === 'function') {
      this.spotifyEngine.onPause();
    }
    if (typeof this.pauseSpotifyFn === 'function') {
      try {
        await this.pauseSpotifyFn();
      } catch {}
    }
  }

  /**
   * Stops any in-progress TTS stream to TCP 6802 clients.
   */
  stopTtsStream() {
    this.ttsActiveUntil = 0;
    if (this.activeTtsTimer) {
      clearTimeout(this.activeTtsTimer);
      this.activeTtsTimer = null;
    }
  }

  /**
   * Streams an unsigned 8-bit mono PCM buffer (`pcm_u8` at `hz` sample rate)
   * at real-time speed over TCP port 6802 to connected 68k Mac clients.
   */
  streamPcmU8ToTcpClients(u8Buffer, { hz = 22255 } = {}) {
    if (!u8Buffer || u8Buffer.length === 0) return { streamedBytes: 0, durationMs: 0 };
    this.stopTtsStream();

    const durationMs = Math.ceil((u8Buffer.length / hz) * 1000);
    this.ttsActiveUntil = Date.now() + durationMs + 500;

    if (!this.spotifyEngine || !this.spotifyEngine.tcpClients || this.spotifyEngine.tcpClients.size === 0) {
      return { streamedBytes: u8Buffer.length, durationMs };
    }

    const frameBytes = Math.max(512, Math.round(hz * 0.08)); // 80ms coalesced TCP frames (~1780 bytes @ 22k)
    let offset = 0;

    const pump = () => {
      if (offset >= u8Buffer.length || !this.spotifyEngine) {
        this.activeTtsTimer = null;
        this.ttsActiveUntil = 0;
        return;
      }
      const end = Math.min(offset + frameBytes, u8Buffer.length);
      const chunk = u8Buffer.subarray(offset, end);
      offset = end;

      for (const sock of this.spotifyEngine.tcpClients) {
        if (!sock.destroyed && sock.writable) {
          sock.write(chunk);
        }
      }

      if (offset < u8Buffer.length) {
        this.activeTtsTimer = setTimeout(pump, 76);
      } else {
        this.activeTtsTimer = null;
        this.ttsActiveUntil = 0;
      }
    };

    pump();
    return { streamedBytes: u8Buffer.length, durationMs };
  }

  /**
   * Converts any input audio buffer (MP3, WAV, OGG, AIFF, or raw PCM) into
   * unsigned 8-bit mono PCM (`pcm_u8`) at 22,255 Hz (or `hz`) using FFmpeg.
   */
  transcodeToPcmU8(inputBuffer, { hz = 22255, inputFormat = null } = {}) {
    return new Promise((resolve, reject) => {
      const args = [
        '-hide_banner',
        '-loglevel', 'error',
      ];
      if (inputFormat) {
        args.push('-f', inputFormat);
      }
      args.push(
        '-i', 'pipe:0',
        '-af', `highpass=f=120,acompressor=threshold=-18dB:ratio=3:makeup=4dB,alimiter=limit=0.95,aresample=osr=${hz}`,
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
