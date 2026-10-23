const express = require('express');
const { createModuleStore } = require('../../core/storage');
const { formatPipeRow, formatPipeTable, sendPipeResponse } = require('../../core/format68k');
const {
  generateCodeVerifier,
  generateCodeChallenge,
  generateState,
  buildAuthorizeUrl,
  exchangeCodeForTokens,
  refreshAccessToken,
  fetchUserProfile,
} = require('./pkce');
const { spotifyFetch, formatTimeMmSs, normalizeNowPlaying, nowPlayingToPipeRow } = require('./api');
const {
  DEFAULT_ART_SIZE,
  processArtworkBuffer,
  generatePlaceholderArtwork,
} = require('../../core/dither');
const { AudioStreamEngine } = require('./audio');

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
  smartShuffleActive: false,
  audioSampleRate: '22k',
  audioProfile: 'warm',
  audioDsp: null,
  artworkDither: 'adaptive',
  lastError: null,
  lastErrorCode: null,
});

const audioEngine = new AudioStreamEngine({
  store,
  getValidAccessToken: () => getValidAccessToken(),
  getFetchImpl: () => getFetchImpl(),
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
 * Helper to call Spotify Web API with automatic token retrieval.
 */
async function callSpotify(endpoint, options = {}) {
  const accessToken = await getValidAccessToken();
  return spotifyFetch(endpoint, {
    ...options,
    accessToken,
    fetchImpl: getFetchImpl(),
  });
}

/**
 * Resilient wrapper for `/me/player/*` transport commands.
 * When switching between the Spotify Desktop app and the "Macintosh 68k" speaker,
 * Spotify's Web API can briefly report no active device (HTTP 404).
 * This helper automatically resolves an available target device and retries once.
 */
async function callPlayerCommand(endpoint, options = {}) {
  try {
    return await callSpotify(endpoint, options);
  } catch (err) {
    if ((err.status === 404 || err.status === 502 || err.status === 504) && !(options.query && options.query.device_id)) {
      try {
        const devData = await callSpotify('/me/player/devices');
        const devices = (devData && Array.isArray(devData.devices)) ? devData.devices : [];
        const target =
          devices.find((d) => d.is_active && d.id) ||
          devices.find((d) => d.name && d.name.toLowerCase().includes('macintosh 68k') && d.id) ||
          devices.find((d) => d.id);
        if (target && target.id) {
          // If no device was active and this isn't `/me/player/play`, wake the device first
          if (!target.is_active && endpoint !== '/me/player/play') {
            await callSpotify('/me/player', {
              method: 'PUT',
              body: { device_ids: [target.id] },
            }).catch(() => {});
          }
          return await callSpotify(endpoint, {
            ...options,
            query: { ...(options.query || {}), device_id: target.id },
          });
        }
      } catch {}
    }
    throw err;
  }
}

/**
 * Normalizes a Spotify resource identifier into both a clean ID and a full URI.
 * Example: normalizeSpotifyUri('2A7GGXmTlXuH9LOvBXgOX4', 'track')
 *   -> { id: '2A7GGXmTlXuH9LOvBXgOX4', uri: 'spotify:track:2A7GGXmTlXuH9LOvBXgOX4' }
 */
function normalizeSpotifyUri(input, defaultType = 'track') {
  if (!input) return { id: '', uri: '', type: defaultType };
  const trimmed = String(input).trim();
  if (trimmed.startsWith('spotify:')) {
    const parts = trimmed.split(':');
    return {
      type: parts[1] || defaultType,
      id: parts[parts.length - 1],
      uri: trimmed,
    };
  }
  if (trimmed.includes('/')) {
    const clean = trimmed.split('?')[0].replace(/\/$/, '');
    const segs = clean.split('/');
    const id = segs.pop();
    const maybeType = segs.pop() || defaultType;
    return {
      type: maybeType,
      id,
      uri: `spotify:${maybeType}:${id}`,
    };
  }
  return {
    type: defaultType,
    id: trimmed,
    uri: `spotify:${defaultType}:${trimmed}`,
  };
}

let likedCache = { uri: null, liked: false, ts: 0 };
const artworkCache = new Map();

/**
 * Checks if a track URI is saved in the user's "Liked Songs" library using Spotify's `/me/library/contains`.
 */
async function checkIsTrackLiked(trackIdOrUri) {
  if (!trackIdOrUri) return false;
  try {
    const { uri } = normalizeSpotifyUri(trackIdOrUri, 'track');
    if (!customFetch && likedCache.uri === uri && (Date.now() - likedCache.ts) < 30000) {
      return likedCache.liked;
    }
    const res = await callSpotify('/me/library/contains', {
      query: { uris: uri },
    });
    const liked = Array.isArray(res) && Boolean(res[0]);
    likedCache = { uri, liked, ts: Date.now() };
    return liked;
  } catch {
    return false;
  }
}

  // Track the most recently loaded tracklist URIs (e.g. Liked Songs, Recently Played,
  // Top Tracks, Search Results) so playing a track without a context_uri still loads
  // the full upcoming tracklist into Spotify's player for Next/Previous & auto-advance.
  let lastTrackListUris = [];
  function recordTrackListRows(rows) {
    if (!Array.isArray(rows)) return;
    const uris = rows
      .filter((r) => Array.isArray(r) && (r[0] === 'TRACK' || r[0] === 'RECENT' || r[0] === 'TOP_TRACK') && typeof r[7] === 'string' && r[7].startsWith('spotify:track:'))
      .map((r) => r[7]);
    if (uris.length > 0) {
      lastTrackListUris = uris;
    }
  }

  // Short-lived optimistic state overrides (3s TTL) so /now-playing never flickers back
  // to stale Spotify cloud state immediately after Play/Pause/Seek/Shuffle/Repeat/Volume/Like.
  const OPTIMISTIC_TTL_MS = 3000;
  const optimisticState = {
    isPlaying: null,
    isPlayingTs: 0,
    progressMs: null,
    progressTs: 0,
    volumePct: null,
    volumeTs: 0,
    shuffleMode: null,
    shuffleTs: 0,
    repeatState: null,
    repeatTs: 0,
    deviceName: null,
    deviceNameTs: 0,
  };

  function registerMacFromRequest(req) {
    if (!req) return null;
    const clientName = req.query && req.query.client_name;
    const clientId = req.query && req.query.client_id;
    if (!clientName && !clientId) return null;
    const rawIp =
      (req.query && req.query.client_ip) ||
      req.ip ||
      (req.socket && req.socket.remoteAddress) ||
      '';
    return audioEngine.registerMacClient({
      id: clientId || rawIp || clientName,
      ip: rawIp,
      name: clientName || 'Macintosh 68k',
    });
  }

  function applyOptimisticOverrides(np) {
    if (!np) return np;
    // When Spotify's active speaker is our librespot receiver ("Macintosh 68k"),
    // map deviceName to the currently selected 68k Mac's speaker name.
    if (np.deviceName && np.deviceName.toLowerCase().includes('macintosh 68k')) {
      np.deviceName = audioEngine.getActiveMacSpeakerName();
    }
    if (customFetch) return np;
    const now = Date.now();
    if (optimisticState.isPlaying !== null && (now - optimisticState.isPlayingTs) < OPTIMISTIC_TTL_MS) {
      np.isPlaying = optimisticState.isPlaying;
    }
    if (optimisticState.progressMs !== null && (now - optimisticState.progressTs) < OPTIMISTIC_TTL_MS) {
      const elapsed = np.isPlaying ? Math.max(0, now - optimisticState.progressTs) : 0;
      np.progressMs = Math.min(np.durationMs || Number.MAX_SAFE_INTEGER, optimisticState.progressMs + elapsed);
      np.progressFormatted = formatTimeMmSs(np.progressMs);
    }
    if (optimisticState.volumePct !== null && (now - optimisticState.volumeTs) < OPTIMISTIC_TTL_MS) {
      np.volumePercent = optimisticState.volumePct;
    }
    if (optimisticState.shuffleMode !== null && (now - optimisticState.shuffleTs) < OPTIMISTIC_TTL_MS) {
      np.shuffleMode = optimisticState.shuffleMode;
      np.shuffleState = optimisticState.shuffleMode !== 'off';
    }
    if (optimisticState.repeatState !== null && (now - optimisticState.repeatTs) < OPTIMISTIC_TTL_MS) {
      np.repeatState = optimisticState.repeatState;
    }
    if (optimisticState.deviceName !== null && (now - optimisticState.deviceNameTs) < 5000) {
      np.deviceName = optimisticState.deviceName;
    }
    return np;
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
  const audioStatus = audioEngine.getStatus();

  if (data.lastError) {
    return {
      id: 'spotify',
      name: 'Spotify Connect',
      ok: false,
      error: true,
      errorCode: data.lastErrorCode || 'SPOTIFY_ERR',
      state: 'ERROR',
      message: data.lastError,
      audio: audioStatus,
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
      audio: audioStatus,
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
      audio: audioStatus,
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
    audio: audioStatus,
    config: {
      clientId: config.clientId,
      redirectUri: config.redirectUri,
      source: config.source,
    },
    loginUrl: '/api/spotify/login',
  };
}

function handleRouteError(req, res, err) {
  const status = err.status || 500;
  const pipeMsg = `${formatPipeRow(['ERR', status, err.message])}\n`;
  if (req.query.format === 'json') {
    return res.status(status).json({ ok: false, error: err.message, pipe: pipeMsg.trim() });
  }
  res.status(status);
  return sendPipeResponse(req, res, pipeMsg);
}

function formatTrackRow(prefix, t) {
  return [
    prefix,
    t.id || '',
    t.name || 'Unknown',
    Array.isArray(t.artists) ? t.artists.map((a) => a.name).join(', ') : '',
    t.album ? t.album.name : '',
    t.duration_ms || 0,
    formatTimeMmSs(t.duration_ms || 0),
    t.uri || (t.id ? `spotify:track:${t.id}` : ''),
  ];
}

// ============================================================================
// STATUS & AUTHENTICATION ROUTES (Milestones 1 & 2)
// ============================================================================

router.get('/status', async (req, res) => {
  const data = store.load();
  if (req.query.verify === '1' && data.accessToken && data.refreshToken) {
    await validateConnection();
  }
  res.json(getStatus());
});

router.get('/ping', (req, res) => {
  const status = getStatus();
  const pipe = `${formatPipeRow(['SPOTIFY', status.ok ? '1' : '0', status.state, status.message])}\n`;
  sendPipeResponse(req, res, pipe);
});

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

  res.json({ ok: true, status: getStatus() });
});

router.post('/clear-error', (req, res) => {
  store.save({ lastError: null, lastErrorCode: null });
  res.json({ ok: true, status: getStatus() });
});

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

    audioEngine.tokenRejected = false;
    audioEngine.ensureStarted({ forceRestart: true }).catch(() => {});

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

router.get('/callback', async (req, res) => {
  try {
    await handleCallbackCode({
      code: req.query.code,
      state: req.query.state,
      error: req.query.error,
    });
    if (audioEngine.hasBinaries && !audioEngine.isPaired()) {
      return res.redirect('/api/spotify/audio-login');
    }
    res.redirect('/');
  } catch (err) {
    res.redirect(`/?error=${encodeURIComponent(err.message)}`);
  }
});

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

router.post('/refresh', async (req, res) => {
  try {
    await getValidAccessToken();
    const validation = await validateConnection();
    res.json({ ok: validation.ok, status: getStatus() });
  } catch (err) {
    res.status(500).json({ ok: false, error: err.message, status: getStatus() });
  }
});

router.post('/logout', (req, res) => {
  const data = store.load();
  const clearConfig = Boolean(req.body && req.body.clearConfig);
  audioEngine.stopProcesses();
  audioEngine.clearCachedCredentials();
  store.save({
    clientId: clearConfig ? null : data.clientId,
    redirectUri: clearConfig ? null : data.redirectUri,
    accessToken: null,
    refreshToken: null,
    expiresAt: null,
    codeVerifier: null,
    oauthState: null,
    userProfile: null,
    librespotAccessToken: null,
    librespotRefreshToken: null,
    librespotExpiresAt: null,
    librespotCodeVerifier: null,
    librespotOauthState: null,
    lastError: null,
    lastErrorCode: null,
  });
  res.json({ ok: true, status: getStatus() });
});

router.get('/audio-login', (req, res) => {
  const authUrl = audioEngine.buildSpeakerAuthorizeUrl();
  res.redirect(authUrl);
});

router.get('/audio-callback', async (req, res) => {
  try {
    await audioEngine.handleSpeakerCallback({
      code: req.query.code,
      state: req.query.state,
      error: req.query.error,
    });
    res.redirect('/');
  } catch (err) {
    res.redirect(`/?error=${encodeURIComponent(err.message)}`);
  }
});

// ============================================================================
// 1. NOW PLAYING & TIMELINE STATUS / SEEKING
// ============================================================================

/**
 * GET /api/spotify/now-playing
 * Returns full playback status, timeline progress/duration (both ms & M:SS),
 * shuffle mode (off/on/smart), repeat mode (off/context/track), and liked status (1/0).
 *
 * Pipe format:
 *   PLAYING|is_playing(1/0)|track_id|track_name|artist_name|album_name|progress_ms|duration_ms|progress_mmss|duration_mmss|volume_pct|shuffle_mode(off/on/smart)|repeat_state(off/context/track)|is_liked(1/0)|device_name|artwork_url|artist_id|album_id|context_uri
 */
router.get('/now-playing', async (req, res) => {
  try {
    registerMacFromRequest(req);
    const raw = await callSpotify('/me/player');
    const data = store.load();
    let isLiked = false;
    if (raw && raw.item && raw.item.id) {
      isLiked = await checkIsTrackLiked(raw.item.id);
    }
    const np = applyOptimisticOverrides(normalizeNowPlaying(raw, {
      isLiked,
      smartShuffleOverride: Boolean(data.smartShuffleActive && raw && raw.shuffle_state),
    }));
    const pipe = `${formatPipeRow(nowPlayingToPipeRow(np))}\n`;
    if (req.query.format === 'json') {
      return res.json({ ok: true, data: np, pipe: pipe.trim() });
    }
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/timeline
 * Ultra-lightweight endpoint specifically for frequent 68k timeline polling:
 *   TIMELINE|is_playing(1/0)|progress_ms|duration_ms|progress_mmss|duration_mmss|percent(0-100)|track_id
 */
router.get('/timeline', async (req, res) => {
  try {
    const raw = await callSpotify('/me/player');
    const np = normalizeNowPlaying(raw);
    const pct = np.durationMs > 0 ? Math.min(100, Math.round((np.progressMs / np.durationMs) * 100)) : 0;
    const pipe = `${formatPipeRow([
      'TIMELINE',
      np.isPlaying ? '1' : '0',
      np.progressMs,
      np.durationMs,
      np.progressFormatted,
      np.durationFormatted,
      pct,
      np.trackId,
    ])}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * POST/GET /api/spotify/seek
 * Supports seeking by:
 *   - ?position_ms=60000 (exact milliseconds)
 *   - ?percent=50 (0-100% along the current track's timeline bar!)
 *   - ?delta_ms=15000 or ?delta_ms=-15000 (relative fast-forward / rewind)
 */
router.all('/seek', async (req, res) => {
  try {
    let positionMs = req.query.position_ms ?? (req.body && req.body.position_ms);
    const percent = req.query.percent ?? (req.body && req.body.percent);
    const deltaMs = req.query.delta_ms ?? (req.body && req.body.delta_ms);

    if (positionMs === undefined && (percent !== undefined || deltaMs !== undefined)) {
      const raw = await callSpotify('/me/player');
      const np = normalizeNowPlaying(raw);
      if (percent !== undefined) {
        const clampedPct = Math.max(0, Math.min(100, parseFloat(percent)));
        positionMs = Math.round((clampedPct / 100) * np.durationMs);
      } else if (deltaMs !== undefined) {
        positionMs = Math.max(0, Math.min(np.durationMs || Number.MAX_SAFE_INTEGER, np.progressMs + parseInt(deltaMs, 10)));
      }
    }

    const targetMs = Math.max(0, parseInt(positionMs || '0', 10));
    optimisticState.progressMs = targetMs;
    optimisticState.progressTs = Date.now();
    await callSpotify('/me/player/seek', {
      method: 'PUT',
      query: { position_ms: targetMs },
    });
    const pipe = `${formatPipeRow(['OK', 'SEEK', targetMs, formatTimeMmSs(targetMs)])}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, positionMs: targetMs, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

// ============================================================================
// 2. TRANSPORT, REPEAT MODES & SHUFFLE (INCLUDING AI / SMART SHUFFLE)
// ============================================================================

/**
 * POST/GET /api/spotify/play
 * Supports:
 *   - Resume playback (no params)
 *   - Play specific track URI/ID: ?uri=spotify:track:... (or ?track_id=...)
 *   - Play context (album/playlist/artist): ?context_uri=spotify:playlist:...
 *   - Play track inside a context at offset: ?context_uri=...&offset_uri=spotify:track:...
 */
router.all('/play', async (req, res) => {
  try {
    const uri = req.query.uri || req.query.track_id || (req.body && (req.body.uri || req.body.track_id));
    const contextUri = req.query.context_uri || (req.body && req.body.context_uri);
    const offsetUri = req.query.offset_uri || (req.body && req.body.offset_uri);
    const positionMs = req.query.position_ms || (req.body && req.body.position_ms);
    const deviceId = req.query.device_id || (req.body && req.body.device_id);

    let body;
    if (contextUri) {
      const normCtx = normalizeSpotifyUri(contextUri, 'playlist');
      body = { context_uri: normCtx.uri };
      if (offsetUri) {
        const normOff = normalizeSpotifyUri(offsetUri, 'track');
        body.offset = { uri: normOff.uri };
      }
      optimisticState.progressMs = 0;
      optimisticState.progressTs = Date.now();
    } else if (uri) {
      const normTrack = normalizeSpotifyUri(uri, 'track');
      if (!customFetch && lastTrackListUris.length > 1 && lastTrackListUris.includes(normTrack.uri)) {
        body = { uris: lastTrackListUris, offset: { uri: normTrack.uri } };
      } else {
        body = { uris: [normTrack.uri] };
      }
      optimisticState.progressMs = 0;
      optimisticState.progressTs = Date.now();
    }
    if (positionMs !== undefined && body) {
      body.position_ms = parseInt(positionMs, 10);
      optimisticState.progressMs = body.position_ms;
      optimisticState.progressTs = Date.now();
    }

    optimisticState.isPlaying = true;
    optimisticState.isPlayingTs = Date.now();
    await callPlayerCommand('/me/player/play', {
      method: 'PUT',
      query: deviceId ? { device_id: deviceId } : undefined,
      body,
    });
    const pipe = 'OK|PLAY\n';
    if (req.query.format === 'json') return res.json({ ok: true, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

router.all('/pause', async (req, res) => {
  try {
    optimisticState.isPlaying = false;
    optimisticState.isPlayingTs = Date.now();
    audioEngine.onPause();
    await callPlayerCommand('/me/player/pause', { method: 'PUT' });
    const pipe = 'OK|PAUSE\n';
    if (req.query.format === 'json') return res.json({ ok: true, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

router.all('/toggle-play', async (req, res) => {
  try {
    const raw = await callSpotify('/me/player');
    const isPlaying = Boolean(raw && raw.is_playing);
    if (isPlaying) {
      optimisticState.isPlaying = false;
      optimisticState.isPlayingTs = Date.now();
      audioEngine.onPause();
      await callPlayerCommand('/me/player/pause', { method: 'PUT' });
      return sendPipeResponse(req, res, 'OK|PAUSE\n');
    } else {
      optimisticState.isPlaying = true;
      optimisticState.isPlayingTs = Date.now();
      await callPlayerCommand('/me/player/play', { method: 'PUT' });
      return sendPipeResponse(req, res, 'OK|PLAY\n');
    }
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

router.all('/next', async (req, res) => {
  try {
    optimisticState.isPlaying = true;
    optimisticState.isPlayingTs = Date.now();
    optimisticState.progressMs = 0;
    optimisticState.progressTs = Date.now();
    await callPlayerCommand('/me/player/next', { method: 'POST' });
    if (!customFetch) {
      await new Promise((r) => setTimeout(r, 250));
    }
    const pipe = 'OK|NEXT\n';
    if (req.query.format === 'json') return res.json({ ok: true, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

router.all('/previous', async (req, res) => {
  try {
    optimisticState.isPlaying = true;
    optimisticState.isPlayingTs = Date.now();
    optimisticState.progressMs = 0;
    optimisticState.progressTs = Date.now();
    await callPlayerCommand('/me/player/previous', { method: 'POST' });
    if (!customFetch) {
      await new Promise((r) => setTimeout(r, 250));
    }
    const pipe = 'OK|PREVIOUS\n';
    if (req.query.format === 'json') return res.json({ ok: true, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

router.all('/volume', async (req, res) => {
  try {
    const delta = req.query.delta ?? (req.body && req.body.delta);
    let vol;
    if (delta !== undefined) {
      const raw = await callSpotify('/me/player');
      const cur = (raw && raw.device && typeof raw.device.volume_percent === 'number') ? raw.device.volume_percent : 50;
      vol = Math.max(0, Math.min(100, cur + parseInt(delta, 10)));
    } else {
      vol = Math.max(0, Math.min(100, parseInt(req.query.volume_percent ?? (req.body && req.body.volume_percent) ?? '50', 10)));
    }
    optimisticState.volumePct = vol;
    optimisticState.volumeTs = Date.now();
    await callPlayerCommand('/me/player/volume', {
      method: 'PUT',
      query: { volume_percent: vol },
    });
    const pipe = `OK|VOLUME|${vol}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, volume: vol, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * POST/GET /api/spotify/repeat
 * Supports all 3 Spotify repeat modes + automatic cycling when `?state=cycle` (or no state) is passed:
 *   - `off`     (Repeat Off)
 *   - `context` (Repeat All / Album / Playlist)
 *   - `track`   (Repeat 1 / Current Track)
 *   - `cycle`   (Cycles off -> context -> track -> off)
 */
router.all('/repeat', async (req, res) => {
  try {
    let state = (req.query.state || (req.body && req.body.state) || 'cycle').toLowerCase().trim();
    if (state === 'one' || state === '1') state = 'track';
    if (state === 'all') state = 'context';

    if (state === 'cycle') {
      const raw = await callSpotify('/me/player');
      const current = (raw && raw.repeat_state) || 'off';
      const order = ['off', 'context', 'track'];
      state = order[(order.indexOf(current) + 1) % order.length];
    }

    optimisticState.repeatState = state;
    optimisticState.repeatTs = Date.now();
    await callSpotify('/me/player/repeat', {
      method: 'PUT',
      query: { state },
    });
    const pipe = `OK|REPEAT|${state}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, state, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * Helper that injects AI / Smart Shuffle recommended tracks into the user's playback queue
 * based on the currently playing artist/track.
 */
async function injectSmartShuffleRecommendations() {
  try {
    const raw = await callSpotify('/me/player');
    if (!raw || !raw.item) return 0;
    const artistName = (Array.isArray(raw.item.artists) && raw.item.artists[0] && raw.item.artists[0].name) || '';
    const trackName = raw.item.name || '';
    const q = artistName || trackName;
    if (!q) return 0;

    // Search related tracks & radio mix tracks for the artist
    const searchData = await callSpotify('/search', {
      query: { q: `${q}`, type: 'track', limit: 10 },
    });
    const candidates = (searchData && searchData.tracks && Array.isArray(searchData.tracks.items))
      ? searchData.tracks.items.filter((t) => t.id !== raw.item.id)
      : [];

    // Pick up to 2 fresh tracks and add to queue
    const shuffled = candidates.sort(() => Math.random() - 0.5).slice(0, 2);
    let added = 0;
    for (const track of shuffled) {
      if (track.uri) {
        await callSpotify('/me/player/queue', {
          method: 'POST',
          query: { uri: track.uri },
        });
        added++;
      }
    }
    return added;
  } catch (err) {
    console.warn('[Spotify] Smart Shuffle queue injection note:', err.message);
    return 0;
  }
}

/**
 * POST/GET /api/spotify/shuffle
 * Supports all shuffle modes + automatic cycling (`off` -> `on` -> `smart` -> `off`):
 *   - `off` or `0` or `false`  -> Shuffle Off
 *   - `on` or `1` or `true`    -> Standard Shuffle On
 *   - `smart` or `ai`          -> Smart Shuffle (Shuffle On + AI/Smart Queue Enrichment)
 *   - `cycle` (default)        -> Cycles off -> on -> smart -> off
 */
router.all('/shuffle', async (req, res) => {
  try {
    let rawMode = req.query.mode ?? req.query.state ?? (req.body && (req.body.mode ?? req.body.state)) ?? 'cycle';
    rawMode = String(rawMode).toLowerCase().trim();

    const data = store.load();
    let targetMode;

    if (rawMode === 'cycle') {
      const rawPlayer = await callSpotify('/me/player');
      const curShuffle = Boolean(rawPlayer && rawPlayer.shuffle_state);
      const curSmart = Boolean((rawPlayer && rawPlayer.smart_shuffle) || (curShuffle && data.smartShuffleActive));
      if (!curShuffle) targetMode = 'on';
      else if (!curSmart) targetMode = 'smart';
      else targetMode = 'off';
    } else if (rawMode === 'smart' || rawMode === 'ai' || rawMode === '2') {
      targetMode = 'smart';
    } else if (rawMode === 'on' || rawMode === '1' || rawMode === 'true') {
      targetMode = 'on';
    } else {
      targetMode = 'off';
    }

    const boolShuffle = targetMode === 'on' || targetMode === 'smart';
    optimisticState.shuffleMode = targetMode;
    optimisticState.shuffleTs = Date.now();
    await callSpotify('/me/player/shuffle', {
      method: 'PUT',
      query: { state: boolShuffle },
    });

    store.save({ smartShuffleActive: targetMode === 'smart' });

    let injected = 0;
    if (targetMode === 'smart') {
      injected = await injectSmartShuffleRecommendations();
    }

    const pipe = `OK|SHUFFLE|${targetMode}|${injected}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, mode: targetMode, injected, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

// ============================================================================
// 3. SPOTIFY RADIO (SEED BY CURRENT TRACK, ARTIST, OR QUERY)
// ============================================================================

/**
 * POST/GET /api/spotify/radio
 * Starts Spotify Radio based on:
 *   - `?seed=...` (track name, artist name, track ID/URI, or artist ID/URI)
 *   - Or if omitted, automatically uses the currently playing track & artist!
 *
 * Strategy:
 *   1. Resolves the seed artist/track name.
 *   2. Searches Spotify for an official `<Seed> Radio` or `<Artist> Mix` playlist; if found and
 *      playable, starts playback or queues its tracks.
 *   3. Builds a rich radio queue of matching tracks and starts playback immediately.
 *
 * Pipe response:
 *   OK|RADIO|<radio_title>|<context_or_seed_uri>
 */
router.all('/radio', async (req, res) => {
  try {
    let seedInput = (req.query.seed || req.query.q || (req.body && (req.body.seed || req.body.q)) || '').trim();
    const deviceId = req.query.device_id || (req.body && req.body.device_id);
    let seedTitle = seedInput;
    let artistQuery = seedInput;

    if (!seedInput) {
      // Automatically seed from currently playing track/artist
      const rawPlayer = await callSpotify('/me/player');
      if (rawPlayer && rawPlayer.item) {
        const artist = (Array.isArray(rawPlayer.item.artists) && rawPlayer.item.artists[0] && rawPlayer.item.artists[0].name) || '';
        const track = rawPlayer.item.name || '';
        seedTitle = artist ? `${artist} - ${track}` : track;
        artistQuery = artist || track;
      }
    } else if (seedInput.startsWith('spotify:track:') || /^[A-Za-z0-9]{22}$/.test(seedInput)) {
      const { id } = normalizeSpotifyUri(seedInput, 'track');
      try {
        const trackObj = await callSpotify(`/tracks/${encodeURIComponent(id)}`);
        if (trackObj) {
          const artist = (Array.isArray(trackObj.artists) && trackObj.artists[0] && trackObj.artists[0].name) || '';
          seedTitle = artist ? `${artist} - ${trackObj.name}` : trackObj.name;
          artistQuery = artist || trackObj.name;
        }
      } catch {
        // Might be an artist ID
        try {
          const artistObj = await callSpotify(`/artists/${encodeURIComponent(id)}`);
          if (artistObj && artistObj.name) {
            seedTitle = artistObj.name;
            artistQuery = artistObj.name;
          }
        } catch {}
      }
    } else if (seedInput.startsWith('spotify:artist:')) {
      const { id } = normalizeSpotifyUri(seedInput, 'artist');
      const artistObj = await callSpotify(`/artists/${encodeURIComponent(id)}`);
      if (artistObj && artistObj.name) {
        seedTitle = artistObj.name;
        artistQuery = artistObj.name;
      }
    }

    if (!artistQuery) {
      const err = new Error('Nothing is currently playing to seed Radio. Provide ?seed=Artist+or+Track');
      err.status = 400;
      throw err;
    }

    // 1. Try searching for a dedicated "<Artist> Radio" playlist first
    const plSearch = await callSpotify('/search', {
      query: { q: `${artistQuery} Radio`, type: 'playlist', limit: 5 },
    });
    const playlists = (plSearch && plSearch.playlists && Array.isArray(plSearch.playlists.items))
      ? plSearch.playlists.items.filter(Boolean)
      : [];

    if (playlists.length > 0 && playlists[0].uri && req.query.mode !== 'tracks') {
      const chosen = playlists[0];
      await callSpotify('/me/player/play', {
        method: 'PUT',
        query: deviceId ? { device_id: deviceId } : undefined,
        body: { context_uri: chosen.uri },
      });
      const pipe = `${formatPipeRow(['OK', 'RADIO', chosen.name, chosen.uri])}\n`;
      if (req.query.format === 'json') return res.json({ ok: true, title: chosen.name, uri: chosen.uri, pipe: pipe.trim() });
      return sendPipeResponse(req, res, pipe);
    }

    // 2. Fallback: Build a dynamic radio track list from search and start playing them
    const trackSearch = await callSpotify('/search', {
      query: { q: artistQuery, type: 'track', limit: 10 },
    });
    const tracks = (trackSearch && trackSearch.tracks && Array.isArray(trackSearch.tracks.items))
      ? trackSearch.tracks.items.filter((t) => t && t.uri)
      : [];

    if (tracks.length === 0) {
      const err = new Error(`No radio tracks found for "${artistQuery}"`);
      err.status = 404;
      throw err;
    }

    // Shuffle the radio tracks so every radio session feels fresh
    const radioUris = tracks.map((t) => t.uri).sort(() => Math.random() - 0.5);
    await callSpotify('/me/player/play', {
      method: 'PUT',
      query: deviceId ? { device_id: deviceId } : undefined,
      body: { uris: radioUris },
    });

    const radioLabel = `${seedTitle} Radio`;
    const pipe = `${formatPipeRow(['OK', 'RADIO', radioLabel, radioUris[0]])}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, title: radioLabel, count: radioUris.length, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

// ============================================================================
// 4. LIKED / LOVED SONGS (LIST, SAVE/LIKE, REMOVE/UNLIKE, TOGGLE)
// ============================================================================

/**
 * GET /api/spotify/liked?limit=25&offset=0
 * Lists the user's "Liked Songs" (Saved Tracks).
 * Pipe format per line:
 *   TRACK|id|name|artist|album|duration_ms|duration_mmss|uri
 */
router.get('/liked', async (req, res) => {
  try {
    const limit = Math.min(50, parseInt(req.query.limit || '25', 10));
    const offset = parseInt(req.query.offset || '0', 10);
    const data = await callSpotify('/me/tracks', { query: { limit, offset } });
    const items = (data && Array.isArray(data.items)) ? data.items : [];
    const rows = items
      .map((entry) => entry && (entry.track || entry.item))
      .filter(Boolean)
      .map((t) => formatTrackRow('TRACK', t));
    recordTrackListRows(rows);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, total: data ? data.total : rows.length, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * POST/PUT/GET /api/spotify/like
 * Saves a track to the user's "Liked Songs" library.
 * If `?id=...` or `?uri=...` is omitted, automatically likes the currently playing track!
 * Pipe format:
 *   OK|LIKE|<track_id>|<track_name>
 */
router.all('/like', async (req, res) => {
  try {
    let target = req.query.uri || req.query.id || (req.body && (req.body.uri || req.body.id));
    let trackName = '';

    if (!target) {
      const raw = await callSpotify('/me/player');
      if (!raw || !raw.item || !raw.item.id) {
        const err = new Error('No track currently playing to like');
        err.status = 400;
        throw err;
      }
      target = raw.item.uri || raw.item.id;
      trackName = raw.item.name || '';
    }

    const { id, uri } = normalizeSpotifyUri(target, 'track');
    await callSpotify('/me/library', {
      method: 'PUT',
      query: { uris: uri },
    });
    likedCache = { uri, liked: true, ts: Date.now() };

    const pipe = `${formatPipeRow(['OK', 'LIKE', id, trackName || uri])}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, id, uri, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * POST/DELETE/GET /api/spotify/unlike
 * Removes a track from the user's "Liked Songs" library.
 * If `?id=...` or `?uri=...` is omitted, automatically unlikes the currently playing track!
 */
router.all('/unlike', async (req, res) => {
  try {
    let target = req.query.uri || req.query.id || (req.body && (req.body.uri || req.body.id));
    let trackName = '';

    if (!target) {
      const raw = await callSpotify('/me/player');
      if (!raw || !raw.item || !raw.item.id) {
        const err = new Error('No track currently playing to unlike');
        err.status = 400;
        throw err;
      }
      target = raw.item.uri || raw.item.id;
      trackName = raw.item.name || '';
    }

    const { id, uri } = normalizeSpotifyUri(target, 'track');
    await callSpotify('/me/library', {
      method: 'DELETE',
      query: { uris: uri },
    });
    likedCache = { uri, liked: false, ts: Date.now() };

    const pipe = `${formatPipeRow(['OK', 'UNLIKE', id, trackName || uri])}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, id, uri, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * POST/GET /api/spotify/toggle-like
 * Toggles Liked/Loved state for a track (or currently playing track if omitted).
 * Returns:
 *   OK|LIKED|1|<track_id>  or  OK|LIKED|0|<track_id>
 */
router.all('/toggle-like', async (req, res) => {
  try {
    let target = req.query.uri || req.query.id || (req.body && (req.body.uri || req.body.id));
    if (!target) {
      const raw = await callSpotify('/me/player');
      if (!raw || !raw.item || !raw.item.id) {
        const err = new Error('No track currently playing');
        err.status = 400;
        throw err;
      }
      target = raw.item.uri || raw.item.id;
    }

    const { id, uri } = normalizeSpotifyUri(target, 'track');
    const currentlyLiked = await checkIsTrackLiked(uri);

    await callSpotify('/me/library', {
      method: currentlyLiked ? 'DELETE' : 'PUT',
      query: { uris: uri },
    });

    const nextLiked = !currentlyLiked;
    likedCache = { uri, liked: nextLiked, ts: Date.now() };
    const pipe = `${formatPipeRow(['OK', 'LIKED', nextLiked ? '1' : '0', id])}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, liked: nextLiked, id, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

// ============================================================================
// 5. DEVICES, QUEUE, PLAYLISTS, ALBUMS, ARTISTS, RECENTLY PLAYED, TOP & SEARCH
// ============================================================================

router.get('/devices', async (req, res) => {
  try {
    registerMacFromRequest(req);
    const data = await callSpotify('/me/player/devices');
    const devices = (data && Array.isArray(data.devices)) ? data.devices : [];
    const macClients = audioEngine.getMacClientList();

    const rows = [];
    const expandedDevices = [];
    let macsInserted = false;

    for (const d of devices) {
      const isLibrespot = d.name && d.name.toLowerCase().includes('macintosh 68k');
      if (isLibrespot && macClients.length > 0) {
        const vol = typeof d.volume_percent === 'number' ? d.volume_percent : 100;
        for (const mac of macClients) {
          const isMacActive = Boolean(d.is_active && mac.isActive);
          rows.push([
            'DEVICE',
            mac.deviceId,
            mac.name,
            'Computer',
            isMacActive ? '1' : '0',
            vol,
          ]);
          expandedDevices.push({
            id: mac.deviceId,
            name: mac.name,
            type: 'Computer',
            is_active: isMacActive,
            volume_percent: vol,
          });
        }
        macsInserted = true;
      } else {
        rows.push([
          'DEVICE',
          d.id || '',
          d.name || 'Unknown Device',
          d.type || 'Speaker',
          d.is_active ? '1' : '0',
          typeof d.volume_percent === 'number' ? d.volume_percent : 100,
        ]);
        expandedDevices.push(d);
      }
    }

    // If librespot is running locally and Macs are registered, ensure they appear in the speaker list
    if (!macsInserted && macClients.length > 0 && audioEngine.isPaired()) {
      for (const mac of macClients) {
        rows.unshift([
          'DEVICE',
          mac.deviceId,
          mac.name,
          'Computer',
          '0',
          100,
        ]);
        expandedDevices.unshift({
          id: mac.deviceId,
          name: mac.name,
          type: 'Computer',
          is_active: false,
          volume_percent: 100,
        });
      }
    }

    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, devices: expandedDevices, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

router.all('/transfer', async (req, res) => {
  try {
    registerMacFromRequest(req);
    let deviceId = req.query.device_id || (req.body && req.body.device_id);
    const target = (req.query.target || (req.body && req.body.target) || '').toLowerCase();
    let deviceName = '';

    const isMacTarget =
      target === '68k' ||
      target === 'macintosh' ||
      deviceId === '68k' ||
      (typeof deviceId === 'string' && deviceId.startsWith('mac68k:')) ||
      Boolean(deviceId && audioEngine.setActiveMacClient(deviceId));

    if (isMacTarget) {
      let chosenMac = null;
      if (deviceId && deviceId !== '68k') {
        chosenMac = audioEngine.setActiveMacClient(deviceId);
      }
      if (!chosenMac) {
        chosenMac = audioEngine.getActiveMacClient();
      }
      await audioEngine.ensureStarted();
      const data = await callSpotify('/me/player/devices');
      const devices = (data && Array.isArray(data.devices)) ? data.devices : [];
      const found = devices.find((d) => d.name && d.name.toLowerCase().includes('macintosh 68k'));
      if (!found) {
        const err = new Error('"Macintosh 68k" speaker not visible in Spotify yet. Wait 2 seconds and try again.');
        err.status = 404;
        throw err;
      }
      deviceId = found.id;
      deviceName = chosenMac ? chosenMac.name : found.name;
      optimisticState.deviceName = deviceName;
      optimisticState.deviceNameTs = Date.now();
    } else if (target === 'desktop' || target === 'other' || deviceId === 'desktop') {
      const data = await callSpotify('/me/player/devices');
      const devices = (data && Array.isArray(data.devices)) ? data.devices : [];
      const found = devices.find((d) => d.name && !d.name.toLowerCase().includes('macintosh 68k'));
      if (!found) {
        const err = new Error('No other Spotify device (Desktop/Phone) is currently open.');
        err.status = 404;
        throw err;
      }
      deviceId = found.id;
      deviceName = found.name;
      optimisticState.deviceName = deviceName;
      optimisticState.deviceNameTs = Date.now();
    } else if (deviceId) {
      try {
        const data = await callSpotify('/me/player/devices');
        const devices = (data && Array.isArray(data.devices)) ? data.devices : [];
        const found = devices.find((d) => d.id === deviceId);
        if (found && found.name) {
          if (found.name.toLowerCase().includes('macintosh 68k')) {
            deviceName = audioEngine.getActiveMacSpeakerName();
          } else {
            deviceName = found.name;
          }
          optimisticState.deviceName = deviceName;
          optimisticState.deviceNameTs = Date.now();
        }
      } catch {}
    }

    if (!deviceId) {
      const err = new Error('Missing device_id');
      err.status = 400;
      throw err;
    }

    await callSpotify('/me/player', {
      method: 'PUT',
      body: { device_ids: [deviceId], play: true },
    });
    const pipe = formatPipeRow(['OK', 'TRANSFER', deviceId, deviceName || deviceId]) + '\n';
    if (req.query.format === 'json') return res.json({ ok: true, deviceId, deviceName, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

router.get('/playlists', async (req, res) => {
  try {
    const limit = Math.min(50, parseInt(req.query.limit || '20', 10));
    const offset = parseInt(req.query.offset || '0', 10);
    const data = await callSpotify('/me/playlists', { query: { limit, offset } });
    const items = (data && Array.isArray(data.items)) ? data.items : [];
    const rows = items.filter(Boolean).map((p) => {
      const total = (p.items && typeof p.items.total === 'number')
        ? p.items.total
        : (p.tracks && typeof p.tracks.total === 'number' ? p.tracks.total : 0);
      return [
        'PLAYLIST',
        p.id || '',
        p.name || 'Untitled',
        total,
        p.owner ? (p.owner.display_name || p.owner.id) : '',
        p.uri || '',
      ];
    });
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

router.get('/playlist/:id/tracks', async (req, res) => {
  try {
    const limit = Math.min(50, parseInt(req.query.limit || '25', 10));
    const offset = parseInt(req.query.offset || '0', 10);
    const { id: playlistId } = normalizeSpotifyUri(req.params.id, 'playlist');

    let items = [];
    try {
      const data = await callSpotify(`/playlists/${encodeURIComponent(playlistId)}/items`, {
        query: { limit, offset },
      });
      items = (data && Array.isArray(data.items)) ? data.items : [];
    } catch {
      const playlistObj = await callSpotify(`/playlists/${encodeURIComponent(playlistId)}`);
      const container = playlistObj && (playlistObj.items || playlistObj.tracks);
      items = (container && Array.isArray(container.items)) ? container.items.slice(offset, offset + limit) : [];
    }

    let rows = items
      .map((entry) => entry && (entry.item || entry.track))
      .filter(Boolean)
      .map((t) => formatTrackRow('TRACK', t));

    // Fallback for followed third-party playlists restricted by Spotify Dev Mode Web API
    if (rows.length === 0 && !customFetch) {
      try {
        const fetchFn = getFetchImpl();
        const embedRes = await fetchFn(`https://open.spotify.com/embed/playlist/${encodeURIComponent(playlistId)}`);
        if (embedRes && embedRes.ok) {
          const html = await embedRes.text();
          const m = html.match(/<script id="__NEXT_DATA__" type="application\/json">([^<]+)<\/script>/);
          if (m) {
            const nextData = JSON.parse(m[1]);
            const trackList =
              nextData &&
              nextData.props &&
              nextData.props.pageProps &&
              nextData.props.pageProps.state &&
              nextData.props.pageProps.state.data &&
              nextData.props.pageProps.state.data.entity &&
              nextData.props.pageProps.state.data.entity.trackList;
            if (Array.isArray(trackList) && trackList.length > 0) {
              rows = trackList.slice(offset, offset + limit).map((tr) => {
                const uri = tr.uri || '';
                const id = uri.includes(':') ? uri.split(':').pop() : uri;
                const durMs = tr.duration || 0;
                return [
                  'TRACK',
                  id,
                  tr.title || 'Unknown',
                  tr.subtitle || '',
                  '',
                  durMs,
                  formatTimeMmSs(durMs),
                  uri,
                ];
              });
            }
          }
        }
      } catch {}
    }

    recordTrackListRows(rows);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * POST /api/spotify/playlist/:id/add?uri=spotify:track:...
 * Adds a track to a user playlist (using Spotify's `/playlists/:id/items`).
 */
router.all('/playlist/:id/add', async (req, res) => {
  try {
    const { id: playlistId } = normalizeSpotifyUri(req.params.id, 'playlist');
    const rawTrack = req.query.uri || req.query.track_id || (req.body && (req.body.uri || req.body.track_id));
    if (!rawTrack) {
      const err = new Error('Missing track uri');
      err.status = 400;
      throw err;
    }
    const { uri: trackUri } = normalizeSpotifyUri(rawTrack, 'track');
    await callSpotify(`/playlists/${encodeURIComponent(playlistId)}/items`, {
      method: 'POST',
      body: { uris: [trackUri] },
    });
    const pipe = `OK|PLAYLIST_ADD|${playlistId}|${trackUri}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

router.get('/queue', async (req, res) => {
  try {
    const data = await callSpotify('/me/player/queue');
    const queue = (data && Array.isArray(data.queue)) ? data.queue.slice(0, 20) : [];
    const rows = queue.map((t) => formatTrackRow('QUEUE', t));
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, queue, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * POST/GET /api/spotify/queue/add?uri=spotify:track:...
 * Adds a track to the user's playback queue.
 */
router.all('/queue/add', async (req, res) => {
  try {
    const rawUri = req.query.uri || req.query.id || (req.body && (req.body.uri || req.body.id));
    if (!rawUri) {
      const err = new Error('Missing track uri or id to add to queue');
      err.status = 400;
      throw err;
    }
    const { uri } = normalizeSpotifyUri(rawUri, 'track');
    await callSpotify('/me/player/queue', {
      method: 'POST',
      query: { uri },
    });
    const pipe = `OK|QUEUE_ADD|${uri}\n`;
    if (req.query.format === 'json') return res.json({ ok: true, uri, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/recently-played?limit=20
 * Lists recently played tracks.
 */
router.get('/recently-played', async (req, res) => {
  try {
    const limit = Math.min(50, parseInt(req.query.limit || '20', 10));
    const data = await callSpotify('/me/player/recently-played', { query: { limit } });
    const items = (data && Array.isArray(data.items)) ? data.items : [];
    const rows = items
      .map((entry) => entry && entry.track)
      .filter(Boolean)
      .map((t) => formatTrackRow('RECENT', t));
    recordTrackListRows(rows);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/top?type=tracks|artists&time_range=short_term|medium_term|long_term&limit=20
 * Lists user's top tracks or top artists.
 */
router.get('/top', async (req, res) => {
  try {
    const type = req.query.type === 'artists' ? 'artists' : 'tracks';
    const timeRange = req.query.time_range || 'medium_term';
    const limit = Math.min(50, parseInt(req.query.limit || '20', 10));
    const data = await callSpotify(`/me/top/${type}`, {
      query: { time_range: timeRange, limit },
    });
    const items = (data && Array.isArray(data.items)) ? data.items : [];
    const rows = type === 'artists'
      ? items.map((a) => [
          'ARTIST',
          a.id || '',
          a.name || 'Unknown',
          Array.isArray(a.genres) ? a.genres.slice(0, 3).join(', ') : '',
          a.uri || '',
        ])
      : items.map((t) => formatTrackRow('TOP_TRACK', t));
    recordTrackListRows(rows);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/albums?limit=20&offset=0
 * Lists user's saved albums.
 * Pipe format:
 *   ALBUM|id|name|artist|total_tracks|release_date|uri
 */
router.get('/albums', async (req, res) => {
  try {
    const limit = Math.min(50, parseInt(req.query.limit || '20', 10));
    const offset = parseInt(req.query.offset || '0', 10);
    const data = await callSpotify('/me/albums', { query: { limit, offset } });
    const items = (data && Array.isArray(data.items)) ? data.items : [];
    const rows = items
      .map((entry) => entry && entry.album)
      .filter(Boolean)
      .map((a) => [
        'ALBUM',
        a.id || '',
        a.name || 'Untitled',
        Array.isArray(a.artists) ? a.artists.map((ar) => ar.name).join(', ') : '',
        a.total_tracks || 0,
        a.release_date || '',
        a.uri || '',
      ]);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/new-releases?limit=20
 * Lists newly released albums.
 */
router.get('/new-releases', async (req, res) => {
  try {
    const limit = Math.min(50, parseInt(req.query.limit || '20', 10));
    let items = [];
    try {
      const data = await callSpotify('/browse/new-releases', { query: { limit } });
      items = (data && data.albums && Array.isArray(data.albums.items)) ? data.albums.items : [];
    } catch {
      const data = await callSpotify('/search', { query: { q: 'tag:new', type: 'album', limit: Math.min(10, limit) } });
      items = (data && data.albums && Array.isArray(data.albums.items)) ? data.albums.items : [];
    }
    const rows = items.filter(Boolean).map((a) => [
      'ALBUM',
      a.id || '',
      a.name || 'Untitled',
      Array.isArray(a.artists) ? a.artists.map((ar) => ar.name).join(', ') : '',
      a.total_tracks || 0,
      a.release_date || '',
      a.uri || '',
    ]);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/album/:id/tracks?limit=50&offset=0
 * Lists tracks inside an album.
 */
router.get('/album/:id/tracks', async (req, res) => {
  try {
    const { id: albumId } = normalizeSpotifyUri(req.params.id, 'album');
    const limit = Math.min(50, parseInt(req.query.limit || '50', 10));
    const offset = parseInt(req.query.offset || '0', 10);
    const albumObj = await callSpotify(`/albums/${encodeURIComponent(albumId)}`);
    const data = await callSpotify(`/albums/${encodeURIComponent(albumId)}/tracks`, {
      query: { limit, offset },
    });
    const items = (data && Array.isArray(data.items)) ? data.items : [];
    const albumName = (albumObj && albumObj.name) || '';
    const rows = items.map((t) => [
      'TRACK',
      t.id || '',
      t.name || 'Unknown',
      Array.isArray(t.artists) ? t.artists.map((a) => a.name).join(', ') : '',
      albumName,
      t.duration_ms || 0,
      formatTimeMmSs(t.duration_ms || 0),
      t.uri || '',
    ]);
    recordTrackListRows(rows);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, album: albumObj, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/artist/:id/albums?limit=20
 * Lists an artist's albums & singles.
 */
router.get('/artist/:id/albums', async (req, res) => {
  try {
    const { id: artistId } = normalizeSpotifyUri(req.params.id, 'artist');
    const limit = Math.min(50, parseInt(req.query.limit || '20', 10));
    const data = await callSpotify(`/artists/${encodeURIComponent(artistId)}/albums`, {
      query: { limit, include_groups: 'album,single' },
    });
    const items = (data && Array.isArray(data.items)) ? data.items : [];
    const rows = items.map((a) => [
      'ALBUM',
      a.id || '',
      a.name || 'Untitled',
      Array.isArray(a.artists) ? a.artists.map((ar) => ar.name).join(', ') : '',
      a.total_tracks || 0,
      a.release_date || '',
      a.uri || '',
    ]);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/shows?limit=20 (Saved Podcasts / Shows)
 */
router.get('/shows', async (req, res) => {
  try {
    const limit = Math.min(50, parseInt(req.query.limit || '20', 10));
    const data = await callSpotify('/me/shows', { query: { limit } });
    const items = (data && Array.isArray(data.items)) ? data.items : [];
    const rows = items
      .map((entry) => entry && entry.show)
      .filter(Boolean)
      .map((s) => [
        'SHOW',
        s.id || '',
        s.name || 'Untitled',
        s.publisher || '',
        s.total_episodes || 0,
        s.uri || '',
      ]);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, items, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/search?q=oasis&type=track|album|artist|playlist&limit=15
 * Multi-type search across tracks, albums, artists, or playlists.
 */
router.get('/search', async (req, res) => {
  try {
    const q = (req.query.q || '').trim();
    if (!q) {
      const err = new Error('Missing search query parameter q');
      err.status = 400;
      throw err;
    }
    const rawType = (req.query.type || 'track').toLowerCase().trim();
    const isMulti = rawType === 'all' || rawType === 'multi' || rawType.includes(',');
    const apiType = isMulti ? 'track,album,playlist' : rawType;
    const limit = Math.max(1, Math.min(10, parseInt(req.query.limit || '10', 10) || 10));
    const data = await callSpotify('/search', {
      query: { q, type: apiType, limit },
    });

    let rows = [];
    if (isMulti && data) {
      const tracks = (data.tracks && Array.isArray(data.tracks.items)) ? data.tracks.items.filter(Boolean) : [];
      const albums = (data.albums && Array.isArray(data.albums.items)) ? data.albums.items.filter(Boolean) : [];
      const playlists = (data.playlists && Array.isArray(data.playlists.items)) ? data.playlists.items.filter(Boolean) : [];
      const maxLen = Math.max(tracks.length, albums.length, playlists.length);
      for (let i = 0; i < maxLen; i++) {
        if (i < tracks.length) {
          rows.push(formatTrackRow('TRACK', tracks[i]));
        }
        if (i < albums.length) {
          const a = albums[i];
          rows.push([
            'ALBUM',
            a.id || '',
            a.name || 'Untitled',
            Array.isArray(a.artists) ? a.artists.map((ar) => ar.name).join(', ') : '',
            a.total_tracks || 0,
            a.release_date || '',
            a.uri || '',
          ]);
        }
        if (i < playlists.length) {
          const p = playlists[i];
          rows.push([
            'PLAYLIST',
            p.id || '',
            p.name || 'Untitled',
            (p.items && p.items.total) || (p.tracks && p.tracks.total) || 0,
            p.owner ? (p.owner.display_name || p.owner.id) : '',
            p.uri || '',
          ]);
        }
      }
    } else if (rawType === 'album' && data && data.albums) {
      rows = (data.albums.items || []).filter(Boolean).map((a) => [
        'ALBUM',
        a.id || '',
        a.name || 'Untitled',
        Array.isArray(a.artists) ? a.artists.map((ar) => ar.name).join(', ') : '',
        a.total_tracks || 0,
        a.release_date || '',
        a.uri || '',
      ]);
    } else if (rawType === 'artist' && data && data.artists) {
      rows = (data.artists.items || []).filter(Boolean).map((a) => [
        'ARTIST',
        a.id || '',
        a.name || 'Unknown',
        Array.isArray(a.genres) ? a.genres.slice(0, 3).join(', ') : '',
        a.uri || '',
      ]);
    } else if (rawType === 'playlist' && data && data.playlists) {
      rows = (data.playlists.items || []).filter(Boolean).map((p) => [
        'PLAYLIST',
        p.id || '',
        p.name || 'Untitled',
        (p.items && p.items.total) || (p.tracks && p.tracks.total) || 0,
        p.owner ? (p.owner.display_name || p.owner.id) : '',
        p.uri || '',
      ]);
    } else {
      const tracks = (data && data.tracks && Array.isArray(data.tracks.items)) ? data.tracks.items : [];
      rows = tracks.filter(Boolean).map((t) => formatTrackRow('TRACK', t));
    }

    recordTrackListRows(rows);
    const pipe = formatPipeTable(rows);
    if (req.query.format === 'json') return res.json({ ok: true, data, pipe: pipe.trim() });
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

// ============================================================================
// 6. MILESTONE 4: 1-BIT MONOCHROME ALBUM ARTWORK ENGINE (200x200 DEFAULT)
// ============================================================================

/**
 * GET /api/spotify/artwork
 * Retrieves album artwork, scales to target dimensions (default 200x200),
 * and dithers to 1-bit monochrome using Bill Atkinson or Floyd-Steinberg dithering.
 *
 * Query Parameters:
 *   - `size`   : Target width & height in pixels (default: `200`)
 *   - `dither` : `atkinson` (default) or `floyd`
 *   - `format` :
 *       - `raw` (default): 8-byte header (width, height, rowBytes, 0) + packed QuickDraw BitMap bytes (5,208 bytes for 200x200)
 *       - `bits`: Pure packed QuickDraw 1-bit scanlines (5,200 bytes for 200x200)
 *       - `png` : 1-bit monochrome PNG image (for browser previewing)
 *       - `hex` : Pipe-delimited string `BITMAP|width|height|rowBytes|byteLen|<hex>`
 *   - `id` / `track_id` / `album_id` / `url`: Optional specific track/album/image URL.
 *     If omitted, automatically uses the currently playing track's album cover (or a 1-bit retro musical note placeholder if idle).
 */
const rawSourceJpegCache = new Map();

router.all('/artwork-dither', (req, res) => {
  const dither = String(req.query.dither || (req.body && req.body.dither) || store.load().artworkDither || 'adaptive').toLowerCase().trim();
  if (req.method === 'POST' || req.query.dither || (req.body && req.body.dither)) {
    store.save({ artworkDither: dither });
    artworkCache.clear();
  }
  const active = store.load().artworkDither || 'adaptive';
  const pipe = `OK|ARTWORK_DITHER|${active}\n`;
  if (req.query.format === 'json') return res.json({ ok: true, artworkDither: active, pipe: pipe.trim() });
  return sendPipeResponse(req, res, pipe);
});

router.get('/artwork', async (req, res) => {
  try {
    const size = parseInt(req.query.size || String(DEFAULT_ART_SIZE), 10);
    const dither = req.query.dither || store.load().artworkDither || 'adaptive';
    const format = req.query.format || 'raw';
    let imageUrl = (req.query.url || '').trim();

    const targetId = (req.query.id || req.query.track_id || req.query.album_id || '').trim();
    if (!imageUrl && targetId) {
      const { type, id } = normalizeSpotifyUri(targetId, req.query.album_id ? 'album' : 'track');
      if (type === 'album') {
        const albumObj = await callSpotify(`/albums/${encodeURIComponent(id)}`);
        const images = (albumObj && Array.isArray(albumObj.images)) ? albumObj.images : [];
        const chosen = images.find((img) => img.width && img.width <= 300) || images[0];
        if (chosen) imageUrl = chosen.url;
      } else {
        const trackObj = await callSpotify(`/tracks/${encodeURIComponent(id)}`);
        const images = (trackObj && trackObj.album && Array.isArray(trackObj.album.images)) ? trackObj.album.images : [];
        const chosen = images.find((img) => img.width && img.width <= 300) || images[0];
        if (chosen) imageUrl = chosen.url;
      }
    }

    if (!imageUrl) {
      const rawPlayer = await callSpotify('/me/player');
      const np = normalizeNowPlaying(rawPlayer);
      imageUrl = np.artworkUrl;
    }

    let result;
    const cacheKey = imageUrl ? `${imageUrl}|${size}|${dither}|${format}` : null;
    if (!customFetch && cacheKey && artworkCache.has(cacheKey)) {
      result = artworkCache.get(cacheKey);
    } else if (imageUrl) {
      let sourceBuf = !customFetch ? rawSourceJpegCache.get(imageUrl) : null;
      if (!sourceBuf) {
        const fetchFn = getFetchImpl();
        const imgRes = await fetchFn(imageUrl);
        if (!imgRes.ok) {
          throw new Error(`Failed to fetch album art JPEG: HTTP ${imgRes.status}`);
        }
        const arrayBuf = await imgRes.arrayBuffer();
        sourceBuf = Buffer.from(arrayBuf);
        if (!customFetch) {
          if (rawSourceJpegCache.size > 6) {
            const oldestUrl = rawSourceJpegCache.keys().next().value;
            rawSourceJpegCache.delete(oldestUrl);
          }
          rawSourceJpegCache.set(imageUrl, sourceBuf);
        }
      }
      result = await processArtworkBuffer(sourceBuf, {
        size,
        dither,
        format,
      });
      if (!customFetch && cacheKey) {
        if (artworkCache.size > 20) {
          const oldest = artworkCache.keys().next().value;
          artworkCache.delete(oldest);
        }
        artworkCache.set(cacheKey, result);
      }
    } else {
      result = await generatePlaceholderArtwork({ size, dither, format });
    }

    res.setHeader('Content-Type', result.contentType);
    res.setHeader('Content-Length', result.buffer.length);
    res.setHeader('X-Art-Width', String(result.width));
    res.setHeader('X-Art-Height', String(result.height));
    res.setHeader('X-Art-RowBytes', String(result.rowBytes));
    res.setHeader('Cache-Control', 'no-store');
    return res.end(result.buffer);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

// ============================================================================
// 7. LOCAL 68K AUDIO STREAMING ENGINE (LIBRESPOT -> FFMPEG -> 8-BIT PCM)
// ============================================================================

/**
 * GET / POST /api/spotify/audio-config
 * Returns or updates current local 68k audio streaming configuration, sound profile, & DSP settings.
 * Pipe format (GET):
 *   AUDIO|<state>|<sampleRateMode(22k/11k)>|<sampleRateHz(22255/11127)>|<tcpPort(6802)>|<deviceName>|<profile>
 */
router.all('/audio-config', async (req, res) => {
  try {
    const src = { ...(req.query || {}), ...(req.body || {}) };
    const hasUpdate =
      req.method === 'POST' ||
      req.method === 'PUT' ||
      src.rate !== undefined ||
      src.profile !== undefined ||
      src.dsp !== undefined ||
      src.ditherMethod !== undefined ||
      src.resampler !== undefined ||
      src.compressorEnabled !== undefined;

    if (!hasUpdate) {
      const st = audioEngine.getStatus();
      const pipe = `${formatPipeRow(['AUDIO', st.state, st.sampleRateMode, st.sampleRateHz, st.tcpPort, st.deviceName, st.profile])}\n`;
      if (req.query.format === 'json') {
        return res.json({ ok: true, audio: st, pipe: pipe.trim() });
      }
      return sendPipeResponse(req, res, pipe);
    }

    const patch = {};
    if (src.rate !== undefined) patch.rate = src.rate;
    if (src.profile !== undefined) patch.profile = src.profile;

    const dspKeys = [
      'resampler',
      'ditherMethod',
      'ditherScale',
      'compressorEnabled',
      'compThresholdDb',
      'compRatio',
      'compMakeupDb',
      'highpassHz',
      'lowpassHz',
      'eqPresenceDb',
      'pregainDb',
      'limiterEnabled',
    ];
    const dspPatch = (src.dsp && typeof src.dsp === 'object') ? { ...src.dsp } : {};
    for (const k of dspKeys) {
      if (src[k] !== undefined) {
        if (k === 'compressorEnabled' || k === 'limiterEnabled') {
          dspPatch[k] = src[k] === true || src[k] === 'true' || src[k] === '1' || src[k] === 1;
        } else if (k === 'resampler' || k === 'ditherMethod') {
          dspPatch[k] = String(src[k]);
        } else {
          dspPatch[k] = Number(src[k]);
        }
      }
    }
    if (Object.keys(dspPatch).length > 0) {
      patch.dsp = dspPatch;
    }

    const st = await audioEngine.updateAudioConfig(patch);
    const pipe = `${formatPipeRow(['OK', 'AUDIO_RATE', st.sampleRateMode, st.sampleRateHz, st.tcpPort, st.profile])}\n`;
    if (req.query.format === 'json') {
      return res.json({ ok: true, audio: st, pipe: pipe.trim() });
    }
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/stream.wav
 * Streams live 8-bit unsigned mono audio (at 22,255 Hz or 11,127 Hz) wrapped in a WAV header
 * so modern browsers can listen to the exact 68k audio stream.
 */
router.get('/stream.wav', async (req, res) => {
  try {
    await audioEngine.attachHttpClient(req, res, { format: 'wav' });
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

/**
 * GET /api/spotify/stream.pcm
 * Streams pure headerless unsigned 8-bit mono PCM bytes over HTTP/1.0 (alternative to raw TCP :6802).
 */
router.get('/stream.pcm', async (req, res) => {
  try {
    await audioEngine.attachHttpClient(req, res, { format: 'raw' });
  } catch (err) {
    return handleRouteError(req, res, err);
  }
});

let healthTimer = null;
function startBackgroundHealthCheck(intervalMs = 10 * 60 * 1000) {
  if (healthTimer) clearInterval(healthTimer);
  healthTimer = setInterval(async () => {
    const data = store.load();
    if (data.refreshToken) {
      await validateConnection();
      await audioEngine.ensureStarted();
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
  audioEngine,
  getStatus,
  getEffectiveConfig,
  getValidAccessToken,
  validateConnection,
  setFetchImpl,
};
