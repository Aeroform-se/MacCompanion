#include <MacTypes.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Windows.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Devices.h>
#include <OSUtils.h>
#include <ToolUtils.h>
#include <SegLoad.h>
#include <StandardFile.h>
#include <Drag.h>
#include <Gestalt.h>
#include <Icons.h>
#include <Processes.h>
#include <LowMem.h>
#include <AppleEvents.h>
#include <Sound.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "version.h"
#include "assets.h"
#include "file_io.h"
#include "network.h"
#include "shared_data.h"

#define MENU_APPLE      128
#define MENU_FILE       129
#define MENU_OPTIONS    130

#define ITEM_ABOUT      1

#define ITEM_SEND_FILE  1
#define ITEM_DOWNLOAD   3
#define ITEM_SET_NAME   4
#define ITEM_SET_SERVER 5
#define ITEM_CLOSE      7
#define ITEM_QUIT       8

#define ITEM_SOUND      1
#define ITEM_NOTIFY     2

#define MAIN_WIN_WIDTH     390
#define MAIN_WIN_HEIGHT    240
#define COMPACT_WIN_HEIGHT 92

Boolean          gDone                 = false;
WindowPtr        gWindow               = NULL;
static MenuHandle gMenuApple           = NULL;
static MenuHandle gMenuFile            = NULL;
static MenuHandle gMenuOptions         = NULL;
static ProcessSerialNumber gMacDropPSN;

/* About Box Modal state */
static WindowPtr gAboutWindow          = NULL;
static Boolean   gWasHiddenBeforeAbout = false;
static Rect      gBtnAboutOkRect;

/* Transfer Notification Alert Window state (non-blocking, auto-clearing, multi-file accumulating) */
#define MAX_PENDING_NOTIFY_FILES 24
static WindowPtr gNotifyWindow            = NULL;
static Boolean   gWasHiddenBeforeNotify   = false;
static Rect      gBtnNotifyOkRect;
static char      gPendingNotifyFiles[MAX_PENDING_NOTIFY_FILES][36];
static short     gPendingNotifyCount      = 0;
static short     gPendingNotifyTotalCount = 0;
static Boolean   gPendingNotifyIsUpload   = false;
static char      gNotifyMessageText[512]  = "";

/* Compact Incoming File Window state */
static Boolean   gCompactReceiveMode   = false;
static Boolean   gAutoOpenedForReceive = false;
static Boolean   gOpeningModalDialog   = false;

static char      gServerIpInput[32]  = "10.0.2.2";
static char      gCachedDownloadPath[160] = "";
static Boolean   gLastDrawnTransferActive = false;

/* UI Rectangles for hit testing */
static Rect      gStatusRowRect;
static Rect      gDownloadFolderRowRect;
static Rect      gDropZoneRect;
static Rect      gBtnRefreshPeersRect;
#define MAX_TARGET_CARDS 4
static Rect      gTargetCardRects[MAX_TARGET_CARDS];
static char      gTargetCardIds[MAX_TARGET_CARDS][48];
static char      gTargetCardNames[MAX_TARGET_CARDS][48];
static short     gTargetCardCount = 0;
static short     gDragHoverTargetIdx = -1;
static MacDropSharedData *gSharedData = NULL;

/* Crisp 16x16 1-bit icons for Target Cards (rowBytes = 2, 16 rows) */
static const unsigned char kIconServer16x16[32] = {
    0x7F, 0xFE, 0x80, 0x01, 0x9F, 0xF9, 0x90, 0x09,
    0x90, 0x09, 0x90, 0x09, 0x9F, 0xF9, 0x80, 0x01,
    0x7F, 0xFE, 0x03, 0xC0, 0x3F, 0xFC, 0x40, 0x02,
    0x55, 0x52, 0x40, 0x02, 0x3F, 0xFC, 0x00, 0x00
};

static const unsigned char kIconMacPeer16x16[32] = {
    0x1F, 0xF8, 0x20, 0x04, 0x27, 0xE4, 0x24, 0x24,
    0x24, 0x24, 0x24, 0x24, 0x27, 0xE4, 0x20, 0x04,
    0x20, 0x34, 0x20, 0x04, 0x1F, 0xF8, 0x10, 0x08,
    0x1F, 0xF8, 0x3F, 0xFC, 0x00, 0x00, 0x00, 0x00
};

void UpdateProgressUI(void);
static void DrawTransferProgressOnly(void);
static void RefreshCachedDownloadPath(void);
static void PrepareServerIpInput(void);
static void EnsureFrontProcess(void);
static void ShowSetClientNameDialog(void);
static void ShowSetServerIpDialog(void);
static void SetCompactReceiveWindowMode(Boolean compact);
static void SwitchToFinder(void);
static void SyncSharedData(void);
static void UpdateMenuStates(void);
static void DrawNotifyWindow(void);
static void DismissNotifyWindow(Boolean acknowledgedByUser);

static void DrawCString(const char *str)
{
    if (str && *str) {
        DrawText(str, 0, (short)strlen(str));
    }
}

static short MeasureCString(const char *str)
{
    if (!str || !*str) return 0;
    return TextWidth(str, 0, (short)strlen(str));
}

static void FormatBytes(unsigned long bytes, char *out)
{
    if (bytes < 1024) {
        sprintf(out, "%lu B", bytes);
    } else if (bytes < 1048576) {
        sprintf(out, "%lu.%lu KB", bytes / 1024, (bytes % 1024) * 10 / 1024);
    } else {
        sprintf(out, "%lu.%lu MB", bytes / 1048576, (bytes % 1048576) * 10 / 1048576);
    }
}

static void DrawButton(const Rect *r, const char *label, Boolean isDefault)
{
    short tw;

    if (isDefault) {
        Rect outer = *r;
        InsetRect(&outer, -3, -3);
        PenSize(2, 2);
        FrameRoundRect(&outer, 12, 12);
        PenNormal();
    }

    EraseRoundRect(r, 8, 8);
    FrameRoundRect(r, 8, 8);

    tw = MeasureCString(label);
    MoveTo((r->left + r->right) / 2 - tw / 2, r->top + 14);
    DrawCString(label);
}

static void DrawProgressBar(short left, short top, short width, short height, unsigned long current, unsigned long total)
{
    Rect barR, fillR;
    short fillW = 0;

    SetRect(&barR, left, top, left + width, top + height);
    FrameRoundRect(&barR, 6, 6);

    if (total > 0 && current > 0) {
        if (current > total) current = total;
        fillW = (short)(((unsigned long long)(width - 4) * current) / total);
    }

    if (fillW > 0) {
        SetRect(&fillR, left + 2, top + 2, left + 2 + fillW, top + height - 2);
        FillRect(&fillR, &qd.black);
    }

    if (fillW < width - 4) {
        SetRect(&fillR, left + 2 + fillW, top + 2, left + width - 2, top + height - 2);
        FillRect(&fillR, &qd.white);
    }
}

static void DrawWindow(void);

static void RefreshCachedDownloadPath(void)
{
    Str255 volName;
    volName[0] = 0;
    GetVInfo(gDestVRefNum, volName, NULL, NULL);
    if (volName[0] > 0) {
        char volStr[64];
        short vLen = volName[0] > 63 ? 63 : volName[0];
        memcpy(volStr, &volName[1], vLen);
        volStr[vLen] = '\0';
        snprintf(gCachedDownloadPath, sizeof(gCachedDownloadPath), "%s:%s:", volStr, gDestFolderName);
    } else {
        snprintf(gCachedDownloadPath, sizeof(gCachedDownloadPath), ":%s:", gDestFolderName);
    }
}

static void PrepareServerIpInput(void)
{
    if (gServerIP != 0) {
        FormatIP(gServerIP, gServerIpInput);
    } else if (gSavedServerIP != 0) {
        FormatIP(gSavedServerIP, gServerIpInput);
    } else if (gOurIP != 0 && (gOurIP & 0xFFFFFF00UL) != 0x0A000200UL) {
        unsigned char b0 = (unsigned char)((gOurIP >> 24) & 0xFF);
        unsigned char b1 = (unsigned char)((gOurIP >> 16) & 0xFF);
        unsigned char b2 = (unsigned char)((gOurIP >> 8)  & 0xFF);
        snprintf(gServerIpInput, sizeof(gServerIpInput), "%u.%u.%u.", b0, b1, b2);
    } else if (gServerIpInput[0] == '\0') {
        strcpy(gServerIpInput, "10.0.2.2");
    }
}

static void SetCompactReceiveWindowMode(Boolean compact)
{
    if (!gWindow) return;
    if (compact) {
        if (!gCompactReceiveMode) {
            gCompactReceiveMode = true;
            SetWTitle(gWindow, "\pIncoming file - MacDrop");
            SizeWindow(gWindow, MAIN_WIN_WIDTH, COMPACT_WIN_HEIGHT, true);
            gLastDrawnTransferActive = false;
        }
    } else {
        if (gCompactReceiveMode) {
            gCompactReceiveMode = false;
            SetWTitle(gWindow, "\pMacDrop");
            SizeWindow(gWindow, MAIN_WIN_WIDTH, MAIN_WIN_HEIGHT, true);
            gLastDrawnTransferActive = false;
        }
    }
}

