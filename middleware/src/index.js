const express = require('express');
const path = require('path');
const registry = require('./core/registry');
const { ensureDataDir, DATA_DIR } = require('./core/storage');
const { startDiscoveryServer, getLocalIpAddress, DEFAULT_UDP_PORT, DISCOVERY_MAGIC } = require('./core/discovery');

// Service Modules & Versioning
const { SERVER_NAME, SERVER_VERSION_SLUG, SERVER_CODENAME } = require('./version');
const spotifyModule = require('./modules/spotify');
const macaiModule = require('./modules/macai');
const macdropModule = require('./modules/macdrop');

const HTTP_PORT = parseInt(process.env.PORT || '6800', 10);
const UDP_PORT = DEFAULT_UDP_PORT;

function createApp() {
  ensureDataDir();

  const app = express();
  app.disable('x-powered-by');
  app.set('etag', false);
  app.use(express.json());
  app.use(express.urlencoded({ extended: true }));

  // Register standalone modules
  registry.register(spotifyModule);
  registry.register(macaiModule);
  registry.register(macdropModule);

  // Mount all registered module routers (/api/spotify, /api/macai, etc.)
  registry.mountAll(app);

  // Global middleware status endpoint (JSON for Web UI)
  app.get('/api/status', (req, res) => {
    res.json({
      server: {
        name: SERVER_NAME,
        version: SERVER_VERSION_SLUG,
        codename: SERVER_CODENAME,
        ip: getLocalIpAddress(),
        httpPort: HTTP_PORT,
        udpPort: UDP_PORT,
        dataDir: DATA_DIR,
        uptimeSeconds: Math.floor(process.uptime()),
      },
      modules: registry.getAllStatuses(),
    });
  });

  // Pipe-delimited discovery/health endpoint for 68k Macintosh HTTP verification
  app.get('/api/ping', (req, res) => {
    const ip = getLocalIpAddress();
    const modules = registry.getModuleIdList() || 'none';
    res.type('text/plain').send(`MACCOMPANION_SERVER|${ip}|${HTTP_PORT}|${modules}\n`);
  });

  // Live UDP Discovery Test endpoint (sends a real UDP packet to the UDP discovery port and returns the reply)
  app.post('/api/test-udp', (req, res) => {
    const dgram = require('dgram');
    const client = dgram.createSocket('udp4');
    const timeout = setTimeout(() => {
      client.close();
      res.status(504).type('text/plain').send('ERR|504|UDP discovery timed out\n');
    }, 2500);

    client.on('message', (msg, rinfo) => {
      clearTimeout(timeout);
      client.close();
      res.type('text/plain').send(`UDP_FROM|${rinfo.address}:${rinfo.port}\n${msg.toString('utf8')}`);
    });

    const buf = Buffer.from(DISCOVERY_MAGIC, 'ascii');
    client.send(buf, 0, buf.length, UDP_PORT, '127.0.0.1', (err) => {
      if (err) {
        clearTimeout(timeout);
        client.close();
        res.status(500).type('text/plain').send(`ERR|500|${err.message}\n`);
      }
    });
  });

  // Handle librespot speaker OAuth callback on http://127.0.0.1:6800/login
  app.get('/login', async (req, res) => {
    try {
      await spotifyModule.audioEngine.handleSpeakerCallback({
        code: req.query.code,
        state: req.query.state,
        error: req.query.error,
      });
      res.redirect('http://localhost:6800/');
    } catch (err) {
      res.redirect(`http://localhost:6800/?error=${encodeURIComponent(err.message)}`);
    }
  });

  // Serve Status Web UI on root (and any static files in public/)
  app.use(express.static(path.join(__dirname, 'public')));

  return app;
}

if (require.main === module) {
  const app = createApp();

  const server = app.listen(HTTP_PORT, '0.0.0.0', () => {
    const ip = getLocalIpAddress();
    console.log(`[MacCompanion] HTTP Server listening on http://0.0.0.0:${HTTP_PORT} (LAN: http://${ip}:${HTTP_PORT})`);
    console.log(`[MacCompanion] Persistent data directory: ${DATA_DIR}`);
  });
  server.keepAliveTimeout = 15000;

  const udpSocket = startDiscoveryServer({
    httpPort: HTTP_PORT,
    udpPort: UDP_PORT,
    registry,
  });

  // Start local 68k raw 8-bit PCM TCP audio server (port 6802) and librespot receiver if authenticated
  spotifyModule.audioEngine.startTcpServer();
  spotifyModule.audioEngine.ensureStarted().catch(() => {});

  const shutdown = () => {
    console.log('[MacCompanion] Shutting down...');
    spotifyModule.audioEngine.close();
    udpSocket.close();
    server.close(() => process.exit(0));
  };

  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

module.exports = { createApp, registry };
