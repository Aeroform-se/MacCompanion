========================================================================
SpotiMac 68k — Read Me
Part of the MacCompanion Suite for Vintage 68k Macintosh
========================================================================

WHAT IS SPOTIMAC?
SpotiMac brings a full (almost) Spotify experience to your vintage mac!
Browse your library, search for tunes, play back locally with multitasking
and mini player support, launch a full screen party mode with visualizers
and more. 

SYSTEM REQUIREMENTS
- System 7.0 through Mac OS 9.2.2 (System 7.1–7.5.5 recommended)
- Motorola 68000 or higher (68020+ recommended)
- Sound Manager 3.0+ for local audio playback
- MacTCP 2.0.6+ or Open Transport with an active network connection
- A running MacCompanion server on your local network
- A Spotify Premium account


SERVER CONFIGURATION (MACCOMPANION WEB UI)
1. Open http://<your-server-ip>:6800 in a modern web browser and click
   "Configure SpotiMac".
2. Create a free app at https://developer.spotify.com/dashboard:
   - Set the Redirect URI to:
     https://aeroform-se.github.io/MacCompanion/callback.html
   - Check "Web API", save, and paste your Client ID into MacCompanion.
3. Click "Connect Spotify" to link your Spotify account.


ENABLING LOCAL PLAYBACK (SPEAKER PAIRING)
To play music directly through your vintage Mac's speaker, you need to
enable it to act as a Spotify speaker once:

Option 1 — The easy way:
  Launch SpotiMac on your vintage Mac and connect to your MacCompanion
  server. Then open any official Spotify app on any phone or computer
  on the same network and select "Macintosh 68k" (or your custom Mac
  client name) from the Speakers menu. Starting playback once
  automatically enables the speaker.

Option 2 — The manual way:
  If the speaker does not appear in your Spotify player (for example,
  if your Docker container is not running in network host mode and
  mDNS discovery is blocked):
  1. In the MacCompanion Web UI ("Configure SpotiMac"), click
     "Open Spotify Speaker Login" (opens in a new tab) and click Agree.
  2. If your browser is on a different computer than the server, it
     will redirect to a http://127.0.0.1:6800/login?code=... page that
     cannot load.
  3. Copy that full 127.0.0.1 URL including the code from your browser's
     address bar, return to the MacCompanion tab, paste it into the box,
     and click "Complete Pairing".


TIPS & SHORTCUTS
- Select "This Mac" in the Speakers menu inside SpotiMac to stream audio
  directly to your vintage Macintosh.
- Use File -> Set Client Name... (Cmd-N) to give your Mac a custom
  Spotify Connect speaker name on the network.
- Switch between 22 kHz and 11 kHz audio or change 8-bit DSP sound
  profiles ("Low Noise Warm", "HiFi", or "Compact Mac Speaker") anytime
  from the MacCompanion Web UI.