static void DrawTransferProgressOnly(void)
{
    Rect r, textEraseR;
    char buf[256];
    char curStr[32], totStr[32];
    short y;
    short barW;

    if (!gWindow || !IsWindowVisible(gWindow) || !gTransferActive) {
        DrawWindow();
        return;
    }

    SetPort(gWindow);
    r = gWindow->portRect;
    TextFont(kFontIDGeneva);
    TextSize(9);
    TextFace(normal);

    y = (gCompactReceiveMode && !gTransferIsUpload) ? 38 : 82;

    barW = r.right - 32 - 76;
    DrawProgressBar(16, y, barW, 14, gActiveTransferBytes, gActiveTransferTotal);
    y += 28;

    SetRect(&textEraseR, 16, y - 10, r.right - 16, y + 5);
    EraseRect(&textEraseR);

    FormatBytes(gActiveTransferBytes, curStr);
    FormatBytes(gActiveTransferTotal, totStr);

    MoveTo(16, y);
    if (gActiveTransferTotal > 0) {
        unsigned short pct = (unsigned short)(((unsigned long long)gActiveTransferBytes * 100) / gActiveTransferTotal);
        if (gTransferSpeedBps >= 1024) {
            sprintf(buf, "%s / %s (%u%%) - %lu.%lu kB/s", curStr, totStr, pct,
                    gTransferSpeedBps / 1024, (gTransferSpeedBps % 1024) * 10 / 1024);
        } else {
            sprintf(buf, "%s / %s (%u%%)", curStr, totStr, pct);
        }
    } else {
        sprintf(buf, "%s transferred", curStr);
    }
    DrawCString(buf);
}

void UpdateProgressUI(void)
{
    if (gTransferActive && !gTransferIsUpload) {
        if (!gWindow) return;
        if (!IsWindowVisible(gWindow)) {
            gAutoOpenedForReceive = true;
            SetCompactReceiveWindowMode(true);
            EnsureFrontProcess();
            ShowWindow(gWindow);
            SelectWindow(gWindow);
            SetPortWindowPort(gWindow);
            DrawMenuBar();
            SyncSharedData();
            UpdateMenuStates();
        }
    } else if (!gTransferActive && gAutoOpenedForReceive) {
        gAutoOpenedForReceive = false;
        if (gWindow && IsWindowVisible(gWindow)) {
            HideWindow(gWindow);
        }
        SetCompactReceiveWindowMode(false);
        if (!gNotifyWindow || !IsWindowVisible(gNotifyWindow)) {
            SwitchToFinder();
        } else {
            gWasHiddenBeforeNotify = true;
        }
        SyncSharedData();
        UpdateMenuStates();
        return;
    }

    if (!gWindow || !IsWindowVisible(gWindow)) return;
    if (gTransferActive && gLastDrawnTransferActive) {
        DrawTransferProgressOnly();
    } else {
        DrawWindow();
    }
}

static DragTrackingHandlerUPP gDragTrackingUPP = NULL;
static DragReceiveHandlerUPP  gDragReceiveUPP  = NULL;

static short FindTargetCardAtPoint(Point localPt)
{
    short i;
    for (i = 0; i < gTargetCardCount; i++) {
        if (PtInRect(localPt, &gTargetCardRects[i])) {
            return i;
        }
    }
    return -1;
}

static pascal OSErr MyDragTrackingHandler(DragTrackingMessage message, WindowRef theWindow, void *handlerRefCon, DragRef theDrag)
{
    Point localPt;
    short newIdx = -1;
    GrafPtr savePort;

    (void)handlerRefCon;
    (void)theDrag;

    if (!gWindow || theWindow != gWindow || gTransferActive || gCompactReceiveMode) {
        return noErr;
    }

    if (message == kDragTrackingInWindow) {
        GetPort(&savePort);
        SetPortWindowPort(gWindow);
        GetMouse(&localPt);
        newIdx = FindTargetCardAtPoint(localPt);
        if (newIdx == -1 && PtInRect(localPt, &gDropZoneRect) && gTargetCardCount > 0) {
            newIdx = 0; /* Default to Server card if hovering general drop box area */
        }
        if (newIdx != gDragHoverTargetIdx) {
            if (gDragHoverTargetIdx >= 0 && gDragHoverTargetIdx < gTargetCardCount) {
                InvertRoundRect(&gTargetCardRects[gDragHoverTargetIdx], 6, 6);
            }
            gDragHoverTargetIdx = newIdx;
            if (gDragHoverTargetIdx >= 0 && gDragHoverTargetIdx < gTargetCardCount) {
                InvertRoundRect(&gTargetCardRects[gDragHoverTargetIdx], 6, 6);
            }
        }
        SetPort(savePort);
    } else if (message == kDragTrackingLeaveWindow) {
        if (gDragHoverTargetIdx >= 0 && gDragHoverTargetIdx < gTargetCardCount) {
            GetPort(&savePort);
            SetPortWindowPort(gWindow);
            InvertRoundRect(&gTargetCardRects[gDragHoverTargetIdx], 6, 6);
            SetPort(savePort);
        }
        gDragHoverTargetIdx = -1;
    }

    return noErr;
}

static pascal OSErr MyDragReceiveHandler(WindowRef theWindow, void *handlerRefCon, DragRef theDrag)
{
    unsigned short numItems = 0;
    unsigned short i;
    OSErr err;
    Point localPt;
    short targetIdx;
    char targetId[48] = "SERVER";
    char targetName[48] = "MacCompanion Server";
    Boolean sentAny = false;

    (void)theWindow;
    (void)handlerRefCon;

    err = CountDragItems(theDrag, &numItems);
    if (err != noErr || numItems == 0) return dragNotAcceptedErr;

    SetPortWindowPort(gWindow);
    GetMouse(&localPt);
    targetIdx = FindTargetCardAtPoint(localPt);
    if (targetIdx < 0 && gDragHoverTargetIdx >= 0 && gDragHoverTargetIdx < gTargetCardCount) {
        targetIdx = gDragHoverTargetIdx;
    }
    gDragHoverTargetIdx = -1;

    if (targetIdx >= 0 && targetIdx < gTargetCardCount) {
        strncpy(targetId, gTargetCardIds[targetIdx], sizeof(targetId) - 1);
        targetId[sizeof(targetId) - 1] = '\0';
        strncpy(targetName, gTargetCardNames[targetIdx], sizeof(targetName) - 1);
        targetName[sizeof(targetName) - 1] = '\0';
    }

    for (i = 1; i <= numItems; i++) {
        ItemReference item;
        HFSFlavor hfs;
        Size sz = sizeof(hfs);

        if (GetDragItemReferenceNumber(theDrag, i, &item) == noErr) {
            if (GetFlavorData(theDrag, item, flavorTypeHFS, &hfs, &sz, 0) == noErr) {
                NetSendFileToTarget(&hfs.fileSpec, targetId, targetName);
                sentAny = true;
            }
        }
    }

    if (sentAny) {
        DrawWindow();
        return noErr;
    }
    return dragNotAcceptedErr;
}

static void SetupDragManager(void)
{
    long dragAttr = 0;
    if (Gestalt(gestaltDragMgrAttr, &dragAttr) == noErr && (dragAttr & (1 << gestaltDragMgrPresent))) {
        gDragTrackingUPP = NewDragTrackingHandlerUPP(MyDragTrackingHandler);
        gDragReceiveUPP  = NewDragReceiveHandlerUPP(MyDragReceiveHandler);
        if (gDragReceiveUPP && gWindow) {
            InstallReceiveHandler(gDragReceiveUPP, gWindow, NULL);
        }
        if (gDragTrackingUPP && gWindow) {
            InstallTrackingHandler(gDragTrackingUPP, gWindow, NULL);
        }
    }
}

static void CleanupDragManager(void)
{
    if (gDragReceiveUPP && gWindow) {
        RemoveReceiveHandler(gDragReceiveUPP, gWindow);
    }
    if (gDragTrackingUPP && gWindow) {
        RemoveTrackingHandler(gDragTrackingUPP, gWindow);
    }
}

/* ============================================================================
 * Apple Events (System 7 Quit & Open events for clean shutdown/reboot)
 * ============================================================================ */

static AEEventHandlerUPP gQuitAEUPP    = NULL;
static AEEventHandlerUPP gOpenAppAEUPP = NULL;
static AEEventHandlerUPP gOpenDocAEUPP = NULL;

static pascal OSErr HandleQuitAppleEvent(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefcon)
{
    (void)theAppleEvent;
    (void)reply;
    (void)handlerRefcon;
    if (gSharedData) {
        gSharedData->DaemonQuitting = true;
        gSharedData->DaemonRunning  = false;
    }
    gDone = true;
    return noErr;
}

static pascal OSErr HandleOpenAppAppleEvent(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefcon)
{
    (void)theAppleEvent;
    (void)reply;
    (void)handlerRefcon;
    return noErr;
}

static pascal OSErr HandleOpenDocAppleEvent(const AppleEvent *theAppleEvent, AppleEvent *reply, long handlerRefcon)
{
    (void)theAppleEvent;
    (void)reply;
    (void)handlerRefcon;
    return noErr;
}

static void SetupAppleEvents(void)
{
    long response = 0;
    if (Gestalt(gestaltAppleEventsAttr, &response) == noErr && (response & (1 << gestaltAppleEventsPresent))) {
        gQuitAEUPP    = NewAEEventHandlerUPP(HandleQuitAppleEvent);
        gOpenAppAEUPP = NewAEEventHandlerUPP(HandleOpenAppAppleEvent);
        gOpenDocAEUPP = NewAEEventHandlerUPP(HandleOpenDocAppleEvent);

        if (gQuitAEUPP)    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, gQuitAEUPP,    0, false);
        if (gOpenAppAEUPP) AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, gOpenAppAEUPP, 0, false);
        if (gOpenDocAEUPP) AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments,   gOpenDocAEUPP, 0, false);
    }
}

