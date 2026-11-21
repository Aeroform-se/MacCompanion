const fs = require('fs');
const path = require('path');
const { execFile } = require('child_process');
const { promisify } = require('util');
const { Readable } = require('stream');
const { pipeline } = require('stream/promises');
const { DATA_DIR } = require('../../core/storage');

const execFileAsync = promisify(execFile);

const DEFAULT_SOLOIST_CDN_BASE = 'https://soloist-builds.spotifycdn.com';
const PROACTIVE_RENEW_THRESHOLD_DAYS = 7;

const ARCH_TARBALL_MAP = {
  x64: 'soloist_release_x86_64.tar.gz',
  arm64: 'soloist_release_arm64.tar.gz',
  arm: 'soloist_release_arm32.tar.gz',
};

/**
 * Resolves the official Spotify CDN archive filename for the current Node CPU architecture.
 */
function getTarballNameForArch(arch = process.arch) {
  return ARCH_TARBALL_MAP[arch] || null;
}

const AUTH_EXCLUDE_NAMES = new Set([
  'crashpad',
  '.lock',
  'LOCK',
  'soloist.pid',
  'ws.addr',
  'ws.port',
  'soloist',
  'soloist.exe',
]);

function validateArchiveEntries(entries) {
  for (const rawLine of entries) {
    const entry = String(rawLine || '').trim();
    if (!entry) continue;
    if (entry.startsWith('/') || entry.startsWith('\\') || /^[A-Za-z]:/.test(entry)) {
      throw new Error(`Rejected unsafe absolute archive path: ${entry}`);
    }
    const segs = entry.split(/[\\/]+/);
    if (segs.includes('..')) {
      throw new Error(`Rejected unsafe traversal archive path: ${entry}`);
    }
  }
}

function findAuthRootDir(baseDir) {
  const hasCoreFiles = (dir) => {
    try {
      const devIdPath = path.join(dir, '.device_id');
      const settingsPath = path.join(dir, 'settings');
      const cachePath = path.join(dir, 'cache');
      return (
        fs.existsSync(devIdPath) &&
        fs.statSync(devIdPath).isFile() &&
        fs.existsSync(settingsPath) &&
        fs.statSync(settingsPath).isDirectory() &&
        fs.existsSync(cachePath) &&
        fs.statSync(cachePath).isDirectory()
      );
    } catch {
      return false;
    }
  };

  if (hasCoreFiles(baseDir)) return baseDir;
  for (const entry of fs.readdirSync(baseDir, { withFileTypes: true })) {
    if (!entry.isDirectory()) continue;
    const sub = path.join(baseDir, entry.name);
    if (hasCoreFiles(sub)) return sub;
  }
  return null;
}

function copySafeAuthTree(srcDir, dstDir) {
  fs.mkdirSync(dstDir, { recursive: true, mode: 0o700 });
  for (const entry of fs.readdirSync(srcDir)) {
    if (AUTH_EXCLUDE_NAMES.has(entry)) continue;
    const srcPath = path.join(srcDir, entry);
    const dstPath = path.join(dstDir, entry);
    const st = fs.lstatSync(srcPath);
    if (st.isSymbolicLink()) {
      throw new Error(`Rejected unsafe symlink in auth archive: ${entry}`);
    }
    if (st.isDirectory()) {
      copySafeAuthTree(srcPath, dstPath);
    } else if (st.isFile()) {
      fs.copyFileSync(srcPath, dstPath);
      fs.chmodSync(dstPath, 0o600);
    }
  }
}

/**
 * Masks a Soloist API key for safe display in Web UI / status payloads.
 */
function maskApiKey(rawKey) {
  const key = String(rawKey || '').trim();
  if (!key) return '';
  if (key.length <= 10) return '••••';
  return `${key.slice(0, 7)}••••${key.slice(-4)}`;
}

/**
 * Manages runtime download, extraction, verification, and 90-day renewal of the
 * official Spotify Soloist binary inside persistent storage (`/app/data/soloist/bin/`).
 */
class SoloistBinaryManager {
  constructor({ store, dataDir = DATA_DIR, getFetchImpl } = {}) {
    this.store = store;
    this.dataDir = dataDir;
    this.getFetchImpl = getFetchImpl || (() => global.fetch);
    this.baseDir = path.join(this.dataDir, 'soloist');
    this.binDir = path.join(this.baseDir, 'bin');
    this.stateDir = path.join(this.baseDir, 'state');
    this.cacheDir = path.join(this.baseDir, 'cache');
    this.binPath = path.join(this.binDir, 'soloist');
    this._downloadPromise = null;
    this.downloading = false;
    this.lastDownloadError = null;
  }

