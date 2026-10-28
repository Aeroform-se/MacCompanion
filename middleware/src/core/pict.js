const sharp = require('sharp');

/**
 * PackBits RLE compression following Apple Macintosh QuickDraw standard.
 * - Literal runs: 1..128 bytes -> count byte (litLen - 1) followed by literal bytes.
 * - Replicate runs: 3..128 repeated bytes -> count byte (1 - runLen) followed by the single byte.
 */
function packBits(rowBuf) {
  const out = [];
  let i = 0;
  const len = rowBuf.length;

  while (i < len) {
    let runLen = 1;
    while (i + runLen < len && runLen < 128 && rowBuf[i + runLen] === rowBuf[i]) {
      runLen++;
    }

    if (runLen >= 3) {
      out.push(1 - runLen);
      out.push(rowBuf[i]);
      i += runLen;
    } else {
      let litStart = i;
      let litLen = 0;
      while (i < len && litLen < 128) {
        if (i + 2 < len && rowBuf[i] === rowBuf[i + 1] && rowBuf[i] === rowBuf[i + 2]) {
          break;
        }
        litLen++;
        i++;
      }
      out.push(litLen - 1);
      for (let k = 0; k < litLen; k++) {
        out.push(rowBuf[litStart + k]);
      }
    }
  }

  return Buffer.from(out);
}

/**
 * Generates a standard Apple QuickDraw PICT Version 2 document containing a 24-bit RGB raster.
 *
 * @param {Buffer} rgbBuffer - Raw RGB bytes (width * height * 3 bytes)
 * @param {number} width - Image width in pixels
 * @param {number} height - Image height in pixels
 * @param {Object} [options]
 * @param {boolean} [options.includeDiskHeader=true] - Whether to prepend the 512-byte zero header for Mac OS disk files
 * @returns {Buffer} Standard PICT v2 binary buffer
 */
