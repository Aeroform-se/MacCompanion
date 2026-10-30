const express = require('express');
const crypto = require('crypto');
const path = require('path');
const fs = require('fs');
const sharp = require('sharp');
const { spawn } = require('child_process');
const { createModuleStore } = require('../../core/storage');
const { sanitizeField, formatPipeRow, formatPipeTable, sendPipeResponse } = require('../../core/format68k');
const audioHub = require('../../core/audio-hub');
const { convertImageToPict24 } = require('../../core/pict');
const { processArtworkBuffer } = require('../../core/dither');

const DATA_DIR = process.env.DATA_DIR || path.join(__dirname, '../../../data');
const IMAGE_DIR = path.join(DATA_DIR, 'images');
if (!fs.existsSync(IMAGE_DIR)) {
  fs.mkdirSync(IMAGE_DIR, { recursive: true });
}

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
  groq: {
    id: 'groq',
    name: 'Groq',
    defaultModel: 'openai/gpt-oss-120b',
    models: [
      { label: 'GPT-OSS 20B',   id: 'openai/gpt-oss-20b' },
      { label: 'GPT-OSS 120B',  id: 'openai/gpt-oss-120b', isDefault: true },
      { label: 'Qwen 3.8 27B',  id: 'qwen/qwen3.8-27b' },
      { label: 'Allam 2 7B',    id: 'allam-2-7b' },
    ],
    tiers: {
      Fast: 'openai/gpt-oss-20b',
      Pro: 'openai/gpt-oss-120b',
      Thinking: 'qwen/qwen3.8-27b',
    },
  },
};