static void CleanupAppleEvents(void)
{
    if (gQuitAEUPP) {
        AERemoveEventHandler(kCoreEventClass, kAEQuitApplication, gQuitAEUPP, false);
        DisposeAEEventHandlerUPP(gQuitAEUPP);
        gQuitAEUPP = NULL;
    }
    if (gOpenAppAEUPP) {
        AERemoveEventHandler(kCoreEventClass, kAEOpenApplication, gOpenAppAEUPP, false);
        DisposeAEEventHandlerUPP(gOpenAppAEUPP);
        gOpenAppAEUPP = NULL;
    }
    if (gOpenDocAEUPP) {
        AERemoveEventHandler(kCoreEventClass, kAEOpenDocuments, gOpenDocAEUPP, false);
        DisposeAEEventHandlerUPP(gOpenDocAEUPP);
        gOpenDocAEUPP = NULL;
    }
}

static void BlitBitmap(const unsigned char *bits, short rowBytes,
                       short srcW, short srcH, short dstX, short dstY, short mode)
{
    BitMap srcBM;
    Rect srcR, dstR;
    GrafPtr curPort;

    GetPort(&curPort);
    srcBM.baseAddr = (Ptr)bits;
    srcBM.rowBytes = rowBytes;
    SetRect(&srcBM.bounds, 0, 0, rowBytes * 8, srcH);

    SetRect(&srcR, 0, 0, srcW, srcH);
    SetRect(&dstR, dstX, dstY, dstX + srcW, dstY + srcH);

    CopyBits(&srcBM, &curPort->portBits, &srcR, &dstR, mode, NULL);
}

static Boolean gInternalSwitch = false;

static void EnsureFrontProcess(void)
{
    ProcessSerialNumber frontPSN;
    Boolean isFront = false;

    GetFrontProcess(&frontPSN);
    SameProcess(&gMacDropPSN, &frontPSN, &isFront);
    if (!isFront) {
        short i;
        gInternalSwitch = true;
        for (i = 0; i < 30; i++) {
            EventRecord ev;
            SetFrontProcess(&gMacDropPSN);
            WaitNextEvent(everyEvent, &ev, 1, NULL);
            GetFrontProcess(&frontPSN);
            SameProcess(&gMacDropPSN, &frontPSN, &isFront);
            if (isFront) break;
        }
        gInternalSwitch = false;
    }
}

static void PascalToCStr(ConstStr255Param pStr, char *cStr, short maxLen)
{
    short len = pStr[0];
    if (len > maxLen - 1) len = maxLen - 1;
    memcpy(cStr, &pStr[1], len);
    cStr[len] = '\0';
}

static void CToPascalStr(const char *cStr, Str255 pStr)
{
    short len = cStr ? (short)strlen(cStr) : 0;
    if (len > 255) len = 255;
    pStr[0] = (unsigned char)len;
    if (len > 0) {
        memcpy(&pStr[1], cStr, len);
    }
}

static void OutlineDialogDefaultButton(DialogPtr dlg)
{
    short iType;
    Handle iHdl;
    Rect iRect;
    GrafPtr savePort;

    if (!dlg) return;
    GetPort(&savePort);
    SetPort(dlg);
    GetDialogItem(dlg, 1, &iType, &iHdl, &iRect);
    InsetRect(&iRect, -4, -4);
    PenSize(3, 3);
    FrameRoundRect(&iRect, 16, 16);
    PenNormal();
    SetPort(savePort);
}

/*
 * Standard System 7 Modal Dialog for "Set Client Name" (DLOG 132)
 * Uses native Mac Toolbox Dialog Manager controls with zero custom redraw flicker.
 */
static void ShowSetClientNameDialog(void)
{
    DialogPtr dlg;
    short itemHit = 0;
    short iType;
    Handle iHdl;
    Rect iRect;
    Str255 pText;
    char newName[64];
    Boolean wasVis = (gWindow && IsWindowVisible(gWindow));

    gOpeningModalDialog = true;
    EnsureFrontProcess();
    DrawMenuBar();

    dlg = GetNewDialog(132, NULL, (WindowPtr)-1);
    if (!dlg) {
        gOpeningModalDialog = false;
        return;
    }

    GetDialogItem(dlg, 4, &iType, &iHdl, &iRect);
    CToPascalStr(gClientName, pText);
    SetDialogItemText(iHdl, pText);
    SelectDialogItemText(dlg, 4, 0, 32767);

    ShowWindow(dlg);
    SelectWindow(dlg);
    OutlineDialogDefaultButton(dlg);

    while (itemHit != 1 && itemHit != 2) {
        ModalDialog(NULL, &itemHit);
    }

    if (itemHit == 1) {
        GetDialogItem(dlg, 4, &iType, &iHdl, &iRect);
        GetDialogItemText(iHdl, pText);
        PascalToCStr(pText, newName, sizeof(newName));
        SetClientName(newName);
        NetStopChannel();
        NetConnectChannel();
    }

    DisposeDialog(dlg);
    gOpeningModalDialog = false;

    if (wasVis && gWindow && IsWindowVisible(gWindow)) {
        SetPortWindowPort(gWindow);
        DrawWindow();
    } else if (!wasVis) {
        SwitchToFinder();
    }
    SyncSharedData();
    UpdateMenuStates();
}

/*
 * Standard System 7 Modal Dialog for "Set Server IP" (DLOG 133)
 * Uses native Mac Toolbox Dialog Manager controls with zero custom redraw flicker.
 */
static void ShowSetServerIpDialog(void)
{
    DialogPtr dlg;
    short itemHit = 0;
    short iType;
    Handle iHdl, statusHdl;
    Rect iRect;
    Str255 pText;
    char ipStr[32];
    Boolean wasVis = (gWindow && IsWindowVisible(gWindow));
    Boolean done = false;

    gOpeningModalDialog = true;
    EnsureFrontProcess();
    DrawMenuBar();
    PrepareServerIpInput();

    dlg = GetNewDialog(133, NULL, (WindowPtr)-1);
    if (!dlg) {
        gOpeningModalDialog = false;
        return;
    }

    GetDialogItem(dlg, 5, &iType, &iHdl, &iRect);
    CToPascalStr(gServerIpInput, pText);
    SetDialogItemText(iHdl, pText);
    SelectDialogItemText(dlg, 5, 0, 32767);

    GetDialogItem(dlg, 6, &iType, &statusHdl, &iRect);
    if (gChannelConnected && gServerIP != 0) {
        char curIp[32], msg[64];
        FormatIP(gServerIP, curIp);
        snprintf(msg, sizeof(msg), "Status: Connected to %s", curIp);
        CToPascalStr(msg, pText);
        SetDialogItemText(statusHdl, pText);
    } else {
        SetDialogItemText(statusHdl, "\pEnter server IP or click Scan LAN.");
    }

    ShowWindow(dlg);
    SelectWindow(dlg);
    OutlineDialogDefaultButton(dlg);

    while (!done) {
        ModalDialog(NULL, &itemHit);
        if (itemHit == 1) { /* Connect */
            ip_addr parsed = 0;
            GetDialogItem(dlg, 5, &iType, &iHdl, &iRect);
            GetDialogItemText(iHdl, pText);
            PascalToCStr(pText, ipStr, sizeof(ipStr));

            if (ParseIPAddress(ipStr, &parsed)) {
                SetDialogItemText(statusHdl, "\pConnecting to server...");
                if (NetVerifyServer(parsed, DEFAULT_HTTP_PORT)) {
                    FormatIP(gServerIP, gServerIpInput);
                    NetConnectChannel();
                    done = true;
                } else {
                    SetDialogItemText(statusHdl, "\pCould not reach server at this IP.");
                    SysBeep(1);
                }
            } else {
                SetDialogItemText(statusHdl, "\pInvalid IP address format (e.g. 192.168.1.50).");
                SysBeep(1);
            }
        } else if (itemHit == 2) { /* Cancel */
            done = true;
        } else if (itemHit == 3) { /* Scan LAN */
            SetDialogItemText(statusHdl, "\pScanning LAN for MacCompanion (UDP 6801)...");
            if (NetDiscoverServer()) {
                FormatIP(gServerIP, gServerIpInput);
                GetDialogItem(dlg, 5, &iType, &iHdl, &iRect);
                CToPascalStr(gServerIpInput, pText);
                SetDialogItemText(iHdl, pText);
                SelectDialogItemText(dlg, 5, 0, 32767);
                NetConnectChannel();
                done = true;
            } else {
                SetDialogItemText(statusHdl, "\pNo server found via broadcast. Enter IP above.");
                SysBeep(1);
            }
        }
    }

    DisposeDialog(dlg);
    gOpeningModalDialog = false;

    if (wasVis && gWindow && IsWindowVisible(gWindow)) {
        SetPortWindowPort(gWindow);
        DrawWindow();
    } else if (!wasVis) {
        SwitchToFinder();
    }
    SyncSharedData();
    UpdateMenuStates();
}

static Boolean gFolderChoosePressed = false;

static pascal short FolderDlgHookYD(short item, DialogRef theDialog, void *yourDataPtr)
{
    if (GetWRefCon(theDialog) != sfMainDialogRefCon)
        return item;

    if (gFolderChoosePressed) {
        short type;
        Rect r;
        ControlHandle openBtn = NULL;
        GetDialogItem(theDialog, sfItemOpenButton, &type, (Handle *)&openBtn, &r);
        if (openBtn) {
            HiliteControl(openBtn, 0);
        }
        return sfItemOpenButton;
    }

    if (item == 10) { /* Custom "Choose" button */
        short type;
        Rect r;
        ControlHandle openBtn = NULL;
        GetDialogItem(theDialog, sfItemOpenButton, &type, (Handle *)&openBtn, &r);

        gFolderChoosePressed = true;
        if (openBtn && (*openBtn)->contrlHilite == 0) {
            /* A folder in the list is highlighted; enter it first */
            return sfHookOpenFolder;
        } else {
            /* No folder highlighted; choose the current directory */
            if (openBtn) {
                HiliteControl(openBtn, 0);
            }
            return sfItemOpenButton;
        }
    }

    return item;
}

