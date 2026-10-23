const sharp = require('sharp');

const DEFAULT_ART_SIZE = 200;

/**
 * Normalizes a grayscale buffer's dynamic range (2nd..98th percentile stretch +
 * automatic midtone gamma balancing) so neither dark moody covers nor bright pastel
 * covers collapse into mostly-black or mostly-white squares.
 */
function autoLevelAndBalanceGrayscale(grayBuffer, width, height) {
  const total = width * height;
  const hist = new Uint32Array(256);
  let sum = 0;

  for (let i = 0; i < total; i++) {
    const v = grayBuffer[i];
    hist[v]++;
    sum += v;
  }

  // Find 2nd and 98th percentile luminance to ignore single-pixel outliers/borders
  const lowTarget = Math.floor(total * 0.02);
  const highTarget = Math.floor(total * 0.98);
  let acc = 0;
  let minVal = 0;
  let maxVal = 255;

  for (let i = 0; i < 256; i++) {
    acc += hist[i];
    if (acc >= lowTarget) {
      minVal = i;
      break;
    }
  }
  acc = 0;
  for (let i = 0; i < 256; i++) {
    acc += hist[i];
    if (acc >= highTarget) {
      maxVal = i;
      break;
    }
  }

  const span = Math.max(24, maxVal - minVal);

  // Compute mean brightness after stretch to determine optimal gamma curve
  const meanNormalized = Math.max(0.08, Math.min(0.92, ((sum / total) - minVal) / span));
  // Choose gamma that gently pulls dark (<0.42) or bright (>0.58) images toward balanced 0.50 midtone
  let gamma = 1.0;
  if (meanNormalized < 0.45 || meanNormalized > 0.55) {
    const rawGamma = Math.log(0.5) / Math.log(meanNormalized);
    // Blend 65% toward ideal midtone balance so dark covers stay atmospheric without turning pure black
    gamma = Math.max(0.55, Math.min(1.75, 1.0 + (rawGamma - 1.0) * 0.65));
  }

  const lut = new Float32Array(256);
  for (let i = 0; i < 256; i++) {
    const norm = Math.max(0, Math.min(1, (i - minVal) / span));
    lut[i] = Math.pow(norm, gamma) * 255.0;
  }

  const out = new Float32Array(total);
  for (let i = 0; i < total; i++) {
    out[i] = lut[grayBuffer[i]];
  }
  return out;
}

/**
 * Applies a subtle 3x3 unsharp mask to preserve album typography, portraits, and line art
 * before 1-bit quantization.
 */
function applyUnsharpMask(lum, width, height, amount = 0.35) {
  const out = new Float32Array(lum);
  for (let y = 1; y < height - 1; y++) {
    const row = y * width;
    for (let x = 1; x < width - 1; x++) {
      const idx = row + x;
      const center = lum[idx];
      const neighbors =
        (lum[idx - 1] + lum[idx + 1] + lum[idx - width] + lum[idx + width]) * 0.25;
      const sharpened = center + (center - neighbors) * amount;
      out[idx] = Math.max(0, Math.min(255, sharpened));
    }
  }
  return out;
}

/**
 * Applies 1-bit monochrome dithering to an 8-bit grayscale pixel buffer.
 *
 * Supported `algorithm` values:
 *   - `'atkinson'`  : Bill Atkinson's classic MacPaint dithering (diffuses 6/8 = 75% of error)
 *   - `'floyd'`     : Serpentine Floyd-Steinberg dithering (diffuses 16/16 = 100% of error)
 *   - `'bluenoise'` : High-frequency 2D Blue Noise (Interleaved Gradient Blue Noise + 35% micro-diffusion)
 *   - `'adaptive'`  : Smart Auto-Level + Gamma Rescue + Unsharp Mask + Stucki 42-weight error diffusion
 *
 * Returns:
 *   - `pixels`: Uint8Array of 0 (black) or 255 (white) values (length = width * height)
 *   - `packed`: Buffer of packed 1-bit QuickDraw monochrome bytes where:
 *       bit = 1 -> black pixel (QuickDraw foreground)
 *       bit = 0 -> white pixel (QuickDraw background)
 */
