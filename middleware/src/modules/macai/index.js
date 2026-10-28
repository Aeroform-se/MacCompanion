const express = require('express');
const crypto = require('crypto');
const { spawn } = require('child_process');
const { createModuleStore } = require('../../core/storage');
const { sanitizeField, formatPipeRow, formatPipeTable, sendPipeResponse } = require('../../core/format68k');
const audioHub = require('../../core/audio-hub');

const router = express.Router();

const PROVIDERS = {
  gemini: {
    id: 'gemini',
    name: 'Gemini',
    defaultModel: 'gemini-3.8-flash',
    models: [
      { label: '3.5 Flash-Lite', id: 'gemini-3.5-flash-lite' },
      { label: '3.8 Flash',      id: 'gemini-3.8-flash', isDefault: true },
      { label: '3.1 Pro',        id: 'gemini-3.1-pro-preview' },
      { label: '4 Argon',        id: 'gemini-4-argon' },
    ],
    tiers: {
      Fast: 'gemini-3.5-flash-lite',
      Pro: 'gemini-3.8-flash',
      Thinking: 'gemini-3.1-pro-preview',
    },
  },
  chatgpt: {
    id: 'chatgpt',
    name: 'ChatGPT',
    defaultModel: 'gpt-4o',
    models: [
      { label: 'GPT-4o Mini',    id: 'gpt-4o-mini' },
      { label: 'GPT-4o',         id: 'gpt-4o', isDefault: true },
      { label: 'o3-mini',        id: 'o3-mini' },
      { label: 'o1',             id: 'o1' },
    ],
    tiers: {
      Fast: 'gpt-4o-mini',
      Pro: 'gpt-4o',
      Thinking: 'o3-mini',
    },
  },
  claude: {
    id: 'claude',
    name: 'Claude',
    defaultModel: 'claude-3-7-sonnet-latest',
    models: [
      { label: 'Claude 3.5 Haiku',  id: 'claude-3-5-haiku-latest' },
      { label: 'Claude 3.7 Sonnet', id: 'claude-3-7-sonnet-latest', isDefault: true },
      { label: 'Claude 3.5 Sonnet', id: 'claude-3-5-sonnet-latest' },
      { label: 'Claude 3 Opus',     id: 'claude-3-opus-latest' },
    ],
    tiers: {
      Fast: 'claude-3-5-haiku-latest',
      Pro: 'claude-3-7-sonnet-latest',
      Thinking: 'claude-3-7-sonnet-latest',
    },
  },
};

function normalizeProviderId(raw) {
  const s = String(raw || '').toLowerCase().trim();
  if (s === 'openai' || s === 'chatgpt') return 'chatgpt';
  if (s === 'claude' || s === 'anthropic') return 'claude';
  if (s === 'gemini' || s === 'google') return 'gemini';
  return '';
}

function normalizeThinking(raw) {
  const s = String(raw || '').toLowerCase().trim();
  if (s === 'fast' || s === 'low') return 'Fast';
  if (s === 'thinking' || s === 'high') return 'Thinking';
  return 'Balanced';
}

function normalizeTier(raw) {
  const s = String(raw || '').toLowerCase().trim();
  if (s === 'fast' || s === 'low') return 'Fast';
  if (s === 'thinking' || s === 'high') return 'Thinking';
  if (s === 'pro' || s === 'balanced' || s === 'medium') return 'Balanced';
  return 'Balanced';
}

const SYSTEM_PROMPT =
  'You are MacAI, a helpful, concise AI assistant running on a classic 1984-1991 Apple Macintosh (68k). ' +
  'Keep answers clear, direct, and conversational (typically 1 to 4 short sentences unless asked for more detail). ' +
  'Use plain text without markdown tables or complex formatting.';

const store = createModuleStore('macai', {
  activeProvider: 'gemini',
  activeTier: 'Balanced',
  activeThinking: 'Balanced',
  activeModels: {
    gemini: 'gemini-3.8-flash',
    chatgpt: 'gpt-4o',
    claude: 'claude-3-7-sonnet-latest',
  },
  customModels: {
    gemini: '',
    chatgpt: '',
    claude: '',
  },
  discoveredModels: {
    gemini: [],
    chatgpt: [],
    claude: [],
  },
  geminiApiKey: null,
  googleClientId: null,
  googleClientSecret: null,
  googleAccessToken: null,
  googleRefreshToken: null,
  googleExpiresAt: null,
  googleOauthState: null,
  openaiApiKey: null,
  anthropicApiKey: null,
  ttsEnabled: true,
  chats: [],
  lastError: null,
});

let customFetch = null;
function setFetchImpl(fn) {
  customFetch = fn;
}
function getFetchImpl() {
  return customFetch || fetch;
}

function getProviderAuth(data = store.load()) {
  const geminiKey = (process.env.GEMINI_API_KEY || data.geminiApiKey || '').trim();
  const geminiOauth = Boolean(data.googleAccessToken || data.googleRefreshToken);
  const openaiKey = (process.env.OPENAI_API_KEY || data.openaiApiKey || '').trim();
  const anthropicKey = (process.env.ANTHROPIC_API_KEY || data.anthropicApiKey || '').trim();

  return {
    gemini: {
      enabled: Boolean(geminiKey || geminiOauth),
      mode: geminiOauth ? 'oauth' : (geminiKey ? 'apikey' : 'none'),
      apiKey: geminiKey,
    },
    chatgpt: {
      enabled: Boolean(openaiKey),
      mode: openaiKey ? 'apikey' : 'none',
      apiKey: openaiKey,
    },
    claude: {
      enabled: Boolean(anthropicKey),
      mode: anthropicKey ? 'apikey' : 'none',
      apiKey: anthropicKey,
    },
  };
}

function getEnabledProviderIds(data = store.load()) {
  const auth = getProviderAuth(data);
  return Object.keys(PROVIDERS).filter((id) => auth[id] && auth[id].enabled);
}

