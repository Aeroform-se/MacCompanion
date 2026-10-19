const express = require('express');
const { createModuleStore } = require('../../core/storage');
const {
  generateCodeVerifier,
  generateCodeChallenge,
  generateState,
  buildAuthorizeUrl,
  exchangeCodeForTokens,
  refreshAccessToken,
  fetchUserProfile,
} = require('./pkce');

const router = express.Router();

const DEFAULT_REDIRECT_URI = 'https://aeroform-se.github.io/spotify68k/callback.html';
const TOKEN_EXPIRY_BUFFER_MS = 2 * 60 * 1000; // Refresh 2 minutes before expiry

// Persistent store mounted in ./data/spotify.json
const store = createModuleStore('spotify', {
  clientId: null,
  redirectUri: null,
  accessToken: null,
  refreshToken: null,
  expiresAt: null,
  codeVerifier: null,
  oauthState: null,
  userProfile: null,
  lastError: null,
  lastErrorCode: null,
});

// Allow injecting custom fetch for unit tests
let customFetch = null;
function setFetchImpl(fn) {
  customFetch = fn;
}
function getFetchImpl() {
  return customFetch || fetch;
}

/**
 * Resolves the active Spotify Client ID and Redirect URI.
 * Environment variables take priority if non-empty, with fallback to persistent web UI config.
 */
function getEffectiveConfig() {
  const data = store.load();
  const envClientId = process.env.SPOTIFY_CLIENT_ID ? process.env.SPOTIFY_CLIENT_ID.trim() : '';
  const envRedirectUri = process.env.SPOTIFY_REDIRECT_URI ? process.env.SPOTIFY_REDIRECT_URI.trim() : '';

  const clientId = envClientId || data.clientId || '';
  const redirectUri = envRedirectUri || data.redirectUri || DEFAULT_REDIRECT_URI;

  return {
    clientId,
    redirectUri,
    source: envClientId ? 'env' : (data.clientId ? 'store' : 'none'),
  };
}

/**
 * Ensures a valid, non-expired access token is available.
 * Automatically refreshes if expired or expiring soon.
 */
async function getValidAccessToken() {
  const data = store.load();
  const { clientId } = getEffectiveConfig();

  if (!clientId) {
    throw new Error('Missing SPOTIFY_CLIENT_ID');
  }
  if (!data.refreshToken) {
    throw new Error('Not authenticated with Spotify');
  }

  const now = Date.now();
  const isExpired = !data.expiresAt || now >= (data.expiresAt - TOKEN_EXPIRY_BUFFER_MS);

  if (!isExpired && data.accessToken) {
    return data.accessToken;
  }

  try {
    console.log('[Spotify] Refreshing access token...');
    const refreshed = await refreshAccessToken({
      clientId,
      refreshToken: data.refreshToken,
      fetchImpl: getFetchImpl(),
    });

    store.save({
      accessToken: refreshed.accessToken,
      refreshToken: refreshed.refreshToken,
      expiresAt: refreshed.expiresAt,
      lastError: null,
      lastErrorCode: null,
    });

    return refreshed.accessToken;
  } catch (err) {
    console.error('[Spotify] Token refresh failed:', err.message);
    store.save({
      lastError: err.message,
      lastErrorCode: err.code || (err.status ? `HTTP_${err.status}` : 'REFRESH_ERR'),
    });
    throw err;
  }
}

/**
 * Validates the current token and checks internet/Spotify API reachability.
 */
async function validateConnection() {
  try {
    const token = await getValidAccessToken();
    const profile = await fetchUserProfile({
      accessToken: token,
      fetchImpl: getFetchImpl(),
    });
    store.save({
      userProfile: profile,
      lastError: null,
      lastErrorCode: null,
    });
    return { ok: true, profile };
  } catch (err) {
    const isNetworkError = err.cause?.code === 'ENOTFOUND' ||
      err.cause?.code === 'ECONNREFUSED' ||
      err.cause?.code === 'ETIMEDOUT' ||
      err.message.includes('fetch failed');
    const errorCode = isNetworkError ? 'NET_ERROR' : (err.status ? `HTTP_${err.status}` : 'AUTH_ERROR');
    store.save({
      lastError: isNetworkError ? 'No internet access or Spotify unreachable' : err.message,
      lastErrorCode: errorCode,
    });
    return { ok: false, error: err.message, errorCode };
  }
}

/**
 * Returns the structured status for the Web UI and ModuleRegistry.
 */
