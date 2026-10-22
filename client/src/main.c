/*
 * MaciSpot — Native 68k Macintosh Spotify Client
 * Version 0.2 Beta ("Cocktail Hour")
 * Compiled with Retro68 + Apple Universal Interfaces 3.4.2
 *
 * UI Design: 512 x 322 flush canvas below the 20px Mac OS menu bar
 * (exact 512 x 342 Compact Mac screen dimensions).
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
#include <Memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "version.h"
#include "assets.h"
#include "network.h"

/* ============================================================================
 * Constants & Layout Geometry (512 x 322 Canvas)
 * ============================================================================ */

#define CANVAS_W            512
#define CANVAS_H            322
#define OFFSCREEN_ROWBYTES  64   /* 512 / 8 */

#define kMenuBarID          128
#define mApple              128
#define iAbout              1

#define mFile               129
#define iServerIP           1
#define iFindSearch         2
#define iQuit               4

#define mPlayback           130
#define iPlayPause          1
#define iNextTrack          2
#define iPrevTrack          3
#define iLikeTrack          5
#define iChooseSpeaker      6

#define MAX_LEFT_ITEMS      48
#define MAX_RIGHT_ITEMS     64
#define MAX_SEARCH_ITEMS    48
#define MAX_DEVICES         10
#define VISIBLE_ROWS        16
#define ROW_HEIGHT          13
#define LIST_TOP_Y          35
#define LIST_BOTTOM_Y       243

#define SEARCH_VISIBLE_ROWS 6
#define SEARCH_ROW_H        40
#define SEARCH_TOP_Y        34

/* Pre-connection Server Screen states (Server1.png, Server2.png, Server3.png) */
#define CONN_SEARCHING      1
#define CONN_FAILED         2
#define CONN_FOUND          3
#define CONN_READY          4

/* Hover / Pressed button identifiers (button-states.png) */
#define BTN_NONE            0
#define BTN_SHUFFLE         1
#define BTN_PREV            2
#define BTN_PLAY            3
#define BTN_NEXT            4
#define BTN_REPEAT          5
#define BTN_SPEAKER         6
#define BTN_TIMELINE        7
#define BTN_VOLUME          8

/* ============================================================================
 * Data Structures
 * ============================================================================ */

typedef struct {
    char id[64];
    char name[64];
    char uri[96];
    char kind[16]; /* "LIKED", "PLAYLIST", "HOME_CAT" */
} LeftItem;

typedef struct {
    char kind[16]; /* "TRACK", "ALBUM", "PLAYLIST" */
    char id[64];
    char name[64];
    char artist[64];
    char durationMmSs[12];
    char uri[96];
} RightItem;

typedef struct {
    char kind[16];     /* "TRACK", "ALBUM", "PLAYLIST" */
    char id[64];
    char title[64];
    char subtitle[64]; /* Artist or Owner */
    char uri[96];
} SearchItem;

typedef struct {
    char    id[64];
    char    name[48];
    char    type[24];
    Boolean isActive;
    short   volumePct;
} DeviceItem;

typedef struct {
    Boolean isPlaying;
    char    trackId[64];
    char    trackName[64];
    char    artistName[64];
    char    albumName[64];
    long    progressMs;
    long    durationMs;
    char    progressMmSs[12];
    char    durationMmSs[12];
    short   volumePct;
    char    shuffleMode[12]; /* "off", "on", "smart" */
    char    repeatState[12]; /* "off", "context", "track" */
    Boolean isLiked;
    char    deviceName[48];
} NowPlayingState;

/* ============================================================================
 * Global Application State
 * ============================================================================ */

static WindowPtr       gMainWindow        = NULL;
static GrafPort        gOffscreenPort;
static Ptr             gOffscreenBits     = NULL;
static Boolean         gOffscreenReady    = false;

static Boolean         gRunning           = true;
static short           gConnState         = CONN_SEARCHING;
static unsigned long   gFoundStartTicks   = 0;
static short           gFoundCountdownSec = 3;
static Boolean         gInitialDataLoaded = false;

static Boolean         gHomeMode          = false; /* false = Playlists (note icon), true = Home (home icon) */

/* Search state */
static Boolean         gSearchFocused     = false;
static Boolean         gSearchViewOpen    = false;
static char            gSearchQuery[48]   = "";
static SearchItem      gSearchItems[MAX_SEARCH_ITEMS];
static short           gSearchCount       = 0;
static short           gSearchScroll      = 0;
static short           gSearchHoverRow    = -1;

/* Speaker Chooser Popover state */
static Boolean         gSpeakerPopupOpen  = false;
static DeviceItem      gDevices[MAX_DEVICES];
static short           gDeviceCount       = 0;

/* Server IP Input state */
static char            gIpInput[32]       = "10.0.2.2";

/* About Box Modal state */
static Boolean         gAboutModalOpen    = false;

/* Button Hover & Pressed states */
static short           gHoverBtn          = BTN_NONE;
static short           gPressedBtn        = BTN_NONE;

/* Left & Right List state */
static LeftItem        gLeftItems[MAX_LEFT_ITEMS];
static short           gLeftCount         = 0;
static short           gLeftSelected      = 0;
static short           gLeftScroll        = 0;

static RightItem       gRightItems[MAX_RIGHT_ITEMS];
static short           gRightCount        = 0;
static short           gRightSelected     = -1;
static short           gRightScroll       = 0;
static char            gActiveContextUri[96] = "";
static char            gRightHeaderLabel[24] = "Tracks";

/* Double-click detection */
static unsigned long   gLastClickTicks     = 0;
static short           gLastClickedRow     = -1;
static unsigned long   gLastLeftClickTicks = 0;
static short           gLastLeftClickedRow = -1;

/* Now Playing & Artwork state */
static NowPlayingState gNowPlaying;
static Boolean         gTimelineFrozen    = false;
static char            gLoadedArtTrackId[64] = "";
static unsigned char   gCurrentArtworkBits[5200];
static unsigned short  gAudioSampleRateHz = 22255;

/* Timing for periodic state & timeline updates */
static unsigned long   gLastPollTicks     = 0;
static unsigned long   gLastSecondTicks   = 0;

/* Shared scratch buffer for HTTP responses */
#define HTTP_BUF_SIZE  16384
static char           *gHttpBuf           = NULL;

/* Forward declarations */
static void RenderCanvas(void);
static void ConnectAndLoadAll(void);

/* ============================================================================
 * String & Pipe Parsing Utilities
 * ============================================================================ */

static void SafeCopy(char *dst, const char *src, short maxLen)
{
    short i = 0;
    if (!dst || maxLen <= 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] != '\0' && i < (maxLen - 1)) {
        unsigned char c = (unsigned char)src[i];
        if (c >= 0x80) {
            dst[i] = '-';
        } else {
            dst[i] = (char)c;
        }
        i++;
    }
    dst[i] = '\0';
}

/*
 * Extracts the `fieldIndex`-th pipe-separated column from a single line.
 */
static void GetPipeField(const char *line, short fieldIndex, char *outBuf, short maxOut)
{
    short curField = 0;
    const char *p = line;
    short outIdx = 0;

    if (!outBuf || maxOut <= 0) return;
    outBuf[0] = '\0';
    if (!line) return;

    while (*p && *p != '\r' && *p != '\n') {
        if (*p == '|') {
            if (curField == fieldIndex) break;
            curField++;
            p++;
            continue;
        }
        if (curField == fieldIndex && outIdx < (maxOut - 1)) {
            unsigned char c = (unsigned char)*p;
            if (c < 0x80) {
                outBuf[outIdx++] = (char)c;
            } else if ((c & 0xE0) == 0xC0 && ((unsigned char)p[1] & 0xC0) == 0x80) {
                unsigned int cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
                /* Map Swedish åäöÅÄÖé to MacRoman for authentic display */
                if (cp == 0xE5) outBuf[outIdx++] = (char)0x8C;      /* å */
                else if (cp == 0xE4) outBuf[outIdx++] = (char)0x8A; /* ä */
                else if (cp == 0xF6) outBuf[outIdx++] = (char)0x9A; /* ö */
                else if (cp == 0xC5) outBuf[outIdx++] = (char)0x81; /* Å */
                else if (cp == 0xC4) outBuf[outIdx++] = (char)0x80; /* Ä */
                else if (cp == 0xD6) outBuf[outIdx++] = (char)0x85; /* Ö */
                else if (cp == 0xE9) outBuf[outIdx++] = (char)0x8E; /* é */
                else outBuf[outIdx++] = '-';
                p++;
            }
        }
        p++;
    }
    outBuf[outIdx] = '\0';
}

/* Always includes leading zero on both minutes and seconds (e.g. 02:00, never 2:00) */
static void FormatMmSs(long ms, char *outBuf)
{
    long totalSec;
    long mins;
    long secs;
    if (ms < 0) ms = 0;
    totalSec = ms / 1000;
    mins = totalSec / 60;
    secs = totalSec % 60;
    sprintf(outBuf, "%02ld:%02ld", mins, secs);
}

/* ============================================================================
 * QuickDraw Offscreen & 1-Bit Bitmap Drawing Helpers
 * ============================================================================ */

static void InitOffscreenCanvas(void)
{
    Rect bounds;
    SetRect(&bounds, 0, 0, CANVAS_W, CANVAS_H);
    gOffscreenBits = NewPtrClear((long)OFFSCREEN_ROWBYTES * CANVAS_H);
    if (gOffscreenBits) {
        OpenPort(&gOffscreenPort);
        gOffscreenPort.portBits.baseAddr = gOffscreenBits;
        gOffscreenPort.portBits.rowBytes = OFFSCREEN_ROWBYTES;
        gOffscreenPort.portBits.bounds   = bounds;
        gOffscreenPort.portRect          = bounds;
        RectRgn(gOffscreenPort.visRgn, &bounds);
        RectRgn(gOffscreenPort.clipRgn, &bounds);
        gOffscreenReady = true;
    }
}

static void BlitIcon1Bit(const unsigned char *bits, short width, short height, short dstX, short dstY, short transferMode)
{
    BitMap srcBM;
    Rect srcRect, dstRect;
    GrafPtr curPort;

    GetPort(&curPort);
    srcBM.baseAddr = (Ptr)bits;
    srcBM.rowBytes = ((width + 15) / 16) * 2;
    SetRect(&srcBM.bounds, 0, 0, width, height);
    SetRect(&srcRect, 0, 0, width, height);
    SetRect(&dstRect, dstX, dstY, dstX + width, dstY + height);

    CopyBits(&srcBM, &curPort->portBits, &srcRect, &dstRect, transferMode, NULL);
}

static void DrawCStringTruncated(const char *str, short maxChars)
{
    char buf[64];
    short len;
    if (!str) return;
    len = (short)strlen(str);
    if (len <= maxChars) {
        DrawText(str, 0, len);
    } else {
        if (maxChars > 3) {
            memcpy(buf, str, maxChars - 3);
            buf[maxChars - 3] = ' ';
            buf[maxChars - 2] = '.';
            buf[maxChars - 1] = '.';
            DrawText(buf, 0, maxChars);
        } else {
            DrawText(str, 0, maxChars);
        }
    }
}

