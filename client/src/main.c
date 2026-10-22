/*
 * MacCompanion Spotify — Native 68k Macintosh Client
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
#define MAX_DEVICES         12
#define VISIBLE_ROWS        16
#define ROW_HEIGHT          13
#define LIST_TOP_Y          35
#define LIST_BOTTOM_Y       243

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
    char kind[16]; /* "TRACK" or "ALBUM" */
    char id[64];
    char name[64];
    char artist[64];
    char durationMmSs[12];
    char uri[96];
} RightItem;

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
static Boolean         gHomeMode          = false; /* false = Playlists (note icon), true = Home (home icon) */

/* Search state */
static Boolean         gSearchFocused     = false;
static char            gSearchQuery[48]   = "";

/* Speaker Chooser Popover state */
static Boolean         gSpeakerPopupOpen  = false;
static DeviceItem      gDevices[MAX_DEVICES];
static short           gDeviceCount       = 0;

/* Manual Server IP Modal state */
static Boolean         gIpModalOpen       = false;
static char            gIpInput[32]       = "10.0.2.2";
static char            gIpModalStatus[64] = "UDP discovery timed out. Enter Server IP:";

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
static unsigned long   gLastClickTicks    = 0;
static short           gLastClickedRow    = -1;

/* Now Playing & Artwork state */
static NowPlayingState gNowPlaying;
static char            gLoadedArtTrackId[64] = "";
static unsigned char   gCurrentArtworkBits[5200];
static unsigned short  gAudioSampleRateHz = 22255;

/* Timing for periodic state & timeline updates */
static unsigned long   gLastPollTicks     = 0;
static unsigned long   gLastSecondTicks   = 0;

/* Shared scratch buffer for HTTP responses */
#define HTTP_BUF_SIZE  16384
static char           *gHttpBuf           = NULL;

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
        /* Sanitize UTF-8 smart quotes/dashes to ASCII for Monaco 9 */
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
            /* Skip continuation bytes of multi-byte UTF-8 and map common Latin-1 */
            if (c < 0x80) {
                outBuf[outIdx++] = (char)c;
            } else if ((c & 0xE0) == 0xC0 && ((unsigned char)p[1] & 0xC0) == 0x80) {
                unsigned int cp = ((c & 0x1F) << 6) | (p[1] & 0x3F);
                /* Map Swedish åäöÅÄÖé to MacRoman for authentic display! */
                if (cp == 0xE5) outBuf[outIdx++] = (char)0x8C;      /* å */
                else if (cp == 0xE4) outBuf[outIdx++] = (char)0x8A; /* ä */
                else if (cp == 0xF6) outBuf[outIdx++] = (char)0x9A; /* ö */
                else if (cp == 0xC5) outBuf[outIdx++] = (char)0x81; /* Å */
                else if (cp == 0xC4) outBuf[outIdx++] = (char)0x80; /* Ä */
                else if (cp == 0xD6) outBuf[outIdx++] = (char)0x85; /* Ö */
                else if (cp == 0xE9) outBuf[outIdx++] = (char)0x8E; /* é */
                else outBuf[outIdx++] = '?';
                p++;
            }
        }
        p++;
    }
    outBuf[outIdx] = '\0';
}

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

/* Draws a 1px vertical dotted line (matching the PSD's 50% alternating pixel lines) */
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

/* Draws the 9x9 circular slider thumb dot centered at (cx, cy) */
static void DrawSliderDot(short cx, short cy)
{
    Rect r;
    SetRect(&r, cx - 4, cy - 4, cx + 5, cy + 5);
    EraseOval(&r);
    FrameOval(&r);
    InsetRect(&r, 1, 1);
    FrameOval(&r);
}

