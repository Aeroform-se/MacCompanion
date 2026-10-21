const { SPOTIFY_API_BASE } = require('./pkce');

/**
 * Performs an authenticated request to the Spotify Web API.
 * Handles 204 No Content and non-JSON responses gracefully.
 */
async function spotifyFetch(endpoint, { method = 'GET', accessToken, body, query, fetchImpl = fetch } = {}) {
  let url = endpoint.startsWith('http') ? endpoint : `${SPOTIFY_API_BASE}${endpoint}`;
  if (query) {
    const params = new URLSearchParams();
    for (const [k, v] of Object.entries(query)) {
      if (v !== undefined && v !== null && v !== '') {
        params.set(k, String(v));
      }
    }
    const qs = params.toString();
    if (qs) url += (url.includes('?') ? '&' : '?') + qs;
  }

  const headers = {
    Authorization: `Bearer ${accessToken}`,
  };
  let payload;
  if (body !== undefined && body !== null) {
    headers['Content-Type'] = 'application/json';
    payload = JSON.stringify(body);
  }

  const fetchOpts = {
    method,
    headers,
    body: payload,
  };
  if (fetchImpl === fetch && typeof AbortSignal !== 'undefined' && AbortSignal.timeout) {
    fetchOpts.signal = AbortSignal.timeout(8000);
  }

  const res = await fetchImpl(url, fetchOpts);

  if (res.status === 204) {
    return null;
  }

  const text = await res.text();
  let data = null;
  if (text) {
    try {
      data = JSON.parse(text);
    } catch {
      data = { raw: text };
    }
  }

  if (!res.ok) {
    const errMsg = (data && data.error && data.error.message) || (data && data.raw) || `HTTP ${res.status}`;
    const err = new Error(errMsg);
    err.status = res.status;
    err.details = data;
    throw err;
  }

  return data;
}

/**
 * Formats milliseconds into M:SS (e.g. 215000 -> "3:35") for direct rendering
 * in the 68k Macintosh timeline UI without client-side division/modulo overhead.
 */
function formatTimeMmSs(ms) {
  const totalSec = Math.max(0, Math.floor((ms || 0) / 1000));
  const min = Math.floor(totalSec / 60);
  const sec = totalSec % 60;
  return `${min}:${sec < 10 ? '0' : ''}${sec}`;
}

/**
 * Extracts and normalizes "Now Playing" state into a flat object + pipe-delimited fields.
 *
 * Pipe format:
 *   STATUS|is_playing(1/0)|track_id|track_name|artist_name|album_name|progress_ms|duration_ms|progress_mmss|duration_mmss|volume_pct|shuffle_mode(off/on/smart)|repeat_state(off/context/track)|is_liked(1/0)|device_name|artwork_url|artist_id|album_id|context_uri
 */
function normalizeNowPlaying(raw, { isLiked = false, smartShuffleOverride = null } = {}) {
  if (!raw || !raw.item) {
    return {
      active: false,
      isPlaying: false,
      trackId: '',
      trackName: 'Nothing Playing',
      artistName: '',
      artistId: '',
      albumName: '',
      albumId: '',
      progressMs: 0,
      durationMs: 0,
      progressFormatted: '0:00',
      durationFormatted: '0:00',
      volumePercent: raw && raw.device && typeof raw.device.volume_percent === 'number' ? raw.device.volume_percent : 100,
      shuffleState: Boolean(raw && raw.shuffle_state),
      smartShuffle: Boolean((raw && raw.smart_shuffle) || smartShuffleOverride),
      shuffleMode: ((raw && raw.smart_shuffle) || smartShuffleOverride)
        ? 'smart'
        : (raw && raw.shuffle_state ? 'on' : 'off'),
      repeatState: (raw && raw.repeat_state) || 'off',
      isLiked: false,
      deviceName: (raw && raw.device && raw.device.name) || 'No Active Device',
      deviceId: (raw && raw.device && raw.device.id) || '',
      artworkUrl: '',
      uri: '',
      contextUri: (raw && raw.context && raw.context.uri) || '',
    };
  }

  const item = raw.item;
  const artists = Array.isArray(item.artists) ? item.artists.map((a) => a.name).join(', ') : '';
  const primaryArtistId = (Array.isArray(item.artists) && item.artists[0] && item.artists[0].id) || '';
  const album = item.album ? item.album.name : '';
  const albumId = (item.album && item.album.id) || '';
  const images = (item.album && Array.isArray(item.album.images)) ? item.album.images : [];
  const preferredImage = images.find((img) => img.width && img.width <= 300) || images[0] || null;

  const progressMs = raw.progress_ms || 0;
  const durationMs = item.duration_ms || 0;
  const smartShuffle = Boolean(raw.smart_shuffle || smartShuffleOverride);
  const shuffleState = Boolean(raw.shuffle_state || smartShuffle);

  return {
    active: true,
    isPlaying: Boolean(raw.is_playing),
    trackId: item.id || '',
    trackName: item.name || 'Unknown Track',
    artistName: artists || 'Unknown Artist',
    artistId: primaryArtistId,
    albumName: album || '',
    albumId,
    progressMs,
    durationMs,
    progressFormatted: formatTimeMmSs(progressMs),
    durationFormatted: formatTimeMmSs(durationMs),
    volumePercent: raw.device && typeof raw.device.volume_percent === 'number' ? raw.device.volume_percent : 100,
    shuffleState,
    smartShuffle,
    shuffleMode: smartShuffle ? 'smart' : (shuffleState ? 'on' : 'off'),
    repeatState: raw.repeat_state || 'off',
    isLiked: Boolean(isLiked),
    deviceName: (raw.device && raw.device.name) || 'Spotify',
    deviceId: (raw.device && raw.device.id) || '',
    artworkUrl: preferredImage ? preferredImage.url : '',
    uri: item.uri || '',
    contextUri: (raw.context && raw.context.uri) || '',
  };
}

function nowPlayingToPipeRow(np) {
  return [
    np.active ? 'PLAYING' : 'IDLE',
    np.isPlaying ? '1' : '0',
    np.trackId,
    np.trackName,
    np.artistName,
    np.albumName,
    np.progressMs,
    np.durationMs,
    np.progressFormatted,
    np.durationFormatted,
    np.volumePercent,
    np.shuffleMode,
    np.repeatState,
    np.isLiked ? '1' : '0',
    np.deviceName,
    np.artworkUrl,
    np.artistId,
    np.albumId,
    np.contextUri,
  ];
}

module.exports = {
  spotifyFetch,
  formatTimeMmSs,
  normalizeNowPlaying,
  nowPlayingToPipeRow,
};