static pascal Boolean FolderFilterYD(CInfoPBPtr pb, void *yourDataPtr)
{
    /* Return true to filter out (hide) regular files from the list */
    if (!(pb->hFileInfo.ioFlAttrib & 0x10)) {
        return true;
    }
    return false;
}

static void ChooseDestinationWithFolderPicker(void)
{
    StandardFileReply reply;
    Point dlgPt = { -1, -1 };
    DlgHookYDUPP hookUPP;
    FileFilterYDUPP filterUPP;

    EnsureFrontProcess();
    DrawMenuBar();

    /* Pre-set the dialog to open into the currently configured download directory */
    if (gDestVRefNum != 0 && gDestDirID != 0) {
        LMSetSFSaveDisk(-gDestVRefNum);
        LMSetCurDirStore(gDestDirID);
    }

    hookUPP = NewDlgHookYDUPP(FolderDlgHookYD);
    filterUPP = NewFileFilterYDUPP(FolderFilterYD);
    gFolderChoosePressed = false;

    CustomGetFile(
        filterUPP,
        -1,
        NULL,
        &reply,
        130,            /* Custom DLOG / DITL ID */
        dlgPt,
        hookUPP,
        NULL,
        NULL,
        NULL,
        NULL
    );

    DisposeDlgHookYDUPP(hookUPP);
    DisposeFileFilterYDUPP(filterUPP);

    if (gFolderChoosePressed) {
        short vRefNum = -LMGetSFSaveDisk();
        long dirID    = LMGetCurDirStore();
        CInfoPBRec cinfo;
        Str255 dirName;

        gDestVRefNum = vRefNum;
        gDestDirID   = dirID;

        memset(&cinfo, 0, sizeof(cinfo));
        cinfo.dirInfo.ioVRefNum   = vRefNum;
        cinfo.dirInfo.ioDrDirID   = dirID;
        cinfo.dirInfo.ioNamePtr   = dirName;
        dirName[0] = 0;
        cinfo.dirInfo.ioFDirIndex = -1; /* get info on directory itself */

        if (PBGetCatInfoSync(&cinfo) == noErr && dirName[0] > 0) {
            PascalToCStr(dirName, gDestFolderName, sizeof(gDestFolderName));
        } else {
            strncpy(gDestFolderName, "Downloads", sizeof(gDestFolderName) - 1);
        }

        SavePreferences();
        RefreshCachedDownloadPath();
        if (gWindow && IsWindowVisible(gWindow)) {
            DrawWindow();
        }
    }
}

static pascal short SendDlgHookYD(short item, DialogRef theDialog, void *yourDataPtr)
{
    if (GetWRefCon(theDialog) != sfMainDialogRefCon)
        return item;

    /* Keep "Send" button (item 10) enabled/disabled state matching "Open" (item 1) */
    if (item == sfHookFirstCall || item == sfHookNullEvent ||
        item == sfHookChangeSelection || item == sfHookRebuildList) {
        short type;
        Rect r;
        ControlHandle openBtn = NULL;
        ControlHandle sendBtn = NULL;
        GetDialogItem(theDialog, sfItemOpenButton, &type, (Handle *)&openBtn, &r);
        GetDialogItem(theDialog, 10, &type, (Handle *)&sendBtn, &r);
        if (openBtn && sendBtn) {
            HiliteControl(sendBtn, (*openBtn)->contrlHilite);
        }
        return item;
    }

    if (item == 10) { /* User clicked "Send" button */
        short type;
        Rect r;
        ControlHandle openBtn = NULL;
        GetDialogItem(theDialog, sfItemOpenButton, &type, (Handle *)&openBtn, &r);
        if (openBtn && (*openBtn)->contrlHilite == 0) {
            return sfItemOpenButton;
        }
        return sfHookNullEvent;
    }

    return item;
}

static void SendFileWithPickerToTarget(const char *targetId, const char *targetName)
{
    StandardFileReply reply;
    Point dlgPt = { -1, -1 };
    DlgHookYDUPP hookUPP;

    EnsureFrontProcess();
    DrawMenuBar();

    hookUPP = NewDlgHookYDUPP(SendDlgHookYD);

    CustomGetFile(
        NULL,           /* fileFilter: show all files */
        -1,             /* numTypes: -1 means pass all types */
        NULL,
        &reply,
        131,            /* Custom DLOG / DITL ID for Send File */
        dlgPt,
        hookUPP,
        NULL,
        NULL,
        NULL,
        NULL
    );

    DisposeDlgHookYDUPP(hookUPP);

    if (reply.sfGood) {
        NetSendFileToTarget(&reply.sfFile, targetId, targetName);
        if (gWindow && IsWindowVisible(gWindow)) {
            DrawWindow();
        }
    }
}

static void SendFileWithPicker(void)
{
    SendFileWithPickerToTarget("SERVER", "MacCompanion Server");
}

static void DrawAboutWindow(void)
{
    Rect r;
    short cx, w;
    const char *verLine    = "MacDrop " APP_VERSION_SHORT;
    const char *codeLine   = "\"" APP_CODENAME "\"";
    const char *coffeeLine = "Like MacDrop? Buy me a coffee!";

    if (!gAboutWindow) return;
    SetPortWindowPort(gAboutWindow);
    r = gAboutWindow->portRect;
    EraseRect(&r);

    cx = (r.left + r.right) / 2;

    /* MacDrop 32x32 Logo centered at top */
    BlitBitmap(kLogoMacDrop32x32, 4, 32, 32, cx - 16, 12, srcCopy);

    TextFont(kFontIDGeneva);
    TextSize(9);
    TextFace(normal);
    TextMode(srcOr);

    w = MeasureCString(verLine);
    MoveTo(cx - (w / 2), 58);
    DrawCString(verLine);

    w = MeasureCString(codeLine);
    MoveTo(cx - (w / 2), 72);
    DrawCString(codeLine);

    w = MeasureCString(coffeeLine);
    MoveTo(cx - (w / 2), 90);
    DrawCString(coffeeLine);

    /* 58x58 QR code (~2x2cm on 9" Compact Mac CRT) centered below text */
    BlitBitmap(gAboutQrCodeBits, 8, 58, 58, cx - 29, 104, srcCopy);

    /* "OK" button */
    SetRect(&gBtnAboutOkRect, cx - 40, 176, cx + 40, 200);
    PaintRoundRect(&gBtnAboutOkRect, 6, 6);
    TextMode(srcBic);
    MoveTo(cx - 8, 192);
    DrawCString("OK");
    TextMode(srcOr);
}

static void CloseAboutWindow(void)
{
    if (gAboutWindow) {
        DisposeWindow(gAboutWindow);
        gAboutWindow = NULL;
    }
    if (gWasHiddenBeforeAbout) {
        SwitchToFinder();
    }
    SyncSharedData();
    UpdateMenuStates();
}

static void OpenAboutWindow(void)
{
    Rect aboutRect;
    short cardW = 240;
    short cardH = 216;
    short scrW = qd.screenBits.bounds.right - qd.screenBits.bounds.left;
    short scrH = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
    short left = qd.screenBits.bounds.left + (scrW / 2) - (cardW / 2);
    short top;

    /*
     * The card should render with its center at the visual screen center.
     * In windowed emulators (1024x768 framebuffer clamped to ~600-625p window),
     * rendering at top=168 lifts the window by half its height (108 px),
     * placing the window center right at y=276.
     */
    if (scrH >= 700) {
        top = qd.screenBits.bounds.top + 168;
    } else if (scrH >= 480) {
        top = qd.screenBits.bounds.top + 20 + ((scrH - 20 - cardH) / 3);
    } else {
        top = qd.screenBits.bounds.top + 20 + ((scrH - 20 - cardH) / 2);
    }

    EnsureFrontProcess();
    DrawMenuBar();

    if (gAboutWindow) {
        SelectWindow(gAboutWindow);
        return;
    }

    SetRect(&aboutRect, left, top, left + cardW, top + cardH);
    gAboutWindow = NewWindow(NULL, &aboutRect, "\p", true, plainDBox, (WindowPtr)-1, false, 0);
    if (gAboutWindow) {
        SelectWindow(gAboutWindow);
        SetPortWindowPort(gAboutWindow);
        DrawAboutWindow();
    }
}

static void DrawNotifyWindow(void)
{
    Rect r, textR;
    short msgLen;

    if (!gNotifyWindow || !IsWindowVisible(gNotifyWindow)) return;

    SetPortWindowPort(gNotifyWindow);
    r = gNotifyWindow->portRect;
    EraseRect(&r);

    /* MacDrop 32x32 icon on the left, matching native System 7 alert layout */
    BlitBitmap(kLogoMacDrop32x32, 4, 32, 32, 18, 16, srcCopy);

    msgLen = (short)strlen(gNotifyMessageText);
    if (msgLen > 110) {
        TextFont(kFontIDGeneva);
        TextSize(9);
    } else {
        TextFont(systemFont);
        TextSize(12);
    }
    TextFace(normal);
    TextMode(srcOr);

    SetRect(&textR, 62, 16, r.right - 16, r.bottom - 42);
    TETextBox(gNotifyMessageText, msgLen, &textR, teJustLeft);

    /* Default "OK" button at bottom-right */
    TextFont(systemFont);
    TextSize(12);
    TextFace(normal);
    SetRect(&gBtnNotifyOkRect, r.right - 84, r.bottom - 32, r.right - 16, r.bottom - 12);
    DrawButton(&gBtnNotifyOkRect, "OK", true);
}

