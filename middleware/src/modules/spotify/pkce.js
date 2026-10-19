const crypto = require('crypto');

const SPOTIFY_AUTH_URL = 'https://accounts.spotify.com/authorize';
const SPOTIFY_TOKEN_URL = 'https://accounts.spotify.com/api/token';
const SPOTIFY_API_BASE = 'https://api.spotify.com/v1';

// Scopes required for full Spotify Connect remote control, library, playlists, and playback state
const SPOTIFY_SCOPES = [
  'user-read-playback-state',
  'user-modify-playback-state',
  'user-read-currently-playing',
  'user-read-recently-played',
  'user-top-read',
  'user-library-read',
  'user-library-modify',
  'playlist-read-private',
  'playlist-read-collaborative',
  'playlist-modify-public',
  'playlist-modify-private',
  'user-read-private',
].join(' ');

/**
 * Generates a high-entropy PKCE code_verifier (43-128 chars, URL-safe).
 */
function generateCodeVerifier(length = 64) {
  return crypto.randomBytes(length).toString('base64url').slice(0, length);
}

/**
 * Computes the SHA-256 base64url-encoded PKCE code_challenge from a code_verifier.
 */
function generateCodeChallenge(codeVerifier) {
  return crypto.createHash('sha256').update(codeVerifier).digest('base64url');
}

/**
 * Generates a random state string to protect against CSRF.
 */
function generateState() {
  return crypto.randomBytes(16).toString('hex');
}

/**
 * Builds the full Spotify OAuth 2.0 PKCE authorization URL.
 */
function buildAuthorizeUrl({ clientId, redirectUri, codeChallenge, state }) {
  const params = new URLSearchParams({
    client_id: clientId,
    response_type: 'code',
    redirect_uri: redirectUri,
    code_challenge_method: 'S256',
    code_challenge: codeChallenge,
    state,
    scope: SPOTIFY_SCOPES,
  });
  return `${SPOTIFY_AUTH_URL}?${params.toString()}`;
}

/**
 * Exchanges an authorization code for access_token and refresh_token using PKCE.
 */
async function exchangeCodeForTokens({ clientId, code, redirectUri, codeVerifier, fetchImpl = fetch }) {
  const body = new URLSearchParams({
    client_id: clientId,
    grant_type: 'authorization_code',
    code,
    redirect_uri: redirectUri,
    code_verifier: codeVerifier,
  });

  const response = await fetchImpl(SPOTIFY_TOKEN_URL, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
    },
    body: body.toString(),
  });

  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    const errMsg = data.error_description || data.error || `HTTP ${response.status}`;
    const err = new Error(`Token exchange failed: ${errMsg}`);
    err.status = response.status;
    err.details = data;
    throw err;
  }

  return {
    accessToken: data.access_token,
    refreshToken: data.refresh_token,
    expiresIn: data.expires_in || 3600,
    expiresAt: Date.now() + (data.expires_in || 3600) * 1000,
    scope: data.scope,
  };
}

/**
 * Refreshes an expired or expiring access_token using the stored refresh_token.
 * Note: Spotify PKCE may return a new refresh_token; if not, keep the existing one.
 */
async function refreshAccessToken({ clientId, refreshToken, fetchImpl = fetch }) {
  const body = new URLSearchParams({
    client_id: clientId,
    grant_type: 'refresh_token',
    refresh_token: refreshToken,
  });

  const response = await fetchImpl(SPOTIFY_TOKEN_URL, {
    method: 'POST',
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
    },
    body: body.toString(),
  });

  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    const errMsg = data.error_description || data.error || `HTTP ${response.status}`;
    const err = new Error(`Token refresh failed: ${errMsg}`);
    err.status = response.status;
    err.details = data;
    throw err;
  }

  return {
    accessToken: data.access_token,
    refreshToken: data.refresh_token || refreshToken,
    expiresIn: data.expires_in || 3600,
    expiresAt: Date.now() + (data.expires_in || 3600) * 1000,
    scope: data.scope,
  };
}

/**
 * Fetches the current Spotify user's profile (/v1/me) to validate the access token and connectivity.
 */
async function fetchUserProfile({ accessToken, fetchImpl = fetch }) {
  const response = await fetchImpl(`${SPOTIFY_API_BASE}/me`, {
    headers: {
      Authorization: `Bearer ${accessToken}`,
    },
  });

  const data = await response.json().catch(() => ({}));
  if (!response.ok) {
    const errMsg = (data.error && data.error.message) || `HTTP ${response.status}`;
    const err = new Error(`Spotify profile check failed: ${errMsg}`);
    err.status = response.status;
    throw err;
  }

  return {
    id: data.id,
    displayName: data.display_name || data.id,
    product: data.product || 'unknown',
    country: data.country || null,
  };
}

module.exports = {
  SPOTIFY_AUTH_URL,
  SPOTIFY_TOKEN_URL,
  SPOTIFY_API_BASE,
  SPOTIFY_SCOPES,
  generateCodeVerifier,
  generateCodeChallenge,
  generateState,
  buildAuthorizeUrl,
  exchangeCodeForTokens,
  refreshAccessToken,
  fetchUserProfile,
};