/* ============================================================================
 * Main UI Rendering (Exact 512 x 322 PSD Layout)
 * ============================================================================ */

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

    /* Search Pill (156, 5)-(356, 17) */
    BlitIcon1Bit(gSearchPillBits, 200, 12, 156, 5, srcCopy);
    if (gSearchFocused || gSearchQuery[0] != '\0') {
        char searchDisplay[56];
        if (gSearchFocused) {
            sprintf(searchDisplay, "%s_", gSearchQuery);
        } else {
            SafeCopy(searchDisplay, gSearchQuery, sizeof(searchDisplay));
        }
        TextMode(srcBic); /* White text on black search pill */
        MoveTo(164, 14);
        DrawCStringTruncated(searchDisplay, 28);
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
        short thumbY = 35 + (short)((long)(trackH - thumbH) * gLeftScroll / maxScroll);
        if (thumbH < 20) thumbH = 20;
        if (gLeftCount <= VISIBLE_ROWS) thumbY = 64;
        SetRect(&thumbRect, 143, thumbY, 146, thumbY + thumbH);
        FillRect(&thumbRect, &qd.gray);
    }

    /* 2B. Center Column: 200x200 1-Bit Album Artwork (156, 45)-(356, 245) */
    BlitIcon1Bit(gCurrentArtworkBits, 200, 200, 156, 45, srcCopy);

    /* Artist name - Song Title centered below artwork at y = 259 */
    if (gNowPlaying.trackName[0] != '\0') {
        if (gNowPlaying.artistName[0] != '\0') {
            sprintf(titleLine, "%s - %s", gNowPlaying.artistName, gNowPlaying.trackName);
        } else {
            SafeCopy(titleLine, gNowPlaying.trackName, sizeof(titleLine));
        }
    } else if (!NetHasServer()) {
        SafeCopy(titleLine, "Not connected to MacCompanion", sizeof(titleLine));
    } else {
        SafeCopy(titleLine, "Spotify Ready - Select a Track", sizeof(titleLine));
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
        short thumbY = 35 + (short)((long)(trackH - thumbH) * gRightScroll / maxScroll);
        if (thumbH < 20) thumbH = 20;
        if (gRightCount <= VISIBLE_ROWS) thumbY = 64;
        SetRect(&thumbRect, 503, thumbY, 506, thumbY + thumbH);
        FillRect(&thumbRect, &qd.gray);
    }

    /* --------------------------------------------------------------------
     * 3. Bottom Play Bar (y = 273..322, White background)
     * -------------------------------------------------------------------- */
    SetRect(&r, 0, 273, CANVAS_W, CANVAS_H);
    EraseRect(&r);
    TextMode(srcOr);
    PenNormal();

    /* Shuffle button (195, 285) 14x11 */
    if (strcmp(gNowPlaying.shuffleMode, "smart") == 0) {
        BlitIcon1Bit(gIconShuffleSmart, 14, 11, 195, 285, srcCopy);
    } else if (strcmp(gNowPlaying.shuffleMode, "on") == 0) {
        BlitIcon1Bit(gIconShuffleOn, 14, 11, 195, 285, srcCopy);
    } else {
        BlitIcon1Bit(gIconShuffleOff, 14, 11, 195, 285, srcCopy);
    }

    /* Previous button (224, 286) 6x9 */
    BlitIcon1Bit(gIconPrev, 6, 9, 224, 286, srcCopy);

    /* Play / Pause circle (244, 278) 24x24 */
    if (gNowPlaying.isPlaying) {
        BlitIcon1Bit(gIconPauseCircle, 24, 24, 244, 278, srcCopy);
    } else {
        BlitIcon1Bit(gIconPlayCircle, 24, 24, 244, 278, srcCopy);
    }

    /* Next button (281, 286) 6x9 */
    BlitIcon1Bit(gIconNext, 6, 9, 281, 286, srcCopy);

    /* Repeat button (302, 286) 12x11 */
    if (strcmp(gNowPlaying.repeatState, "track") == 0) {
        BlitIcon1Bit(gIconRepeatTrack, 12, 11, 302, 286, srcCopy);
    } else if (strcmp(gNowPlaying.repeatState, "context") == 0) {
        BlitIcon1Bit(gIconRepeatAll, 12, 11, 302, 286, srcCopy);
    } else {
        BlitIcon1Bit(gIconRepeatOff, 12, 11, 302, 286, srcCopy);
    }

    /* Vertical dotted divider (331, 281..300) */
    DrawVerticalDottedLine(331, 281, 300, false);

    /* Speaker Icon (348, 285) 8x11 + Active Speaker Name (364, 294) */
    BlitIcon1Bit(gIconSpeaker, 8, 11, 348, 285, srcCopy);
    MoveTo(364, 294);
    if (gNowPlaying.deviceName[0] != '\0') {
        DrawCStringTruncated(gNowPlaying.deviceName, 18);
    } else {
        DrawCStringTruncated("Choose Speaker", 18);
    }

    /* Timeline Bar: elapsed (109, 312), bar (143, 308)-(369, 311), total (374, 312) */
    MoveTo(109, 312);
    DrawCStringTruncated(gNowPlaying.progressMmSs[0] ? gNowPlaying.progressMmSs : "00:00", 5);

    SetRect(&barRect, 143, 308, 369, 311);
    FrameRect(&barRect);
    {
        short fillW = 0;
        short dotX  = 143;
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
        DrawSliderDot(dotX, 309);
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
        DrawSliderDot(dotX, 309);
    }

    /* --------------------------------------------------------------------
     * 4. Speaker Chooser Popover ((330, 152)-(466, 272) + pointer triangle)
     * -------------------------------------------------------------------- */
    if (gSpeakerPopupOpen) {
        Rect popRect, shadowRect;
        short row;

        /* Subtle dithered drop shadow under popover */
        SetRect(&shadowRect, 333, 155, 468, 275);
        FillRoundRect(&shadowRect, 12, 12, &qd.gray);

        SetRect(&popRect, 330, 152, 466, 272);
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

        /* Render up to 6 devices (18px row height starting at y = 158) */
        for (row = 0; row < 6 && row < gDeviceCount; row++) {
            short yTop = 158 + row * 18;
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
     * 5. Manual Server IP Modal Dialog (when UDP discovery fails or Cmd+R)
     * -------------------------------------------------------------------- */
    if (gIpModalOpen) {
        Rect dlgRect, shadowRect, boxRect, btnConnect, btnUdp;
        char ipField[40];

        SetRect(&shadowRect, 124, 94, 394, 214);
        FillRoundRect(&shadowRect, 10, 10, &qd.gray);

        SetRect(&dlgRect, 120, 90, 390, 210);
        EraseRoundRect(&dlgRect, 10, 10);
        FrameRoundRect(&dlgRect, 10, 10);
        InsetRect(&dlgRect, 2, 2);
        FrameRoundRect(&dlgRect, 8, 8);

        TextMode(srcOr);
        MoveTo(136, 112);
        DrawCStringTruncated("MacCompanion Server Connection", 36);

        MoveTo(136, 128);
        DrawCStringTruncated(gIpModalStatus, 38);

        /* IP Input Field Box */
        SetRect(&boxRect, 136, 138, 374, 158);
        FrameRect(&boxRect);
        sprintf(ipField, "%s_", gIpInput);
        MoveTo(144, 152);
        DrawCStringTruncated(ipField, 32);

        /* Retry UDP Button */
        SetRect(&btnUdp, 136, 172, 244, 196);
        FrameRoundRect(&btnUdp, 6, 6);
        MoveTo(156, 187);
        DrawCStringTruncated("UDP Discover", 14);

        /* Connect Button */
        SetRect(&btnConnect, 260, 172, 374, 196);
        PaintRoundRect(&btnConnect, 6, 6);
        TextMode(srcBic);
        MoveTo(286, 187);
        DrawCStringTruncated("Connect (Ret)", 14);
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
            /* Check configured sample rate from /api/spotify/audio-config */
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
        /* Skip 8-byte header (width, height, rowBytes, reserved) -> 5200 bytes of 1-bit scanlines */
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

    GetPipeField(gHttpBuf, 2, gNowPlaying.trackId, sizeof(gNowPlaying.trackId));
    GetPipeField(gHttpBuf, 3, gNowPlaying.trackName, sizeof(gNowPlaying.trackName));
    GetPipeField(gHttpBuf, 4, gNowPlaying.artistName, sizeof(gNowPlaying.artistName));
    GetPipeField(gHttpBuf, 5, gNowPlaying.albumName, sizeof(gNowPlaying.albumName));

    GetPipeField(gHttpBuf, 6, field, sizeof(field));
    gNowPlaying.progressMs = atol(field);

    GetPipeField(gHttpBuf, 7, field, sizeof(field));
    gNowPlaying.durationMs = atol(field);

    GetPipeField(gHttpBuf, 8, gNowPlaying.progressMmSs, sizeof(gNowPlaying.progressMmSs));
    GetPipeField(gHttpBuf, 9, gNowPlaying.durationMmSs, sizeof(gNowPlaying.durationMmSs));

    GetPipeField(gHttpBuf, 10, field, sizeof(field));
    gNowPlaying.volumePct = (short)atoi(field);

    GetPipeField(gHttpBuf, 11, gNowPlaying.shuffleMode, sizeof(gNowPlaying.shuffleMode));
    GetPipeField(gHttpBuf, 12, gNowPlaying.repeatState, sizeof(gNowPlaying.repeatState));

    GetPipeField(gHttpBuf, 13, field, sizeof(field));
    gNowPlaying.isLiked = (field[0] == '1');

    GetPipeField(gHttpBuf, 14, gNowPlaying.deviceName, sizeof(gNowPlaying.deviceName));

    if (fetchArtIfChanged &&
        gNowPlaying.trackId[0] != '\0' &&
        strcmp(gNowPlaying.trackId, gLoadedArtTrackId) != 0) {
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
        /* Home Mode Categories */
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
        /* Playlists Mode: Row 0 = Liked Songs, Rows 1..N = User Playlists */
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
        /* Double-clicking an album loads its tracks into the right column and sets context */
        SafeCopy(gActiveContextUri, it->uri, sizeof(gActiveContextUri));
        SafeCopy(gRightHeaderLabel, "Tracks", sizeof(gRightHeaderLabel));
        sprintf(path, "/api/spotify/album/%s/tracks?limit=50", it->id);
        if (NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE) > 0) {
            ParseTracksResponseIntoRightList(gHttpBuf);
            RenderCanvas();
        }
        return;
    }

    /* Play selected track */
    UrlEncode(it->uri[0] ? it->uri : it->id, encUri, sizeof(encUri));
    if (gActiveContextUri[0] != '\0') {
        char encCtx[128];
        UrlEncode(gActiveContextUri, encCtx, sizeof(encCtx));
        sprintf(path, "/api/spotify/play?context_uri=%s&offset_uri=%s", encCtx, encUri);
    } else {
        sprintf(path, "/api/spotify/play?uri=%s", encUri);
    }

    NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
    RefreshNowPlaying(true);
    RenderCanvas();
}

static void ExecuteSearchQuery(void)
{
    char encQ[128];
    char path[192];

    if (!NetHasServer() || gSearchQuery[0] == '\0') return;
    UrlEncode(gSearchQuery, encQ, sizeof(encQ));
    sprintf(path, "/api/spotify/search?q=%s&type=track&limit=35", encQ);

    gActiveContextUri[0] = '\0';
    SafeCopy(gRightHeaderLabel, "Tracks", sizeof(gRightHeaderLabel));
    if (NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE) > 0) {
        ParseTracksResponseIntoRightList(gHttpBuf);
    }
    gSearchFocused = false;
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
    RenderCanvas();
}