function getStatus() {
  const data = store.load();
  const config = getEffectiveConfig();
  const hasClientId = Boolean(config.clientId);
  const isAuthenticated = Boolean(data.accessToken && data.refreshToken);

  if (data.lastError) {
    return {
      id: 'spotify',
      name: 'Spotify Connect',
      ok: false,
      error: true,
      errorCode: data.lastErrorCode || 'SPOTIFY_ERR',
      state: 'ERROR',
      message: data.lastError,
      config: {
        clientId: config.clientId,
        redirectUri: config.redirectUri,
        source: config.source,
      },
      loginUrl: '/api/spotify/login',
    };
  }

  if (isAuthenticated) {
    return {
      id: 'spotify',
      name: 'Spotify Connect',
      ok: true,
      error: false,
      state: 'READY',
      message: 'Spotify service is running OK',
      user: data.userProfile ? data.userProfile.displayName || data.userProfile.id : null,
      expiresAt: data.expiresAt,
      config: {
        clientId: config.clientId,
        redirectUri: config.redirectUri,
        source: config.source,
      },
      updatedAt: data.updatedAt || null,
    };
  }

  if (!hasClientId) {
    return {
      id: 'spotify',
      name: 'Spotify Connect',
      ok: false,
      error: false,
      state: 'AWAITING_CONFIG',
      message: 'Enter your Spotify Client ID below or via SPOTIFY_CLIENT_ID env var',
      config: {
        clientId: '',
        redirectUri: config.redirectUri,
        source: config.source,
      },
      loginUrl: '/api/spotify/login',
    };
  }

  return {
    id: 'spotify',
    name: 'Spotify Connect',
    ok: false,
    error: false,
    state: 'AWAITING_LOGIN',
    message: 'Ready to connect your Spotify account',
    config: {
      clientId: config.clientId,
      redirectUri: config.redirectUri,
      source: config.source,
    },
    loginUrl: '/api/spotify/login',
  };
}

// 1. JSON status endpoint for Web UI polling
router.get('/status', async (req, res) => {
  const data = store.load();
  if (req.query.verify === '1' && data.accessToken && data.refreshToken) {
    await validateConnection();
  }
  res.json(getStatus());
});

// 2. Pipe-delimited status endpoint for 68k Macintosh client
router.get('/ping', (req, res) => {
  const status = getStatus();
  res.type('text/plain').send(`SPOTIFY|${status.ok ? '1' : '0'}|${status.state}|${status.message}\n`);
});

// 3. Save Client ID and Redirect URI from the Web UI
router.post('/config', (req, res) => {
  const { clientId, redirectUri } = req.body || {};
  const trimmedId = (clientId || '').trim();
  const trimmedUri = (redirectUri || DEFAULT_REDIRECT_URI).trim();

  if (!trimmedId) {
    return res.status(400).json({ error: 'Client ID is required' });
  }

  store.save({
    clientId: trimmedId,
    redirectUri: trimmedUri || DEFAULT_REDIRECT_URI,
    lastError: null,
    lastErrorCode: null,
  });

  res.json({
    ok: true,
    status: getStatus(),
  });
});

// 4. Clear error state from Web UI so user can retry cleanly
router.post('/clear-error', (req, res) => {
  store.save({
    lastError: null,
    lastErrorCode: null,
  });
  res.json({ ok: true, status: getStatus() });
});

// 5. Initiate PKCE OAuth 2.0 Login Flow
// Encodes the requesting browser's middleware origin into `state` so the static GitHub Pages
// callback.html automatically redirects straight back to `http://<middleware-ip>:6800/api/spotify/callback`
router.get('/login', (req, res) => {
  const { clientId, redirectUri } = getEffectiveConfig();

  if (!clientId) {
    store.save({
      lastError: 'Cannot start login: SPOTIFY_CLIENT_ID is not configured',
      lastErrorCode: 'MISSING_CONFIG',
    });
    return res.redirect('/?error=missing_client_id');
  }

  const codeVerifier = generateCodeVerifier();
  const codeChallenge = generateCodeChallenge(codeVerifier);
  const csrfToken = generateState();

  // Determine the origin the user used to access the middleware UI (e.g., http://192.168.1.50:6800)
  const origin = req.query.origin || `${req.protocol}://${req.get('host')}`;
  const encodedOrigin = Buffer.from(origin, 'utf8').toString('base64url');
  const state = `${csrfToken}.${encodedOrigin}`;

  store.save({
    codeVerifier,
    oauthState: state,
    lastError: null,
    lastErrorCode: null,
  });

  const authUrl = buildAuthorizeUrl({
    clientId,
    redirectUri,
    codeChallenge,
    state,
  });

  if (req.query.json === '1') {
    return res.json({ authUrl, state, redirectUri });
  }

  res.redirect(authUrl);
});

/**
 * Shared handler for completing the PKCE callback.
 */
