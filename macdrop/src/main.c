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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "version.h"
#include "file_io.h"
#include "network.h"

#define MENU_APPLE      128
#define MENU_FILE       129
#define MENU_OPTIONS    130

#define ITEM_ABOUT      1

#define ITEM_RECONNECT  1
#define ITEM_DOWNLOAD   2
#define ITEM_SET_NAME   3
#define ITEM_CLOSE      5
#define ITEM_QUIT       6

#define ITEM_SOUND      1
#define ITEM_NOTIFY     2

typedef enum {
    CONN_READY = 0,
    CONN_SEARCHING,
    CONN_FAILED
} ConnState;

static Boolean   gDone               = false;
static WindowPtr gWindow             = NULL;
static MenuHandle gMenuApple         = NULL;
static MenuHandle gMenuFile          = NULL;
static MenuHandle gMenuOptions       = NULL;

static ConnState gConnState          = CONN_READY;
static char      gServerIpInput[32]  = "10.0.2.2";
static char      gConnErrorMsg[64]   = "";

static Boolean   gModalSetName       = false;
static char      gClientNameInput[64]= "";

/* UI Rectangles for hit testing */
static Rect      gBtnRetryScanRect;
static Rect      gBtnConnectRect;
static Rect      gBtnCancelConnRect;
static Rect      gBtnSaveNameRect;
static Rect      gBtnCancelNameRect;

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
        fillW = (short)(((unsigned long)(width - 4) * current) / total);
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

static void DrawWindow(void)
{
    Rect r;
    char buf[256];
    char ipStr[32];
    short y = 20;

    if (!gWindow) return;

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
        DrawCString("Receiving: ");
        TextFace(normal);
        DrawCString(gActiveTransferName[0] ? gActiveTransferName : "Incoming stream...");
        y += 16;

        DrawProgressBar(16, y, r.right - 32, 14, gActiveTransferBytes, gActiveTransferTotal);
        y += 20;

        MoveTo(16, y);
        if (gActiveTransferTotal > 0) {
            unsigned short pct = (unsigned short)(((unsigned long)gActiveTransferBytes * 100) / gActiveTransferTotal);
            sprintf(buf, "%s / %s (%u%%)", curStr, totStr, pct);
        } else {
            sprintf(buf, "%s transferred", curStr);
        }
        DrawCString(buf);
    } else {
        MoveTo(16, y);
        DrawCString("Ready to receive files via drag-and-drop on MacCompanion.");
        y += 15;

        MoveTo(16, y);
        TextFace(italic);
        DrawCString("Drop files on this Mac from your modern browser or phone.");
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

static void HandleMenuChoice(long choice)
{
    short menuID = HiWord(choice);
    short itemID = LoWord(choice);

    if (menuID == 0) return;

    switch (menuID) {
        case MENU_APPLE:
            if (itemID == ITEM_ABOUT) {
                if (gWindow) {
                    ShowWindow(gWindow);
                    SelectWindow(gWindow);
                    DrawWindow();
                }
            } else {
                Str255 name;
                GetMenuItemText(gMenuApple, itemID, name);
                OpenDeskAcc(name);
            }
            break;

        case MENU_FILE:
            switch (itemID) {
                case ITEM_RECONNECT:
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
                    break;
                case ITEM_DOWNLOAD:
                    if (ChooseDestinationFolder()) {
                        DrawWindow();
                    }
                    break;
                case ITEM_SET_NAME:
                    gModalSetName = true;
                    strncpy(gClientNameInput, gClientName, sizeof(gClientNameInput) - 1);
                    gClientNameInput[sizeof(gClientNameInput) - 1] = '\0';
                    DrawWindow();
                    break;
                case ITEM_CLOSE:
                    if (gWindow) {
                        HideWindow(gWindow);
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
                CheckItem(gMenuOptions, ITEM_SOUND, gSoundEnabled);
                SavePreferences();
            } else if (itemID == ITEM_NOTIFY) {
                gNotifyEnabled = !gNotifyEnabled;
                CheckItem(gMenuOptions, ITEM_NOTIFY, gNotifyEnabled);
                SavePreferences();
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
            }
            break;

        case inContent:
            if (win != FrontWindow()) {
                SelectWindow(win);
            } else if (win == gWindow) {
                Point localPt = evt->where;
                GlobalToLocal(&localPt);

                /* Modal 1: Set Client Name */
                if (gModalSetName) {
                    if (PtInRect(localPt, &gBtnSaveNameRect)) {
                        SetClientName(gClientNameInput);
                        gModalSetName = false;
                        NetStopChannel();
                        NetConnectChannel();
                        DrawWindow();
                        return;
                    }
                    if (PtInRect(localPt, &gBtnCancelNameRect)) {
                        gModalSetName = false;
                        DrawWindow();
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
            }
            break;
    }
}

static void HandleKeyDown(EventRecord *evt)
{
    char key = (char)(evt->message & charCodeMask);

    if (evt->modifiers & cmdKey) {
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
            DrawWindow();
        } else if (key == 27) {
            gModalSetName = false;
            DrawWindow();
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
    Boolean lastActive = false;
    Boolean lastChannel = false;

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    mbar = GetNewMBar(128);
    if (mbar) {
        SetMenuBar(mbar);
        AppendResMenu(GetMenuHandle(MENU_APPLE), 'DRVR');
        DrawMenuBar();
    }

    gMenuApple   = GetMenuHandle(MENU_APPLE);
    gMenuFile    = GetMenuHandle(MENU_FILE);
    gMenuOptions = GetMenuHandle(MENU_OPTIONS);

    InitFileManager();

    if (gMenuOptions) {
        CheckItem(gMenuOptions, ITEM_SOUND, gSoundEnabled);
        CheckItem(gMenuOptions, ITEM_NOTIFY, gNotifyEnabled);
    }

    /* Create status window (centered horizontally) */
    SetRect(&r, 40, 45, 430, 245);
    gWindow = NewWindow(NULL, &r, "\pMacDrop", true, documentProc, (WindowPtr)-1, true, 0);

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

    DrawWindow();

    while (!gDone) {
        EventRecord event;
        short sleep = gTransferActive ? 0 : 20;
        Boolean gotEvent = WaitNextEvent(everyEvent, &event, sleep, NULL);

        if (gConnState == CONN_READY) {
            NetIdle();
        }

        if (gTransferActive != lastActive ||
            gChannelConnected != lastChannel ||
            (gTransferActive && (gActiveTransferBytes != lastTransferBytes))) {
            DrawWindow();
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
                        if (resuming && gWindow) {
                            ShowWindow(gWindow);
                            SelectWindow(gWindow);
                            DrawWindow();
                        }
                    }
                    break;
                case updateEvt:
                    if ((WindowPtr)event.message == gWindow) {
                        BeginUpdate(gWindow);
                        DrawWindow();
                        EndUpdate(gWindow);
                    }
                    break;
                case activateEvt:
                    DrawWindow();
                    break;
            }
        }
    }

    NetStopChannel();
    NetStopListener();
    if (gWindow) {
        DisposeWindow(gWindow);
    }

    return 0;
}
