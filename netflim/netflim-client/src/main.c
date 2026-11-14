#include <Types.h>
#include <Memory.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Menus.h>
#include <Windows.h>
#include <Dialogs.h>
#include <Devices.h>
#include <ToolUtils.h>
#include <Traps.h>
#include <OSUtils.h>
#include <Sound.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "version.h"
#include "codec.h"
#include "net.h"
#include "player.h"

/* ============================================================================
 * Window & UI Constants (Fits 512x342 Compact Mac SE / SE/30 Screen Cleanly)
 * ============================================================================ */

static short            gWinW             = 512;
static short            gWinH             = 304;
static Boolean          gCompactScreen    = false;
static short            gCardLeftMargin   = 8;
static short            gCardTopMargin    = 29;

#define WIN_W               gWinW
#define WIN_H               gWinH

#define TOP_BAR_H           26
#define BOT_BAR_H           22
#define GALLERY_TOP         TOP_BAR_H
#define GALLERY_BOTTOM      (WIN_H - BOT_BAR_H)

#define PAGE_COLS           3
#define PAGE_ROWS           2
#define ITEMS_PER_PAGE      (PAGE_COLS * PAGE_ROWS)

#define CARD_W              156
#define CARD_H              120
#define CARD_GAP_X          8
#define CARD_GAP_Y          6
#define CARD_LEFT_MARGIN    gCardLeftMargin
#define CARD_TOP_MARGIN     gCardTopMargin

/* Resource & Menu IDs */
#define mApple              128
#define iAbout              1

#define mFile               129
#define iRefreshCatalog     1
#define iSetServerIP        2
#define iStopPlayback       4
#define iQuit               6

#define mPlayback           130
#define iPlayPause          1
#define iFullscreen         2
#define iLoopVideo          4
#define iMuteAudio          5
#define iNextPage           7
#define iPrevPage           8

#define mProfile            131
#define iProfileAuto        1
#define iProfileSE30        2
#define iProfileSE          3
#define iProfilePlus        4

#define rSetServerDLOG      128
#define rMessageALRT        129

/* ============================================================================
 * Application State
 * ============================================================================ */

static Boolean          gRunning          = true;
static Boolean          gHasWNE           = true;
static WindowPtr        gMainWindow       = NULL;
static Boolean          gMacTcpOk         = false;
static Boolean          gServerConnected  = false;
static char             gGalleryStatus[96] = "Initializing MacTCP...";

static NetFlimVideoItem gVideos[NF_MAX_VIDEOS];
static short            gVideoCount       = 0;
static short            gPageIdx          = 0;
static short            gSelectedIdx      = 0;

/* Cached 128x86 1-bit posters for the currently visible 6 cards (8.2 KB total) */
static unsigned char    gPagePosters[ITEMS_PER_PAGE][NF_POSTER_BYTES];
static Boolean          gPagePosterValid[ITEMS_PER_PAGE];
static char             gPagePosterId[ITEMS_PER_PAGE][18];

static unsigned long    gLastClickTick    = 0;
static short            gLastClickIdx     = -1;

/* Clickable Toolbar / Gallery / Player Hit Rects */
static Rect             gViewportRect;
static Rect             gBtnRefreshRect;
static Rect             gBtnServerRect;
static Rect             gBtnPrevRect;
static Rect             gBtnNextRect;
static Rect             gBtnPlayGalleryRect;

static Rect             gBtnStopRect;
static Rect             gBtnPauseRect;
static Rect             gBtnFullRect;
static Rect             gBtnMuteRect;
static Rect             gBtnLoopRect;

/* ============================================================================
 * Helpers
 * ============================================================================ */

static void CopyCToPStr(const char *cstr, Str255 pstr) {
    size_t len = cstr ? strlen(cstr) : 0;
    if (len > 255) len = 255;
    pstr[0] = (unsigned char)len;
    if (len > 0) memcpy(pstr + 1, cstr, len);
}

static void CopyPToCStr(ConstStr255Param pstr, char *cstr, int maxLen) {
    int len = pstr ? (int)pstr[0] : 0;
    if (len >= maxLen) len = maxLen - 1;
    if (len > 0) memcpy(cstr, pstr + 1, len);
    cstr[len] = '\0';
}

static void ShowMessageAlert(const char *line0, const char *line1, const char *line2) {
    Str255 p0, p1, p2;
    CopyCToPStr(line0 ? line0 : "", p0);
    CopyCToPStr(line1 ? line1 : "", p1);
    CopyCToPStr(line2 ? line2 : "", p2);
    ParamText(p0, p1, p2, "\p");
    Alert(rMessageALRT, NULL);
}

static void DrawTruncatedCText(const char *text, short x, short y, short maxW) {
    Str255 p;
    char buf[96];
    int len;

    if (!text) return;
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    len = (int)strlen(buf);

    CopyCToPStr(buf, p);
    if (StringWidth(p) <= maxW) {
        MoveTo(x, y);
        DrawString(p);
        return;
    }

    while (len > 1) {
        char tmp[96];
        len--;
        memcpy(tmp, buf, len);
        tmp[len] = '\0';
        strcat(tmp, "...");
        CopyCToPStr(tmp, p);
        if (StringWidth(p) <= maxW || len <= 2) {
            MoveTo(x, y);
            DrawString(p);
            return;
        }
    }
}

static void FormatDuration(unsigned long sec, char *out) {
    unsigned long m = sec / 60UL;
    unsigned long s = sec % 60UL;
    sprintf(out, "%lu:%02lu", m, s);
}

static void DrawPixelButton(const Rect *r, const char *label, Boolean inverted, Boolean disabled) {
    Str255 p;
    short tw, tx, ty;
    Pattern dkGray;

    EraseRect(r);
    if (inverted) {
        PaintRect(r);
        TextMode(srcBic);
    } else {
        FrameRect(r);
        TextMode(srcOr);
    }

    if (disabled && !inverted) {
        GetIndPattern(&dkGray, sysPatListID, 4);
        PenPat(&dkGray);
        FrameRect(r);
        PenNormal();
    }

    TextFont(systemFont);
    TextSize(10);
    TextFace(normal);
    CopyCToPStr(label, p);
    tw = StringWidth(p);
    tx = r->left + ((r->right - r->left) - tw) / 2;
    ty = r->top + ((r->bottom - r->top) + 8) / 2;
    MoveTo(tx, ty);
    DrawString(p);

    TextMode(srcOr);
    PenNormal();
    TextFont(0);
    TextSize(12);
}

