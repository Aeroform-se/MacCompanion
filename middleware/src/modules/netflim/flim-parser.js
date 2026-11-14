const fs = require('fs');
const sharp = require('sharp');

const COMMENT_SIZE = 1022;
const CHECKSUM_SIZE = 2;

const COMPONENT_TYPES = {
  INFO: 0,
  MOVIE: 1,
  TOC: 2,
  POSTER: 3,
  INITIAL: 4,
};

const CODEC_NAMES = {
  0x00: 'null',
  0x01: 'z16',
  0x02: 'z32',
  0x03: 'invert',
  0x04: 'lines',
};

/**
 * Compute big-endian Fletcher-16 checksum matching flim.cpp.
 */
function fletcher16(chunks) {
  let sum = 0;
  for (const buf of chunks) {
    for (let i = 0; i + 1 < buf.length; i += 2) {
      sum = (sum + buf.readUInt16BE(i)) % 65535;
    }
  }
  return sum;
}

/**
 * Parse header, component directory, info component, TOC, and optional poster/initial frame
 * from a .flim file on disk without loading the entire movie stream into RAM.
 */
function parseFlimFile(filePath) {
  const stat = fs.statSync(filePath);
  const fd = fs.openSync(filePath, 'r');

  try {
    if (stat.size < COMMENT_SIZE + CHECKSUM_SIZE + 4) {
      throw new Error('File too small to be a valid .flim file');
    }

    const headBuf = Buffer.alloc(COMMENT_SIZE + CHECKSUM_SIZE + 4);
    fs.readSync(fd, headBuf, 0, headBuf.length, 0);

    const magic = headBuf.subarray(0, 4).toString('ascii');
    const sep = headBuf[4];
    if (magic !== 'FLIM' || (sep !== 0x0a && sep !== 0x0d && sep !== 0x00)) {
      throw new Error('Invalid .flim magic header (expected FLIM)');
    }

    const comment = headBuf
      .subarray(0, COMMENT_SIZE)
      .toString('utf8')
      .replace(/\0+$/, '')
      .trim();

    const storedChecksum = headBuf.readUInt16BE(COMMENT_SIZE);
    const version = headBuf.readUInt16BE(COMMENT_SIZE + CHECKSUM_SIZE);
    const componentCount = headBuf.readUInt16BE(COMMENT_SIZE + CHECKSUM_SIZE + 2);

    if (componentCount > 64) {
      throw new Error(`Unexpected .flim component count: ${componentCount}`);
    }

    const dirSize = componentCount * 10;
    const dirStart = COMMENT_SIZE + CHECKSUM_SIZE + 4;
    const dataStart = dirStart + dirSize;

    if (stat.size < dataStart) {
      throw new Error('Truncated .flim component directory');
    }

    const dirBuf = Buffer.alloc(dirSize);
    fs.readSync(fd, dirBuf, 0, dirSize, dirStart);

    const components = [];
    for (let i = 0; i < componentCount; i++) {
      const off = i * 10;
      const type = dirBuf.readUInt16BE(off);
      const offset = dirBuf.readUInt32BE(off + 2);
      const size = dirBuf.readUInt32BE(off + 6);
      if (dataStart + offset + size > stat.size) {
        throw new Error(`Truncated .flim component #${i} (type ${type})`);
      }
      components.push({
        type,
        offset,
        size,
        fileOffset: dataStart + offset,
      });
    }

    const readComponent = (type) => {
      const entry = components.find((c) => c.type === type);
      if (!entry) return null;
      const buf = Buffer.alloc(entry.size);
      fs.readSync(fd, buf, 0, entry.size, entry.fileOffset);
      return { entry, buf };
    };

    // Info component (type 0x00, 16 bytes)
    const infoComp = readComponent(COMPONENT_TYPES.INFO);
    if (!infoComp || infoComp.buf.length < 16) {
      throw new Error('.flim file missing info component');
    }
    const info = {
      width: infoComp.buf.readUInt16BE(0),
      height: infoComp.buf.readUInt16BE(2),
      silent: infoComp.buf.readUInt16BE(4) !== 0,
      frameCount: infoComp.buf.readUInt32BE(6),
      totalTicks: infoComp.buf.readUInt32BE(10),
      byterate: infoComp.buf.readUInt16BE(14),
    };

    const durationSec = Number((info.totalTicks / 60).toFixed(2));

    // Optional poster (type 0x03, 128x86 1-bit bitmap = 1376 bytes)
    let posterBitmap = null;
    const posterComp = readComponent(COMPONENT_TYPES.POSTER);
    if (posterComp && posterComp.buf.length >= 1376) {
      posterBitmap = posterComp.buf.subarray(0, 1376);
    }

    // Optional initial frame (type 0x04)
    let initialFrame = null;
    const initComp = readComponent(COMPONENT_TYPES.INITIAL);
    if (initComp && initComp.buf.length >= 6) {
      initialFrame = {
        tag: initComp.buf.readUInt16BE(0),
        width: initComp.buf.readUInt16BE(2),
        height: initComp.buf.readUInt16BE(4),
        bitmap: initComp.buf.subarray(6),
      };
    }

    const movieEntry = components.find((c) => c.type === COMPONENT_TYPES.MOVIE) || null;
    const tocEntry = components.find((c) => c.type === COMPONENT_TYPES.TOC) || null;

    return {
      version,
      fileSize: stat.size,
      storedChecksum,
      comment,
      info,
      durationSec,
      posterBitmap,
      initialFrame,
      components,
      movieEntry,
      tocEntry,
    };
  } finally {
    fs.closeSync(fd);
  }
}

