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

/* Owner Signature Resource ('MCAI' ID 0) */
type 'MCAI' as 'STR ';
resource 'MCAI' (0, "Owner Resource") {
    "MacAI " APP_VERSION_SHORT " (" APP_CODENAME ")"
};

/* File Reference ('FREF' 128) & Bundle ('BNDL' 128) for Custom Finder Icons */
resource 'FREF' (128) {
    'APPL',
    0,
    ""
};

resource 'BNDL' (128) {
    'MCAI',
    0,
    {
        'ICN#', { 0, 128 },
        'ics#', { 0, 128 },
        'FREF', { 0, 128 }
    }
};

/* 32x32 Large Application Icon & Mask ('ICN#' 128) from ui/MacAI/MacAI_32x32.png */
resource 'ICN#' (128, "MacAI", purgeable) {
    {
        /* Icon Bits */
        $"00000880"
        $"00200140"
        $"00502140"
        $"00880630"
        $"0050180C"
        $"0127E630"
        $"02000140"
        $"02000148"
        $"023FFC88"
        $"02400294"
        $"02400248"
        $"024C3248"
        $"02565A40"
        $"02524A40"
        $"024C3240"
        $"02400240"
        $"0243C240"
        $"02400240"
        $"023FFC40"
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
        $"00000F80"
        $"003FFFC0"
        $"007FFFC0"
        $"00FFFFF0"
        $"007FFFFC"
        $"01FFFFF0"
        $"03FFFFC0"
        $"03FFFFF8"
        $"03FFFFF8"
        $"03FFFFFC"
        $"03FFFFF8"
        $"03FFFFF8"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
        $"03FFFFC0"
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

/* 16x16 Small Application Icon & Mask ('ics#' 128) from ui/MacAI/MacAI_16x16.png */
resource 'ics#' (128, "MacAI Small", purgeable) {
    {
        /* Small Icon Bits */
        $"0045"
        $"00A4"
        $"1E4A"
        $"2004"
        $"27C4"
        $"2820"
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
        $"007F"
        $"00FC"
        $"1FFE"
        $"3FFC"
        $"3FFC"
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
    384 * 1024,
    256 * 1024
};

resource 'MBAR' (128) {
    { 128, 129, 130, 131, 132 }
};

resource 'MENU' (128, "Apple") {
    128, textMenuProc,
    0x7FFFFFFD,
    enabled, apple,
    {
        "About MacAI...", noIcon, noKey, noMark, plain,
        "-", noIcon, noKey, noMark, plain
    }
};

resource 'MENU' (129, "File") {
    129, textMenuProc,
    allEnabled,
    enabled, "File",
    {
        "New Chat", noIcon, "N", noMark, plain,
        "Search Chats...", noIcon, "K", noMark, plain,
        "Toggle Sidebar", noIcon, "S", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Quit", noIcon, "Q", noMark, plain
    }
};

resource 'MENU' (130, "Settings") {
    130, textMenuProc,
    allEnabled,
    enabled, "Settings",
    {
        "Server Connection...", noIcon, "R", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Use Google Gemini", noIcon, "1", noMark, plain,
        "Use ChatGPT (OpenAI)", noIcon, "2", noMark, plain,
        "Use Claude (Anthropic)", noIcon, "3", noMark, plain,
        "Use Groq", noIcon, "4", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Model: 3.5 Flash-Lite", noIcon, noKey, noMark, plain,
        "Model: 3.8 Flash", noIcon, noKey, noMark, plain,
        "Model: 3.1 Pro", noIcon, noKey, noMark, plain,
        "Model: 4 Argon", noIcon, noKey, noMark, plain,
        "Model: Custom", noIcon, noKey, noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Thinking: Fast", noIcon, noKey, noMark, plain,
        "Thinking: Balanced", noIcon, noKey, noMark, plain,
        "Thinking: Thinking", noIcon, noKey, noMark, plain
    }
};

resource 'MENU' (131, "Speech") {
    131, textMenuProc,
    allEnabled,
    enabled, "Speech",
    {
        "Read Replies Aloud", noIcon, "T", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Voice: Alloy (Neutral)", noIcon, noKey, noMark, plain,
        "Voice: Echo (Warm)", noIcon, noKey, noMark, plain,
        "Voice: Fable (Expressive)", noIcon, noKey, noMark, plain,
        "Voice: Onyx (Deep)", noIcon, noKey, noMark, plain,
        "Voice: Nova (Bright)", noIcon, noKey, noMark, plain,
        "Voice: Shimmer (Soft)", noIcon, noKey, noMark, plain,
        "Voice: Retro Synth (Robotic)", noIcon, noKey, noMark, plain
    }
};

resource 'MENU' (132, "View") {
    132, textMenuProc,
    allEnabled,
    enabled, "View",
    {
        "Sci-Fi Mode", noIcon, "F", noMark, plain
    }
};
