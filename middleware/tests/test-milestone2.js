const http = require('http');
const assert = require('assert');
const { createApp } = require('../src/index');
const spotifyModule = require('../src/modules/spotify');
const { generateCodeChallenge } = require('../src/modules/spotify/pkce');

const TEST_HTTP_PORT = 16802;

function httpRequest(method, path, bodyObj = null) {
  return new Promise((resolve, reject) => {
    const payload = bodyObj ? JSON.stringify(bodyObj) : null;
    const req = http.request(
      {
        hostname: '127.0.0.1',
        port: TEST_HTTP_PORT,
        path,
        method,
        headers: payload
          ? {
              'Content-Type': 'application/json',
              'Content-Length': Buffer.byteLength(payload),
            }
          : {},
      },
      (res) => {
        const chunks = [];
        res.on('data', (chunk) => chunks.push(chunk));
        res.on('end', () => {
          const buffer = Buffer.concat(chunks);
          resolve({
            statusCode: res.statusCode,
            headers: res.headers,
            body: buffer.toString('utf8'),
            buffer,
          });
        });
      }
    );
    req.on('error', reject);
    if (payload) req.write(payload);
    req.end();
  });
}

async function runTests() {
  console.log('--- Milestone 2 Verification Tests (PKCE OAuth & Web Config) ---');
  spotifyModule.store.clear();
  delete process.env.SPOTIFY_CLIENT_ID;
  delete process.env.SPOTIFY_REDIRECT_URI;

  const app = createApp();
  const server = await new Promise((resolve) => {
    const s = app.listen(TEST_HTTP_PORT, '127.0.0.1', () => resolve(s));
  });

  try {
    // 1. Verify happymac.png and sadmac.png are served properly
    const happyImg = await httpRequest('GET', '/img/happymac.png');
    assert.strictEqual(happyImg.statusCode, 200);
    assert.ok(happyImg.buffer.length > 1000, 'happymac.png should be non-empty PNG');

    const sadImg = await httpRequest('GET', '/img/sadmac.png');
    assert.strictEqual(sadImg.statusCode, 200);
    assert.ok(sadImg.buffer.length > 1000, 'sadmac.png should be non-empty PNG');
    console.log('[PASS] happymac.png and sadmac.png served from /img/');

    // 2. Verify initial status is AWAITING_CONFIG when no env or saved clientId exists
    const initialStatus = JSON.parse((await httpRequest('GET', '/api/spotify/status')).body);
    assert.strictEqual(initialStatus.state, 'AWAITING_CONFIG');
    console.log('[PASS] Initial state without Client ID -> AWAITING_CONFIG');

    // 3. Save Client ID via Web UI endpoint (POST /api/spotify/config)
    const configRes = await httpRequest('POST', '/api/spotify/config', {
      clientId: 'web_ui_client_id_123',
    });
    assert.strictEqual(configRes.statusCode, 200);
    const configData = JSON.parse(configRes.body);
    assert.strictEqual(configData.status.state, 'AWAITING_LOGIN');
    assert.strictEqual(configData.status.config.clientId, 'web_ui_client_id_123');
    assert.strictEqual(configData.status.config.redirectUri, 'https://aeroform-se.github.io/spotify68k/callback.html');
    assert.strictEqual(configData.status.config.source, 'store');
    console.log('[PASS] Web UI configuration saved to persistent store -> AWAITING_LOGIN');

    // 4. Verify environment variable override (SPOTIFY_CLIENT_ID)
    process.env.SPOTIFY_CLIENT_ID = 'env_override_client_id_999';
    const envStatus = JSON.parse((await httpRequest('GET', '/api/spotify/status')).body);
    assert.strictEqual(envStatus.config.clientId, 'env_override_client_id_999');
    assert.strictEqual(envStatus.config.source, 'env');
    delete process.env.SPOTIFY_CLIENT_ID;
    console.log('[PASS] Environment variable SPOTIFY_CLIENT_ID overrides store when present');

    // 5. Initiate PKCE OAuth Login (GET /api/spotify/login?origin=http://192.168.1.50:6800)
    const loginRes = await httpRequest('GET', '/api/spotify/login?origin=' + encodeURIComponent('http://192.168.1.50:6800'));
    assert.strictEqual(loginRes.statusCode, 302, 'Login should redirect to Spotify authorize URL');
    const redirectLocation = loginRes.headers.location;
    assert.ok(redirectLocation.startsWith('https://accounts.spotify.com/authorize?'));
    assert.ok(redirectLocation.includes(encodeURIComponent('https://aeroform-se.github.io/spotify68k/callback.html')));

    const storedAfterLoginInit = spotifyModule.store.load();
    assert.ok(storedAfterLoginInit.codeVerifier, 'codeVerifier should be persisted');
    assert.ok(storedAfterLoginInit.oauthState, 'oauthState should be persisted');
    const stateParts = storedAfterLoginInit.oauthState.split('.');
    assert.strictEqual(Buffer.from(stateParts[1], 'base64url').toString('utf8'), 'http://192.168.1.50:6800');
    const expectedChallenge = generateCodeChallenge(storedAfterLoginInit.codeVerifier);
    assert.ok(redirectLocation.includes(`code_challenge=${expectedChallenge}`), 'URL should contain matching S256 challenge');
    console.log('[PASS] PKCE login redirect, GitHub Pages callback URI, & origin-encoded state verified');

    // 6. Mock Spotify Token Exchange & User Profile for Callback
    spotifyModule.setFetchImpl(async (url, options) => {
      if (url.includes('/api/token')) {
        const bodyParams = new URLSearchParams(options.body);
        if (bodyParams.get('grant_type') === 'authorization_code') {
          assert.strictEqual(bodyParams.get('code'), 'mock_auth_code_abc');
          assert.strictEqual(bodyParams.get('code_verifier'), storedAfterLoginInit.codeVerifier);
          return {
            ok: true,
            status: 200,
            json: async () => ({
              access_token: 'mock_access_token_initial',
              refresh_token: 'mock_refresh_token_initial',
              expires_in: 3600,
              scope: 'user-read-playback-state',
            }),
          };
        }
        if (bodyParams.get('grant_type') === 'refresh_token') {
          assert.strictEqual(bodyParams.get('refresh_token'), 'mock_refresh_token_initial');
          return {
            ok: true,
            status: 200,
            json: async () => ({
              access_token: 'mock_access_token_refreshed',
              refresh_token: 'mock_refresh_token_rotated',
              expires_in: 3600,
            }),
          };
        }
      }
      if (url.includes('/v1/me')) {
        return {
          ok: true,
          status: 200,
          json: async () => ({
            id: 'retro_mac_user',
            display_name: 'Macintosh SE/30',
            product: 'premium',
          }),
        };
      }
      throw new Error(`Unexpected URL in mock fetch: ${url}`);
    });

    const callbackRes = await httpRequest(
      'GET',
      `/api/spotify/callback?code=mock_auth_code_abc&state=${storedAfterLoginInit.oauthState}`
    );
    assert.strictEqual(callbackRes.statusCode, 302);
    assert.strictEqual(callbackRes.headers.location, '/');

    const readyStatus = JSON.parse((await httpRequest('GET', '/api/spotify/status')).body);
    assert.strictEqual(readyStatus.ok, true);
    assert.strictEqual(readyStatus.state, 'READY');
    assert.strictEqual(readyStatus.message, 'Spotify service is running OK');
    assert.strictEqual(readyStatus.user, 'Macintosh SE/30');
    console.log('[PASS] PKCE Callback exchanged code for tokens -> State READY ("Spotify service is running OK")');

    // 7. Test Automatic Token Refresh when token is near expiry
    spotifyModule.store.save({ expiresAt: Date.now() - 1000 }); // Force expired timestamp
    const refreshedToken = await spotifyModule.getValidAccessToken();
    assert.strictEqual(refreshedToken, 'mock_access_token_refreshed');
    const afterRefreshStore = spotifyModule.store.load();
    assert.strictEqual(afterRefreshStore.refreshToken, 'mock_refresh_token_rotated');
    console.log('[PASS] Automatic token refresh & rotation verified');

    // 8. Test Error State / Sad Mac trigger on network or OAuth error
    spotifyModule.setFetchImpl(async () => {
      const err = new Error('fetch failed');
      err.cause = { code: 'ENOTFOUND' };
      throw err;
    });
    const validation = await spotifyModule.validateConnection();
    assert.strictEqual(validation.ok, false);
    const errorStatus = JSON.parse((await httpRequest('GET', '/api/spotify/status')).body);
    assert.strictEqual(errorStatus.error, true);
    assert.strictEqual(errorStatus.state, 'ERROR');
    assert.strictEqual(errorStatus.errorCode, 'NET_ERROR');
    console.log('[PASS] Network/Spotify failure sets ERROR state & errorCode for Sad Mac UI');

    // 9. Clean up
    spotifyModule.setFetchImpl(null);
    spotifyModule.store.clear();
    console.log('--- ALL MILESTONE 2 TESTS PASSED ---');
  } finally {
    spotifyModule.setFetchImpl(null);
    server.close();
  }
}

runTests().catch((err) => {
  console.error('Milestone 2 test failed:', err);
  process.exit(1);
});