function ditherGrayscaleTo1Bit(grayBuffer, width, height, algorithm = 'adaptive') {
  const alg = String(algorithm || 'adaptive').toLowerCase().trim();
  const isFloyd = alg.startsWith('floyd');
  const isBlueNoise = alg.includes('blue');
  const isAdaptive = alg.startsWith('adapt') || alg.startsWith('stucki') || alg.startsWith('smart');

  let lum;
  if (isAdaptive) {
    const leveled = autoLevelAndBalanceGrayscale(grayBuffer, width, height);
    lum = applyUnsharpMask(leveled, width, height, 0.45);
  } else if (isBlueNoise) {
    // Gentle auto-level + crisp edge boost for blue noise stippling
    const leveled = autoLevelAndBalanceGrayscale(grayBuffer, width, height);
    lum = applyUnsharpMask(leveled, width, height, 0.3);
  } else {
    lum = new Float32Array(width * height);
    for (let i = 0; i < width * height; i++) {
      lum[i] = grayBuffer[i];
    }
  }

  const outPixels = new Uint8Array(width * height);

  if (isBlueNoise) {
    // High-frequency 2D Blue Noise (Jorge Jimenez Interleaved Gradient Noise + light 25% error feedback
    // to preserve fine line detail while keeping organic isotropic stippling)
    for (let y = 0; y < height; y++) {
      const serpentine = (y & 1) === 1;
      const xStart = serpentine ? width - 1 : 0;
      const xEnd = serpentine ? -1 : width;
      const xStep = serpentine ? -1 : 1;

      for (let x = xStart; x !== xEnd; x += xStep) {
        const idx = y * width + x;
        const oldVal = lum[idx];

        // 2D Blue-Noise spatial threshold in [0..1) -> mapped to [-56..+56]
        const ign = (52.9829189 * ((0.06711056 * x + 0.00583715 * y) % 1.0)) % 1.0;
        const threshold = 128 + (ign - 0.5) * 108;

        const newVal = oldVal < threshold ? 0 : 255;
        outPixels[idx] = newVal;

        // Subtle 35% local error feedback keeps thin text & borders intact
        const err = (oldVal - newVal) * 0.35;
        if (x + xStep >= 0 && x + xStep < width) {
          lum[idx + xStep] += err * 0.55;
        }
        if (y + 1 < height) {
          lum[idx + width] += err * 0.45;
        }
      }
    }
  } else if (isAdaptive) {
    // Serpentine Stucki 42-weight wide-kernel 100% error diffusion
    // Row 0:       *   8   4  (/42)
    // Row 1: 2  4  8   4   2  (/42)
    // Row 2: 1  2  4   2   1  (/42)
    for (let y = 0; y < height; y++) {
      const serpentine = (y & 1) === 1;
      const xStart = serpentine ? width - 1 : 0;
      const xEnd = serpentine ? -1 : width;
      const xStep = serpentine ? -1 : 1;

      for (let x = xStart; x !== xEnd; x += xStep) {
        const idx = y * width + x;
        const oldVal = lum[idx];
        const newVal = oldVal < 128 ? 0 : 255;
        outPixels[idx] = newVal;
        const e = (oldVal - newVal) / 42.0;

        const x1 = x + xStep;
        const x2 = x + xStep * 2;
        const xm1 = x - xStep;
        const xm2 = x - xStep * 2;

        if (x1 >= 0 && x1 < width) lum[idx + xStep] += e * 8;
        if (x2 >= 0 && x2 < width) lum[idx + xStep * 2] += e * 4;

        if (y + 1 < height) {
          const r1 = (y + 1) * width;
          if (xm2 >= 0 && xm2 < width) lum[r1 + xm2] += e * 2;
          if (xm1 >= 0 && xm1 < width) lum[r1 + xm1] += e * 4;
          lum[r1 + x] += e * 8;
          if (x1 >= 0 && x1 < width) lum[r1 + x1] += e * 4;
          if (x2 >= 0 && x2 < width) lum[r1 + x2] += e * 2;
        }

        if (y + 2 < height) {
          const r2 = (y + 2) * width;
          if (xm2 >= 0 && xm2 < width) lum[r2 + xm2] += e * 1;
          if (xm1 >= 0 && xm1 < width) lum[r2 + xm1] += e * 2;
          lum[r2 + x] += e * 4;
          if (x1 >= 0 && x1 < width) lum[r2 + x1] += e * 2;
          if (x2 >= 0 && x2 < width) lum[r2 + x2] += e * 1;
        }
      }
    }
  } else {
    // Classic Atkinson or Serpentine Floyd-Steinberg
    for (let y = 0; y < height; y++) {
      const serpentine = isFloyd && (y & 1) === 1;
      const xStart = serpentine ? width - 1 : 0;
      const xEnd = serpentine ? -1 : width;
      const xStep = serpentine ? -1 : 1;

      for (let x = xStart; x !== xEnd; x += xStep) {
        const idx = y * width + x;
        const oldVal = lum[idx];
        const newVal = oldVal < 128 ? 0 : 255;
        outPixels[idx] = newVal;
        const err = oldVal - newVal;

        if (isFloyd) {
          // Serpentine Floyd-Steinberg (diffuses 100% of error: 7/16, 3/16, 5/16, 1/16)
          const xNext = x + xStep;
          const xPrev = x - xStep;
          if (xNext >= 0 && xNext < width) lum[idx + xStep] += (err * 7) / 16;
          if (y + 1 < height) {
            if (xPrev >= 0 && xPrev < width) lum[idx + width - xStep] += (err * 3) / 16;
            lum[idx + width] += (err * 5) / 16;
            if (xNext >= 0 && xNext < width) lum[idx + width + xStep] += (err * 1) / 16;
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
 * @param {string} [options.dither='adaptive'] - 'adaptive' | 'atkinson' | 'floyd' | 'bluenoise'
 * @param {string} [options.format='raw'] - 'raw' (8-byte header + QuickDraw bits), 'bits' (pure bits only), 'png' (1-bit PNG preview), 'hex' (pipe-delimited hex string)
 */
async function processArtworkBuffer(inputBuffer, {
  size = DEFAULT_ART_SIZE,
  width,
  height,
  dither = 'adaptive',
  format = 'raw',
  contrast = 1.08,
} = {}) {
  const targetW = Math.max(16, Math.min(512, parseInt(width || size || DEFAULT_ART_SIZE, 10)));
  const targetH = Math.max(16, Math.min(512, parseInt(height || size || DEFAULT_ART_SIZE, 10)));
  const ditherKey = String(dither || 'adaptive').toLowerCase().trim();

  // Downscale with Lanczos3, flatten alpha onto white, convert to 8-bit grayscale
  let pipeline = sharp(inputBuffer)
    .flatten({ background: { r: 255, g: 255, b: 255 } })
    .resize(targetW, targetH, { fit: 'cover' })
    .grayscale();

  // Adaptive and Blue Noise perform their own histogram auto-leveling & gamma rescue
  if (contrast && contrast !== 1.0 && !ditherKey.startsWith('adapt') && !ditherKey.includes('blue')) {
    pipeline = pipeline.linear(contrast, -(128 * contrast) + 128);
  }

  const grayBuffer = await pipeline.raw().toBuffer();
  const dithered = ditherGrayscaleTo1Bit(grayBuffer, targetW, targetH, ditherKey);

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
 * Generates the default album placeholder artwork (using default-album-artwork.png or musical note fallback)
 * when no artwork is available.
 */
async function generatePlaceholderArtwork({ size = DEFAULT_ART_SIZE, dither = 'adaptive', format = 'raw' } = {}) {
  const s = Math.max(16, Math.min(512, parseInt(size || DEFAULT_ART_SIZE, 10)));
  const defaultPngPath = require('path').join(__dirname, '../public/img/default-album-artwork.png');
  try {
    const fs = require('fs');
    if (fs.existsSync(defaultPngPath)) {
      const defaultBuf = fs.readFileSync(defaultPngPath);
      return processArtworkBuffer(defaultBuf, { size: s, dither, format, contrast: 1.0 });
    }
  } catch (_) {}

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
