const sharp = require('sharp');

const DEFAULT_ART_SIZE = 200;

/**
 * Applies Bill Atkinson's classic Macintosh 1-bit error-diffusion dithering
 * (or Floyd-Steinberg dithering) to an 8-bit grayscale pixel buffer.
 *
 * Returns:
 *   - `pixels`: Uint8Array of 0 (black) or 255 (white) values (length = width * height)
 *   - `packed`: Buffer of packed 1-bit QuickDraw monochrome bytes where:
 *       bit = 1 -> black pixel (QuickDraw foreground)
 *       bit = 0 -> white pixel (QuickDraw background)
 *       Each scanline is padded to an even number of bytes (`rowBytes`), matching
 *       Macintosh QuickDraw `BitMap.rowBytes` requirements!
 */
function ditherGrayscaleTo1Bit(grayBuffer, width, height, algorithm = 'atkinson') {
  const lum = new Float32Array(width * height);
  for (let i = 0; i < width * height; i++) {
    lum[i] = grayBuffer[i];
  }

  const outPixels = new Uint8Array(width * height);
  const useFloyd = String(algorithm).toLowerCase().startsWith('floyd');

  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const idx = y * width + x;
      const oldVal = lum[idx];
      const newVal = oldVal < 128 ? 0 : 255;
      outPixels[idx] = newVal;
      const err = oldVal - newVal;

      if (useFloyd) {
        // Floyd-Steinberg (diffuses 100% of error: 7/16, 3/16, 5/16, 1/16)
        if (x + 1 < width) lum[idx + 1] += (err * 7) / 16;
        if (y + 1 < height) {
          if (x > 0) lum[idx + width - 1] += (err * 3) / 16;
          lum[idx + width] += (err * 5) / 16;
          if (x + 1 < width) lum[idx + width + 1] += (err * 1) / 16;
        }
      } else {
        // Bill Atkinson MacPaint Dithering (diffuses 6/8 = 75% of error for crisp high-contrast midtones)
        const e8 = err / 8;
        if (x + 1 < width) lum[idx + 1] += e8;
        if (x + 2 < width) lum[idx + 2] += e8;
        if (y + 1 < height) {
          if (x > 0) lum[idx + width - 1] += e8;
          lum[idx + width] += e8;
          if (x + 1 < width) lum[idx + width + 1] += e8;
        }
        if (y + 2 < height) {
          lum[idx + width * 2] += e8;
        }
      }
    }
  }

  // QuickDraw BitMap requires rowBytes to be an even integer: ceil(width / 16) * 2
  const rowBytes = Math.ceil(width / 16) * 2;
  const packed = Buffer.alloc(rowBytes * height, 0);

  for (let y = 0; y < height; y++) {
    const rowOffset = y * rowBytes;
    for (let x = 0; x < width; x++) {
      const isBlack = outPixels[y * width + x] === 0;
      if (isBlack) {
        const byteIndex = rowOffset + (x >> 3);
        const bitMask = 0x80 >> (x & 7);
        packed[byteIndex] |= bitMask;
      }
    }
  }

  return {
    width,
    height,
    rowBytes,
    pixels: outPixels,
    packed,
  };
}

/**
 * Packages a 1-bit QuickDraw bitmap with a compact 8-byte Big-Endian header (`MCBM`)
 * so the 68k C client can read the dimensions & `rowBytes` and pass the buffer directly
 * to `CopyBits()`:
 *   Bytes 0..1: uint16 BE width    (e.g. 200)
 *   Bytes 2..3: uint16 BE height   (e.g. 200)
 *   Bytes 4..5: uint16 BE rowBytes (e.g. 26 for 200px width)
 *   Bytes 6..7: uint16 BE reserved (0)
 *   Bytes 8..N: raw QuickDraw 1-bit pixel rows (rowBytes * height bytes)
 */
function packQuickDrawBitMapWithHeader({ width, height, rowBytes, packed }) {
  const header = Buffer.alloc(8);
  header.writeUInt16BE(width, 0);
  header.writeUInt16BE(height, 2);
  header.writeUInt16BE(rowBytes, 4);
  header.writeUInt16BE(0, 6);
  return Buffer.concat([header, packed]);
}

