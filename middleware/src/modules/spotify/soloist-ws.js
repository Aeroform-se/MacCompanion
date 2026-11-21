const { EventEmitter } = require('events');

const COMMAND_TIMEOUT_MS = 1800;

/**
 * Lightweight WebSocket client wrapping Soloist's `--ws <addr>:<port>` local JSON API
 * using Node.js 22 built-in global `WebSocket` (zero external npm dependencies).
 */
class SoloistWsClient extends EventEmitter {
  constructor({ WebSocketImpl } = {}) {
    super();
    this.WebSocketImpl = WebSocketImpl || global.WebSocket;
    this.ws = null;
    this.host = '127.0.0.1';
    this.port = null;
    this.connected = false;
    this.shouldReconnect = false;
    this._reconnectTimer = null;
    this._generation = 0;

    // Pending FIFO command ack queue (`command_result` or `error` responses)
    this._pendingCommands = [];

    // Cached real-time telemetry state from Soloist
    this.loggedIn = false;
    this.isActiveDevice = false;
    this.deviceName = null;
    this.playbackStatus = 'idle'; // playing | paused | buffering | idle
    this.currentItem = null;
    this.currentContext = null;
    this.volumePercent = null;
    this.options = null;
    this.positionAnchor = null; // { position_ms, timestamp_ms, speed }
    this.lastTrackUri = null;
  }

  resetState() {
    this.connected = false;
    this.loggedIn = false;
    this.isActiveDevice = false;
    this.playbackStatus = 'idle';
    this.currentItem = null;
    this.currentContext = null;
    this.volumePercent = null;
    this.options = null;
    this.positionAnchor = null;
    this.lastTrackUri = null;
    this._rejectAllPending(new Error('Soloist WebSocket closed'));
  }

  _rejectAllPending(err) {
    while (this._pendingCommands.length > 0) {
      const pending = this._pendingCommands.shift();
      clearTimeout(pending.timer);
      pending.reject(err);
    }
  }

  connect(port, host = '127.0.0.1') {
    if (!port || !this.WebSocketImpl) return;
    this.disconnect();
    this.host = host;
    this.port = Number(port);
    this.shouldReconnect = true;
    this._openSocket();
  }

  _openSocket() {
    if (!this.shouldReconnect || !this.port || !this.WebSocketImpl) return;
    const gen = ++this._generation;
    const url = `ws://${this.host}:${this.port}`;

    let ws;
    try {
      ws = new this.WebSocketImpl(url);
    } catch (err) {
      this._scheduleReconnect(gen);
      return;
    }
    this.ws = ws;

    const handleOpen = () => {
      if (gen !== this._generation) return;
      this.connected = true;
      this.emit('open');
      this._safeSend({ type: 'command', command: 'get_auth_state' });
    };

    const handleMessage = (rawData) => {
      if (gen !== this._generation) return;
      const text = typeof rawData === 'string'
        ? rawData
        : (rawData && rawData.data !== undefined ? String(rawData.data) : String(rawData));
      let msg;
      try {
        msg = JSON.parse(text);
      } catch {
        return;
      }
      this._handleEvent(msg);
    };

    const handleClose = () => {
      if (gen !== this._generation) return;
      this.ws = null;
      this.connected = false;
      this._rejectAllPending(new Error('Soloist WebSocket disconnected'));
      this.emit('close');
      this._scheduleReconnect(gen);
    };

    const handleError = () => {
      // `close` event follows and triggers reconnect
    };

    if (typeof ws.addEventListener === 'function') {
      ws.addEventListener('open', handleOpen);
      ws.addEventListener('message', handleMessage);
      ws.addEventListener('close', handleClose);
      ws.addEventListener('error', handleError);
    } else if (typeof ws.on === 'function') {
      ws.on('open', handleOpen);
      ws.on('message', handleMessage);
      ws.on('close', handleClose);
      ws.on('error', handleError);
    }
  }

  _scheduleReconnect(gen) {
    if (!this.shouldReconnect || gen !== this._generation) return;
    if (this._reconnectTimer) clearTimeout(this._reconnectTimer);
    this._reconnectTimer = setTimeout(() => {
      this._reconnectTimer = null;
      if (this.shouldReconnect && gen === this._generation) {
        this._openSocket();
      }
    }, 600);
    if (typeof this._reconnectTimer.unref === 'function') this._reconnectTimer.unref();
  }

  disconnect() {
    this.shouldReconnect = false;
    this._generation++;
    if (this._reconnectTimer) {
      clearTimeout(this._reconnectTimer);
      this._reconnectTimer = null;
    }
    if (this.ws) {
      try { this.ws.close(); } catch {}
      this.ws = null;
    }
    this.resetState();
  }

  _safeSend(payload) {
    if (!this.ws || !this.connected) return false;
    try {
      this.ws.send(JSON.stringify(payload));
      return true;
    } catch {
      return false;
    }
  }

  _updatePositionAnchor(rawPos, fallbackStatus) {
    const pos = rawPos && rawPos.position ? rawPos.position : rawPos;
    if (!pos || typeof pos.position_ms !== 'number') return;
    const speed = typeof pos.speed === 'number'
      ? pos.speed
      : (fallbackStatus === 'playing' ? 1.0 : 0.0);
    this.positionAnchor = {
      position_ms: pos.position_ms,
      timestamp_ms: typeof pos.timestamp_ms === 'number' ? pos.timestamp_ms : Date.now(),
      speed,
    };
  }

