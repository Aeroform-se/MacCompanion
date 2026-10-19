/**
 * Data Translation & Sanitization Layer for 68k Macintosh Clients.
 *
 * Converts modern UTF-8 strings into clean ASCII / Mac OS Roman compatible bytes
 * and strips pipe ('|') and newline characters so C `strtok()` parsing on the
 * 68k client is 100% deterministic.
 */

// Common typographical characters mapped to clean ASCII equivalents
const TYPOGRAPHY_MAP = {
  '\u2018': "'", // Left single quotation mark
  '\u2019': "'", // Right single quotation mark
  '\u201C': '"', // Left double quotation mark
  '\u201D': '"', // Right double quotation mark
  '\u2013': '-', // En dash
  '\u2014': '-', // Em dash
  '\u2026': '...', // Horizontal ellipsis
  '\u00A0': ' ', // Non-breaking space
  '\u00D7': 'x', // Multiplication sign
};

// Unicode code point to Mac OS Roman byte value (0x80 - 0xFF)
const UNICODE_TO_MACROMAN = new Map([
  [0x00C4, 0x80], [0x00C5, 0x81], [0x00C7, 0x82], [0x00C9, 0x83],
  [0x00D1, 0x84], [0x00D6, 0x85], [0x00DC, 0x86], [0x00E1, 0x87],
  [0x00E0, 0x88], [0x00E2, 0x89], [0x00E4, 0x8A], [0x00E3, 0x8B],
  [0x00E5, 0x8C], [0x00E7, 0x8D], [0x00E9, 0x8E], [0x00E8, 0x8F],
  [0x00EA, 0x90], [0x00EB, 0x91], [0x00ED, 0x92], [0x00EC, 0x93],
  [0x00EE, 0x94], [0x00EF, 0x95], [0x00F1, 0x96], [0x00F3, 0x97],
  [0x00F2, 0x98], [0x00F4, 0x99], [0x00F6, 0x9A], [0x00F5, 0x9B],
  [0x00FA, 0x9C], [0x00F9, 0x9D], [0x00FB, 0x9E], [0x00FC, 0x9F],
  [0x00DF, 0xA7], [0x00AE, 0xA8], [0x00A9, 0xA9], [0x2122, 0xAA],
  [0x00C6, 0xAE], [0x00D8, 0xAF], [0x00E6, 0xBE], [0x00F8, 0xBF],
  [0x00C0, 0xCB], [0x00C3, 0xCC], [0x00D5, 0xCD], [0x0152, 0xCE],
  [0x0153, 0xCF], [0x00D2, 0xF1], [0x00DA, 0xF2], [0x00DB, 0xF3],
  [0x00D9, 0xF4],
]);

/**
 * Sanitizes a single field for pipe-delimited output.
 * Replaces pipes with '/' and newlines/tabs with spaces.
 */
function sanitizeField(value) {
  if (value === null || value === undefined) return '';
  let str = String(value);
  for (const [char, replacement] of Object.entries(TYPOGRAPHY_MAP)) {
    str = str.split(char).join(replacement);
  }
  return str
    .replace(/\|/g, '/')
    .replace(/[\r\n\t]+/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
}

/**
 * Joins an array of values into a single pipe-delimited row.
 */
function formatPipeRow(fields) {
  return fields.map(sanitizeField).join('|');
}

/**
 * Joins multiple rows of fields into a newline-separated pipe-delimited payload.
 */
function formatPipeTable(rows) {
  if (!rows || rows.length === 0) return 'EMPTY\n';
  return rows.map(formatPipeRow).join('\n') + '\n';
}

/**
 * Converts a UTF-8 JavaScript string into a Mac OS Roman Buffer for authentic
 * rendering on System 7 QuickDraw fonts (Chicago, Geneva).
 * Characters outside MacRoman are transliterated to base ASCII where possible.
 */
function toMacRomanBuffer(str) {
  const bytes = [];
  for (let i = 0; i < str.length; i++) {
    const code = str.charCodeAt(i);
    if (code === 0x0A || (code >= 0x20 && code <= 0x7E)) {
      bytes.push(code);
    } else if (UNICODE_TO_MACROMAN.has(code)) {
      bytes.push(UNICODE_TO_MACROMAN.get(code));
    } else {
      // Normalize accented characters not in MacRoman to base ASCII
      const decomposed = str[i].normalize('NFD').replace(/[\u0300-\u036f]/g, '');
      const asciiCode = decomposed.charCodeAt(0);
      if (asciiCode >= 0x20 && asciiCode <= 0x7E) {
        bytes.push(asciiCode);
      } else {
        bytes.push(0x3F); // '?'
      }
    }
  }
  return Buffer.from(bytes);
}

/**
 * Sends a pipe-delimited response.
 * By default sends UTF-8 text/plain so modern browsers & curl read it cleanly,
 * or raw Mac OS Roman bytes if `?encoding=macroman` is passed by the 68k client.
 */
function sendPipeResponse(req, res, payloadString) {
  res.setHeader('Content-Type', 'text/plain; charset=utf-8');
  if (req.query.encoding === 'macroman') {
    const buf = toMacRomanBuffer(payloadString);
    res.setHeader('Content-Length', buf.length);
    return res.end(buf);
  }
  return res.send(payloadString);
}

module.exports = {
  sanitizeField,
  formatPipeRow,
  formatPipeTable,
  toMacRomanBuffer,
  sendPipeResponse,
};
