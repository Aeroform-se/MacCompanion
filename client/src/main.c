/*
 * MacCompanion Spotify — Native 68k Macintosh Client
 * Compiled with Retro68 + Apple Universal Interfaces 3.4.2
 *
 * Target: Compact Mac 9" CRT (512 x 342, 496 x 300 usable window)
 * SDK Verification Build: Confirms QuickDraw, Window Manager, Event Manager,
 * Sound Manager (Sound.h), and MacTCP (MacTCP.h) headers and libraries link cleanly.
 */

#include <QuickDraw.h>
#include <Windows.h>
#include <Menus.h>
#include <Fonts.h>
#include <Events.h>
#include <Dialogs.h>
#include <TextEdit.h>
#include <Devices.h>
#include <Sound.h>
#include <MacTCP.h>
#include <ToolUtils.h>
#include <Processes.h>
#include <OSUtils.h>
#include <string.h>
#include <stdio.h>

#define kMenuBarID      128
#define mApple          128
#define iAbout          1

#define mFile           129
#define iReconnect      1
#define iQuit           3

#define mPlayback       130
#define iPlayPause      1
#define iNextTrack      2
#define iPrevTrack      3
#define iToggleAudio    5

static WindowPtr gMainWindow = NULL;
static Boolean   gRunning    = true;
static Boolean   gAudioOn    = false;
static short     gMacTCPRef  = 0;
static Boolean   gMacTCPOk   = false;

static void InitToolbox(void)
{
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
}

static void SetupMenus(void)
{
    Handle menuBar = GetNewMBar(kMenuBarID);
    if (menuBar != NULL) {
        SetMenuBar(menuBar);
        DisposeHandle(menuBar);
        AppendResMenu(GetMenuHandle(mApple), 'DRVR');
        DrawMenuBar();
    }
}

static void ProbeMacTCP(void)
{
    OSErr err = OpenDriver("\p.ipp", &gMacTCPRef);
    gMacTCPOk = (err == noErr);
}

static void PlayTestChime(void)
{
    SysBeep(10);
}

static void DrawCString(const char *str)
{
    DrawText(str, 0, (short)strlen(str));
}

static void DrawMainWindow(WindowPtr win)
{
    Rect r;
    Rect artRect;
    Rect progOuter;
    Rect progFill;
    Rect btnRect;

    if (win == NULL) return;
    SetPort(win);

    r = win->portRect;
    EraseRect(&r);

    /* Top status bar */
    MoveTo(12, 18);
    TextFont(systemFont);
    TextSize(12);
    TextFace(bold);
    DrawCString("MacCompanion Spotify (68k Native)");

    TextFont(applFont);
    TextSize(9);
    TextFace(normal);
    MoveTo(310, 18);
    if (gMacTCPOk) {
        DrawCString("MacTCP (.ipp): Ready");
    } else {
        DrawCString("MacTCP (.ipp): Not Loaded");
    }

    MoveTo(0, 26);
    LineTo(r.right, 26);

    /* 120x120 1-bit Album Art Placeholder Frame */
    SetRect(&artRect, 14, 38, 134, 158);
    FrameRect(&artRect);
    InsetRect(&artRect, 2, 2);
    FillRect(&artRect, &qd.ltGray);
    InsetRect(&artRect, 24, 48);
    EraseRect(&artRect);
    FrameRect(&artRect);
    MoveTo(48, 101);
    DrawCString("1-Bit Art");

    /* Track Metadata Area */
    TextFont(systemFont);
    TextSize(12);
    TextFace(bold);
    MoveTo(148, 56);
    DrawCString("Retro68 + Universal 3.4.2 Ready");

    TextFont(applFont);
    TextSize(10);
    TextFace(normal);
    MoveTo(148, 76);
    DrawCString("Artist: MacCompanion Middleware");
    MoveTo(148, 94);
    DrawCString("Album: Basilisk II Integration Test");

    MoveTo(148, 118);
    TextSize(9);
    DrawCString("Host target: 10.0.2.2 (UDP :6801 / TCP :6802)");

    MoveTo(148, 136);
    if (gAudioOn) {
        DrawCString("Speaker: 68k Sound Manager [ACTIVE]");
    } else {
        DrawCString("Speaker: Spotify Connect Remote");
    }

    /* Progress Bar */
    SetRect(&progOuter, 14, 174, r.right - 14, 184);
    FrameRect(&progOuter);
    SetRect(&progFill, 16, 176, 180, 182);
    FillRect(&progFill, &qd.dkGray);

    /* Transport Controls */
    SetRect(&btnRect, 14, 198, 94, 224);
    FrameRoundRect(&btnRect, 8, 8);
    MoveTo(32, 215);
    DrawCString("<< Prev");

    SetRect(&btnRect, 104, 198, 204, 224);
    FrameRoundRect(&btnRect, 8, 8);
    MoveTo(124, 215);
    DrawCString("Play / Pause");

    SetRect(&btnRect, 214, 198, 294, 224);
    FrameRoundRect(&btnRect, 8, 8);
    MoveTo(232, 215);
    DrawCString("Next >>");

    SetRect(&btnRect, 314, 198, r.right - 14, 224);
    FrameRoundRect(&btnRect, 8, 8);
    MoveTo(328, 215);
    DrawCString("Chime / Audio Test");

    /* Bottom Footer */
    MoveTo(0, r.bottom - 24);
    LineTo(r.right, r.bottom - 24);
    MoveTo(14, r.bottom - 8);
    DrawCString("Cmd+Q: Quit   Cmd+P: Play/Pause   Cmd+L: Toggle 68k Audio");
}