function createPict24Bit(rgbBuffer, width, height, { includeDiskHeader = true } = {}) {
  const chunks = [];
  let streamLen = 0;

  function pushChunk(buf) {
    chunks.push(buf);
    streamLen += buf.length;
  }

  // 1. 512-byte disk header (Mac OS disk files only; memory scrap 'PICT' omits this header)
  if (includeDiskHeader) {
    chunks.push(Buffer.alloc(512, 0));
  }

  // 2. Standard PICT Header
  // picSize (2 bytes, 0 for V2), picFrame (8 bytes: top, left, bottom, right)
  const picHeader = Buffer.alloc(10);
  picHeader.writeUInt16BE(0, 0);       // picSize (always 0 in PICT v2, as length exceeds 64KB)
  picHeader.writeUInt16BE(0, 2);       // top
  picHeader.writeUInt16BE(0, 4);       // left
  picHeader.writeUInt16BE(height, 6);  // bottom
  picHeader.writeUInt16BE(width, 8);   // right
  pushChunk(picHeader);

  // 3. PICT v2 VersionOp and HeaderOp (Extended V2 Header)
  const v2Header = Buffer.alloc(30);
  let off = 0;
  v2Header.writeUInt16BE(0x0011, off); off += 2;      // VersionOp opcode
  v2Header.writeUInt16BE(0x02FF, off); off += 2;      // Version 2 identifier
  v2Header.writeUInt16BE(0x0C00, off); off += 2;      // HeaderOp opcode
  v2Header.writeUInt16BE(0xFFFE, off); off += 2;      // Extended V2 flag (-2)
  v2Header.writeUInt16BE(0x0000, off); off += 2;      // Reserved
  v2Header.writeUInt32BE(0x00480000, off); off += 4;  // hRes = 72.0 Fixed (0x0048.0000)
  v2Header.writeUInt32BE(0x00480000, off); off += 4;  // vRes = 72.0 Fixed
  v2Header.writeUInt16BE(0, off); off += 2;           // Best-fit srcRect top
  v2Header.writeUInt16BE(0, off); off += 2;           // left
  v2Header.writeUInt16BE(height, off); off += 2;      // bottom
  v2Header.writeUInt16BE(width, off); off += 2;       // right
  v2Header.writeUInt32BE(0x00000000, off); off += 4;  // Reserved
  pushChunk(v2Header);

  // 4. ClipRgn Opcode (0x0001)
  const clipBuf = Buffer.alloc(12);
  clipBuf.writeUInt16BE(0x0001, 0);       // ClipRgn opcode
  clipBuf.writeUInt16BE(10, 2);           // rgnSize (10 bytes for rectangular region)
  clipBuf.writeUInt16BE(0, 4);            // rgnBBox top
  clipBuf.writeUInt16BE(0, 6);            // left
  clipBuf.writeUInt16BE(height, 8);       // bottom
  clipBuf.writeUInt16BE(width, 10);       // right
  pushChunk(clipBuf);

  // 5. DirectBitsRect Opcode (0x009A)
  const rowBytes = (width * 4) | 0x8000;  // PixMap rowBytes (high bit set)
  const pixMapHdr = Buffer.alloc(52);
  off = 0;
  pixMapHdr.writeUInt16BE(0x009A, off); off += 2;      // DirectBitsRect Opcode
  pixMapHdr.writeUInt32BE(0x000000FF, off); off += 4;  // baseAddr (0x000000FF for direct PixMap in PICT)
  pixMapHdr.writeUInt16BE(rowBytes, off); off += 2;    // rowBytes
  pixMapHdr.writeUInt16BE(0, off); off += 2;           // Bounds top
  pixMapHdr.writeUInt16BE(0, off); off += 2;           // left
  pixMapHdr.writeUInt16BE(height, off); off += 2;      // bottom
  pixMapHdr.writeUInt16BE(width, off); off += 2;       // right
  pixMapHdr.writeUInt16BE(0, off); off += 2;           // pmVersion = 0
  pixMapHdr.writeUInt16BE(4, off); off += 2;           // packType = 4 (Component PackBits for 32-bit/24-bit direct color)
  pixMapHdr.writeUInt32BE(0, off); off += 4;           // packSize = 0
  pixMapHdr.writeUInt32BE(0x00480000, off); off += 4;  // hRes = 72 dpi
  pixMapHdr.writeUInt32BE(0x00480000, off); off += 4;  // vRes = 72 dpi
  pixMapHdr.writeUInt16BE(16, off); off += 2;          // pixelType = 16 (RGBDirect)
  pixMapHdr.writeUInt16BE(32, off); off += 2;          // pixelSize = 32
  pixMapHdr.writeUInt16BE(3, off); off += 2;           // cmpCount = 3 (RGB)
  pixMapHdr.writeUInt16BE(8, off); off += 2;           // cmpSize = 8 bits per component
  pixMapHdr.writeUInt32BE(0, off); off += 4;           // planeBytes = 0
  pixMapHdr.writeUInt32BE(0, off); off += 4;           // pmTable = 0
  pixMapHdr.writeUInt32BE(0, off); off += 4;           // pmReserved = 0
  pushChunk(pixMapHdr);

  // srcRect, dstRect, transfer mode (srcCopy)
  const rectsBuf = Buffer.alloc(18);
  off = 0;
  rectsBuf.writeUInt16BE(0, off); off += 2;       // srcRect top
  rectsBuf.writeUInt16BE(0, off); off += 2;       // srcRect left
  rectsBuf.writeUInt16BE(height, off); off += 2;  // bottom
  rectsBuf.writeUInt16BE(width, off); off += 2;   // right
  rectsBuf.writeUInt16BE(0, off); off += 2;       // dstRect top
  rectsBuf.writeUInt16BE(0, off); off += 2;       // dstRect left
  rectsBuf.writeUInt16BE(height, off); off += 2;  // bottom
  rectsBuf.writeUInt16BE(width, off); off += 2;   // right
  rectsBuf.writeUInt16BE(0x0000, off); off += 2;  // mode = 0 (srcCopy)
  pushChunk(rectsBuf);

  // Scanline data: packType = 4 (Component PackBits for 32-bit/24-bit direct color):
  const wideCount = (width * 4) >= 250;
  for (let y = 0; y < height; y++) {
    const rScan = Buffer.alloc(width);
    const gScan = Buffer.alloc(width);
    const bScan = Buffer.alloc(width);
    for (let x = 0; x < width; x++) {
      const idx = (y * width + x) * 3;
      rScan[x] = rgbBuffer[idx];
      gScan[x] = rgbBuffer[idx + 1];
      bScan[x] = rgbBuffer[idx + 2];
    }
    const packed = Buffer.concat([packBits(rScan), packBits(gScan), packBits(bScan)]);
    const countBuf = Buffer.alloc(wideCount ? 2 : 1);
    if (wideCount) {
      countBuf.writeUInt16BE(packed.length, 0);
    } else {
      countBuf.writeUInt8(packed.length, 0);
    }
    pushChunk(countBuf);
    pushChunk(packed);
  }

  // Word-align padding before OpEndPic if picture stream length is odd
  if ((streamLen % 2) !== 0) {
    pushChunk(Buffer.from([0]));
  }

  // 6. OpEndPic Opcode (0x00FF)
  const endBuf = Buffer.alloc(2);
  endBuf.writeUInt16BE(0x00FF, 0);
  pushChunk(endBuf);

  return Buffer.concat(chunks);
}

/**
 * Transcodes any source image buffer (PNG, JPEG, WebP) into a 24-bit QuickDraw PICT v2 file.
 * Resizes the image to fit inside maxWidth x maxHeight (default 1024x1024) while preserving aspect ratio.
 */
async function convertImageToPict24(imageBuffer, { maxWidth = 1024, maxHeight = 1024, includeDiskHeader = true } = {}) {
  const { data: rgbBuffer, info } = await sharp(imageBuffer)
    .flatten({ background: { r: 255, g: 255, b: 255 } })
    .resize(maxWidth, maxHeight, { fit: 'inside' })
    .removeAlpha()
    .raw()
    .toBuffer({ resolveWithObject: true });

  const pictBuffer = createPict24Bit(rgbBuffer, info.width, info.height, { includeDiskHeader });
  return {
    buffer: pictBuffer,
    width: info.width,
    height: info.height,
  };
}

module.exports = {
  packBits,
  createPict24Bit,
  convertImageToPict24,
};
