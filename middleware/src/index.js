const express = require('express');
const path = require('path');
const registry = require('./core/registry');
const { ensureDataDir, DATA_DIR } = require('./core/storage');
const { startDiscoveryServer, getLocalIpAddress, DEFAULT_UDP_PORT } = require('./core/discovery');

// Service Modules
const spotifyModule = require('./modules/spotify');

const HTTP_PORT = parseInt(process.env.PORT || '6800', 10);
const UDP_PORT = DEFAULT_UDP_PORT;

function createApp() {
  ensureDataDir();

  const app = express();
  app.use(express.json());
  app.use(express.urlencoded({ extended: true }));

  // Register standalone modules
  registry.register(spotifyModule);

  // Mount all registered module routers (/api/spotify, etc.)
  registry.mountAll(app);

  // Global middleware status endpoint (JSON for Web UI)
  app.get('/api/status', (req, res) => {
    res.json({
      server: {
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
    res.type('text/plain').send(`RETRO68_SERVER|${ip}|${HTTP_PORT}|${modules}\n`);
  });

  // Serve Status Web UI (Happy Mac page) on root
  app.use(express.static(path.join(__dirname, 'public')));

  return app;
}

if (require.main === module) {
  const app = createApp();

  const server = app.listen(HTTP_PORT, '0.0.0.0', () => {
    const ip = getLocalIpAddress();
    console.log(`[Middleware] HTTP Server listening on http://0.0.0.0:${HTTP_PORT} (LAN: http://${ip}:${HTTP_PORT})`);
    console.log(`[Middleware] Persistent data directory: ${DATA_DIR}`);
  });

  const udpSocket = startDiscoveryServer({
    httpPort: HTTP_PORT,
    udpPort: UDP_PORT,
    registry,
  });

  const shutdown = () => {
    console.log('[Middleware] Shutting down...');
    udpSocket.close();
    server.close(() => process.exit(0));
  };

  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);
}

module.exports = { createApp, registry };
