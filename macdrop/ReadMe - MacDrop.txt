========================================================================
MacDrop 68k — Read Me
Part of the MacCompanion Suite for Vintage 68k Macintosh
========================================================================

WHAT IS MACDROP?
MacDrop is AirDrop for vintage 68k Macintosh computers. Send files,
archives (.sit, .hqx, .bin), applications, and documents wirelessly
between any modern computer, phone, or tablet and your 68k Mac—in both
directions, all while preserving Classic Mac OS Resource Forks, Finder icons,
and Type/Creator codes via automatic MacBinary II wrapping.


SYSTEM REQUIREMENTS
- System 7.0 through Mac OS 9.2.2 (System 7.1–7.5.5 recommended)
- Motorola 68000 or higher (512 KB RAM minimum)
- MacTCP 2.0.6+ or Open Transport with an active network connection
- A running MacCompanion server on your local network


SERVER & CLIENT SETUP

1. MacDrop is an extension made to launch when you boot your macintosh.
   To install, simply drag the MacDrop file into your system/extensions
   folder and reboot your mac. You should now see MacDrop running in
   your menu bar.
2. Macdrop will automatically try to discover your MacCompanion server 
   via UDP broadcast (or you can enter your server IP via File -> 
   Set Server IP...).
3. To give your Mac a custom display name, choose File -> Set Client
   Name... (Cmd-N), or choose File -> Set Download Folder... (Cmd-D)
   to pick where incoming files are saved.
4. Open http://<your-server-ip>:6800 on any modern computer, phone, or
   tablet. Your vintage Mac will appear immediately inside the MacDrop
   card on the dashboard.
   - Tip: Click the pop-out icon in the top-right corner of the MacDrop
     card (or visit http://<your-server-ip>:6800/#drop) for a compact
     floating drop window.
5. Should your client not appear in the MacCompanion dashboard, you can
   try to add it manually in the configuration page.


SENDING & RECEIVING FILES
- Modern Device -> Vintage Mac:
  Drag and drop any file onto your Mac's icon in the MacDrop web card
  (or click the icon to browse). MacBinary (.bin) files are unpacked
  automatically on your Mac with their intact Resource Fork, Data Fork,
  and Finder icon!
- Vintage Mac -> Modern Device:
  Drag and drop any Mac file onto the MacDrop application icon or
  window, or click "Send File..." (Cmd-O). Click "Accept & Download" in
  the MacCompanion web UI to download the file to your modern device.
