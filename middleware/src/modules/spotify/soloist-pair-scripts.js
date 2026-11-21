/**
 * Generates OS-native, zero-dependency one-time pairing scripts for Spotify Soloist.
 *
 * When mDNS/Zeroconf cannot cross Docker bridge / VM boundaries, users can run the
 * appropriate script on their laptop (macOS Apple Silicon, Windows, or Linux), pair
 * once via their Spotify app, and automatically (or manually) upload the resulting
 * `.device_id + settings/ + cache/` auth bundle to MacCompanion.
 */

function sanitizeApiKey(rawKey) {
  const key = String(rawKey || '').trim();
  if (!key) return '';
  if (!/^[A-Za-z0-9._-]+$/.test(key)) {
    throw new Error('SOLOIST_API_KEY contains unsupported characters');
  }
  return key;
}

function sanitizeDeviceName(rawName) {
  const cleaned = String(rawName || 'Macintosh 68k')
    .replace(/["'`$\\;\r\n<>|&]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
    .slice(0, 64);
  return cleaned || 'Macintosh 68k';
}

function sanitizeOrigin(rawOrigin) {
  const trimmed = String(rawOrigin || 'http://127.0.0.1:6800').trim().replace(/\/+$/, '');
  const u = new URL(trimmed);
  if (u.protocol !== 'http:' && u.protocol !== 'https:') {
    throw new Error('Invalid server origin URL');
  }
  return u.origin;
}

function buildUnixPairScript({ osTarget, origin, deviceName, apiKey }) {
  const isMac = osTarget === 'mac';
  const uploadPath = `${origin}/api/spotify/soloist-upload-auth`;
  const keyExpr = apiKey
    ? `SOLOIST_KEY='${apiKey}'`
    : 'printf "Enter your Spotify Soloist API Key (spak_...): " && read -r SOLOIST_KEY';

  const archResolveBlock = isMac
    ? `ARCH=$(uname -m)
if [ "$ARCH" != "arm64" ]; then
  echo "ERROR: Spotify only publishes macOS Soloist builds for Apple Silicon (arm64); detected $ARCH."
  echo "Please run this helper on an Apple Silicon Mac, Windows x64 PC, or Linux host instead."
  exit 1
fi
TARBALL="soloist_release_osx_arm64.tar.gz"`
    : `ARCH=$(uname -m)
case "$ARCH" in
  x86_64|amd64) TARBALL="soloist_release_x86_64.tar.gz" ;;
  aarch64|arm64) TARBALL="soloist_release_arm64.tar.gz" ;;
  armv7*|armhf) TARBALL="soloist_release_arm32.tar.gz" ;;
  *) echo "Unsupported Linux architecture: $ARCH"; exit 1 ;;
esac`;

  const fallbackDest = isMac ? '$HOME/Desktop/spotimac-soloist-auth.tar.gz' : './spotimac-soloist-auth.tar.gz';

  return `#!/usr/bin/env bash
set -euo pipefail

DEVICE_NAME="${deviceName}"
UPLOAD_URL="${uploadPath}"
FALLBACK_FILE="${fallbackDest}"
${keyExpr}

if [ -z "\${SOLOIST_KEY:-}" ]; then
  echo "ERROR: Spotify Soloist API key is required."
  exit 1
fi

${archResolveBlock}

WORK=$(mktemp -d)
cleanup() { rm -rf "$WORK"; }
trap cleanup EXIT

echo ""
echo "=== MacCompanion / SpotiMac One-Time Soloist Pairing ==="
echo "Downloading $TARBALL from Spotify CDN..."
curl -fsSL "https://soloist-builds.spotifycdn.com/$TARBALL" -o "$WORK/soloist.tgz"
tar -xzf "$WORK/soloist.tgz" -C "$WORK"
chmod +x "$WORK/soloist"

mkdir -p "$WORK/data" "$WORK/cache"
echo ""
echo "Waiting for Spotify Connect pairing as \\"$DEVICE_NAME\\"..."
echo "Open Spotify Desktop (recommended) or Spotify Mobile on this Wi-Fi/LAN and select \\"$DEVICE_NAME\\" in Speakers."
echo ""

"$WORK/soloist" -n "$DEVICE_NAME" -D "$WORK/data" -C "$WORK/cache" -k "$SOLOIST_KEY" -p

if [ ! -f "$WORK/data/.device_id" ] || [ ! -d "$WORK/data/settings" ] || [ ! -d "$WORK/data/cache" ]; then
  echo "ERROR: Pairing did not produce complete Soloist session files."
  exit 1
fi

echo ""
echo "Packaging paired credentials..."
COPYFILE_DISABLE=1 tar -czf "$WORK/auth.tar.gz" \\
  --exclude="crashpad" --exclude=".lock" --exclude="ws.*" --exclude="soloist.pid" \\
  -C "$WORK/data" .device_id settings cache

echo "Uploading auth bundle to MacCompanion ($UPLOAD_URL)..."
if curl -fsS -X POST "$UPLOAD_URL" -H "Content-Type: application/octet-stream" --data-binary "@$WORK/auth.tar.gz" >/dev/null; then
  echo ""
  echo "SUCCESS! Soloist credentials uploaded to MacCompanion."
  echo "Return to the MacCompanion web UI — Step 3 will switch to Paired automatically."
else
  cp "$WORK/auth.tar.gz" "$FALLBACK_FILE"
  echo ""
  echo "Could not reach MacCompanion automatically."
  echo "Saved auth bundle to: $FALLBACK_FILE"
  echo "Upload that file using 'Upload Auth File' in Step 3 of the MacCompanion web UI."
fi
`;
}

function buildWindowsPairScript({ origin, deviceName, apiKey }) {
  const uploadPath = `${origin}/api/spotify/soloist-upload-auth`;
  const keyExpr = apiKey
    ? `$SoloistKey = '${apiKey}'`
    : `$SoloistKey = Read-Host 'Enter your Spotify Soloist API Key (spak_...)'`;

  return `$ErrorActionPreference = 'Stop'
$DeviceName = '${deviceName.replace(/'/g, "''")}'
$UploadUrl = '${uploadPath}'
$FallbackZip = Join-Path ([Environment]::GetFolderPath('Desktop')) 'spotimac-soloist-auth.zip'
${keyExpr}

if ([string]::IsNullOrWhiteSpace($SoloistKey)) {
  Write-Error 'Spotify Soloist API key is required.'
  exit 1
}

$Work = Join-Path $env:TEMP ("maccompanion-soloist-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $Work | Out-Null

try {
  Write-Host ''
  Write-Host '=== MacCompanion / SpotiMac One-Time Soloist Pairing ==='
  Write-Host 'Downloading soloist_release_windows_x86_64.zip from Spotify CDN...'
  $ZipFile = Join-Path $Work 'soloist.zip'
  Invoke-WebRequest -Uri 'https://soloist-builds.spotifycdn.com/soloist_release_windows_x86_64.zip' -OutFile $ZipFile
  Expand-Archive -Path $ZipFile -DestinationPath $Work -Force

  $DataDir = Join-Path $Work 'data'
  $CacheDir = Join-Path $Work 'cache'
  New-Item -ItemType Directory -Force -Path $DataDir, $CacheDir | Out-Null

  Write-Host ''
  Write-Host "Waiting for Spotify Connect pairing as ""$DeviceName""..."
  Write-Host "Open Spotify Desktop (recommended) or Spotify Mobile on this LAN and select ""$DeviceName"" in Speakers."
  Write-Host ''

  & (Join-Path $Work 'soloist.exe') -n $DeviceName -D $DataDir -C $CacheDir -k $SoloistKey -p
  if ($LASTEXITCODE -ne 0) {
    throw "Soloist pairing exited with code $LASTEXITCODE"
  }

  $Stage = Join-Path $Work 'bundle'
  New-Item -ItemType Directory -Force -Path $Stage | Out-Null
  Copy-Item (Join-Path $DataDir '.device_id') -Destination $Stage -Force
  Copy-Item (Join-Path $DataDir 'settings') -Destination $Stage -Recurse -Force
  Copy-Item (Join-Path $DataDir 'cache') -Destination $Stage -Recurse -Force

  $AuthZip = Join-Path $Work 'spotimac-soloist-auth.zip'
  Compress-Archive -Path (Join-Path $Stage '*') -DestinationPath $AuthZip -Force

  Write-Host "Uploading auth bundle to MacCompanion ($UploadUrl)..."
  try {
    $Bytes = [System.IO.File]::ReadAllBytes($AuthZip)
    Invoke-RestMethod -Uri $UploadUrl -Method Post -ContentType 'application/octet-stream' -Body $Bytes | Out-Null
    Write-Host ''
    Write-Host 'SUCCESS! Soloist credentials uploaded to MacCompanion.'
    Write-Host 'Return to the MacCompanion web UI — Step 3 will switch to Paired automatically.'
  } catch {
    Copy-Item $AuthZip -Destination $FallbackZip -Force
    Write-Host ''
    Write-Host 'Could not reach MacCompanion automatically.'
    Write-Host "Saved auth bundle to: $FallbackZip"
    Write-Host "Upload that file using 'Upload Auth File' in Step 3 of the MacCompanion web UI."
  }
} finally {
  Remove-Item -Path $Work -Recurse -Force -ErrorAction SilentlyContinue
}
`;
}

function buildPairScript({ os = 'mac', origin, deviceName, apiKey } = {}) {
  const safeOs = String(os || 'mac').toLowerCase().trim();
  const safeOrigin = sanitizeOrigin(origin);
  const safeName = sanitizeDeviceName(deviceName);
  const safeKey = sanitizeApiKey(apiKey);

  if (safeOs === 'win' || safeOs === 'windows') {
    return {
      os: 'win',
      filename: 'MacCompanion-Soloist-Pair.ps1',
      content: buildWindowsPairScript({
        origin: safeOrigin,
        deviceName: safeName,
        apiKey: safeKey,
      }),
    };
  }

  if (safeOs === 'linux') {
    return {
      os: 'linux',
      filename: 'MacCompanion-Soloist-Pair.sh',
      content: buildUnixPairScript({
        osTarget: 'linux',
        origin: safeOrigin,
        deviceName: safeName,
        apiKey: safeKey,
      }),
    };
  }

  return {
    os: 'mac',
    filename: 'MacCompanion-Soloist-Pair.command',
    content: buildUnixPairScript({
      osTarget: 'mac',
      origin: safeOrigin,
      deviceName: safeName,
      apiKey: safeKey,
    }),
  };
}

module.exports = {
  buildPairScript,
  sanitizeApiKey,
  sanitizeDeviceName,
  sanitizeOrigin,
};