/* Draws a 1px vertical dotted line */
static void DrawVerticalDottedLine(short x, short yTop, short yBottom, Boolean whiteDots)
{
    short y;
    PenNormal();
    PenMode(whiteDots ? patBic : patCopy);
    for (y = yTop; y <= yBottom; y += 2) {
        MoveTo(x, y);
        LineTo(x, y);
    }
    PenNormal();
}

/* Draws a 1px horizontal dotted line */
static void DrawHorizontalDottedLine(short xLeft, short xRight, short y, Boolean whiteDots)
{
    short x;
    PenNormal();
    PenMode(whiteDots ? patBic : patCopy);
    for (x = xLeft; x <= xRight; x += 2) {
        MoveTo(x, y);
        LineTo(x, y);
    }
    PenNormal();
}

/*
 * Draws the 9x9 circular slider thumb dot centered at (cx, cy).
 * Matches `button-states.png`:
 * - Normal (`hollow == false`): Solid black 9x9 circle.
 * - Hover/Active (`hollow == true`): 2px black ring with white center.
 */
static void DrawSliderDot(short cx, short cy, Boolean hollow)
{
    Rect r;
    SetRect(&r, cx - 4, cy - 4, cx + 5, cy + 5);
    if (hollow) {
        EraseOval(&r);
        FrameOval(&r);
        InsetRect(&r, 1, 1);
        FrameOval(&r);
    } else {
        PaintOval(&r);
    }
}

/*
 * Draws the hover or pressed border box around a transport button as shown in `button-states.png`:
 * - Hover (`pressed == false`): Rounded box at `y = 281..298` + 2px gray shadow at `y = 298..300`.
 * - Pressed (`pressed == true`): Shifted down 1px (`y = 282..299`) + 1px gray shadow at `y = 299..300`.
 */
static void DrawTransportBtnBox(short xLeft, short xRight, Boolean isHover, Boolean isPressed)
{
    Rect box, shadow;
    if (!isHover && !isPressed) return;

    if (isPressed) {
        SetRect(&shadow, xLeft + 2, 298, xRight - 2, 300);
        FillRect(&shadow, &qd.gray);
        SetRect(&box, xLeft, 282, xRight, 299);
        EraseRoundRect(&box, 6, 6);
        FrameRoundRect(&box, 6, 6);
    } else {
        SetRect(&shadow, xLeft + 2, 297, xRight - 2, 300);
        FillRect(&shadow, &qd.gray);
        SetRect(&box, xLeft, 281, xRight, 298);
        EraseRoundRect(&box, 6, 6);
        FrameRoundRect(&box, 6, 6);
    }
}

/* Helper to compute Speaker button right edge based on active speaker label width */
static short GetSpeakerButtonRightX(void)
{
    const char *label = (gNowPlaying.deviceName[0] != '\0') ? gNowPlaying.deviceName : "Choose Speaker";
    short len = (short)strlen(label);
    short w;
    if (len > 18) len = 18;
    w = len * 6; /* Monaco 9 is 6px per char */
    return 364 + w + 8;
}

/* ============================================================================
 * Main UI Rendering (Exact 512 x 322 PSD Layout)
 * ============================================================================ */

static void RenderPreConnectionScreen(void)
{
    Rect fullRect, ipBox, countBox;
    char ipField[40];
    char secStr[16];

    SetRect(&fullRect, 0, 0, CANVAS_W, CANVAS_H);
    FillRect(&fullRect, &qd.gray);

    if (gConnState == CONN_SEARCHING) {
        BlitIcon1Bit(gServerCard1Bits, 236, 199, 140, 63, srcCopy);
        /* Erase placeholder 192.168.000.000 inside input box and draw gIpInput */
        SetRect(&ipBox, 170, 219, 275, 239);
        EraseRect(&ipBox);
        TextMode(srcOr);
        MoveTo(176, 232);
        DrawCStringTruncated(gIpInput, 16);
    } else if (gConnState == CONN_FAILED) {
        BlitIcon1Bit(gServerCard2Bits, 236, 199, 140, 63, srcCopy);
        SetRect(&ipBox, 170, 219, 275, 239);
        EraseRect(&ipBox);
        sprintf(ipField, "%s_", gIpInput);
        TextMode(srcOr);
        MoveTo(176, 232);
        DrawCStringTruncated(ipField, 16);
    } else if (gConnState == CONN_FOUND) {
        BlitIcon1Bit(gServerCard3Bits, 236, 199, 140, 63, srcCopy);
        /* Update countdown digit in "automatically in 3 seconds." at (248, 227)-(256, 238) */
        SetRect(&countBox, 247, 226, 256, 238);
        EraseRect(&countBox);
        sprintf(secStr, "%d", (int)gFoundCountdownSec);
        TextMode(srcOr);
        MoveTo(248, 236);
        DrawText(secStr, 0, 1);
    }
}

static void RenderSearchCenterPanel(void)
{
    short row;
    Rect r, badgeRect, thumbRect;

    /* Top-right circular (X) close button (485, 34) 15x15 */
    BlitIcon1Bit(gSearchCloseCircleBits, 15, 15, 485, 34, srcCopy);

    /* Search Scrollbar at x = 469, y = 35..266 */
    DrawVerticalDottedLine(469, 35, 266, true);
    {
        short trackH = 231;
        short thumbH = (gSearchCount > SEARCH_VISIBLE_ROWS)
            ? (short)((long)trackH * SEARCH_VISIBLE_ROWS / gSearchCount)
            : 57;
        short maxScroll = (gSearchCount > SEARCH_VISIBLE_ROWS) ? (gSearchCount - SEARCH_VISIBLE_ROWS) : 1;
        short thumbY = 35 + (short)((long)(trackH - thumbH) * gSearchScroll / maxScroll);
        if (thumbH < 20) thumbH = 20;
        if (gSearchCount <= SEARCH_VISIBLE_ROWS) thumbY = 48;
        SetRect(&thumbRect, 468, thumbY, 471, thumbY + thumbH);
        FillRect(&thumbRect, &qd.gray);
    }

    if (gSearchCount == 0) {
        TextMode(srcBic);
        MoveTo(78, 60);
        DrawCStringTruncated("No results found.", 32);
        TextMode(srcOr);
        return;
    }

    for (row = 0; row < SEARCH_VISIBLE_ROWS; row++) {
        short idx = gSearchScroll + row;
        short yTop = SEARCH_TOP_Y + row * SEARCH_ROW_H;
        Boolean isHovered = (idx == gSearchHoverRow);
        SearchItem *it;
        const char *badgeLabel = "Song";
        short badgeW = 36;

        if (idx >= gSearchCount) break;
        it = &gSearchItems[idx];

        if (strcmp(it->kind, "ALBUM") == 0) {
            badgeLabel = "Album";
            badgeW = 42;
        } else if (strcmp(it->kind, "PLAYLIST") == 0) {
            badgeLabel = "Playlist";
            badgeW = 58;
        }

        if (isHovered) {
            SetRect(&r, 61, yTop + 1, 452, yTop + SEARCH_ROW_H - 1);
            EraseRect(&r); /* White highlight bar */

            /* Play circle icon (355, yTop + 8) 12x12 */
            BlitIcon1Bit(gSearchPlayIconBits, 12, 12, 355, yTop + 8, srcCopy);

            /* Solid black badge pill with white text */
            SetRect(&badgeRect, 371, yTop + 8, 371 + badgeW, yTop + 20);
            PaintRoundRect(&badgeRect, 6, 6);
            TextMode(srcBic);
            MoveTo(377, yTop + 17);
            DrawCStringTruncated(badgeLabel, 10);

            /* Black title & subtitle */
            TextMode(srcOr);
            MoveTo(78, yTop + 17);
            DrawCStringTruncated(it->title, 42);
            MoveTo(78, yTop + 29);
            DrawCStringTruncated(it->subtitle, 42);
        } else {
            /* White text on black background */
            TextMode(srcBic);
            MoveTo(78, yTop + 17);
            DrawCStringTruncated(it->title, 42);
            MoveTo(78, yTop + 29);
            DrawCStringTruncated(it->subtitle, 42);

            /* White-outlined badge pill on black background */
            SetRect(&badgeRect, 371, yTop + 8, 371 + badgeW, yTop + 20);
            PenMode(patBic);
            FrameRoundRect(&badgeRect, 6, 6);
            PenNormal();
            MoveTo(377, yTop + 17);
            DrawCStringTruncated(badgeLabel, 10);
        }

        /* Dotted horizontal separator line at bottom of row */
        DrawHorizontalDottedLine(61, 452, yTop + SEARCH_ROW_H - 1, true);
    }
    TextMode(srcOr);
}

