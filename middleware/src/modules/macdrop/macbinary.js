/**
 * MacBinary II Encoding and Detection Utilities for MacDrop
 *
 * Implements standard MacBinary II specification:
 * - 128-byte header with CCITT CRC-16 checksum
 * - Classic Mac OS 1904 epoch timestamp conversion
 * - Type and Creator mapping for modern file extensions
 * - Automatic detection of existing MacBinary (.bin) payloads
 */

const MAC_EPOCH_OFFSET = 2082844800; // Seconds between Jan 1 1904 and Jan 1 1970

/**
 * Extension to Classic Mac OS Type / Creator mapping
 */
const EXTENSION_MAP = {
  // Text & documents
  txt:   { type: 'TEXT', creator: 'ttxt' },
  text:  { type: 'TEXT', creator: 'ttxt' },
  log:   { type: 'TEXT', creator: 'ttxt' },
  c:     { type: 'TEXT', creator: 'ttxt' },
  h:     { type: 'TEXT', creator: 'ttxt' },
  py:    { type: 'TEXT', creator: 'ttxt' },
  js:    { type: 'TEXT', creator: 'ttxt' },
  json:  { type: 'TEXT', creator: 'ttxt' },
  csv:   { type: 'TEXT', creator: 'ttxt' },
  md:    { type: 'TEXT', creator: 'ttxt' },
  html:  { type: 'TEXT', creator: 'MSIE' },
  htm:   { type: 'TEXT', creator: 'MSIE' },
  pdf:   { type: 'PDF ', creator: 'CARO' },
  rtf:   { type: 'TEXT', creator: 'MSWD' },
  doc:   { type: 'WDBN', creator: 'MSWD' },

  // Archives & Disk Images
  sit:   { type: 'SIT!', creator: 'SIT!' },
  cpt:   { type: 'PACT', creator: 'CPTO' },
  sea:   { type: 'APPL', creator: '????' },
  dsk:   { type: 'dImg', creator: 'dImg' },
  img:   { type: 'dImg', creator: 'dImg' },
  image: { type: 'dImg', creator: 'dImg' },
  bin:   { type: 'BINA', creator: 'ttxt' },
  hqx:   { type: 'TEXT', creator: 'BnHx' },
  zip:   { type: 'ZIP ', creator: 'ZIP ' },

  // Graphics
  gif:   { type: 'GIFf', creator: 'ogle' },
  jpg:   { type: 'JPEG', creator: 'ogle' },
  jpeg:  { type: 'JPEG', creator: 'ogle' },
  pict:  { type: 'PICT', creator: 'ttxt' },
  pct:   { type: 'PICT', creator: 'ttxt' },
  bmp:   { type: 'BMP ', creator: 'ogle' },
  tif:   { type: 'TIFF', creator: 'ogle' },
  tiff:  { type: 'TIFF', creator: 'ogle' },
  png:   { type: 'PNGf', creator: 'ogle' },

  // Audio / Music
  mid:   { type: 'Midi', creator: 'ttxt' },
  midi:  { type: 'Midi', creator: 'ttxt' },
  wav:   { type: 'WAVE', creator: 'SCPL' },
  wave:  { type: 'WAVE', creator: 'SCPL' },
  aif:   { type: 'AIFF', creator: 'SCPL' },
  aiff:  { type: 'AIFF', creator: 'SCPL' },
  mp3:   { type: 'Mp3 ', creator: 'SCPL' },
  mod:   { type: 'MOD ', creator: 'SCPL' },

  // HyperCard & System
  stk:   { type: 'STAK', creator: 'WILD' },
  rsrc:  { type: 'rsrc', creator: 'RSED' },
  font:  { type: 'FFIL', creator: 'DMOV' },
};

const DEFAULT_MAPPING = { type: 'BINA', creator: 'ttxt' };

/**
 * Calculates CCITT CRC-16 (standard polynomial 0x1021, seed 0x0000)
 * used in MacBinary II specification for the first 124 bytes.
 */
function calcMacBinaryCRC(buf, length = 124) {
  let crc = 0;
  for (let i = 0; i < length; i++) {
    crc ^= (buf[i] << 8);
    for (let j = 0; j < 8; j++) {
      if (crc & 0x8000) {
        crc = ((crc << 1) ^ 0x1021) & 0xFFFF;
      } else {
        crc = (crc << 1) & 0xFFFF;
      }
    }
  }
  return crc;
}

