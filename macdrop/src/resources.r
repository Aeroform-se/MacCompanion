#include "Retro68.r"
#include "MacTypes.r"
#include "Processes.r"
#include "Types.r"
#include "SysTypes.r"
#include "Icons.r"
#include "Finder.r"
#include "Windows.r"
#include "Dialogs.r"
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

/* Owner Signature Resource ('MDRO' ID 0) */
type 'MDRO' as 'STR ';
resource 'MDRO' (0, "Owner Resource") {
    "MacDrop " APP_VERSION_SHORT " (" APP_CODENAME ")"
};

/* File Reference ('FREF' 128) & Bundle ('BNDL' 128) for appe */
resource 'FREF' (128, purgeable) {
    'appe',
    0,
    ""
};

resource 'BNDL' (128, purgeable) {
    'MDRO',
    0,
    {
        'ICN#', { 0, 128 },
        'FREF', { 0, 128 }
    }
};

/* 32x32 Large Application Icon & Mask ('ICN#' 128) from ui/MacDrop/macdrop-extension-icon.png */
resource 'ICN#' (128, "MacDrop", purgeable) {
    {
        /* Icon Bits */
        $"7FFFFFF0"
        $"80000008"
        $"803FC008"
        $"81C03808"
        $"86000608"
        $"881FC108"
        $"90603088"
        $"81800C48"
        $"A20F8228"
        $"D430611A"
        $"98C0189D"
        $"110F8489"
        $"11306441"
        $"127FF241"
        $"12800A41"
        $"149FC921"
        $"14A02921"
        $"14A8A921"
        $"14A22921"
        $"12A8AA41"
        $"12A72A49"
        $"90A0284D"
        $"D09FC88A"
        $"A0800808"
        $"80800808"
        $"8087C808"
        $"80800808"
        $"80800808"
        $"80FFF808"
        $"80401008"
        $"807FF008"
        $"7FFFFFF0",
        /* Mask Bits */
        $"7FFFFFF0"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"DFFFFFFA"
        $"9FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"9FFFFFFF"
        $"DFFFFFFA"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"7FFFFFF0"
    }
};

/* 16x16 Small Application Icon & Mask ('ics#' 128) from ui/MacDrop/MacDrop-16x16-update.png */
resource 'ics#' (128, "MacDrop Small", purgeable) {
    {
        /* Small Icon Bits */
        $"07C0"
        $"1830"
        $"2388"
        $"4C64"
        $"5014"
        $"0FE0"
        $"1010"
        $"17D0"
        $"1450"
        $"1450"
        $"17D0"
        $"1010"
        $"15D0"
        $"1010"
        $"0FE0"
        $"0000",
        /* Small Mask Bits */
        $"07C0"
        $"1830"
        $"2388"
        $"4C64"
        $"5014"
        $"0FE0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"0FE0"
        $"0000"
    }
};

