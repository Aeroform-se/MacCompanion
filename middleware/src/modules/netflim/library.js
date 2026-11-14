const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { DATA_DIR, ensureDataDir } = require('../../core/storage');

const DEFAULT_ROOT = path.join(DATA_DIR, 'netflim');

/**
 * Generates a short URL-safe ID suitable for 68k HTTP paths and filenames.
 */
function generateVideoId() {
  return `nf_${crypto.randomBytes(4).toString('hex')}`;
}

/**
 * Sanitizes an arbitrary filename into a safe basename (ASCII, no path traversal).
 */
function sanitizeFilename(name, fallback = 'video.bin') {
  const base = path.basename(String(name || fallback)).replace(/[^\w.\-() ]+/g, '_').trim();
  if (!base || base === '.' || base === '..') return fallback;
  return base.slice(0, 120);
}

/**
 * Derives a human-friendly title from a filename if no explicit title was provided.
 */
function titleFromFilename(filename) {
  const base = path.basename(String(filename || 'Untitled'), path.extname(String(filename || '')));
  const cleaned = base.replace(/[_]+/g, ' ').replace(/\s+/g, ' ').trim();
  return cleaned.slice(0, 80) || 'Untitled Video';
}

/**
 * Creates a NetFlim persistent library manager anchored at `rootDir`.
 */