static void DismissNotifyWindow(Boolean acknowledgedByUser)
{
    if (acknowledgedByUser) {
        gPendingNotifyCount      = 0;
        gPendingNotifyTotalCount = 0;
        gNotifyMessageText[0]    = '\0';
    }

    if (gNotifyWindow && IsWindowVisible(gNotifyWindow)) {
        HideWindow(gNotifyWindow);
    }

    if (acknowledgedByUser) {
        if (gWasHiddenBeforeNotify && (!gWindow || !IsWindowVisible(gWindow))) {
            gWasHiddenBeforeNotify = false;
            SwitchToFinder();
        } else {
            gWasHiddenBeforeNotify = false;
            if (gWindow && IsWindowVisible(gWindow)) {
                SelectWindow(gWindow);
                SetPortWindowPort(gWindow);
                DrawWindow();
            }
        }
        SyncSharedData();
        UpdateMenuStates();
    }
}

void OnTransferStarted(Boolean isUpload)
{
    /* Automatically clear (hide) any unacknowledged alert on screen when a new file begins,
     * while preserving gPendingNotifyFiles so previous file(s) are baked into the next alert! */
    if (gNotifyWindow && IsWindowVisible(gNotifyWindow)) {
        DismissNotifyWindow(false);
    }
    if (gPendingNotifyCount > 0 && gPendingNotifyIsUpload != isUpload) {
        gPendingNotifyCount      = 0;
        gPendingNotifyTotalCount = 0;
    }
}

void ShowTransferNotifyAlert(const char *fileName, Boolean isUpload)
{
    const char *cleanName = (fileName && fileName[0]) ? fileName : "file";
    short i;
    int pos;
    short cardW = 336;
    short cardH = 108;
    short scrW, scrH, left, top;
    Rect notifyRect;

    if (gSoundEnabled) {
        SysBeep(30);
    }

    if (!gNotifyEnabled) {
        gPendingNotifyCount      = 0;
        gPendingNotifyTotalCount = 0;
        return;
    }

    if (gPendingNotifyCount > 0 && gPendingNotifyIsUpload != isUpload) {
        gPendingNotifyCount      = 0;
        gPendingNotifyTotalCount = 0;
    }
    gPendingNotifyIsUpload = isUpload;

    gPendingNotifyTotalCount++;
    if (gPendingNotifyCount < MAX_PENDING_NOTIFY_FILES) {
        strncpy(gPendingNotifyFiles[gPendingNotifyCount], cleanName, sizeof(gPendingNotifyFiles[0]) - 1);
        gPendingNotifyFiles[gPendingNotifyCount][sizeof(gPendingNotifyFiles[0]) - 1] = '\0';
        gPendingNotifyCount++;
    }

    pos = snprintf(gNotifyMessageText, sizeof(gNotifyMessageText),
                   "%s", isUpload ? "Sent file " : "Received file ");
    for (i = 0; i < gPendingNotifyCount && pos < (int)sizeof(gNotifyMessageText) - 24; i++) {
        if (i > 0) {
            pos += snprintf(gNotifyMessageText + pos, sizeof(gNotifyMessageText) - (size_t)pos, ", ");
        }
        pos += snprintf(gNotifyMessageText + pos, sizeof(gNotifyMessageText) - (size_t)pos, "%s", gPendingNotifyFiles[i]);
    }
    if (gPendingNotifyTotalCount > gPendingNotifyCount && pos < (int)sizeof(gNotifyMessageText) - 16) {
        pos += snprintf(gNotifyMessageText + pos, sizeof(gNotifyMessageText) - (size_t)pos,
                        " (+%d more).", (int)(gPendingNotifyTotalCount - gPendingNotifyCount));
    } else if (pos > 0 && pos < (int)sizeof(gNotifyMessageText) - 2) {
        if (gNotifyMessageText[pos - 1] != '.') {
            gNotifyMessageText[pos++] = '.';
            gNotifyMessageText[pos]   = '\0';
        }
    }

    /* Hide the compact receive progress window if it was auto-opened for this transfer */
    if (gAutoOpenedForReceive) {
        gAutoOpenedForReceive = false;
        if (gWindow && IsWindowVisible(gWindow)) {
            HideWindow(gWindow);
        }
        SetCompactReceiveWindowMode(false);
        gWasHiddenBeforeNotify = true;
    } else if (!gNotifyWindow || !IsWindowVisible(gNotifyWindow)) {
        if (!gWasHiddenBeforeNotify) {
            gWasHiddenBeforeNotify = (!gWindow || !IsWindowVisible(gWindow));
        }
    }

    if (strlen(gNotifyMessageText) > 95) {
        cardH = 136;
    }

    scrW = qd.screenBits.bounds.right - qd.screenBits.bounds.left;
    scrH = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
    left = qd.screenBits.bounds.left + (scrW / 2) - (cardW / 2);
    if (scrH >= 700) {
        top = qd.screenBits.bounds.top + 150;
    } else if (scrH >= 480) {
        top = qd.screenBits.bounds.top + 20 + ((scrH - 20 - cardH) / 3);
    } else {
        top = qd.screenBits.bounds.top + 20 + ((scrH - 20 - cardH) / 3);
    }

    EnsureFrontProcess();
    DrawMenuBar();

    if (!gNotifyWindow) {
        SetRect(&notifyRect, left, top, left + cardW, top + cardH);
        gNotifyWindow = NewWindow(NULL, &notifyRect, "\p", false, dBoxProc, (WindowPtr)-1, false, 0);
    } else {
        MoveWindow(gNotifyWindow, left, top, false);
        SizeWindow(gNotifyWindow, cardW, cardH, true);
    }

    if (gNotifyWindow) {
        ShowWindow(gNotifyWindow);
        BringToFront(gNotifyWindow);
        SelectWindow(gNotifyWindow);
        SetPortWindowPort(gNotifyWindow);
        DrawNotifyWindow();
    }
    SyncSharedData();
    UpdateMenuStates();
}

static void SwitchToFinder(void)
{
    ProcessSerialNumber psn;
    ProcessInfoRec info;
    psn.highLongOfPSN = 0;
    psn.lowLongOfPSN  = kNoProcess;
    while (GetNextProcess(&psn) == noErr) {
        memset(&info, 0, sizeof(info));
        info.processInfoLength = sizeof(ProcessInfoRec);
        if (GetProcessInformation(&psn, &info) == noErr) {
            if (info.processSignature == 'MACS' || info.processType == 'FNDR') {
                SetFrontProcess(&psn);
                return;
            }
        }
    }
}

static void ConnectSharedData(void)
{
    long val = 0;
    if (Gestalt(MACDROP_GESTALT_TAG, &val) == noErr && val != 0) {
        gSharedData = (MacDropSharedData *)val;
    }
    if (gSharedData) {
        gSharedData->DaemonRunning    = true;
        gSharedData->ChannelConnected = gChannelConnected;
        gSharedData->TransferActive   = gTransferActive;
        gSharedData->WindowVisible    = (gWindow && IsWindowVisible(gWindow));
        gSharedData->SoundEnabled     = gSoundEnabled;
        gSharedData->NotifyEnabled    = gNotifyEnabled;
        gSharedData->DaemonPSN        = gMacDropPSN;
        if (gClientName[0]) {
            strncpy(gSharedData->ClientName, gClientName, sizeof(gSharedData->ClientName) - 1);
            gSharedData->ClientName[sizeof(gSharedData->ClientName) - 1] = '\0';
        }
        gSharedData->UpdateUI         = true;
    }
}

static void SyncSharedData(void)
{
    if (!gSharedData) return;
    gSharedData->DaemonRunning    = true;
    gSharedData->ChannelConnected = gChannelConnected;
    gSharedData->TransferActive   = gTransferActive;
    gSharedData->WindowVisible    = (gWindow && IsWindowVisible(gWindow));
    gSharedData->SoundEnabled     = gSoundEnabled;
    gSharedData->NotifyEnabled    = gNotifyEnabled;
    gSharedData->DaemonPSN        = gMacDropPSN;
    if (gClientName[0]) {
        strncpy(gSharedData->ClientName, gClientName, sizeof(gSharedData->ClientName) - 1);
        gSharedData->ClientName[sizeof(gSharedData->ClientName) - 1] = '\0';
    }
    gSharedData->UpdateUI         = true;
}

static void UpdateMenuStates(void)
{
    Boolean isVis = (gWindow && IsWindowVisible(gWindow));

    /* Update File Menu Item 6: "Open Window" vs "Hide Window" */
    if (gMenuFile) {
        SetMenuItemText(gMenuFile, ITEM_CLOSE, isVis ? "\pHide Window" : "\pOpen Window");
    }

    if (gMenuOptions) {
        CheckItem(gMenuOptions, ITEM_SOUND, gSoundEnabled);
        CheckItem(gMenuOptions, ITEM_NOTIFY, gNotifyEnabled);
    }

    SyncSharedData();
}