function resolveActiveProviderAndTier(data = store.load()) {
  const enabled = getEnabledProviderIds(data);
  const normProv = normalizeProviderId(data.activeProvider);
  let provider = normProv && PROVIDERS[normProv] ? normProv : 'gemini';
  if (enabled.length > 0 && !enabled.includes(provider)) {
    provider = enabled[0];
  }
  const tier = normalizeTier(data.activeThinking || data.activeTier) || 'Balanced';
  return { provider, tier, thinking: tier, enabled };
}

function formatRelativeAge(updatedAtMs) {
  const diffMs = Math.max(0, Date.now() - (Number(updatedAtMs) || Date.now()));
  const days = Math.floor(diffMs / (24 * 60 * 60 * 1000));
  if (days <= 0) return 'Today';
  if (days < 30) return `${days}d`;
  const months = Math.floor(days / 30);
  return `${months}mo`;
}

function generateChatTitle(promptText) {
  const cleaned = String(promptText || '')
    .replace(/[?!.,;:"'()[\]{}]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim();
  if (!cleaned) return 'New conversation';
  const words = cleaned.split(' ');
  const stopWords = new Set(['did', 'do', 'does', 'can', 'could', 'would', 'what', 'is', 'are', 'the', 'a', 'an', 'how', 'to', 'please', 'tell', 'me', 'about']);
  const meaningful = words.filter((w, idx) => idx === 0 || !stopWords.has(w.toLowerCase()));
  const selected = (meaningful.length >= 2 ? meaningful : words).slice(0, 4).join(' ');
  const capitalized = selected.charAt(0).toUpperCase() + selected.slice(1);
  return capitalized.length > 28 ? capitalized.slice(0, 28).trim() : capitalized;
}

async function getValidGoogleAccessToken(data = store.load()) {
  if (data.googleAccessToken && data.googleExpiresAt && Date.now() < data.googleExpiresAt - 60000) {
    return data.googleAccessToken;
  }
  if (!data.googleRefreshToken || !data.googleClientId || !data.googleClientSecret) {
    return data.googleAccessToken || null;
  }
  const fetchFn = getFetchImpl();
  const res = await fetchFn('https://oauth2.googleapis.com/token', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams({
      client_id: data.googleClientId,
      client_secret: data.googleClientSecret,
      refresh_token: data.googleRefreshToken,
      grant_type: 'refresh_token',
    }).toString(),
  });
  if (!res.ok) {
    throw new Error(`Google OAuth token refresh failed (${res.status})`);
  }
  const json = await res.json();
  const expiresAt = Date.now() + (Number(json.expires_in) || 3600) * 1000;
  store.save({
    googleAccessToken: json.access_token,
    googleExpiresAt: expiresAt,
  });
  return json.access_token;
}

function extractApiErrorMessage(errText, maxLen = 140) {
  if (!errText) return 'Unknown API error';
  try {
    const parsed = JSON.parse(errText);
    if (parsed && parsed.error && typeof parsed.error.message === 'string') {
      return parsed.error.message.trim().slice(0, maxLen);
    }
    if (parsed && typeof parsed.message === 'string') {
      return parsed.message.trim().slice(0, maxLen);
    }
  } catch (_) {}
  return String(errText).replace(/\s+/g, ' ').trim().slice(0, maxLen);
}

/**
 * Calls Google Gemini API (`generateContent`) using either OAuth Bearer token or API Key.
 */
async function callGeminiChat(messages, model = 'gemini-3.8-flash', thinking = 'Balanced', data = store.load(), signal = undefined) {
  const auth = getProviderAuth(data).gemini;
  const fetchFn = getFetchImpl();
  const targetModel = model || PROVIDERS.gemini.defaultModel;

  const contents = messages.map((m) => ({
    role: m.role === 'assistant' ? 'model' : 'user',
    parts: [{ text: m.content }],
  }));

  const headers = { 'Content-Type': 'application/json' };
  let querySuffix = '';
  if (auth.mode === 'oauth') {
    const token = await getValidGoogleAccessToken(data);
    headers.Authorization = `Bearer ${token}`;
  } else if (auth.apiKey) {
    querySuffix = `?key=${encodeURIComponent(auth.apiKey)}`;
  } else {
    throw new Error('Gemini is not configured');
  }

  const cleanThinking = normalizeThinking(thinking);
  const url = `https://generativelanguage.googleapis.com/v1beta/models/${targetModel}:generateContent${querySuffix}`;
  const isGemini3Or4 = /^gemini-[34]/i.test(targetModel);
  const thinkingLevel = cleanThinking === 'Fast' ? 'LOW' : (cleanThinking === 'Thinking' ? 'HIGH' : 'MEDIUM');

  const buildBody = (withThinkingConfig) => {
    const genCfg = {
      temperature: cleanThinking === 'Fast' ? 0.7 : (cleanThinking === 'Thinking' ? 0.3 : 0.5),
      maxOutputTokens: 2048,
    };
    if (withThinkingConfig && isGemini3Or4) {
      genCfg.thinkingConfig = { thinkingLevel };
    }
    return {
      systemInstruction: { parts: [{ text: SYSTEM_PROMPT }] },
      contents,
      generationConfig: genCfg,
    };
  };

  let res = await fetchFn(url, {
    method: 'POST',
    headers,
    body: JSON.stringify(buildBody(true)),
    ...(signal ? { signal } : {}),
  });

  if (res.status === 400 && isGemini3Or4) {
    res = await fetchFn(url, {
      method: 'POST',
      headers,
      body: JSON.stringify(buildBody(false)),
      ...(signal ? { signal } : {}),
    });
  }

  if (!res.ok) {
    const errText = await res.text().catch(() => '');
    throw new Error(`Gemini error (${res.status}): ${extractApiErrorMessage(errText)}`);
  }

  const json = await res.json();
  const parts =
    json &&
    json.candidates &&
    json.candidates[0] &&
    json.candidates[0].content &&
    Array.isArray(json.candidates[0].content.parts)
      ? json.candidates[0].content.parts
      : [];
  const nonThoughtText = parts
    .filter((p) => !p.thought)
    .map((p) => p.text || '')
    .join('')
    .trim();
  const text = nonThoughtText || parts.map((p) => p.text || '').join('').trim();
  if (!text) throw new Error('Gemini returned an empty response');
  return text;
}

/**
 * Calls OpenAI Chat Completions API (`ChatGPT`).
 */
async function callOpenAIChat(messages, model = 'gpt-4o', thinking = 'Balanced', data = store.load(), signal = undefined) {
  const auth = getProviderAuth(data).chatgpt;
  if (!auth.apiKey) throw new Error('ChatGPT API key is not configured');
  const fetchFn = getFetchImpl();
  const targetModel = model || PROVIDERS.chatgpt.defaultModel;

  const isReasoning = targetModel.startsWith('o1') || targetModel.startsWith('o3') || targetModel.startsWith('o4');
  const cleanThinking = normalizeThinking(thinking);

  const apiMessages = [
    { role: isReasoning ? 'developer' : 'system', content: SYSTEM_PROMPT },
    ...messages.map((m) => ({
      role: m.role === 'assistant' ? 'assistant' : 'user',
      content: m.content,
    })),
  ];
  const body = {
    model: targetModel,
    messages: apiMessages,
  };
  if (isReasoning) {
    body.reasoning_effort = cleanThinking === 'Fast' ? 'low' : (cleanThinking === 'Thinking' ? 'high' : 'medium');
  } else {
    body.max_tokens = 1024;
    body.temperature = cleanThinking === 'Fast' ? 0.7 : (cleanThinking === 'Thinking' ? 0.3 : 0.5);
  }

  const res = await fetchFn('https://api.openai.com/v1/chat/completions', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      Authorization: `Bearer ${auth.apiKey}`,
    },
    body: JSON.stringify(body),
    ...(signal ? { signal } : {}),
  });

  if (!res.ok) {
    const errText = await res.text().catch(() => '');
    throw new Error(`ChatGPT error (${res.status}): ${extractApiErrorMessage(errText)}`);
  }

  const json = await res.json();
  const text =
    json &&
    json.choices &&
    json.choices[0] &&
    json.choices[0].message &&
    json.choices[0].message.content
      ? String(json.choices[0].message.content).trim()
      : '';
  if (!text) throw new Error('ChatGPT returned an empty response');
  return text;
}

