# MacCompanion

Classic Macs are timeless, but the modern web left them behind around 1998. **MacCompanion** is a lightweight bridge server that lets your vintage 68k Macintosh party like it's 1984.

**Requirements:**
- Any always-on computer, NAS, or Raspberry Pi on your local network capable of running **Docker** with an internet connection.

---

## Client Apps

### SpotiMac
Spotify for your 68k Mac. Supports local playback and multitasking so you can get that real work done and delivered to your favorite tunes.

**Requirements:**
- **Account:** Spotify Premium + a free Spotify Developer Client ID.
- **Minimum:** Macintosh Plus (2 MB+ RAM) or SE / Classic (68000 @ 8 MHz), System 6.0.8, Sound Manager 2.0+, and **MacTCP** (via Ethernet card, LocalTalk-to-Ethernet bridge, or BlueSCSI v2 Wi-Fi).
- **Recommended:** Macintosh SE/30, Classic II, LC, Mac II, or Quadra (68020/030/040) — or Basilisk II — running System 7.1 or 7.5.5. Supports both 9" Compact Mac (`512×342`) and larger displays.

---

## Quick Start

1. **Start the MacCompanion server:**
   ```bash
   docker-compose up -d --build
   ```
2. **Link Spotify:** Open `http://<your-server-ip>:6800` in a modern browser, enter your Spotify Client ID, and click **Connect with Spotify**.
3. **Run SpotiMac on your Mac:** Mount `SpotiMac.dsk` (or decode `SpotiMac.bin` with StuffIt Expander) and launch **SpotiMac** — it will find your MacCompanion server automatically.

---

## Credits & Open Source Acknowledgments

This project is built with the help of the following open-source projects and libraries:

- **[Express](https://expressjs.com/)** — Fast, unopinionated, minimalist web framework for Node.js ([MIT License](https://github.com/expressjs/express/blob/master/LICENSE))
- **[Sharp](https://sharp.pixelplumbing.com/)** — High-performance Node.js image processing powered by [libvips](https://www.libvips.org/) ([Apache-2.0 License](https://github.com/lovell/sharp/blob/main/LICENSE))
- **[librespot](https://github.com/librespot-org/librespot)** — Open-source Spotify Connect client library ([MIT License](https://github.com/librespot-org/librespot/blob/dev/LICENSE))
- **[FFmpeg](https://ffmpeg.org/)** — Multimedia framework used for real-time 8-bit PCM downsampling ([LGPLv2.1+ / GPLv2+](https://ffmpeg.org/legal.html))
- **[Retro68](https://github.com/autc04/Retro68)** by Wolfgang Thaller — GCC-based cross-compiler toolchain targeting 68k and PowerPC Macintosh systems ([GPLv3 with GCC Runtime Library Exception](https://github.com/autc04/Retro68/blob/master/COPYING))
