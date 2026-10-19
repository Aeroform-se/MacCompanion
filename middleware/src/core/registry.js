/**
 * Central Registry for Middleware Service Modules.
 *
 * Allows standalone services (Spotify, Home Assistant, Weather, etc.) to be
 * registered cleanly without coupling core server or discovery code to any
 * single service.
 */
class ModuleRegistry {
  constructor() {
    this.modules = new Map();
  }

  /**
   * Registers a service module.
   * @param {Object} mod
   * @param {string} mod.id - Unique identifier (e.g., 'spotify')
   * @param {string} mod.name - Human-readable name (e.g., 'Spotify Connect')
   * @param {string} mod.basePath - Express router mount path (e.g., '/api/spotify')
   * @param {import('express').Router} mod.router - Express router instance
   * @param {Function} mod.getStatus - Function returning current module status object
   */
  register(mod) {
    if (!mod || !mod.id || !mod.router) {
      throw new Error('Invalid module registration: requires id and router');
    }
    this.modules.set(mod.id, mod);
    console.log(`[Registry] Registered module: ${mod.name} (${mod.id}) -> ${mod.basePath}`);
  }

  /**
   * Mounts all registered module routers onto an Express app.
   * @param {import('express').Application} app
   */
  mountAll(app) {
    for (const mod of this.modules.values()) {
      app.use(mod.basePath, mod.router);
    }
  }

  /**
   * Returns a specific module by ID.
   */
  get(id) {
    return this.modules.get(id);
  }

  /**
   * Returns status metadata for all registered modules.
   */
  getAllStatuses() {
    const statuses = {};
    for (const [id, mod] of this.modules.entries()) {
      statuses[id] = typeof mod.getStatus === 'function'
        ? mod.getStatus()
        : { id, name: mod.name, ok: true, state: 'READY', message: 'Service ready' };
    }
    return statuses;
  }

  /**
   * Returns comma-separated list of active module IDs for UDP discovery responses.
   */
  getModuleIdList() {
    return Array.from(this.modules.keys()).join(',');
  }
}

module.exports = new ModuleRegistry();