/**
 * Calls Anthropic Messages API (`Claude`).
 */
async function callClaudeChat(messages, model = 'claude-3-7-sonnet-latest', thinking = 'Balanced', data = store.load(), signal = undefined) {
  const auth = getProviderAuth(data).claude;
  if (!auth.apiKey) throw new Error('Claude API key is not configured');
  const fetchFn = getFetchImpl();
  const targetModel = model || PROVIDERS.claude.defaultModel;

  const apiMessages = messages.map((m) => ({
    role: m.role === 'assistant' ? 'assistant' : 'user',
    content: m.content,
  }));

  const cleanThinking = normalizeThinking(thinking);
  const body = {
    model: targetModel,
    max_tokens: 1024,
    system: SYSTEM_PROMPT,
    messages: apiMessages,
  };

  if (targetModel.includes('3-7-sonnet')) {
    if (cleanThinking === 'Thinking') {
      body.thinking = { type: 'enabled', budget_tokens: 16384 };
      body.max_tokens = 20000;
    } else if (cleanThinking === 'Balanced') {
      body.thinking = { type: 'enabled', budget_tokens: 4096 };
      body.max_tokens = 8000;
    }
  }

  const res = await fetchFn('https://api.anthropic.com/v1/messages', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'x-api-key': auth.apiKey,
      'anthropic-version': '2023-06-01',
    },
    body: JSON.stringify(body),
    ...(signal ? { signal } : {}),
  });

  if (!res.ok) {
    const errText = await res.text().catch(() => '');
    throw new Error(`Claude error (${res.status}): ${extractApiErrorMessage(errText)}`);
  }

  const json = await res.json();
  const text =
    json && Array.isArray(json.content)
      ? json.content
          .filter((c) => c.type === 'text')
          .map((c) => c.text || '')
          .join('')
          .trim()
      : '';
  if (!text) throw new Error('Claude returned an empty response');
  return text;
}

async function generateProviderReply(provider, model, thinking, messages, data = store.load(), signal = undefined) {
  if (provider === 'chatgpt') {
    return callOpenAIChat(messages, model, thinking, data, signal);
  }
  if (provider === 'claude') {
    return callClaudeChat(messages, model, thinking, data, signal);
  }
  return callGeminiChat(messages, model, thinking, data, signal);
}

/**
 * Transcribes a 16-bit WAV audio buffer using Gemini Multimodal Audio or OpenAI Whisper.
 */