static void RenderCanvas(void)
{
    GrafPtr savePort;
    Rect r, barRect, thumbRect;
    short i;
    char titleLine[160];

    if (!gMainWindow) return;
    GetPort(&savePort);

    if (gOffscreenReady) {
        SetPort(&gOffscreenPort);
    } else {
        SetPort(gMainWindow);
    }

    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);

    /* --------------------------------------------------------------------
     * 0. Pre-Connection Server Screens (Server1.png, Server2.png, Server3.png)
     * -------------------------------------------------------------------- */
    if (gConnState != CONN_READY) {
        RenderPreConnectionScreen();
        if (gOffscreenReady) {
            Rect fullRect;
            SetRect(&fullRect, 0, 0, CANVAS_W, CANVAS_H);
            SetPort(gMainWindow);
            CopyBits(&gOffscreenPort.portBits, &gMainWindow->portBits, &fullRect, &fullRect, srcCopy, NULL);
        }
        SetPort(savePort);
        return;
    }

    /* --------------------------------------------------------------------
     * 1. Top Header Bar (y = 0..22, White background)
     * -------------------------------------------------------------------- */
    SetRect(&r, 0, 0, CANVAS_W, 22);
    EraseRect(&r);

    TextMode(srcOr);
    MoveTo(14, 14);
    DrawCStringTruncated("Library", 12);

    /* Home / Playlists Toggle (96, 6) 38x10 */
    if (gHomeMode) {
        BlitIcon1Bit(gHeaderToggleHomeBits, 38, 10, 96, 6, srcCopy);
    } else {
        BlitIcon1Bit(gHeaderTogglePlaylistBits, 38, 10, 96, 6, srcCopy);
    }

    /* Dotted vertical dividers in header */
    DrawVerticalDottedLine(144, 4, 17, false);
    DrawVerticalDottedLine(366, 4, 17, false);

    /* Search Pill (156, 5)-(356, 17) — show clear (X) icon when search query or view active */
    if (gSearchViewOpen || gSearchQuery[0] != '\0') {
        BlitIcon1Bit(gSearchPillClearBits, 200, 12, 156, 5, srcCopy);
    } else {
        BlitIcon1Bit(gSearchPillBits, 200, 12, 156, 5, srcCopy);
    }
    if (gSearchFocused || gSearchQuery[0] != '\0') {
        char searchDisplay[56];
        if (gSearchFocused) {
            sprintf(searchDisplay, "%s_", gSearchQuery);
        } else {
            SafeCopy(searchDisplay, gSearchQuery, sizeof(searchDisplay));
        }
        TextMode(srcBic); /* White text on black search pill */
        MoveTo(171, 14);
        DrawCStringTruncated(searchDisplay, 26);
        TextMode(srcOr);
    }

    /* Right column header ("Tracks" or "Albums") */
    MoveTo(372, 14);
    DrawCStringTruncated(gRightHeaderLabel, 18);

    /* --------------------------------------------------------------------
     * 2. Middle Main Content Area (y = 22..273, Black background)
     * -------------------------------------------------------------------- */
    SetRect(&r, 0, 22, CANVAS_W, 273);
    PaintRect(&r);

    if (gSearchViewOpen) {
        RenderSearchCenterPanel();
    } else {
        /* 2A. Left Column: Library / Home Items (x = 10..138, y = 35..243) */
        for (i = 0; i < VISIBLE_ROWS; i++) {
            short itemIdx = gLeftScroll + i;
            short rowTop  = LIST_TOP_Y + i * ROW_HEIGHT;
            if (itemIdx >= gLeftCount) break;

            if (itemIdx == gLeftSelected) {
                SetRect(&r, 10, rowTop, 138, rowTop + 12);
                EraseRect(&r); /* White highlight pill */
                TextMode(srcOr); /* Black text */
            } else {
                TextMode(srcBic); /* White text on black background */
            }
            MoveTo(13, rowTop + 9);
            DrawCStringTruncated(gLeftItems[itemIdx].name, 19);
        }

        /* Left Scrollbar (x = 144, y = 35..266) */
        DrawVerticalDottedLine(144, 35, 266, true);
        {
            short trackH = 231;
            short thumbH = (gLeftCount > VISIBLE_ROWS)
                ? (short)((long)trackH * VISIBLE_ROWS / gLeftCount)
                : 57;
            short maxScroll = (gLeftCount > VISIBLE_ROWS) ? (gLeftCount - VISIBLE_ROWS) : 1;
            short thumbY;
            if (thumbH < 20) thumbH = 20;
            thumbY = 35 + (short)((long)(trackH - thumbH) * gLeftScroll / maxScroll);
            if (gLeftCount <= VISIBLE_ROWS) thumbY = 64;
            SetRect(&thumbRect, 143, thumbY, 146, thumbY + thumbH);
            FillRect(&thumbRect, &qd.gray);
        }

        /* 2B. Center Column: 200x200 1-Bit Album Artwork or Initial Graphic (156, 45)-(356, 245) */
        BlitIcon1Bit(gCurrentArtworkBits, 200, 200, 156, 45, srcCopy);

        /* Artist name - Song Title (or "Party like it's 1984") centered below artwork at y = 259 */
        if (gNowPlaying.trackName[0] != '\0') {
            if (gNowPlaying.artistName[0] != '\0') {
                sprintf(titleLine, "%s - %s", gNowPlaying.artistName, gNowPlaying.trackName);
            } else {
                SafeCopy(titleLine, gNowPlaying.trackName, sizeof(titleLine));
            }
        } else {
            SafeCopy(titleLine, "Party like it's 1984", sizeof(titleLine));
        }
        {
            char truncTitle[40];
            short len = (short)strlen(titleLine);
            short pixelW;
            short startX;
            if (len > 33) {
                memcpy(truncTitle, titleLine, 30);
                truncTitle[30] = '.';
                truncTitle[31] = '.';
                truncTitle[32] = '\0';
                len = 32;
            } else {
                SafeCopy(truncTitle, titleLine, sizeof(truncTitle));
            }
            pixelW = TextWidth(truncTitle, 0, len);
            startX = 256 - (pixelW / 2);
            if (startX < 152) startX = 152;
            TextMode(srcBic); /* White text on black background */
            MoveTo(startX, 259);
            DrawText(truncTitle, 0, len);
        }

        /* 2C. Right Column: Tracks List (x = 370..498, y = 35..243) */
        for (i = 0; i < VISIBLE_ROWS; i++) {
            short itemIdx = gRightScroll + i;
            short rowTop  = LIST_TOP_Y + i * ROW_HEIGHT;
            if (itemIdx >= gRightCount) break;

            if (itemIdx == gRightSelected) {
                SetRect(&r, 370, rowTop, 498, rowTop + 12);
                EraseRect(&r); /* White highlight pill */
                TextMode(srcOr); /* Black text */
            } else {
                TextMode(srcBic); /* White text on black background */
            }
            MoveTo(373, rowTop + 9);
            DrawCStringTruncated(gRightItems[itemIdx].name, 19);
        }

        /* Right Scrollbar (x = 504, y = 35..266) */
        DrawVerticalDottedLine(504, 35, 266, true);
        {
            short trackH = 231;
            short thumbH = (gRightCount > VISIBLE_ROWS)
                ? (short)((long)trackH * VISIBLE_ROWS / gRightCount)
                : 57;
            short maxScroll = (gRightCount > VISIBLE_ROWS) ? (gRightCount - VISIBLE_ROWS) : 1;
            short thumbY;
            if (thumbH < 20) thumbH = 20;
            thumbY = 35 + (short)((long)(trackH - thumbH) * gRightScroll / maxScroll);
            if (gRightCount <= VISIBLE_ROWS) thumbY = 64;
            SetRect(&thumbRect, 503, thumbY, 506, thumbY + thumbH);
            FillRect(&thumbRect, &qd.gray);
        }
    }

    /* --------------------------------------------------------------------
     * 3. Bottom Play Bar (y = 273..322, White background)
     * -------------------------------------------------------------------- */
    SetRect(&r, 0, 273, CANVAS_W, CANVAS_H);
    EraseRect(&r);
    TextMode(srcOr);
    PenNormal();

    /* Shuffle button (195, 285) 14x11, box (191, 281)-(213, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_SHUFFLE);
        Boolean prs = (gPressedBtn == BTN_SHUFFLE);
        short dy = prs ? 1 : 0;
        DrawTransportBtnBox(191, 213, hov, prs);
        if (strcmp(gNowPlaying.shuffleMode, "smart") == 0) {
            BlitIcon1Bit(gIconShuffleSmart, 14, 11, 195, 285 + dy, srcCopy);
        } else if (strcmp(gNowPlaying.shuffleMode, "on") == 0) {
            BlitIcon1Bit(gIconShuffleOn, 14, 11, 195, 285 + dy, srcCopy);
        } else {
            BlitIcon1Bit(gIconShuffleOff, 14, 11, 195, 285 + dy, srcCopy);
        }
    }

    /* Previous button (224, 286) 6x9, box (216, 281)-(238, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_PREV);
        Boolean prs = (gPressedBtn == BTN_PREV);
        short dy = prs ? 1 : 0;
        DrawTransportBtnBox(216, 238, hov, prs);
        BlitIcon1Bit(gIconPrev, 6, 9, 224, 286 + dy, srcCopy);
    }

    /* Play / Pause circle (244, 278) 24x24 (shifts down 1px when pressed) */
    {
        Boolean prs = (gPressedBtn == BTN_PLAY);
        short dy = prs ? 1 : 0;
        if (gNowPlaying.isPlaying) {
            BlitIcon1Bit(gIconPauseCircle, 24, 24, 244, 278 + dy, srcCopy);
        } else {
            BlitIcon1Bit(gIconPlayCircle, 24, 24, 244, 278 + dy, srcCopy);
        }
    }

    /* Next button (281, 286) 6x9, box (273, 281)-(295, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_NEXT);
        Boolean prs = (gPressedBtn == BTN_NEXT);
        short dy = prs ? 1 : 0;
        DrawTransportBtnBox(273, 295, hov, prs);
        BlitIcon1Bit(gIconNext, 6, 9, 281, 286 + dy, srcCopy);
    }

    /* Repeat button (302, 286) 12x11, box (297, 281)-(319, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_REPEAT);
        Boolean prs = (gPressedBtn == BTN_REPEAT);
        short dy = prs ? 1 : 0;
        DrawTransportBtnBox(297, 319, hov, prs);
        if (strcmp(gNowPlaying.repeatState, "track") == 0) {
            BlitIcon1Bit(gIconRepeatTrack, 12, 11, 302, 286 + dy, srcCopy);
        } else if (strcmp(gNowPlaying.repeatState, "context") == 0) {
            BlitIcon1Bit(gIconRepeatAll, 12, 11, 302, 286 + dy, srcCopy);
        } else {
            BlitIcon1Bit(gIconRepeatOff, 12, 11, 302, 286 + dy, srcCopy);
        }
    }

    /* Vertical dotted divider (331, 281..300) */
    DrawVerticalDottedLine(331, 281, 300, false);

    /* Speaker Icon (348, 285) 8x11 + Active Speaker Name (364, 294) */
    {
        Boolean hov = (gHoverBtn == BTN_SPEAKER) || gSpeakerPopupOpen;
        Boolean prs = (gPressedBtn == BTN_SPEAKER);
        short dy = prs ? 1 : 0;
        short rightX = GetSpeakerButtonRightX();
        DrawTransportBtnBox(342, rightX, hov, prs);
        BlitIcon1Bit(gIconSpeaker, 8, 11, 348, 285 + dy, srcCopy);
        MoveTo(364, 293 + dy);
        if (gNowPlaying.deviceName[0] != '\0') {
            DrawCStringTruncated(gNowPlaying.deviceName, 18);
        } else {
            DrawCStringTruncated("Choose Speaker", 18);
        }
    }

    /* Timeline Bar: elapsed (109, 312), bar (143, 308)-(369, 311), total (374, 312) */
    MoveTo(109, 312);
    DrawCStringTruncated(gNowPlaying.progressMmSs[0] ? gNowPlaying.progressMmSs : "00:00", 5);

    SetRect(&barRect, 143, 308, 369, 311);
    FrameRect(&barRect);
    {
        short fillW = 0;
        short dotX  = 143;
        Boolean hollowDot = (gHoverBtn == BTN_TIMELINE || gPressedBtn == BTN_TIMELINE);
        if (gNowPlaying.durationMs > 0) {
            long pct = (gNowPlaying.progressMs * 226L) / gNowPlaying.durationMs;
            if (pct < 0) pct = 0;
            if (pct > 226) pct = 226;
            fillW = (short)pct;
        }
        if (fillW > 0) {
            SetRect(&r, 143, 308, 143 + fillW, 311);
            PaintRect(&r);
        }
        dotX = 143 + fillW;
        if (dotX < 147) dotX = 147;
        if (dotX > 365) dotX = 365;
        DrawSliderDot(dotX, 309, hollowDot);
    }

    MoveTo(374, 312);
    DrawCStringTruncated(gNowPlaying.durationMmSs[0] ? gNowPlaying.durationMmSs : "00:00", 5);

    /* Volume Icon (424, 306) 10x7 + Volume Bar (438, 308)-(493, 311) */
    BlitIcon1Bit(gIconVolume, 10, 7, 424, 306, srcCopy);
    SetRect(&barRect, 438, 308, 493, 311);
    FrameRect(&barRect);
    {
        short vol = gNowPlaying.volumePct;
        short fillW;
        short dotX;
        Boolean hollowDot = (gHoverBtn == BTN_VOLUME || gPressedBtn == BTN_VOLUME);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        fillW = (short)((vol * 55L) / 100L);
        if (fillW > 0) {
            SetRect(&r, 438, 308, 438 + fillW, 311);
            PaintRect(&r);
        }
        dotX = 438 + fillW;
        if (dotX < 442) dotX = 442;
        if (dotX > 489) dotX = 489;
        DrawSliderDot(dotX, 309, hollowDot);
    }

    /* --------------------------------------------------------------------
     * 4. Dynamic-Height Speaker Chooser Popover (anchored at yBottom = 272)
     * -------------------------------------------------------------------- */
    if (gSpeakerPopupOpen) {
        Rect popRect, shadowRect;
        short row;
        short visibleDevs = (gDeviceCount > 0) ? gDeviceCount : 1;
        short popH, popTop;

        if (visibleDevs > 8) visibleDevs = 8;
        popH   = visibleDevs * 18 + 12;
        popTop = 272 - popH;

        /* Subtle dithered drop shadow under popover */
        SetRect(&shadowRect, 333, popTop + 3, 468, 275);
        FillRoundRect(&shadowRect, 12, 12, &qd.gray);

        SetRect(&popRect, 330, popTop, 466, 272);
        EraseRoundRect(&popRect, 12, 12);
        FrameRoundRect(&popRect, 12, 12);

        /* Downward pointer triangle at (347, 272)-(355, 276) */
        for (row = 0; row < 5; row++) {
            MoveTo(347 + row, 271 + row);
            PenMode(patBic);
            LineTo(355 - row, 271 + row);
            PenMode(patCopy);
            MoveTo(347 + row, 272 + row);
            LineTo(347 + row, 272 + row);
            MoveTo(355 - row, 272 + row);
            LineTo(355 - row, 272 + row);
        }

        if (gDeviceCount == 0) {
            TextMode(srcOr);
            MoveTo(342, popTop + 18);
            DrawCStringTruncated("No speakers found", 17);
        }

        for (row = 0; row < visibleDevs && row < gDeviceCount; row++) {
            short yTop = popTop + 6 + row * 18;
            DeviceItem *dev = &gDevices[row];
            const unsigned char *devIcon = gIconSpeaker;
            short iconW = 8, iconH = 11, iconX = 339, iconY = yTop + 3;

            if (strstr(dev->type, "TV") || strstr(dev->type, "Cast") || strstr(dev->name, "projector")) {
                devIcon = gIconDevTV; iconW = 11; iconH = 9; iconX = 338; iconY = yTop + 4;
            } else if (strstr(dev->type, "Game") || strstr(dev->name, "Playstation")) {
                devIcon = gIconDevGamepad; iconW = 12; iconH = 9; iconX = 337; iconY = yTop + 4;
            } else if (strstr(dev->type, "AVR") || strstr(dev->type, "Audio") || strstr(dev->name, "Receiver")) {
                devIcon = gIconDevReceiver; iconW = 12; iconH = 8; iconX = 337; iconY = yTop + 5;
            } else if (strstr(dev->type, "Computer") || strstr(dev->name, "Macbook") || strstr(dev->name, "MacBook")) {
                devIcon = gIconDevComputer; iconW = 8; iconH = 8; iconX = 339; iconY = yTop + 5;
            }

            if (dev->isActive) {
                Rect hiRect;
                SetRect(&hiRect, 331, yTop, 465, yTop + 17);
                PaintRect(&hiRect);
                BlitIcon1Bit(devIcon, iconW, iconH, iconX, iconY, notSrcCopy);
                TextMode(srcBic);
            } else {
                BlitIcon1Bit(devIcon, iconW, iconH, iconX, iconY, srcCopy);
                TextMode(srcOr);
            }

            MoveTo(356, yTop + 12);
            DrawCStringTruncated(dev->name, 16);
        }
        TextMode(srcOr);
    }

    /* --------------------------------------------------------------------
     * 5. About MaciSpot Modal Dialog
     * -------------------------------------------------------------------- */
    if (gAboutModalOpen) {
        Rect dlgRect, shadowRect, btnOk;

        SetRect(&shadowRect, 144, 74, 374, 232);
        FillRoundRect(&shadowRect, 10, 10, &qd.gray);

        SetRect(&dlgRect, 140, 70, 370, 228);
        EraseRoundRect(&dlgRect, 10, 10);
        FrameRoundRect(&dlgRect, 10, 10);
        InsetRect(&dlgRect, 2, 2);
        FrameRoundRect(&dlgRect, 8, 8);

        /* MaciSpot Logo (56x44) centered at top of About dialog */
        BlitIcon1Bit(gMaciSpotLogoBits, 56, 44, 228, 82, srcCopy);

        TextMode(srcOr);
        MoveTo(184, 142);
        DrawCStringTruncated("MaciSpot " APP_VERSION_SHORT, 32);

        MoveTo(188, 158);
        DrawCStringTruncated("\"" APP_VERSION_CODENAME "\"", 32);

        MoveTo(164, 174);
        DrawCStringTruncated("68k Native + Sound Manager", 32);

        SetRect(&btnOk, 215, 190, 295, 214);
        PaintRoundRect(&btnOk, 6, 6);
        TextMode(srcBic);
        MoveTo(248, 205);
        DrawCStringTruncated("OK", 4);
        TextMode(srcOr);
    }

    /* Blit offscreen canvas to main window in a single tear-free CopyBits call */
    if (gOffscreenReady) {
        Rect fullRect;
        SetRect(&fullRect, 0, 0, CANVAS_W, CANVAS_H);
        SetPort(gMainWindow);
        CopyBits(&gOffscreenPort.portBits, &gMainWindow->portBits, &fullRect, &fullRect, srcCopy, NULL);
    }

    SetPort(savePort);
}