function createLibrary(customRootDir) {
  const rootDir = customRootDir || process.env.NETFLIM_DATA_DIR || DEFAULT_ROOT;
  const catalogPath = path.join(rootDir, 'library.json');
  const dirs = {
    root: rootDir,
    sources: path.join(rootDir, 'sources'),
    flims: path.join(rootDir, 'flims'),
    posters: path.join(rootDir, 'posters'),
    tmp: path.join(rootDir, 'tmp'),
  };

  function ensureDirs() {
    ensureDataDir();
    for (const dir of Object.values(dirs)) {
      if (!fs.existsSync(dir)) {
        fs.mkdirSync(dir, { recursive: true });
      }
    }
  }

  function cleanTmpOnStartup() {
    ensureDirs();
    try {
      const entries = fs.readdirSync(dirs.tmp);
      for (const entry of entries) {
        fs.rmSync(path.join(dirs.tmp, entry), { recursive: true, force: true });
      }
    } catch {
      // Ignore transient cleanup issues
    }
  }

  function resolvePath(relOrAbsPath) {
    if (!relOrAbsPath) return null;
    if (path.isAbsolute(relOrAbsPath)) return relOrAbsPath;
    return path.join(rootDir, relOrAbsPath);
  }

  function toRelativePath(absPath) {
    if (!absPath) return null;
    return path.relative(rootDir, absPath);
  }

  function loadCatalog() {
    ensureDirs();
    if (!fs.existsSync(catalogPath)) {
      return { version: 1, items: [] };
    }
    try {
      const raw = fs.readFileSync(catalogPath, 'utf8');
      const parsed = JSON.parse(raw);
      return {
        version: parsed.version || 1,
        items: Array.isArray(parsed.items) ? parsed.items : [],
      };
    } catch {
      return { version: 1, items: [] };
    }
  }

  function saveCatalog(catalog) {
    ensureDirs();
    const tmpFile = `${catalogPath}.tmp.${process.pid}`;
    fs.writeFileSync(tmpFile, JSON.stringify(catalog, null, 2), 'utf8');
    fs.renameSync(tmpFile, catalogPath);
  }

  function listItems() {
    const catalog = loadCatalog();
    return [...catalog.items].sort((a, b) => {
      const ta = Date.parse(a.createdAt || 0) || 0;
      const tb = Date.parse(b.createdAt || 0) || 0;
      return tb - ta;
    });
  }

  function getItem(id) {
    const catalog = loadCatalog();
    return catalog.items.find((item) => item.id === id) || null;
  }

  function upsertItem(item) {
    const catalog = loadCatalog();
    const now = new Date().toISOString();
    const idx = catalog.items.findIndex((x) => x.id === item.id);
    const next = {
      ...item,
      updatedAt: now,
      createdAt: item.createdAt || now,
    };
    if (idx >= 0) {
      catalog.items[idx] = next;
    } else {
      catalog.items.push(next);
    }
    saveCatalog(catalog);
    return next;
  }

  function updateItem(id, patch) {
    const catalog = loadCatalog();
    const idx = catalog.items.findIndex((x) => x.id === id);
    if (idx < 0) return null;

    const current = catalog.items[idx];
    const updated = {
      ...current,
      ...patch,
      id: current.id,
      updatedAt: new Date().toISOString(),
    };
    catalog.items[idx] = updated;
    saveCatalog(catalog);
    return updated;
  }

  function addProfileToItem(id, profileKey, profileMeta) {
    const catalog = loadCatalog();
    const idx = catalog.items.findIndex((x) => x.id === id);
    if (idx < 0) return null;

    const current = catalog.items[idx];
    const profiles = { ...(current.profiles || {}), [profileKey]: profileMeta };
    const updated = {
      ...current,
      durationSec: current.durationSec || profileMeta.durationSec || 0,
      width: current.width || profileMeta.width || 512,
      height: current.height || profileMeta.height || 342,
      profiles,
      updatedAt: new Date().toISOString(),
    };
    catalog.items[idx] = updated;
    saveCatalog(catalog);
    return updated;
  }

  function removeFileSafe(relOrAbsPath) {
    const abs = resolvePath(relOrAbsPath);
    if (!abs) return;
    try {
      if (fs.existsSync(abs)) {
        fs.unlinkSync(abs);
      }
    } catch {
      // Best-effort cleanup
    }
  }

  function deleteProfile(id, profileKey) {
    const catalog = loadCatalog();
    const idx = catalog.items.findIndex((x) => x.id === id);
    if (idx < 0) return null;

    const current = catalog.items[idx];
    const profiles = { ...(current.profiles || {}) };
    if (profiles[profileKey]) {
      removeFileSafe(profiles[profileKey].file);
      delete profiles[profileKey];
    }
    const updated = {
      ...current,
      profiles,
      updatedAt: new Date().toISOString(),
    };
    catalog.items[idx] = updated;
    saveCatalog(catalog);
    return updated;
  }

  function deleteItem(id) {
    const catalog = loadCatalog();
    const idx = catalog.items.findIndex((x) => x.id === id);
    if (idx < 0) return null;

    const [removed] = catalog.items.splice(idx, 1);
    saveCatalog(catalog);

    if (removed.sourcePath) removeFileSafe(removed.sourcePath);
    if (removed.posterBin) removeFileSafe(removed.posterBin);
    if (removed.posterPng) removeFileSafe(removed.posterPng);
    if (removed.profiles && typeof removed.profiles === 'object') {
      for (const prof of Object.values(removed.profiles)) {
        if (prof && prof.file) removeFileSafe(prof.file);
      }
    }
    return removed;
  }

  function getProfileFile(id, profileKey) {
    const item = getItem(id);
    if (!item || !item.profiles) return null;
    const entry = item.profiles[profileKey] || null;
    if (!entry || !entry.file) return null;
    const absPath = resolvePath(entry.file);
    if (!absPath || !fs.existsSync(absPath)) return null;
    return { item, entry, absPath };
  }

  function getPosterPaths(id) {
    const item = getItem(id);
    if (!item) return null;
    const binPath = resolvePath(item.posterBin);
    const pngPath = resolvePath(item.posterPng);
    return {
      item,
      binPath: binPath && fs.existsSync(binPath) ? binPath : null,
      pngPath: pngPath && fs.existsSync(pngPath) ? pngPath : null,
    };
  }

  function getSummary() {
    const items = listItems();
    let flimCount = 0;
    let totalBytes = 0;

    for (const item of items) {
      totalBytes += Number(item.sourceSize || 0);
      const profiles = item.profiles || {};
      for (const prof of Object.values(profiles)) {
        if (prof) {
          flimCount += 1;
          totalBytes += Number(prof.size || 0);
        }
      }
    }

    return {
      videoCount: items.length,
      flimCount,
      totalBytes,
    };
  }

  cleanTmpOnStartup();

  return {
    rootDir,
    dirs,
    ensureDirs,
    cleanTmpOnStartup,
    resolvePath,
    toRelativePath,
    listItems,
    getItem,
    upsertItem,
    updateItem,
    addProfileToItem,
    deleteProfile,
    deleteItem,
    getProfileFile,
    getPosterPaths,
    getSummary,
  };
}

module.exports = {
  createLibrary,
  generateVideoId,
  sanitizeFilename,
  titleFromFilename,
};