async function transcribeWavBuffer(wavBuffer, data = store.load()) {
  const auth = getProviderAuth(data);
  const fetchFn = getFetchImpl();

  if (auth.gemini.enabled) {
    const headers = { 'Content-Type': 'application/json' };
    let querySuffix = '';
    if (auth.gemini.mode === 'oauth') {
      const token = await getValidGoogleAccessToken(data);
      headers.Authorization = `Bearer ${token}`;
    } else {
      querySuffix = `?key=${encodeURIComponent(auth.gemini.apiKey)}`;
    }
    for (const sttModel of ['gemini-3.5-flash', 'gemini-3.5-flash-lite', 'gemini-3.6-flash', 'gemini-2.5-flash']) {
      const url = `https://generativelanguage.googleapis.com/v1beta/models/${sttModel}:generateContent${querySuffix}`;
      const res = await fetchFn(url, {
        method: 'POST',
        headers,
        body: JSON.stringify({
          contents: [
            {
              role: 'user',
              parts: [
                { text: 'Transcribe the spoken question or statement in this audio accurately. Return ONLY the transcription text and nothing else.' },
                {
                  inlineData: {
                    mimeType: 'audio/wav',
                    data: wavBuffer.toString('base64'),
                  },
                },
              ],
            },
          ],
        }),
      });
      if (res.ok) {
        const json = await res.json();
        const parts =
          json &&
          json.candidates &&
          json.candidates[0] &&
          json.candidates[0].content &&
          Array.isArray(json.candidates[0].content.parts)
            ? json.candidates[0].content.parts
            : [];
        const text = parts.filter((p) => !p.thought).map((p) => p.text || '').join('').trim();
        if (text) return text;
      }
    }
  }

  if (auth.chatgpt.enabled) {
    const boundary = '----MacAIVoiceBoundary' + Date.now();
    const parts = [
      Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="model"\r\n\r\nwhisper-1\r\n`),
      Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="file"; filename="speech.wav"\r\nContent-Type: audio/wav\r\n\r\n`),
      wavBuffer,
      Buffer.from(`\r\n--${boundary}--\r\n`),
    ];
    const res = await fetchFn('https://api.openai.com/v1/audio/transcriptions', {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${auth.chatgpt.apiKey}`,
        'Content-Type': `multipart/form-data; boundary=${boundary}`,
      },
      body: Buffer.concat(parts),
    });
    if (res.ok) {
      const json = await res.json();
      if (json && json.text) return String(json.text).trim();
    }
  }

  throw new Error('Voice transcription requires Gemini or OpenAI to be configured');
}

/**
 * Synthesizes spoken TTS audio into unsigned 8-bit 22,255 Hz mono PCM (`pcm_u8`)
 * using OpenAI TTS (if configured) or local OS speech synthesis (`say` / `espeak-ng`) + FFmpeg.
 */
async function synthesizeSpeechToPcmU8(text, { hz = 22255, data = store.load() } = {}) {
  const cleanText = String(text || '').replace(/\s+/g, ' ').trim().slice(0, 600);
  if (!cleanText) return Buffer.alloc(0);

  const auth = getProviderAuth(data);
  const fetchFn = getFetchImpl();

  // 1. If OpenAI key is configured, use OpenAI's natural alloy/nova TTS
  if (auth.chatgpt.enabled && !customFetch) {
    try {
      const res = await fetchFn('https://api.openai.com/v1/audio/speech', {
        method: 'POST',
        headers: {
          Authorization: `Bearer ${auth.chatgpt.apiKey}`,
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          model: 'tts-1',
          voice: 'alloy',
          input: cleanText,
          response_format: 'mp3',
        }),
      });
      if (res.ok) {
        const mp3Buf = Buffer.from(await res.arrayBuffer());
        return await audioHub.transcodeToPcmU8(mp3Buf, { hz });
      }
    } catch {}
  }

  // 2. Fallback: local `espeak-ng` / `espeak` (Linux/Alpine Docker) or `say` (macOS) -> stdout WAV -> FFmpeg
  return new Promise((resolve) => {
    const tryCommand = (cmd, args) => {
      try {
        const proc = spawn(cmd, args, { stdio: ['ignore', 'pipe', 'ignore'] });
        const chunks = [];
        proc.stdout.on('data', (c) => chunks.push(c));
        proc.on('error', () => resolve(Buffer.alloc(0)));
        proc.on('close', async (code) => {
          if (code === 0 && chunks.length > 0) {
            try {
              const u8 = await audioHub.transcodeToPcmU8(Buffer.concat(chunks), { hz });
              resolve(u8);
            } catch {
              resolve(Buffer.alloc(0));
            }
          } else {
            resolve(Buffer.alloc(0));
          }
        });
      } catch {
        resolve(Buffer.alloc(0));
      }
    };

    if (process.platform === 'darwin') {
      tryCommand('say', ['--data-format=LEI16@22050', '-o', '-', cleanText]);
    } else {
      tryCommand('espeak-ng', ['--stdout', cleanText]);
    }
  });
}

// ============================================================================
// ROUTES: 68k Client Pipe-Delimited Endpoints & Web UI JSON Endpoints
// ============================================================================

router.get('/ping', (req, res) => {
  const data = store.load();
  const { provider, tier, enabled } = resolveActiveProviderAndTier(data);
  if (enabled.length === 0) {
    return sendPipeResponse(
      req,
      res,
      formatPipeRow(['MACAI', '0', 'AWAITING_LOGIN', 'You have not logged in to any AI services.']) + '\n'
    );
  }
  return sendPipeResponse(
    req,
    res,
    formatPipeRow(['MACAI', '1', 'READY', provider, tier, enabled.join(',')]) + '\n'
  );
});

router.get('/status', (req, res) => {
  res.json(getStatus());
});

router.post('/config', (req, res) => {
  const data = store.load();
  const body = req.body || {};
  const updates = {};

  if (body.geminiApiKey !== undefined) updates.geminiApiKey = String(body.geminiApiKey || '').trim() || null;
  if (body.googleClientId !== undefined) updates.googleClientId = String(body.googleClientId || '').trim() || null;
  if (body.googleClientSecret !== undefined) updates.googleClientSecret = String(body.googleClientSecret || '').trim() || null;
  if (body.openaiApiKey !== undefined) updates.openaiApiKey = String(body.openaiApiKey || '').trim() || null;
  if (body.anthropicApiKey !== undefined) updates.anthropicApiKey = String(body.anthropicApiKey || '').trim() || null;
  if (body.activeProvider && PROVIDERS[body.activeProvider]) updates.activeProvider = body.activeProvider;
  if (body.activeTier && ['Fast', 'Pro', 'Thinking'].includes(body.activeTier)) updates.activeTier = body.activeTier;
  if (body.ttsEnabled !== undefined) updates.ttsEnabled = Boolean(body.ttsEnabled);

  if (body.disconnectProvider === 'gemini') {
    updates.geminiApiKey = null;
    updates.googleAccessToken = null;
    updates.googleRefreshToken = null;
    updates.googleExpiresAt = null;
  } else if (body.disconnectProvider === 'chatgpt') {
    updates.openaiApiKey = null;
  } else if (body.disconnectProvider === 'claude') {
    updates.anthropicApiKey = null;
  }

  if (body.clearChats || body.deleteChats) {
    updates.chats = [];
  }

  if (body.customModels && typeof body.customModels === 'object') {
    const prev = data.customModels || { gemini: '', chatgpt: '', claude: '' };
    updates.customModels = {
      gemini: body.customModels.gemini !== undefined ? String(body.customModels.gemini || '').trim() : prev.gemini,
      chatgpt: body.customModels.chatgpt !== undefined ? String(body.customModels.chatgpt || '').trim() : prev.chatgpt,
      claude: body.customModels.claude !== undefined ? String(body.customModels.claude || '').trim() : prev.claude,
    };
  }
  if (body.customModelGemini !== undefined) {
    updates.customModels = updates.customModels || { ...(data.customModels || {}) };
    updates.customModels.gemini = String(body.customModelGemini || '').trim();
  }
  if (body.customModelChatgpt !== undefined) {
    updates.customModels = updates.customModels || { ...(data.customModels || {}) };
    updates.customModels.chatgpt = String(body.customModelChatgpt || '').trim();
  }
  if (body.customModelClaude !== undefined) {
    updates.customModels = updates.customModels || { ...(data.customModels || {}) };
    updates.customModels.claude = String(body.customModelClaude || '').trim();
  }

  store.save(updates);
  res.json({ ok: true, status: getStatus() });
});

router.post('/fetch-models', async (req, res) => {
  try {
    const data = store.load();
    const auth = getProviderAuth(data);
    const targetProvider = normalizeProviderId(req.body && req.body.provider);
    const providersToFetch = targetProvider ? [targetProvider] : ['gemini', 'chatgpt', 'claude'];
    const results = {};

    for (const p of providersToFetch) {
      if (!auth[p] || !auth[p].enabled) {
        results[p] = { ok: false, error: 'Provider not configured or missing API key' };
        continue;
      }
      try {
        if (p === 'gemini') {
          const fetchFn = getFetchImpl();
          const querySuffix = auth.gemini.apiKey ? `?key=${encodeURIComponent(auth.gemini.apiKey)}` : '';
          const headers = {};
          if (auth.gemini.mode === 'oauth') {
            const token = await getValidGoogleAccessToken(data);
            headers.Authorization = `Bearer ${token}`;
          }
          const resp = await fetchFn(`https://generativelanguage.googleapis.com/v1beta/models${querySuffix}`, { headers });
          if (!resp.ok) {
            results[p] = { ok: false, error: `Google API HTTP ${resp.status}` };
          } else {
            const json = await resp.json();
            const models = (json.models || []).map((m) => m.name.replace(/^models\//, ''));
            results[p] = { ok: true, models };
          }
        } else if (p === 'chatgpt') {
          const fetchFn = getFetchImpl();
          const resp = await fetchFn('https://api.openai.com/v1/models', {
            headers: { Authorization: `Bearer ${auth.chatgpt.apiKey}` },
          });
          if (!resp.ok) {
            results[p] = { ok: false, error: `OpenAI API HTTP ${resp.status}` };
          } else {
            const json = await resp.json();
            const models = (json.data || []).map((m) => m.id);
            results[p] = { ok: true, models };
          }
        } else if (p === 'claude') {
          const fetchFn = getFetchImpl();
          const resp = await fetchFn('https://api.anthropic.com/v1/models', {
            headers: {
              'x-api-key': auth.claude.apiKey,
              'anthropic-version': '2023-06-01',
            },
          });
          if (!resp.ok) {
            results[p] = { ok: false, error: `Anthropic API HTTP ${resp.status}` };
          } else {
            const json = await resp.json();
            const models = (json.data || []).map((m) => m.id);
            results[p] = { ok: true, models };
          }
        }
      } catch (err) {
        results[p] = { ok: false, error: err.message };
      }
    }

    const discovered = data.discoveredModels || { gemini: [], chatgpt: [], claude: [] };
    for (const [p, r] of Object.entries(results)) {
      if (r.ok && Array.isArray(r.models)) {
        discovered[p] = r.models;
      }
    }
    store.save({ discoveredModels: discovered });

    return res.json({ ok: true, results, discoveredModels: discovered });
  } catch (e) {
    return res.status(500).json({ ok: false, error: e.message });
  }
});