/* ============================================================================
 * Middleware API Actions & Data Loading
 * ============================================================================ */

static void SyncLocalAudioWithSpeaker(void)
{
    Boolean is68kSpeaker = false;
    if (strstr(gNowPlaying.deviceName, "Macintosh 68k") != NULL ||
        strstr(gNowPlaying.deviceName, "macintosh 68k") != NULL) {
        is68kSpeaker = true;
    }

    if (is68kSpeaker && gNowPlaying.isPlaying) {
        if (!AudioStreamIsRunning()) {
            long n = NetHttpGet("/api/spotify/audio-config", gHttpBuf, HTTP_BUF_SIZE);
            if (n > 0 && strncmp(gHttpBuf, "AUDIO|", 6) == 0) {
                char hzStr[16];
                GetPipeField(gHttpBuf, 3, hzStr, sizeof(hzStr));
                if (atoi(hzStr) > 8000) {
                    gAudioSampleRateHz = (unsigned short)atoi(hzStr);
                }
            }
            AudioStreamStart(gAudioSampleRateHz);
        }
    } else {
        if (AudioStreamIsRunning()) {
            AudioStreamStop();
        }
    }
}

static void FetchCurrentArtwork(void)
{
    long n;
    if (!NetHasServer() || !gHttpBuf) return;

    n = NetHttpGet("/api/spotify/artwork?size=200&format=raw", gHttpBuf, HTTP_BUF_SIZE);
    if (n >= 5208) {
        memcpy(gCurrentArtworkBits, gHttpBuf + 8, 5200);
        SafeCopy(gLoadedArtTrackId, gNowPlaying.trackId, sizeof(gLoadedArtTrackId));
    }
}

static void RefreshNowPlaying(Boolean fetchArtIfChanged)
{
    long n;
    char field[96];

    if (!NetHasServer() || !gHttpBuf) return;

    n = NetHttpGet("/api/spotify/now-playing", gHttpBuf, HTTP_BUF_SIZE);
    if (n <= 0 || strncmp(gHttpBuf, "PLAYING|", 8) != 0) {
        return;
    }

    GetPipeField(gHttpBuf, 1, field, sizeof(field));
    gNowPlaying.isPlaying = (field[0] == '1');
    gTimelineFrozen       = false;

    GetPipeField(gHttpBuf, 2, gNowPlaying.trackId, sizeof(gNowPlaying.trackId));
    GetPipeField(gHttpBuf, 3, gNowPlaying.trackName, sizeof(gNowPlaying.trackName));
    GetPipeField(gHttpBuf, 4, gNowPlaying.artistName, sizeof(gNowPlaying.artistName));
    GetPipeField(gHttpBuf, 5, gNowPlaying.albumName, sizeof(gNowPlaying.albumName));

    GetPipeField(gHttpBuf, 6, field, sizeof(field));
    gNowPlaying.progressMs = atol(field);

    GetPipeField(gHttpBuf, 7, field, sizeof(field));
    gNowPlaying.durationMs = atol(field);

    /* Always format timecodes with leading zeros (02:00, never 2:00) */
    FormatMmSs(gNowPlaying.progressMs, gNowPlaying.progressMmSs);
    FormatMmSs(gNowPlaying.durationMs, gNowPlaying.durationMmSs);

    GetPipeField(gHttpBuf, 10, field, sizeof(field));
    gNowPlaying.volumePct = (short)atoi(field);

    GetPipeField(gHttpBuf, 11, gNowPlaying.shuffleMode, sizeof(gNowPlaying.shuffleMode));
    GetPipeField(gHttpBuf, 12, gNowPlaying.repeatState, sizeof(gNowPlaying.repeatState));

    GetPipeField(gHttpBuf, 13, field, sizeof(field));
    gNowPlaying.isLiked = (field[0] == '1');

    GetPipeField(gHttpBuf, 14, gNowPlaying.deviceName, sizeof(gNowPlaying.deviceName));

    if (gNowPlaying.trackId[0] == '\0') {
        /* Nothing playing -> show initial.png placeholder artwork */
        memcpy(gCurrentArtworkBits, gInitialArtworkBits, 5200);
        gLoadedArtTrackId[0] = '\0';
    } else if (fetchArtIfChanged && strcmp(gNowPlaying.trackId, gLoadedArtTrackId) != 0) {
        FetchCurrentArtwork();
    }

    SyncLocalAudioWithSpeaker();
}

