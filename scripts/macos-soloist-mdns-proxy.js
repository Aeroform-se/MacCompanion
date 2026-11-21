#!/usr/bin/env node
/**
 * macOS OrbStack / Docker Desktop Zeroconf mDNS Helper for Spotify Soloist
 *
 * Bridges Spotify Connect mDNS discovery (`_spotify-connect._tcp`) from macOS's
 * physical Wi-Fi/Ethernet interface into the `maccompanion` container's dynamic
 * Soloist Zeroconf HTTP port so you can test Spotify Soloist pairing locally on macOS.
 *
 * Usage:
 *   node scripts/macos-soloist-mdns-proxy.js
 */

const net = require('net');
const http = require('http');
const { spawn, execSync } = require('child_process');

const STATUS_URL = (process.env.MACCOMPANION_URL || 'http://127.0.0.1:6800').replace(/\/$/, '');
const CONTAINER_NAME = process.env.MACCOMPANION_CONTAINER || 'maccompanion';
const ORB_HOST = process.env.SOLOIST_TARGET_HOST || `${CONTAINER_NAME}.orb.local`;

let tcpServer = null;
let proxyPort = null;
let dnsSdProc = null;
let activeSignature = null;
let currentTargetHost = ORB_HOST;
let currentTargetPort = null;

function resolveContainerHostFallback() {
  if (process.env.SOLOIST_TARGET_HOST) return process.env.SOLOIST_TARGET_HOST;
  try {
    const ip = execSync(
      `docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' ${CONTAINER_NAME}`,
      { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }
    ).trim();
    if (ip) return ip;
  } catch {}
  return ORB_HOST;
}

function fetchJson(url) {
  return new Promise((resolve, reject) => {
    const req = http.get(url, { timeout: 2500 }, (res) => {
      let body = '';
      res.on('data', (chunk) => { body += chunk; });
      res.on('end', () => {
        if (res.statusCode !== 200) {
          return reject(new Error(`HTTP ${res.statusCode}`));
        }
        try {
          resolve(JSON.parse(body));
        } catch (err) {
          reject(err);
        }
      });
    });
    req.on('timeout', () => {
      req.destroy(new Error('timeout'));
    });
    req.on('error', reject);
  });
}

function ensureTcpProxy() {
  if (tcpServer && proxyPort) return Promise.resolve(proxyPort);
  return new Promise((resolve, reject) => {
    const server = net.createServer((clientSock) => {
      if (!currentTargetPort) {
        clientSock.destroy();
        return;
      }
      const upstream = net.connect({ host: currentTargetHost, port: currentTargetPort });
      clientSock.pipe(upstream);
      upstream.pipe(clientSock);
      upstream.on('error', () => {
        // Fallback once to direct container IP if `.orb.local` isn't resolvable
        const fallbackIp = resolveContainerHostFallback();
        if (fallbackIp && fallbackIp !== currentTargetHost) {
          currentTargetHost = fallbackIp;
        }
        clientSock.destroy();
      });
      clientSock.on('error', () => upstream.destroy());
    });
    server.on('error', reject);
    server.listen(Number(process.env.PROXY_PORT || 0), '0.0.0.0', () => {
      tcpServer = server;
      proxyPort = server.address().port;
      resolve(proxyPort);
    });
  });
}

function stopDnsSd() {
  if (dnsSdProc) {
    try { dnsSdProc.kill('SIGTERM'); } catch {}
    dnsSdProc = null;
  }
}

function startDnsSd({ instanceName, port, zcPath }) {
  stopDnsSd();
  const safePath = zcPath || '/zc';
  const args = [
    '-R',
    instanceName,
    '_spotify-connect._tcp',
    'local',
    String(port),
    `CPath=${safePath}`,
    'VERSION=1.0',
    'Stack=SP',
  ];
  dnsSdProc = spawn('dns-sd', args, { stdio: ['ignore', 'ignore', 'pipe'] });
  dnsSdProc.stderr.on('data', (d) => {
    const line = d.toString().trim();
    if (line) console.warn('[dns-sd]', line);
  });
  dnsSdProc.on('exit', (code) => {
    if (code !== null && code !== 0 && activeSignature) {
      console.warn(`[dns-sd] exited with code ${code}`);
    }
    dnsSdProc = null;
    activeSignature = null;
  });
}

async function testUpstreamReachable(port) {
  const candidates = [ORB_HOST, resolveContainerHostFallback()].filter(Boolean);
  for (const host of candidates) {
    const ok = await new Promise((resolve) => {
      const sock = net.connect({ host, port, timeout: 1000 });
      sock.once('connect', () => {
        sock.destroy();
        resolve(true);
      });
      sock.once('error', () => resolve(false));
      sock.once('timeout', () => {
        sock.destroy();
        resolve(false);
      });
    });
    if (ok) return host;
  }
  return ORB_HOST;
}

async function tick() {
  try {
    const status = await fetchJson(`${STATUS_URL}/api/spotify/status`);
    const audio = status && status.audio;
    const sol = audio && audio.soloist;

    if (!audio || audio.backend !== 'soloist' || !sol || !sol.running || !sol.zeroconfPort) {
      if (activeSignature) {
        console.log('[SoloistProxy] Soloist ZeroConf inactive; pausing mDNS advertisement.');
        activeSignature = null;
        currentTargetPort = null;
        stopDnsSd();
      }
      return;
    }

    const zcPort = Number(sol.zeroconfPort);
    // eslint-disable-next-line no-control-regex
    const zcPath = String(sol.zeroconfPath || '/zc').replace(/\x1b\[[0-9;]*m/g, '').trim() || '/zc';
    const deviceName = audio.deviceName || 'Macintosh 68k';
    const instanceName = sol.deviceId || deviceName;
    const sig = `${deviceName}|${instanceName}|${zcPort}|${zcPath}`;

    if (sig !== activeSignature || !dnsSdProc) {
      const localPort = await ensureTcpProxy();
      currentTargetHost = await testUpstreamReachable(zcPort);
      currentTargetPort = zcPort;
      activeSignature = sig;
      startDnsSd({ instanceName, port: localPort, zcPath });
      console.log(
        `[SoloistProxy] Advertising "${deviceName}" via macOS mDNS :${localPort} -> ${currentTargetHost}:${zcPort}${zcPath}`
      );
    }
  } catch (err) {
    if (activeSignature) {
      console.warn('[SoloistProxy] Lost connection to MacCompanion:', err.message);
      activeSignature = null;
      currentTargetPort = null;
      stopDnsSd();
    }
  }
}

function shutdown() {
  console.log('\n[SoloistProxy] Shutting down mDNS advertisement...');
  activeSignature = null;
  stopDnsSd();
  if (tcpServer) {
    try { tcpServer.close(); } catch {}
  }
  process.exit(0);
}

process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);

if (process.platform !== 'darwin') {
  console.error('This helper is only needed on macOS (for OrbStack/Docker Desktop mDNS bridging).');
  process.exit(1);
}

console.log(`[SoloistProxy] Polling ${STATUS_URL}/api/spotify/status for Spotify Soloist ZeroConf...`);
tick();
setInterval(tick, 3000);
