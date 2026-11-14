#include "Types.r"
#include "SysTypes.r"
#include "Icons.r"
#include "Finder.r"
#include "version.h"

/*
 * Classic Mac OS 'vers' (1) & (2) resources for Finder Get Info
 */
resource 'vers' (1) {
    APP_VERSION_MAJOR_BCD,
    APP_VERSION_MINOR_BCD,
    APP_VERSION_STAGE,
    APP_VERSION_NONFINAL,
    verUS,
    APP_VERSION_SHORT,
    APP_VERSION_LONG
};

resource 'vers' (2) {
    APP_VERSION_MAJOR_BCD,
    APP_VERSION_MINOR_BCD,
    APP_VERSION_STAGE,
    APP_VERSION_NONFINAL,
    verUS,
    APP_VERSION_SHORT,
    "MacCompanion Suite"
};

/* Owner Signature Resource ('NFLM' ID 0) */
type 'NFLM' as 'STR ';
resource 'NFLM' (0, "Owner Resource") {
    "NetFlim " APP_VERSION_SHORT " (" APP_VERSION_CODENAME ")"
};

/* File Reference ('FREF' 128) & Bundle ('BNDL' 128) */
resource 'FREF' (128) {
    'APPL',
    0,
    ""
};

resource 'BNDL' (128) {
    'NFLM',
    0,
    {
        'ICN#', { 0, 128 },
        'ics#', { 0, 128 },
        'FREF', { 0, 128 }
    }
};

/*
 * 32x32 Large Application Icon & Mask ('ICN#' 128)
 * Compact Macintosh with film reel & cinema play symbol on screen
 */
resource 'ICN#' (128, "NetFlim", purgeable) {
    {
        /* Icon Bits */
        $"07FFFFE0"
        $"08000010"
        $"09FFFF90"
        $"0A000050"
        $"0A630050"
        $"0A948050"
        $"0A630C50"
        $"0A000E50"
        $"0A800F50"
        $"0AA00F90"
        $"0AA80F50"
        $"0AA40E50"
        $"0AA80C50"
        $"0AA00050"
        $"0A800050"
        $"0A000050"
        $"09FFFF90"
        $"08000010"
        $"08000010"
        $"0807E010"
        $"08000010"
        $"08000010"
        $"08000010"
        $"0FFFFFF0"
        $"04000020"
        $"04000020"
        $"04000020"
        $"07FFFFE0"
        $"02444440"
        $"02444440"
        $"02444440"
        $"07FFFFE0",
        /* Mask Bits */
        $"07FFFFE0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"0FFFFFF0"
        $"07FFFFE0"
        $"07FFFFE0"
        $"07FFFFE0"
        $"07FFFFE0"
        $"07FFFFE0"
        $"07FFFFE0"
        $"07FFFFE0"
        $"07FFFFE0"
    }
};

resource 'ics#' (128, "NetFlim Small", purgeable) {
    {
        /* Small Icon Bits */
        $"3FFC"
        $"4002"
        $"5FF6"
        $"5212"
        $"5392"
        $"53D2"
        $"5392"
        $"5212"
        $"5FF2"
        $"4002"
        $"43C2"
        $"7FFE"
        $"2004"
        $"3FFC"
        $"1558"
        $"3FFC",
        /* Small Mask Bits */
        $"3FFC"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"3FFC"
        $"3FFC"
        $"3FFC"
        $"3FFC"
    }
};

resource 'ics#' (-16455, "Custom Small Icon", purgeable) {
    {
        /* Small Icon Bits */
        $"3FFC"
        $"4002"
        $"5FF6"
        $"5212"
        $"5392"
        $"53D2"
        $"5392"
        $"5212"
        $"5FF2"
        $"4002"
        $"43C2"
        $"7FFE"
        $"2004"
        $"3FFC"
        $"1558"
        $"3FFC",
        /* Small Mask Bits */
        $"3FFC"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"7FFE"
        $"3FFC"
        $"3FFC"
        $"3FFC"
        $"3FFC"
    }
};

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
    1536 * 1024,
    384 * 1024
};

resource 'MBAR' (128) {
    { 128, 129, 130, 131 }
};

resource 'MENU' (128, "Apple") {
    128, textMenuProc,
    0x7FFFFFFD,
    enabled, apple,
    {
        "About NetFlim...", noIcon, noKey, noMark, plain,
        "-", noIcon, noKey, noMark, plain
    }
};

resource 'MENU' (129, "File") {
    129, textMenuProc,
    allEnabled,
    enabled, "File",
    {
        "Refresh Catalog", noIcon, "R", noMark, plain,
        "Set Server IP...", noIcon, "I", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Stop Playback", noIcon, ".", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Quit", noIcon, "Q", noMark, plain
    }
};

resource 'MENU' (130, "Playback") {
    130, textMenuProc,
    allEnabled,
    enabled, "Playback",
    {
        "Play / Pause", noIcon, noKey, noMark, plain,
        "Fullscreen", noIcon, "F", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Loop Video", noIcon, "L", noMark, plain,
        "Mute Audio", noIcon, "M", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Next Page", noIcon, noKey, noMark, plain,
        "Previous Page", noIcon, noKey, noMark, plain
    }
};

resource 'MENU' (131, "Profile") {
    131, textMenuProc,
    allEnabled,
    enabled, "Profile",
    {
        "Auto (By Mac CPU)", noIcon, noKey, noMark, plain,
        "Mac SE/30 (180 KB/s)", noIcon, noKey, noMark, plain,
        "Mac SE (75 KB/s)", noIcon, noKey, noMark, plain,
        "Mac Plus (45 KB/s)", noIcon, noKey, noMark, plain
    }
};

/* Standard System 7 Modal Dialog for "Set Server IP" ('DLOG' 128 & 'DITL' 128) */
/* Mirrors MacDrop DLOG 133 layout: Connect, Cancel, Scan LAN + live inline status */
resource 'DLOG' (128, "Set Server IP", purgeable) {
    {80, 96, 232, 416},
    dBoxProc,
    invisible,
    noGoAway,
    0x0,
    128,
    "Set Server IP",
    centerMainScreen
};

resource 'DITL' (128, "Set Server IP Items", purgeable) {
    {
        /* [1] Connect button (default) */
        {120, 228, 140, 304}, Button { enabled, "Connect" },
        /* [2] Cancel button */
        {120, 142, 140, 216}, Button { enabled, "Cancel" },
        /* [3] Scan LAN button */
        {120, 16, 140, 106},  Button { enabled, "Scan LAN" },
        /* [4] Prompt label */
        {14, 16, 30, 304},    StaticText { disabled, "MacCompanion Server IP & Port:" },
        /* [5] Server IP EditText */
        {38, 18, 56, 224},    EditText { enabled, "" },
        /* [6] HTTP Port EditText */
        {38, 234, 56, 302},   EditText { enabled, "6800" },
        /* [7] Status / Error feedback label */
        {68, 16, 110, 304},   StaticText { disabled, "" }
    }
};

/* About & Alert Dialog (ID 129) */
resource 'ALRT' (129, "Message") {
    {90, 96, 226, 416},
    129,
    {
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1,
        OK, visible, sound1
    },
    centerMainScreen
};

resource 'DITL' (129, "Message Items") {
    {
        {100, 228, 122, 304}, Button { enabled, "OK" },
        {14, 16, 88, 304},    StaticText { disabled, "^0\n^1\n^2" }
    }
};