/**
 * Checks whether a buffer represents a valid MacBinary II / III file.
 */
function isMacBinary(buf) {
  if (!buf || buf.length < 128) return false;

  // Byte 0 must be 0
  if (buf[0] !== 0x00) return false;

  // Byte 1 is filename length (1..63)
  const nameLen = buf[1];
  if (nameLen < 1 || nameLen > 63) return false;

  // Bytes 74 and 82 must be 0
  if (buf[74] !== 0x00 || buf[82] !== 0x00) return false;

  // Check CRC at bytes 124-125
  const expectedCRC = buf.readUInt16BE(124);
  const actualCRC = calcMacBinaryCRC(buf, 124);

  return expectedCRC === actualCRC;
}

/**
 * Resolves Classic Mac OS Type and Creator codes for a given filename.
 */
function getTypeCreatorForFilename(filename) {
  if (!filename) return { ...DEFAULT_MAPPING };
  const ext = filename.split('.').pop().toLowerCase();
  return EXTENSION_MAP[ext] || { ...DEFAULT_MAPPING };
}

/**
 * Mapping of common Unicode characters (Latin-1 + typographical symbols) to
 * Classic Mac OS Roman byte values (0x80..0xFF), with ASCII transliteration fallback
 * for characters outside Mac OS Roman.
 */
const UNICODE_TO_MAC_ROMAN = {
  0x00C4: 0x80, // Ä
  0x00C5: 0x81, // Å
  0x00C7: 0x82, // Ç
  0x00C9: 0x83, // É
  0x00D1: 0x84, // Ñ
  0x00D6: 0x85, // Ö
  0x00DC: 0x86, // Ü
  0x00E1: 0x87, // á
  0x00E0: 0x88, // à
  0x00E2: 0x89, // â
  0x00E4: 0x8A, // ä
  0x00E3: 0x8B, // ã
  0x00E5: 0x8C, // å
  0x00E7: 0x8D, // ç
  0x00E9: 0x8E, // é
  0x00E8: 0x8F, // è
  0x00EA: 0x90, // ê
  0x00EB: 0x91, // ë
  0x00ED: 0x92, // í
  0x00EC: 0x93, // ì
  0x00EE: 0x94, // î
  0x00EF: 0x95, // ï
  0x00F1: 0x96, // ñ
  0x00F3: 0x97, // ó
  0x00F2: 0x98, // ò
  0x00F4: 0x99, // ô
  0x00F6: 0x9A, // ö
  0x00F5: 0x9B, // õ
  0x00FA: 0x9C, // ú
  0x00F9: 0x9D, // ù
  0x00FB: 0x9E, // û
  0x00FC: 0x9F, // ü
  0x2020: 0xA0, // †
  0x00B0: 0xA1, // °
  0x00A2: 0xA2, // ¢
  0x00A3: 0xA3, // £
  0x00A7: 0xA4, // §
  0x2022: 0xA5, // •
  0x00B6: 0xA6, // ¶
  0x00DF: 0xA7, // ß
  0x00AE: 0xA8, // ®
  0x00A9: 0xA9, // ©
  0x2122: 0xAA, // ™
  0x00B4: 0xAB, // ´
  0x00A8: 0xAC, // ¨
  0x00C6: 0xAE, // Æ
  0x00D8: 0xAF, // Ø
  0x00F8: 0xBF, // ø
  0x00BF: 0xC0, // ¿
  0x00A1: 0xC1, // ¡
  0x00AB: 0xC7, // «
  0x00BB: 0xC8, // »
  0x2026: 0xC9, // …
  0x00A0: 0xCA, // non-breaking space
  0x00C0: 0xCB, // À
  0x00C3: 0xCC, // Ã
  0x00D5: 0xCD, // Õ
  0x0152: 0xCE, // Œ
  0x0153: 0xCF, // œ
  0x2013: 0xD0, // –
  0x2014: 0xD1, // —
  0x201C: 0xD2, // “
  0x201D: 0xD3, // ”
  0x2018: 0xD4, // ‘
  0x2019: 0xD5, // ’
  0x00F7: 0xD6, // ÷
  0x00FF: 0xD8, // ÿ
  0x0178: 0xD9, // Ÿ
  0x00E6: 0xBE, // æ
  0x00C2: 0xE5, // Â
  0x00CA: 0xE6, // Ê
  0x00C1: 0xE7, // Á
  0x00CB: 0xE8, // Ë
  0x00C8: 0xE9, // È
  0x00CD: 0xEA, // Í
  0x00CE: 0xEB, // Î
  0x00CF: 0xEC, // Ï
  0x00CC: 0xED, // Ì
  0x00D3: 0xEE, // Ó
  0x00D4: 0xEF, // Ô
  0x00D2: 0xF1, // Ò
  0x00DA: 0xF2, // Ú
  0x00DB: 0xF3, // Û
  0x00D9: 0xF4, // Ù
};