router.get('/oauth/google/start', (req, res) => {
  const data = store.load();
  const clientId = (req.query.client_id || data.googleClientId || '').trim();
  const clientSecret = (req.query.client_secret || data.googleClientSecret || '').trim();
  if (!clientId || !clientSecret) {
    return res.status(400).send('Missing Google OAuth Client ID or Client Secret.');
  }
  const state = crypto.randomBytes(16).toString('hex');
  const redirectUri = `${req.protocol}://${req.get('host')}/api/macai/oauth/google/callback`;
  store.save({
    googleClientId: clientId,
    googleClientSecret: clientSecret,
    googleOauthState: state,
  });

  const params = new URLSearchParams({
    client_id: clientId,
    redirect_uri: redirectUri,
    response_type: 'code',
    scope: 'https://www.googleapis.com/auth/generative-language.retriever openid email',
    access_type: 'offline',
    prompt: 'consent',
    state,
  });
  res.redirect(`https://accounts.google.com/o/oauth2/v2/auth?${params.toString()}`);
});

router.get('/oauth/google/callback', async (req, res) => {
  try {
    const { code } = req.query;
    if (!code) throw new Error('Missing authorization code from Google');
    const data = store.load();
    const redirectUri = `${req.protocol}://${req.get('host')}/api/macai/oauth/google/callback`;
    const fetchFn = getFetchImpl();
    const tokenRes = await fetchFn('https://oauth2.googleapis.com/token', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: new URLSearchParams({
        code: String(code),
        client_id: data.googleClientId || '',
        client_secret: data.googleClientSecret || '',
        redirect_uri: redirectUri,
        grant_type: 'authorization_code',
      }).toString(),
    });
    if (!tokenRes.ok) {
      const errText = await tokenRes.text();
      throw new Error(`Google token exchange failed: ${errText}`);
    }
    const tokens = await tokenRes.json();
    store.save({
      googleAccessToken: tokens.access_token,
      googleRefreshToken: tokens.refresh_token || data.googleRefreshToken,
      googleExpiresAt: Date.now() + (Number(tokens.expires_in) || 3600) * 1000,
      activeProvider: 'gemini',
    });
    res.redirect('/?macai_auth=success');
  } catch (err) {
    store.save({ lastError: err.message });
    res.redirect(`/?macai_error=${encodeURIComponent(err.message)}`);
  }
});

