#include "Types.r"
#include "SysTypes.r"

/*
 * SIZE -1 resource:
 * - acceptSuspendResumeEvents: cooperative multitasking support
 * - canBackground: receive nullEvent while in background so TCP audio & UDP state keep running!
 * - 384 KB preferred / 256 KB minimum partition (plenty for double-buffered 22kHz PCM + QuickDraw UI)
 */
resource 'SIZE' (-1) {
    reserved,
    acceptSuspendResumeEvents,
    reserved,
    canBackground,
    multiFinderAware,
    backgroundAndForeground,
    dontGetFrontClicks,
    ignoreChildDiedEvents,
    is32BitCompatible,
    notHighLevelEventAware,
    onlyLocalHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    reserved,
    reserved,
    reserved,
    393216,
    262144
};

resource 'MBAR' (128) {
    { 128, 129, 130 }
};

resource 'MENU' (128, "Apple") {
    128, textMenuProc,
    0x7FFFFFFD,
    enabled, apple,
    {
        "About MacCompanion Spotify...", noIcon, noKey, noMark, plain,
        "-", noIcon, noKey, noMark, plain
    }
};

resource 'MENU' (129, "File") {
    129, textMenuProc,
    allEnabled,
    enabled, "File",
    {
        "Reconnect to Server", noIcon, "R", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Quit", noIcon, "Q", noMark, plain
    }
};

resource 'MENU' (130, "Playback") {
    130, textMenuProc,
    allEnabled,
    enabled, "Playback",
    {
        "Play / Pause", noIcon, "P", noMark, plain,
        "Next Track", noIcon, "N", noMark, plain,
        "Previous Track", noIcon, "B", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Toggle Local 68k Audio", noIcon, "L", noMark, plain
    }
};