const MAC_ROMAN_TO_UNICODE = {};
for (const [uni, mac] of Object.entries(UNICODE_TO_MAC_ROMAN)) {
  MAC_ROMAN_TO_UNICODE[mac] = Number(uni);
}

/**
 * Encodes a Unicode JavaScript string into a Classic Mac OS Roman Buffer (max `maxBytes` bytes).
 * Normalizes macOS NFD decomposed accents (`a\u030a` -> `å`) to NFC first so Swedish/European
 * characters (`å`, `ä`, `ö`, `Å`, `Ä`, `Ö`, `é`, `ü`, etc.) map cleanly to single Mac Roman bytes!
 */
function encodeMacRoman(str, maxBytes = 31) {
  const normalized = String(str || '').normalize('NFC');
  const bytes = [];
  for (const ch of normalized) {
    if (bytes.length >= maxBytes) break;
    const code = ch.codePointAt(0);
    if (code >= 0x20 && code <= 0x7E) {
      bytes.push(code);
    } else if (UNICODE_TO_MAC_ROMAN[code] !== undefined) {
      bytes.push(UNICODE_TO_MAC_ROMAN[code]);
    } else {
      // Strip combining marks to fall back to base ASCII letter if outside Mac OS Roman
      const base = ch.normalize('NFD').replace(/[\u0300-\u036f]/g, '');
      if (base && base.length > 0) {
        for (const bch of base) {
          if (bytes.length >= maxBytes) break;
          const bcode = bch.codePointAt(0);
          if (bcode >= 0x20 && bcode <= 0x7E) {
            bytes.push(bcode);
          } else {
            bytes.push(0x5F); // '_'
          }
        }
      } else {
        bytes.push(0x5F); // '_'
      }
    }
  }
  return Buffer.from(bytes);
}

/**
 * Decodes a Classic Mac OS Roman byte slice into a modern UTF-8 JavaScript string.
 */
function decodeMacRoman(buf, start = 0, end = buf.length) {
  let out = '';
  const limit = Math.min(buf.length, end);
  for (let i = start; i < limit; i++) {
    const b = buf[i];
    if (b < 0x80) {
      out += String.fromCharCode(b);
    } else if (MAC_ROMAN_TO_UNICODE[b] !== undefined) {
      out += String.fromCharCode(MAC_ROMAN_TO_UNICODE[b]);
    } else {
      out += String.fromCharCode(b);
    }
  }
  return out;
}

/**
 * Sanitizes a filename for Classic Mac OS HFS:
 * - Normalizes NFD/NFC Unicode (so macOS Finder filenames with å, ä, ö work seamlessly)
 * - Replaces colons (':') and control chars
 * - Max length 31 characters (preserving file extension)
 */
function sanitizeMacFilename(name) {
  if (!name) return 'Untitled';
  let clean = String(name)
    .normalize('NFC')
    .replace(/[:/\\]/g, '-')
    .replace(/[\x00-\x1F\x7F]/g, '')
    .trim();
  if (clean.length > 31) {
    const extIdx = clean.lastIndexOf('.');
    if (extIdx > 0 && clean.length - extIdx <= 8) {
      const ext = clean.slice(extIdx);
      clean = clean.slice(0, 31 - ext.length) + ext;
    } else {
      clean = clean.slice(0, 31);
    }
  }
  return clean || 'Untitled';
}

/**
 * Parses an existing 128-byte MacBinary header.
 */
function parseMacBinaryHeader(buf) {
  if (!buf || buf.length < 128) return null;

  const nameLen = Math.min(buf[1], 63);
  const fileName = decodeMacRoman(buf, 2, 2 + nameLen);
  const type = buf.toString('latin1', 65, 69);
  const creator = buf.toString('latin1', 69, 73);
  const flags = (buf[73] << 8) | buf[101];
  const dataLength = buf.readUInt32BE(83);
  const rsrcLength = buf.readUInt32BE(87);
  const creationDate = buf.readUInt32BE(91);
  const modDate = buf.readUInt32BE(95);

  return {
    fileName,
    type,
    creator,
    flags,
    dataLength,
    rsrcLength,
    creationDate,
    modDate,
  };
}

