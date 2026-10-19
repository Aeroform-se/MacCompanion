const fs = require('fs');
const path = require('path');

const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, '../../../data');

/**
 * Ensures the persistent data directory exists.
 */
function ensureDataDir() {
  if (!fs.existsSync(DATA_DIR)) {
    fs.mkdirSync(DATA_DIR, { recursive: true });
  }
}

/**
 * Creates a scoped persistent JSON store for a specific module (e.g. 'spotify').
 * Writes are atomic via a temporary file rename.
 */
function createModuleStore(moduleId, defaultState = {}) {
  ensureDataDir();
  const filePath = path.join(DATA_DIR, `${moduleId}.json`);

  function load() {
    try {
      if (fs.existsSync(filePath)) {
        const raw = fs.readFileSync(filePath, 'utf8');
        return { ...defaultState, ...JSON.parse(raw) };
      }
    } catch (err) {
      console.error(`[Storage:${moduleId}] Failed to read ${filePath}:`, err.message);
    }
    return { ...defaultState };
  }

  function save(data) {
    ensureDataDir();
    const current = load();
    const next = { ...current, ...data, updatedAt: new Date().toISOString() };
    const tmpPath = `${filePath}.tmp`;
    fs.writeFileSync(tmpPath, JSON.stringify(next, null, 2), 'utf8');
    fs.renameSync(tmpPath, filePath);
    return next;
  }

  function clear() {
    ensureDataDir();
    if (fs.existsSync(filePath)) {
      fs.unlinkSync(filePath);
    }
    return { ...defaultState };
  }

  return {
    filePath,
    load,
    save,
    clear,
  };
}

module.exports = {
  DATA_DIR,
  ensureDataDir,
  createModuleStore,
};