static void DrawWindow(void)
{
    Rect r;
    char buf[256];
    short y = 20;

    if (!gWindow || !IsWindowVisible(gWindow)) return;

    SetPort(gWindow);
    r = gWindow->portRect;
    EraseRect(&r);

    TextFont(kFontIDGeneva);
    TextSize(9);
    TextFace(normal);

    /* --- COMPACT INCOMING FILE VIEW ("Incoming file - MacDrop") --- */
    if (gCompactReceiveMode && gTransferActive && !gTransferIsUpload) {
        char curStr[32], totStr[32];
        short barW = r.right - 32 - 76;

        FormatBytes(gActiveTransferBytes, curStr);
        FormatBytes(gActiveTransferTotal, totStr);

        y = 22;
        MoveTo(16, y);
        TextFace(bold);
        DrawCString("Receiving: ");
        TextFace(normal);
        DrawCString(gActiveTransferName[0] ? gActiveTransferName : "Active stream...");
        y += 16; /* y = 38 */

        DrawProgressBar(16, y, barW, 14, gActiveTransferBytes, gActiveTransferTotal);

        SetRect(&gBtnCancelTransferRect, r.right - 16 - 70, y - 2, r.right - 16, y + 16);
        DrawButton(&gBtnCancelTransferRect, "Cancel", false);
        y += 28; /* y = 66 */

        MoveTo(16, y);
        if (gActiveTransferTotal > 0) {
            unsigned short pct = (unsigned short)(((unsigned long long)gActiveTransferBytes * 100) / gActiveTransferTotal);
            if (gTransferSpeedBps >= 1024) {
                sprintf(buf, "%s / %s (%u%%) - %lu.%lu kB/s", curStr, totStr, pct,
                        gTransferSpeedBps / 1024, (gTransferSpeedBps % 1024) * 10 / 1024);
            } else {
                sprintf(buf, "%s / %s (%u%%)", curStr, totStr, pct);
            }
        } else {
            sprintf(buf, "%s transferred", curStr);
        }
        DrawCString(buf);
        gLastDrawnTransferActive = true;
        return;
    }

    /* --- NORMAL MAIN WINDOW VIEW --- */

    /* Top Status line (clickable to open Server IP configuration) */
    SetRect(&gStatusRowRect, 16, y - 10, r.right - 16, y + 4);
    MoveTo(16, y);
    TextFace(bold);
    DrawCString("Status: ");
    TextFace(normal);
    if (gChannelConnected) {
        if (gClientName[0]) {
            snprintf(buf, sizeof(buf), "Connected to server as %s", gClientName);
        } else {
            snprintf(buf, sizeof(buf), "Connected to server");
        }
    } else if (gServerConnected) {
        snprintf(buf, sizeof(buf), "Connecting to server... (Click to set IP)");
    } else {
        snprintf(buf, sizeof(buf), "Searching for server... (Click to set IP)");
    }
    DrawCString(buf);
    y += 16;

    /* Destination folder (labeled "Download Dir:" to match menu) */
    SetRect(&gDownloadFolderRowRect, 16, y - 10, r.right - 16, y + 4);
    MoveTo(16, y);
    TextFace(bold);
    DrawCString("Download Dir: ");
    TextFace(normal);
    if (gCachedDownloadPath[0] == '\0') {
        RefreshCachedDownloadPath();
    }
    DrawCString(gCachedDownloadPath);
    y += 16;

    /* Divider */
    MoveTo(16, y);
    LineTo(r.right - 16, y);
    y += 14;

    /* --- Transfer Area --- */
    if (gTransferActive) {
        char curStr[32], totStr[32];
        FormatBytes(gActiveTransferBytes, curStr);
        FormatBytes(gActiveTransferTotal, totStr);

        MoveTo(16, y);
        TextFace(bold);
        if (gTransferIsUpload) {
            snprintf(buf, sizeof(buf), "Sending to %s: ",
                     gActiveUploadTargetName[0] ? gActiveUploadTargetName : "Server");
            DrawCString(buf);
        } else {
            DrawCString("Receiving: ");
        }
        TextFace(normal);
        DrawCString(gActiveTransferName[0] ? gActiveTransferName : "Active stream...");
        y += 16;

        /* Progress Bar with Cancel Button */
        short barW = r.right - 32 - 76;
        DrawProgressBar(16, y, barW, 14, gActiveTransferBytes, gActiveTransferTotal);

        SetRect(&gBtnCancelTransferRect, r.right - 16 - 70, y - 2, r.right - 16, y + 16);
        DrawButton(&gBtnCancelTransferRect, "Cancel", false);
        y += 28;

        MoveTo(16, y);
        if (gActiveTransferTotal > 0) {
            unsigned short pct = (unsigned short)(((unsigned long long)gActiveTransferBytes * 100) / gActiveTransferTotal);
            if (gTransferSpeedBps >= 1024) {
                sprintf(buf, "%s / %s (%u%%) - %lu.%lu kB/s", curStr, totStr, pct,
                        gTransferSpeedBps / 1024, (gTransferSpeedBps % 1024) * 10 / 1024);
            } else {
                sprintf(buf, "%s / %s (%u%%)", curStr, totStr, pct);
            }
        } else {
            sprintf(buf, "%s transferred", curStr);
        }
        DrawCString(buf);
        gLastDrawnTransferActive = true;
    } else {
        /* Drop Box Area with Server + Peer Macintosh Target Cards */
        Rect dropBox;
        short boxH = 152;
        short maxPeers = gOnlinePeerCount > (MAX_TARGET_CARDS - 1) ? (MAX_TARGET_CARDS - 1) : gOnlinePeerCount;
        short totalCards = 1 + maxPeers;
        short cardH = (totalCards >= 4) ? 26 : ((totalCards == 3) ? 34 : 36);
        short cardGap = (totalCards >= 4) ? 4 : 6;
        short cardTop;
        short i;

        SetRect(&dropBox, 16, y, r.right - 16, y + boxH);
        gDropZoneRect = dropBox;

        EraseRoundRect(&dropBox, 8, 8);
        FrameRoundRect(&dropBox, 8, 8);
        InsetRect(&dropBox, 2, 2);
        PenPat(&qd.gray);
        FrameRoundRect(&dropBox, 6, 6);
        PenNormal();

        /* Header inside Drop Box + [ Refresh ] pill button */
        TextFont(kFontIDGeneva);
        TextSize(9);
        TextFace(bold);
        MoveTo(dropBox.left + 10, dropBox.top + 15);
        DrawCString("Drop file on target or click to send:");
        TextFace(normal);

        SetRect(&gBtnRefreshPeersRect, dropBox.right - 64, dropBox.top + 4, dropBox.right - 8, dropBox.top + 19);
        EraseRoundRect(&gBtnRefreshPeersRect, 6, 6);
        FrameRoundRect(&gBtnRefreshPeersRect, 6, 6);
        MoveTo((gBtnRefreshPeersRect.left + gBtnRefreshPeersRect.right) / 2 - MeasureCString("Refresh") / 2,
               gBtnRefreshPeersRect.top + 11);
        DrawCString("Refresh");

        /* Populate Target Cards: Card 0 = Server, Cards 1..N = Online 68k Peers */
        gTargetCardCount = totalCards;
        strcpy(gTargetCardIds[0], "SERVER");
        strcpy(gTargetCardNames[0], "MacCompanion Server");
        for (i = 0; i < maxPeers; i++) {
            strncpy(gTargetCardIds[1 + i], gOnlinePeers[i].id, sizeof(gTargetCardIds[0]) - 1);
            gTargetCardIds[1 + i][sizeof(gTargetCardIds[0]) - 1] = '\0';
            strncpy(gTargetCardNames[1 + i], gOnlinePeers[i].name, sizeof(gTargetCardNames[0]) - 1);
            gTargetCardNames[1 + i][sizeof(gTargetCardNames[0]) - 1] = '\0';
        }

        cardTop = dropBox.top + 24;
        for (i = 0; i < totalCards; i++) {
            Rect cardR, badgeR;
            Boolean isServer = (i == 0);
            const char *titleStr = gTargetCardNames[i];
            const char *subStr   = isServer ? "Modern Browser / Web UI" : gOnlinePeers[i - 1].machine;
            short iconY;

            SetRect(&cardR, dropBox.left + 10, cardTop, dropBox.right - 10, cardTop + cardH);
            gTargetCardRects[i] = cardR;

            EraseRoundRect(&cardR, 6, 6);
            FrameRoundRect(&cardR, 6, 6);
            /* Subtle 1px bottom-right shadow */
            MoveTo(cardR.left + 3, cardR.bottom);
            LineTo(cardR.right, cardR.bottom);
            LineTo(cardR.right, cardR.top + 3);

            iconY = cardR.top + (cardH - 16) / 2;
            BlitBitmap(isServer ? kIconServer16x16 : kIconMacPeer16x16, 2, 16, 16, cardR.left + 8, iconY, srcCopy);

            if (cardH >= 34) {
                TextFace(bold);
                MoveTo(cardR.left + 30, cardR.top + 14);
                DrawCString(titleStr);
                TextFace(normal);
                MoveTo(cardR.left + 30, cardR.top + 27);
                DrawCString(subStr);
            } else {
                TextFace(bold);
                MoveTo(cardR.left + 30, cardR.top + 17);
                DrawCString(titleStr);
                TextFace(normal);
                DrawCString("  (");
                DrawCString(subStr);
                DrawCString(")");
            }

            /* Right-side "Send..." pill indicator */
            SetRect(&badgeR, cardR.right - 52, cardR.top + (cardH - 16) / 2, cardR.right - 8, cardR.top + (cardH - 16) / 2 + 16);
            FrameRoundRect(&badgeR, 6, 6);
            MoveTo((badgeR.left + badgeR.right) / 2 - MeasureCString("Send...") / 2, badgeR.top + 11);
            DrawCString("Send...");

            if (gDragHoverTargetIdx == i) {
                InvertRoundRect(&cardR, 6, 6);
            }

            cardTop += cardH + cardGap;
        }

        /* When only the Server is online (0 peer Macs), show a helpful hint in the remaining space */
        if (totalCards == 1) {
            short hintY = cardTop + 10;
            BlitBitmap(kLogoMacDrop32x32, 4, 32, 32, dropBox.left + 16, hintY, srcCopy);
            TextFace(bold);
            MoveTo(dropBox.left + 58, hintY + 13);
            DrawCString("No other 68k Macs currently online");
            TextFace(normal);
            MoveTo(dropBox.left + 58, hintY + 27);
            DrawCString("Run MacDrop on another Mac to drop Mac-to-Mac.");
        }

        gLastDrawnTransferActive = false;
    }
}