/**
 * Processes an input image Buffer (JPEG/PNG/WebP) into a 1-bit monochrome dithered image.
 *
 * @param {Buffer} inputBuffer - Raw source image bytes
 * @param {Object} options
 * @param {number} [options.size=200] - Target width & height in pixels (default 200x200)
 * @param {string} [options.dither='atkinson'] - 'atkinson' or 'floyd'
 * @param {string} [options.format='raw'] - 'raw' (8-byte header + QuickDraw bits), 'bits' (pure bits only), 'png' (1-bit PNG preview), 'hex' (pipe-delimited hex string)
 */
async function processArtworkBuffer(inputBuffer, {
  size = DEFAULT_ART_SIZE,
  width,
  height,
  dither = 'atkinson',
  format = 'raw',
  contrast = 1.08,
} = {}) {
  const targetW = Math.max(16, Math.min(512, parseInt(width || size || DEFAULT_ART_SIZE, 10)));
  const targetH = Math.max(16, Math.min(512, parseInt(height || size || DEFAULT_ART_SIZE, 10)));

  // Downscale with Lanczos3, flatten alpha onto white, convert to 8-bit grayscale
  let pipeline = sharp(inputBuffer)
    .flatten({ background: { r: 255, g: 255, b: 255 } })
    .resize(targetW, targetH, { fit: 'cover' })
    .grayscale();

  if (contrast && contrast !== 1.0) {
    pipeline = pipeline.linear(contrast, -(128 * contrast) + 128);
  }

  const grayBuffer = await pipeline.raw().toBuffer();
  const dithered = ditherGrayscaleTo1Bit(grayBuffer, targetW, targetH, dither);

  const fmt = String(format).toLowerCase();
  if (fmt === 'png') {
    const pngBuffer = await sharp(dithered.pixels, {
      raw: { width: targetW, height: targetH, channels: 1 },
    })
      .png({ compressionLevel: 9 })
      .toBuffer();

    return {
      contentType: 'image/png',
      width: targetW,
      height: targetH,
      rowBytes: dithered.rowBytes,
      buffer: pngBuffer,
    };
  }

  if (fmt === 'bits') {
    return {
      contentType: 'application/octet-stream',
      width: targetW,
      height: targetH,
      rowBytes: dithered.rowBytes,
      buffer: dithered.packed,
    };
  }

  if (fmt === 'hex' || fmt === 'pipe') {
    const hexStr = dithered.packed.toString('hex').toUpperCase();
    const pipePayload = `BITMAP|${targetW}|${targetH}|${dithered.rowBytes}|${dithered.packed.length}|${hexStr}\n`;
    return {
      contentType: 'text/plain; charset=utf-8',
      width: targetW,
      height: targetH,
      rowBytes: dithered.rowBytes,
      buffer: Buffer.from(pipePayload, 'utf8'),
    };
  }

  // Default: 'raw' -> 8-byte header (width, height, rowBytes, 0) + packed QuickDraw BitMap bytes
  const rawBuffer = packQuickDrawBitMapWithHeader(dithered);
  return {
    contentType: 'application/octet-stream',
    width: targetW,
    height: targetH,
    rowBytes: dithered.rowBytes,
    buffer: rawBuffer,
  };
}

/**
 * Generates a classic 1-bit Macintosh musical note placeholder image when no artwork is available.
 */
async function generatePlaceholderArtwork({ size = DEFAULT_ART_SIZE, dither = 'atkinson', format = 'raw' } = {}) {
  const s = Math.max(16, Math.min(512, parseInt(size || DEFAULT_ART_SIZE, 10)));
  const svg = `
    <svg width="${s}" height="${s}" viewBox="0 0 200 200" xmlns="http://www.w3.org/2000/svg">
      <rect width="200" height="200" fill="#ffffff" stroke="#000000" stroke-width="6"/>
      <rect x="10" y="10" width="180" height="180" fill="none" stroke="#000000" stroke-width="2" stroke-dasharray="4,4"/>
      <circle cx="72" cy="138" r="20" fill="#000000"/>
      <circle cx="136" cy="124" r="20" fill="#000000"/>
      <rect x="86" y="54" width="8" height="84" fill="#000000"/>
      <rect x="150" y="40" width="8" height="84" fill="#000000"/>
      <polygon points="86,50 158,36 158,56 86,70" fill="#000000"/>
    </svg>
  `;
  return processArtworkBuffer(Buffer.from(svg), { size: s, dither, format, contrast: 1.0 });
}

module.exports = {
  DEFAULT_ART_SIZE,
  ditherGrayscaleTo1Bit,
  packQuickDrawBitMapWithHeader,
  processArtworkBuffer,
  generatePlaceholderArtwork,
};