  ensureDirs() {
    for (const dir of [this.baseDir, this.binDir, this.stateDir, this.cacheDir]) {
      if (!fs.existsSync(dir)) {
        fs.mkdirSync(dir, { recursive: true, mode: 0o700 });
      }
    }
  }

  isBinaryInstalled() {
    try {
      const st = fs.statSync(this.binPath);
      if (!st.isFile() || st.size < 1024) return false;
      fs.accessSync(this.binPath, fs.constants.X_OK);
      return true;
    } catch {
      return false;
    }
  }

  getCdnUrl(arch = process.arch) {
    const tarball = getTarballNameForArch(arch);
    if (!tarball) {
      throw new Error(`Unsupported CPU architecture for Spotify Soloist Linux binary: ${arch}`);
    }
    const base = (process.env.SOLOIST_CDN_BASE || DEFAULT_SOLOIST_CDN_BASE).replace(/\/$/, '');
    return { url: `${base}/${tarball}`, tarball };
  }

  needsProactiveRenewal() {
    if (!this.store) return false;
    const data = this.store.load();
    if (typeof data.soloistExpiresAt === 'number' && data.soloistExpiresAt > 0) {
      const msLeft = data.soloistExpiresAt - Date.now();
      return msLeft <= PROACTIVE_RENEW_THRESHOLD_DAYS * 24 * 60 * 60 * 1000;
    }
    if (typeof data.soloistExpiresDaysLeft === 'number') {
      return data.soloistExpiresDaysLeft <= PROACTIVE_RENEW_THRESHOLD_DAYS;
    }
    return false;
  }

  recordExpiryDays(daysLeft) {
    if (!Number.isFinite(daysLeft) || daysLeft < 0 || !this.store) return;
    const expiresAt = Date.now() + daysLeft * 24 * 60 * 60 * 1000;
    this.store.save({
      soloistExpiresDaysLeft: daysLeft,
      soloistExpiresAt: expiresAt,
    });
  }

  async ensureBinary({ forceDownload = false } = {}) {
    if (!forceDownload && this.isBinaryInstalled() && !this.needsProactiveRenewal()) {
      return this.binPath;
    }
    if (this._downloadPromise) {
      return this._downloadPromise;
    }
    this._downloadPromise = this._downloadAndInstall().finally(() => {
      this._downloadPromise = null;
      this.downloading = false;
    });
    return this._downloadPromise;
  }

  async _downloadAndInstall() {
    if (process.platform !== 'linux') {
      throw new Error(`Spotify Soloist container binary requires Linux (current platform: ${process.platform})`);
    }
    this.ensureDirs();
    const { url, tarball } = this.getCdnUrl(process.arch);
    const tmpTgz = path.join(this.binDir, `${tarball}.tmp.${process.pid}`);
    const tmpExtractDir = path.join(this.binDir, `extract.tmp.${process.pid}`);

    this.downloading = true;
    this.lastDownloadError = null;
    console.log(`[SoloistManager] Downloading ${tarball} from ${url}...`);

    try {
      const fetchFn = this.getFetchImpl();
      const res = await fetchFn(url);
      if (!res || !res.ok || !res.body) {
        throw new Error(`HTTP ${res ? res.status : 'network error'} fetching ${tarball}`);
      }

      const readable = typeof res.body.getReader === 'function'
        ? Readable.fromWeb(res.body)
        : res.body;
      await pipeline(readable, fs.createWriteStream(tmpTgz, { mode: 0o600 }));

      fs.rmSync(tmpExtractDir, { recursive: true, force: true });
      fs.mkdirSync(tmpExtractDir, { recursive: true, mode: 0o700 });
      await execFileAsync('tar', ['-xzf', tmpTgz, '-C', tmpExtractDir]);

      const extractedBin = path.join(tmpExtractDir, 'soloist');
      if (!fs.existsSync(extractedBin)) {
        throw new Error(`Downloaded ${tarball} did not contain soloist binary`);
      }

      const stageBin = path.join(this.binDir, `soloist.new.${process.pid}`);
      fs.copyFileSync(extractedBin, stageBin);
      fs.chmodSync(stageBin, 0o755);
      fs.renameSync(stageBin, this.binPath);

      let versionStr = null;
      try {
        const { stdout, stderr } = await execFileAsync(this.binPath, ['--version'], { timeout: 5000 });
        versionStr = (stdout || stderr || '').trim() || null;
      } catch (verErr) {
        throw new Error(`Downloaded soloist binary failed verification (--version): ${verErr.message}`);
      }

      if (this.store) {
        this.store.save({
          soloistBinaryVersion: versionStr,
          soloistBinaryArch: process.arch,
          soloistBinaryDownloadedAt: new Date().toISOString(),
          soloistExpiresDaysLeft: 89,
          soloistExpiresAt: Date.now() + 89 * 24 * 60 * 60 * 1000,
        });
      }

      console.log(`[SoloistManager] Installed ${versionStr || 'soloist'} at ${this.binPath}`);
      return this.binPath;
    } catch (err) {
      this.lastDownloadError = err.message;
      throw err;
    } finally {
      try { fs.rmSync(tmpTgz, { force: true }); } catch {}
      try { fs.rmSync(tmpExtractDir, { recursive: true, force: true }); } catch {}
    }
  }

