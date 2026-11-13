========================================================================
  MacDrop X — Installation & macOS Security Instructions
========================================================================

System Requirements:
  - Mac with Apple Silicon (M1/M2/M3/M4+) or Intel (x86_64) CPU
  - macOS 11.0 (Big Sur) or later

Installation:
  1. Drag "MacDrop X.app" into the "Applications" folder shortcut inside
     this disk image.
  2. Eject the disk image, then launch "MacDrop X" from /Applications.

------------------------------------------------------------------------
  Opening MacDrop X (Gatekeeper / "Developer Cannot Be Verified")
------------------------------------------------------------------------
Because MacDrop X is open-source software and is not signed with a paid
Apple Developer ID certificate, macOS Gatekeeper will block it the first
time you open it after downloading it from the internet.

Choose EITHER Option A (System Settings) OR Option B (Terminal):

--- Option A: Allow via macOS Privacy & Security (No Terminal) ---

  On macOS 15 Sequoia & macOS 13–14 Ventura / Sonoma:
    1. Double-click "MacDrop X" in /Applications.
    2. When macOS warns "Apple could not verify 'MacDrop X' is free of
       malware..." (or "cannot be opened because the developer cannot be
       verified"), click "Done" or "Cancel" (do NOT click Move to Trash).
    3. Open System Settings -> Privacy & Security.
    4. Scroll down to the Security section where it says:
       "MacDrop X" was blocked to protect your Mac.
    5. Click "Open Anyway", confirm with your password / Touch ID, and
       click "Open Anyway" in the confirmation prompt.

  On macOS 11 Big Sur & macOS 12 Monterey:
    1. Open Finder -> Applications.
    2. Control-click (or right-click) "MacDrop X" and choose "Open".
    3. Click "Open" in the confirmation dialog. (If needed, go to
       System Preferences -> Security & Privacy -> General and click
       "Open Anyway".)

--- Option B: Allow via Terminal (Fastest, All macOS Versions) ---

  After dragging MacDrop X into /Applications, open Terminal and run:

    xattr -dr com.apple.quarantine "/Applications/MacDrop X.app" && open "/Applications/MacDrop X.app"

  If macOS prompts for Local Network access on first launch, click
  "Allow" so MacDrop X can discover and transfer files with your 68k Macs.
========================================================================
