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

/* Owner Signature Resource ('MCDP' ID 0) */
type 'MCDP' as 'STR ';
resource 'MCDP' (0, "Owner Resource") {
    "MacDrop " APP_VERSION_SHORT " (" APP_CODENAME ")"
};

/* File Reference ('FREF' 128) & Bundle ('BNDL' 128) */
resource 'FREF' (128) {
    'APPL',
    0,
    ""
};

resource 'BNDL' (128) {
    'MCDP',
    0,
    {
        'ICN#', { 0, 128 },
        'FREF', { 0, 128 }
    }
};

/* 32x32 Large Application Icon & Mask ('ICN#' 128) from ui/MacDrop/MacDrop-icon_32x32.png */
resource 'ICN#' (128, "MacDrop", purgeable) {
    {
        /* Icon Bits */
        $"000FF000"
        $"00700E00"
        $"01800180"
        $"02000040"
        $"0407E020"
        $"08381C10"
        $"10C00308"
        $"21000084"
        $"2207E044"
        $"42181842"
        $"44000022"
        $"447FFE22"
        $"84800121"
        $"889FF911"
        $"88A00511"
        $"88A42511"
        $"88A4A511"
        $"84A08521"
        $"44A18522"
        $"44A42522"
        $"40A3C502"
        $"20A00504"
        $"209FF904"
        $"00800100"
        $"00800100"
        $"0093F900"
        $"00800100"
        $"00800100"
        $"00800100"
        $"00FFFF00"
        $"00400200"
        $"007FFE00",
        /* Mask Bits */
        $"000FF000"
        $"007FFE00"
        $"01FFFF80"
        $"03FFFFC0"
        $"07FFFFE0"
        $"0FFFFFF0"
        $"1FFFFFF8"
        $"3FFFFFFC"
        $"3FFFFFFC"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"3FFFFFFC"
        $"3FFFFFFC"
        $"0FFFFFF0"
        $"07FFFFE0"
        $"03FFFFC0"
        $"01FFFF80"
        $"00FFFF00"
        $"00FFFF00"
        $"00FFFF00"
        $"007FFE00"
        $"007FFE00"
    }
};

/* 16x16 Small Application Icon & Mask ('ics#' 128) from ui/MacDrop/MacDrop-icon_16x16.png */
resource 'ics#' (128, "MacDrop Small", purgeable) {
    {
        /* Small Icon Bits */
        $"3FF8"
        $"4004"
        $"1FF0"
        $"2008"
        $"27C8"
        $"2828"
        $"2AA8"
        $"2008"
        $"2828"
        $"27C8"
        $"2008"
        $"29E8"
        $"2008"
        $"3FF8"
        $"1010"
        $"1FF0",
        /* Small Mask Bits */
        $"3FF8"
        $"4004"
        $"1FF0"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"1FF0"
        $"1FF0"
    }
};

/* 16x16 Small Icon List ('SICN' 128) for Menu Bar & MultiFinder Switcher */
resource 'SICN' (128, "MacDrop", purgeable) {
    {
        $"3FF8"
        $"4004"
        $"1FF0"
        $"2008"
        $"27C8"
        $"2828"
        $"2AA8"
        $"2008"
        $"2828"
        $"27C8"
        $"2008"
        $"29E8"
        $"2008"
        $"3FF8"
        $"1010"
        $"1FF0"
    }
};

/* Custom Finder Icon resources (-16455) for immediate icon display on all volumes */
resource 'ICN#' (-16455, "Custom Icon", purgeable) {
    {
        /* Icon Bits */
        $"000FF000"
        $"00700E00"
        $"01800180"
        $"02000040"
        $"0407E020"
        $"08381C10"
        $"10C00308"
        $"21000084"
        $"2207E044"
        $"42181842"
        $"44000022"
        $"447FFE22"
        $"84800121"
        $"889FF911"
        $"88A00511"
        $"88A42511"
        $"88A4A511"
        $"84A08521"
        $"44A18522"
        $"44A42522"
        $"40A3C502"
        $"20A00504"
        $"209FF904"
        $"00800100"
        $"00800100"
        $"0093F900"
        $"00800100"
        $"00800100"
        $"00800100"
        $"00FFFF00"
        $"00400200"
        $"007FFE00",
        /* Mask Bits */
        $"000FF000"
        $"007FFE00"
        $"01FFFF80"
        $"03FFFFC0"
        $"07FFFFE0"
        $"0FFFFFF0"
        $"1FFFFFF8"
        $"3FFFFFFC"
        $"3FFFFFFC"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"FFFFFFFF"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"7FFFFFFE"
        $"3FFFFFFC"
        $"3FFFFFFC"
        $"0FFFFFF0"
        $"07FFFFE0"
        $"03FFFFC0"
        $"01FFFF80"
        $"00FFFF00"
        $"00FFFF00"
        $"00FFFF00"
        $"007FFE00"
        $"007FFE00"
    }
};

resource 'ics#' (-16455, "Custom Small Icon", purgeable) {
    {
        /* Small Icon Bits */
        $"3FF8"
        $"4004"
        $"1FF0"
        $"2008"
        $"27C8"
        $"2828"
        $"2AA8"
        $"2008"
        $"2828"
        $"27C8"
        $"2008"
        $"29E8"
        $"2008"
        $"3FF8"
        $"1010"
        $"1FF0",
        /* Small Mask Bits */
        $"3FF8"
        $"4004"
        $"1FF0"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
        $"3FF8"
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
 * MultiFinder & System 7 SIZE Resource:
 * - canBackground: Receives CPU cycles when running in background
 * - multiFinderAware: Full MultiFinder event handling
 * - backgroundAndForeground: Standard windowed app that runs in background
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
    1024 * 1024,   /* Preferred size: 1024 KB (1 MB) */
    512 * 1024     /* Minimum size: 512 KB */
};

resource 'SIZE' (0) {
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
    1024 * 1024,
    512 * 1024
};

/* Menu Bar Specification */
resource 'MBAR' (128) {
    { 128, 129, 130 }
};

resource 'MENU' (128, "Apple") {
    128, textMenuProc,
    0x7FFFFFFD,
    enabled, apple,
    {
        "About MacDrop...", noIcon, noKey, noMark, plain,
        "-", noIcon, noKey, noMark, plain
    }
};

resource 'MENU' (129, "File") {
    129, textMenuProc,
    allEnabled,
    enabled, "File",
    {
        "Server Connection...", noIcon, "R", noMark, plain,
        "Download Folder...", noIcon, "F", noMark, plain,
        "Set Client Name...", noIcon, "M", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Close Window", noIcon, "W", noMark, plain,
        "Quit", noIcon, "Q", noMark, plain
    }
};

resource 'MENU' (130, "Options") {
    130, textMenuProc,
    allEnabled,
    enabled, "Options",
    {
        "Play Sound Chime", noIcon, "S", check, plain,
        "Notification Alert", noIcon, "N", check, plain
    }
};