static void HandleMenuChoice(long menuChoice)
{
    short menuID = HiWord(menuChoice);
    short itemID = LoWord(menuChoice);
    Str255 daName;

    if (menuID == 0) return;

    switch (menuID) {
        case mApple:
            if (itemID == iAbout) {
                PlayTestChime();
            } else {
                GetMenuItemText(GetMenuHandle(mApple), itemID, daName);
                OpenDeskAcc(daName);
            }
            break;

        case mFile:
            if (itemID == iReconnect) {
                ProbeMacTCP();
                if (gMainWindow) {
                    SetPort(gMainWindow);
                    InvalRect(&gMainWindow->portRect);
                }
            } else if (itemID == iQuit) {
                gRunning = false;
            }
            break;

        case mPlayback:
            if (itemID == iPlayPause || itemID == iNextTrack || itemID == iPrevTrack) {
                PlayTestChime();
            } else if (itemID == iToggleAudio) {
                gAudioOn = !gAudioOn;
                PlayTestChime();
                if (gMainWindow) {
                    SetPort(gMainWindow);
                    InvalRect(&gMainWindow->portRect);
                }
            }
            break;
    }
    HiliteMenu(0);
}

static void HandleMouseDown(EventRecord *ev)
{
    WindowPtr whichWindow;
    short part = FindWindow(ev->where, &whichWindow);

    switch (part) {
        case inMenuBar:
            HandleMenuChoice(MenuSelect(ev->where));
            break;

        case inSysWindow:
            SystemClick(ev, whichWindow);
            break;

        case inDrag:
            if (whichWindow != NULL) {
                DragWindow(whichWindow, ev->where, &qd.screenBits.bounds);
            }
            break;

        case inGoAway:
            if (TrackGoAway(whichWindow, ev->where)) {
                gRunning = false;
            }
            break;

        case inContent:
            if (whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            } else if (whichWindow == gMainWindow) {
                Point pt = ev->where;
                SetPort(gMainWindow);
                GlobalToLocal(&pt);
                if (pt.v >= 198 && pt.v <= 224 && pt.h >= 314) {
                    gAudioOn = !gAudioOn;
                    PlayTestChime();
                    InvalRect(&gMainWindow->portRect);
                }
            }
            break;
    }
}

int main(void)
{
    Rect winBounds;
    EventRecord ev;

    InitToolbox();
    SetupMenus();
    ProbeMacTCP();

    /* Compact Mac 512x342 friendly window (480 x 260 centered) */
    SetRect(&winBounds, 16, 44, 496, 304);
    gMainWindow = NewWindow(
        NULL,
        &winBounds,
        "\pMacCompanion Spotify",
        true,
        documentProc,
        (WindowPtr)-1L,
        true,
        0
    );

    if (gMainWindow != NULL) {
        SetPort(gMainWindow);
        DrawMainWindow(gMainWindow);
    }

    while (gRunning) {
        SystemTask();
        if (WaitNextEvent(everyEvent, &ev, 10, NULL)) {
            switch (ev.what) {
                case mouseDown:
                    HandleMouseDown(&ev);
                    break;

                case keyDown:
                case autoKey:
                    if ((ev.modifiers & cmdKey) != 0) {
                        HandleMenuChoice(MenuKey((char)(ev.message & charCodeMask)));
                    }
                    break;

                case updateEvt:
                    BeginUpdate((WindowPtr)ev.message);
                    if ((WindowPtr)ev.message == gMainWindow) {
                        DrawMainWindow(gMainWindow);
                    }
                    EndUpdate((WindowPtr)ev.message);
                    break;
            }
        }
    }

    if (gMainWindow != NULL) {
        DisposeWindow(gMainWindow);
    }
    return 0;
}