  async importAuthArchive(archiveBuffer) {
    if (!Buffer.isBuffer(archiveBuffer) || archiveBuffer.length < 22) {
      throw new Error('Auth archive is empty or invalid');
    }
    if (archiveBuffer.length > 5 * 1024 * 1024) {
      throw new Error('Auth archive exceeds 5 MB limit');
    }

    const isGzip = archiveBuffer[0] === 0x1f && archiveBuffer[1] === 0x8b;
    const isZip = archiveBuffer[0] === 0x50 && archiveBuffer[1] === 0x4b;
    if (!isGzip && !isZip) {
      throw new Error('Unsupported auth archive format (please upload .tar.gz or .zip)');
    }

    this.ensureDirs();
    const tmpWork = path.join(this.baseDir, `upload.tmp.${process.pid}.${Date.now()}`);
    const archivePath = path.join(tmpWork, isGzip ? 'auth.tar.gz' : 'auth.zip');
    const extractDir = path.join(tmpWork, 'extracted');
    const stagedStateDir = path.join(this.baseDir, `state.new.${process.pid}.${Date.now()}`);

    fs.mkdirSync(extractDir, { recursive: true, mode: 0o700 });
    fs.writeFileSync(archivePath, archiveBuffer, { mode: 0o600 });

    try {
      if (isGzip) {
        const { stdout } = await execFileAsync('tar', ['-tzf', archivePath], { timeout: 5000 });
        validateArchiveEntries(stdout.split(/\r?\n/));
        await execFileAsync('tar', ['-xzf', archivePath, '--no-same-owner', '--no-same-permissions', '-C', extractDir], {
          timeout: 10000,
        });
      } else {
        const { stdout } = await execFileAsync('unzip', ['-Z1', archivePath], { timeout: 5000 });
        validateArchiveEntries(stdout.split(/\r?\n/));
        await execFileAsync('unzip', ['-q', '-o', archivePath, '-d', extractDir], { timeout: 10000 });
      }

      const authRoot = findAuthRootDir(extractDir);
      if (!authRoot) {
        throw new Error('Auth archive is missing required Soloist files (.device_id, settings/, cache/)');
      }

      copySafeAuthTree(authRoot, stagedStateDir);

      fs.rmSync(this.stateDir, { recursive: true, force: true });
      fs.renameSync(stagedStateDir, this.stateDir);
      console.log(`[SoloistManager] Imported paired Soloist credentials into ${this.stateDir}`);
      return { ok: true, stateDir: this.stateDir };
    } finally {
      try { fs.rmSync(tmpWork, { recursive: true, force: true }); } catch {}
      try { fs.rmSync(stagedStateDir, { recursive: true, force: true }); } catch {}
    }
  }

  getStatus() {
    const data = this.store ? this.store.load() : {};
    return {
      installed: this.isBinaryInstalled(),
      downloading: Boolean(this.downloading),
      binPath: this.binPath,
      version: data.soloistBinaryVersion || null,
      arch: data.soloistBinaryArch || process.arch,
      downloadedAt: data.soloistBinaryDownloadedAt || null,
      expiresDaysLeft: typeof data.soloistExpiresDaysLeft === 'number' ? data.soloistExpiresDaysLeft : null,
      expiresAt: data.soloistExpiresAt || null,
      lastDownloadError: this.lastDownloadError,
    };
  }
}

module.exports = {
  DEFAULT_SOLOIST_CDN_BASE,
  PROACTIVE_RENEW_THRESHOLD_DAYS,
  getTarballNameForArch,
  maskApiKey,
  SoloistBinaryManager,
};