/* 16x16 Menu Bar Icon Suite ('ICN#' 129 & 'ics#' 129) */
resource 'ICN#' (129, "Menu Bar Icon Suite Large", purgeable) {
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

/* 16x16 Menu Bar Icon Suite ('ics#' 129) */
resource 'ics#' (129, "Menu Bar Icon", purgeable) {
    {
        /* Small Icon Bits */
        $"07C0"
        $"1830"
        $"2388"
        $"4C64"
        $"5014"
        $"0FE0"
        $"1010"
        $"17D0"
        $"1450"
        $"1450"
        $"17D0"
        $"1010"
        $"15D0"
        $"1010"
        $"0FE0"
        $"0000",
        /* Small Mask Bits */
        $"07C0"
        $"1830"
        $"2388"
        $"4C64"
        $"5014"
        $"0FE0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"0FE0"
        $"0000"
    }
};

/* 16x16 Small Icon List ('SICN' 128) for Menu Bar & MultiFinder Switcher */
resource 'SICN' (128, "MacDrop", purgeable) {
    {
        $"07C0"
        $"1830"
        $"2388"
        $"4C64"
        $"5014"
        $"0FE0"
        $"1010"
        $"17D0"
        $"1450"
        $"1450"
        $"17D0"
        $"1010"
        $"15D0"
        $"1010"
        $"0FE0"
        $"0000"
    }
};

/* Custom Finder Icon resources (-16455) for immediate icon display on all volumes */
resource 'ICN#' (-16455, "Custom Icon", purgeable) {
    {
        /* Icon Bits */
        $"7FFFFFF0"
        $"80000008"
        $"803FC008"
        $"81C03808"
        $"86000608"
        $"881FC108"
        $"90603088"
        $"81800C48"
        $"A20F8228"
        $"D430611A"
        $"98C0189D"
        $"110F8489"
        $"11306441"
        $"127FF241"
        $"12800A41"
        $"149FC921"
        $"14A02921"
        $"14A8A921"
        $"14A22921"
        $"12A8AA41"
        $"12A72A49"
        $"90A0284D"
        $"D09FC88A"
        $"A0800808"
        $"80800808"
        $"8087C808"
        $"80800808"
        $"80800808"
        $"80FFF808"
        $"80401008"
        $"807FF008"
        $"7FFFFFF0",
        /* Mask Bits */
        $"7FFFFFF0"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"DFFFFFFA"
        $"9FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"1FFFFFFF"
        $"9FFFFFFF"
        $"DFFFFFFA"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"FFFFFFF8"
        $"7FFFFFF0"
    }
};

resource 'ics#' (-16455, "Custom Small Icon", purgeable) {
    {
        /* Small Icon Bits */
        $"07C0"
        $"1830"
        $"2388"
        $"4C64"
        $"5014"
        $"0FE0"
        $"1010"
        $"17D0"
        $"1450"
        $"1450"
        $"17D0"
        $"1010"
        $"15D0"
        $"1010"
        $"0FE0"
        $"0000",
        /* Small Mask Bits */
        $"07C0"
        $"1830"
        $"2388"
        $"4C64"
        $"5014"
        $"0FE0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"1FF0"
        $"0FE0"
        $"0000"
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
    doesActivateOnFGSwitch,
    backgroundAndForeground,
    getFrontClicks,
    ignoreAppDiedEvents,
    is32BitCompatible,
    isHighLevelEventAware,
    localAndRemoteHLEvents,
    notStationeryAware,
    dontUseTextEditServices,
    notDisplayManagerAware,
    reserved,
    reserved,
    1536 * 1024,
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
        "Send a file...", noIcon, "S", noMark, plain,
        "-", noIcon, noKey, noMark, plain,
        "Set Download Dir.", noIcon, "F", noMark, plain,
        "Set Client Name...", noIcon, "M", noMark, plain,
        "Set Server IP...", noIcon, "I", noMark, plain,
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
        "Play Sound Chime", noIcon, noKey, check, plain,
        "Notification Alert", noIcon, "N", check, plain
    }
};

/*
 * Custom Dialog & Item List for Folder Selection ('DLOG' 130 and 'DITL' 130)
 * Adds item 10 ("Choose") to the standard System 7 file dialog layout.
 */
resource 'DLOG' (130, "Choose Folder", purgeable) {
    {0, 0, 200, 344}, dBoxProc, invisible, noGoAway, 0,
    130, "", noAutoCenter
};

resource 'DITL' (130, "Choose Folder", purgeable) {
    {
        /* [1] Open button */
        {169, 252, 189, 332}, Button { enabled, "Open" },
        /* [2] Cancel button */
        {107, 252, 127, 332}, Button { enabled, "Cancel" },
        /* [3] Balloon Help item */
        {0, 0, 0, 0}, HelpItem { disabled, HMScanhdlg { -6042 } },
        /* [4] Volume user item */
        {8, 235, 24, 337}, UserItem { enabled },
        /* [5] Eject button */
        {32, 252, 52, 332}, Button { enabled, "Eject" },
        /* [6] Desktop button */
        {60, 252, 80, 332}, Button { enabled, "Desktop" },
        /* [7] File list user item */
        {29, 12, 193, 230}, UserItem { enabled },
        /* [8] Pop-up menu user item */
        {6, 12, 25, 230}, UserItem { enabled },
        /* [9] Divider line picture */
        {93, 251, 94, 333}, Picture { disabled, 11 },
        /* [10] Custom Choose button */
        {138, 252, 158, 332}, Button { enabled, "Choose" }
    }
};

/*
 * Custom Dialog & Item List for Send File Selection ('DLOG' 131 and 'DITL' 131)
 * Adds item 10 ("Send") to the standard System 7 file dialog layout.
 */
resource 'DLOG' (131, "Send File", purgeable) {
    {0, 0, 200, 344}, dBoxProc, invisible, noGoAway, 0,
    131, "", noAutoCenter
};

resource 'DITL' (131, "Send File", purgeable) {
    {
        /* [1] Open button */
        {169, 252, 189, 332}, Button { enabled, "Open" },
        /* [2] Cancel button */
        {107, 252, 127, 332}, Button { enabled, "Cancel" },
        /* [3] Balloon Help item */
        {0, 0, 0, 0}, HelpItem { disabled, HMScanhdlg { -6042 } },
        /* [4] Volume user item */
        {8, 235, 24, 337}, UserItem { enabled },
        /* [5] Eject button */
        {32, 252, 52, 332}, Button { enabled, "Eject" },
        /* [6] Desktop button */
        {60, 252, 80, 332}, Button { enabled, "Desktop" },
        /* [7] File list user item */
        {29, 12, 193, 230}, UserItem { enabled },
        /* [8] Pop-up menu user item */
        {6, 12, 25, 230}, UserItem { enabled },
        /* [9] Divider line picture */
        {93, 251, 94, 333}, Picture { disabled, 11 },
        /* [10] Custom Send button */
        {138, 252, 158, 332}, Button { enabled, "Send" }
    }
};

/*
 * Standard System 7 Modal Dialog for "Set Client Name" ('DLOG' 132 & 'DITL' 132)
 */
resource 'DLOG' (132, "Set Macintosh Name", purgeable) {
    {80, 100, 196, 412}, dBoxProc, invisible, noGoAway, 0,
    132, "Set Client Name", alertPositionMainScreen
};

resource 'DITL' (132, "Set Macintosh Name", purgeable) {
    {
        /* [1] Save button (default) */
        {80, 224, 100, 296}, Button { enabled, "Save" },
        /* [2] Cancel button */
        {80, 138, 100, 210}, Button { enabled, "Cancel" },
        /* [3] Prompt label */
        {14, 16, 30, 296}, StaticText { disabled, "Enter a name for this Mac on MacCompanion:" },
        /* [4] Client Name EditText */
        {40, 18, 56, 294}, EditText { enabled, "" }
    }
};

/*
 * Standard System 7 Modal Dialog for "Set Server IP" ('DLOG' 133 & 'DITL' 133)
 */
resource 'DLOG' (133, "Set Server IP", purgeable) {
    {75, 96, 219, 416}, dBoxProc, invisible, noGoAway, 0,
    133, "Set Server IP", alertPositionMainScreen
};

resource 'DITL' (133, "Set Server IP", purgeable) {
    {
        /* [1] Connect button (default) */
        {108, 228, 128, 304}, Button { enabled, "Connect" },
        /* [2] Cancel button */
        {108, 142, 128, 216}, Button { enabled, "Cancel" },
        /* [3] Scan LAN button */
        {108, 16, 128, 104}, Button { enabled, "Scan LAN" },
        /* [4] Prompt label */
        {14, 16, 30, 304}, StaticText { disabled, "MacCompanion Server IP Address:" },
        /* [5] Server IP EditText */
        {40, 18, 56, 302}, EditText { enabled, "" },
        /* [6] Status / Error feedback label */
        {70, 16, 98, 304}, StaticText { disabled, "" }
    }
};