router.get('/providers', (req, res) => {
  const data = store.load();
  const auth = getProviderAuth(data);
  const { provider, tier } = resolveActiveProviderAndTier(data);
  const customModels = data.customModels || {};
  const activeModels = data.activeModels || {};
  const rows = [
    ['ACTIVE', provider, PROVIDERS[provider].name, tier, activeModels[provider] || PROVIDERS[provider].defaultModel],
  ];
  for (const p of Object.values(PROVIDERS)) {
    rows.push([
      'PROVIDER',
      p.id,
      p.name,
      auth[p.id] && auth[p.id].enabled ? '1' : '0',
      'fast,pro,thinking',
      customModels[p.id] || '',
    ]);
  }
  return sendPipeResponse(req, res, formatPipeTable(rows));
});

router.all('/select', (req, res) => {
  const data = store.load();
  const auth = getProviderAuth(data);
  const reqProvider = normalizeProviderId(req.query.provider || (req.body && req.body.provider));
  const reqTier = normalizeTier(req.query.tier || (req.body && req.body.tier));

  const updates = {};
  if (reqProvider && PROVIDERS[reqProvider] && auth[reqProvider] && auth[reqProvider].enabled) {
    updates.activeProvider = reqProvider;
  }
  if (reqTier) {
    updates.activeTier = reqTier;
  }
  const saved = store.save(updates);
  const { provider, tier } = resolveActiveProviderAndTier(saved);
  return sendPipeResponse(req, res, formatPipeRow(['OK', 'SELECT', provider, PROVIDERS[provider].name, tier]) + '\n');
});

router.delete('/chats', (req, res) => {
  store.save({ chats: [] });
  return res.json({ ok: true, message: 'All chat history deleted', chatCount: 0 });
});

router.post('/chats/clear', (req, res) => {
  store.save({ chats: [] });
  return res.json({ ok: true, message: 'All chat history deleted', chatCount: 0 });
});

router.all('/clear-chats', (req, res) => {
  store.save({ chats: [] });
  if (req.accepts(['json', 'text']) === 'json' || req.xhr || (req.headers && req.headers['content-type'] === 'application/json')) {
    return res.json({ ok: true, message: 'All chat history deleted', chatCount: 0 });
  }
  return sendPipeResponse(req, res, formatPipeRow(['OK', 'CLEARED_CHATS']) + '\n');
});

router.get('/chats', (req, res) => {
  const data = store.load();
  const limit = Math.min(48, Math.max(1, parseInt(req.query.limit || '30', 10) || 30));
  const query = String(req.query.q || '').toLowerCase().trim();
  const chats = Array.isArray(data.chats) ? [...data.chats] : [];

  chats.sort((a, b) => (b.updatedAt || 0) - (a.updatedAt || 0));

  const filtered = query
    ? chats.filter((c) => {
        const inTitle = String(c.title || '').toLowerCase().includes(query);
        const inMsgs = Array.isArray(c.messages) && c.messages.some((m) => String(m.content || '').toLowerCase().includes(query));
        return inTitle || inMsgs;
      })
    : chats;

  const rows = filtered.slice(0, limit).map((c) => [
    'CHAT',
    c.id,
    c.title || 'New chat',
    c.provider || 'gemini',
    c.tier || 'Pro',
    formatRelativeAge(c.updatedAt),
  ]);

  return sendPipeResponse(req, res, formatPipeTable(rows));
});

router.get('/chat/:id', (req, res) => {
  const data = store.load();
  const chatId = String(req.params.id || '').trim();
  const chats = Array.isArray(data.chats) ? data.chats : [];
  const chat = chats.find((c) => c.id === chatId);
  if (!chat) {
    return res.status(404).type('text/plain').send('ERR|404|Chat not found\n');
  }

  const rows = [
    ['META', chat.id, chat.title || 'New chat', chat.provider || 'gemini', chat.tier || 'Pro', formatRelativeAge(chat.updatedAt)],
  ];
  for (const m of chat.messages || []) {
    rows.push(['MSG', m.role === 'assistant' ? 'assistant' : 'user', m.content || '']);
  }
  return sendPipeResponse(req, res, formatPipeTable(rows));
});

router.all('/new-chat', (req, res) => {
  const data = store.load();
  const { provider, tier } = resolveActiveProviderAndTier(data);
  const newId = 'chat_' + Date.now().toString(36) + '_' + Math.random().toString(36).slice(2, 6);
  return sendPipeResponse(req, res, formatPipeRow(['OK', 'NEW_CHAT', newId, provider, tier]) + '\n');
});