static void ParseTracksResponseIntoRightList(const char *buf)
{
    const char *line = buf;
    gRightCount    = 0;
    gRightSelected = -1;
    gRightScroll   = 0;

    while (line && *line && gRightCount < MAX_RIGHT_ITEMS) {
        const char *nextLine = strchr(line, '\n');
        char rowKind[16];
        GetPipeField(line, 0, rowKind, sizeof(rowKind));

        if (strcmp(rowKind, "TRACK") == 0 ||
            strcmp(rowKind, "RECENT") == 0 ||
            strcmp(rowKind, "TOP_TRACK") == 0 ||
            strcmp(rowKind, "QUEUE") == 0) {
            RightItem *it = &gRightItems[gRightCount++];
            SafeCopy(it->kind, "TRACK", sizeof(it->kind));
            GetPipeField(line, 1, it->id, sizeof(it->id));
            GetPipeField(line, 2, it->name, sizeof(it->name));
            GetPipeField(line, 3, it->artist, sizeof(it->artist));
            GetPipeField(line, 6, it->durationMmSs, sizeof(it->durationMmSs));
            GetPipeField(line, 7, it->uri, sizeof(it->uri));
        } else if (strcmp(rowKind, "ALBUM") == 0) {
            RightItem *it = &gRightItems[gRightCount++];
            SafeCopy(it->kind, "ALBUM", sizeof(it->kind));
            GetPipeField(line, 1, it->id, sizeof(it->id));
            GetPipeField(line, 2, it->name, sizeof(it->name));
            GetPipeField(line, 3, it->artist, sizeof(it->artist));
            it->durationMmSs[0] = '\0';
            GetPipeField(line, 6, it->uri, sizeof(it->uri));
        }

        if (!nextLine) break;
        line = nextLine + 1;
    }

    if (gRightCount > 0) {
        gRightSelected = 0;
    }
}

static void LoadSelectedLeftItemIntoRightList(void)
{
    LeftItem *sel;
    char path[160];
    long n;

    if (!NetHasServer() || gLeftSelected < 0 || gLeftSelected >= gLeftCount) return;
    sel = &gLeftItems[gLeftSelected];

    SafeCopy(gRightHeaderLabel, "Tracks", sizeof(gRightHeaderLabel));
    gActiveContextUri[0] = '\0';

    if (strcmp(sel->kind, "LIKED") == 0) {
        n = NetHttpGet("/api/spotify/liked?limit=40", gHttpBuf, HTTP_BUF_SIZE);
        if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
    } else if (strcmp(sel->kind, "PLAYLIST") == 0) {
        SafeCopy(gActiveContextUri, sel->uri, sizeof(gActiveContextUri));
        sprintf(path, "/api/spotify/playlist/%s/tracks?limit=40", sel->id);
        n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
        /* Fallback if playlist has 0 accessible track items: provide direct playable entry */
        if (gRightCount == 0 && sel->uri[0] != '\0') {
            RightItem *it = &gRightItems[0];
            SafeCopy(it->kind, "PLAYLIST", sizeof(it->kind));
            SafeCopy(it->id, sel->id, sizeof(it->id));
            SafeCopy(it->name, sel->name, sizeof(it->name));
            SafeCopy(it->uri, sel->uri, sizeof(it->uri));
            gRightCount    = 1;
            gRightSelected = 0;
        }
    } else if (strcmp(sel->kind, "HOME_CAT") == 0) {
        if (strcmp(sel->id, "recent") == 0) {
            n = NetHttpGet("/api/spotify/recently-played?limit=35", gHttpBuf, HTTP_BUF_SIZE);
            if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
        } else if (strcmp(sel->id, "top") == 0) {
            n = NetHttpGet("/api/spotify/top?type=tracks&limit=35", gHttpBuf, HTTP_BUF_SIZE);
            if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
        } else if (strcmp(sel->id, "albums") == 0) {
            SafeCopy(gRightHeaderLabel, "Albums", sizeof(gRightHeaderLabel));
            n = NetHttpGet("/api/spotify/albums?limit=35", gHttpBuf, HTTP_BUF_SIZE);
            if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
        } else if (strcmp(sel->id, "new") == 0) {
            SafeCopy(gRightHeaderLabel, "New Releases", sizeof(gRightHeaderLabel));
            n = NetHttpGet("/api/spotify/new-releases?limit=35", gHttpBuf, HTTP_BUF_SIZE);
            if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
        }
    }
}

static void LoadLeftColumn(void)
{
    gLeftCount    = 0;
    gLeftSelected = 0;
    gLeftScroll   = 0;

    if (gHomeMode) {
        SafeCopy(gLeftItems[0].kind, "HOME_CAT", 16);
        SafeCopy(gLeftItems[0].id,   "recent",   64);
        SafeCopy(gLeftItems[0].name, "Recently Played", 64);

        SafeCopy(gLeftItems[1].kind, "HOME_CAT", 16);
        SafeCopy(gLeftItems[1].id,   "top",      64);
        SafeCopy(gLeftItems[1].name, "Top Tracks", 64);

        SafeCopy(gLeftItems[2].kind, "HOME_CAT", 16);
        SafeCopy(gLeftItems[2].id,   "albums",   64);
        SafeCopy(gLeftItems[2].name, "Saved Albums", 64);

        SafeCopy(gLeftItems[3].kind, "HOME_CAT", 16);
        SafeCopy(gLeftItems[3].id,   "new",      64);
        SafeCopy(gLeftItems[3].name, "New Releases", 64);

        gLeftCount = 4;
    } else {
        long n;
        SafeCopy(gLeftItems[0].kind, "LIKED", 16);
        SafeCopy(gLeftItems[0].id,   "liked", 64);
        SafeCopy(gLeftItems[0].name, "Liked", 64);
        gLeftItems[0].uri[0] = '\0';
        gLeftCount = 1;

        if (NetHasServer() && gHttpBuf) {
            n = NetHttpGet("/api/spotify/playlists?limit=35", gHttpBuf, HTTP_BUF_SIZE);
            if (n > 0) {
                const char *line = gHttpBuf;
                while (line && *line && gLeftCount < MAX_LEFT_ITEMS) {
                    const char *nextLine = strchr(line, '\n');
                    char rowKind[16];
                    GetPipeField(line, 0, rowKind, sizeof(rowKind));
                    if (strcmp(rowKind, "PLAYLIST") == 0) {
                        LeftItem *it = &gLeftItems[gLeftCount++];
                        SafeCopy(it->kind, "PLAYLIST", sizeof(it->kind));
                        GetPipeField(line, 1, it->id, sizeof(it->id));
                        GetPipeField(line, 2, it->name, sizeof(it->name));
                        GetPipeField(line, 5, it->uri, sizeof(it->uri));
                    }
                    if (!nextLine) break;
                    line = nextLine + 1;
                }
            }
        }
    }

    LoadSelectedLeftItemIntoRightList();
}

static void ActivateSelectedRightItem(void)
{
    RightItem *it;
    char path[320];
    char encUri[128];

    if (!NetHasServer() || gRightSelected < 0 || gRightSelected >= gRightCount) return;
    it = &gRightItems[gRightSelected];

    if (strcmp(it->kind, "ALBUM") == 0) {
        SafeCopy(gActiveContextUri, it->uri, sizeof(gActiveContextUri));
        SafeCopy(gRightHeaderLabel, "Tracks", sizeof(gRightHeaderLabel));
        sprintf(path, "/api/spotify/album/%s/tracks?limit=50", it->id);
        if (NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE) > 0) {
            ParseTracksResponseIntoRightList(gHttpBuf);
            RenderCanvas();
        }
        return;
    }

    /* Optimistic UI update before network request */
    gTimelineFrozen = true;
    gNowPlaying.isPlaying = true;
    gNowPlaying.progressMs = 0;
    FormatMmSs(0, gNowPlaying.progressMmSs);
    SafeCopy(gNowPlaying.trackName, it->name, sizeof(gNowPlaying.trackName));
    if (it->artist[0] != '\0') {
        SafeCopy(gNowPlaying.artistName, it->artist, sizeof(gNowPlaying.artistName));
    }
    RenderCanvas();

    if (strcmp(it->kind, "PLAYLIST") == 0) {
        UrlEncode(it->uri, encUri, sizeof(encUri));
        sprintf(path, "/api/spotify/play?context_uri=%s", encUri);
    } else {
        UrlEncode(it->uri[0] ? it->uri : it->id, encUri, sizeof(encUri));
        if (gActiveContextUri[0] != '\0') {
            char encCtx[128];
            UrlEncode(gActiveContextUri, encCtx, sizeof(encCtx));
            sprintf(path, "/api/spotify/play?context_uri=%s&offset_uri=%s", encCtx, encUri);
        } else {
            sprintf(path, "/api/spotify/play?uri=%s", encUri);
        }
    }

    NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
    RefreshNowPlaying(true);
    RenderCanvas();
}

static void ParseSearchResults(const char *buf)
{
    const char *line = buf;
    gSearchCount    = 0;
    gSearchScroll   = 0;
    gSearchHoverRow = 0;

    while (line && *line && gSearchCount < MAX_SEARCH_ITEMS) {
        const char *nextLine = strchr(line, '\n');
        char rowKind[16];
        GetPipeField(line, 0, rowKind, sizeof(rowKind));

        if (strcmp(rowKind, "TRACK") == 0) {
            SearchItem *s = &gSearchItems[gSearchCount++];
            SafeCopy(s->kind, "TRACK", sizeof(s->kind));
            GetPipeField(line, 1, s->id, sizeof(s->id));
            GetPipeField(line, 2, s->title, sizeof(s->title));
            GetPipeField(line, 3, s->subtitle, sizeof(s->subtitle));
            GetPipeField(line, 7, s->uri, sizeof(s->uri));
        } else if (strcmp(rowKind, "ALBUM") == 0) {
            SearchItem *s = &gSearchItems[gSearchCount++];
            SafeCopy(s->kind, "ALBUM", sizeof(s->kind));
            GetPipeField(line, 1, s->id, sizeof(s->id));
            GetPipeField(line, 2, s->title, sizeof(s->title));
            GetPipeField(line, 3, s->subtitle, sizeof(s->subtitle));
            GetPipeField(line, 6, s->uri, sizeof(s->uri));
        } else if (strcmp(rowKind, "PLAYLIST") == 0) {
            SearchItem *s = &gSearchItems[gSearchCount++];
            SafeCopy(s->kind, "PLAYLIST", sizeof(s->kind));
            GetPipeField(line, 1, s->id, sizeof(s->id));
            GetPipeField(line, 2, s->title, sizeof(s->title));
            GetPipeField(line, 3, s->subtitle, sizeof(s->subtitle));
            GetPipeField(line, 5, s->uri, sizeof(s->uri));
        }

        if (!nextLine) break;
        line = nextLine + 1;
    }
}

static void ExecuteSearchQuery(void)
{
    char encQ[128];
    char path[192];

    if (!NetHasServer() || gSearchQuery[0] == '\0') return;
    UrlEncode(gSearchQuery, encQ, sizeof(encQ));
    sprintf(path, "/api/spotify/search?q=%s&type=all&limit=10", encQ);

    if (NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE) > 0) {
        ParseSearchResults(gHttpBuf);
    } else {
        gSearchCount = 0;
    }
    gSearchFocused  = false;
    gSearchViewOpen = true;
    RenderCanvas();
}