async function handleCallbackCode({ code, state, error }) {
  if (error) {
    store.save({
      lastError: `Spotify authorization denied: ${error}`,
      lastErrorCode: 'OAUTH_DENIED',
    });
    const err = new Error(`Spotify authorization denied: ${error}`);
    err.status = 400;
    throw err;
  }

  if (!code) {
    store.save({
      lastError: 'Missing authorization code in callback',
      lastErrorCode: 'MISSING_CODE',
    });
    const err = new Error('Missing authorization code');
    err.status = 400;
    throw err;
  }

  const data = store.load();
  const { clientId, redirectUri } = getEffectiveConfig();

  if (state && data.oauthState && state !== data.oauthState) {
    store.save({
      lastError: 'OAuth state mismatch (CSRF protection)',
      lastErrorCode: 'STATE_MISMATCH',
    });
    const err = new Error('OAuth state mismatch');
    err.status = 400;
    throw err;
  }

  if (!data.codeVerifier) {
    store.save({
      lastError: 'Missing PKCE code_verifier. Please start login again.',
      lastErrorCode: 'MISSING_VERIFIER',
    });
    const err = new Error('Missing PKCE code_verifier');
    err.status = 400;
    throw err;
  }

  try {
    const tokens = await exchangeCodeForTokens({
      clientId,
      code,
      redirectUri,
      codeVerifier: data.codeVerifier,
      fetchImpl: getFetchImpl(),
    });

    let userProfile = null;
    try {
      userProfile = await fetchUserProfile({
        accessToken: tokens.accessToken,
        fetchImpl: getFetchImpl(),
      });
    } catch (profileErr) {
      console.warn('[Spotify] Connected, but profile fetch warning:', profileErr.message);
    }

    store.save({
      accessToken: tokens.accessToken,
      refreshToken: tokens.refreshToken,
      expiresAt: tokens.expiresAt,
      codeVerifier: null,
      oauthState: null,
      userProfile,
      lastError: null,
      lastErrorCode: null,
    });

    return { ok: true, userProfile };
  } catch (err) {
    const isNetworkError = err.cause?.code === 'ENOTFOUND' ||
      err.cause?.code === 'ECONNREFUSED' ||
      err.message.includes('fetch failed');
    store.save({
      lastError: isNetworkError ? 'Network error reaching Spotify servers' : err.message,
      lastErrorCode: isNetworkError ? 'NET_ERROR' : 'TOKEN_ERR',
    });
    throw err;
  }
}

// 6. OAuth PKCE Redirect Callback (GET /api/spotify/callback)
router.get('/callback', async (req, res) => {
  try {
    await handleCallbackCode({
      code: req.query.code,
      state: req.query.state,
      error: req.query.error,
    });
    res.redirect('/');
  } catch (err) {
    res.redirect(`/?error=${encodeURIComponent(err.message)}`);
  }
});

// 7. Manual Callback URL / Code submission fallback
router.post('/callback-manual', async (req, res) => {
  try {
    const { callbackInput } = req.body || {};
    if (!callbackInput) {
      return res.status(400).json({ error: 'Please provide the callback URL or authorization code' });
    }

    let code = callbackInput.trim();
    let state = null;
    let error = null;

    if (code.includes('code=') || code.includes('error=') || code.startsWith('http')) {
      const urlObj = new URL(code.startsWith('http') ? code : `http://localhost/?${code}`);
      code = urlObj.searchParams.get('code');
      state = urlObj.searchParams.get('state');
      error = urlObj.searchParams.get('error');
    }

    const result = await handleCallbackCode({ code, state, error });
    res.json({ ok: true, result, status: getStatus() });
  } catch (err) {
    res.status(err.status || 500).json({
      ok: false,
      error: err.message,
      status: getStatus(),
    });
  }
});

// 8. Force Token Refresh / Connection Validation endpoint
router.post('/refresh', async (req, res) => {
  try {
    await getValidAccessToken();
    const validation = await validateConnection();
    res.json({ ok: validation.ok, status: getStatus() });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message, status: getStatus() });
  }
});

// 9. Logout / Disconnect Spotify session
router.post('/logout', (req, res) => {
  const data = store.load();
  const clearConfig = Boolean(req.body && req.body.clearConfig);
  store.save({
    clientId: clearConfig ? null : data.clientId,
    redirectUri: clearConfig ? null : data.redirectUri,
    accessToken: null,
    refreshToken: null,
    expiresAt: null,
    codeVerifier: null,
    oauthState: null,
    userProfile: null,
    lastError: null,
    lastErrorCode: null,
  });
  res.json({ ok: true, status: getStatus() });
});

let healthTimer = null;
function startBackgroundHealthCheck(intervalMs = 10 * 60 * 1000) {
  if (healthTimer) clearInterval(healthTimer);
  healthTimer = setInterval(async () => {
    const data = store.load();
    if (data.refreshToken) {
      await validateConnection();
    }
  }, intervalMs);
  if (healthTimer.unref) healthTimer.unref();
}

startBackgroundHealthCheck();

module.exports = {
  id: 'spotify',
  name: 'Spotify Connect',
  basePath: '/api/spotify',
  DEFAULT_REDIRECT_URI,
  router,
  store,
  getStatus,
  getEffectiveConfig,
  getValidAccessToken,
  validateConnection,
  setFetchImpl,
};