router.all('/ask', async (req, res) => {
  let timeoutId = null;
  let healthCheckId = null;
  const controller = new AbortController();
  let abortReason = null;

  try {
    const data = store.load();
    const { provider: defaultProvider, tier: defaultTier, enabled } = resolveActiveProviderAndTier(data);
    if (enabled.length === 0) {
      return res.status(401).type('text/plain').send('ERR|401|No AI service logged in\n');
    }

    const prompt = String(req.query.q || (req.body && req.body.q) || '').trim();
    if (!prompt) {
      return res.status(400).type('text/plain').send('ERR|400|Empty prompt\n');
    }

    const reqProv = normalizeProviderId(req.query.provider || (req.body && req.body.provider)) || defaultProvider;
    const provider = enabled.includes(reqProv) ? reqProv : defaultProvider;
    const reqThinking = normalizeThinking(req.query.thinking || req.query.tier || (req.body && (req.body.thinking || req.body.tier)));
    const thinking = reqThinking || defaultTier;
    const reqModel = String(req.query.model || (req.body && req.body.model) || '').trim();
    const model = reqModel || (data.activeModels && data.activeModels[provider]) || PROVIDERS[provider].defaultModel;
    const wantTts = req.query.tts === '1' || (req.body && req.body.tts === true);
    const isRetry = req.query.retry === '1' || (req.body && req.body.retry === true);

    let chatId = String(req.query.chat_id || (req.body && req.body.chat_id) || '').trim();
    const chats = Array.isArray(data.chats) ? [...data.chats] : [];
    let chat = chats.find((c) => c.id === chatId);

    if (!chat) {
      chatId = chatId || ('chat_' + Date.now().toString(36) + '_' + Math.random().toString(36).slice(2, 6));
      chat = {
        id: chatId,
        title: generateChatTitle(prompt),
        provider,
        tier: thinking,
        model,
        createdAt: Date.now(),
        updatedAt: Date.now(),
        messages: [],
      };
      chats.unshift(chat);
    }

    chat.provider = provider;
    chat.tier = thinking;
    chat.model = model;
    if (isRetry && Array.isArray(chat.messages) && chat.messages.length > 0) {
      if (chat.messages[chat.messages.length - 1].role === 'assistant') {
        chat.messages.pop();
      }
      const lastMsg = chat.messages[chat.messages.length - 1];
      if (!lastMsg || lastMsg.role !== 'user' || lastMsg.content !== prompt) {
        chat.messages.push({ role: 'user', content: prompt, ts: Date.now() });
      }
    } else {
      chat.messages.push({ role: 'user', content: prompt, ts: Date.now() });
    }

    /* 45-second hard timeout */
    timeoutId = setTimeout(() => {
      abortReason = 'Request timed out after 45s without response';
      controller.abort();
    }, 45000);

    const replyText = await generateProviderReply(
      provider,
      model,
      thinking,
      chat.messages.slice(-12),
      data,
      controller.signal
    );

    if (timeoutId) clearTimeout(timeoutId);
    if (healthCheckId) clearTimeout(healthCheckId);

    chat.messages.push({ role: 'assistant', content: replyText, ts: Date.now() });
    chat.updatedAt = Date.now();

    const activeModels = { ...(data.activeModels || {}), [provider]: model };
    store.save({
      activeProvider: provider,
      activeTier: thinking,
      activeThinking: thinking,
      activeModels,
      chats: chats.slice(0, 60),
      lastError: null,
    });

    if (wantTts) {
      await audioHub.pauseSpotifyForVoice();
      synthesizeSpeechToPcmU8(replyText, { hz: 22255, data })
        .then((u8) => {
          if (u8 && u8.length > 0) {
            audioHub.streamPcmU8ToTcpClients(u8, { hz: 22255 });
          }
        })
        .catch(() => {});
    }

    const pipe = formatPipeRow(['REPLY', chat.id, chat.title || prompt, provider, thinking, replyText]) + '\n';
    if (req.query.format === 'json') {
      return res.json({ ok: true, chatId: chat.id, title: chat.title, provider, model, tier: thinking, reply: replyText });
    }
    return sendPipeResponse(req, res, pipe);
  } catch (err) {
    if (timeoutId) clearTimeout(timeoutId);
    if (healthCheckId) clearTimeout(healthCheckId);
    const errMsg = abortReason || err.message || 'AI request failed';
    store.save({ lastError: errMsg });
    return sendPipeResponse(req, res, `ERR|500|${sanitizeField(errMsg)}\n`);
  }
});

/**
 * POST/GET /api/macai/voice
 * Full two-way voice endpoint:
 *   1. Pauses SpotiMac so audio doesn't collide
 *   2. Accepts 8-bit unsigned mono PCM audio (via hex query/body or raw binary body)
 *   3. Transcribes speech -> sends to LLM -> synthesizes TTS -> streams 8-bit PCM over TCP 6802
 */