static void UpdateHitRects(void) {
    SetRect(&gViewportRect, 0, TOP_BAR_H, WIN_W, WIN_H - BOT_BAR_H);

    /* Gallery top-right buttons */
    SetRect(&gBtnRefreshRect, WIN_W - 152, 4, WIN_W - 84, 22);
    SetRect(&gBtnServerRect,  WIN_W - 80,  4, WIN_W - 6,  22);

    /* Gallery bottom-right buttons */
    SetRect(&gBtnPrevRect,        WIN_W - 186, WIN_H - 19, WIN_W - 162, WIN_H - 3);
    SetRect(&gBtnNextRect,        WIN_W - 106, WIN_H - 19, WIN_W - 82,  WIN_H - 3);
    SetRect(&gBtnPlayGalleryRect, WIN_W - 76,  WIN_H - 19, WIN_W - 6,   WIN_H - 3);

    /* Player top bar transport buttons */
    SetRect(&gBtnStopRect,  WIN_W - 294, 4, WIN_W - 234, 22);
    SetRect(&gBtnPauseRect, WIN_W - 230, 4, WIN_W - 172, 22);
    SetRect(&gBtnFullRect,  WIN_W - 168, 4, WIN_W - 118, 22);
    SetRect(&gBtnMuteRect,  WIN_W - 114, 4, WIN_W - 62,  22);
    SetRect(&gBtnLoopRect,  WIN_W - 58,  4, WIN_W - 6,   22);
}

static void SyncMenuState(void) {
    MenuHandle fileM  = GetMenuHandle(mFile);
    MenuHandle playM  = GetMenuHandle(mPlayback);
    MenuHandle profM  = GetMenuHandle(mProfile);
    PlayerState pst   = PlayerGetState();
    Boolean active    = (pst == kPlayerBuffering || pst == kPlayerPlaying || pst == kPlayerPaused);
    NetFlimProfileMode pm = NetGetProfileMode();

    if (fileM) {
        if (active) EnableItem(fileM, iStopPlayback);
        else        DisableItem(fileM, iStopPlayback);
    }

    if (playM) {
        if (active || gVideoCount > 0) EnableItem(playM, iPlayPause);
        else                           DisableItem(playM, iPlayPause);

        if (active) EnableItem(playM, iFullscreen);
        else        DisableItem(playM, iFullscreen);

        CheckItem(playM, iFullscreen, PlayerIsFullscreen());
        CheckItem(playM, iLoopVideo, PlayerIsLooping());
        CheckItem(playM, iMuteAudio, PlayerIsMuted());

        if (!active && gVideoCount > ITEMS_PER_PAGE) {
            EnableItem(playM, iNextPage);
            EnableItem(playM, iPrevPage);
        } else {
            DisableItem(playM, iNextPage);
            DisableItem(playM, iPrevPage);
        }
    }

    if (profM) {
        CheckItem(profM, iProfileAuto, (pm == kProfileAuto));
        CheckItem(profM, iProfileSE30, (pm == kProfileSE30));
        CheckItem(profM, iProfileSE,   (pm == kProfileSE));
        CheckItem(profM, iProfilePlus, (pm == kProfilePlus));
    }
}

/* ============================================================================
 * Gallery Poster Cache & Catalog Operations
 * ============================================================================ */

static void ClearPagePosterCache(void) {
    short i;
    for (i = 0; i < ITEMS_PER_PAGE; i++) {
        gPagePosterValid[i] = false;
        gPagePosterId[i][0] = '\0';
    }
}

static void FetchPagePosters(void) {
    short startIdx = gPageIdx * ITEMS_PER_PAGE;
    short slot;

    for (slot = 0; slot < ITEMS_PER_PAGE; slot++) {
        short vidIdx = startIdx + slot;
        if (vidIdx >= gVideoCount) {
            gPagePosterValid[slot] = false;
            gPagePosterId[slot][0] = '\0';
            continue;
        }
        if (!gVideos[vidIdx].hasPoster) {
            gPagePosterValid[slot] = false;
            strcpy(gPagePosterId[slot], gVideos[vidIdx].id);
            continue;
        }
        if (gPagePosterValid[slot] && strcmp(gPagePosterId[slot], gVideos[vidIdx].id) == 0) {
            continue;
        }
        gPagePosterValid[slot] = NetFetchPosterRaw(gVideos[vidIdx].id, gPagePosters[slot]);
        strcpy(gPagePosterId[slot], gVideos[vidIdx].id);
    }
}

