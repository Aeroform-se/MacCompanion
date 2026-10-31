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
 * Sanitizes a filename for Classic Mac OS HFS:
 * - Max length 31 characters
 * - No colons (':') which are path separators in classic Mac OS
 */
function sanitizeMacFilename(name) {
  if (!name) return 'Untitled';
  // Replace colons with dashes or underscores
  let clean = name.replace(/:/g, '-').trim();
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
  const fileName = buf.toString('latin1', 2, 2 + nameLen);
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

  // Byte 1: Filename length (max 31 chars for classic HFS)
  const pNameLen = Math.min(Buffer.byteLength(fileName, 'latin1'), 31);
  header[1] = pNameLen;

  // Bytes 2..64: Filename characters
  header.write(fileName, 2, pNameLen, 'latin1');

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