static void ProcessSharedCommand(void)
{
    if (!gSharedData || gSharedData->PendingCommand == CMD_NONE) return;

    MacDropCommand cmd = gSharedData->PendingCommand;
    gSharedData->PendingCommand = CMD_NONE;

    switch (cmd) {
        case CMD_OPEN_WINDOW:
            gAutoOpenedForReceive = false;
            SetCompactReceiveWindowMode(false);
            if (gChannelConnected || gServerIP != 0) {
                NetRefreshPeers();
            }
            EnsureFrontProcess();
            if (gWindow) {
                ShowWindow(gWindow);
                SelectWindow(gWindow);
                SetPortWindowPort(gWindow);
                DrawWindow();
            }
            DrawMenuBar();
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_HIDE_WINDOW:
            gAutoOpenedForReceive = false;
            if (gWindow && IsWindowVisible(gWindow)) {
                HideWindow(gWindow);
                SwitchToFinder();
            }
            SetCompactReceiveWindowMode(false);
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_SEND_FILE:
            SendFileWithPicker();
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_CHOOSE_FOLDER:
            ChooseDestinationWithFolderPicker();
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_SET_NAME:
            ShowSetClientNameDialog();
            break;

        case CMD_FIND_SERVER:
            ShowSetServerIpDialog();
            break;

        case CMD_TOGGLE_SOUND:
            gSoundEnabled = gSharedData->SoundEnabled;
            SavePreferences();
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_TOGGLE_NOTIFY:
            gNotifyEnabled = gSharedData->NotifyEnabled;
            SavePreferences();
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_ABOUT:
            gWasHiddenBeforeAbout = (!gWindow || !IsWindowVisible(gWindow));
            EnsureFrontProcess();
            DrawMenuBar();
            OpenAboutWindow();
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_QUIT:
            if (gSharedData) {
                gSharedData->DaemonQuitting = true;
                gSharedData->DaemonRunning  = false;
            }
            gDone = true;
            break;

        default:
            break;
    }
}

static void HandleMenuChoice(long choice)
{
    short menuID = HiWord(choice);
    short itemID = LoWord(choice);

    if (menuID == 0) return;

    switch (menuID) {
        case MENU_APPLE:
            if (itemID == ITEM_ABOUT) {
                Boolean isVis = (gWindow && IsWindowVisible(gWindow));
                gWasHiddenBeforeAbout = !isVis;
                OpenAboutWindow();
                SyncSharedData();
                UpdateMenuStates();
            } else {
                Str255 name;
                GetMenuItemText(gMenuApple, itemID, name);
                OpenDeskAcc(name);
            }
            break;

        case MENU_FILE:
            switch (itemID) {
                case ITEM_SEND_FILE:
                    if (!gTransferActive) {
                        SendFileWithPicker();
                    }
                    break;
                case ITEM_DOWNLOAD:
                    ChooseDestinationWithFolderPicker();
                    break;
                case ITEM_SET_NAME:
                    ShowSetClientNameDialog();
                    break;
                case ITEM_SET_SERVER:
                    ShowSetServerIpDialog();
                    break;
                case ITEM_CLOSE:
                    if (gWindow) {
                        gAutoOpenedForReceive = false;
                        if (IsWindowVisible(gWindow)) {
                            HideWindow(gWindow);
                            SetCompactReceiveWindowMode(false);
                            SwitchToFinder();
                        } else {
                            if (gChannelConnected || gServerIP != 0) {
                                NetRefreshPeers();
                            }
                            SetCompactReceiveWindowMode(false);
                            ShowWindow(gWindow);
                            SelectWindow(gWindow);
                            SetPortWindowPort(gWindow);
                            DrawWindow();
                        }
                        SyncSharedData();
                        UpdateMenuStates();
                    }
                    break;
                case ITEM_QUIT:
                    if (gSharedData) {
                        gSharedData->DaemonQuitting = true;
                        gSharedData->DaemonRunning  = false;
                    }
                    gDone = true;
                    break;
            }
            break;

        case MENU_OPTIONS:
            if (itemID == ITEM_SOUND) {
                gSoundEnabled = !gSoundEnabled;
                SavePreferences();
                UpdateMenuStates();
            } else if (itemID == ITEM_NOTIFY) {
                gNotifyEnabled = !gNotifyEnabled;
                if (!gNotifyEnabled && gNotifyWindow && IsWindowVisible(gNotifyWindow)) {
                    DismissNotifyWindow(true);
                }
                SavePreferences();
                UpdateMenuStates();
            }
            break;
    }

    HiliteMenu(0);
}

static void HandleMouseDown(EventRecord *evt)
{
    WindowPtr win;
    short part = FindWindow(evt->where, &win);

    switch (part) {
        case inMenuBar:
            UpdateMenuStates();
            HandleMenuChoice(MenuSelect(evt->where));
            break;

        case inSysWindow:
            SystemClick(evt, win);
            break;

        case inDrag:
            if (win == gWindow) {
                Rect bounds = qd.screenBits.bounds;
                InsetRect(&bounds, 4, 4);
                DragWindow(win, evt->where, &bounds);
            }
            break;

        case inGoAway:
            if (win == gWindow && TrackGoAway(win, evt->where)) {
                gAutoOpenedForReceive = false;
                HideWindow(gWindow);
                SetCompactReceiveWindowMode(false);
                SwitchToFinder();
                SyncSharedData();
                UpdateMenuStates();
            }
            break;

        case inContent:
            if (win == gAboutWindow) {
                CloseAboutWindow();
                return;
            }
            if (win == gNotifyWindow) {
                Point localPt = evt->where;
                Rect hitRect = gBtnNotifyOkRect;
                InsetRect(&hitRect, -4, -4);
                SetPortWindowPort(gNotifyWindow);
                GlobalToLocal(&localPt);
                if (PtInRect(localPt, &hitRect)) {
                    unsigned long finalTicks;
                    InvertRoundRect(&gBtnNotifyOkRect, 8, 8);
                    Delay(6, &finalTicks);
                    InvertRoundRect(&gBtnNotifyOkRect, 8, 8);
                    DismissNotifyWindow(true);
                } else if (win != FrontWindow()) {
                    SelectWindow(gNotifyWindow);
                }
                return;
            }
            if (gNotifyWindow && IsWindowVisible(gNotifyWindow)) {
                DismissNotifyWindow(true);
            }
            if (win != FrontWindow()) {
                SelectWindow(win);
            }
            if (win == gWindow) {
                Point localPt = evt->where;
                SetPort(gWindow);
                GlobalToLocal(&localPt);

                /* Ready View: Click on Status row -> Open Server IP configuration */
                if (!gTransferActive && !gCompactReceiveMode && PtInRect(localPt, &gStatusRowRect)) {
                    ShowSetServerIpDialog();
                    return;
                }

                /* Ready View: Click on Download Folder row -> Open Folder Picker */
                if (!gTransferActive && !gCompactReceiveMode && PtInRect(localPt, &gDownloadFolderRowRect)) {
                    ChooseDestinationWithFolderPicker();
                    return;
                }

                /* Ready View: Drop Box Click -> Refresh button, specific Target Card, or fallback Server */
                if (!gTransferActive && !gCompactReceiveMode && PtInRect(localPt, &gDropZoneRect)) {
                    short clickedIdx;
                    if (PtInRect(localPt, &gBtnRefreshPeersRect)) {
                        unsigned long finalTicks;
                        InvertRoundRect(&gBtnRefreshPeersRect, 6, 6);
                        Delay(5, &finalTicks);
                        InvertRoundRect(&gBtnRefreshPeersRect, 6, 6);
                        NetRefreshPeers();
                        DrawWindow();
                        return;
                    }
                    clickedIdx = FindTargetCardAtPoint(localPt);
                    if (clickedIdx >= 0 && clickedIdx < gTargetCardCount) {
                        unsigned long finalTicks;
                        char chosenId[48];
                        char chosenName[48];
                        strncpy(chosenId, gTargetCardIds[clickedIdx], sizeof(chosenId) - 1);
                        chosenId[sizeof(chosenId) - 1] = '\0';
                        strncpy(chosenName, gTargetCardNames[clickedIdx], sizeof(chosenName) - 1);
                        chosenName[sizeof(chosenName) - 1] = '\0';
                        InvertRoundRect(&gTargetCardRects[clickedIdx], 6, 6);
                        Delay(5, &finalTicks);
                        InvertRoundRect(&gTargetCardRects[clickedIdx], 6, 6);
                        SendFileWithPickerToTarget(chosenId, chosenName);
                        return;
                    }
                    SendFileWithPickerToTarget("SERVER", "MacCompanion Server");
                    return;
                }

                /* Active Transfer: Cancel Button */
                if (gTransferActive && PtInRect(localPt, &gBtnCancelTransferRect)) {
                    NetCancelActiveTransfer();
                    DrawWindow();
                    return;
                }
            }
            break;
    }
}

static void HandleKeyDown(EventRecord *evt)
{
    char key = (char)(evt->message & charCodeMask);

    /* If About Window is open, any key dismisses it */
    if (gAboutWindow) {
        CloseAboutWindow();
        return;
    }

    /* If Transfer Notification Alert is open, Return, Enter, Escape, or Space dismisses it */
    if (gNotifyWindow && IsWindowVisible(gNotifyWindow)) {
        if (key == '\r' || key == 3 || key == 27 || key == ' ') {
            unsigned long finalTicks;
            SetPortWindowPort(gNotifyWindow);
            InvertRoundRect(&gBtnNotifyOkRect, 8, 8);
            Delay(6, &finalTicks);
            InvertRoundRect(&gBtnNotifyOkRect, 8, 8);
            DismissNotifyWindow(true);
            return;
        }
    }

    /* Cancel active transfer via Cmd-. or Escape */
    if (gTransferActive) {
        if (((evt->modifiers & cmdKey) && key == '.') || key == 27) {
            NetCancelActiveTransfer();
            DrawWindow();
            return;
        }
    }

    if (evt->modifiers & cmdKey) {
        UpdateMenuStates();
        HandleMenuChoice(MenuKey(key));
        return;
    }
}