static void RunInitialDiscovery(void)
{
    if (NetDiscoverServer()) {
        FormatIPAddress(NetGetServerIP(), gIpInput);
        gIpModalOpen = false;
        ConnectAndLoadAll();
    } else {
        SafeCopy(gIpModalStatus, "UDP discovery failed. Enter Server IP:", sizeof(gIpModalStatus));
        gIpModalOpen = true;
        RenderCanvas();
    }
}

/* ============================================================================
 * Mouse & Keyboard Interaction
 * ============================================================================ */

static void HandleCanvasClick(Point pt)
{
    /* 1. If Manual Server IP Modal is open, handle modal buttons */
    if (gIpModalOpen) {
        /* UDP Discover button (136, 172)-(244, 196) */
        if (pt.v >= 172 && pt.v <= 196 && pt.h >= 136 && pt.h <= 244) {
            SafeCopy(gIpModalStatus, "Searching via UDP :6801...", sizeof(gIpModalStatus));
            RenderCanvas();
            if (NetDiscoverServer()) {
                FormatIPAddress(NetGetServerIP(), gIpInput);
                gIpModalOpen = false;
                ConnectAndLoadAll();
            } else {
                SafeCopy(gIpModalStatus, "No UDP reply. Enter IP manually:", sizeof(gIpModalStatus));
                RenderCanvas();
            }
            return;
        }
        /* Connect button (260, 172)-(374, 196) */
        if (pt.v >= 172 && pt.v <= 196 && pt.h >= 260 && pt.h <= 374) {
            ip_addr parsed = 0;
            if (ParseIPAddress(gIpInput, &parsed)) {
                NetSetServer(parsed, DEFAULT_HTTP_PORT);
                gIpModalOpen = false;
                ConnectAndLoadAll();
            } else {
                SafeCopy(gIpModalStatus, "Invalid IP format (use e.g. 10.0.2.2)", sizeof(gIpModalStatus));
                RenderCanvas();
            }
            return;
        }
        return;
    }

    /* 2. If Speaker Chooser Popover is open, check if clicked inside (330, 152)-(466, 272) */
    if (gSpeakerPopupOpen) {
        if (pt.h >= 330 && pt.h <= 466 && pt.v >= 152 && pt.v <= 272) {
            short row = (pt.v - 158) / 18;
            if (row >= 0 && row < 6 && row < gDeviceCount) {
                char path[160];
                char encId[96];
                UrlEncode(gDevices[row].id, encId, sizeof(encId));
                sprintf(path, "/api/spotify/transfer?device_id=%s", encId);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                SafeCopy(gNowPlaying.deviceName, gDevices[row].name, sizeof(gNowPlaying.deviceName));
                gSpeakerPopupOpen = false;
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
            gSearchFocused = false;
            LoadLeftColumn();
            RenderCanvas();
            return;
        }
        /* Search Pill (156..356) */
        if (pt.h >= 156 && pt.h <= 356) {
            if (pt.h >= 342 && gSearchQuery[0] != '\0') {
                ExecuteSearchQuery();
            } else {
                gSearchFocused = true;
                RenderCanvas();
            }
            return;
        }
        gSearchFocused = false;
        RenderCanvas();
        return;
    }

    gSearchFocused = false;

    /* 4. Middle Content Area (y = 22..272) */
    if (pt.v >= 22 && pt.v < 273) {
        /* Left Scrollbar (x = 139..150) */
        if (pt.h >= 139 && pt.h <= 150 && gLeftCount > VISIBLE_ROWS) {
            short maxScroll = gLeftCount - VISIBLE_ROWS;
            short newScroll = (short)((long)(pt.v - 35) * maxScroll / 200);
            if (newScroll < 0) newScroll = 0;
            if (newScroll > maxScroll) newScroll = maxScroll;
            gLeftScroll = newScroll;
            RenderCanvas();
            return;
        }
        /* Left Column Items (x = 8..138, y = 35..243) */
        if (pt.h >= 8 && pt.h <= 138 && pt.v >= LIST_TOP_Y && pt.v < LIST_BOTTOM_Y) {
            short clickedRow = gLeftScroll + (pt.v - LIST_TOP_Y) / ROW_HEIGHT;
            if (clickedRow >= 0 && clickedRow < gLeftCount) {
                gLeftSelected = clickedRow;
                LoadSelectedLeftItemIntoRightList();
                RenderCanvas();
            }
            return;
        }
        /* Right Scrollbar (x = 499..511) */
        if (pt.h >= 499 && pt.h <= 511 && gRightCount > VISIBLE_ROWS) {
            short maxScroll = gRightCount - VISIBLE_ROWS;
            short newScroll = (short)((long)(pt.v - 35) * maxScroll / 200);
            if (newScroll < 0) newScroll = 0;
            if (newScroll > maxScroll) newScroll = maxScroll;
            gRightScroll = newScroll;
            RenderCanvas();
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
            /* Shuffle (192..212) */
            if (pt.h >= 192 && pt.h <= 212) {
                NetHttpGet("/api/spotify/shuffle?mode=cycle", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
                return;
            }
            /* Previous (220..235) */
            if (pt.h >= 220 && pt.h <= 235) {
                NetHttpGet("/api/spotify/previous", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(true);
                RenderCanvas();
                return;
            }
            /* Play / Pause Circle (242..270) */
            if (pt.h >= 242 && pt.h <= 270) {
                NetHttpGet("/api/spotify/toggle-play", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
                return;
            }
            /* Next (278..293) */
            if (pt.h >= 278 && pt.h <= 293) {
                NetHttpGet("/api/spotify/next", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(true);
                RenderCanvas();
                return;
            }
            /* Repeat (299..318) */
            if (pt.h >= 299 && pt.h <= 318) {
                NetHttpGet("/api/spotify/repeat?state=cycle", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
                return;
            }
            /* Speaker Selector (344..465) */
            if (pt.h >= 344 && pt.h <= 465) {
                LoadDevicesAndTogglePopover();
                return;
            }
        }

        /* Timeline & Volume Row (y = 303..320) */
        if (pt.v >= 303 && pt.v <= 320) {
            /* Timeline Scrubber (143..369) */
            if (pt.h >= 143 && pt.h <= 369) {
                char path[96];
                short pct = (short)(((long)(pt.h - 143) * 100L) / 226L);
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                sprintf(path, "/api/spotify/seek?percent=%d", pct);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
                return;
            }
            /* Volume Scrubber (438..493) */
            if (pt.h >= 438 && pt.h <= 493) {
                char path[96];
                short vol = (short)(((long)(pt.h - 438) * 100L) / 55L);
                if (vol < 0) vol = 0;
                if (vol > 100) vol = 100;
                gNowPlaying.volumePct = vol;
                sprintf(path, "/api/spotify/volume?volume_percent=%d", vol);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                RenderCanvas();
                return;
            }
        }
    }
}

static void HandleKeyPress(char ch)
{
    /* 1. Manual Server IP Modal Typing */
    if (gIpModalOpen) {
        short len = (short)strlen(gIpInput);
        if (ch == '\r' || ch == 3) { /* Return or Enter */
            ip_addr parsed = 0;
            if (ParseIPAddress(gIpInput, &parsed)) {
                NetSetServer(parsed, DEFAULT_HTTP_PORT);
                gIpModalOpen = false;
                ConnectAndLoadAll();
            } else {
                SafeCopy(gIpModalStatus, "Invalid IP format (use e.g. 10.0.2.2)", sizeof(gIpModalStatus));
                RenderCanvas();
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
            gSearchFocused = false;
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
    if (ch == ' ') {
        NetHttpGet("/api/spotify/toggle-play", gHttpBuf, HTTP_BUF_SIZE);
        RefreshNowPlaying(false);
        RenderCanvas();
    } else if (ch == '\r' || ch == 3) {
        ActivateSelectedRightItem();
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
                SysBeep(10);
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
                SafeCopy(gIpModalStatus, "Enter MacCompanion Server IP:", sizeof(gIpModalStatus));
                gIpModalOpen = true;
                RenderCanvas();
            } else if (itemID == iFindSearch) {
                gSearchFocused = true;
                RenderCanvas();
            } else if (itemID == iQuit) {
                gRunning = false;
            }
            break;

        case mPlayback:
            if (itemID == iPlayPause) {
                NetHttpGet("/api/spotify/toggle-play", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(false);
                RenderCanvas();
            } else if (itemID == iNextTrack) {
                NetHttpGet("/api/spotify/next", gHttpBuf, HTTP_BUF_SIZE);
                RefreshNowPlaying(true);
                RenderCanvas();
            } else if (itemID == iPrevTrack) {
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
                /* Allow Cmd-dragging the borderless window on large screens */
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

    gHttpBuf = NewPtrClear(HTTP_BUF_SIZE);
    memcpy(gCurrentArtworkBits, gDefaultArtworkBits, 5200);
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
        "\pMacCompanion Spotify",
        true,
        plainDBox,
        (WindowPtr)-1L,
        false,
        0
    );

    RenderCanvas();

    /* Perform automatic UDP discovery (:6801) on launch; fallback to Manual IP Modal */
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

        if (WaitNextEvent(everyEvent, &ev, AudioStreamIsRunning() ? 1 : 4, NULL)) {
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
                    BeginUpdate((WindowPtr)ev.message);
                    if ((WindowPtr)ev.message == gMainWindow) {
                        RenderCanvas();
                    }
                    EndUpdate((WindowPtr)ev.message);
                    break;
            }
        }

        nowTicks = TickCount();

        /* Advance local timeline smoothly every 1 second while playing */
        if (gNowPlaying.isPlaying && !gIpModalOpen && (nowTicks - gLastSecondTicks) >= 60) {
            gLastSecondTicks = nowTicks;
            if (gNowPlaying.durationMs > 0 && gNowPlaying.progressMs + 1000 <= gNowPlaying.durationMs) {
                gNowPlaying.progressMs += 1000;
                FormatMmSs(gNowPlaying.progressMs, gNowPlaying.progressMmSs);
                RenderCanvas();
            }
        }

        /* Poll server for Now Playing changes every ~4 seconds when idle or ~6 seconds during audio */
        if (NetHasServer() && !gIpModalOpen &&
            (nowTicks - gLastPollTicks) >= (AudioStreamIsRunning() ? 360 : 240)) {
            gLastPollTicks   = nowTicks;
            gLastSecondTicks = nowTicks;
            RefreshNowPlaying(true);
            RenderCanvas();
        }
    }

    AudioStreamStop();
    if (gMainWindow != NULL) {
        DisposeWindow(gMainWindow);
    }
    return 0;
}