/**
 * Creates a complete MacBinary II file buffer wrapping the given data fork payload.
 *
 * @param {Object} opts
 * @param {string} opts.fileName - Target filename on Macintosh
 * @param {Buffer} opts.dataBuffer - Data fork contents
 * @param {Buffer} [opts.rsrcBuffer] - Optional resource fork contents
 * @param {string} [opts.fileType] - 4-char Mac OS Type (e.g. 'TEXT')
 * @param {string} [opts.fileCreator] - 4-char Mac OS Creator (e.g. 'ttxt')
 * @param {number} [opts.modTime] - Unix timestamp (milliseconds)
 * @returns {Buffer} Complete MacBinary II stream
 */
function createMacBinary(opts) {
  const fileName = sanitizeMacFilename(opts.fileName || 'Untitled');
  const mapping = getTypeCreatorForFilename(fileName);
  const fileType = (opts.fileType || mapping.type || 'BINA').padEnd(4, ' ').slice(0, 4);
  const fileCreator = (opts.fileCreator || mapping.creator || 'ttxt').padEnd(4, ' ').slice(0, 4);

  const dataFork = opts.dataBuffer || Buffer.alloc(0);
  const rsrcFork = opts.rsrcBuffer || Buffer.alloc(0);

  const dataLen = dataFork.length;
  const rsrcLen = rsrcFork.length;

  const nowSec = Math.floor((opts.modTime ? opts.modTime : Date.now()) / 1000);
  const macTime = nowSec + MAC_EPOCH_OFFSET;

  // Build 128-byte MacBinary II header
  const header = Buffer.alloc(128, 0);

  // Byte 0: Must be 0
  header[0] = 0x00;

  // Byte 1: Filename length (max 31 chars for classic HFS, encoded in Mac OS Roman)
  const macRomanName = encodeMacRoman(fileName, 31);
  const pNameLen = Math.min(macRomanName.length, 31);
  header[1] = pNameLen;

  // Bytes 2..64: Filename characters in Mac OS Roman
  macRomanName.copy(header, 2, 0, pNameLen);

  // Bytes 65..68: File Type (4 ASCII chars)
  header.write(fileType, 65, 4, 'ascii');

  // Bytes 69..72: File Creator (4 ASCII chars)
  header.write(fileCreator, 69, 4, 'ascii');

  // Byte 73: Finder flags high byte
  header[73] = 0x00;

  // Byte 74: Must be 0
  header[74] = 0x00;

  // Bytes 75..78: Window vertical/horizontal position (0)
  // Bytes 79..80: Window/folder ID (0)
  // Byte 81: Protected flag (0)
  // Byte 82: Must be 0

  // Bytes 83..86: Data fork length (UInt32 big-endian)
  header.writeUInt32BE(dataLen, 83);

  // Bytes 87..90: Resource fork length (UInt32 big-endian)
  header.writeUInt32BE(rsrcLen, 87);

  // Bytes 91..94: Creation date (Mac OS epoch)
  header.writeUInt32BE(macTime, 91);

  // Bytes 95..98: Modification date (Mac OS epoch)
  header.writeUInt32BE(macTime, 95);

  // Byte 101: Finder flags low byte
  header[101] = 0x00;

  // Bytes 122..123: MacBinary II version marker (0x81 = 129)
  header[122] = 0x81;
  header[123] = 0x81;

  // Calculate CRC-16 over first 124 bytes
  const crc = calcMacBinaryCRC(header, 124);
  header.writeUInt16BE(crc, 124);

  // Pad data fork to 128-byte boundary
  const dataPad = (128 - (dataLen % 128)) % 128;
  const rsrcPad = (128 - (rsrcLen % 128)) % 128;

  const parts = [header, dataFork];
  if (dataPad > 0) parts.push(Buffer.alloc(dataPad, 0));

  if (rsrcLen > 0) {
    parts.push(rsrcFork);
    if (rsrcPad > 0) parts.push(Buffer.alloc(rsrcPad, 0));
  }

  return Buffer.concat(parts);
}

module.exports = {
  isMacBinary,
  parseMacBinaryHeader,
  createMacBinary,
  getTypeCreatorForFilename,
  sanitizeMacFilename,
  calcMacBinaryCRC,
  EXTENSION_MAP,
  MAC_EPOCH_OFFSET,
};
