NetFlim v0.1.0 "First Reel"
===========================
Streaming MacFlim Cinema for Vintage 68k Macintosh

Requirements:
- Macintosh SE (68000), Macintosh SE/30 (68030), Classic, LC, Quadra, or BasiliskII
- System 6.0.7 / 7.0 - 7.6.1 / Mac OS 8.1
- MacTCP or Open Transport (Ethernet PDS / NuBus card recommended)
- MacCompanion Middleware Server v0.4.73+ on your local network
  (HTTP port 6800, UDP discovery port 6801, UDP VOD stream port 6804)

Quick Start:
1. Copy NetFlim to your Macintosh hard disk.
2. Start MacCompanion on your host/Docker server and upload/encode videos
   in the Web UI at http://<server-ip>:6800/netflim.html
3. Launch NetFlim. It automatically broadcasts on UDP 6801 to find your server.
   If your server is on another subnet or behind NAT (e.g. BasiliskII SLiRP),
   choose File -> Set Server IP... (Cmd+I) and enter its IP (such as 10.0.2.2)
   with port 6800.

Gallery Controls:
- Click any video poster or title card to select & start streaming.
- Double-click a poster (or press Return) to play the selected video.
- Left / Right Arrow keys (or Prev / Next Page buttons): Browse catalog pages.
- Cmd+R: Refresh video library from server.

Player Controls:
- Spacebar (or Play/Pause button): Pause / Resume stream.
- F or Cmd+F (or Full button): Toggle 1:1 Cinema Fullscreen (512x342).
  Press Escape, Space, F, or click the mouse to exit Fullscreen or control playback.
- M or Cmd+M (or Mute button): Toggle 22.25 kHz 8-bit Sound Manager audio.
  Note: On slow links or 68000 Macs, Muting stops audio streaming from the
  server to cut network & CPU usage!
- L or Cmd+L (or Loop button): Toggle looping at end of video.
- Escape or Cmd+. (or Stop button): Return to Gallery.

Profile Menu:
- Auto: Uses se30 on 68020+/030+ Macs, se on 68000 Macs, falling back cleanly.
- Manually pick se30 (120-180 KB/s), se (50-90 KB/s), or plus (30-60 KB/s)
  to match your Macintosh CPU and PDS Ethernet card speed.
