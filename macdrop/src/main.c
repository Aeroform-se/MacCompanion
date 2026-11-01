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
#define ITEM_CLOSE      6
#define ITEM_QUIT       7

#define ITEM_SOUND      1
#define ITEM_NOTIFY     2

typedef enum {
    CONN_READY = 0,
    CONN_SEARCHING,
    CONN_FAILED
} ConnState;

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

/* Set Client Name Modal state */
static Boolean   gModalSetName         = false;
static Boolean   gWasHiddenBeforeName  = false;
static char      gClientNameInput[64]  = "";

static ConnState gConnState          = CONN_READY;
static char      gServerIpInput[32]  = "10.0.2.2";
static char      gConnErrorMsg[64]   = "";

/* UI Rectangles for hit testing */
static Rect      gBtnRetryScanRect;
static Rect      gBtnConnectRect;
static Rect      gBtnCancelConnRect;
static Rect      gBtnSaveNameRect;
static Rect      gBtnCancelNameRect;
static Rect      gDownloadFolderRowRect;
static Rect      gDropZoneRect;

void UpdateProgressUI(void);
static void SwitchToFinder(void);
static void SyncSharedData(void);
static void UpdateMenuStates(void);

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

static void DrawShadowRoundRect(const Rect *r, short ovW, short ovH, short shadowOffset)
{
    Rect shadowR, bodyR;
    SetRect(&shadowR, r->left + shadowOffset, r->top + shadowOffset, r->right + shadowOffset, r->bottom + shadowOffset);
    FillRoundRect(&shadowR, ovW, ovH, &qd.black);

    bodyR = *r;
    EraseRoundRect(&bodyR, ovW, ovH);
    FrameRoundRect(&bodyR, ovW, ovH);
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

void UpdateProgressUI(void)
{
    if (!gWindow || !IsWindowVisible(gWindow)) return;
    DrawWindow();
}

static DragTrackingHandlerUPP gDragTrackingUPP = NULL;
static DragReceiveHandlerUPP  gDragReceiveUPP  = NULL;

static pascal OSErr MyDragTrackingHandler(DragTrackingMessage message, WindowRef theWindow, void *handlerRefCon, DragRef theDrag)
{
    return noErr;
}

static pascal OSErr MyDragReceiveHandler(WindowRef theWindow, void *handlerRefCon, DragRef theDrag)
{
    unsigned short numItems = 0;
    unsigned short i;
    OSErr err;

    err = CountDragItems(theDrag, &numItems);
    if (err != noErr || numItems == 0) return dragNotAcceptedErr;

    for (i = 1; i <= numItems; i++) {
        ItemReference item;
        HFSFlavor hfs;
        Size sz = sizeof(hfs);

        if (GetDragItemReferenceNumber(theDrag, i, &item) == noErr) {
            if (GetFlavorData(theDrag, item, flavorTypeHFS, &hfs, &sz, 0) == noErr) {
                NetSendFile(&hfs.fileSpec);
                DrawWindow();
                return noErr;
            }
        }
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

static void EnsureFrontProcess(void)
{
    ProcessSerialNumber frontPSN;
    Boolean isFront = false;

    GetFrontProcess(&frontPSN);
    SameProcess(&gMacDropPSN, &frontPSN, &isFront);
    if (!isFront) {
        short i;
        SetFrontProcess(&gMacDropPSN);
        for (i = 0; i < 30; i++) {
            EventRecord ev;
            WaitNextEvent(everyEvent, &ev, 2, NULL);
            GetFrontProcess(&frontPSN);
            SameProcess(&gMacDropPSN, &frontPSN, &isFront);
            if (isFront) break;
        }
    }
}

static void PascalToCStr(ConstStr255Param pStr, char *cStr, short maxLen)
{
    short len = pStr[0];
    if (len > maxLen - 1) len = maxLen - 1;
    memcpy(cStr, &pStr[1], len);
    cStr[len] = '\0';
}

static void ChooseDestinationWithFilePicker(void)
{
    StandardFileReply reply;
    EnsureFrontProcess();
    DrawMenuBar();
    StandardGetFile(NULL, -1, NULL, &reply);
    if (reply.sfGood) {
        CInfoPBRec cinfo;
        memset(&cinfo, 0, sizeof(cinfo));
        cinfo.hFileInfo.ioVRefNum   = reply.sfFile.vRefNum;
        cinfo.hFileInfo.ioDirID     = reply.sfFile.parID;
        cinfo.hFileInfo.ioNamePtr   = reply.sfFile.name;
        cinfo.hFileInfo.ioFDirIndex = 0;

        if (PBGetCatInfoSync(&cinfo) == noErr) {
            if (cinfo.hFileInfo.ioFlAttrib & 0x10) {
                /* Selected item is a directory */
                gDestVRefNum = reply.sfFile.vRefNum;
                gDestDirID   = cinfo.dirInfo.ioDrDirID;
                PascalToCStr(reply.sfFile.name, gDestFolderName, sizeof(gDestFolderName));
            } else {
                /* Selected item is a file: use its parent directory */
                CInfoPBRec dirCinfo;
                Str255 dirName;
                gDestVRefNum = reply.sfFile.vRefNum;
                gDestDirID   = reply.sfFile.parID;

                memset(&dirCinfo, 0, sizeof(dirCinfo));
                dirCinfo.dirInfo.ioVRefNum   = reply.sfFile.vRefNum;
                dirCinfo.dirInfo.ioDrDirID   = reply.sfFile.parID;
                dirCinfo.dirInfo.ioNamePtr   = dirName;
                dirCinfo.dirInfo.ioFDirIndex = -1; /* get info on directory itself */

                if (PBGetCatInfoSync(&dirCinfo) == noErr) {
                    PascalToCStr(dirName, gDestFolderName, sizeof(gDestFolderName));
                } else {
                    strncpy(gDestFolderName, "Downloads", sizeof(gDestFolderName) - 1);
                }
            }
            SavePreferences();
            if (gWindow && IsWindowVisible(gWindow)) {
                DrawWindow();
            }
        }
    }
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
    short left = (scrW - cardW) / 2;
    short top  = qd.screenBits.bounds.top + 20 + ((scrH - 20 - cardH) / 2);

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

static MacDropSharedData *gSharedData = NULL;

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
    char ipStr[32];
    short y = 20;

    if (!gWindow || !IsWindowVisible(gWindow)) return;

    SetPort(gWindow);
    r = gWindow->portRect;
    EraseRect(&r);

    TextFont(kFontIDGeneva);
    TextSize(9);
    TextFace(normal);

    /* --- VIEW A: SEARCHING FOR SERVER --- */
    if (gConnState == CONN_SEARCHING) {
        Rect cardR;
        short cx = (r.left + r.right) / 2;
        const char *t1 = "Searching for MacCompanion";
        const char *t2 = "server on your network (UDP 6801)...";
        const char *t3 = "Please ensure the server container is running.";

        SetRect(&cardR, cx - 145, 30, cx + 145, 175);
        DrawShadowRoundRect(&cardR, 12, 12, 2);

        TextFace(bold);
        MoveTo(cx - MeasureCString("MacCompanion Server") / 2, cardR.top + 22);
        DrawCString("MacCompanion Server");
        TextFace(normal);

        MoveTo(cx - MeasureCString(t1) / 2, cardR.top + 48);
        DrawCString(t1);
        MoveTo(cx - MeasureCString(t2) / 2, cardR.top + 62);
        DrawCString(t2);
        MoveTo(cx - MeasureCString(t3) / 2, cardR.top + 82);
        DrawCString(t3);

        SetRect(&gBtnCancelConnRect, cx - 40, cardR.bottom - 32, cx + 40, cardR.bottom - 12);
        DrawButton(&gBtnCancelConnRect, "Cancel", false);
        return;
    }

    /* --- VIEW B: SERVER CONNECTION FAILED / MANUAL ENTRY --- */
    if (gConnState == CONN_FAILED) {
        Rect cardR, boxR;
        short cx = (r.left + r.right) / 2;
        const char *t1 = "No MacCompanion server found";
        const char *t2 = "Or connect manually:";

        SetRect(&cardR, cx - 150, 20, cx + 150, 185);
        DrawShadowRoundRect(&cardR, 12, 12, 2);

        TextFace(bold);
        MoveTo(cx - MeasureCString(t1) / 2, cardR.top + 20);
        DrawCString(t1);
        TextFace(normal);

        SetRect(&gBtnRetryScanRect, cx - 55, cardR.top + 30, cx + 55, cardR.top + 50);
        DrawButton(&gBtnRetryScanRect, "Retry Scan", false);

        MoveTo(cardR.left + 15, cardR.top + 68);
        LineTo(cardR.right - 15, cardR.top + 68);

        MoveTo(cx - MeasureCString(t2) / 2, cardR.top + 84);
        DrawCString(t2);

        /* IP address input box */
        SetRect(&boxR, cx - 85, cardR.top + 94, cx + 85, cardR.top + 114);
        EraseRect(&boxR);
        FrameRect(&boxR);
        sprintf(buf, "%s_", gServerIpInput);
        MoveTo(boxR.left + 6, boxR.top + 14);
        DrawCString(buf);

        if (gConnErrorMsg[0]) {
            TextFace(italic);
            MoveTo(cx - MeasureCString(gConnErrorMsg) / 2, cardR.top + 128);
            DrawCString(gConnErrorMsg);
            TextFace(normal);
        }

        SetRect(&gBtnConnectRect, cx - 85, cardR.bottom - 32, cx - 5, cardR.bottom - 12);
        DrawButton(&gBtnConnectRect, "Connect", true);

        SetRect(&gBtnCancelConnRect, cx + 10, cardR.bottom - 32, cx + 85, cardR.bottom - 12);
        DrawButton(&gBtnCancelConnRect, "Cancel", false);
        return;
    }

    /* --- VIEW C: NORMAL READY STATUS VIEW --- */

    /* Top Title */
    MoveTo(16, y);
    TextFace(bold);
    DrawCString("MacDrop");
    TextFace(normal);
    DrawCString(" - Retro Push File Receiver (v" APP_VERSION_STR ")");
    y += 15;

    /* Machine identification */
    MoveTo(16, y);
    sprintf(buf, "Machine: %s (%s)", gClientName, GetMacMachineName());
    DrawCString(buf);
    y += 13;

    /* Network connection status */
    MoveTo(16, y);
    if (gChannelConnected) {
        FormatIP(gServerIP, ipStr);
        sprintf(buf, "Server: Connected to %s:%u (Push Channel Active)", ipStr, gServerPort);
    } else if (gServerConnected) {
        FormatIP(gServerIP, ipStr);
        sprintf(buf, "Server: Connecting to %s:%u...", ipStr, gServerPort);
    } else {
        sprintf(buf, "Server: Searching on local subnet (UDP %u)...", DEFAULT_UDP_PORT);
    }
    DrawCString(buf);
    y += 13;

    /* Local listener info */
    MoveTo(16, y);
    FormatIP(gOurIP, ipStr);
    sprintf(buf, "Receiver: Listening on %s:%u", ipStr, MACDROP_TCP_PORT);
    DrawCString(buf);
    y += 13;

    /* Destination folder (labeled "Download Folder:" per user request) */
    SetRect(&gDownloadFolderRowRect, 16, y - 10, r.right - 16, y + 4);
    MoveTo(16, y);
    {
        Str255 volName;
        volName[0] = 0;
        GetVInfo(gDestVRefNum, volName, NULL, NULL);
        if (volName[0] > 0) {
            char volStr[64];
            short vLen = volName[0] > 63 ? 63 : volName[0];
            memcpy(volStr, &volName[1], vLen);
            volStr[vLen] = '\0';
            snprintf(buf, sizeof(buf), "Download Folder: %s:%s:", volStr, gDestFolderName);
        } else {
            snprintf(buf, sizeof(buf), "Download Folder: :%s:", gDestFolderName);
        }
    }
    DrawCString(buf);
    y += 15;

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
            DrawCString("Sending to Server: ");
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
        y += 20;

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
    } else {
        /* Drop Box Area */
        Rect dropBox;
        short boxH = 46;
        short cx = (r.left + r.right) / 2;
        const char *t1 = "Drag file here from Finder";
        const char *t2 = "or Click to send to MacCompanion";

        SetRect(&dropBox, 16, y, r.right - 16, y + boxH);
        gDropZoneRect = dropBox;

        EraseRoundRect(&dropBox, 8, 8);
        FrameRoundRect(&dropBox, 8, 8);
        InsetRect(&dropBox, 2, 2);
        PenPat(&qd.gray);
        FrameRoundRect(&dropBox, 6, 6);
        PenNormal();

        TextFace(bold);
        MoveTo(cx - MeasureCString(t1) / 2, dropBox.top + 18);
        DrawCString(t1);
        TextFace(normal);
        MoveTo(cx - MeasureCString(t2) / 2, dropBox.top + 33);
        DrawCString(t2);

        y += boxH + 12;
        MoveTo(16, y);
        TextFace(italic);
        DrawCString("Also ready to receive files via drag-and-drop on server.");
        TextFace(normal);
    }

    /* --- MODAL DIALOG: SET CLIENT NAME --- */
    if (gModalSetName) {
        Rect dlgR, boxR;
        short cx = (r.left + r.right) / 2;
        const char *t1 = "Set Macintosh Name";
        const char *t2 = "Enter a name for this Mac on MacCompanion:";

        SetRect(&dlgR, cx - 145, 35, cx + 145, 165);
        DrawShadowRoundRect(&dlgR, 12, 12, 2);

        TextFace(bold);
        MoveTo(cx - MeasureCString(t1) / 2, dlgR.top + 22);
        DrawCString(t1);
        TextFace(normal);

        MoveTo(cx - MeasureCString(t2) / 2, dlgR.top + 46);
        DrawCString(t2);

        SetRect(&boxR, cx - 100, dlgR.top + 58, cx + 100, dlgR.top + 78);
        EraseRect(&boxR);
        FrameRect(&boxR);
        sprintf(buf, "%s_", gClientNameInput);
        MoveTo(boxR.left + 6, boxR.top + 14);
        DrawCString(buf);

        SetRect(&gBtnSaveNameRect, cx - 90, dlgR.bottom - 32, cx - 10, dlgR.bottom - 12);
        DrawButton(&gBtnSaveNameRect, "Save", true);

        SetRect(&gBtnCancelNameRect, cx + 15, dlgR.bottom - 32, cx + 90, dlgR.bottom - 12);
        DrawButton(&gBtnCancelNameRect, "Cancel", false);
    }

}

static void ProcessSharedCommand(void)
{
    if (!gSharedData || gSharedData->PendingCommand == CMD_NONE) return;

    MacDropCommand cmd = gSharedData->PendingCommand;
    gSharedData->PendingCommand = CMD_NONE;

    switch (cmd) {
        case CMD_OPEN_WINDOW:
            SetFrontProcess(&gMacDropPSN);
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
            if (gWindow && IsWindowVisible(gWindow)) {
                HideWindow(gWindow);
                SwitchToFinder();
            }
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_SEND_FILE: {
            StandardFileReply reply;
            EnsureFrontProcess();
            DrawMenuBar();
            StandardGetFile(NULL, -1, NULL, &reply);
            if (reply.sfGood) {
                NetSendFile(&reply.sfFile);
                if (gWindow && IsWindowVisible(gWindow)) {
                    DrawWindow();
                }
            }
            SyncSharedData();
            UpdateMenuStates();
            break;
        }

        case CMD_CHOOSE_FOLDER:
            ChooseDestinationWithFilePicker();
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_SET_NAME:
            SetFrontProcess(&gMacDropPSN);
            DrawMenuBar();
            gWasHiddenBeforeName = (!gWindow || !IsWindowVisible(gWindow));
            gModalSetName = true;
            strncpy(gClientNameInput, gClientName, sizeof(gClientNameInput) - 1);
            gClientNameInput[sizeof(gClientNameInput) - 1] = '\0';
            if (gWindow) {
                ShowWindow(gWindow);
                SelectWindow(gWindow);
                SetPortWindowPort(gWindow);
                DrawWindow();
            }
            SyncSharedData();
            UpdateMenuStates();
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
            EnsureFrontProcess();
            DrawMenuBar();
            gWasHiddenBeforeAbout = (!gWindow || !IsWindowVisible(gWindow));
            OpenAboutWindow();
            SyncSharedData();
            UpdateMenuStates();
            break;

        case CMD_QUIT:
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
                        StandardFileReply reply;
                        EnsureFrontProcess();
                        DrawMenuBar();
                        StandardGetFile(NULL, -1, NULL, &reply);
                        if (reply.sfGood) {
                            NetSendFile(&reply.sfFile);
                            DrawWindow();
                        }
                    }
                    break;
                case ITEM_DOWNLOAD:
                    ChooseDestinationWithFilePicker();
                    break;
                case ITEM_SET_NAME:
                    gWasHiddenBeforeName = (!gWindow || !IsWindowVisible(gWindow));
                    gModalSetName = true;
                    strncpy(gClientNameInput, gClientName, sizeof(gClientNameInput) - 1);
                    gClientNameInput[sizeof(gClientNameInput) - 1] = '\0';
                    DrawWindow();
                    break;
                case ITEM_CLOSE:
                    if (gWindow) {
                        if (IsWindowVisible(gWindow)) {
                            HideWindow(gWindow);
                            SwitchToFinder();
                        } else {
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
                HideWindow(gWindow);
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
            if (win != FrontWindow()) {
                SelectWindow(win);
            } else if (win == gWindow) {
                Point localPt = evt->where;
                GlobalToLocal(&localPt);

                /* Modal: Set Client Name */
                if (gModalSetName) {
                    if (PtInRect(localPt, &gBtnSaveNameRect)) {
                        SetClientName(gClientNameInput);
                        gModalSetName = false;
                        NetStopChannel();
                        NetConnectChannel();
                        if (gWasHiddenBeforeName) {
                            HideWindow(gWindow);
                            SwitchToFinder();
                        } else {
                            DrawWindow();
                        }
                        SyncSharedData();
                        UpdateMenuStates();
                        return;
                    }
                    if (PtInRect(localPt, &gBtnCancelNameRect)) {
                        gModalSetName = false;
                        if (gWasHiddenBeforeName) {
                            HideWindow(gWindow);
                            SwitchToFinder();
                        } else {
                            DrawWindow();
                        }
                        return;
                    }
                    return;
                }

                /* View 2: Server Searching */
                if (gConnState == CONN_SEARCHING) {
                    if (PtInRect(localPt, &gBtnCancelConnRect)) {
                        gConnState = CONN_READY;
                        DrawWindow();
                    }
                    return;
                }

                /* View 3: Server Failed / Manual */
                if (gConnState == CONN_FAILED) {
                    if (PtInRect(localPt, &gBtnRetryScanRect)) {
                        gConnState = CONN_SEARCHING;
                        gConnErrorMsg[0] = '\0';
                        DrawWindow();
                        if (NetDiscoverServer()) {
                            FormatIP(gServerIP, gServerIpInput);
                            gConnState = CONN_READY;
                            NetConnectChannel();
                        } else {
                            gConnState = CONN_FAILED;
                        }
                        DrawWindow();
                        return;
                    }
                    if (PtInRect(localPt, &gBtnConnectRect)) {
                        ip_addr parsed = 0;
                        if (ParseIPAddress(gServerIpInput, &parsed)) {
                            if (NetVerifyServer(parsed, DEFAULT_HTTP_PORT)) {
                                gConnState = CONN_READY;
                                NetConnectChannel();
                                DrawWindow();
                            } else {
                                strcpy(gConnErrorMsg, "Could not reach server at this IP");
                                DrawWindow();
                            }
                        } else {
                            strcpy(gConnErrorMsg, "Invalid IP address syntax");
                            DrawWindow();
                        }
                        return;
                    }
                    if (PtInRect(localPt, &gBtnCancelConnRect)) {
                        gConnState = CONN_READY;
                        DrawWindow();
                        return;
                    }
                }

                /* Ready View: Click on Download Folder row -> Open File Picker */
                if (!gTransferActive && gConnState == CONN_READY && PtInRect(localPt, &gDownloadFolderRowRect)) {
                    ChooseDestinationWithFilePicker();
                    return;
                }

                /* Ready View: Drop Box Click -> Open Standard File Picker */
                if (!gTransferActive && gConnState == CONN_READY && PtInRect(localPt, &gDropZoneRect)) {
                    StandardFileReply reply;
                    EnsureFrontProcess();
                    DrawMenuBar();
                    StandardGetFile(NULL, -1, NULL, &reply);
                    if (reply.sfGood) {
                        NetSendFile(&reply.sfFile);
                        DrawWindow();
                    }
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

    /* Modal: Set Client Name typing */
    if (gModalSetName) {
        short len = (short)strlen(gClientNameInput);
        if (key == '\r' || key == 3) {
            SetClientName(gClientNameInput);
            gModalSetName = false;
            NetStopChannel();
            NetConnectChannel();
            if (gWasHiddenBeforeName) {
                HideWindow(gWindow);
                SwitchToFinder();
            } else {
                DrawWindow();
            }
            SyncSharedData();
            UpdateMenuStates();
        } else if (key == 27) {
            gModalSetName = false;
            if (gWasHiddenBeforeName) {
                HideWindow(gWindow);
                SwitchToFinder();
            } else {
                DrawWindow();
            }
        } else if (key == 8 && len > 0) {
            gClientNameInput[len - 1] = '\0';
            DrawWindow();
        } else if (key >= 32 && key <= 126 && len < (short)(sizeof(gClientNameInput) - 2)) {
            gClientNameInput[len]     = key;
            gClientNameInput[len + 1] = '\0';
            DrawWindow();
        }
        return;
    }

    /* View: Server Failed Manual IP typing */
    if (gConnState == CONN_FAILED) {
        short len = (short)strlen(gServerIpInput);
        if (key == '\r' || key == 3) {
            ip_addr parsed = 0;
            if (ParseIPAddress(gServerIpInput, &parsed)) {
                if (NetVerifyServer(parsed, DEFAULT_HTTP_PORT)) {
                    gConnState = CONN_READY;
                    NetConnectChannel();
                    DrawWindow();
                } else {
                    strcpy(gConnErrorMsg, "Could not reach server at this IP");
                    DrawWindow();
                }
            } else {
                strcpy(gConnErrorMsg, "Invalid IP address syntax");
                DrawWindow();
            }
        } else if (key == 27) {
            gConnState = CONN_READY;
            DrawWindow();
        } else if (key == 8 && len > 0) {
            gServerIpInput[len - 1] = '\0';
            DrawWindow();
        } else if ((key == '.' || (key >= '0' && key <= '9')) && len < (short)(sizeof(gServerIpInput) - 2)) {
            gServerIpInput[len]     = key;
            gServerIpInput[len + 1] = '\0';
            DrawWindow();
        }
        return;
    }

    if (gConnState == CONN_SEARCHING && key == 27) {
        gConnState = CONN_READY;
        DrawWindow();
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
    ConnectSharedData();

    /* Create status window (centered horizontally) - initially hidden */
    SetRect(&r, 40, 45, 430, 275);
    gWindow = NewWindow(NULL, &r, "\pMacDrop", false, documentProc, (WindowPtr)-1, true, 0);

    UpdateMenuStates();

    /* Initialize Drag Manager handlers if system supports it */
    SetupDragManager();

    /* Initialize Network */
    NetInit();
    NetStartListener();

    /* Startup Server Connection Flow:
     * 1. If saved server IP exists in preferences, verify it.
     * 2. If valid, connect reverse channel and set CONN_READY.
     * 3. If no saved server IP or unreachable, run UDP discovery.
     * 4. If UDP discovery fails, display CONN_FAILED manual IP entry screen.
     */
    if (gSavedServerIP != 0) {
        FormatIP(gSavedServerIP, gServerIpInput);
        if (NetVerifyServer(gSavedServerIP, gSavedServerPort)) {
            gConnState = CONN_READY;
            NetConnectChannel();
        } else {
            /* Try auto-discovery */
            if (NetDiscoverServer()) {
                FormatIP(gServerIP, gServerIpInput);
                gConnState = CONN_READY;
                NetConnectChannel();
            } else {
                gConnState = CONN_FAILED;
            }
        }
    } else {
        if (NetDiscoverServer()) {
            FormatIP(gServerIP, gServerIpInput);
            gConnState = CONN_READY;
            NetConnectChannel();
        } else {
            gConnState = CONN_FAILED;
        }
    }

    UpdateMenuStates();

    while (!gDone) {
        EventRecord event;
        short sleep = gTransferActive ? 0 : 20;
        Boolean gotEvent;

        ProcessSharedCommand();

        /* If inbound transfer begins, reveal and raise window */
        if (gTransferActive && !lastActive) {
            SetFrontProcess(&gMacDropPSN);
            if (gWindow) {
                ShowWindow(gWindow);
                SelectWindow(gWindow);
                SetPortWindowPort(gWindow);
            }
            DrawMenuBar();
            SyncSharedData();
            UpdateMenuStates();
        }

        gotEvent = WaitNextEvent(everyEvent, &event, sleep, NULL);

        ProcessSharedCommand();

        if (gConnState == CONN_READY) {
            NetIdle();
        }

        Boolean needDraw = false;
        if (gTransferActive != lastActive || gChannelConnected != lastChannel) {
            needDraw = true;
            SyncSharedData();
            UpdateMenuStates();
        } else if (gTransferActive && (gActiveTransferBytes != lastTransferBytes)) {
            unsigned long nowTick = TickCount();
            if (nowTick - lastDrawTick >= 15) {
                needDraw = true;
                lastDrawTick = nowTick;
            }
        }

        if (needDraw) {
            if (gWindow && IsWindowVisible(gWindow)) {
                DrawWindow();
            }
            lastActive = gTransferActive;
            lastChannel = gChannelConnected;
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
                            if (gWindow && IsWindowVisible(gWindow)) {
                                DrawWindow();
                            }
                        }
                    }
                    break;
                case updateEvt:
                    if ((WindowPtr)event.message == gAboutWindow) {
                        BeginUpdate(gAboutWindow);
                        DrawAboutWindow();
                        EndUpdate(gAboutWindow);
                    } else if ((WindowPtr)event.message == gWindow) {
                        BeginUpdate(gWindow);
                        DrawWindow();
                        EndUpdate(gWindow);
                    }
                    break;
                case activateEvt:
                    if (gWindow && IsWindowVisible(gWindow)) {
                        DrawWindow();
                    }
                    break;
            }
        }
    }

    if (gSharedData) {
        gSharedData->DaemonRunning = false;
        gSharedData->UpdateUI = true;
    }

    CleanupDragManager();
    NetStopChannel();
    NetStopListener();
    if (gAboutWindow) {
        DisposeWindow(gAboutWindow);
        gAboutWindow = NULL;
    }
    if (gWindow) {
        DisposeWindow(gWindow);
    }

    return 0;
}
