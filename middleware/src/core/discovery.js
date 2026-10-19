const dgram = require('dgram');
const os = require('os');

const DEFAULT_UDP_PORT = parseInt(process.env.UDP_DISCOVERY_PORT || '6801', 10);
const DISCOVERY_MAGIC = 'RETRO68_DISCOVER';

/**
 * Determines the primary LAN IPv4 address of the host/container.
 * Can be overridden with HOST_LAN_IP env variable when running inside NAT/bridged Docker.
 */
function getLocalIpAddress() {
  if (process.env.HOST_LAN_IP) {
    return process.env.HOST_LAN_IP;
  }

  const interfaces = os.networkInterfaces();
  for (const name of Object.keys(interfaces)) {
    for (const iface of interfaces[name]) {
      if (iface.family === 'IPv4' && !iface.internal) {
        return iface.address;
      }
    }
  }
  return '127.0.0.1';
}

/**
 * Starts a UDP broadcast listener that responds to 68k Mac discovery pings
 * with a pipe-delimited string:
 *   RETRO68_SERVER|<ip>|<httpPort>|<comma_separated_modules>
 */
function startDiscoveryServer({ httpPort, registry, udpPort = DEFAULT_UDP_PORT }) {
  const socket = dgram.createSocket({ type: 'udp4', reuseAddr: true });

  socket.on('error', (err) => {
    console.error(`[Discovery] UDP socket error:\n${err.stack}`);
  });

  socket.on('message', (msg, rinfo) => {
    const text = msg.toString('utf8').trim();
    if (text.startsWith(DISCOVERY_MAGIC) || text === 'PING') {
      const ip = getLocalIpAddress();
      const modules = registry.getModuleIdList() || 'none';
      const payload = `RETRO68_SERVER|${ip}|${httpPort}|${modules}\n`;
      const responseBuffer = Buffer.from(payload, 'ascii');

      socket.send(responseBuffer, 0, responseBuffer.length, rinfo.port, rinfo.address, (err) => {
        if (err) {
          console.error(`[Discovery] Error sending UDP reply to ${rinfo.address}:${rinfo.port}:`, err.message);
        } else {
          console.log(`[Discovery] Replied to ${rinfo.address}:${rinfo.port} -> ${payload.trim()}`);
        }
      });
    }
  });

  socket.on('listening', () => {
    try {
      socket.setBroadcast(true);
    } catch (e) {
      // Ignore if platform restricts setBroadcast prior to send
    }
    const address = socket.address();
    console.log(`[Discovery] UDP broadcast listener active on ${address.address}:${address.port}`);
  });

  socket.bind(udpPort, '0.0.0.0');
  return socket;
}

module.exports = {
  DISCOVERY_MAGIC,
  DEFAULT_UDP_PORT,
  getLocalIpAddress,
  startDiscoveryServer,
};