static void ActivateSearchResultRow(short idx)
{
    SearchItem *s;
    char encUri[128];
    char path[256];

    if (!NetHasServer() || idx < 0 || idx >= gSearchCount) return;
    s = &gSearchItems[idx];

    /* Single-click plays immediately and closes search view back to main UI */
    gSearchViewOpen = false;
    gSearchFocused  = false;
    gTimelineFrozen = true;
    gNowPlaying.isPlaying  = true;
    gNowPlaying.progressMs = 0;
    FormatMmSs(0, gNowPlaying.progressMmSs);
    SafeCopy(gNowPlaying.trackName, s->title, sizeof(gNowPlaying.trackName));
    SafeCopy(gNowPlaying.artistName, s->subtitle, sizeof(gNowPlaying.artistName));
    RenderCanvas();

    UrlEncode(s->uri[0] ? s->uri : s->id, encUri, sizeof(encUri));
    if (strcmp(s->kind, "ALBUM") == 0 || strcmp(s->kind, "PLAYLIST") == 0) {
        SafeCopy(gActiveContextUri, s->uri, sizeof(gActiveContextUri));
        sprintf(path, "/api/spotify/play?context_uri=%s", encUri);
        NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        /* Also load the selected album/playlist tracks into the right panel */
        if (strcmp(s->kind, "ALBUM") == 0) {
            sprintf(path, "/api/spotify/album/%s/tracks?limit=50", s->id);
        } else {
            sprintf(path, "/api/spotify/playlist/%s/tracks?limit=40", s->id);
        }
        if (NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE) > 0) {
            ParseTracksResponseIntoRightList(gHttpBuf);
        }
    } else {
        sprintf(path, "/api/spotify/play?uri=%s", encUri);
        NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
    }

    RefreshNowPlaying(true);
    RenderCanvas();
}

static void LoadDevicesAndTogglePopover(void)
{
    long n;
    if (gSpeakerPopupOpen) {
        gSpeakerPopupOpen = false;
        RenderCanvas();
        return;
    }

    if (!NetHasServer() || !gHttpBuf) return;
    gDeviceCount = 0;

    n = NetHttpGet("/api/spotify/devices", gHttpBuf, HTTP_BUF_SIZE);
    if (n > 0) {
        const char *line = gHttpBuf;
        while (line && *line && gDeviceCount < MAX_DEVICES) {
            const char *nextLine = strchr(line, '\n');
            char rowKind[16];
            GetPipeField(line, 0, rowKind, sizeof(rowKind));
            if (strcmp(rowKind, "DEVICE") == 0) {
                char activeFlag[8], volStr[12];
                DeviceItem *d = &gDevices[gDeviceCount++];
                GetPipeField(line, 1, d->id, sizeof(d->id));
                GetPipeField(line, 2, d->name, sizeof(d->name));
                GetPipeField(line, 3, d->type, sizeof(d->type));
                GetPipeField(line, 4, activeFlag, sizeof(activeFlag));
                GetPipeField(line, 5, volStr, sizeof(volStr));
                d->isActive  = (activeFlag[0] == '1');
                d->volumePct = (short)atoi(volStr);
            }
            if (!nextLine) break;
            line = nextLine + 1;
        }
    }

    gSpeakerPopupOpen = true;
    RenderCanvas();
}

static void ConnectAndLoadAll(void)
{
    RefreshNowPlaying(true);
    LoadLeftColumn();
    gInitialDataLoaded = true;
}

static Boolean VerifyServerReachable(void)
{
    long n;
    if (!NetHasServer() || !gHttpBuf) return false;
    n = NetHttpGet("/api/spotify/status", gHttpBuf, HTTP_BUF_SIZE);
    return (n > 0 && strncmp(gHttpBuf, "STATUS|", 7) == 0);
}

static void TransitionToServerFound(void)
{
    gConnState         = CONN_FOUND;
    gFoundCountdownSec = 3;
    gFoundStartTicks   = TickCount();
    gInitialDataLoaded = false;
    RenderCanvas();
}

static void RunInitialDiscovery(void)
{
    gConnState = CONN_SEARCHING;
    RenderCanvas();

    if (NetDiscoverServer()) {
        FormatIPAddress(NetGetServerIP(), gIpInput);
        TransitionToServerFound();
    } else {
        gConnState = CONN_FAILED;
        RenderCanvas();
    }
}

/* ============================================================================
 * Hover & Drag Tracking Helpers
 * ============================================================================ */

static short HitTestTransportHover(Point pt)
{
    if (gConnState != CONN_READY || gAboutModalOpen) return BTN_NONE;
    if (pt.v >= 278 && pt.v <= 301) {
        if (pt.h >= 191 && pt.h <= 213) return BTN_SHUFFLE;
        if (pt.h >= 216 && pt.h <= 238) return BTN_PREV;
        if (pt.h >= 242 && pt.h <= 270) return BTN_PLAY;
        if (pt.h >= 273 && pt.h <= 295) return BTN_NEXT;
        if (pt.h >= 297 && pt.h <= 319) return BTN_REPEAT;
        if (pt.h >= 342 && pt.h <= GetSpeakerButtonRightX()) return BTN_SPEAKER;
    }
    if (pt.v >= 303 && pt.v <= 318) {
        if (pt.h >= 140 && pt.h <= 372) return BTN_TIMELINE;
        if (pt.h >= 435 && pt.h <= 496) return BTN_VOLUME;
    }
    return BTN_NONE;
}

static void UpdateHoverState(void)
{
    Point pt;
    short newBtn;
    short newSearchRow = -1;
    Boolean changed = false;

    if (!gMainWindow || gConnState != CONN_READY) return;
    SetPort(gMainWindow);
    GetMouse(&pt);

    newBtn = HitTestTransportHover(pt);
    if (newBtn != gHoverBtn) {
        gHoverBtn = newBtn;
        changed = true;
    }

    if (gSearchViewOpen) {
        if (pt.h >= 61 && pt.h <= 452 && pt.v >= SEARCH_TOP_Y && pt.v < (SEARCH_TOP_Y + SEARCH_VISIBLE_ROWS * SEARCH_ROW_H)) {
            short r = gSearchScroll + (pt.v - SEARCH_TOP_Y) / SEARCH_ROW_H;
            if (r >= 0 && r < gSearchCount) {
                newSearchRow = r;
            }
        }
        if (newSearchRow != gSearchHoverRow) {
            gSearchHoverRow = newSearchRow;
            changed = true;
        }
    }

    if (changed) {
        RenderCanvas();
    }
}

/* Tracks a transport button press with visual down-state (`button-states.png` Row 3) */
static Boolean TrackTransportButtonPress(short btnId)
{
    Point pt;
    gPressedBtn = btnId;
    RenderCanvas();
    while (StillDown()) {
        GetMouse(&pt);
    }
    gPressedBtn = BTN_NONE;
    return true;
}

/* ============================================================================
 * Mouse & Keyboard Interaction
 * ============================================================================ */