/**
 * Parse TOC offsets + frame sizes for fast random access & UDP streaming.
 */
function readFlimToc(filePath) {
  const parsed = parseFlimFile(filePath);
  if (!parsed.tocEntry || !parsed.movieEntry) {
    throw new Error('.flim file missing TOC or movie component');
  }

  const fd = fs.openSync(filePath, 'r');
  try {
    const tocBuf = Buffer.alloc(parsed.tocEntry.size);
    fs.readSync(fd, tocBuf, 0, tocBuf.length, parsed.tocEntry.fileOffset);

    const frameCount = Math.floor(tocBuf.length / 2);
    const frames = [];
    let relOffset = 0;

    for (let i = 0; i < frameCount; i++) {
      const size = tocBuf.readUInt16BE(i * 2);
      frames.push({
        index: i,
        offset: parsed.movieEntry.fileOffset + relOffset,
        size,
      });
      relOffset += size;
    }

    return {
      parsed,
      frames,
    };
  } finally {
    fs.closeSync(fd);
  }
}

/**
 * Decode a single serialized frame slice from a movie component buffer.
 */
function decodeFramePacket(frameBuf) {
  if (!Buffer.isBuffer(frameBuf) || frameBuf.length < 4) {
    throw new Error('Invalid frame buffer');
  }

  const ticks = frameBuf.readUInt16BE(0);
  const soundSize = frameBuf.readUInt16BE(2);
  let pos = 4;

  let audio = Buffer.alloc(0);
  if (soundSize > 2) {
    const payloadLen = soundSize - 2;
    if (pos + payloadLen > frameBuf.length) {
      throw new Error('Truncated audio block in .flim frame');
    }
    // Skip 2B ffMode + 4B fixed-point rate; extract raw 8-bit unsigned PCM
    if (payloadLen > 6) {
      audio = frameBuf.subarray(pos + 6, pos + payloadLen);
    }
    pos += payloadLen;
  }

  let video = Buffer.alloc(0);
  if (pos + 2 <= frameBuf.length) {
    const videoSizeField = frameBuf.readUInt16BE(pos);
    pos += 2;
    const videoLen = videoSizeField > 2 ? videoSizeField - 2 : 0;
    if (videoLen > 0 && pos + videoLen <= frameBuf.length) {
      video = frameBuf.subarray(pos, pos + videoLen);
    }
  }

  const codecId = video.length >= 4 ? video[3] : 0x00;

  return {
    ticks,
    audio,
    video,
    codecId,
    codecName: CODEC_NAMES[codecId] || `0x${codecId.toString(16)}`,
  };
}

/**
 * Convert a 128x86 1-bit MSB-first Mac bitmap (1 = black, 0 = white) into a crisp upscaled PNG.
 */
async function renderPosterPng(bitmapBuf, scale = 2) {
  const width = 128;
  const height = 86;
  const rowBytes = 16;

  if (!Buffer.isBuffer(bitmapBuf) || bitmapBuf.length < rowBytes * height) {
    return null;
  }

  const gray = Buffer.alloc(width * height);
  for (let y = 0; y < height; y++) {
    const rowOff = y * rowBytes;
    for (let x = 0; x < width; x++) {
      const byte = bitmapBuf[rowOff + (x >> 3)];
      const bit = (byte >> (7 - (x & 7))) & 1;
      gray[y * width + x] = bit ? 0 : 255;
    }
  }

  let pipeline = sharp(gray, {
    raw: { width, height, channels: 1 },
  });

  if (scale > 1) {
    pipeline = pipeline.resize(width * scale, height * scale, {
      kernel: sharp.kernel.nearest,
    });
  }

  return pipeline.png().toBuffer();
}

module.exports = {
  COMMENT_SIZE,
  COMPONENT_TYPES,
  CODEC_NAMES,
  fletcher16,
  parseFlimFile,
  readFlimToc,
  decodeFramePacket,
  renderPosterPng,
};
