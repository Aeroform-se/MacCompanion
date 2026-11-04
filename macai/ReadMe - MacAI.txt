========================================================================
MacAI 68k — Read Me
Part of the MacCompanion Suite for Vintage 68k Macintosh
========================================================================

WHAT IS MACAI?
MacAI is a native System 7 AI client for vintage 68k Macintosh computers.
Chat with Google Gemini, OpenAI ChatGPT, Anthropic Claude, and Groq,
generate 1-bit dithered images (with 24-bit color PICT export), use
bi-directional voice (Push-to-Talk microphone input & spoken TTS
replies), or enter Sci-Fi Mainframe Mode with HAL 9000 and GLaDOS voices.


SYSTEM REQUIREMENTS
- System 7.0 through Mac OS 9.2.2 (System 7.1–7.5.5 recommended)
- Motorola 68000 or higher (68020+ recommended)
- Sound Manager 3.0+ (for spoken TTS replies and microphone input)
- MacTCP 2.0.6+ or Open Transport with an active network connection
- A running MacCompanion server on your local network


SERVER CONFIGURATION (MACCOMPANION WEB UI)
1. Open http://<your-server-ip>:6800 in a modern web browser and click
   "Configure MacAI".
2. Paste an API key for at least one AI provider and click Save:
   - Google Gemini: https://aistudio.google.com/apikey (free tier available)
   - OpenAI (ChatGPT): https://platform.openai.com/api-keys
   - Anthropic (Claude): https://console.anthropic.com/
   - Groq: https://console.groq.com/keys
3. (Optional) Under "Speech & Voice Settings", choose your preferred
   TTS voice (natural voices or synthetic Retro 1984, HAL 9000, and
   GLaDOS voices) and choose whether MacAI should duck or pause SpotiMac
   music during spoken replies.


USING MACAI ON YOUR MAC
- Launch MacAI. It will automatically discover your MacCompanion server
  on the local network (or let you enter the server IP manually).
- Switch AI providers and speed/thinking tiers anytime from the top bar
  or menu.
- Type "/image <prompt>" (or click the Image button) to generate artwork
  rendered for your Mac's screen, and save it as a standard MacPaint or
  PICT file.
- Enable the Speaker icon for spoken AI replies, or hold the Mic button
  to talk to your Macintosh.