static void HandleCanvasClick(Point pt)
{
    /* 0. If About Modal is open, any click closes it */
    if (gAboutModalOpen) {
        gAboutModalOpen = false;
        RenderCanvas();
        return;
    }

    /* 1. Pre-Connection Server Screens (Server1.png, Server2.png, Server3.png) */
    if (gConnState != CONN_READY) {
        if (gConnState == CONN_FOUND) {
            /* "Let's party!" button (204, 154)-(311, 180) skips remaining countdown */
            if (pt.h >= 200 && pt.h <= 315 && pt.v >= 150 && pt.v <= 184) {
                if (!gInitialDataLoaded) {
                    ConnectAndLoadAll();
                }
                gConnState = CONN_READY;
                RenderCanvas();
            }
            return;
        }

        /* "Retry Scan" button (204, 154)-(311, 180) in Server2.png */
        if (gConnState == CONN_FAILED && pt.h >= 200 && pt.h <= 315 && pt.v >= 150 && pt.v <= 184) {
            RunInitialDiscovery();
            return;
        }

        /* "Connect" button (281, 217)-(346, 243) in Server1.png / Server2.png */
        if (pt.h >= 278 && pt.h <= 350 && pt.v >= 214 && pt.v <= 246) {
            ip_addr parsed = 0;
            if (ParseIPAddress(gIpInput, &parsed)) {
                gConnState = CONN_SEARCHING;
                RenderCanvas();
                NetSetServer(parsed, DEFAULT_HTTP_PORT);
                if (VerifyServerReachable()) {
                    TransitionToServerFound();
                } else {
                    gConnState = CONN_FAILED;
                    RenderCanvas();
                }
            }
            return;
        }
        return;
    }

    /* 2. Dynamic-Height Speaker Chooser Popover */
    if (gSpeakerPopupOpen) {
        short visibleDevs = (gDeviceCount > 0) ? gDeviceCount : 1;
        short popH, popTop;
        if (visibleDevs > 8) visibleDevs = 8;
        popH   = visibleDevs * 18 + 12;
        popTop = 272 - popH;

        if (pt.h >= 330 && pt.h <= 466 && pt.v >= popTop && pt.v <= 272) {
            short row = (pt.v - (popTop + 6)) / 18;
            if (row >= 0 && row < visibleDevs && row < gDeviceCount) {
                char path[160];
                char encId[96];
                SafeCopy(gNowPlaying.deviceName, gDevices[row].name, sizeof(gNowPlaying.deviceName));
                gSpeakerPopupOpen = false;
                RenderCanvas();

                UrlEncode(gDevices[row].id, encId, sizeof(encId));
                sprintf(path, "/api/spotify/transfer?device_id=%s", encId);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
            }
            return;
        } else {
            gSpeakerPopupOpen = false;
            RenderCanvas();
            return;
        }
    }

    /* 3. Top Header Bar (y = 0..22) */
    if (pt.v < 22) {
        /* Home / Playlists Toggle (90..138) */
        if (pt.h >= 90 && pt.h <= 138) {
            gHomeMode = !gHomeMode;
            gSearchFocused  = false;
            gSearchViewOpen = false;
            LoadLeftColumn();
            RenderCanvas();
            return;
        }
        /* Search Pill (156..356) */
        if (pt.h >= 156 && pt.h <= 356) {
            /* Clear (X) icon on right side of search pill (340..356) */
            if (pt.h >= 340 && (gSearchViewOpen || gSearchQuery[0] != '\0')) {
                gSearchQuery[0] = '\0';
                gSearchViewOpen = false;
                gSearchFocused  = false;
                RenderCanvas();
                return;
            }
            /* Magnifying glass icon on left side of search pill (156..172) triggers search if query non-empty */
            if (pt.h <= 172 && gSearchQuery[0] != '\0') {
                ExecuteSearchQuery();
                return;
            }
            gSearchFocused = true;
            RenderCanvas();
            return;
        }
        gSearchFocused = false;
        RenderCanvas();
        return;
    }

    gSearchFocused = false;

    /* 4. Middle Content Area (y = 22..272) */
    if (pt.v >= 22 && pt.v < 273) {
        /* 4A. If Search Results Center Panel is open */
        if (gSearchViewOpen) {
            /* Top-right Close Circle (X) at (485, 34) 15x15 */
            if (pt.h >= 480 && pt.h <= 504 && pt.v >= 30 && pt.v <= 54) {
                gSearchViewOpen = false;
                RenderCanvas();
                return;
            }
            /* Draggable Search Scrollbar (x = 463..476, y = 35..266) */
            if (pt.h >= 463 && pt.h <= 476 && gSearchCount > SEARCH_VISIBLE_ROWS) {
                short maxScroll = gSearchCount - SEARCH_VISIBLE_ROWS;
                Point curPt = pt;
                do {
                    short newScroll = (short)((long)(curPt.v - 35) * maxScroll / 200);
                    if (newScroll < 0) newScroll = 0;
                    if (newScroll > maxScroll) newScroll = maxScroll;
                    if (newScroll != gSearchScroll) {
                        gSearchScroll = newScroll;
                        RenderCanvas();
                    }
                    GetMouse(&curPt);
                } while (StillDown());
                return;
            }
            /* Single-click on a Search Result Row (x = 61..452, y = 34..272) */
            if (pt.h >= 61 && pt.h <= 452 && pt.v >= SEARCH_TOP_Y && pt.v < (SEARCH_TOP_Y + SEARCH_VISIBLE_ROWS * SEARCH_ROW_H)) {
                short clickedIdx = gSearchScroll + (pt.v - SEARCH_TOP_Y) / SEARCH_ROW_H;
                if (clickedIdx >= 0 && clickedIdx < gSearchCount) {
                    ActivateSearchResultRow(clickedIdx);
                }
                return;
            }
            return;
        }

        /* 4B. Normal 3-Column View: Draggable Left Scrollbar (x = 139..150) */
        if (pt.h >= 139 && pt.h <= 150 && gLeftCount > VISIBLE_ROWS) {
            short maxScroll = gLeftCount - VISIBLE_ROWS;
            Point curPt = pt;
            do {
                short newScroll = (short)((long)(curPt.v - 35) * maxScroll / 200);
                if (newScroll < 0) newScroll = 0;
                if (newScroll > maxScroll) newScroll = maxScroll;
                if (newScroll != gLeftScroll) {
                    gLeftScroll = newScroll;
                    RenderCanvas();
                }
                GetMouse(&curPt);
            } while (StillDown());
            return;
        }
        /* Left Column Items (x = 8..138, y = 35..243) */
        if (pt.h >= 8 && pt.h <= 138 && pt.v >= LIST_TOP_Y && pt.v < LIST_BOTTOM_Y) {
            short clickedRow = gLeftScroll + (pt.v - LIST_TOP_Y) / ROW_HEIGHT;
            if (clickedRow >= 0 && clickedRow < gLeftCount) {
                unsigned long now = TickCount();
                Boolean isDblClick = (clickedRow == gLastLeftClickedRow && (now - gLastLeftClickTicks) <= GetDblTime());
                gLeftSelected       = clickedRow;
                gLastLeftClickedRow = clickedRow;
                gLastLeftClickTicks = now;
                RenderCanvas();

                LoadSelectedLeftItemIntoRightList();
                RenderCanvas();

                /* Double-clicking a playlist in the left column starts playing it right away */
                if (isDblClick && gLeftItems[clickedRow].uri[0] != '\0') {
                    char encUri[128], path[256];
                    gTimelineFrozen = true;
                    gNowPlaying.isPlaying  = true;
                    gNowPlaying.progressMs = 0;
                    FormatMmSs(0, gNowPlaying.progressMmSs);
                    RenderCanvas();

                    UrlEncode(gLeftItems[clickedRow].uri, encUri, sizeof(encUri));
                    sprintf(path, "/api/spotify/play?context_uri=%s", encUri);
                    NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                    RefreshNowPlaying(true);
                    RenderCanvas();
                }
            }
            return;
        }
        /* Draggable Right Scrollbar (x = 499..511) */
        if (pt.h >= 499 && pt.h <= 511 && gRightCount > VISIBLE_ROWS) {
            short maxScroll = gRightCount - VISIBLE_ROWS;
            Point curPt = pt;
            do {
                short newScroll = (short)((long)(curPt.v - 35) * maxScroll / 200);
                if (newScroll < 0) newScroll = 0;
                if (newScroll > maxScroll) newScroll = maxScroll;
                if (newScroll != gRightScroll) {
                    gRightScroll = newScroll;
                    RenderCanvas();
                }
                GetMouse(&curPt);
            } while (StillDown());
            return;
        }
        /* Right Column Items (x = 370..498, y = 35..243) */
        if (pt.h >= 370 && pt.h <= 498 && pt.v >= LIST_TOP_Y && pt.v < LIST_BOTTOM_Y) {
            short clickedRow = gRightScroll + (pt.v - LIST_TOP_Y) / ROW_HEIGHT;
            if (clickedRow >= 0 && clickedRow < gRightCount) {
                unsigned long now = TickCount();
                if (clickedRow == gLastClickedRow && (now - gLastClickTicks) <= GetDblTime()) {
                    gRightSelected = clickedRow;
                    ActivateSelectedRightItem();
                } else {
                    gRightSelected  = clickedRow;
                    gLastClickedRow = clickedRow;
                    gLastClickTicks = now;
                    RenderCanvas();
                }
            }
            return;
        }
        return;
    }

    /* 5. Bottom Play Bar (y = 273..322) */
    if (pt.v >= 273) {
        /* Transport Row (y = 276..303) */
        if (pt.v >= 276 && pt.v <= 303) {
            /* Shuffle (191..213) */
            if (pt.h >= 191 && pt.h <= 213) {
                char path[96];
                TrackTransportButtonPress(BTN_SHUFFLE);
                /* Optimistic UI update */
                if (strcmp(gNowPlaying.shuffleMode, "off") == 0) {
                    SafeCopy(gNowPlaying.shuffleMode, "on", sizeof(gNowPlaying.shuffleMode));
                } else if (strcmp(gNowPlaying.shuffleMode, "on") == 0) {
                    SafeCopy(gNowPlaying.shuffleMode, "smart", sizeof(gNowPlaying.shuffleMode));
                } else {
                    SafeCopy(gNowPlaying.shuffleMode, "off", sizeof(gNowPlaying.shuffleMode));
                }
                RenderCanvas();

                sprintf(path, "/api/spotify/shuffle?mode=%s", gNowPlaying.shuffleMode);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
                return;
            }
            /* Previous (216..238) */
            if (pt.h >= 216 && pt.h <= 238) {
                TrackTransportButtonPress(BTN_PREV);
                /* Stop timeline immediately and reset to 00:00 before server callback */
                gTimelineFrozen        = true;
                gNowPlaying.progressMs = 0;
                FormatMmSs(0, gNowPlaying.progressMmSs);
                RenderCanvas();

                NetHttpGet("/api/spotify/previous", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(true);
                RenderCanvas();
                return;
            }
            /* Play / Pause Circle (242..270) */
            if (pt.h >= 242 && pt.h <= 270) {
                Boolean wantPlay = !gNowPlaying.isPlaying;
                TrackTransportButtonPress(BTN_PLAY);
                /* Optimistic UI update: flip icon and stop timeline immediately */
                gNowPlaying.isPlaying = wantPlay;
                if (!wantPlay) {
                    gTimelineFrozen = true;
                }
                SyncLocalAudioWithSpeaker();
                RenderCanvas();

                if (wantPlay) {
                    NetHttpGet("/api/spotify/play", gHttpBuf, HTTP_BUF_SIZE);
                } else {
                    NetHttpGet("/api/spotify/pause", gHttpBuf, HTTP_BUF_SIZE);
                }
                RefreshNowPlaying(false);
                RenderCanvas();
                return;
            }
            /* Next (273..295) */
            if (pt.h >= 273 && pt.h <= 295) {
                TrackTransportButtonPress(BTN_NEXT);
                /* Stop timeline immediately and reset to 00:00 before server callback */
                gTimelineFrozen        = true;
                gNowPlaying.progressMs = 0;
                FormatMmSs(0, gNowPlaying.progressMmSs);
                RenderCanvas();

                NetHttpGet("/api/spotify/next", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(true);
                RenderCanvas();
                return;
            }
            /* Repeat (297..319) */
            if (pt.h >= 297 && pt.h <= 319) {
                char path[96];
                TrackTransportButtonPress(BTN_REPEAT);
                /* Optimistic UI update */
                if (strcmp(gNowPlaying.repeatState, "off") == 0) {
                    SafeCopy(gNowPlaying.repeatState, "context", sizeof(gNowPlaying.repeatState));
                } else if (strcmp(gNowPlaying.repeatState, "context") == 0) {
                    SafeCopy(gNowPlaying.repeatState, "track", sizeof(gNowPlaying.repeatState));
                } else {
                    SafeCopy(gNowPlaying.repeatState, "off", sizeof(gNowPlaying.repeatState));
                }
                RenderCanvas();

                sprintf(path, "/api/spotify/repeat?state=%s", gNowPlaying.repeatState);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
                return;
            }
            /* Speaker Selector (342..rightX) */
            if (pt.h >= 342 && pt.h <= GetSpeakerButtonRightX()) {
                TrackTransportButtonPress(BTN_SPEAKER);
                LoadDevicesAndTogglePopover();
                return;
            }
        }

        /* Draggable Timeline & Volume Row (y = 303..320) */
        if (pt.v >= 303 && pt.v <= 320) {
            /* Timeline Scrubber (140..372) */
            if (pt.h >= 140 && pt.h <= 372) {
                char path[96];
                short pct = 0;
                Point curPt = pt;
                gPressedBtn     = BTN_TIMELINE;
                gTimelineFrozen = true;
                do {
                    pct = (short)(((long)(curPt.h - 143) * 100L) / 226L);
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    if (gNowPlaying.durationMs > 0) {
                        gNowPlaying.progressMs = (gNowPlaying.durationMs * pct) / 100L;
                        FormatMmSs(gNowPlaying.progressMs, gNowPlaying.progressMmSs);
                    }
                    RenderCanvas();
                    GetMouse(&curPt);
                } while (StillDown());
                gPressedBtn = BTN_NONE;
                RenderCanvas();

                sprintf(path, "/api/spotify/seek?percent=%d", pct);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
                return;
            }
            /* Volume Scrubber (435..496) */
            if (pt.h >= 435 && pt.h <= 496) {
                char path[96];
                short vol = gNowPlaying.volumePct;
                Point curPt = pt;
                gPressedBtn = BTN_VOLUME;
                do {
                    vol = (short)(((long)(curPt.h - 438) * 100L) / 55L);
                    if (vol < 0) vol = 0;
                    if (vol > 100) vol = 100;
                    if (vol != gNowPlaying.volumePct) {
                        gNowPlaying.volumePct = vol;
                        RenderCanvas();
                    }
                    GetMouse(&curPt);
                } while (StillDown());
                gPressedBtn = BTN_NONE;
                RenderCanvas();

                sprintf(path, "/api/spotify/volume?volume_percent=%d", vol);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                return;
            }
        }
    }
}

static void HandleKeyPress(char ch)
{
    /* 0. If About Modal is open, any key closes it */
    if (gAboutModalOpen) {
        gAboutModalOpen = false;
        RenderCanvas();
        return;
    }

    /* 1. Pre-Connection Server Screens Typing */
    if (gConnState != CONN_READY) {
        short len = (short)strlen(gIpInput);
        if (gConnState == CONN_FOUND) {
            if (ch == '\r' || ch == 3 || ch == ' ') {
                if (!gInitialDataLoaded) {
                    ConnectAndLoadAll();
                }
                gConnState = CONN_READY;
                RenderCanvas();
            }
            return;
        }
        if (ch == '\r' || ch == 3) { /* Return or Enter */
            ip_addr parsed = 0;
            if (ParseIPAddress(gIpInput, &parsed)) {
                gConnState = CONN_SEARCHING;
                RenderCanvas();
                NetSetServer(parsed, DEFAULT_HTTP_PORT);
                if (VerifyServerReachable()) {
                    TransitionToServerFound();
                } else {
                    gConnState = CONN_FAILED;
                    RenderCanvas();
                }
            }
        } else if (ch == 8 && len > 0) { /* Backspace */
            gIpInput[len - 1] = '\0';
            RenderCanvas();
        } else if ((ch == '.' || (ch >= '0' && ch <= '9')) && len < (short)(sizeof(gIpInput) - 2)) {
            gIpInput[len]     = ch;
            gIpInput[len + 1] = '\0';
            RenderCanvas();
        }
        return;
    }

    /* 2. Search Bar Typing */
    if (gSearchFocused) {
        short len = (short)strlen(gSearchQuery);
        if (ch == '\r' || ch == 3) {
            ExecuteSearchQuery();
        } else if (ch == 27) { /* Escape */
            gSearchFocused  = false;
            gSearchViewOpen = false;
            RenderCanvas();
        } else if (ch == 8 && len > 0) {
            gSearchQuery[len - 1] = '\0';
            RenderCanvas();
        } else if (ch >= 32 && ch < 127 && len < (short)(sizeof(gSearchQuery) - 2)) {
            gSearchQuery[len]     = ch;
            gSearchQuery[len + 1] = '\0';
            RenderCanvas();
        }
        return;
    }

    /* 3. Global Keyboard Shortcuts */
    if (ch == 27 && gSearchViewOpen) { /* Escape closes search view */
        gSearchViewOpen = false;
        RenderCanvas();
        return;
    }

    if (ch == ' ') {
        Boolean wantPlay = !gNowPlaying.isPlaying;
        gNowPlaying.isPlaying = wantPlay;
        if (!wantPlay) gTimelineFrozen = true;
        SyncLocalAudioWithSpeaker();
        RenderCanvas();

        if (wantPlay) {
            NetHttpGet("/api/spotify/play", gHttpBuf, HTTP_BUF_SIZE);
        } else {
            NetHttpGet("/api/spotify/pause", gHttpBuf, HTTP_BUF_SIZE);
        }
        RefreshNowPlaying(false);
        RenderCanvas();
    } else if (ch == '\r' || ch == 3) {
        if (gSearchViewOpen && gSearchHoverRow >= 0 && gSearchHoverRow < gSearchCount) {
            ActivateSearchResultRow(gSearchHoverRow);
        } else {
            ActivateSelectedRightItem();
        }
    } else if (ch == 30) { /* Up Arrow */
        if (gRightSelected > 0) {
            gRightSelected--;
            if (gRightSelected < gRightScroll) gRightScroll = gRightSelected;
            RenderCanvas();
        }
    } else if (ch == 31) { /* Down Arrow */
        if (gRightSelected + 1 < gRightCount) {
            gRightSelected++;
            if (gRightSelected >= gRightScroll + VISIBLE_ROWS) {
                gRightScroll = gRightSelected - VISIBLE_ROWS + 1;
            }
            RenderCanvas();
        }
    }
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
                gAboutModalOpen = true;
                RenderCanvas();
            } else {
                GetMenuItemText(GetMenuHandle(mApple), itemID, daName);
                OpenDeskAcc(daName);
            }
            break;

        case mFile:
            if (itemID == iServerIP) {
                if (NetHasServer()) {
                    FormatIPAddress(NetGetServerIP(), gIpInput);
                }
                gConnState = CONN_FAILED;
                RenderCanvas();
            } else if (itemID == iFindSearch) {
                if (gConnState == CONN_READY) {
                    gSearchFocused = true;
                    RenderCanvas();
                }
            } else if (itemID == iQuit) {
                gRunning = false;
            }
            break;

        case mPlayback:
            if (gConnState != CONN_READY) break;
            if (itemID == iPlayPause) {
                Boolean wantPlay = !gNowPlaying.isPlaying;
                gNowPlaying.isPlaying = wantPlay;
                if (!wantPlay) gTimelineFrozen = true;
                SyncLocalAudioWithSpeaker();
                RenderCanvas();
                if (wantPlay) {
                    NetHttpGet("/api/spotify/play", gHttpBuf, HTTP_BUF_SIZE);
                } else {
                    NetHttpGet("/api/spotify/pause", gHttpBuf, HTTP_BUF_SIZE);
                }
                RefreshNowPlaying(false);
                RenderCanvas();
            } else if (itemID == iNextTrack) {
                gTimelineFrozen        = true;
                gNowPlaying.progressMs = 0;
                FormatMmSs(0, gNowPlaying.progressMmSs);
                RenderCanvas();
                NetHttpGet("/api/spotify/next", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(true);
                RenderCanvas();
            } else if (itemID == iPrevTrack) {
                gTimelineFrozen        = true;
                gNowPlaying.progressMs = 0;
                FormatMmSs(0, gNowPlaying.progressMmSs);
                RenderCanvas();
                NetHttpGet("/api/spotify/previous", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(true);
                RenderCanvas();
            } else if (itemID == iLikeTrack) {
                NetHttpGet("/api/spotify/toggle-like", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
            } else if (itemID == iChooseSpeaker) {
                LoadDevicesAndTogglePopover();
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

        case inContent:
            if (whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            } else if (whichWindow == gMainWindow) {
                Point pt = ev->where;
                SetPort(gMainWindow);
                GlobalToLocal(&pt);
                if ((ev->modifiers & cmdKey) != 0 && pt.v < 22) {
                    DragWindow(gMainWindow, ev->where, &qd.screenBits.bounds);
                } else {
                    HandleCanvasClick(pt);
                }
            }
            break;
    }
}

/* ============================================================================
 * Main Entry Point & Cooperative Event Loop
 * ============================================================================ */

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

int main(void)
{
    Rect winBounds;
    EventRecord ev;
    short screenW, screenH, winLeft, winTop;

    MaxApplZone();
    MoreMasters();
    MoreMasters();

    InitToolbox();
    SetupMenus();
    NetInit();
    AudioStreamInit();

    gHttpBuf = NewPtrClear(HTTP_BUF_SIZE);
    /* Start with initial.png placeholder graphic ("Party like it's 1984") */
    memcpy(gCurrentArtworkBits, gInitialArtworkBits, 5200);
    memset(&gNowPlaying, 0, sizeof(gNowPlaying));
    SafeCopy(gNowPlaying.progressMmSs, "00:00", sizeof(gNowPlaying.progressMmSs));
    SafeCopy(gNowPlaying.durationMmSs, "00:00", sizeof(gNowPlaying.durationMmSs));
    SafeCopy(gNowPlaying.shuffleMode,  "off",   sizeof(gNowPlaying.shuffleMode));
    SafeCopy(gNowPlaying.repeatState,  "off",   sizeof(gNowPlaying.repeatState));
    gNowPlaying.volumePct = 70;

    InitOffscreenCanvas();

    /* Position flush at (0, 20) on 512x342 Compact Mac, or centered on larger screens */
    screenW = qd.screenBits.bounds.right - qd.screenBits.bounds.left;
    screenH = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
    if (screenW <= 512 && screenH <= 342) {
        winLeft = 0;
        winTop  = 20;
    } else {
        winLeft = (screenW - CANVAS_W) / 2;
        winTop  = 20 + ((screenH - 20 - CANVAS_H) / 2);
    }

    SetRect(&winBounds, winLeft, winTop, winLeft + CANVAS_W, winTop + CANVAS_H);
    gMainWindow = NewWindow(
        NULL,
        &winBounds,
        "\pMaciSpot",
        true,
        plainDBox,
        (WindowPtr)-1L,
        false,
        0
    );

    /* Show Server1.png and perform UDP discovery (:6801); transition to Server3.png or Server2.png */
    RunInitialDiscovery();

    gLastPollTicks   = TickCount();
    gLastSecondTicks = TickCount();

    while (gRunning) {
        unsigned long nowTicks;

        SystemTask();

        /* Pump local 68k Sound Manager TCP stream (:6802) every iteration */
        if (AudioStreamIsRunning()) {
            AudioStreamPump();
        }

        if (WaitNextEvent(everyEvent, &ev, AudioStreamIsRunning() ? 1 : 2, NULL)) {
            switch (ev.what) {
                case mouseDown:
                    HandleMouseDown(&ev);
                    break;

                case keyDown:
                case autoKey:
                    if ((ev.modifiers & cmdKey) != 0) {
                        HandleMenuChoice(MenuKey((char)(ev.message & charCodeMask)));
                    } else {
                        HandleKeyPress((char)(ev.message & charCodeMask));
                    }
                    break;

                case updateEvt:
                    if ((WindowPtr)ev.message == gMainWindow) {
                        BeginUpdate(gMainWindow);
                        RenderCanvas();
                        EndUpdate(gMainWindow);
                    }
                    break;
            }
        }

        /* Track button hover states and search result row hover */
        UpdateHoverState();

        nowTicks = TickCount();

        /* Handle 3-second countdown on Server3.png (CONN_FOUND) */
        if (gConnState == CONN_FOUND) {
            long elapsedTicks;
            short remSec;
            if (!gInitialDataLoaded) {
                ConnectAndLoadAll();
            }
            elapsedTicks = (long)(nowTicks - gFoundStartTicks);
            remSec = 3 - (short)(elapsedTicks / 60);
            if (remSec <= 0) {
                gConnState = CONN_READY;
                RenderCanvas();
            } else if (remSec != gFoundCountdownSec) {
                gFoundCountdownSec = remSec;
                RenderCanvas();
            }
            continue;
        }

        if (gConnState != CONN_READY) {
            continue;
        }

        /* Local 1-second timeline progression while playing and not frozen */
        if (gNowPlaying.isPlaying && !gTimelineFrozen && (nowTicks - gLastSecondTicks) >= 60) {
            gLastSecondTicks = nowTicks;
            if (gNowPlaying.progressMs + 1000 <= gNowPlaying.durationMs) {
                gNowPlaying.progressMs += 1000;
                FormatMmSs(gNowPlaying.progressMs, gNowPlaying.progressMmSs);
                if (!gSpeakerPopupOpen && !gAboutModalOpen) {
                    RenderCanvas();
                }
            }
        }

        /* Poll /api/spotify/now-playing every ~4 seconds (240 ticks) */
        if (NetHasServer() && !gAboutModalOpen && (nowTicks - gLastPollTicks) >= 240) {
            gLastPollTicks   = nowTicks;
            gLastSecondTicks = nowTicks;
            RefreshNowPlaying(true);
            RenderCanvas();
        }
    }

    AudioStreamStop();
    if (gHttpBuf) DisposePtr(gHttpBuf);
    if (gOffscreenBits) DisposePtr(gOffscreenBits);
    return 0;
}
