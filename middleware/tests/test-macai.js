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
const { SERVER_VERSION_TAG } = require('../src/version');

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
  console.log(`--- MacAI Module Verification Tests (Middleware ${SERVER_VERSION_TAG}) ---`);
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

    console.log('--- ALL MACAI TESTS PASSED ---');
  } finally {
    server.close();
    process.exit(0);
  }
})();