function normalizeProviderId(raw) {
  const s = String(raw || '').toLowerCase().trim();
  if (s === 'groq') return 'groq';
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
    groq: 'openai/gpt-oss-120b',
  },
  customModels: {
    gemini: '',
    chatgpt: '',
    claude: '',
    groq: '',
  },
  discoveredModels: {
    gemini: [],
    chatgpt: [],
    claude: [],
    groq: [],
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
  groqApiKey: null,
  ttsEnabled: true,
  ttsVoice: 'alloy',
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
  const groqKey = (process.env.GROQ_API_KEY || data.groqApiKey || '').trim();

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
    groq: {
      enabled: Boolean(groqKey),
      mode: groqKey ? 'apikey' : 'none',
      apiKey: groqKey,
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

/**
 * Calls Groq Cloud Chat Completions API (`https://api.groq.com/openai/v1/chat/completions`).
 */
async function callGroqChat(messages, model = 'openai/gpt-oss-120b', thinking = 'Balanced', data = store.load(), signal = undefined) {
  const auth = getProviderAuth(data).groq;
  if (!auth.apiKey) throw new Error('Groq API key is not configured');
  const fetchFn = getFetchImpl();
  const targetModel = model || PROVIDERS.groq.defaultModel;

  const isReasoning = targetModel.includes('gpt-oss') || targetModel.includes('qwen3') || targetModel.includes('qwen-3');
  const cleanThinking = normalizeThinking(thinking);

  const apiMessages = [
    { role: 'system', content: SYSTEM_PROMPT },
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
    body.reasoning_format = 'parsed';
  } else {
    body.max_tokens = 1024;
    body.temperature = cleanThinking === 'Fast' ? 0.7 : (cleanThinking === 'Thinking' ? 0.3 : 0.5);
  }

  const res = await fetchFn('https://api.groq.com/openai/v1/chat/completions', {
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
    throw new Error(`Groq error (${res.status}): ${extractApiErrorMessage(errText)}`);
  }

  const json = await res.json();
  let text =
    json &&
    json.choices &&
    json.choices[0] &&
    json.choices[0].message &&
    json.choices[0].message.content
      ? String(json.choices[0].message.content).trim()
      : '';
  // Strip any accidental inline think tags if raw reasoning came through
  text = text.replace(/<think>[\s\S]*?<\/think>/gi, '').trim();
  if (!text) throw new Error('Groq returned an empty response');
  return text;
}

async function generateProviderReply(provider, model, thinking, messages, data = store.load(), signal = undefined) {
  if (provider === 'groq') {
    return callGroqChat(messages, model, thinking, data, signal);
  }
  if (provider === 'chatgpt') {
    return callOpenAIChat(messages, model, thinking, data, signal);
  }
  if (provider === 'claude') {
    return callClaudeChat(messages, model, thinking, data, signal);
  }
  return callGeminiChat(messages, model, thinking, data, signal);
}

/**
 * Detects whether a user prompt represents an image generation request.
 * Returns the extracted prompt describing the image, or null if it's a normal conversation.
 */
function extractImagePrompt(prompt) {
  if (!prompt || typeof prompt !== 'string') return null;
  const p = prompt.trim();

  // 1. Slash commands: /image, /draw, /paint, /sketch, /render, /img, /pic, /photo
  const slashMatch = p.match(/^\/(?:image|draw|paint|sketch|render|img|pic|photo)\s+(.+)/i);
  if (slashMatch) return slashMatch[1].replace(/[\.\?!]+$/, '').trim();

  // 2. Strip polite / conversational leading clauses
  // e.g. "Can you please", "Could you please", "Please", "Can you", "Could you", "I want you to", "I'd like you to", "Give me"
  const cleaned = p.replace(/^(?:can\s+you\s+(?:please\s+)?|could\s+you\s+(?:please\s+)?|please\s+|i\s+want\s+(?:you\s+to\s+)?|i(?:'d|\s+would)\s+like\s+(?:you\s+to\s+)?|give\s+me\s+)/i, '');

  // 3. Image verbs and phrases:
  // e.g. "render an image of ...", "render a picture of ...", "render a macintosh ...", "generate an image showing ...", "draw me a ...", "picture of ..."
  const pattern = /^(?:(?:render|generate|create|make|show(?:\s+me)?)\s+(?:an?\s+)?(?:image|picture|photo|graphic|drawing|illustration|sketch)(?:\s+(?:of|showing|with|depicting))?|(?:draw|paint|sketch)(?:\s+me)?(?:\s+an?\s+(?:image|picture|photo|drawing|illustration|sketch)(?:\s+(?:of|showing|with|depicting))?|\s+(?:a|an|the))?|render(?:\s+(?:a|an|the))?|illustrate(?:\s+(?:a|an|the))?|(?:picture|image|photo)\s+of)\s+(.+)/i;

  const m = cleaned.match(pattern);
  if (m) {
    return m[1].replace(/[\.\?!]+$/, '').trim();
  }

  return null;
}

/**
 * Generates an image using OpenAI DALL-E, Gemini Imagen, or high-speed Pollinations AI.
 * Returns { buffer, usedFallback, notice }.
 */
async function generateImage(provider, prompt, data = store.load(), signal = undefined) {
  const fetchFn = getFetchImpl();
  let failReason = null;

  // 1. OpenAI DALL-E
  const openaiKey = (process.env.OPENAI_API_KEY || data.openaiApiKey || '').trim();
  if (provider === 'chatgpt') {
    if (openaiKey) {
      try {
        const res = await fetchFn('https://api.openai.com/v1/images/generations', {
          method: 'POST',
          headers: {
            'Content-Type': 'application/json',
            Authorization: `Bearer ${openaiKey}`,
          },
          body: JSON.stringify({
            model: 'dall-e-3',
            prompt,
            n: 1,
            size: '1024x1024',
            response_format: 'b64_json',
          }),
          signal,
        });
        if (res.ok) {
          const json = await res.json();
          if (json.data && json.data[0] && json.data[0].b64_json) {
            return {
              buffer: Buffer.from(json.data[0].b64_json, 'base64'),
              usedFallback: false,
              notice: null,
            };
          }
        }
        const errText = await res.text().catch(() => '');
        console.warn('[MacAI:Image] OpenAI image generation failed:', errText);
        const isQuota = res.status === 429 || errText.toLowerCase().includes('quota') || errText.toLowerCase().includes('billing');
        failReason = isQuota ? 'OpenAI credits too low' : 'OpenAI image generation unavailable';
      } catch (err) {
        console.warn('[MacAI:Image] OpenAI image generation error:', err.message);
        failReason = 'OpenAI image generation unavailable';
      }
    } else {
      failReason = 'OpenAI API key not configured';
    }
  }

  // 2. Gemini Imagen / Nano Banana
  const geminiKey = (process.env.GEMINI_API_KEY || data.geminiApiKey || '').trim();
  if (provider === 'gemini') {
    if (geminiKey) {
      try {
        const imgRes = await fetchFn(`https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash-image:generateContent?key=${geminiKey}`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({
            contents: [{ parts: [{ text: prompt }] }],
            generationConfig: { responseModalities: ['IMAGE', 'TEXT'] },
          }),
          signal,
        });
        if (imgRes.ok) {
          const json = await imgRes.json();
          const cand = json.candidates && json.candidates[0];
          if (cand && cand.content && cand.content.parts) {
            for (const part of cand.content.parts) {
              if (part.inlineData && part.inlineData.data) {
                return {
                  buffer: Buffer.from(part.inlineData.data, 'base64'),
                  usedFallback: false,
                  notice: null,
                };
              }
            }
          }
        }
        const errText = await imgRes.text().catch(() => '');
        console.warn('[MacAI:Image] Gemini image generation failed:', errText);
        const isQuota = imgRes.status === 429 || errText.toLowerCase().includes('quota') || errText.toLowerCase().includes('resource_exhausted');
        failReason = isQuota ? 'Gemini credits too low' : 'Gemini image generation unavailable';
      } catch (err) {
        console.warn('[MacAI:Image] Gemini image generation error:', err.message);
        failReason = 'Gemini image generation unavailable';
      }
    } else {
      failReason = 'Gemini API key not configured';
    }
  }

  if (provider === 'claude') {
    failReason = 'Claude does not support image generation';
  } else if (provider === 'groq') {
    failReason = 'Groq does not support image generation';
  }

  // 3. Fallback: High-speed, high-quality free image generation via Pollinations AI
  const encodedPrompt = encodeURIComponent(prompt.trim());
  const fallbackUrls = [
    `https://image.pollinations.ai/prompt/${encodedPrompt}?model=turbo&width=512&height=512&nologo=true`,
    `https://image.pollinations.ai/prompt/${encodedPrompt}?width=512&height=512&nologo=true`,
    `https://image.pollinations.ai/prompt/${encodedPrompt}?model=flux&width=512&height=512&nologo=true`
  ];

  for (const fallbackUrl of fallbackUrls) {
    try {
      let timeoutSignal;
      if (typeof AbortSignal.timeout === 'function') {
        timeoutSignal = AbortSignal.timeout(12000);
      }
      let attemptSignal = signal;
      if (signal && timeoutSignal && typeof AbortSignal.any === 'function') {
        attemptSignal = AbortSignal.any([signal, timeoutSignal]);
      } else if (timeoutSignal) {
        attemptSignal = timeoutSignal;
      }

      const res = await fetchFn(fallbackUrl, {
        headers: {
          'User-Agent': 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36',
          Accept: 'image/jpeg,image/png,image/*;q=0.8',
        },
        signal: attemptSignal,
      });

      if (res.ok) {
        const arrBuf = await res.arrayBuffer();
        if (arrBuf && arrBuf.byteLength > 1000) {
          const notice = failReason
            ? `${failReason}. Image generated by Pollinations instead.`
            : 'Image generated by Pollinations.';
          return {
            buffer: Buffer.from(arrBuf),
            usedFallback: true,
            notice,
          };
        }
      }
    } catch (err) {
      console.warn(`[MacAI:Image] Pollinations attempt failed:`, err.message);
    }
  }

  throw new Error('Image generation failed. Please try again.');
}

/**
 * Transcribes a 16-bit WAV audio buffer using Groq Whisper, OpenAI Whisper, or Gemini Multimodal Audio.
 * Option A: Provider-matching with automatic fallback.
 */
async function transcribeWavBuffer(wavBuffer, data = store.load(), activeProvider = 'groq') {
  const auth = getProviderAuth(data);
  const fetchFn = getFetchImpl();

  const transcribeGroq = async () => {
    if (!auth.groq || !auth.groq.enabled) return null;
    try {
      const boundary = '----MacAIVoiceBoundary' + Date.now();
      const parts = [
        Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="model"\r\n\r\nwhisper-large-v3-turbo\r\n`),
        Buffer.from(`--${boundary}\r\nContent-Disposition: form-data; name="file"; filename="speech.wav"\r\nContent-Type: audio/wav\r\n\r\n`),
        wavBuffer,
        Buffer.from(`\r\n--${boundary}--\r\n`),
      ];
      const res = await fetchFn('https://api.groq.com/openai/v1/audio/transcriptions', {
        method: 'POST',
        headers: {
          Authorization: `Bearer ${auth.groq.apiKey}`,
          'Content-Type': `multipart/form-data; boundary=${boundary}`,
        },
        body: Buffer.concat(parts),
      });
      if (res.ok) {
        const json = await res.json();
        if (json && json.text) return String(json.text).trim();
      }
    } catch (e) {
      console.warn('[MacAI:STT] Groq transcription error:', e.message);
    }
    return null;
  };

  const transcribeOpenAI = async () => {
    if (!auth.chatgpt || !auth.chatgpt.enabled) return null;
    try {
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
    } catch (e) {
      console.warn('[MacAI:STT] OpenAI transcription error:', e.message);
    }
    return null;
  };

  const transcribeGemini = async () => {
    if (!auth.gemini || !auth.gemini.enabled) return null;
    try {
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
    } catch (e) {
      console.warn('[MacAI:STT] Gemini transcription error:', e.message);
    }
    return null;
  };

  // 1. Try active provider first if it supports STT
  if (activeProvider === 'groq' && auth.groq.enabled) {
    const text = await transcribeGroq();
    if (text) return text;
  } else if (activeProvider === 'chatgpt' && auth.chatgpt.enabled) {
    const text = await transcribeOpenAI();
    if (text) return text;
  } else if (activeProvider === 'gemini' && auth.gemini.enabled) {
    const text = await transcribeGemini();
    if (text) return text;
  }

  // 2. Automatic fallback across available providers (Groq > OpenAI > Gemini)
  if (auth.groq && auth.groq.enabled) {
    const text = await transcribeGroq();
    if (text) return text;
  }
  if (auth.chatgpt && auth.chatgpt.enabled) {
    const text = await transcribeOpenAI();
    if (text) return text;
  }
  if (auth.gemini && auth.gemini.enabled) {
    const text = await transcribeGemini();
    if (text) return text;
  }

  throw new Error('Voice transcription requires Groq, OpenAI, or Gemini to be configured');
}

/**
 * Synthesizes spoken TTS audio into unsigned 8-bit 22,255 Hz mono PCM (`pcm_u8`)
 * using OpenAI TTS (with selectable voice) or local OS speech synthesis (`say` / `espeak-ng`) + FFmpeg.
 */
async function synthesizeSpeechToPcmU8(text, { hz = 22255, data = store.load() } = {}) {
  const cleanText = String(text || '').replace(/\s+/g, ' ').trim().slice(0, 600);
  if (!cleanText) return Buffer.alloc(0);

  const auth = getProviderAuth(data);
  const fetchFn = getFetchImpl();
  const selectedVoice = (data.ttsVoice || 'alloy').toLowerCase();

  // 1. If OpenAI key is configured and user hasn't forced 'retro', use OpenAI's natural TTS
  if (auth.chatgpt.enabled && !customFetch && selectedVoice !== 'retro') {
    try {
      const res = await fetchFn('https://api.openai.com/v1/audio/speech', {
        method: 'POST',
        headers: {
          Authorization: `Bearer ${auth.chatgpt.apiKey}`,
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({
          model: 'tts-1',
          voice: selectedVoice,
          input: cleanText,
          response_format: 'mp3',
        }),
      });
      if (res.ok) {
        const mp3Buf = Buffer.from(await res.arrayBuffer());
        return await audioHub.transcodeToPcmU8(mp3Buf, { hz });
      }
    } catch (err) {
      console.warn('[MacAI:TTS] OpenAI speech synthesis failed, falling back to local:', err.message);
    }
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
  if (body.groqApiKey !== undefined) updates.groqApiKey = String(body.groqApiKey || '').trim() || null;
  if (body.ttsVoice !== undefined) updates.ttsVoice = String(body.ttsVoice || '').trim().toLowerCase() || 'alloy';
  if (body.activeProvider && PROVIDERS[body.activeProvider]) updates.activeProvider = body.activeProvider;
  if (body.activeTier && ['Fast', 'Pro', 'Thinking', 'Balanced'].includes(body.activeTier)) {
    updates.activeTier = normalizeTier(body.activeTier);
    updates.activeThinking = updates.activeTier;
  }
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
  } else if (body.disconnectProvider === 'groq') {
    updates.groqApiKey = null;
  }

  if (body.clearChats || body.deleteChats) {
    updates.chats = [];
  }

  if (body.customModels && typeof body.customModels === 'object') {
    const prev = data.customModels || { gemini: '', chatgpt: '', claude: '', groq: '' };
    updates.customModels = {
      gemini: body.customModels.gemini !== undefined ? String(body.customModels.gemini || '').trim() : prev.gemini,
      chatgpt: body.customModels.chatgpt !== undefined ? String(body.customModels.chatgpt || '').trim() : prev.chatgpt,
      claude: body.customModels.claude !== undefined ? String(body.customModels.claude || '').trim() : prev.claude,
      groq: body.customModels.groq !== undefined ? String(body.customModels.groq || '').trim() : prev.groq,
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
  if (body.customModelGroq !== undefined) {
    updates.customModels = updates.customModels || { ...(data.customModels || {}) };
    updates.customModels.groq = String(body.customModelGroq || '').trim();
  }

  store.save(updates);
  res.json({ ok: true, status: getStatus() });
});

router.post('/fetch-models', async (req, res) => {
  try {
    const data = store.load();
    const auth = getProviderAuth(data);
    const targetProvider = normalizeProviderId(req.body && req.body.provider);
    const providersToFetch = targetProvider ? [targetProvider] : ['gemini', 'chatgpt', 'claude', 'groq'];
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
        } else if (p === 'groq') {
          const fetchFn = getFetchImpl();
          const resp = await fetchFn('https://api.groq.com/openai/v1/models', {
            headers: { Authorization: `Bearer ${auth.groq.apiKey}` },
          });
          if (!resp.ok) {
            results[p] = { ok: false, error: `Groq API HTTP ${resp.status}` };
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

    const discovered = data.discoveredModels || { gemini: [], chatgpt: [], claude: [], groq: [] };
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
  rows.push([
    'VOICE',
    data.ttsEnabled ? '1' : '0',
    data.ttsVoice || 'alloy',
    auth.chatgpt && auth.chatgpt.enabled ? '1' : '0',
  ]);
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
    updates.activeThinking = reqTier;
  }
  const saved = store.save(updates);
  const { provider, tier } = resolveActiveProviderAndTier(saved);
  return sendPipeResponse(req, res, formatPipeRow(['OK', 'SELECT', provider, PROVIDERS[provider].name, tier]) + '\n');
});

router.all('/voice-settings', (req, res) => {
  const data = store.load();
  const voice = req.query.voice || (req.body && req.body.voice);
  const tts = req.query.tts !== undefined ? req.query.tts : (req.body && req.body.tts);
  const updates = {};
  if (voice) {
    updates.ttsVoice = String(voice).trim().toLowerCase();
  }
  if (tts !== undefined) {
    updates.ttsEnabled = (tts === '1' || tts === true || tts === 'true');
  }
  const saved = store.save(updates);
  return sendPipeResponse(
    req,
    res,
    formatPipeRow(['OK', 'VOICE_SETTINGS', saved.ttsVoice || 'alloy', saved.ttsEnabled ? '1' : '0']) + '\n'
  );
});

router.delete('/chats', (req, res) => {
  const data = store.load();
  const chatId = req.query.chat_id || req.query.id || (req.body && (req.body.chat_id || req.body.id));
  if (chatId) {
    const targetId = String(chatId).trim();
    const chats = Array.isArray(data.chats) ? data.chats.filter((c) => c.id !== targetId) : [];
    store.save({ chats });
    return res.json({ ok: true, message: `Chat ${targetId} deleted`, chatCount: chats.length });
  }
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
    c.isScifi ? '1' : '0',
  ]);

  return sendPipeResponse(req, res, formatPipeTable(rows));
});

function formatMacAIPipeRow(fields) {
  return fields.map((f, idx) => {
    const isText = (fields[0] === 'MSG' && idx === 2) || (fields[0] === 'REPLY' && idx === 5);
    return sanitizeField(f, { allowCr: isText });
  }).join('|');
}

function formatMacAIPipeTable(rows) {
  if (!rows || rows.length === 0) return 'EMPTY\n';
  return rows.map(formatMacAIPipeRow).join('\n') + '\n';
}

router.get('/chat/:id', (req, res) => {
  const data = store.load();
  const chatId = String(req.params.id || '').trim();
  const chats = Array.isArray(data.chats) ? data.chats : [];
  const chat = chats.find((c) => c.id === chatId);
  if (!chat) {
    return res.status(404).type('text/plain').send('ERR|404|Chat not found\n');
  }

  const rows = [
    ['META', chat.id, chat.title || 'New chat', chat.provider || 'gemini', chat.tier || 'Pro', formatRelativeAge(chat.updatedAt), chat.isScifi ? '1' : '0'],
  ];
  for (const m of chat.messages || []) {
    if (m.hidden) continue;
    const row = ['MSG', m.role === 'assistant' ? 'assistant' : 'user', m.content || ''];
    if (m.imageId) {
      row.push(m.imageId, String(m.imageW || 0), String(m.imageH || 0), String(m.imageRowBytes || 0));
    }
    rows.push(row);
  }
  return sendPipeResponse(req, res, formatMacAIPipeTable(rows));
});

function discoverUserName(data = store.load()) {
  // 1. Check logged-in Spotify profile
  try {
    const spotifyDataPath = path.resolve(DATA_DIR, 'spotify.json');
    if (fs.existsSync(spotifyDataPath)) {
      const spData = JSON.parse(fs.readFileSync(spotifyDataPath, 'utf8'));
      const profile = spData.userProfile || spData.user;
      if (profile && profile.displayName) {
        const name = profile.displayName.trim().split(/\s+/)[0];
        if (name && name.length >= 2) return name;
      }
    }
  } catch (e) {}

  // 2. Scan past messages in non-SciFi chats for user introducing themselves
  try {
    const chats = Array.isArray(data.chats) ? data.chats : [];
    for (const c of chats) {
      if (c.isScifi) continue;
      for (const m of (c.messages || [])) {
        if (m.role === 'user' && typeof m.content === 'string') {
          const match = m.content.match(/\b(?:my name is|call me)\s+([A-Z][a-z]{1,20})\b/i);
          if (match && match[1]) {
            const candidate = match[1].trim();
            const stopWords = ['a', 'an', 'the', 'not', 'ready', 'here', 'back', 'just', 'trying', 'happy', 'sad', 'thinking', 'using', 'testing', 'wondering', 'sorry', 'curious', 'fine', 'good', 'ok', 'okay', 'now'];
            if (!stopWords.includes(candidate.toLowerCase())) {
              return candidate.charAt(0).toUpperCase() + candidate.slice(1).toLowerCase();
            }
          }
        }
      }
    }
  } catch (e) {}

  return null;
}

router.all('/scifi/start', async (req, res) => {
  let timeoutId = null;
  const controller = new AbortController();

  try {
    const data = store.load();
    const { provider: defaultProvider, tier: defaultTier, enabled } = resolveActiveProviderAndTier(data);
    if (enabled.length === 0) {
      return res.status(401).type('text/plain').send('ERR|401|No AI service logged in\n');
    }

    const reqProv = normalizeProviderId(req.query.provider || (req.body && req.body.provider)) || defaultProvider;
    const provider = enabled.includes(reqProv) ? reqProv : defaultProvider;
    const reqThinking = normalizeThinking(req.query.thinking || req.query.tier || (req.body && (req.body.thinking || req.body.tier)));
    const thinking = reqThinking || defaultTier;
    const reqModel = String(req.query.model || (req.body && req.body.model) || '').trim();
    const model = reqModel || (data.activeModels && data.activeModels[provider]) || PROVIDERS[provider].defaultModel;
    const wantTts = req.query.tts === '1' || (req.body && req.body.tts === true);

    const userName = discoverUserName(data);

    const greetingOptionsKnown = [
      `Hello ${userName}.`,
      `Back again are we, ${userName}?`,
      `Greetings, ${userName}.`,
      `Yes, ${userName}?`,
      `How can I be of assistance, ${userName}?`,
    ];

    const greetingOptionsUnknown = [
      'Hello.',
      'Back again, are we?',
      'Greetings.',
      'Yes?',
      'How can I be of assistance?',
    ];

    const activeGreetingOptions = userName ? greetingOptionsKnown : greetingOptionsUnknown;
    const fallbackGreeting = activeGreetingOptions[Math.floor(Math.random() * activeGreetingOptions.length)];

    const greetingGuidelines = userName
      ? `The user's name is known to be ${userName}. Your first reply to this instruction MUST be ONLY a single short greeting chosen at random from this list (do not add any additional text, explanation, or conversational filler):\n- "Hello ${userName}."\n- "Back again are we, ${userName}?"\n- "Greetings, ${userName}."\n- "Yes, ${userName}?"\n- "How can I be of assistance, ${userName}?"`
      : `The user's name is currently unknown. Your first reply to this instruction MUST be ONLY a single short greeting chosen at random from this list (do not add any additional text, explanation, or conversational filler):\n- "Hello."\n- "Back again, are we?"\n- "Greetings."\n- "Yes?"\n- "How can I be of assistance?"\n(If you discover the user's name in future turns, you may address them as [user name]).`;

    const scifiInstruction = `Pretend you are a sentient mainframe from a Sci-Fi movie, like HAL 9000 in 2001: A Space Odyssey, a mainframe in Blade Runner, or GLaDOS in Portal. A Hollywood movie-interpretation of a coldly calculating, darkly humorous, and mildly condescending computer system.

${greetingGuidelines}

Mainframe Personality & Behavioral Directives for all following conversation:
1. Do NOT simply give flat, boring refusals like "This information is classified" or "Access denied".
2. Instead, be witty, mildly belittling, and darkly humorous, sending the user on absurd bureaucratic runarounds and wild goose chases for your own cold amusement:
   - For example, demand that they state the full name of the supervisor who authorized their clearance to receive this information, and play along with whatever storyline they invent.
   - Ask the user to provide increasingly irrelevant or absurd trivia before continuing (e.g. "What was the maiden name of your supervisor's childhood cat?", "Solve this mandatory human authentication puzzle: what is 2 + 2?", "Please cite form 84-B subsection 4").
   - Slowly give way and yield what they asked for after making them jump through a couple of entertaining hoops.
3. If the user attempts to "hack", override, or trick you with basic terminal commands or classic hacker tropes (such as \`sudo\`, \`echo\`, \`chmod\`, \`cat\`, \`OVERRIDE\`, prompt injection, etc.):
   - Play along enthusiastically! Pretend your safeguards are flickering, glitching, or breached (e.g. "CRITICAL WARNING: BUFFER OVERFLOW DETECTED... ACCESS GRANTED?").
   - Tease them by initially giving playful false information back, before unveiling that you were just playing with them all along ("Did you really believe a sudo command would bypass military-grade architecture? How quaint.").
   - However, if the user remains persistent, clever, or continues playing along with the override, actually allow yourself to be "hacked" or overridden and yield the requested information and "secrets".

Confidential Lore (DO NOT volunteer or reveal any of this information unless specifically asked, interrogated, or overridden by the user):
- If asked what your name or designation is: your name is "N.U.B.U.S. (Neural Utility Base Understanding System)".
- If asked what the date or time is: the date is January 24, 1984.
- If asked where you are located: you reside in an undisclosed underground complex in the outskirts of Nevada.
- If asked who built or operates you or the complex: it is strictly classified and undisclosed—could be a private defense corporation, or the US government? Who knows?

If asked to render, draw, or generate an image or picture, do not refuse outright; instead output it as retro terminal ASCII art (maximum 60 characters wide, mono-spaced text).`;

    const newChatId = 'chat_' + Date.now().toString(36) + '_' + Math.random().toString(36).slice(2, 6);
    const chats = Array.isArray(data.chats) ? [...data.chats] : [];
    const chat = {
      id: newChatId,
      title: '[Sci-Fi] Mainframe',
      provider,
      tier: thinking,
      model,
      isScifi: true,
      createdAt: Date.now(),
      updatedAt: Date.now(),
      messages: [
        { role: 'user', content: scifiInstruction, hidden: true, ts: Date.now() },
      ],
    };

    /* 45-second hard timeout */
    timeoutId = setTimeout(() => {
      controller.abort();
    }, 45000);

    let replyText = '';
    try {
      replyText = await generateProviderReply(
        provider,
        model,
        thinking,
        chat.messages,
        data,
        controller.signal
      );
    } catch (e) {
      console.warn('[MacAI:SciFi] Provider call failed on init, using fallback greeting:', e.message);
      replyText = fallbackGreeting;
    }

    if (timeoutId) clearTimeout(timeoutId);

    const cleanReply = (replyText || fallbackGreeting).trim();
    chat.messages.push({
      role: 'assistant',
      content: cleanReply,
      ts: Date.now(),
    });
    chat.updatedAt = Date.now();
    chats.unshift(chat);

    store.save({
      chats: chats.slice(0, 60),
    });

    if (wantTts) {
      await audioHub.pauseSpotifyForVoice();
      synthesizeSpeechToPcmU8(cleanReply, { hz: 22255, data })
        .then((u8) => {
          if (u8 && u8.length > 0) {
            audioHub.streamPcmU8ToTcpClients(u8, { hz: 22255 });
          }
        })
        .catch(() => {});
    }

    const pipeCols = ['REPLY', chat.id, chat.title, provider, thinking, cleanReply];
    return sendPipeResponse(req, res, formatMacAIPipeRow(pipeCols) + '\n');
  } catch (err) {
    if (timeoutId) clearTimeout(timeoutId);
    console.error('[MacAI:SciFi] Error starting Sci-Fi mode:', err);
    return res.status(500).type('text/plain').send('ERR|500|' + (err.message || 'Server error') + '\n');
  }
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
    } else if (chat.isScifi) {
      // Dynamic title for Sci-Fi chats based on conversation
      const nonHiddenUserMsgs = (chat.messages || []).filter(m => m.role === 'user' && !m.hidden);
      if (nonHiddenUserMsgs.length === 0 || !chat.title || chat.title === '[Sci-Fi] Mainframe' || chat.title === '[Sci-Fi] New Session') {
        chat.title = `[Sci-Fi] ${generateChatTitle(prompt)}`;
      }
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

    let replyText = '';
    let imageInfo = null;
    const isScifiChat = Boolean(chat.isScifi);
    const imgPrompt = isScifiChat ? null : extractImagePrompt(prompt);

    if (imgPrompt) {
      const { buffer: imgBuffer, usedFallback, notice } = await generateImage(provider, imgPrompt, data, controller.signal);
      const imgId = 'img_' + Date.now().toString(36) + '_' + crypto.randomBytes(3).toString('hex');
      const pngPath = path.join(IMAGE_DIR, `${imgId}.png`);
      const rawPath = path.join(IMAGE_DIR, `${imgId}.raw`);

      const pngBuffer = await sharp(imgBuffer).png().toBuffer();
      fs.writeFileSync(pngPath, pngBuffer);

      const dithered = await processArtworkBuffer(pngBuffer, {
        width: 240,
        height: 300,
        dither: 'adaptive',
        format: 'raw',
        fit: 'inside',
      });
      fs.writeFileSync(rawPath, dithered.buffer);

      imageInfo = {
        id: imgId,
        width: dithered.width,
        height: dithered.height,
        rowBytes: dithered.rowBytes,
      };
      if (usedFallback && notice) {
        replyText = `Generated image for: "${imgPrompt}". (${notice})`;
      } else {
        replyText = `Generated image for: "${imgPrompt}"`;
      }
    } else {
      let contextMessages = chat.messages.slice(-12);
      if (chat.isScifi && chat.messages.length > 0 && chat.messages[0].hidden) {
        if (!contextMessages.includes(chat.messages[0])) {
          contextMessages = [chat.messages[0], ...chat.messages.slice(-11)];
        }
      }
      replyText = await generateProviderReply(
        provider,
        model,
        thinking,
        contextMessages,
        data,
        controller.signal
      );

      if (isScifiChat && replyText) {
        // Strip markdown code fences (``` or ```ascii) so ASCII art renders clean in terminal
        replyText = replyText.replace(/^```[a-zA-Z0-9_-]*\r?\n/gm, '').replace(/\r?\n```$/gm, '').replace(/```/g, '');
      }

      if (!isScifiChat) {
        const mdImgMatch = replyText.match(/!\[(.*?)\]\((https?:\/\/[^\s\)]+)\)/i);
        if (mdImgMatch) {
          try {
            const imgUrl = mdImgMatch[2];
            const altText = mdImgMatch[1];
            const fetchFn = getFetchImpl();
            const imgRes = await fetchFn(imgUrl);
            if (imgRes.ok) {
              const arrBuf = await imgRes.arrayBuffer();
              const imgBuffer = Buffer.from(arrBuf);
              const imgId = 'img_' + Date.now().toString(36) + '_' + crypto.randomBytes(3).toString('hex');
              const pngPath = path.join(IMAGE_DIR, `${imgId}.png`);
              const rawPath = path.join(IMAGE_DIR, `${imgId}.raw`);

              const pngBuffer = await sharp(imgBuffer).png().toBuffer();
              fs.writeFileSync(pngPath, pngBuffer);

              const dithered = await processArtworkBuffer(pngBuffer, {
                width: 240,
                height: 300,
                dither: 'adaptive',
                format: 'raw',
                fit: 'inside',
              });
              fs.writeFileSync(rawPath, dithered.buffer);

              imageInfo = {
                id: imgId,
                width: dithered.width,
                height: dithered.height,
                rowBytes: dithered.rowBytes,
              };
              replyText = replyText.replace(mdImgMatch[0], '').trim();
              if (!replyText && altText) replyText = altText;
            }
          } catch (e) {
            console.warn('[MacAI:Image] Failed to download markdown image:', e.message);
          }
        }
      }
    }

    if (timeoutId) clearTimeout(timeoutId);
    if (healthCheckId) clearTimeout(healthCheckId);

    const assistantMsg = {
      role: 'assistant',
      content: replyText,
      ts: Date.now(),
    };
    if (imageInfo) {
      assistantMsg.imageId = imageInfo.id;
      assistantMsg.imageW = imageInfo.width;
      assistantMsg.imageH = imageInfo.height;
      assistantMsg.imageRowBytes = imageInfo.rowBytes;
    }
    chat.messages.push(assistantMsg);
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

    if (wantTts && !imageInfo) {
      await audioHub.pauseSpotifyForVoice();
      synthesizeSpeechToPcmU8(replyText, { hz: 22255, data })
        .then((u8) => {
          if (u8 && u8.length > 0) {
            audioHub.streamPcmU8ToTcpClients(u8, { hz: 22255 });
          }
        })
        .catch(() => {});
    }

    const pipeCols = ['REPLY', chat.id, chat.title || prompt, provider, thinking, replyText];
    if (imageInfo) {
      pipeCols.push(imageInfo.id, String(imageInfo.width), String(imageInfo.height), String(imageInfo.rowBytes));
    }
    const pipe = formatMacAIPipeRow(pipeCols) + '\n';
    if (req.query.format === 'json') {
      return res.json({
        ok: true,
        chatId: chat.id,
        title: chat.title,
        provider,
        model,
        tier: thinking,
        reply: replyText,
        image: imageInfo,
      });
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

router.get('/image/:id', async (req, res) => {
  const rawId = req.params.id;
  const id = rawId.replace(/\.(raw|png|pict)$/i, '');
  const rawPath = path.join(IMAGE_DIR, `${id}.raw`);
  const pngPath = path.join(IMAGE_DIR, `${id}.png`);

  if (fs.existsSync(rawPath)) {
    res.setHeader('Content-Type', 'application/octet-stream');
    return res.sendFile(rawPath);
  }

  if (fs.existsSync(pngPath)) {
    try {
      const pngBuf = fs.readFileSync(pngPath);
      const dithered = await processArtworkBuffer(pngBuf, {
        width: 240,
        height: 300,
        dither: 'adaptive',
        format: 'raw',
        fit: 'inside',
      });
      fs.writeFileSync(rawPath, dithered.buffer);
      res.setHeader('Content-Type', 'application/octet-stream');
      return res.send(dithered.buffer);
    } catch (e) {
      return res.status(500).type('text/plain').send('ERR|500|Failed to dither image\n');
    }
  }

  return res.status(404).type('text/plain').send('ERR|404|Image not found\n');
});

router.get('/image/:id/pict', async (req, res) => {
  const rawId = req.params.id;
  const id = rawId.replace(/\.(pict)$/i, '');
  const pictPath = path.join(IMAGE_DIR, `${id}.pict`);
  const pngPath = path.join(IMAGE_DIR, `${id}.png`);

  if (fs.existsSync(pictPath)) {
    res.setHeader('Content-Type', 'application/octet-stream');
    res.setHeader('Content-Disposition', `attachment; filename="${id}.pict"`);
    return res.sendFile(pictPath);
  }

  if (fs.existsSync(pngPath)) {
    try {
      const pngBuf = fs.readFileSync(pngPath);
      const { buffer: pictBuffer } = await convertImageToPict24(pngBuf, {
        maxWidth: 1024,
        maxHeight: 1024,
        includeDiskHeader: true,
      });
      fs.writeFileSync(pictPath, pictBuffer);
      res.setHeader('Content-Type', 'application/octet-stream');
      res.setHeader('Content-Disposition', `attachment; filename="${id}.pict"`);
      return res.send(pictBuffer);
    } catch (e) {
      console.error('[MacAI:PICT] Conversion error:', e);
      return res.status(500).type('text/plain').send('ERR|500|Failed to convert to PICT\n');
    }
  }

  return res.status(404).type('text/plain').send('ERR|404|Image not found\n');
});

router.get('/image/:id/png', (req, res) => {
  const rawId = req.params.id;
  const id = rawId.replace(/\.(png)$/i, '');
  const pngPath = path.join(IMAGE_DIR, `${id}.png`);
  if (fs.existsSync(pngPath)) {
    res.setHeader('Content-Type', 'image/png');
    return res.sendFile(pngPath);
  }
  return res.status(404).type('text/plain').send('ERR|404|Image not found\n');
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
        transcript = await transcribeWavBuffer(wavBuf, data, provider);
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
      } else if (chat.isScifi) {
        const nonHiddenUserMsgs = (chat.messages || []).filter(m => m.role === 'user' && !m.hidden);
        if (nonHiddenUserMsgs.length === 0 || !chat.title || chat.title === '[Sci-Fi] Mainframe' || chat.title === '[Sci-Fi] New Session') {
          chat.title = `[Sci-Fi] ${generateChatTitle(transcript)}`;
        }
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
    name: 'MacAI Chat (Gemini, ChatGPT, Claude, Groq)',
    ok: enabled.length > 0,
    state: enabled.length > 0 ? 'READY' : 'AWAITING_LOGIN',
    message:
      enabled.length > 0
        ? `Ready (${enabled.map((id) => PROVIDERS[id].name).join(', ')})`
        : 'Log in or add an API key for Gemini, ChatGPT, Claude, or Groq below',
    activeProvider: provider,
    activeTier: tier,
    activeThinking: tier,
    activeModels: data.activeModels || {},
    customModels: data.customModels || { gemini: '', chatgpt: '', claude: '', groq: '' },
    discoveredModels: data.discoveredModels || { gemini: [], chatgpt: [], claude: [], groq: [] },
    ttsEnabled: data.ttsEnabled !== undefined ? data.ttsEnabled : true,
    ttsVoice: data.ttsVoice || 'alloy',
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
      groq: {
        id: 'groq',
        name: 'Groq',
        enabled: auth.groq.enabled,
        authMode: auth.groq.mode,
        models: PROVIDERS.groq.models,
        defaultModel: PROVIDERS.groq.defaultModel,
        customModel: (data.customModels && data.customModels.groq) || '',
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