  getEstimatedPositionMs() {
    if (!this.positionAnchor) return null;
    const elapsed = Math.max(0, Date.now() - this.positionAnchor.timestamp_ms);
    return Math.max(0, Math.round(this.positionAnchor.position_ms + elapsed * this.positionAnchor.speed));
  }

  _handleEvent(msg) {
    if (!msg || typeof msg !== 'object') return;

    switch (msg.type) {
      case 'auth_state': {
        const wasLoggedIn = this.loggedIn;
        this.loggedIn = Boolean(msg.logged_in);
        if (typeof msg.is_active === 'boolean') this.isActiveDevice = msg.is_active;
        if (msg.device_name) this.deviceName = msg.device_name;
        this.emit('authState', msg);
        if (this.loggedIn && !wasLoggedIn) {
          this._safeSend({ type: 'command', command: 'get_state' });
        }
        break;
      }

      case 'playback_state': {
        const prevUri = this.lastTrackUri;
        if (msg.status) this.playbackStatus = String(msg.status).toLowerCase();
        if (typeof msg.is_active === 'boolean') this.isActiveDevice = msg.is_active;
        if (msg.volume !== undefined && msg.volume !== null) {
          const v = typeof msg.volume === 'number' ? msg.volume : msg.volume.volume;
          if (typeof v === 'number') this.volumePercent = v;
        }
        if (msg.options) this.options = msg.options;
        if (msg.context) this.currentContext = msg.context;
        if (msg.item !== undefined) this.currentItem = msg.item;
        if (msg.position) this._updatePositionAnchor(msg.position, this.playbackStatus);

        const nextUri = (msg.item && msg.item.uri) || null;
        this.lastTrackUri = nextUri;
        this.emit('playbackState', msg);
        if (nextUri && nextUri !== prevUri) {
          this.emit('trackChange', msg.item);
        }
        break;
      }

      case 'playback_changed': {
        if (msg.status) this.playbackStatus = String(msg.status).toLowerCase();
        if (msg.position) {
          this._updatePositionAnchor(msg.position, this.playbackStatus);
        } else if (this.positionAnchor) {
          this.positionAnchor = {
            position_ms: this.getEstimatedPositionMs() ?? this.positionAnchor.position_ms,
            timestamp_ms: Date.now(),
            speed: this.playbackStatus === 'playing' ? 1.0 : 0.0,
          };
        }
        this.emit('playbackChanged', msg);
        break;
      }

      case 'track_changed': {
        const item = msg.item || msg.track || msg;
        this.currentItem = item || null;
        this.lastTrackUri = (item && item.uri) || this.lastTrackUri;
        if (msg.position) this._updatePositionAnchor(msg.position, this.playbackStatus);
        this.emit('trackChange', item);
        break;
      }

      case 'position_sync': {
        this._updatePositionAnchor(msg, this.playbackStatus);
        this.emit('positionSync', this.positionAnchor);
        break;
      }

      case 'volume_changed': {
        const v = typeof msg.volume === 'number' ? msg.volume : msg.volume_percent;
        if (typeof v === 'number') this.volumePercent = v;
        this.emit('volumeChanged', msg);
        break;
      }

      case 'options_changed': {
        this.options = msg.options || msg;
        this.emit('optionsChanged', this.options);
        break;
      }

      case 'device_changed': {
        if (typeof msg.is_active === 'boolean') this.isActiveDevice = msg.is_active;
        if (msg.device_name) this.deviceName = msg.device_name;
        this.emit('deviceChanged', msg);
        break;
      }

      case 'command_result': {
        const pending = this._pendingCommands.shift();
        if (pending) {
          clearTimeout(pending.timer);
          if (msg.success === false) {
            pending.reject(new Error(msg.error || msg.message || `Soloist command "${pending.command}" failed`));
          } else {
            pending.resolve(msg);
          }
        }
        break;
      }

      case 'error': {
        const pending = this._pendingCommands.shift();
        if (pending) {
          clearTimeout(pending.timer);
          pending.reject(new Error(msg.message || `Soloist error during "${pending.command}"`));
        } else {
          this.emit('wsError', msg);
        }
        break;
      }

      default:
        break;
    }
  }

  sendCommand(command, params = {}, timeoutMs = COMMAND_TIMEOUT_MS) {
    if (!this.connected || !this.ws) {
      return Promise.reject(new Error('Soloist WebSocket is not connected'));
    }
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const idx = this._pendingCommands.findIndex((p) => p.resolve === resolve);
        if (idx >= 0) this._pendingCommands.splice(idx, 1);
        reject(new Error(`Soloist command "${command}" timed out`));
      }, timeoutMs);

      this._pendingCommands.push({ command, resolve, reject, timer });
      const sent = this._safeSend({ type: 'command', command, ...params });
      if (!sent) {
        clearTimeout(timer);
        const idx = this._pendingCommands.findIndex((p) => p.resolve === resolve);
        if (idx >= 0) this._pendingCommands.splice(idx, 1);
        reject(new Error('Failed to write command to Soloist WebSocket'));
      }
    });
  }
}

module.exports = {
  SoloistWsClient,
};
