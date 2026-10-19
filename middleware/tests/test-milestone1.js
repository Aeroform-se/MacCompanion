const dgram = require('dgram');
const http = require('http');
const assert = require('assert');
const { createApp, registry } = require('../src/index');
const { startDiscoveryServer, DISCOVERY_MAGIC } = require('../src/core/discovery');
const spotifyModule = require('../src/modules/spotify');

const TEST_HTTP_PORT = 16800;
const TEST_UDP_PORT = 16801;

function httpGet(path) {
  return new Promise((resolve, reject) => {
    http.get({ hostname: '127.0.0.1', port: TEST_HTTP_PORT, path }, (res) => {
      let body = '';
      res.on('data', (chunk) => (body += chunk));
      res.on('end', () => resolve({ statusCode: res.statusCode, headers: res.headers, body }));
    }).on('error', reject);
  });
}

function udpPing(message) {
  return new Promise((resolve, reject) => {
    const client = dgram.createSocket('udp4');
    const timeout = setTimeout(() => {
      client.close();
      reject(new Error('UDP discovery timed out'));
    }, 3000);

    client.on('message', (msg) => {
      clearTimeout(timeout);
      client.close();
      resolve(msg.toString('utf8'));
    });

    const buf = Buffer.from(message, 'ascii');
    client.send(buf, 0, buf.length, TEST_UDP_PORT, '127.0.0.1', (err) => {
      if (err) {
        clearTimeout(timeout);
        client.close();
        reject(err);
      }
    });
  });
}

async function runTests() {
  console.log('--- Milestone 1 Verification Tests ---');
  const app = createApp();
  const server = await new Promise((resolve) => {
    const s = app.listen(TEST_HTTP_PORT, '127.0.0.1', () => resolve(s));
  });
  const udpSocket = startDiscoveryServer({
    httpPort: TEST_HTTP_PORT,
    udpPort: TEST_UDP_PORT,
    registry,
  });

  try {
    // 1. Test Status Web UI (Happy Mac HTML)
    const homeRes = await httpGet('/');
    assert.strictEqual(homeRes.statusCode, 200, 'Root UI should return 200');
    assert.ok(homeRes.body.includes('/img/sadmac.png') && homeRes.body.includes('/img/happymac.png'), 'Root UI should include Sad Mac and Happy Mac images');
    console.log('[PASS] Status Web UI (Happy Mac) served on /');

    // 2. Test Global & Modular Status JSON
    const statusRes = await httpGet('/api/status');
    assert.strictEqual(statusRes.statusCode, 200);
    const statusData = JSON.parse(statusRes.body);
    assert.ok(statusData.modules.spotify, 'Spotify module should be registered in /api/status');
    console.log('[PASS] Modular /api/status returns registered modules:', Object.keys(statusData.modules));

    // 3. Test Pipe-Delimited Ping Endpoints
    const pingRes = await httpGet('/api/ping');
    assert.strictEqual(pingRes.statusCode, 200);
    assert.ok(pingRes.body.startsWith('MACCOMPANION_SERVER|'), 'Ping response should start with MACCOMPANION_SERVER|');
    console.log('[PASS] Pipe-delimited HTTP /api/ping ->', pingRes.body.trim());

    const spotifyPing = await httpGet('/api/spotify/ping');
    assert.strictEqual(spotifyPing.statusCode, 200);
    assert.ok(spotifyPing.body.startsWith('SPOTIFY|'), 'Spotify ping should start with SPOTIFY|');
    console.log('[PASS] Pipe-delimited /api/spotify/ping ->', spotifyPing.body.trim());

    // 4. Test UDP Broadcast Discovery
    const udpReply = await udpPing(DISCOVERY_MAGIC);
    const parts = udpReply.trim().split('|');
    assert.strictEqual(parts[0], 'MACCOMPANION_SERVER');
    assert.strictEqual(parts[2], String(TEST_HTTP_PORT));
    assert.ok(parts[3].includes('spotify'));
    console.log('[PASS] UDP broadcast discovery reply ->', udpReply.trim());

    // 5. Test Persistent Storage Volume (./data/spotify.json)
    spotifyModule.store.save({ testPersistence: 'verified' });
    const reloaded = spotifyModule.store.load();
    assert.strictEqual(reloaded.testPersistence, 'verified');
    spotifyModule.store.clear();
    console.log('[PASS] Persistent module store read/write verified at:', spotifyModule.store.filePath);

    console.log('--- ALL MILESTONE 1 TESTS PASSED ---');
  } finally {
    udpSocket.close();
    server.close();
  }
}

runTests().catch((err) => {
  console.error('Test failed:', err);
  process.exit(1);
});
