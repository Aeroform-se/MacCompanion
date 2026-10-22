#include "Types.r"
#include "SysTypes.r"
#include "Icons.r"
#include "Finder.r"
#include "version.h"

/*
 * Classic Mac OS 'vers' (1) & (2) resources for System 7 Finder "Get Info" (Cmd+I)
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

/* Owner Signature Resource ('MCSP' ID 0) */
type 'MCSP' as 'STR ';
resource 'MCSP' (0, "Owner Resource") {
    "MaciSpot " APP_VERSION_SHORT " (" APP_VERSION_CODENAME ")"
};

/* File Reference ('FREF' 128) & Bundle ('BNDL' 128) for Custom Finder Icons */
resource 'FREF' (128) {
    'APPL',
    0,
    ""
};

resource 'BNDL' (128) {
    'MCSP',
    0,
    {
        'ICN#', { 0, 128 },
        'ics#', { 0, 128 },
        'FREF', { 0, 128 }
    }
};

/* 32x32 Large Application Icon & Mask ('ICN#' 128) */
resource 'ICN#' (128, "MaciSpot", purgeable) {
    {
        /* Icon Bits */
        $"000FF000"
        $"007C3E00"
        $"01C00380"
        $"030000C0"
        $"06000060"
        $"09FFFF90"
        $"1A000058"
        $"12000048"
        $"123FFC48"
        $"12400248"
        $"1E400278"
        $"7E44A27E"
        $"4E408272"
        $"4E418272"
        $"4E400272"
        $"4E442272"
        $"4E43C272"
        $"3E40027C"
        $"1E3FFC78"
        $"02000040"
        $"02000040"
        $"02000040"
        $"02000040"
        $"0241FE40"
        $"02000040"
        $"02000040"
        $"02000040"
        $"03FFFFC0"
        $"01000080"
        $"01000080"
        $"01000080"
        $"01FFFF80",
        /* Mask Bits */
        $"000FF000"
        $"007C3E00"
        $"01C00380"
        $"030000C0"
        $"06000060"
        $"09FFFF90"
        $"1BFFFFD8"
        $"13FFFFC8"
        $"13FFFFC8"
        $"13FFFFC8"
        $"1FFFFFF8"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"3FFFFFFC"
        $"1FFFFFF8"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"01FFFF80"
        $"01FFFF80"
        $"01FFFF80"
        $"01FFFF80"
    }
};

/* 16x16 Small Application Icon & Mask ('ics#' 128) */
resource 'ics#' (128, "MaciSpot Small", purgeable) {
    {
        /* Small Icon Bits */
        $"1FF0"
        $"2008"
        $"5FF4"
        $"600C"
        $"67CC"
        $"E82E"
        $"AAAA"
        $"A28A"
        $"682C"
        $"27C8"
        $"2008"
        $"29E8"
        $"2008"
        $"3FF8"
        $"1010"
        $"1FF0",
        /* Small Mask Bits */
        $"1FF0"
        $"2008"
        $"5FF4"
        $"7FFC"
        $"7FFC"
        $"FFFE"
        $"FFFE"
        $"FFFE"
        $"7FFC"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"1FF0"
        $"1FF0"
    }
};

/*
 * SIZE -1 resource:
 * - acceptSuspendResumeEvents & canBackground: keeps TCP audio & timeline alive in background
 * - 512 KB preferred / 384 KB minimum RAM partition
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
    524288,
    393216
};

resource 'MBAR' (128) {
    { 128, 129, 130 }
};

resource 'MENU' (128, "Apple") {
    128, textMenuProc,
    0x7FFFFFFD,
    enabled, apple,
    {
        "About MaciSpot...", noIcon, noKey, noMark, plain,
        "-", noIcon, noKey, noMark, plain
    }
};

resource 'MENU' (129, "File") {
    129, textMenuProc,
    allEnabled,
    enabled, "File",
    {
        "Server Connection...", noIcon, "R", noMark, plain,
        "Find / Search...", noIcon, "F", noMark, plain,
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
        "Like / Unlike Track", noIcon, "L", noMark, plain,
        "Choose Speaker...", noIcon, "K", noMark, plain
    }
};