static short GetTotalPages(void) {
    if (gVideoCount <= 0) return 1;
    return (short)((gVideoCount + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE);
}

static void InvalidateWindow(void) {
    if (gMainWindow && !PlayerIsFullscreen()) {
        GrafPtr savedPort;
        GetPort(&savedPort);
        SetPort(gMainWindow);
        InvalRect(&gMainWindow->portRect);
        SetPort(savedPort);
    }
}

static void DrawMainWindow(void);

static void RefreshCatalog(void) {
    char errBuf[128];
    char statusBuf[128];
    char ipBuf[32];
    short count;

    if (!gMacTcpOk) {
        strcpy(gGalleryStatus, "MacTCP not available");
        InvalidateWindow();
        return;
    }

    if (!gServerConnected || !NetHasServer()) {
        strcpy(gGalleryStatus, "Checking prefs & scanning LAN...");
        if (gMainWindow && !PlayerIsFullscreen()) {
            DrawMainWindow();
        }
        if (!NetAutoConnect(statusBuf, sizeof(statusBuf))) {
            const char *note = NetGetLastErrorNote();
            gServerConnected = false;
            gVideoCount = 0;
            if (note && note[0] != '\0') {
                strncpy(gGalleryStatus, note, sizeof(gGalleryStatus) - 1);
                gGalleryStatus[sizeof(gGalleryStatus) - 1] = '\0';
            } else {
                strcpy(gGalleryStatus, "No NetFlim server found - click Server...");
            }
            SyncMenuState();
            InvalidateWindow();
            return;
        }
    }

    FormatIPAddress(NetGetServerIP(), ipBuf);
    sprintf(gGalleryStatus, "Loading library from %s...", ipBuf);
    if (gMainWindow && !PlayerIsFullscreen()) {
        DrawMainWindow();
    }

    count = NetFetchCatalog(gVideos, NF_MAX_VIDEOS, errBuf, sizeof(errBuf));
    if (count < 0) {
        /* Retry auto-connect once in case server IP changed or sibling prefs has working server */
        if (NetAutoConnect(statusBuf, sizeof(statusBuf))) {
            FormatIPAddress(NetGetServerIP(), ipBuf);
            sprintf(gGalleryStatus, "Loading library from %s...", ipBuf);
            count = NetFetchCatalog(gVideos, NF_MAX_VIDEOS, errBuf, sizeof(errBuf));
        }
    }

    if (count < 0) {
        const char *note = NetGetLastErrorNote();
        gServerConnected = false;
        gVideoCount = 0;
        if (note && note[0] != '\0') {
            strncpy(gGalleryStatus, note, sizeof(gGalleryStatus) - 1);
            gGalleryStatus[sizeof(gGalleryStatus) - 1] = '\0';
        } else {
            strncpy(gGalleryStatus, errBuf, sizeof(gGalleryStatus) - 1);
            gGalleryStatus[sizeof(gGalleryStatus) - 1] = '\0';
        }
    } else {
        short totalPages;
        gServerConnected = true;
        gVideoCount = count;
        totalPages = GetTotalPages();
        if (gPageIdx >= totalPages) gPageIdx = 0;
        if (gSelectedIdx >= gVideoCount) gSelectedIdx = (gVideoCount > 0) ? 0 : -1;
        if (gVideoCount > 0 && gSelectedIdx < 0) gSelectedIdx = 0;
        ClearPagePosterCache();
        FetchPagePosters();
        sprintf(gGalleryStatus, "%d video%s ready (%s)",
                gVideoCount, (gVideoCount == 1) ? "" : "s", NetResolveProfileName());
    }

    SyncMenuState();
    InvalidateWindow();
}

/* ============================================================================
 * Drawing: Gallery Mode & Player HUD
 * ============================================================================ */

static void DrawHeaderBar(Boolean inPlayer, const PlayerSnapshot *snap) {
    Rect r, sep;
    Str255 p;
    char ipStr[32];
    char headerTxt[128];

    SetRect(&r, 0, 0, WIN_W, TOP_BAR_H);
    EraseRect(&r);

    TextFont(systemFont);
    TextSize(12);
    TextFace(bold);
    MoveTo(8, 17);
    DrawString("\pNetFlim");
    TextFace(normal);

    TextFont(applFont);
    TextSize(9);

    if (inPlayer && snap) {
        char durCur[16], durTot[16];
        FormatDuration(snap->elapsedTicks / 60UL, durCur);
        FormatDuration(snap->totalTicks / 60UL, durTot);
        sprintf(headerTxt, "%s  (%s/%s)", snap->videoTitle, durCur, durTot);
        DrawTruncatedCText(headerTxt, 68, 16, 134);

        DrawPixelButton(&gBtnStopRect,  "Gallery", false, false);
        DrawPixelButton(&gBtnPauseRect, (snap->state == kPlayerPaused) ? "Resume" : "Pause",
                        (snap->state == kPlayerPaused), false);
        DrawPixelButton(&gBtnFullRect,  "Full", false, false);
        DrawPixelButton(&gBtnMuteRect,  snap->muted ? "Muted" : "Sound", snap->muted, false);
        DrawPixelButton(&gBtnLoopRect,  "Loop", snap->looping, false);
    } else {
        if (NetHasServer()) {
            FormatIPAddress(NetGetServerIP(), ipStr);
            sprintf(headerTxt, "Server %s:%u  [%s]",
                    ipStr, (unsigned int)NetGetServerPort(),
                    gServerConnected ? "Online" : "Offline");
        } else {
            strcpy(headerTxt, "No Server Connected");
        }
        CopyCToPStr(headerTxt, p);
        MoveTo(72, 16);
        DrawString(p);

        DrawPixelButton(&gBtnRefreshRect, "Refresh", false, false);
        DrawPixelButton(&gBtnServerRect,  "Server...", false, false);
    }

    SetRect(&sep, 0, TOP_BAR_H - 1, WIN_W, TOP_BAR_H);
    PaintRect(&sep);

    TextFont(0);
    TextSize(12);
}

static void DrawFooterBar(Boolean inPlayer, const PlayerSnapshot *snap) {
    Rect r, sep;
    char info[192];

    SetRect(&sep, 0, WIN_H - BOT_BAR_H, WIN_W, WIN_H - BOT_BAR_H + 1);
    PaintRect(&sep);

    TextFont(applFont);
    TextSize(9);

    if (inPlayer && snap) {
        Rect barOuter, barFill, barEmpty;
        short fillW = 0;

        SetRect(&r, 0, WIN_H - BOT_BAR_H + 1, WIN_W - 136, WIN_H);
        EraseRect(&r);

        sprintf(info, "%s | F:%lu/%lu | %s | Buf:%d/%d",
                snap->statusMsg,
                snap->currentFrame,
                snap->totalFrames,
                snap->activeProfile,
                snap->bufferedReady,
                snap->numSlots);
        DrawTruncatedCText(info, 8, WIN_H - 7, WIN_W - 148);

        SetRect(&barOuter, WIN_W - 134, WIN_H - 14, WIN_W - 8, WIN_H - 6);
        FrameRect(&barOuter);
        if (snap->totalFrames > 0 && snap->currentFrame > 0) {
            fillW = (short)(((snap->currentFrame) * (unsigned long)(barOuter.right - barOuter.left - 2)) / snap->totalFrames);
            if (fillW > barOuter.right - barOuter.left - 2) fillW = barOuter.right - barOuter.left - 2;
            if (fillW > 0) {
                SetRect(&barFill, barOuter.left + 1, barOuter.top + 1, barOuter.left + 1 + fillW, barOuter.bottom - 1);
                PaintRect(&barFill);
            }
        }
        if (barOuter.left + 1 + fillW < barOuter.right - 1) {
            SetRect(&barEmpty, barOuter.left + 1 + fillW, barOuter.top + 1, barOuter.right - 1, barOuter.bottom - 1);
            EraseRect(&barEmpty);
        }
    } else {
        short totalPages = GetTotalPages();
        char pageStr[24];
        Str255 p;
        Boolean canPlay = (gVideoCount > 0 && gSelectedIdx >= 0 && gSelectedIdx < gVideoCount);

        SetRect(&r, 0, WIN_H - BOT_BAR_H + 1, WIN_W, WIN_H);
        EraseRect(&r);

        DrawTruncatedCText(gGalleryStatus, 8, WIN_H - 7, WIN_W - 202);

        DrawPixelButton(&gBtnPrevRect, "<", false, (gPageIdx <= 0));
        sprintf(pageStr, "%d / %d", gPageIdx + 1, totalPages);
        CopyCToPStr(pageStr, p);
        MoveTo(WIN_W - 156 + (46 - StringWidth(p)) / 2, WIN_H - 7);
        DrawString(p);
        DrawPixelButton(&gBtnNextRect, ">", false, (gPageIdx + 1 >= totalPages));
        DrawPixelButton(&gBtnPlayGalleryRect, "Play", canPlay, !canPlay);
    }

    TextFont(0);
    TextSize(12);
}

static void GetCardRect(short slot, Rect *outR) {
    short col = slot % PAGE_COLS;
    short row = slot / PAGE_COLS;
    short x = CARD_LEFT_MARGIN + col * (CARD_W + CARD_GAP_X);
    short y = CARD_TOP_MARGIN  + row * (CARD_H + CARD_GAP_Y);
    SetRect(outR, x, y, x + CARD_W, y + CARD_H);
}

static void DrawGalleryCard(short slot, short vidIdx) {
    Rect cardR, posterR, selR;
    const NetFlimVideoItem *it = &gVideos[vidIdx];
    Boolean selected = (vidIdx == gSelectedIdx);
    char durStr[16];
    char metaStr[48];
    short px, py;

    GetCardRect(slot, &cardR);
    EraseRect(&cardR);

    FrameRect(&cardR);
    if (selected) {
        selR = cardR;
        InsetRect(&selR, 1, 1);
        FrameRect(&selR);
    }

    px = cardR.left + (CARD_W - NF_POSTER_W) / 2;
    py = cardR.top + 5;
    SetRect(&posterR, px, py, px + NF_POSTER_W, py + NF_POSTER_H);

    if (gPagePosterValid[slot]) {
        BitMap bm;
        Rect srcR;
        bm.baseAddr = (Ptr)gPagePosters[slot];
        bm.rowBytes = NF_POSTER_ROWBYTES;
        SetRect(&bm.bounds, 0, 0, NF_POSTER_W, NF_POSTER_H);
        SetRect(&srcR, 0, 0, NF_POSTER_W, NF_POSTER_H);
        ForeColor(blackColor);
        BackColor(whiteColor);
        CopyBits(&bm, &gMainWindow->portBits, &srcR, &posterR, srcCopy, NULL);
        FrameRect(&posterR);
    } else {
        Pattern ltGray;
        GetIndPattern(&ltGray, sysPatListID, 22);
        FillRect(&posterR, &ltGray);
        FrameRect(&posterR);
        TextFont(applFont);
        TextSize(9);
        MoveTo(px + 38, py + 46);
        DrawString("\pNo Poster");
    }

    TextFont(applFont);
    TextSize(9);
    TextFace(selected ? bold : normal);
    DrawTruncatedCText(it->title, cardR.left + 6, posterR.bottom + 12, CARD_W - 12);

    TextFace(normal);
    FormatDuration(it->durationSec, durStr);
    sprintf(metaStr, "%s  [%s]%s",
            durStr,
            it->profilesCsv[0] ? it->profilesCsv : "-",
            it->silent ? " Mute" : "");
    DrawTruncatedCText(metaStr, cardR.left + 6, posterR.bottom + 23, CARD_W - 12);

    if (selected) {
        Rect badgeR;
        SetRect(&badgeR, cardR.right - 2, cardR.top + 2, cardR.right - 1, cardR.bottom - 2);
        /* Crisp inner bottom accent bar for instant active focus clarity */
        SetRect(&badgeR, cardR.left + 2, cardR.bottom - 3, cardR.right - 2, cardR.bottom - 1);
        PaintRect(&badgeR);
    }

    TextFont(0);
    TextSize(12);
}

static void DrawGalleryBody(void) {
    Rect bodyR;
    Pattern pat;

    SetRect(&bodyR, 0, GALLERY_TOP, WIN_W, GALLERY_BOTTOM);
    GetIndPattern(&pat, sysPatListID, 23);
    FillRect(&bodyR, &pat);

    if (gVideoCount <= 0) {
        Rect boxR;
        short boxTop = GALLERY_TOP + ((GALLERY_BOTTOM - GALLERY_TOP) - 128) / 2;
        if (boxTop < GALLERY_TOP + 12) boxTop = GALLERY_TOP + 12;
        SetRect(&boxR, 64, boxTop, WIN_W - 64, boxTop + 128);
        EraseRect(&boxR);
        FrameRect(&boxR);
        InsetRect(&boxR, 2, 2);
        FrameRect(&boxR);

        TextFont(systemFont);
        TextSize(12);
        TextFace(bold);
        MoveTo((WIN_W - StringWidth(gServerConnected ? "\pNetFlim Video Library" : "\pNetFlim Server Setup")) / 2, boxTop + 38);
        DrawString(gServerConnected ? "\pNetFlim Video Library" : "\pNetFlim Server Setup");
        TextFace(normal);
        TextFont(applFont);
        TextSize(9);
        DrawTruncatedCText(gGalleryStatus, 82, boxTop + 64, WIN_W - 164);
        if (gServerConnected) {
            DrawTruncatedCText("Upload & encode videos at http://<server>:6800/netflim.html", 82, boxTop + 88, WIN_W - 164);
            DrawTruncatedCText("Press Cmd+R to refresh or Cmd+I for Server Settings.", 82, boxTop + 104, WIN_W - 164);
        } else {
            DrawTruncatedCText("Auto-checks NetFlim, SpotiMac, MacAI & MacDrop server prefs.", 82, boxTop + 88, WIN_W - 164);
            DrawTruncatedCText("Click 'Server...' (Cmd+I) to Scan LAN or enter Server IP.", 82, boxTop + 104, WIN_W - 164);
        }
        TextFont(0);
        TextSize(12);
    } else {
        short startIdx = gPageIdx * ITEMS_PER_PAGE;
        short slot;
        for (slot = 0; slot < ITEMS_PER_PAGE; slot++) {
            short idx = startIdx + slot;
            if (idx < gVideoCount) {
                DrawGalleryCard(slot, idx);
            }
        }
    }
}

static PlayerState gLastHdrState   = (PlayerState)-1;
static Boolean     gLastHdrMuted   = false;
static Boolean     gLastHdrLooping = false;
static char        gLastHdrTitle[64];

static void DrawMainWindow(void) {
    PlayerSnapshot snap;
    Boolean inPlayer;

    if (!gMainWindow || PlayerIsFullscreen()) return;

    SetPort(gMainWindow);
    ForeColor(blackColor);
    BackColor(whiteColor);

    PlayerGetSnapshot(&snap);
    inPlayer = (snap.state == kPlayerBuffering || snap.state == kPlayerPlaying || snap.state == kPlayerPaused);

    DrawHeaderBar(inPlayer, &snap);
    gLastHdrState = snap.state;
    gLastHdrMuted = snap.muted;
    gLastHdrLooping = snap.looping;
    strncpy(gLastHdrTitle, snap.videoTitle, sizeof(gLastHdrTitle) - 1);
    gLastHdrTitle[sizeof(gLastHdrTitle) - 1] = '\0';

    if (inPlayer) {
        PlayerDrawViewport(gMainWindow, &gViewportRect);
    } else {
        DrawGalleryBody();
    }
    DrawFooterBar(inPlayer, &snap);
}

static void RedrawPlayerChromeOnly(void) {
    PlayerSnapshot snap;
    if (!gMainWindow || PlayerIsFullscreen()) return;
    SetPort(gMainWindow);
    ForeColor(blackColor);
    BackColor(whiteColor);
    PlayerGetSnapshot(&snap);
    if (snap.state != gLastHdrState ||
        snap.muted != gLastHdrMuted ||
        snap.looping != gLastHdrLooping ||
        strncmp(snap.videoTitle, gLastHdrTitle, sizeof(gLastHdrTitle) - 1) != 0) {
        DrawHeaderBar(true, &snap);
        gLastHdrState = snap.state;
        gLastHdrMuted = snap.muted;
        gLastHdrLooping = snap.looping;
        strncpy(gLastHdrTitle, snap.videoTitle, sizeof(gLastHdrTitle) - 1);
        gLastHdrTitle[sizeof(gLastHdrTitle) - 1] = '\0';
    }
    DrawFooterBar(true, &snap);
}

/* ============================================================================
 * Dialogs & User Actions
 * ============================================================================ */

static void OutlineDialogDefaultButton(DialogPtr dlg) {
    short iType;
    Handle iHdl;
    Rect iRect;
    GrafPtr savedPort;

    GetPort(&savedPort);
    SetPort(dlg);
    GetDialogItem(dlg, 1, &iType, &iHdl, &iRect);
    PenSize(3, 3);
    InsetRect(&iRect, -4, -4);
    FrameRoundRect(&iRect, 16, 16);
    PenNormal();
    SetPort(savedPort);
}

static void SetServerDialogStatus(DialogPtr dlg, Handle statusHdl, const char *msg) {
    Str255 pText;
    GrafPtr savedPort;

    GetPort(&savedPort);
    SetPort(dlg);
    CopyCToPStr(msg, pText);
    SetDialogItemText(statusHdl, pText);
    DrawDialog(dlg);
    OutlineDialogDefaultButton(dlg);
    SetPort(savedPort);
}

static void OpenSetServerDialog(void) {
    DialogPtr dlg;
    short itemHit = 0;
    short iType;
    Handle ipHdl, portHdl, statusHdl;
    Rect iRect;
    Str255 pIp, pPort, pStatus;
    char ipBuf[32], portBuf[16], statusMsg[128];
    Boolean connected = false;

    dlg = GetNewDialog(rSetServerDLOG, NULL, (WindowPtr)-1L);
    if (!dlg) return;

    if (NetHasServer()) {
        FormatIPAddress(NetGetServerIP(), ipBuf);
    } else {
        ip_addr sibIp = 0;
        unsigned short sibPort = DEFAULT_HTTP_PORT;
        if (NetReadSiblingServerIP(&sibIp, &sibPort, NULL, 0)) {
            FormatIPAddress(sibIp, ipBuf);
        } else {
            strcpy(ipBuf, "10.0.2.2");
        }
    }
    sprintf(portBuf, "%u", (unsigned int)NetGetServerPort());

    GetDialogItem(dlg, 5, &iType, &ipHdl, &iRect);
    CopyCToPStr(ipBuf, pIp);
    SetDialogItemText(ipHdl, pIp);

    GetDialogItem(dlg, 6, &iType, &portHdl, &iRect);
    CopyCToPStr(portBuf, pPort);
    SetDialogItemText(portHdl, pPort);

    GetDialogItem(dlg, 7, &iType, &statusHdl, &iRect);
    if (gServerConnected && NetHasServer()) {
        const char *src = NetGetPrefsSourceName();
        char curIp[32];
        FormatIPAddress(NetGetServerIP(), curIp);
        if (src && src[0] != '\0') {
            snprintf(statusMsg, sizeof(statusMsg), "Connected to %s (%s)", curIp, src);
        } else {
            snprintf(statusMsg, sizeof(statusMsg), "Connected to %s", curIp);
        }
    } else if (NetGetLastErrorNote()[0] != '\0') {
        strncpy(statusMsg, NetGetLastErrorNote(), sizeof(statusMsg) - 1);
        statusMsg[sizeof(statusMsg) - 1] = '\0';
    } else {
        strcpy(statusMsg, "Enter MacCompanion IP or click Scan LAN.");
    }
    CopyCToPStr(statusMsg, pStatus);
    SetDialogItemText(statusHdl, pStatus);

    SelectDialogItemText(dlg, 5, 0, 32767);
    ShowWindow(dlg);
    SetPort(dlg);
    DrawDialog(dlg);
    OutlineDialogDefaultButton(dlg);

    while (!connected) {
        ModalDialog(NULL, &itemHit);

        if (itemHit == 2) {
            break;
        } else if (itemHit == 3) {
            CursHandle watchCurs = GetCursor(watchCursor);
            SetServerDialogStatus(dlg, statusHdl, "Scanning LAN for MacCompanion...");
            if (watchCurs) SetCursor(*watchCurs);

            if (NetDiscoverServer(NULL, 0)) {
                char foundIp[32];
                InitCursor();
                FormatIPAddress(NetGetServerIP(), foundIp);
                sprintf(portBuf, "%u", (unsigned int)NetGetServerPort());

                CopyCToPStr(foundIp, pIp);
                SetDialogItemText(ipHdl, pIp);
                CopyCToPStr(portBuf, pPort);
                SetDialogItemText(portHdl, pPort);
                SelectDialogItemText(dlg, 5, 0, 32767);

                snprintf(statusMsg, sizeof(statusMsg), "Found %s (click Connect)", foundIp);
                SetServerDialogStatus(dlg, statusHdl, statusMsg);
            } else {
                const char *note = NetGetLastErrorNote();
                InitCursor();
                if (note && note[0] != '\0') {
                    SetServerDialogStatus(dlg, statusHdl, note);
                } else {
                    SetServerDialogStatus(dlg, statusHdl, "No NetFlim server found on LAN.");
                }
            }
        } else if (itemHit == 1) {
            ip_addr newIP = 0;
            unsigned int newPort = DEFAULT_HTTP_PORT;
            char pingResp[96];
            CursHandle watchCurs;

            GetDialogItemText(ipHdl, pIp);
            CopyPToCStr(pIp, ipBuf, sizeof(ipBuf));

            GetDialogItemText(portHdl, pPort);
            CopyPToCStr(pPort, portBuf, sizeof(portBuf));
            if (atoi(portBuf) > 0 && atoi(portBuf) <= 65535) {
                newPort = (unsigned int)atoi(portBuf);
            }

            if (!ParseIPAddress(ipBuf, &newIP)) {
                SelectDialogItemText(dlg, 5, 0, 32767);
                SetServerDialogStatus(dlg, statusHdl, "Invalid IPv4 address (e.g. 10.0.2.2)");
                continue;
            }

            snprintf(statusMsg, sizeof(statusMsg), "Connecting to %s:%u...", ipBuf, newPort);
            SetServerDialogStatus(dlg, statusHdl, statusMsg);
            watchCurs = GetCursor(watchCursor);
            if (watchCurs) SetCursor(*watchCurs);

            if (NetProbeServer(newIP, (unsigned short)newPort, pingResp, sizeof(pingResp))) {
                InitCursor();
                NetSetServer(newIP, (unsigned short)newPort);
                NetSavePrefs();
                gServerConnected = true;
                connected = true;
                break;
            } else {
                const char *note = NetGetLastErrorNote();
                InitCursor();
                if (note && note[0] != '\0') {
                    SetServerDialogStatus(dlg, statusHdl, note);
                } else {
                    snprintf(statusMsg, sizeof(statusMsg), "Could not reach NetFlim API on %s", ipBuf);
                    SetServerDialogStatus(dlg, statusHdl, statusMsg);
                }
            }
        }
    }

    DisposeDialog(dlg);

    if (connected) {
        RefreshCatalog();
    }

    if (gMainWindow) SetPort(gMainWindow);
    InvalidateWindow();
}

static void StartSelectedVideo(Boolean fullscreen) {
    if (gSelectedIdx < 0 || gSelectedIdx >= gVideoCount) return;
    /* On compact 512x342 displays (SE / SE/30), start in 1:1 Direct-VRAM Fullscreen by default */
    if (gCompactScreen) {
        fullscreen = true;
    }
    if (!PlayerStart(&gVideos[gSelectedIdx], gMainWindow, fullscreen)) {
        ShowMessageAlert("Stream Error", "Could not start HTTP/TCP stream on port 6800.", "");
    }
    SyncMenuState();
    InvalidateWindow();
}

static void StepPage(short delta) {
    short totalPages = GetTotalPages();
    short next = (short)(gPageIdx + delta);
    if (next < 0 || next >= totalPages) return;
    gPageIdx = next;
    gSelectedIdx = gPageIdx * ITEMS_PER_PAGE;
    FetchPagePosters();
    SyncMenuState();
    InvalidateWindow();
}

/* ============================================================================
 * Event Handlers
 * ============================================================================ */

static void HandleMenuCommand(long menuChoice) {
    short menuId = HiWord(menuChoice);
    short itemId = LoWord(menuChoice);

    switch (menuId) {
        case mApple:
            if (itemId == iAbout) {
                char line2[64];
                sprintf(line2, "HTTP/TCP 6800 Streaming Player (%s)", NetResolveProfileName());
                ShowMessageAlert(APP_VERSION_LONG, "MacFlim Cinema Client for Macintosh SE / SE/30+", line2);
            } else {
                MenuHandle appleM = GetMenuHandle(mApple);
                Str255 daName;
                GetMenuItemText(appleM, itemId, daName);
                OpenDeskAcc(daName);
            }
            break;

        case mFile:
            switch (itemId) {
                case iRefreshCatalog:
                    if (PlayerGetState() == kPlayerStopped || PlayerGetState() == kPlayerError) {
                        RefreshCatalog();
                    }
                    break;
                case iSetServerIP:
                    if (PlayerGetState() != kPlayerStopped && PlayerGetState() != kPlayerError) {
                        PlayerStop(gMainWindow);
                    }
                    OpenSetServerDialog();
                    break;
                case iStopPlayback:
                    PlayerStop(gMainWindow);
                    SyncMenuState();
                    break;
                case iQuit:
                    gRunning = false;
                    break;
            }
            break;

        case mPlayback:
            switch (itemId) {
                case iPlayPause:
                    if (PlayerGetState() == kPlayerStopped || PlayerGetState() == kPlayerError) {
                        StartSelectedVideo(false);
                    } else {
                        PlayerTogglePause();
                        SyncMenuState();
                        RedrawPlayerChromeOnly();
                    }
                    break;
                case iFullscreen:
                    if (PlayerGetState() != kPlayerStopped && PlayerGetState() != kPlayerError) {
                        PlayerToggleFullscreen(gMainWindow);
                        SyncMenuState();
                    }
                    break;
                case iLoopVideo:
                    PlayerToggleLoop();
                    SyncMenuState();
                    if (PlayerGetState() != kPlayerStopped && PlayerGetState() != kPlayerError) {
                        RedrawPlayerChromeOnly();
                    }
                    break;
                case iMuteAudio:
                    PlayerToggleMute();
                    SyncMenuState();
                    if (PlayerGetState() != kPlayerStopped && PlayerGetState() != kPlayerError) {
                        RedrawPlayerChromeOnly();
                    }
                    break;
                case iNextPage:
                    StepPage(1);
                    break;
                case iPrevPage:
                    StepPage(-1);
                    break;
            }
            break;

        case mProfile:
            if (itemId >= iProfileAuto && itemId <= iProfilePlus) {
                NetSetProfileMode((NetFlimProfileMode)(itemId - iProfileAuto));
                NetSavePrefs();
                SyncMenuState();
                sprintf(gGalleryStatus, "Profile set to %s", NetResolveProfileName());
                if (PlayerGetState() == kPlayerStopped || PlayerGetState() == kPlayerError) {
                    InvalidateWindow();
                }
            }
            break;
    }

    HiliteMenu(0);
}

static void HandleContentClick(Point localPt) {
    PlayerState pst = PlayerGetState();
    Boolean inPlayer = (pst == kPlayerBuffering || pst == kPlayerPlaying || pst == kPlayerPaused);

    if (inPlayer) {
        if (PtInRect(localPt, &gBtnStopRect)) {
            PlayerStop(gMainWindow);
            SyncMenuState();
        } else if (PtInRect(localPt, &gBtnPauseRect)) {
            PlayerTogglePause();
            SyncMenuState();
            RedrawPlayerChromeOnly();
        } else if (PtInRect(localPt, &gBtnFullRect)) {
            PlayerToggleFullscreen(gMainWindow);
            SyncMenuState();
        } else if (PtInRect(localPt, &gBtnMuteRect)) {
            PlayerToggleMute();
            SyncMenuState();
            RedrawPlayerChromeOnly();
        } else if (PtInRect(localPt, &gBtnLoopRect)) {
            PlayerToggleLoop();
            SyncMenuState();
            RedrawPlayerChromeOnly();
        } else if (PtInRect(localPt, &gViewportRect)) {
            PlayerTogglePause();
            SyncMenuState();
            RedrawPlayerChromeOnly();
        }
        return;
    }

    /* Gallery Mode Clicks */
    if (PtInRect(localPt, &gBtnRefreshRect)) {
        RefreshCatalog();
        return;
    }
    if (PtInRect(localPt, &gBtnServerRect)) {
        OpenSetServerDialog();
        return;
    }
    if (PtInRect(localPt, &gBtnPrevRect)) {
        StepPage(-1);
        return;
    }
    if (PtInRect(localPt, &gBtnNextRect)) {
        StepPage(1);
        return;
    }
    if (PtInRect(localPt, &gBtnPlayGalleryRect)) {
        StartSelectedVideo(false);
        return;
    }

    /* Card grid hit testing */
    {
        short startIdx = gPageIdx * ITEMS_PER_PAGE;
        short slot;
        for (slot = 0; slot < ITEMS_PER_PAGE; slot++) {
            short idx = startIdx + slot;
            Rect cardR;
            if (idx >= gVideoCount) break;
            GetCardRect(slot, &cardR);
            if (PtInRect(localPt, &cardR)) {
                unsigned long now = TickCount();
                Boolean dbl = (idx == gLastClickIdx && (now - gLastClickTick) <= GetDblTime());
                gSelectedIdx = idx;
                gLastClickIdx = idx;
                gLastClickTick = now;

                if (dbl) {
                    StartSelectedVideo(false);
                } else {
                    InvalidateWindow();
                }
                return;
            }
        }
    }
}

static void HandleKeyDown(const EventRecord *ev) {
    char ch = (char)(ev->message & charCodeMask);
    PlayerState pst = PlayerGetState();
    Boolean active = (pst == kPlayerBuffering || pst == kPlayerPlaying || pst == kPlayerPaused);

    if (ev->modifiers & cmdKey) {
        long menuChoice = MenuKey(ch);
        if (menuChoice != 0) {
            HandleMenuCommand(menuChoice);
        }
        return;
    }

    if (ch == 0x1B) { /* Escape */
        if (PlayerIsFullscreen()) {
            PlayerSetFullscreen(false, gMainWindow);
            SyncMenuState();
        } else if (active) {
            PlayerStop(gMainWindow);
            SyncMenuState();
        }
        return;
    }

    if (ch == ' ') {
        if (active) {
            PlayerTogglePause();
            SyncMenuState();
            RedrawPlayerChromeOnly();
        } else if (gVideoCount > 0) {
            StartSelectedVideo(false);
        }
        return;
    }

    if (ch == 'f' || ch == 'F') {
        if (active) {
            PlayerToggleFullscreen(gMainWindow);
            SyncMenuState();
        } else if (gVideoCount > 0) {
            StartSelectedVideo(true);
        }
        return;
    }

    if (ch == 'm' || ch == 'M') {
        PlayerToggleMute();
        SyncMenuState();
        if (active) RedrawPlayerChromeOnly();
        return;
    }

    if (ch == 'l' || ch == 'L') {
        PlayerToggleLoop();
        SyncMenuState();
        if (active) RedrawPlayerChromeOnly();
        return;
    }

    if (ch == '\r' || ch == 0x03) { /* Return / Enter */
        if (!active && gVideoCount > 0) {
            StartSelectedVideo(false);
        }
        return;
    }

    if (!active && gVideoCount > 0) {
        if (ch == 0x1C) { /* Left arrow */
            StepPage(-1);
        } else if (ch == 0x1D) { /* Right arrow */
            StepPage(1);
        }
    }
}

/* ============================================================================
 * Main Application Entry Point
 * ============================================================================ */

int main(void) {
    Handle menuBar;
    Rect winBounds;
    short scrW, scrH, winLeft, winTop;
    EventRecord ev;

    MaxApplZone();
    MoreMasters();
    MoreMasters();

    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();
    FlushEvents(everyEvent, 0);
    gHasWNE = (NGetTrapAddress(_WaitNextEvent, ToolTrap) != NGetTrapAddress(_Unimplemented, ToolTrap));

    menuBar = GetNewMBar(128);
    if (menuBar) {
        SetMenuBar(menuBar);
        AppendResMenu(GetMenuHandle(mApple), 'DRVR');
        DrawMenuBar();
    }

    scrW = qd.screenBits.bounds.right - qd.screenBits.bounds.left;
    scrH = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
    if (scrW >= 544 && scrH >= 420) {
        /* 640x480+ displays get a 512x390 window -> exact 512x342 1:1 unscaled viewport */
        gCompactScreen = false;
        gWinW = 512;
        gWinH = 390;
        winLeft = ((scrW - WIN_W) / 2) & ~15;
        winTop  = 20 + ((scrH - 20) - WIN_H) / 2;
        if (winTop < 38) winTop = 38;
    } else {
        /* 512x342 Compact Mac (SE / SE/30): full 512px width at x=0 (32-bit aligned in VRAM) */
        gCompactScreen = true;
        gWinW = 512;
        gWinH = 304;
        winLeft = 0;
        winTop  = 38;
    }
    gCardLeftMargin = (gWinW - (PAGE_COLS * CARD_W + (PAGE_COLS - 1) * CARD_GAP_X)) / 2;
    gCardTopMargin  = GALLERY_TOP + ((GALLERY_BOTTOM - GALLERY_TOP) - (PAGE_ROWS * CARD_H + CARD_GAP_Y)) / 2;
    if (gCardTopMargin < GALLERY_TOP + 3) gCardTopMargin = GALLERY_TOP + 3;

    SetRect(&winBounds, winLeft, winTop, winLeft + WIN_W, winTop + WIN_H);
    gMainWindow = NewWindow(NULL, &winBounds, "\pNetFlim", true, noGrowDocProc, (WindowPtr)-1L, true, 0);
    if (!gMainWindow) return 1;

    SetPort(gMainWindow);
    UpdateHitRects();

    PlayerInit();
    gMacTcpOk = NetInit();
    SyncMenuState();
    DrawMainWindow();

    if (gMacTcpOk) {
        RefreshCatalog();
        if (!gServerConnected) {
            OpenSetServerDialog();
        }
    } else {
        strcpy(gGalleryStatus, "MacTCP driver (.IPP) not installed");
        InvalidateWindow();
    }

    {
        unsigned long lastWneTick = 0;
        unsigned long lastSysTick = 0;
        while (gRunning) {
            Boolean streamingActive;
            Boolean uiChanged = false;
            Boolean haveEvent = false;
            unsigned long now = TickCount();

            if (now != lastSysTick) {
                SystemTask();
                lastSysTick = now;
            }

            streamingActive = PlayerTick(gMainWindow, &gViewportRect, &uiChanged);
            if (uiChanged) {
                PlayerState st = PlayerGetState();
                SyncMenuState();
                if (st == kPlayerPlaying || st == kPlayerBuffering || st == kPlayerPaused) {
                    RedrawPlayerChromeOnly();
                } else {
                    if (st == kPlayerError) {
                        PlayerSnapshot snap;
                        PlayerGetSnapshot(&snap);
                        strncpy(gGalleryStatus, snap.statusMsg, sizeof(gGalleryStatus) - 1);
                        gGalleryStatus[sizeof(gGalleryStatus) - 1] = '\0';
                    }
                    InvalidateWindow();
                }
            }

            now = TickCount();
            if (streamingActive) {
                if (gHasWNE) {
                    if (!PlayerWantsZeroSleep()) {
                        haveEvent = WaitNextEvent(everyEvent, &ev, 1L, NULL);
                        lastWneTick = now;
                    } else if (EventAvail(everyEvent, &ev) || (now - lastWneTick) >= 4UL) {
                        haveEvent = WaitNextEvent(everyEvent, &ev, 0L, NULL);
                        lastWneTick = now;
                    }
                } else if (EventAvail(everyEvent, &ev)) {
                    haveEvent = GetNextEvent(everyEvent, &ev);
                }
            } else {
                long sleepTicks = (PlayerGetState() == kPlayerPaused) ? 2L : 4L;
                haveEvent = gHasWNE
                    ? WaitNextEvent(everyEvent, &ev, sleepTicks, NULL)
                    : GetNextEvent(everyEvent, &ev);
                lastWneTick = now;
            }

            if (haveEvent) {
                switch (ev.what) {
                case mouseDown: {
                    WindowPtr clickWin = NULL;
                    short part;

                    if (PlayerIsFullscreen()) {
                        /* Click in fullscreen cleanly returns to windowed player HUD */
                        PlayerSetFullscreen(false, gMainWindow);
                        SyncMenuState();
                        break;
                    }

                    part = FindWindow(ev.where, &clickWin);
                    switch (part) {
                        case inMenuBar:
                            HandleMenuCommand(MenuSelect(ev.where));
                            break;
                        case inSysWindow:
                            SystemClick(&ev, clickWin);
                            break;
                        case inDrag:
                            DragWindow(clickWin, ev.where, &qd.screenBits.bounds);
                            break;
                        case inGoAway:
                            if (TrackGoAway(clickWin, ev.where)) {
                                if (PlayerGetState() != kPlayerStopped) {
                                    PlayerStop(gMainWindow);
                                    SyncMenuState();
                                } else {
                                    gRunning = false;
                                }
                            }
                            break;
                        case inContent:
                            if (clickWin != FrontWindow()) {
                                SelectWindow(clickWin);
                            } else if (clickWin == gMainWindow) {
                                Point pt = ev.where;
                                SetPort(gMainWindow);
                                GlobalToLocal(&pt);
                                HandleContentClick(pt);
                            }
                            break;
                    }
                    break;
                }

                case keyDown:
                case autoKey:
                    HandleKeyDown(&ev);
                    break;

                case updateEvt: {
                    WindowPtr updWin = (WindowPtr)ev.message;
                    if (updWin == gMainWindow && !PlayerIsFullscreen()) {
                        BeginUpdate(updWin);
                        DrawMainWindow();
                        EndUpdate(updWin);
                    }
                    break;
                }

                case activateEvt:
                    SyncMenuState();
                    break;
            }
        }
    }
    }

    PlayerShutdown();
    NetShutdown();
    if (gMainWindow) DisposeWindow(gMainWindow);
    return 0;
}
