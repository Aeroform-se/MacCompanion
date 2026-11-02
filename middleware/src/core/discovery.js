const dgram = require('dgram');
const os = require('os');

const DEFAULT_UDP_PORT = parseInt(process.env.UDP_DISCOVERY_PORT || '6801', 10);
const DISCOVERY_MAGIC = 'MACCOMPANION_DISCOVER';

let observedHostIp = null;

/**
 * Records a LAN IPv4 address seen in an incoming HTTP Host header (e.g. "192.168.1.177:6800")
 * so when running inside bridged/NAT Docker without HOST_LAN_IP set, the server automatically
 * learns its real host LAN IP from browser or 68k client requests.
 */
function recordObservedHost(hostHeader) {
  if (!hostHeader || typeof hostHeader !== 'string') return;
  const host = hostHeader.split(':')[0].trim();
  if (/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(host)) {
    if (host !== '127.0.0.1' && host !== '0.0.0.0' && !host.startsWith('172.17.') && !host.startsWith('192.168.97.') && !host.startsWith('192.168.65.')) {
      observedHostIp = host;
    }
  }
}

function getObservedHostIp() {
  return observedHostIp;
}

/**
 * Determines the primary LAN IPv4 address of the host/container.
 * Can be overridden with HOST_LAN_IP env variable when running inside NAT/bridged Docker,
 * or learned dynamically from incoming HTTP Host headers.
 */
function getLocalIpAddress() {
  if (process.env.HOST_LAN_IP) {
    return process.env.HOST_LAN_IP;
  }
  if (observedHostIp) {
    return observedHostIp;
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
 *   MACCOMPANION_SERVER|<ip>|<httpPort>|<comma_separated_modules>
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
      const payload = `MACCOMPANION_SERVER|${ip}|${httpPort}|${modules}\n`;
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
  recordObservedHost,
  getObservedHostIp,
  startDiscoveryServer,
};
