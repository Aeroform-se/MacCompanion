const assert = require('assert');
const fs = require('fs');
const path = require('path');
const http = require('http');

const testDataDir = process.env.DATA_DIR || '/tmp/maccompanion-test-macai';
process.env.DATA_DIR = testDataDir;
if (fs.existsSync(testDataDir)) {
  fs.rmSync(testDataDir, { recursive: true, force: true });
}
fs.mkdirSync(testDataDir, { recursive: true });

const { createApp } = require('../src/index');
const { SERVER_VERSION_SLUG } = require('../src/version');

function httpRequest(port, method, reqPath, bodyObj = null) {
  return new Promise((resolve, reject) => {
    const payload = bodyObj ? JSON.stringify(bodyObj) : null;
    const req = http.request({
      hostname: '127.0.0.1',
      port,
      path: reqPath,
      method,
      headers: payload ? {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(payload)
      } : {}
    }, (res) => {
      let data = '';
      res.on('data', (chunk) => { data += chunk; });
      res.on('end', () => resolve({ statusCode: res.statusCode, body: data }));
    });
    req.on('error', reject);
    if (payload) req.write(payload);
    req.end();
  });
}

(async () => {
  console.log(`--- MacAI Module Verification Tests (Middleware ${SERVER_VERSION_SLUG}) ---`);
  const app = createApp();
  const server = await new Promise((resolve) => {
    const s = app.listen(16810, '127.0.0.1', () => resolve(s));
  });

  try {
    // 1. Verify initial /api/macai/ping is AWAITING_LOGIN
    const ping1 = await httpRequest(16810, 'GET', '/api/macai/ping');
    assert(ping1.body.startsWith('MACAI|0|AWAITING_LOGIN|'), `Unexpected initial ping: ${ping1.body}`);
    console.log('[PASS] Initial /api/macai/ping ->', ping1.body);

    // 2. Configure Gemini & OpenAI keys via /api/macai/config
    const cfgRes = await httpRequest(16810, 'POST', '/api/macai/config', {
      geminiApiKey: 'test-gemini-key',
      openaiApiKey: 'test-openai-key',
      activeProvider: 'gemini',
      activeTier: 'pro'
    });
    const cfgJson = JSON.parse(cfgRes.body);
    assert.strictEqual(cfgJson.ok, true);
    console.log('[PASS] Saved MacAI provider keys via /api/macai/config');

    // 3. Verify /api/macai/ping is now READY
    const ping2 = await httpRequest(16810, 'GET', '/api/macai/ping');
    assert.strictEqual(ping2.body, 'MACAI|1|READY|gemini|pro|gemini,openai');
    console.log('[PASS] Configured /api/macai/ping ->', ping2.body);

    // 4. Verify /api/macai/providers pipe-delimited output
    const provRes = await httpRequest(16810, 'GET', '/api/macai/providers');
    assert(provRes.body.includes('ACTIVE|gemini|Gemini|pro'));
    assert(provRes.body.includes('PROVIDER|gemini|Gemini|1|fast,pro,thinking'));
    assert(provRes.body.includes('PROVIDER|openai|ChatGPT|1|fast,pro,thinking'));
    assert(provRes.body.includes('PROVIDER|claude|Claude|0|fast,pro,thinking'));
    console.log('[PASS] /api/macai/providers pipe-delimited listing verified');

    // 5. Verify /api/macai/select updates active provider & tier
    const selRes = await httpRequest(16810, 'GET', '/api/macai/select?provider=openai&tier=thinking');
    assert.strictEqual(selRes.body, 'OK|openai|ChatGPT|thinking');
    console.log('[PASS] /api/macai/select ->', selRes.body);

    // 6. Verify deleting all AI chats via DELETE /api/macai/chats
    const delRes = await httpRequest(16810, 'DELETE', '/api/macai/chats');
    const delJson = JSON.parse(delRes.body);
    assert.strictEqual(delJson.ok, true);
    assert.strictEqual(delJson.chatCount, 0);
    console.log('[PASS] /api/macai/chats DELETE cleared all chat history');

    // 7. Verify Image serving & 24-bit PICT on-demand conversion
    const sharp = require('sharp');
    const imgDir = path.join(testDataDir, 'images');
    if (!fs.existsSync(imgDir)) fs.mkdirSync(imgDir, { recursive: true });
    const sampleSvg = `<svg width="400" height="300" xmlns="http://www.w3.org/2000/svg"><rect width="400" height="300" fill="#336699"/></svg>`;
    const samplePng = await sharp(Buffer.from(sampleSvg)).png().toBuffer();
    fs.writeFileSync(path.join(imgDir, 'img_sample.png'), samplePng);

    // Verify 1-bit dithered bitmap endpoint
    const rawRes = await new Promise((resolve, reject) => {
      http.get('http://127.0.0.1:16810/api/macai/image/img_sample', (res) => {
        const chunks = [];
        res.on('data', c => chunks.push(c));
        res.on('end', () => resolve({ status: res.statusCode, buf: Buffer.concat(chunks) }));
      }).on('error', reject);
    });
    assert.strictEqual(rawRes.status, 200);
    assert(rawRes.buf.length > 8, 'Raw image buffer should have header + bitmap');
    const rawW = rawRes.buf.readUInt16BE(0);
    const rawH = rawRes.buf.readUInt16BE(2);
    const rawRb = rawRes.buf.readUInt16BE(4);
    assert.strictEqual(rawW, 240);
    assert.strictEqual(rawH, 180); // 400x300 fit: 'inside' within 240x300 bounding box
    console.log(`[PASS] /api/macai/image/img_sample -> 1-bit QuickDraw bitmap (${rawW}x${rawH}, rowBytes: ${rawRb})`);

    // Verify 24-bit PICT v2 on-demand conversion endpoint
    const pictRes = await new Promise((resolve, reject) => {
      http.get('http://127.0.0.1:16810/api/macai/image/img_sample/pict', (res) => {
        const chunks = [];
        res.on('data', c => chunks.push(c));
        res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, buf: Buffer.concat(chunks) }));
      }).on('error', reject);
    });
    assert.strictEqual(pictRes.status, 200);
    assert(pictRes.headers['content-disposition'].includes('img_sample.pict'));
    assert(pictRes.buf.length > 512, 'PICT should include 512-byte disk header and data');
    // Verify Color QuickDraw PICT v2 DirectBitsRect header fields
    const packType = pictRes.buf.readUInt16BE(582);
    const pixelSize = pictRes.buf.readUInt16BE(598);
    const cmpCount = pictRes.buf.readUInt16BE(600);
    assert.strictEqual(packType, 4, 'packType must be 4 (Component PackBits for 32/24-bit direct color)');
    assert.strictEqual(pixelSize, 32, 'pixelSize must be 32 bits');
    assert.strictEqual(cmpCount, 3, 'cmpCount must be 3 (RGB components)');
    console.log(`[PASS] /api/macai/image/img_sample/pict -> 24-bit PICT v2 file (${pictRes.buf.length} bytes, packType: ${packType}, cmpCount: ${cmpCount})`);

    console.log('--- ALL MACAI TESTS PASSED ---');
  } finally {
    server.close();
    process.exit(0);
  }
})();