router.all(
  '/voice',
  express.raw({ type: ['application/octet-stream', 'audio/*'], limit: '2mb' }),
  async (req, res) => {
    try {
      await audioHub.pauseSpotifyForVoice();

      const data = store.load();
      const { provider, tier, enabled } = resolveActiveProviderAndTier(data);
      if (enabled.length === 0) {
        return res.status(401).type('text/plain').send('ERR|401|No AI service logged in\n');
      }

      const inputRate = parseInt(req.query.rate || '11127', 10) || 11127;
      let pcmU8 = Buffer.alloc(0);
      if (Buffer.isBuffer(req.body) && req.body.length > 0) {
        pcmU8 = req.body;
      } else if (req.query.hex) {
        pcmU8 = Buffer.from(String(req.query.hex), 'hex');
      } else if (req.body && req.body.hex) {
        pcmU8 = Buffer.from(String(req.body.hex), 'hex');
      }

      let transcript = String(req.query.q || '').trim();
      if (!transcript && pcmU8.length > 100) {
        const wavBuf = audioHub.pcmU8ToWav16k(pcmU8, inputRate);
        transcript = await transcribeWavBuffer(wavBuf, data);
      }
      if (!transcript) {
        return res.status(400).type('text/plain').send('ERR|400|Could not hear speech\n');
      }

      let chatId = String(req.query.chat_id || '').trim();
      const chats = Array.isArray(data.chats) ? [...data.chats] : [];
      let chat = chats.find((c) => c.id === chatId);
      if (!chat) {
        chatId = chatId || ('chat_' + Date.now().toString(36) + '_' + Math.random().toString(36).slice(2, 6));
        chat = {
          id: chatId,
          title: generateChatTitle(transcript),
          provider,
          tier,
          createdAt: Date.now(),
          updatedAt: Date.now(),
          messages: [],
        };
        chats.unshift(chat);
      }

      chat.messages.push({ role: 'user', content: transcript, ts: Date.now() });
      const activeMod = (data.activeModels && data.activeModels[provider]) || PROVIDERS[provider].defaultModel;
      const replyText = await generateProviderReply(provider, activeMod, tier, chat.messages.slice(-12), data);
      chat.messages.push({ role: 'assistant', content: replyText, ts: Date.now() });
      chat.updatedAt = Date.now();

      store.save({ chats: chats.slice(0, 60) });

      let durationMs = 0;
      const u8Speech = await synthesizeSpeechToPcmU8(replyText, { hz: 22255, data });
      if (u8Speech && u8Speech.length > 0) {
        const streamed = audioHub.streamPcmU8ToTcpClients(u8Speech, { hz: 22255 });
        durationMs = streamed.durationMs;
      }

      const pipe =
        formatPipeRow(['VOICE_REPLY', chat.id, chat.title, transcript, replyText, durationMs]) + '\n';
      return sendPipeResponse(req, res, pipe);
    } catch (err) {
      return res.status(500).type('text/plain').send(`ERR|500|${sanitizeField(err.message)}\n`);
    }
  }
);

router.all('/tts', async (req, res) => {
  try {
    await audioHub.pauseSpotifyForVoice();
    const data = store.load();
    let text = String(req.query.text || (req.body && req.body.text) || '').trim();
    if (!text && req.query.chat_id) {
      const chat = (data.chats || []).find((c) => c.id === req.query.chat_id);
      if (chat && Array.isArray(chat.messages)) {
        const lastAssistant = [...chat.messages].reverse().find((m) => m.role === 'assistant');
        if (lastAssistant) text = lastAssistant.content;
      }
    }
    if (!text && Array.isArray(data.chats) && data.chats[0] && Array.isArray(data.chats[0].messages)) {
      const lastAssistant = [...data.chats[0].messages].reverse().find((m) => m.role === 'assistant');
      if (lastAssistant) text = lastAssistant.content;
    }
    if (!text) {
      return sendPipeResponse(req, res, 'ERR|400|No text to speak\n');
    }

    const u8Speech = await synthesizeSpeechToPcmU8(text, { hz: 22255, data });
    const streamed = audioHub.streamPcmU8ToTcpClients(u8Speech, { hz: 22255 });
    return sendPipeResponse(req, res, formatPipeRow(['OK', 'TTS', streamed.durationMs]) + '\n');
  } catch (err) {
    return sendPipeResponse(req, res, `ERR|500|${sanitizeField(err.message)}\n`);
  }
});

function getStatus() {
  const data = store.load();
  const auth = getProviderAuth(data);
  const { provider, tier, enabled } = resolveActiveProviderAndTier(data);

  return {
    id: 'macai',
    name: 'MacAI Chat (Gemini, ChatGPT, Claude)',
    ok: enabled.length > 0,
    state: enabled.length > 0 ? 'READY' : 'AWAITING_LOGIN',
    message:
      enabled.length > 0
        ? `Ready (${enabled.map((id) => PROVIDERS[id].name).join(', ')})`
        : 'Log in or add an API key for Gemini, ChatGPT, or Claude below',
    activeProvider: provider,
    activeTier: tier,
    activeThinking: tier,
    activeModels: data.activeModels || {},
    customModels: data.customModels || { gemini: '', chatgpt: '', claude: '' },
    discoveredModels: data.discoveredModels || { gemini: [], chatgpt: [], claude: [] },
    enabledProviders: enabled,
    providers: {
      gemini: {
        id: 'gemini',
        name: 'Google Gemini',
        enabled: auth.gemini.enabled,
        authMode: auth.gemini.mode,
        hasOAuthClient: Boolean(data.googleClientId && data.googleClientSecret),
        models: PROVIDERS.gemini.models,
        defaultModel: PROVIDERS.gemini.defaultModel,
        customModel: (data.customModels && data.customModels.gemini) || '',
      },
      chatgpt: {
        id: 'chatgpt',
        name: 'OpenAI ChatGPT',
        enabled: auth.chatgpt.enabled,
        authMode: auth.chatgpt.mode,
        models: PROVIDERS.chatgpt.models,
        defaultModel: PROVIDERS.chatgpt.defaultModel,
        customModel: (data.customModels && data.customModels.chatgpt) || '',
      },
      claude: {
        id: 'claude',
        name: 'Anthropic Claude',
        enabled: auth.claude.enabled,
        authMode: auth.claude.mode,
        models: PROVIDERS.claude.models,
        defaultModel: PROVIDERS.claude.defaultModel,
        customModel: (data.customModels && data.customModels.claude) || '',
      },
    },
    chatCount: Array.isArray(data.chats) ? data.chats.length : 0,
    lastError: data.lastError || null,
  };
}

module.exports = {
  id: 'macai',
  name: 'MacAI Chat',
  basePath: '/api/macai',
  router,
  getStatus,
  store,
  setFetchImpl,
};
