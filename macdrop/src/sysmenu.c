/*
 * MacDrop System Menu Extension (INIT)
 * Based on Matt Slot's SysMenu architecture & MacWifi by Anthony Super.
 */

#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Resources.h>
#include <Memory.h>
#include <Icons.h>
#include <Traps.h>
#include <OSUtils.h>
#include <ToolUtils.h>
#include <Files.h>
#include <Folders.h>
#include <Gestalt.h>
#include <Processes.h>
#include <string.h>
#include <stdio.h>

#include "Retro68Runtime.h"
#include "ShowInitIcon.h"
#include "version.h"
#include "shared_data.h"
#include "sysmenu.h"

static GlobalsRec        glob;
static MacDropSharedData sharedData;

static void InitSharedData(void);
static short CurResFileAsFSSpec(FSSpec *fileSpec);
static ProcPtr ApplyTrapPatch(short trap, ProcPtr patchPtr);
static pascal short DetachIcons(long iconType, Handle *iconHdl, void *data);
static pascal OSErr MacDropGestaltProc(OSType selector, long *response);
static void TryLaunchDaemon(void);
static pascal void Patched_InsertMenu(MenuHandle mHdl, short beforeID);
static pascal void Patched_DrawMenuBar(void);
static pascal long Patched_MenuSelect(Point where);
static pascal void Patched_SystemMenu(long result);
static void UpdateMenuContents(void);

static pascal OSErr MacDropGestaltProc(OSType selector, long *response)
{
    if (selector == MACDROP_GESTALT_TAG) {
        *response = (long)&sharedData;
        return noErr;
    }
    return gestaltUndefSelectorErr;
}

void _start(void)
{
    Handle myHandle = NULL;
    Handle h;

    RETRO68_RELOCATE();

    if (CurResFileAsFSSpec(&glob.homeFile)) {
        glob.homeFile.vRefNum = 0;
    }

    /* Draw MacDrop startup icon in the boot progress row */
    ShowInitIcon(128, true);

    SetZone(SystemZone());

    InitSharedData();

    /* Register Gestalt selector so any process can locate sharedData */
    {
        SelectorFunctionUPP upp = NewSelectorFunctionProc(MacDropGestaltProc);
        NewGestalt(MACDROP_GESTALT_TAG, upp);
    }

    /* Hook system menu traps in SystemZone */
    glob.saveInsertMenu  = (InsertMenuProcPtr)ApplyTrapPatch(_InsertMenu, (ProcPtr)Patched_InsertMenu);
    glob.saveDrawMenuBar = (DrawMenuProcPtr)ApplyTrapPatch(_DrawMenuBar, (ProcPtr)Patched_DrawMenuBar);
    glob.saveMenuSelect  = (MenuSelectProcPtr)ApplyTrapPatch(_MenuSelect, (ProcPtr)Patched_MenuSelect);
    glob.saveSystemMenu  = (SystemMenuProcPtr)ApplyTrapPatch(_SystemMenu, (ProcPtr)Patched_SystemMenu);

    glob.mHdl     = NULL;
    glob.menuID   = 0;
    glob.menuIcon = NULL;
    glob.sysMenus = (short **)NewHandle(0);

    /* Load icon suite for the menu bar title (ID 129) */
    if (kPrefIconID && !GetIconSuite(&myHandle, kPrefIconID, svAllAvailableData) &&
        !ForEachIconDo(myHandle, svAllAvailableData, NewIconActionProc((IconActionProcPtr)DetachIcons), 0)) {
        glob.menuIcon = myHandle;
    }

    SetZone(ApplicationZone());

    /* Detach INIT resource to keep code permanently resident in RAM */
    h = GetResource('INIT', 128);
    if (h) {
        DetachResource(h);
    }
}

static void InitSharedData(void)
{
    memset(&sharedData, 0, sizeof(sharedData));
    sharedData.DaemonRunning    = false;
    sharedData.ChannelConnected = false;
    sharedData.TransferActive   = false;
    sharedData.WindowVisible    = false;
    sharedData.SoundEnabled     = true;
    sharedData.NotifyEnabled    = true;
    sharedData.UpdateUI         = true;
    strcpy(sharedData.StatusText, "Daemon offline");
    sharedData.PendingCommand   = CMD_NONE;
}

static short CurResFileAsFSSpec(FSSpec *fileSpec)
{
    short i, err = 0;
    Str63 textBuff;
    FCBPBRec fcbPB;

    for (i = 0; i < sizeof(FSSpec); ((char *)fileSpec)[i++] = 0);

    fcbPB.ioCompletion = NULL;
    fcbPB.ioFCBIndx    = 0;
    fcbPB.ioVRefNum    = 0;
    fcbPB.ioRefNum     = CurResFile();
    fcbPB.ioNamePtr    = textBuff;
    err = PBGetFCBInfoSync(&fcbPB);
    if (err) return err;

    fileSpec->vRefNum = fcbPB.ioFCBVRefNum;
    fileSpec->parID   = fcbPB.ioFCBParID;
    BlockMove(textBuff, fileSpec->name, textBuff[0] + 1);

    return 0;
}