int main(void)
{
    Handle mbar;
    Rect r;
    unsigned long lastTransferBytes = 0;
    unsigned long lastDrawTick = 0;
    Boolean lastActive = false;
    Boolean lastChannel = false;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    GetCurrentProcess(&gMacDropPSN);

    mbar = GetNewMBar(128);
    if (mbar) {
        SetMenuBar(mbar);
        AppendResMenu(GetMenuHandle(MENU_APPLE), 'DRVR');
        /* Note: Do not call DrawMenuBar() here, as MacDrop starts in the background */
    }

    gMenuApple   = GetMenuHandle(MENU_APPLE);
    gMenuFile    = GetMenuHandle(MENU_FILE);
    gMenuOptions = GetMenuHandle(MENU_OPTIONS);

    InitFileManager();
    RefreshCachedDownloadPath();
    ConnectSharedData();

    /* Create status window (centered horizontally) - initially hidden */
    SetRect(&r, 40, 45, 430, 285);
    gWindow = NewWindow(NULL, &r, "\pMacDrop", false, documentProc, (WindowPtr)-1, true, 0);

    UpdateMenuStates();

    /* Initialize Drag Manager handlers if system supports it */
    SetupDragManager();

    /* Initialize Apple Events (handle system quit/restart signals) */
    SetupAppleEvents();

    /* Initialize Network & Port 6803 Passive Listener immediately */
    NetInit();
    NetStartListener();

    /* Non-blocking Startup Server Connection Flow:
     * 1. If a saved server IP exists (from MacDrop Prefs, SpotiMac Prefs, or MacAI Prefs),
     *    set it immediately and start an asynchronous reverse channel connection in 0ms.
     * 2. Otherwise try a quick 1-second UDP broadcast discovery; if not found, show the
     *    manual IP screen when the user opens the window while continuing background listening.
     */
    if (gSavedServerIP != 0) {
        /* When running inside Basilisk II SLIRP (10.0.2.15), prefer the on-subnet SLIRP host
         * gateway (10.0.2.2) via quick UDP probe so MacTCP uses 1460-byte Ethernet MSS
         * instead of 536-byte off-subnet MSS! */
        if ((gOurIP & 0xFFFFFF00UL) == 0x0A000200UL && (gSavedServerIP & 0xFFFFFF00UL) != 0x0A000200UL) {
            if (!NetDiscoverServer()) {
                NetSetServer(gSavedServerIP, gSavedServerPort ? gSavedServerPort : DEFAULT_HTTP_PORT);
            }
        } else {
            NetSetServer(gSavedServerIP, gSavedServerPort ? gSavedServerPort : DEFAULT_HTTP_PORT);
        }
        FormatIP(gServerIP, gServerIpInput);
        NetConnectChannel();
    } else {
        PrepareServerIpInput();
        if (NetDiscoverServer()) {
            FormatIP(gServerIP, gServerIpInput);
            NetConnectChannel();
        }
    }

    UpdateMenuStates();

    while (!gDone) {
        EventRecord event;
        short sleep = (gTransferActive || !gChannelConnected) ? 0 : 10;
        Boolean gotEvent;

        ProcessSharedCommand();

        /* If a transfer begins while window is hidden, open compact Incoming window (or full window if upload) */
        if (gTransferActive && !lastActive) {
            if (!gTransferIsUpload && gWindow && !IsWindowVisible(gWindow)) {
                gAutoOpenedForReceive = true;
                SetCompactReceiveWindowMode(true);
            }
            EnsureFrontProcess();
            if (gWindow) {
                ShowWindow(gWindow);
                SelectWindow(gWindow);
                SetPortWindowPort(gWindow);
            }
            DrawMenuBar();
            SyncSharedData();
            UpdateMenuStates();
        } else if (!gTransferActive && lastActive && gAutoOpenedForReceive) {
            gAutoOpenedForReceive = false;
            if (gWindow && IsWindowVisible(gWindow)) {
                HideWindow(gWindow);
            }
            SetCompactReceiveWindowMode(false);
            if (!gNotifyWindow || !IsWindowVisible(gNotifyWindow)) {
                SwitchToFinder();
            } else {
                gWasHiddenBeforeNotify = true;
            }
            SyncSharedData();
            UpdateMenuStates();
        }

        gotEvent = WaitNextEvent(everyEvent, &event, sleep, NULL);

        ProcessSharedCommand();

        /* Always run NetIdle so port 6803 listener & background pairing stay live! */
        NetIdle();

        if (!gTransferActive && gAutoOpenedForReceive) {
            gAutoOpenedForReceive = false;
            if (gWindow && IsWindowVisible(gWindow)) {
                HideWindow(gWindow);
            }
            SetCompactReceiveWindowMode(false);
            if (!gNotifyWindow || !IsWindowVisible(gNotifyWindow)) {
                SwitchToFinder();
            } else {
                gWasHiddenBeforeNotify = true;
            }
            SyncSharedData();
            UpdateMenuStates();
        }

        if (gChannelConnected && !lastChannel) {
            FormatIP(gServerIP, gServerIpInput);
        }

        Boolean needFullDraw = false;
        Boolean needProgressDraw = false;
        if (gTransferActive != lastActive || gChannelConnected != lastChannel) {
            needFullDraw = true;
            SyncSharedData();
            UpdateMenuStates();
        } else if (gTransferActive && (gActiveTransferBytes != lastTransferBytes)) {
            unsigned long nowTick = TickCount();
            if (nowTick - lastDrawTick >= 12) {
                needProgressDraw = true;
                lastDrawTick = nowTick;
            }
        }

        if (needFullDraw) {
            if (gWindow && IsWindowVisible(gWindow)) {
                DrawWindow();
            }
            lastActive = gTransferActive;
            lastChannel = gChannelConnected;
            lastTransferBytes = gActiveTransferBytes;
        } else if (needProgressDraw) {
            if (gWindow && IsWindowVisible(gWindow)) {
                DrawTransferProgressOnly();
            }
            lastTransferBytes = gActiveTransferBytes;
        }

        if (gotEvent) {
            switch (event.what) {
                case mouseDown:
                    HandleMouseDown(&event);
                    break;
                case keyDown:
                case autoKey:
                    HandleKeyDown(&event);
                    break;
                case osEvt:
                    if (((event.message >> 24) & 0xFF) == suspendResumeMessage) {
                        Boolean resuming = (event.message & resumeFlag) != 0;
                        if (resuming) {
                            DrawMenuBar();
                            /* Only auto-show gWindow if the user switched to MacDrop via Application menu
                             * (i.e. noPendingCommand, not an internal switch, and no modal dialog opening) */
                            if (!gInternalSwitch && !gOpeningModalDialog &&
                                (!gSharedData || gSharedData->PendingCommand == CMD_NONE) &&
                                !gAboutWindow && !gWasHiddenBeforeAbout &&
                                !(gNotifyWindow && IsWindowVisible(gNotifyWindow)) && !gWasHiddenBeforeNotify) {
                                if (gWindow) {
                                    if (!IsWindowVisible(gWindow)) {
                                        SetCompactReceiveWindowMode(false);
                                        ShowWindow(gWindow);
                                        SelectWindow(gWindow);
                                        SetPortWindowPort(gWindow);
                                    }
                                    DrawWindow();
                                    SyncSharedData();
                                    UpdateMenuStates();
                                }
                            } else {
                                if (gWindow && IsWindowVisible(gWindow)) {
                                    DrawWindow();
                                }
                                if (gAboutWindow && IsWindowVisible(gAboutWindow)) {
                                    DrawAboutWindow();
                                }
                                if (gNotifyWindow && IsWindowVisible(gNotifyWindow)) {
                                    DrawNotifyWindow();
                                }
                            }
                        }
                    }
                    break;
                case updateEvt:
                    if ((WindowPtr)event.message == gAboutWindow) {
                        BeginUpdate(gAboutWindow);
                        DrawAboutWindow();
                        EndUpdate(gAboutWindow);
                    } else if ((WindowPtr)event.message == gNotifyWindow) {
                        BeginUpdate(gNotifyWindow);
                        DrawNotifyWindow();
                        EndUpdate(gNotifyWindow);
                    } else if ((WindowPtr)event.message == gWindow) {
                        BeginUpdate(gWindow);
                        DrawWindow();
                        EndUpdate(gWindow);
                    }
                    break;
                case activateEvt:
                    if ((WindowPtr)event.message == gNotifyWindow && gNotifyWindow && IsWindowVisible(gNotifyWindow)) {
                        DrawNotifyWindow();
                    } else if (gWindow && IsWindowVisible(gWindow)) {
                        DrawWindow();
                    }
                    break;
                case kHighLevelEvent: {
                    OSType eventID = 0;
                    memcpy(&eventID, &event.where, sizeof(OSType));
                    if (event.message == kCoreEventClass && eventID == kAEQuitApplication) {
                        if (gSharedData) {
                            gSharedData->DaemonQuitting = true;
                            gSharedData->DaemonRunning  = false;
                        }
                        gDone = true;
                    }
                    AEProcessAppleEvent(&event);
                    break;
                }
            }
        }
    }

    if (gSharedData) {
        gSharedData->DaemonQuitting = true;
        gSharedData->DaemonRunning  = false;
        gSharedData->UpdateUI       = true;
    }

    CleanupAppleEvents();
    CleanupDragManager();
    NetStopChannel();
    NetStopListener();
    if (gAboutWindow) {
        DisposeWindow(gAboutWindow);
        gAboutWindow = NULL;
    }
    if (gNotifyWindow) {
        DisposeWindow(gNotifyWindow);
        gNotifyWindow = NULL;
    }
    if (gWindow) {
        DisposeWindow(gWindow);
    }

    ExitToShell();
    return 0;
}