static ProcPtr ApplyTrapPatch(short trap, ProcPtr patchPtr)
{
    ProcPtr trapPtr;
    if (!patchPtr) return NULL;
    trapPtr = (ProcPtr)NGetTrapAddress(trap, (trap & 0x0800) ? ToolTrap : OSTrap);
    NSetTrapAddress(patchPtr, trap, (trap & 0x0800) ? ToolTrap : OSTrap);
    return trapPtr;
}

static pascal short DetachIcons(long iconType, Handle *iconHdl, void *data)
{
    short err = 0;
    if (*iconHdl) {
        DetachResource(*iconHdl);
        HNoPurge(*iconHdl);
        err = ResError();
    }
    return err;
}

static void TryLaunchDaemon(void)
{
    LaunchParamBlockRec launchPB;
    OSErr err;

    if (sharedData.DaemonRunning) return;

    if (glob.homeFile.vRefNum == 0) {
        short vRef = 0;
        long dirID = 0;
        if (FindFolder(kOnSystemDisk, kExtensionFolderType, kDontCreateFolder, &vRef, &dirID) == noErr) {
            FSMakeFSSpec(vRef, dirID, "\pMacDrop", &glob.homeFile);
        } else if (FindFolder(kOnSystemDisk, kSystemFolderType, kDontCreateFolder, &vRef, &dirID) == noErr) {
            FSMakeFSSpec(vRef, dirID, "\pMacDrop", &glob.homeFile);
        }
    }

    if (glob.homeFile.vRefNum == 0) {
        snprintf(sharedData.StatusText, sizeof(sharedData.StatusText), "No homeFile (vRef=0)");
        sharedData.UpdateUI = true;
        return;
    }

    memset(&launchPB, 0, sizeof(launchPB));
    launchPB.launchBlockID      = extendedBlock;
    launchPB.launchEPBLength    = extendedBlockLen;
    launchPB.launchFileFlags    = 0;
    launchPB.launchControlFlags = launchContinue | launchNoFileFlags | launchDontSwitch | launchUseMinimum;
    launchPB.launchAppSpec      = &glob.homeFile;

    err = LaunchApplication(&launchPB);
    if (err == noErr) {
        snprintf(sharedData.StatusText, sizeof(sharedData.StatusText), "Daemon starting...");
        sharedData.UpdateUI = true;
    } else {
        snprintf(sharedData.StatusText, sizeof(sharedData.StatusText), "Launch err %d", (int)err);
        sharedData.UpdateUI = true;
    }
}

static pascal void Patched_InsertMenu(MenuHandle mHdl, short beforeID)
{
    short menuID = (*mHdl)->menuID;
    InsertMenuProcPtr proc;

    /* Track all installed negative menu IDs to avoid collisions */
    if (!glob.menuID && glob.sysMenus && (menuID < 0)) {
        PtrAndHand((Ptr)&menuID, (Handle)glob.sysMenus, sizeof(menuID));
    }

    proc = glob.saveInsertMenu;
    (*proc)(mHdl, beforeID);
}

static pascal void Patched_DrawMenuBar(void)
{
    short i, found;
    DrawMenuProcPtr proc;
    Str32 textBuff;
    THz saveZone;

    if (!glob.mHdl) {
        saveZone = GetZone();
        SetZone(SystemZone());

        /* Find an unused negative menu ID starting at kPrefMenuID (-19999) */
        for (glob.menuID = kPrefMenuID; ; glob.menuID++) {
            i = GetHandleSize((Handle)glob.sysMenus) / sizeof(**glob.sysMenus);
            for (found = 0; !found && (i > 0);
                 found = ((*glob.sysMenus)[--i] == glob.menuID));
            if (!found) {
                DisposeHandle((Handle)glob.sysMenus);
                glob.sysMenus = NULL;
                break;
            }
        }

        /* Configure menu bar title: icon suite pointer with \01 prefix */
        if (glob.menuIcon) {
            textBuff[0] = 5;
            textBuff[1] = 1;
            BlockMove(&glob.menuIcon, textBuff + 2, sizeof(glob.menuIcon));
        } else {
            BlockMove("\pMacDrop", textBuff, sizeof("\pMacDrop"));
        }
        glob.mHdl = NewMenu(glob.menuID, textBuff);

        /* Insert menu (negative ID places it on far right next to Balloon Help) */
        InsertMenu(glob.mHdl, 0);

        SetZone(saveZone);
    }

    proc = glob.saveDrawMenuBar;
    (*proc)();
}

static void AddMenuItem(MenuHandle mHdl, ConstStr255Param text, Boolean enabled)
{
    short count;
    AppendMenu(mHdl, "\p ");
    count = CountMItems(mHdl);
    SetMenuItemText(mHdl, count, text);
    if (!enabled) {
        DisableItem(mHdl, count);
    }
}

static void UpdateMenuContents(void)
{
    short itemCount;
    char sbuf[96];
    Str255 pStatus;
    short slen;

    if (!glob.mHdl) return;

    itemCount = CountMItems(glob.mHdl);
    while (itemCount > 0) {
        DeleteMenuItem(glob.mHdl, 1);
        itemCount--;
    }

    /* Item 1: Status */
    if (sharedData.DaemonRunning) {
        if (sharedData.TransferActive) {
            snprintf(sbuf, sizeof(sbuf), "Status: Transferring...");
        } else if (sharedData.ChannelConnected) {
            snprintf(sbuf, sizeof(sbuf), "Server connection OK");
        } else {
            snprintf(sbuf, sizeof(sbuf), "Status: Ready / Offline");
        }
    } else {
        if (sharedData.StatusText[0]) {
            snprintf(sbuf, sizeof(sbuf), "Status: %s", sharedData.StatusText);
        } else {
            snprintf(sbuf, sizeof(sbuf), "Status: Daemon offline");
        }
    }

    slen = (short)strlen(sbuf);
    if (slen > 255) slen = 255;
    pStatus[0] = (unsigned char)slen;
    memcpy(pStatus + 1, sbuf, slen);
    AddMenuItem(glob.mHdl, pStatus, false);

    /* Item 2: Separator */
    AppendMenu(glob.mHdl, "\p-");

    /* Item 3: Window visibility toggle */
    if (sharedData.WindowVisible) {
        AddMenuItem(glob.mHdl, "\pHide MacDrop Window", true);
    } else {
        AddMenuItem(glob.mHdl, "\pOpen MacDrop Window", true);
    }

    /* Item 4: Send file */
    AddMenuItem(glob.mHdl, "\pSend a file...", !sharedData.TransferActive);

    /* Item 5: Separator */
    AppendMenu(glob.mHdl, "\p-");

    /* Item 6: Download directory */
    AddMenuItem(glob.mHdl, "\pSet Download Dir.", true);

    /* Item 7: Set client name */
    AddMenuItem(glob.mHdl, "\pSet Client Name...", true);

    /* Item 8: Separator */
    AppendMenu(glob.mHdl, "\p-");

    /* Item 9: Play Sound Chime */
    AddMenuItem(glob.mHdl, "\pPlay Sound Chime", true);
    if (sharedData.SoundEnabled) {
        SetItemMark(glob.mHdl, 9, checkMark);
    }

    /* Item 10: Notification Alert */
    AddMenuItem(glob.mHdl, "\pNotification Alert", true);
    if (sharedData.NotifyEnabled) {
        SetItemMark(glob.mHdl, 10, checkMark);
    }

    /* Item 11: Separator */
    AppendMenu(glob.mHdl, "\p-");

    /* Item 12: About */
    AddMenuItem(glob.mHdl, "\pAbout MacDrop...", true);
}

static pascal long Patched_MenuSelect(Point where)
{
    long value;
    MenuSelectProcPtr proc;
    THz saveZone;

    if (!sharedData.DaemonRunning) {
        TryLaunchDaemon();
    }

    if (glob.mHdl && (sharedData.UpdateUI || CountMItems(glob.mHdl) == 0)) {
        saveZone = GetZone();
        SetZone(SystemZone());
        UpdateMenuContents();
        SetZone(saveZone);
        sharedData.UpdateUI = false;
    }

    proc = glob.saveMenuSelect;
    value = (*proc)(where);
    return value;
}

static pascal void Patched_SystemMenu(long result)
{
    short menuID = (result & 0xFFFF0000) >> 16;
    short itemID = result & 0x0000FFFF;
    SystemMenuProcPtr proc;

    if (menuID == glob.menuID) {
        InitCursor();

        if (!sharedData.DaemonRunning) {
            TryLaunchDaemon();
        }

        switch (itemID) {
            case 3: /* Window toggle */
                sharedData.PendingCommand = sharedData.WindowVisible ? CMD_HIDE_WINDOW : CMD_OPEN_WINDOW;
                break;
            case 4: /* Send file */
                sharedData.PendingCommand = CMD_SEND_FILE;
                break;
            case 6: /* Download folder */
                sharedData.PendingCommand = CMD_CHOOSE_FOLDER;
                break;
            case 7: /* Client name */
                sharedData.PendingCommand = CMD_SET_NAME;
                break;
            case 9: /* Sound */
                sharedData.SoundEnabled = !sharedData.SoundEnabled;
                sharedData.PendingCommand = CMD_TOGGLE_SOUND;
                sharedData.UpdateUI = true;
                break;
            case 10: /* Notify */
                sharedData.NotifyEnabled = !sharedData.NotifyEnabled;
                sharedData.PendingCommand = CMD_TOGGLE_NOTIFY;
                sharedData.UpdateUI = true;
                break;
            case 12: /* About */
                sharedData.PendingCommand = CMD_ABOUT;
                break;
        }

        if (sharedData.DaemonRunning && sharedData.DaemonPSN.lowLongOfPSN != 0) {
            WakeUpProcess(&sharedData.DaemonPSN);
        }

        HiliteMenu(0);
    }

    proc = glob.saveSystemMenu;
    if (menuID != glob.menuID) {
        (*proc)(result);
    }
}
