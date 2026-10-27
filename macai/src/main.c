#include <Types.h>
#include <Memory.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Events.h>
#include <Menus.h>
#include <Windows.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Devices.h>
#include <ToolUtils.h>
#include <Processes.h>
#include <Files.h>
#include <Folders.h>
#include <Scrap.h>
#include <Sound.h>
#include <OSUtils.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "version.h"
#include "assets.h"
#include "network.h"

#define DEFAULT_WIN_W       512
#define DEFAULT_WIN_H       322
#define MIN_WIN_W           380
#define MIN_WIN_H           240
#define MAX_WIN_W           1024
#define MAX_WIN_H           768

#define SIDEBAR_W           143
#define MONACO_CHAR_W       6
#define MAX_PROMPT_VIS_LINES 5

#define mApple              128
#define iAbout              1

#define mFile               129
#define iNewChat            1
#define iSearchChats        2
#define iToggleSidebar      3
#define iReconnect          5
#define iQuit               7

#define mAI                 130
#define iUseGemini          1
#define iUseOpenAI          2
#define iUseClaude          3
#define iTierFast           5
#define iTierPro            6
#define iTierThinking       7
#define iToggleTTS          9

#define MAX_CHATS           30
#define MAX_MESSAGES        40
#define MAX_MSG_TEXT        1024
#define MAX_WRAPPED_LINES   64

typedef struct {
    char id[28];
    char title[48];
    char provider[12];
    char tier[12];
    char age[12];
} ChatSummary;

typedef struct {
    Boolean isUser;
    char    text[MAX_MSG_TEXT];
    short   cachedMaxW;
    short   cachedLines;
    short   cachedWidestPx;
    short   cachedBubbleH;
} ChatMessage;

typedef struct {
    char    id[12];
    char    name[16];
    Boolean enabled;
} ProviderInfo;

/* Global Window & Offscreen Double-Buffer */
static WindowPtr      gMainWindow       = NULL;
static short          gWinW             = DEFAULT_WIN_W;
static short          gWinH             = DEFAULT_WIN_H;
static GrafPort       gOffscreenPort;
static Boolean        gPortOpened       = false;
static Ptr            gOffscreenMem     = NULL;
static Boolean        gHasOffscreen     = false;
static short          gOffscreenW       = 0;
static short          gOffscreenH       = 0;

/* Connection & App State */
static Boolean        gRunning          = true;
static Boolean        gScanningServer   = true;
static Boolean        gServerConnected  = false;
static Boolean        gAwaitingLogin    = false;
static Boolean        gAboutModalOpen   = false;
static Boolean        gManualIPFocused  = true;
static char           gManualIPBuf[32]  = "10.0.2.2";
static char           gServerIPStr[32]  = "";

/* AI Provider & Tier State */
static ProviderInfo   gProviders[3] = {
    { "gemini",  "Gemini",  false },
    { "chatgpt", "ChatGPT", false },
    { "claude",  "Claude",  false }
};
static char           gActiveProvider[12] = "gemini";
static char           gActiveTier[12]     = "Pro";
static Boolean        gProviderMenuOpen   = false;
static Boolean        gTierMenuOpen       = false;
static Boolean        gReadRepliesTTS     = false;

/* Sidebar & Chat History State */
static Boolean        gSidebarCollapsed = false;
static ChatSummary    gChats[MAX_CHATS];
static short          gChatCount        = 0;
static short          gChatScroll       = 0;
static short          gSelectedChatIdx  = -1;
static char           gActiveChatId[28] = "";

/* Active Conversation Messages & Scroll */
static ChatMessage    gMessages[MAX_MESSAGES];
static short          gMessageCount     = 0;
static short          gMsgScrollY       = 0;
static short          gMsgMaxScrollY    = 0;
static Boolean        gWaitingForAI     = false;
static short          gCopiedMsgIdx     = -1;
static unsigned long  gCopiedTick       = 0;

/* Bottom Prompt Input State (Multi-line auto-expanding up to 5 lines) */
static char           gPromptBuf[1024]  = "";
static Boolean        gPromptFocused    = false;
static Boolean        gCaretVisible     = true;
static unsigned long  gLastCaretTick    = 0;
static short          gPromptScrollLine = 0;
static short          gPrevPromptVisLines = 1;

/* Search Modal State */
static Boolean        gSearchModalOpen  = false;
static char           gSearchQuery[64]  = "";
static ChatSummary    gSearchResults[12];
static short          gSearchCount      = 0;

/* Sound Input (Microphone) State */
static Boolean        gHasMicHardware   = false;
static Boolean        gIsRecordingVoice = false;

/* Shared Network Scratch Buffer */
static char          *gNetBuf           = NULL;
#define NET_BUF_SIZE  16384

/* Custom 1-bit QuickDraw Patterns matching ui/MacAI PNGs */
static const Pattern kPatLightDither = { { 0x88, 0x00, 0x22, 0x00, 0x88, 0x00, 0x22, 0x00 } };
static const Pattern kPatModalDim    = { { 0xAA, 0x00, 0x55, 0x00, 0xAA, 0x00, 0x55, 0x00 } };

/* Forward Declarations */
static void RenderWindow(void);
static void RenderChatAreaOnly(void);
static void RenderPromptBarOnly(void);
static void SyncAIMenuState(void);
static Boolean IsProviderEnabled(const char *id);
static void FetchProvidersAndChats(void);
static void LoadChatById(const char *chatId);
static void SendPromptToAI(const char *promptText, Boolean isRetry);

/* ============================================================================
 * Preferences File ("MacAI Prefs" in System Folder:Preferences)
 * ============================================================================ */

#define MACAI_PREFS_MAGIC     0x4D434131UL /* 'MCA1' */
#define SPOTIMAC_PREFS_MAGIC  0x4D435331UL /* 'MCS1' */

typedef struct {
    unsigned long magic;
    ip_addr       serverIp;
} HeaderPrefs;

static Boolean GetPrefsFileSpec(FSSpec *outSpec, Boolean spotiMacFallback)
{
    short vRefNum = 0;
    long  dirID   = 0;
    OSErr err;
    ConstStr255Param fileName = spotiMacFallback ? "\pSpotiMac Prefs" : "\pMacAI Prefs";

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &vRefNum, &dirID);
    if (err != noErr) {
        SysEnvRec theWorld;
        if (SysEnvirons(1, &theWorld) == noErr && theWorld.sysVRefNum != 0) {
            vRefNum = theWorld.sysVRefNum;
            dirID   = 0;
        } else {
            return false;
        }
    }

    err = FSMakeFSSpec(vRefNum, dirID, fileName, outSpec);
    return (err == noErr || err == fnfErr);
}

static void SaveLastServerIP(ip_addr ip)
{
    short refNum = 0;
    long count = sizeof(HeaderPrefs);
    FSSpec spec;
    HeaderPrefs prefs;
    OSErr err;

    if (ip == 0) return;
    prefs.magic    = MACAI_PREFS_MAGIC;
    prefs.serverIp = ip;

    if (GetPrefsFileSpec(&spec, false)) {
        FSpCreate(&spec, 'MCAI', 'pref', smSystemScript);
        err = FSpOpenDF(&spec, fsWrPerm, &refNum);
        if (err == noErr) {
            SetEOF(refNum, 0);
            FSWrite(refNum, &count, &prefs);
            FSClose(refNum);
        }
    }
}

static Boolean LoadLastServerIP(ip_addr *outIP)
{
    short refNum = 0;
    long count;
    FSSpec spec;
    HeaderPrefs prefs;
    short attempt;
    OSErr err;

    if (!outIP) return false;
    *outIP = 0;

    /* Remove any legacy "MacAI Prefs" file sitting in the application folder from v0.1.0 */
    FSDelete("\pMacAI Prefs", 0);

    for (attempt = 0; attempt < 2; attempt++) {
        if (GetPrefsFileSpec(&spec, attempt == 1)) {
            err = FSpOpenDF(&spec, fsRdPerm, &refNum);
            if (err == noErr) {
                count = sizeof(HeaderPrefs);
                memset(&prefs, 0, sizeof(prefs));
                err = FSRead(refNum, &count, &prefs);
                FSClose(refNum);
                if ((err == noErr || err == eofErr) &&
                    count >= (long)sizeof(HeaderPrefs) &&
                    (prefs.magic == MACAI_PREFS_MAGIC || prefs.magic == SPOTIMAC_PREFS_MAGIC) &&
                    prefs.serverIp != 0) {
                    *outIP = prefs.serverIp;
                    return true;
                }
            }
        }
    }
    return false;
}

/* ============================================================================
 * Offscreen Double-Buffer Management (Dynamic for Resizable Window)
 * ============================================================================ */

static void EnsureOffscreenBuffer(short w, short h)
{
    short rowBytes = ((w + 15) / 16) * 2;
    long bytesNeeded = (long)rowBytes * (long)h;
    Rect bounds;

    if (gHasOffscreen && gOffscreenMem && gOffscreenW == w && gOffscreenH == h) {
        return;
    }

    if (gOffscreenMem) {
        DisposePtr(gOffscreenMem);
        gOffscreenMem = NULL;
        gHasOffscreen = false;
    }

    SetRect(&bounds, 0, 0, w, h);
    gOffscreenMem = NewPtrClear(bytesNeeded);
    if (gOffscreenMem != NULL) {
        GrafPtr savePort;
        GetPort(&savePort);
        if (!gPortOpened) {
            OpenPort(&gOffscreenPort);
            gPortOpened = true;
        }
        gOffscreenPort.portBits.baseAddr = gOffscreenMem;
        gOffscreenPort.portBits.rowBytes = rowBytes;
        gOffscreenPort.portBits.bounds   = bounds;
        gOffscreenPort.portRect          = bounds;
        RectRgn(gOffscreenPort.visRgn, &bounds);
        RectRgn(gOffscreenPort.clipRgn, &bounds);
        SetPort(savePort);
        gOffscreenW   = w;
        gOffscreenH   = h;
        gHasOffscreen = true;
    }
}

/* ============================================================================
 * QuickDraw Drawing Helpers
 * ============================================================================ */

static void DrawCString(const char *str)
{
    unsigned char pstr[256];
    short len = 0;
    if (!str) return;
    while (str[len] && len < 254) {
        pstr[len + 1] = (unsigned char)str[len];
        len++;
    }
    pstr[0] = (unsigned char)len;
    DrawString(pstr);
}

static short MeasureCString(const char *str)
{
    if (!str) return 0;
    return (short)strlen(str) * MONACO_CHAR_W;
}

static void DrawCStringTruncated(const char *str, short maxPx)
{
    char buf[128];
    short len, maxChars;
    if (!str) return;
    len = (short)strlen(str);
    maxChars = maxPx / MONACO_CHAR_W;
    if (len <= maxChars) {
        DrawCString(str);
        return;
    }
    if (maxChars <= 2) return;
    if (maxChars > (short)(sizeof(buf) - 1)) maxChars = sizeof(buf) - 1;
    memcpy(buf, str, maxChars - 2);
    buf[maxChars - 2] = '.';
    buf[maxChars - 1] = '.';
    buf[maxChars]     = '\0';
    DrawCString(buf);
}

static void CopyTextToClipboard(const char *text)
{
    long len;
    if (!text) return;
    len = (long)strlen(text);
    ZeroScrap();
    if (len > 0) {
        PutScrap(len, 'TEXT', (Ptr)text);
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

static void DrawDottedHLine(short x1, short x2, short y)
{
    short x;
    for (x = x1; x <= x2; x += 2) {
        MoveTo(x, y);
        Line(0, 0);
    }
}

static void DrawDottedVLine(short x, short y1, short y2)
{
    short y;
    for (y = y1; y <= y2; y += 2) {
        MoveTo(x, y);
        Line(0, 0);
    }
}

static void DrawChevronDown(short x, short y)
{
    /* 5x3 down chevron matching Main UI.png */
    MoveTo(x,     y);     Line(0, 0);
    MoveTo(x + 4, y);     Line(0, 0);
    MoveTo(x + 1, y + 1); Line(0, 0);
    MoveTo(x + 3, y + 1); Line(0, 0);
    MoveTo(x + 2, y + 2); Line(0, 0);
}

static void DrawShadowRoundRect(const Rect *r, short ovalW, short ovalH, short shadowOffset)
{
    Rect shadowR = *r;
    OffsetRect(&shadowR, shadowOffset, shadowOffset);
    PenNormal();
    FillRoundRect(&shadowR, ovalW, ovalH, &qd.black);
    EraseRoundRect(r, ovalW, ovalH);
    FrameRoundRect(r, ovalW, ovalH);
}

static const char *GetProviderDisplayName(const char *id)
{
    if (!id) return "Gemini";
    if (strcmp(id, "openai") == 0 || strcmp(id, "chatgpt") == 0) return "ChatGPT";
    if (strcmp(id, "claude") == 0) return "Claude";
    return "Gemini";
}

static const char *GetTierDisplayName(const char *tier)
{
    if (!tier) return "Pro";
    if (tier[0] == 'f' || tier[0] == 'F') return "Fast";
    if (tier[0] == 't' || tier[0] == 'T') return "Thinking";
    return "Pro";
}

/* ============================================================================
 * Fast Monospace Word Wrapping & Cached Bubble Metrics
 * ============================================================================ */

static short WrapMessageLines(const char *text, short maxWidthPx,
                              char lines[][96], short maxLines)
{
    short lineCount = 0;
    short maxChars = maxWidthPx / MONACO_CHAR_W;
    const char *p = text;

    if (!text || !*text) return 0;
    if (maxChars < 6) maxChars = 6;
    if (maxChars > 90) maxChars = 90;

    while (*p && lineCount < maxLines) {
        short curLen = 0;
        short lastSpaceInLine = -1;
        const char *lastSpacePtr = NULL;
        const char *scan = p;

        while (*scan && *scan != '\n' && curLen < 90) {
            if (*scan == ' ') {
                lastSpaceInLine = curLen;
                lastSpacePtr = scan;
            }
            if (curLen + 1 > maxChars) {
                if (lastSpaceInLine > 0 && lastSpacePtr != NULL) {
                    if (lines) {
                        memcpy(lines[lineCount], p, lastSpaceInLine);
                        lines[lineCount][lastSpaceInLine] = '\0';
                    }
                    p = lastSpacePtr + 1;
                } else {
                    short take = (curLen > 0) ? curLen : 1;
                    if (lines) {
                        memcpy(lines[lineCount], p, take);
                        lines[lineCount][take] = '\0';
                    }
                    p = scan;
                }
                lineCount++;
                goto next_line;
            }
            curLen++;
            scan++;
        }

        if (lines) {
            memcpy(lines[lineCount], p, curLen);
            lines[lineCount][curLen] = '\0';
        }
        lineCount++;
        if (*scan == '\n') {
            p = scan + 1;
            if (*p == '\0' && lineCount < maxLines) {
                if (lines) lines[lineCount][0] = '\0';
                lineCount++;
            }
        } else {
            p = scan;
        }
next_line:;
    }

    return (lineCount > 0) ? lineCount : 1;
}

static void EnsureMessageMetrics(ChatMessage *msg, short maxW)
{
    static char tmpWrapped[MAX_WRAPPED_LINES][96];
    short nLines, L, widestPx = 40;

    if (!msg) return;
    if (msg->cachedMaxW == maxW && msg->cachedBubbleH > 0) {
        return;
    }

    nLines = WrapMessageLines(msg->text, maxW, tmpWrapped, MAX_WRAPPED_LINES);
    for (L = 0; L < nLines; L++) {
        short lw = (short)strlen(tmpWrapped[L]) * MONACO_CHAR_W;
        if (lw > widestPx) widestPx = lw;
    }

    msg->cachedMaxW     = maxW;
    msg->cachedLines    = nLines;
    msg->cachedWidestPx = widestPx;
    msg->cachedBubbleH  = nLines * 12 + 12;
}

/* ============================================================================
 * Pre-Connection & Login Error Screens (Server-search.png & login-error.png)
 * ============================================================================ */

static void GetCenteredCardRect(Rect *outCard)
{
    short cardW = 236;
    short cardH = 199;
    short left  = (gWinW - cardW) / 2;
    short top   = (gWinH - cardH) / 2;
    if (left < 10) left = 10;
    if (top < 10)  top  = 10;
    SetRect(outCard, left, top, left + cardW, top + cardH);
}

static void DrawPreConnectOrLoginScreen(void)
{
    Rect winR, cardR, btnR, ipR, connR;
    short cx;

    SetRect(&winR, 0, 0, gWinW, gWinH);
    FillRect(&winR, &qd.gray);

    GetCenteredCardRect(&cardR);
    cx = (cardR.left + cardR.right) / 2;

    /* Card with 2px black drop shadow */
    DrawShadowRoundRect(&cardR, 16, 16, 2);

    /* MacAI+ 32x45 Logo centered near top of card */
    BlitBitmap(kLogoMacAI32x45, 4, 32, 45, cx - 16, cardR.top + 14, srcOr);

    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);

    if (gScanningServer) {
        const char *s1 = "Searching for MacCompanion";
        const char *s2 = "server on your network...";
        MoveTo(cx - MeasureCString(s1) / 2, cardR.top + 104);
        DrawCString(s1);
        MoveTo(cx - MeasureCString(s2) / 2, cardR.top + 118);
        DrawCString(s2);
    } else if (gServerConnected && gAwaitingLogin) {
        /* Matches ui/MacAI/login-error.png */
        const char *l1 = "You have not logged";
        const char *l2 = "in to any AI services.";
        char l3[64];
        const char *l4 = "from a modern browser to login.";
        const char *btnTxt = "Done! Retry!";

        sprintf(l3, "Please visit %s:%u", gServerIPStr, NetGetServerPort());

        MoveTo(cx - MeasureCString(l1) / 2, cardR.top + 78);
        DrawCString(l1);
        MoveTo(cx - MeasureCString(l2) / 2, cardR.top + 90);
        DrawCString(l2);

        MoveTo(cx - MeasureCString(l3) / 2, cardR.top + 114);
        DrawCString(l3);
        MoveTo(cx - MeasureCString(l4) / 2, cardR.top + 126);
        DrawCString(l4);

        SetRect(&btnR, cx - 54, cardR.top + 140, cx + 54, cardR.top + 165);
        DrawShadowRoundRect(&btnR, 12, 12, 2);
        MoveTo(cx - MeasureCString(btnTxt) / 2, btnR.top + 16);
        DrawCString(btnTxt);
    } else {
        /* Matches ui/MacAI/Server-search.png */
        const char *msg = "No MacCompanion server found";
        const char *retryTxt = "Retry Scan";
        const char *orTxt = "Or connect manually";
        const char *connTxt = "Connect";
        char ipDisp[40];

        MoveTo(cx - MeasureCString(msg) / 2, cardR.top + 81);
        DrawCString(msg);

        /* Retry Scan button (108x25) */
        SetRect(&btnR, cx - 54, cardR.top + 91, cx + 54, cardR.top + 116);
        DrawShadowRoundRect(&btnR, 12, 12, 2);
        MoveTo(cx - MeasureCString(retryTxt) / 2, btnR.top + 16);
        DrawCString(retryTxt);

        /* Divider + "Or connect manually" */
        MoveTo(cardR.left + 20, cardR.top + 138);
        LineTo(cardR.right - 20, cardR.top + 138);
        {
            short tw = MeasureCString(orTxt);
            Rect lblBg;
            SetRect(&lblBg, cx - tw / 2 - 4, cardR.top + 131, cx + tw / 2 + 4, cardR.top + 145);
            EraseRect(&lblBg);
            MoveTo(cx - tw / 2, cardR.top + 141);
            DrawCString(orTxt);
        }

        /* Manual IP box + Connect button */
        SetRect(&ipR, cardR.left + 28, cardR.top + 154, cardR.left + 137, cardR.top + 179);
        EraseRoundRect(&ipR, 10, 10);
        FrameRoundRect(&ipR, 10, 10);

        sprintf(ipDisp, "%s%s", gManualIPBuf, gManualIPFocused ? "_" : "");
        MoveTo(ipR.left + 8, ipR.top + 16);
        DrawCStringTruncated(ipDisp, (ipR.right - ipR.left) - 14);

        SetRect(&connR, cardR.left + 141, cardR.top + 154, cardR.left + 206, cardR.top + 179);
        DrawShadowRoundRect(&connR, 12, 12, 2);
        MoveTo((connR.left + connR.right) / 2 - MeasureCString(connTxt) / 2, connR.top + 16);
        DrawCString(connTxt);
    }
}

/* ============================================================================
 * Main Chat UI Rendering (Main UI.png, sidebar-collapsed.png, Search.png)
 * ============================================================================ */

static short GetPromptTextMaxWidthPx(void)
{
    short chatLeft  = gSidebarCollapsed ? 0 : (SIDEBAR_W + 1);
    short chatWidth = gWinW - chatLeft;
    short pillW     = 220 + (gWinW - DEFAULT_WIN_W);
    const char *tierLabel = GetTierDisplayName(gActiveTier);
    short tierW     = MeasureCString(tierLabel) + 18;
    short maxPx;

    if (pillW < 190) pillW = 190;
    if (pillW > chatWidth - 60) pillW = chatWidth - 60;

    maxPx = pillW - tierW - 30;
    if (maxPx < 60) maxPx = 60;
    return maxPx;
}

static short WrapPromptInputLines(char outLines[][96], short maxLines, short *outVisLines)
{
    static char promptWithCaret[1032];
    short maxPromptPx = GetPromptTextMaxWidthPx();
    short totalLines;
    short visLines;

    if (gIsRecordingVoice) {
        if (outLines) strcpy(outLines[0], "Listening... (click mic)");
        if (outVisLines) *outVisLines = 1;
        return 1;
    }
    if (!gPromptFocused && gPromptBuf[0] == '\0') {
        if (outLines) strcpy(outLines[0], "Ask MacAI");
        if (outVisLines) *outVisLines = 1;
        return 1;
    }

    sprintf(promptWithCaret, "%s%s", gPromptBuf, (gPromptFocused && gCaretVisible) ? "|" : "");
    totalLines = WrapMessageLines(promptWithCaret, maxPromptPx, outLines, maxLines);
    if (totalLines < 1) totalLines = 1;
    visLines = (totalLines > MAX_PROMPT_VIS_LINES) ? MAX_PROMPT_VIS_LINES : totalLines;
    if (outVisLines) *outVisLines = visLines;
    return totalLines;
}

static void GetPromptBarGeometry(Rect *outPillR, Rect *outTierR, Rect *outMicR)
{
    short chatLeft  = gSidebarCollapsed ? 0 : (SIDEBAR_W + 1);
    short chatWidth = gWinW - chatLeft;
    short pillW     = 220 + (gWinW - DEFAULT_WIN_W);
    short totalW;
    short startX;
    short visLines  = 1;
    short pillH;
    short botY      = gWinH - 21;
    short topY;
    const char *tierLabel = GetTierDisplayName(gActiveTier);
    short tierW;

    if (pillW < 190) pillW = 190;
    if (pillW > chatWidth - 60) pillW = chatWidth - 60;

    WrapPromptInputLines(NULL, 32, &visLines);
    pillH = 22 + (visLines - 1) * 12;
    topY  = botY - pillH;

    totalW = pillW + (gHasMicHardware ? 27 : 0);
    startX = chatLeft + (chatWidth - totalW) / 2;

    SetRect(outPillR, startX, topY, startX + pillW, botY);

    tierW = MeasureCString(tierLabel) + 18;
    SetRect(outTierR, outPillR->right - tierW - 6, botY - 20, outPillR->right - 4, botY - 2);

    if (gHasMicHardware) {
        SetRect(outMicR, outPillR->right + 4, botY - 22, outPillR->right + 27, botY);
    } else {
        SetRect(outMicR, 0, 0, 0, 0);
    }
}

static void SyncPromptExpansionWithScroll(void)
{
    short visLines = 1;
    short totalLines = WrapPromptInputLines(NULL, 32, &visLines);

    /* Keep prompt scroll pinned to bottom when typing unless scrolled manually */
    if (totalLines > MAX_PROMPT_VIS_LINES) {
        if (gPromptScrollLine > totalLines - MAX_PROMPT_VIS_LINES) {
            gPromptScrollLine = totalLines - MAX_PROMPT_VIS_LINES;
        }
        if (gPromptScrollLine < 0) gPromptScrollLine = 0;
    } else {
        gPromptScrollLine = 0;
    }

    if (visLines != gPrevPromptVisLines) {
        short deltaPx = (visLines - gPrevPromptVisLines) * 12;
        gPrevPromptVisLines = visLines;
        if (gMessageCount > 0 || gWaitingForAI) {
            gMsgScrollY += deltaPx;
            if (gMsgScrollY < 0) gMsgScrollY = 0;
        }
    }
}

static void DrawPromptBar(const Rect *pillR, const Rect *tierR, const Rect *micR)
{
    static char promptLines[32][96];
    short visLines = 1;
    short totalLines = WrapPromptInputLines(promptLines, 32, &visLines);
    short i;

    {
        Rect pillShadow = *pillR;
        OffsetRect(&pillShadow, 0, 2);
        FillRoundRect(&pillShadow, 20, 20, &qd.black);
        EraseRoundRect(pillR, 20, 20);
        FrameRoundRect(pillR, 20, 20);
    }

    if (totalLines > MAX_PROMPT_VIS_LINES) {
        if (gPromptScrollLine > totalLines - MAX_PROMPT_VIS_LINES) {
            gPromptScrollLine = totalLines - MAX_PROMPT_VIS_LINES;
        }
        if (gPromptScrollLine < 0) gPromptScrollLine = 0;
    } else {
        gPromptScrollLine = 0;
    }

    for (i = 0; i < visLines; i++) {
        short lineIdx = gPromptScrollLine + i;
        if (lineIdx >= 0 && lineIdx < totalLines) {
            MoveTo(pillR->left + 12, pillR->top + 14 + i * 12);
            DrawCString(promptLines[lineIdx]);
        }
    }

    /* Prompt input scrollbar when > 5 lines high */
    if (totalLines > MAX_PROMPT_VIS_LINES) {
        short sbX = tierR->left - 6;
        short sbTop = pillR->top + 6;
        short sbBot = pillR->bottom - 6;
        short trackH = sbBot - sbTop;
        short maxSc = totalLines - MAX_PROMPT_VIS_LINES;
        short thumbH = 14;
        short thumbTop = sbTop + (short)(((long)(trackH - thumbH) * gPromptScrollLine) / (maxSc > 0 ? maxSc : 1));
        Rect thumbR;

        DrawDottedVLine(sbX, sbTop, sbBot);
        SetRect(&thumbR, sbX - 1, thumbTop, sbX + 2, thumbTop + thumbH);
        FillRect(&thumbR, &qd.black);
    }

    /* Model Tier selector ("Pro v") anchored at bottom-right of prompt pill */
    {
        const char *tierName = GetTierDisplayName(gActiveTier);
        short tw = MeasureCString(tierName);
        MoveTo(tierR->left + 4, pillR->bottom - 8);
        DrawCString(tierName);
        DrawChevronDown(tierR->left + 4 + tw + 5, pillR->bottom - 12);
    }

    /* Conditional Microphone Button (hidden on Macs without Sound Input hardware!) */
    if (gHasMicHardware) {
        Rect micShadow = *micR;
        OffsetRect(&micShadow, 0, 2);
        FillOval(&micShadow, &qd.black);
        EraseOval(micR);
        FrameOval(micR);
        BlitBitmap(kIconMic9x11, 2, 9, 11, micR->left + 7, micR->top + 5, srcOr);
        if (gIsRecordingVoice) {
            InvertOval(micR);
        }
    }
}

static void DrawSidebar(void)
{
    Rect sideR;
    const char *provName = GetProviderDisplayName(gActiveProvider);
    short provW;
    short listTop = 55;
    short listBot = gWinH - 8;
    short visibleRows = (listBot - listTop) / 15;
    short i;

    SetRect(&sideR, 0, 0, SIDEBAR_W, gWinH);
    EraseRect(&sideR);

    /* Vertical dotted divider at x = 143 */
    PenNormal();
    DrawDottedVLine(SIDEBAR_W, 0, gWinH);

    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);

    /* Top Left: Active AI Provider ("Gemini v") */
    MoveTo(12, 15);
    DrawCString(provName);
    provW = MeasureCString(provName);
    DrawChevronDown(12 + provW + 6, 11);

    /* Top Right of Sidebar: Search icon (112, 7) and Collapse Sidebar icon (128, 7) */
    BlitBitmap(kIconSearch10x10, 2, 10, 10, 112, 7, srcOr);
    BlitBitmap(kIconSidebar10x10, 2, 10, 10, 128, 7, srcOr);

    /* "New chat" button row */
    BlitBitmap(kIconNewChat11x11, 2, 11, 11, 11, 30, srcOr);
    MoveTo(27, 38);
    DrawCString("New chat");

    /* Horizontal dotted divider at y = 49 */
    DrawDottedHLine(0, SIDEBAR_W - 1, 49);

    /* Chat History List (y = 55..listBot, 15px per row) */
    if (visibleRows < 1) visibleRows = 1;
    if (gChatScroll > gChatCount - visibleRows) {
        gChatScroll = gChatCount - visibleRows;
    }
    if (gChatScroll < 0) gChatScroll = 0;

    for (i = 0; i < visibleRows; i++) {
        short idx = gChatScroll + i;
        short rowY = listTop + i * 15;
        if (idx >= gChatCount) break;

        if (idx == gSelectedChatIdx ||
            (gActiveChatId[0] != '\0' && strcmp(gChats[idx].id, gActiveChatId) == 0)) {
            Rect selR;
            SetRect(&selR, 5, rowY, 128, rowY + 14);
            FillRoundRect(&selR, 6, 6, &kPatLightDither);
            FrameRoundRect(&selR, 6, 6);
        }

        MoveTo(11, rowY + 11);
        DrawCStringTruncated(gChats[idx].title, 112);
    }

    /* Sidebar Scrollbar (only drawn if chats exceed visibleRows) */
    if (gChatCount > visibleRows) {
        short sbX = 136;
        short sbTop = 56;
        short sbBot = gWinH - 10;
        short trackH = sbBot - sbTop;
        short maxScroll = gChatCount - visibleRows;
        short thumbH = 24;
        short thumbTop = sbTop + (short)(((long)(trackH - thumbH) * gChatScroll) / (maxScroll > 0 ? maxScroll : 1));
        Rect thumbR;

        DrawDottedVLine(sbX, sbTop, sbBot);
        SetRect(&thumbR, sbX - 2, thumbTop, sbX + 3, thumbTop + thumbH);
        FillRect(&thumbR, &qd.black);
    }
}

static void DrawCollapsedSidebarControls(void)
{
    /* Matches ui/MacAI/sidebar-collapsed.png: 3 icons at top-left on dithered bg */
    Rect bgR;
    SetRect(&bgR, 10, 5, 27, 51);
    EraseRoundRect(&bgR, 6, 6);

    BlitBitmap(kIconSidebar10x10, 2, 10, 10, 13, 8,  srcOr);
    BlitBitmap(kIconSearch10x10,  2, 10, 10, 13, 22, srcOr);
    BlitBitmap(kIconNewChat11x11, 2, 11, 11, 13, 37, srcOr);
}

static void DrawChatArea(void)
{
    static char wrapped[MAX_WRAPPED_LINES][96];
    short chatLeft  = gSidebarCollapsed ? 0 : (SIDEBAR_W + 1);
    short chatWidth = gWinW - chatLeft;
    Rect chatBgR, clipR, savedClip;
    Rect pillR, tierR, micR;
    short msgAreaTop = 8;
    short msgAreaBot;
    short totalContentH = 0;
    short curY;
    short i, L;
    short userMaxW, aiMaxW, aiLeftX;

    GetPromptBarGeometry(&pillR, &tierR, &micR);
    msgAreaBot = pillR.top - 6;
    if (msgAreaBot < 60) msgAreaBot = 60;

    SetRect(&chatBgR, chatLeft, 0, gWinW, gWinH);
    FillRect(&chatBgR, &kPatLightDither);

    if (gSidebarCollapsed) {
        DrawCollapsedSidebarControls();
    }

    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);

    userMaxW = chatWidth - 90;
    if (userMaxW > 240) userMaxW = 240;
    if (userMaxW < 120) userMaxW = 120;

    aiLeftX = chatLeft + (gSidebarCollapsed ? 34 : 18);
    aiMaxW  = (gWinW - 22) - aiLeftX - 24;
    if (aiMaxW < 140) aiMaxW = 140;

    /* Pass 1: Compute total height using cached bubble metrics (O(1) per message!) */
    for (i = 0; i < gMessageCount; i++) {
        short maxW = gMessages[i].isUser ? userMaxW : aiMaxW;
        EnsureMessageMetrics(&gMessages[i], maxW);
        /* AI responses have 15px (~1 line height) extra space below for Copy & Retry buttons */
        totalContentH += gMessages[i].cachedBubbleH + (gMessages[i].isUser ? 8 : 23);
    }
    if (gWaitingForAI) {
        totalContentH += 34;
    }

    {
        short availH = msgAreaBot - msgAreaTop;
        gMsgMaxScrollY = (totalContentH > availH) ? (totalContentH - availH) : 0;
        if (gMsgScrollY > gMsgMaxScrollY) gMsgScrollY = gMsgMaxScrollY;
        if (gMsgScrollY < 0) gMsgScrollY = 0;
    }

    /* Clip message drawing to above the prompt bar */
    SetRect(&clipR, chatLeft, 0, gWinW, msgAreaBot + 2);
    ClipRect(&clipR);

    if (gMessageCount == 0 && !gWaitingForAI) {
        /* Friendly empty chat state */
        const char *welcome = "Ready to chat with ";
        char welcomeFull[64];
        Rect badgeR;
        short tw;
        sprintf(welcomeFull, "%s%s (%s)", welcome,
                GetProviderDisplayName(gActiveProvider),
                GetTierDisplayName(gActiveTier));
        tw = MeasureCString(welcomeFull);
        SetRect(&badgeR,
                chatLeft + (chatWidth - tw) / 2 - 10,
                (msgAreaBot / 2) - 12,
                chatLeft + (chatWidth + tw) / 2 + 10,
                (msgAreaBot / 2) + 10);
        EraseRoundRect(&badgeR, 12, 12);
        FrameRoundRect(&badgeR, 12, 12);
        MoveTo(badgeR.left + 10, badgeR.top + 14);
        DrawCString(welcomeFull);
    }

    curY = msgAreaTop - gMsgScrollY;

    /* Pass 2: Render ONLY visible message bubbles */
    for (i = 0; i < gMessageCount; i++) {
        short maxW      = gMessages[i].isUser ? userMaxW : aiMaxW;
        short bubbleH   = gMessages[i].cachedBubbleH;
        short widestLine = gMessages[i].cachedWidestPx;
        short stepH     = bubbleH + (gMessages[i].isUser ? 8 : 23);

        if (curY + stepH >= 0 && curY <= msgAreaBot) {
            short nLines = WrapMessageLines(gMessages[i].text, maxW, wrapped, MAX_WRAPPED_LINES);
            Rect bubbleR;

            if (gMessages[i].isUser) {
                /* Right-aligned white pill bubble (Main UI.png) */
                short bw = widestLine + 20;
                short rightEdge = gWinW - 14;
                SetRect(&bubbleR, rightEdge - bw, curY, rightEdge, curY + bubbleH);
                EraseRoundRect(&bubbleR, 16, 16);

                for (L = 0; L < nLines; L++) {
                    MoveTo(bubbleR.left + 10, curY + 14 + L * 12);
                    DrawCString(wrapped[L]);
                }
            } else {
                /* Left-aligned bordered card bubble with 2px bottom shadow (Main UI.png) */
                short bw = widestLine + 24;
                Rect shadowR;
                short actionY = curY + bubbleH + 5;
                short retryX  = aiLeftX + 24;
                if (bw < 100) bw = 100;
                SetRect(&bubbleR, aiLeftX, curY, aiLeftX + bw, curY + bubbleH);

                shadowR = bubbleR;
                OffsetRect(&shadowR, 0, 2);
                FillRoundRect(&shadowR, 16, 16, &qd.black);
                EraseRoundRect(&bubbleR, 16, 16);
                FrameRoundRect(&bubbleR, 16, 16);

                for (L = 0; L < nLines; L++) {
                    MoveTo(bubbleR.left + 12, curY + 14 + L * 12);
                    DrawCString(wrapped[L]);
                }

                /* Action row (1 line height below AI response): Copy to Clipboard + Retry */
                {
                    Rect actionBgR;
                    Boolean showCopied = (gCopiedMsgIdx == i && (TickCount() - gCopiedTick) < 90);
                    short bgW = showCopied ? 74 : 38;
                    SetRect(&actionBgR, aiLeftX + 4, actionY - 1, aiLeftX + 4 + bgW, actionY + 12);
                    EraseRoundRect(&actionBgR, 6, 6);

                    BlitBitmap(kIconCopy10x11, 2, 10, 11, aiLeftX + 6, actionY, srcOr);
                    if (showCopied) {
                        MoveTo(aiLeftX + 19, actionY + 9);
                        DrawCString("Copied");
                        retryX = aiLeftX + 60;
                    }
                    BlitBitmap(kIconRetry10x10, 2, 10, 10, retryX, actionY, srcOr);
                }
            }
        }
        curY += stepH;
    }

    if (gWaitingForAI) {
        Rect waitR, shadowR;
        SetRect(&waitR, aiLeftX, curY, aiLeftX + 120, curY + 24);
        shadowR = waitR;
        OffsetRect(&shadowR, 0, 2);
        FillRoundRect(&shadowR, 14, 14, &qd.black);
        EraseRoundRect(&waitR, 14, 14);
        FrameRoundRect(&waitR, 14, 14);
        MoveTo(waitR.left + 12, waitR.top + 15);
        DrawCString("Thinking...");
    }

    /* Restore full window clip */
    SetRect(&savedClip, 0, 0, gWinW, gWinH);
    ClipRect(&savedClip);

    /* Right-edge Chat Scrollbar (only shown when messages overflow) */
    if (gMsgMaxScrollY > 0) {
        short sbX = gWinW - 6;
        short sbTop = 10;
        short sbBot = msgAreaBot - 4;
        short trackH = sbBot - sbTop;
        short thumbH = 28;
        short thumbTop = sbTop + (short)(((long)(trackH - thumbH) * gMsgScrollY) / gMsgMaxScrollY);
        Rect thumbR;

        DrawDottedVLine(sbX, sbTop, sbBot);
        SetRect(&thumbR, sbX - 2, thumbTop, sbX + 3, thumbTop + thumbH);
        FillRect(&thumbR, &qd.black);
    }

    /* Bottom Auto-Expanding Prompt Bar */
    DrawPromptBar(&pillR, &tierR, &micR);

    /* Standard Mac OS Grow Icon indicator in bottom-right corner */
    MoveTo(gWinW - 11, gWinH - 3);
    LineTo(gWinW - 3,  gWinH - 3);
    LineTo(gWinW - 3,  gWinH - 11);
    MoveTo(gWinW - 7,  gWinH - 3);
    LineTo(gWinW - 3,  gWinH - 7);
}

/* ============================================================================
 * Dropdown Popups for Provider ("Gemini v") & Model Tier ("Pro v")
 * ============================================================================ */

static void DrawProviderPopup(void)
{
    Rect popR;
    short i;
    SetRect(&popR, 8, 20, 98, 20 + 3 * 16 + 4);
    DrawShadowRoundRect(&popR, 8, 8, 2);

    TextFont(kFontIDMonaco);
    TextSize(9);
    for (i = 0; i < 3; i++) {
        short ry = popR.top + 2 + i * 16;
        Rect rowR;
        SetRect(&rowR, popR.left + 3, ry, popR.right - 3, ry + 15);
        if (strcmp(gProviders[i].id, gActiveProvider) == 0) {
            FillRoundRect(&rowR, 4, 4, &kPatLightDither);
        }
        MoveTo(popR.left + 10, ry + 11);
        DrawCString(gProviders[i].name);
        if (!gProviders[i].enabled) {
            MoveTo(popR.right - 26, ry + 11);
            DrawCString("(--)");
            PenPat(&qd.gray);
            PenMode(patBic);
            PaintRect(&rowR);
            PenNormal();
        }
    }
}

static void DrawTierPopup(void)
{
    Rect pillR, tierR, micR, popR;
    const char *tiers[3] = { "Fast", "Pro", "Thinking" };
    short i;

    GetPromptBarGeometry(&pillR, &tierR, &micR);
    SetRect(&popR, tierR.right - 78, pillR.top - 54, tierR.right, pillR.top - 2);
    DrawShadowRoundRect(&popR, 8, 8, 2);

    TextFont(kFontIDMonaco);
    TextSize(9);
    for (i = 0; i < 3; i++) {
        short ry = popR.top + 2 + i * 16;
        Rect rowR;
        SetRect(&rowR, popR.left + 3, ry, popR.right - 3, ry + 15);
        if (strcmp(GetTierDisplayName(gActiveTier), tiers[i]) == 0) {
            FillRoundRect(&rowR, 4, 4, &kPatLightDither);
        }
        MoveTo(popR.left + 10, ry + 11);
        DrawCString(tiers[i]);
    }
}

/* ============================================================================
 * Search Chats Modal Overlay (ui/MacAI/Search.png)
 * ============================================================================ */

static void GetSearchModalRect(Rect *outModal)
{
    short mw = 285;
    short mh = 190;
    short left = (gWinW - mw) / 2;
    short top  = (gWinH - mh) / 2;
    SetRect(outModal, left, top, left + mw, top + mh);
}

static void DrawSearchModal(void)
{
    Rect winR, modalR;
    short i;
    char qDisp[72];

    /* Tone down background with semi-transparent dither */
    SetRect(&winR, 0, 0, gWinW, gWinH);
    PenPat(&kPatModalDim);
    PenMode(patBic);
    PaintRect(&winR);
    PenNormal();

    GetSearchModalRect(&modalR);
    DrawShadowRoundRect(&modalR, 16, 16, 2);

    /* Top Search Bar inside modal */
    BlitBitmap(kIconSearch10x10, 2, 10, 10, modalR.left + 12, modalR.top + 12, srcOr);
    BlitBitmap(kIconCloseX8x8,   2, 8,  8,  modalR.right - 19, modalR.top + 13, srcOr);

    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);
    MoveTo(modalR.left + 28, modalR.top + 20);
    if (gSearchQuery[0] == '\0') {
        DrawCString("Search chats");
    } else {
        sprintf(qDisp, "%s_", gSearchQuery);
        DrawCStringTruncated(qDisp, 210);
    }

    /* Dotted divider at modalR.top + 32 */
    DrawDottedHLine(modalR.left + 12, modalR.right - 12, modalR.top + 32);

    /* Up to 8 matching chat rows (16px each) */
    for (i = 0; i < gSearchCount && i < 8; i++) {
        short ry = modalR.top + 45 + i * 16;
        short ageW = MeasureCString(gSearchResults[i].age);

        BlitBitmap(kIconChatBubble9x8, 2, 9, 8, modalR.left + 12, ry, srcOr);
        MoveTo(modalR.left + 27, ry + 8);
        DrawCStringTruncated(gSearchResults[i].title, 195);

        MoveTo(modalR.right - 12 - ageW, ry + 8);
        DrawCString(gSearchResults[i].age);
    }
}

/* ============================================================================
 * About MacAI Modal Dialog (Matches SpotiMac About Modal + Buy Me a Coffee QR)
 * ============================================================================ */

static void DrawAboutModal(void)
{
    Rect dlgRect, shadowRect, btnOk;
    const char *verLine    = "MacAI " APP_VERSION_SHORT;
    const char *codeLine   = "\"" APP_CODENAME "\"";
    const char *coffeeLine = "Like MacAI? Buy me a coffee!";
    short cx = gWinW / 2;
    short topY = (gWinH - 216) / 2;
    short w;

    if (topY < 16) topY = 16;

    SetRect(&shadowRect, cx - 116, topY + 4, cx + 124, topY + 220);
    FillRoundRect(&shadowRect, 10, 10, &qd.gray);

    SetRect(&dlgRect, cx - 120, topY, cx + 120, topY + 216);
    EraseRoundRect(&dlgRect, 10, 10);
    FrameRoundRect(&dlgRect, 10, 10);
    InsetRect(&dlgRect, 2, 2);
    FrameRoundRect(&dlgRect, 8, 8);

    /* MacAI+ Logo (32x45) centered at top */
    BlitBitmap(kLogoMacAI32x45, 4, 32, 45, cx - 16, topY + 10, srcCopy);

    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);
    TextMode(srcOr);

    w = MeasureCString(verLine);
    MoveTo(cx - (w / 2), topY + 68);
    DrawCString(verLine);

    w = MeasureCString(codeLine);
    MoveTo(cx - (w / 2), topY + 82);
    DrawCString(codeLine);

    w = MeasureCString(coffeeLine);
    MoveTo(cx - (w / 2), topY + 100);
    DrawCString(coffeeLine);

    /* 58x58 QR code (~2x2cm on 9" Compact Mac CRT) centered below text */
    BlitBitmap(gAboutQrCodeBits, 8, 58, 58, cx - 29, topY + 110, srcCopy);

    SetRect(&btnOk, cx - 40, topY + 178, cx + 40, topY + 202);
    PaintRoundRect(&btnOk, 6, 6);
    TextMode(srcBic);
    MoveTo(cx - 8, topY + 193);
    DrawCString("OK");
    TextMode(srcOr);
}

/* ============================================================================
 * Master Window Render & Fast Partial Redraws
 * ============================================================================ */

static void RenderPromptBarOnly(void)
{
    GrafPtr savePort;
    Rect pillR, tierR, micR, dirtyR;

    if (!gMainWindow || !gServerConnected || gAwaitingLogin ||
        gSearchModalOpen || gAboutModalOpen || gTierMenuOpen || gProviderMenuOpen) {
        RenderWindow();
        return;
    }

    GetPort(&savePort);
    EnsureOffscreenBuffer(gWinW, gWinH);
    GetPromptBarGeometry(&pillR, &tierR, &micR);

    SetRect(&dirtyR, pillR.left - 2, pillR.top - 2,
            (gHasMicHardware ? micR.right : pillR.right) + 3,
            pillR.bottom + 4);

    if (gHasOffscreen) {
        SetPort(&gOffscreenPort);
    } else {
        SetPort(gMainWindow);
    }

    PenNormal();
    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);
    TextMode(srcOr);

    FillRect(&dirtyR, &kPatLightDither);
    DrawPromptBar(&pillR, &tierR, &micR);

    if (gHasOffscreen) {
        SetPort(gMainWindow);
        CopyBits(&gOffscreenPort.portBits, &gMainWindow->portBits, &dirtyR, &dirtyR, srcCopy, NULL);
    }

    SetPort(savePort);
}

static void RenderChatAreaOnly(void)
{
    GrafPtr savePort;
    Rect chatR;
    short chatLeft = gSidebarCollapsed ? 0 : (SIDEBAR_W + 1);

    if (!gMainWindow || !gServerConnected || gAwaitingLogin ||
        gSearchModalOpen || gAboutModalOpen || gProviderMenuOpen) {
        RenderWindow();
        return;
    }

    GetPort(&savePort);
    EnsureOffscreenBuffer(gWinW, gWinH);
    SetRect(&chatR, chatLeft, 0, gWinW, gWinH);

    if (gHasOffscreen) {
        SetPort(&gOffscreenPort);
    } else {
        SetPort(gMainWindow);
    }

    PenNormal();
    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);
    TextMode(srcOr);

    DrawChatArea();
    if (gTierMenuOpen) {
        DrawTierPopup();
    }

    if (gHasOffscreen) {
        SetPort(gMainWindow);
        CopyBits(&gOffscreenPort.portBits, &gMainWindow->portBits, &chatR, &chatR, srcCopy, NULL);
    }

    SetPort(savePort);
}

static void RenderWindow(void)
{
    GrafPtr savePort;
    Rect winR;

    if (!gMainWindow) return;
    GetPort(&savePort);

    EnsureOffscreenBuffer(gWinW, gWinH);
    SetRect(&winR, 0, 0, gWinW, gWinH);

    if (gHasOffscreen) {
        SetPort(&gOffscreenPort);
    } else {
        SetPort(gMainWindow);
    }

    PenNormal();
    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);
    TextMode(srcOr);

    if (!gServerConnected || gAwaitingLogin) {
        DrawPreConnectOrLoginScreen();
    } else {
        if (!gSidebarCollapsed) {
            DrawSidebar();
        }
        DrawChatArea();
        if (gProviderMenuOpen && !gSidebarCollapsed) {
            DrawProviderPopup();
        }
        if (gTierMenuOpen) {
            DrawTierPopup();
        }
        if (gSearchModalOpen) {
            DrawSearchModal();
        }
    }

    if (gAboutModalOpen) {
        DrawAboutModal();
    }

    if (gHasOffscreen) {
        SetPort(gMainWindow);
        CopyBits(&gOffscreenPort.portBits, &gMainWindow->portBits, &winR, &winR, srcCopy, NULL);
    }

    SetPort(savePort);
}

/* ============================================================================
 * Provider / Menu State Helpers & Clipboard / Retry Actions
 * ============================================================================ */

static Boolean IsProviderEnabled(const char *id)
{
    short k;
    const char *normId = id;
    if (!normId || !*normId) return false;
    if (strcmp(normId, "openai") == 0) normId = "chatgpt";

    for (k = 0; k < 3; k++) {
        if (strcmp(gProviders[k].id, normId) == 0) {
            return gProviders[k].enabled;
        }
    }
    return false;
}

static void NormalizeTierName(const char *inTier, char *outTier, short maxLen)
{
    if (!inTier || !*inTier || maxLen < 2) return;
    if (inTier[0] == 'f' || inTier[0] == 'F') {
        strncpy(outTier, "Fast", maxLen - 1);
    } else if (inTier[0] == 'p' || inTier[0] == 'P') {
        strncpy(outTier, "Pro", maxLen - 1);
    } else if (inTier[0] == 't' || inTier[0] == 'T') {
        strncpy(outTier, "Thinking", maxLen - 1);
    } else {
        strncpy(outTier, inTier, maxLen - 1);
    }
    outTier[maxLen - 1] = '\0';
}

static void SyncAIMenuState(void)
{
    MenuHandle aiMenu = GetMenuHandle(mAI);
    if (!aiMenu) return;

    if (IsProviderEnabled("gemini")) EnableItem(aiMenu, iUseGemini);
    else DisableItem(aiMenu, iUseGemini);

    if (IsProviderEnabled("chatgpt")) EnableItem(aiMenu, iUseOpenAI);
    else DisableItem(aiMenu, iUseOpenAI);

    if (IsProviderEnabled("claude")) EnableItem(aiMenu, iUseClaude);
    else DisableItem(aiMenu, iUseClaude);

    CheckItem(aiMenu, iUseGemini, (strcmp(gActiveProvider, "gemini") == 0));
    CheckItem(aiMenu, iUseOpenAI, (strcmp(gActiveProvider, "chatgpt") == 0 || strcmp(gActiveProvider, "openai") == 0));
    CheckItem(aiMenu, iUseClaude, (strcmp(gActiveProvider, "claude") == 0));

    CheckItem(aiMenu, iTierFast,     (gActiveTier[0] == 'F' || gActiveTier[0] == 'f'));
    CheckItem(aiMenu, iTierPro,      (gActiveTier[0] == 'P' || gActiveTier[0] == 'p'));
    CheckItem(aiMenu, iTierThinking, (gActiveTier[0] == 'T' || gActiveTier[0] == 't'));

    CheckItem(aiMenu, iToggleTTS, gReadRepliesTTS);
}

static void CopyMessageToClipboard(short msgIdx)
{
    if (msgIdx < 0 || msgIdx >= gMessageCount) return;
    CopyTextToClipboard(gMessages[msgIdx].text);
    gCopiedMsgIdx = msgIdx;
    gCopiedTick   = TickCount();
    RenderChatAreaOnly();
}

static void RetryAIResponseAtIndex(short aiMsgIdx)
{
    static char userPrompt[MAX_MSG_TEXT];
    short i;
    short userIdx = -1;

    if (gWaitingForAI || aiMsgIdx < 0 || aiMsgIdx >= gMessageCount) return;

    for (i = aiMsgIdx - 1; i >= 0; i--) {
        if (gMessages[i].isUser) {
            userIdx = i;
            break;
        }
    }
    if (userIdx < 0) return;

    strncpy(userPrompt, gMessages[userIdx].text, MAX_MSG_TEXT - 1);
    userPrompt[MAX_MSG_TEXT - 1] = '\0';

    /* Remove the AI response being retried (and any messages after it) */
    gMessageCount = aiMsgIdx;
    gCopiedMsgIdx = -1;
    gMsgScrollY   = 30000;

    SendPromptToAI(userPrompt, true);
}

/* ============================================================================
 * Middleware Network API Synchronization
 * ============================================================================ */

static void ParseChatsResponse(const char *buf, ChatSummary *outArr, short maxItems, short *outCount)
{
    const char *line = buf;
    short count = 0;

    while (line && *line && count < maxItems) {
        const char *next = strchr(line, '\n');
        char row[256];
        short len = next ? (short)(next - line) : (short)strlen(line);
        if (len > (short)(sizeof(row) - 1)) len = sizeof(row) - 1;
        memcpy(row, line, len);
        row[len] = '\0';
        if (len > 0 && row[len - 1] == '\r') row[len - 1] = '\0';

        if (strncmp(row, "CHAT|", 5) == 0) {
            char *p1 = row + 5;
            char *p2 = strchr(p1, '|');
            if (p2) {
                char *p3, *p4, *p5;
                *p2++ = '\0';
                p3 = strchr(p2, '|');
                if (p3) {
                    *p3++ = '\0';
                    p4 = strchr(p3, '|');
                    if (p4) {
                        *p4++ = '\0';
                        p5 = strchr(p4, '|');
                        if (p5) *p5++ = '\0';

                        strncpy(outArr[count].id, p1, sizeof(outArr[count].id) - 1);
                        strncpy(outArr[count].title, p2, sizeof(outArr[count].title) - 1);
                        strncpy(outArr[count].provider, p3, sizeof(outArr[count].provider) - 1);
                        strncpy(outArr[count].tier, p4, sizeof(outArr[count].tier) - 1);
                        strncpy(outArr[count].age, p5 ? p5 : "Today", sizeof(outArr[count].age) - 1);
                        count++;
                    }
                }
            }
        }
        line = next ? (next + 1) : NULL;
    }
    *outCount = count;
}

static void RefreshChatList(void)
{
    if (!gNetBuf || !gServerConnected) return;
    if (NetHttpGet("/api/macai/chats?limit=30", gNetBuf, NET_BUF_SIZE) > 0) {
        ParseChatsResponse(gNetBuf, gChats, MAX_CHATS, &gChatCount);
    }
}

static void RunSearchQuery(void)
{
    char path[160];
    char enc[96];

    if (!gNetBuf || !gServerConnected) return;
    UrlEncode(gSearchQuery, enc, sizeof(enc));
    sprintf(path, "/api/macai/chats?limit=8&q=%s", enc);
    if (NetHttpGet(path, gNetBuf, NET_BUF_SIZE) > 0) {
        ParseChatsResponse(gNetBuf, gSearchResults, 8, &gSearchCount);
    }
}

static void FetchProvidersAndChats(void)
{
    const char *line;

    if (!gNetBuf || !gServerConnected) return;

    if (NetHttpGet("/api/macai/providers", gNetBuf, NET_BUF_SIZE) > 0) {
        line = gNetBuf;
        while (line && *line) {
            const char *next = strchr(line, '\n');
            char row[160];
            short len = next ? (short)(next - line) : (short)strlen(line);
            if (len > (short)(sizeof(row) - 1)) len = sizeof(row) - 1;
            memcpy(row, line, len);
            row[len] = '\0';

            if (strncmp(row, "ACTIVE|", 7) == 0) {
                char *p1 = row + 7;
                char *p2 = strchr(p1, '|');
                if (p2) {
                    char *p3;
                    *p2++ = '\0';
                    p3 = strchr(p2, '|');
                    if (p3) {
                        *p3++ = '\0';
                        strncpy(gActiveProvider, p1, sizeof(gActiveProvider) - 1);
                        NormalizeTierName(p3, gActiveTier, sizeof(gActiveTier));
                    }
                }
            } else if (strncmp(row, "PROVIDER|", 9) == 0) {
                char *p1 = row + 9;
                char *p2 = strchr(p1, '|');
                if (p2) {
                    char *p3;
                    *p2++ = '\0';
                    p3 = strchr(p2, '|');
                    if (p3) {
                        short k;
                        *p3++ = '\0';
                        for (k = 0; k < 3; k++) {
                            if (strcmp(gProviders[k].id, p1) == 0) {
                                gProviders[k].enabled = (p3[0] == '1');
                            }
                        }
                    }
                }
            }
            line = next ? (next + 1) : NULL;
        }
    }

    /* Ensure active provider is actually enabled */
    if (!IsProviderEnabled(gActiveProvider)) {
        short k;
        for (k = 0; k < 3; k++) {
            if (gProviders[k].enabled) {
                strcpy(gActiveProvider, gProviders[k].id);
                break;
            }
        }
    }
    SyncAIMenuState();

    RefreshChatList();

    /* Automatically load the most recent chat if none is active yet */
    if (gActiveChatId[0] == '\0' && gChatCount > 0) {
        gSelectedChatIdx = 0;
        LoadChatById(gChats[0].id);
    }
}

static Boolean CheckServerAndLoginStatus(void)
{
    long n;
    if (!gNetBuf || !NetHasServer()) {
        gServerConnected = false;
        return false;
    }

    FormatIPAddress(NetGetServerIP(), gServerIPStr);
    n = NetHttpGet("/api/macai/ping", gNetBuf, NET_BUF_SIZE);
    if (n <= 0 || strncmp(gNetBuf, "MACAI|", 6) != 0) {
        gServerConnected = false;
        return false;
    }

    gServerConnected = true;
    SaveLastServerIP(NetGetServerIP());

    if (strncmp(gNetBuf, "MACAI|1|READY|", 14) == 0) {
        gAwaitingLogin = false;
        FetchProvidersAndChats();
        return true;
    } else {
        gAwaitingLogin = true;
        return true;
    }
}

static void AttemptDiscoveryOrSavedConnect(void)
{
    ip_addr savedIP = 0;

    gScanningServer = true;
    RenderWindow();

    if (LoadLastServerIP(&savedIP) && savedIP != 0) {
        NetSetServer(savedIP, DEFAULT_HTTP_PORT);
        FormatIPAddress(savedIP, gManualIPBuf);
        if (CheckServerAndLoginStatus()) {
            gScanningServer = false;
            RenderWindow();
            return;
        }
    } else {
        ip_addr ourIP = NetGetOurIP();
        if (ourIP != 0 && (ourIP & 0xFFFFFF00UL) != 0x0A000200UL) {
            unsigned char b0 = (unsigned char)((ourIP >> 24) & 0xFF);
            unsigned char b1 = (unsigned char)((ourIP >> 16) & 0xFF);
            unsigned char b2 = (unsigned char)((ourIP >> 8)  & 0xFF);
            sprintf(gManualIPBuf, "%u.%u.%u.", b0, b1, b2);
        }
    }

    if (NetDiscoverServer()) {
        FormatIPAddress(NetGetServerIP(), gManualIPBuf);
        CheckServerAndLoginStatus();
    } else {
        gServerConnected = false;
    }
    gScanningServer = false;
    RenderWindow();
}

static void SelectProviderOrTier(const char *provider, const char *tier)
{
    if (provider && *provider) {
        const char *normProv = (strcmp(provider, "openai") == 0) ? "chatgpt" : provider;
        if (!IsProviderEnabled(normProv)) {
            SysBeep(1);
            SyncAIMenuState();
            return;
        }
        strncpy(gActiveProvider, normProv, sizeof(gActiveProvider) - 1);
        gActiveProvider[sizeof(gActiveProvider) - 1] = '\0';
    }
    if (tier && *tier) {
        NormalizeTierName(tier, gActiveTier, sizeof(gActiveTier));
    }
    SyncAIMenuState();
    RenderWindow();
}

static void StartNewChat(void)
{
    gActiveChatId[0]    = '\0';
    gSelectedChatIdx    = -1;
    gMessageCount       = 0;
    gMsgScrollY         = 0;
    gMsgMaxScrollY      = 0;
    gCopiedMsgIdx       = -1;
    gPromptScrollLine   = 0;
    gPrevPromptVisLines = 1;
    RenderWindow();
}

static void LoadChatById(const char *chatId)
{
    static char row[MAX_MSG_TEXT + 32];
    char path[96];
    const char *line;

    if (!chatId || !*chatId || !gNetBuf || !gServerConnected) return;

    sprintf(path, "/api/macai/chat/%s", chatId);
    if (NetHttpGet(path, gNetBuf, NET_BUF_SIZE) <= 0) return;

    strncpy(gActiveChatId, chatId, sizeof(gActiveChatId) - 1);
    gMessageCount = 0;
    gCopiedMsgIdx = -1;

    line = gNetBuf;
    while (line && *line && gMessageCount < MAX_MESSAGES) {
        const char *next = strchr(line, '\n');
        short len = next ? (short)(next - line) : (short)strlen(line);
        if (len > (short)(sizeof(row) - 1)) len = sizeof(row) - 1;
        memcpy(row, line, len);
        row[len] = '\0';
        if (len > 0 && row[len - 1] == '\r') row[len - 1] = '\0';

        if (strncmp(row, "META|", 5) == 0) {
            char *p1 = row + 5;
            char *p2 = strchr(p1, '|');
            if (p2) {
                char *p3 = strchr(p2 + 1, '|');
                if (p3) {
                    char *p4 = strchr(p3 + 1, '|');
                    if (p4) {
                        *p4 = '\0';
                        if (IsProviderEnabled(p3 + 1)) {
                            strncpy(gActiveProvider, p3 + 1, sizeof(gActiveProvider) - 1);
                        }
                        NormalizeTierName(p4 + 1, gActiveTier, sizeof(gActiveTier));
                        SyncAIMenuState();
                    }
                }
            }
        } else if (strncmp(row, "MSG|user|", 9) == 0) {
            gMessages[gMessageCount].isUser = true;
            gMessages[gMessageCount].cachedMaxW = 0;
            gMessages[gMessageCount].cachedBubbleH = 0;
            strncpy(gMessages[gMessageCount].text, row + 9, MAX_MSG_TEXT - 1);
            gMessages[gMessageCount].text[MAX_MSG_TEXT - 1] = '\0';
            gMessageCount++;
        } else if (strncmp(row, "MSG|assistant|", 14) == 0) {
            gMessages[gMessageCount].isUser = false;
            gMessages[gMessageCount].cachedMaxW = 0;
            gMessages[gMessageCount].cachedBubbleH = 0;
            strncpy(gMessages[gMessageCount].text, row + 14, MAX_MSG_TEXT - 1);
            gMessages[gMessageCount].text[MAX_MSG_TEXT - 1] = '\0';
            gMessageCount++;
        }
        line = next ? (next + 1) : NULL;
    }

    /* Scroll to bottom of loaded conversation */
    gMsgScrollY = 30000;
    RenderWindow();
}

static void AppendMessage(Boolean isUser, const char *text)
{
    if (!text) return;
    if (gMessageCount >= MAX_MESSAGES) {
        memmove(&gMessages[0], &gMessages[1], sizeof(ChatMessage) * (MAX_MESSAGES - 1));
        gMessageCount = MAX_MESSAGES - 1;
    }
    gMessages[gMessageCount].isUser        = isUser;
    gMessages[gMessageCount].cachedMaxW    = 0;
    gMessages[gMessageCount].cachedLines   = 0;
    gMessages[gMessageCount].cachedWidestPx = 0;
    gMessages[gMessageCount].cachedBubbleH = 0;
    strncpy(gMessages[gMessageCount].text, text, MAX_MSG_TEXT - 1);
    gMessages[gMessageCount].text[MAX_MSG_TEXT - 1] = '\0';
    gMessageCount++;
    gMsgScrollY = 30000;
}

static void SendPromptToAI(const char *promptText, Boolean isRetry)
{
    static char encPrompt[600];
    static char path[768];
    long n;

    if (!promptText || !*promptText || !gNetBuf || !gServerConnected) return;

    if (!isRetry) {
        AppendMessage(true, promptText);
    }
    gWaitingForAI = true;
    RenderWindow();

    if (gReadRepliesTTS) {
        AudioStreamStart(22255);
    }

    UrlEncode(promptText, encPrompt, sizeof(encPrompt));
    sprintf(path, "/api/macai/ask?chat_id=%s&provider=%s&tier=%s&tts=%d&retry=%d&q=%s",
            gActiveChatId, gActiveProvider, gActiveTier, gReadRepliesTTS ? 1 : 0, isRetry ? 1 : 0, encPrompt);

    n = NetHttpGet(path, gNetBuf, NET_BUF_SIZE);
    gWaitingForAI = false;

    if (n > 0 && strncmp(gNetBuf, "REPLY|", 6) == 0) {
        char *p1 = gNetBuf + 6;
        char *p2 = strchr(p1, '|');
        if (p2) {
            char *p3, *p4, *p5;
            *p2++ = '\0';
            p3 = strchr(p2, '|');
            if (p3) {
                *p3++ = '\0';
                p4 = strchr(p3, '|');
                if (p4) {
                    *p4++ = '\0';
                    p5 = strchr(p4, '|');
                    if (p5) {
                        *p5++ = '\0';
                        strncpy(gActiveChatId, p1, sizeof(gActiveChatId) - 1);
                        if (IsProviderEnabled(p3)) {
                            strncpy(gActiveProvider, p3, sizeof(gActiveProvider) - 1);
                        }
                        NormalizeTierName(p4, gActiveTier, sizeof(gActiveTier));
                        SyncAIMenuState();
                        AppendMessage(false, p5);
                        RefreshChatList();
                    }
                }
            }
        }
    } else if (n > 0 && strncmp(gNetBuf, "ERR|", 4) == 0) {
        char *msg = strchr(gNetBuf + 4, '|');
        AppendMessage(false, msg ? (msg + 1) : (gNetBuf + 4));
    } else {
        AppendMessage(false, "Network timeout or server error.");
    }

    RenderWindow();
}

static void ToggleVoiceRecording(void)
{
    if (!gHasMicHardware) return;

    if (!gIsRecordingVoice) {
        unsigned short rateHz = 11127;
        AudioStreamStop();
        if (SoundInputStartRecording(&rateHz)) {
            gIsRecordingVoice = true;
            RenderWindow();
        }
    } else {
        Ptr pcmBuf = NULL;
        long pcmLen = 0;
        unsigned short rateHz = 11127;
        gIsRecordingVoice = false;

        if (SoundInputStopRecording(&pcmBuf, &pcmLen, &rateHz) && pcmBuf && pcmLen > 0) {
            char path[256];
            long n;

            gWaitingForAI = true;
            RenderWindow();

            /* Open TCP 6802 audio stream so middleware TTS plays back immediately */
            AudioStreamStart(22255);

            sprintf(path, "/api/macai/voice?chat_id=%s&provider=%s&tier=%s&rate=%u&tts=1",
                    gActiveChatId, gActiveProvider, gActiveTier, rateHz);
            n = NetHttpPost(path, "application/octet-stream", pcmBuf, pcmLen, gNetBuf, NET_BUF_SIZE);
            gWaitingForAI = false;

            if (n > 0 && strncmp(gNetBuf, "VOICE_REPLY|", 12) == 0) {
                /* VOICE_REPLY|<chatId>|<title>|<provider>|<tier>|<transcript>|<reply> */
                char *p1 = gNetBuf + 12;
                char *p2 = strchr(p1, '|');
                if (p2) {
                    char *p3, *p4, *p5, *p6;
                    *p2++ = '\0';
                    p3 = strchr(p2, '|');
                    if (p3) {
                        *p3++ = '\0';
                        p4 = strchr(p3, '|');
                        if (p4) {
                            *p4++ = '\0';
                            p5 = strchr(p4, '|');
                            if (p5) {
                                *p5++ = '\0';
                                p6 = strchr(p5, '|');
                                if (p6) {
                                    *p6++ = '\0';
                                    strncpy(gActiveChatId, p1, sizeof(gActiveChatId) - 1);
                                    AppendMessage(true, p5);
                                    AppendMessage(false, p6);
                                    RefreshChatList();
                                }
                            }
                        }
                    }
                }
            } else if (n > 0 && strncmp(gNetBuf, "ERR|", 4) == 0) {
                char *msg = strchr(gNetBuf + 4, '|');
                AppendMessage(false, msg ? (msg + 1) : (gNetBuf + 4));
            }
        }
        RenderWindow();
    }
}

/* ============================================================================
 * Mouse Click & Keyboard Event Handling
 * ============================================================================ */

static void HandleContentClick(Point localPt)
{
    /* 0. If About Modal is open, any click closes it */
    if (gAboutModalOpen) {
        gAboutModalOpen = false;
        RenderWindow();
        return;
    }

    /* 1. Pre-connection / Login Error Screens */
    if (!gServerConnected || gAwaitingLogin) {
        Rect cardR, btnR, ipR, connR;
        short cx;
        GetCenteredCardRect(&cardR);
        cx = (cardR.left + cardR.right) / 2;

        if (gScanningServer) {
            return;
        } else if (gServerConnected && gAwaitingLogin) {
            SetRect(&btnR, cx - 54, cardR.top + 140, cx + 54, cardR.top + 165);
            if (PtInRect(localPt, &btnR)) {
                gScanningServer = true;
                RenderWindow();
                CheckServerAndLoginStatus();
                gScanningServer = false;
                RenderWindow();
            }
        } else {
            SetRect(&btnR, cx - 54, cardR.top + 91, cx + 54, cardR.top + 116);
            SetRect(&ipR, cardR.left + 28, cardR.top + 154, cardR.left + 137, cardR.top + 179);
            SetRect(&connR, cardR.left + 141, cardR.top + 154, cardR.left + 206, cardR.top + 179);

            if (PtInRect(localPt, &btnR)) {
                AttemptDiscoveryOrSavedConnect();
            } else if (PtInRect(localPt, &ipR)) {
                gManualIPFocused = true;
                RenderWindow();
            } else if (PtInRect(localPt, &connR)) {
                ip_addr manualIP = 0;
                if (ParseIPAddress(gManualIPBuf, &manualIP)) {
                    gScanningServer = true;
                    RenderWindow();
                    NetSetServer(manualIP, DEFAULT_HTTP_PORT);
                    CheckServerAndLoginStatus();
                    gScanningServer = false;
                    RenderWindow();
                }
            }
        }
        return;
    }

    /* 2. Search Chats Modal Overlay */
    if (gSearchModalOpen) {
        Rect modalR, closeR;
        short i;
        GetSearchModalRect(&modalR);
        SetRect(&closeR, modalR.right - 26, modalR.top + 6, modalR.right - 6, modalR.top + 26);

        if (PtInRect(localPt, &closeR) || !PtInRect(localPt, &modalR)) {
            gSearchModalOpen = false;
            RenderWindow();
            return;
        }

        for (i = 0; i < gSearchCount && i < 8; i++) {
            Rect rowR;
            short ry = modalR.top + 45 + i * 16;
            SetRect(&rowR, modalR.left + 8, ry - 4, modalR.right - 8, ry + 12);
            if (PtInRect(localPt, &rowR)) {
                gSearchModalOpen = false;
                LoadChatById(gSearchResults[i].id);
                return;
            }
        }
        return;
    }

    /* 3. Open Provider Popup Menu */
    if (gProviderMenuOpen) {
        Rect popR;
        SetRect(&popR, 8, 20, 98, 20 + 3 * 16 + 4);
        gProviderMenuOpen = false;
        if (PtInRect(localPt, &popR)) {
            short idx = (localPt.v - (popR.top + 2)) / 16;
            if (idx >= 0 && idx < 3) {
                if (gProviders[idx].enabled) {
                    SelectProviderOrTier(gProviders[idx].id, NULL);
                    return;
                } else {
                    SysBeep(1);
                }
            }
        }
        RenderWindow();
        return;
    }

    /* 4. Open Model Tier Popup Menu */
    if (gTierMenuOpen) {
        Rect pillR, tierR, micR, popR;
        const char *tiers[3] = { "Fast", "Pro", "Thinking" };
        GetPromptBarGeometry(&pillR, &tierR, &micR);
        SetRect(&popR, tierR.right - 78, pillR.top - 54, tierR.right, pillR.top - 2);
        gTierMenuOpen = false;
        if (PtInRect(localPt, &popR)) {
            short idx = (localPt.v - (popR.top + 2)) / 16;
            if (idx >= 0 && idx < 3) {
                SelectProviderOrTier(NULL, tiers[idx]);
                return;
            }
        }
        RenderWindow();
        return;
    }

    /* 5. Collapsed Sidebar Top-Left Controls */
    if (gSidebarCollapsed) {
        Rect expR, srchR, newR;
        SetRect(&expR,  10, 6,  28, 20);
        SetRect(&srchR, 10, 20, 28, 34);
        SetRect(&newR,  10, 35, 28, 50);

        if (PtInRect(localPt, &expR)) {
            gSidebarCollapsed = false;
            RenderWindow();
            return;
        }
        if (PtInRect(localPt, &srchR)) {
            gSearchModalOpen = true;
            gSearchQuery[0] = '\0';
            RunSearchQuery();
            RenderWindow();
            return;
        }
        if (PtInRect(localPt, &newR)) {
            StartNewChat();
            return;
        }
    } else if (localPt.h <= SIDEBAR_W) {
        /* 6. Expanded Left Sidebar Clicks */
        Rect provBtnR, srchBtnR, colBtnR, newChatR;
        SetRect(&provBtnR, 6,   4,  98,  20);
        SetRect(&srchBtnR, 108, 4,  124, 20);
        SetRect(&colBtnR,  125, 4,  142, 20);
        SetRect(&newChatR, 6,   26, 136, 45);

        if (PtInRect(localPt, &provBtnR)) {
            gProviderMenuOpen = !gProviderMenuOpen;
            RenderWindow();
            return;
        }
        if (PtInRect(localPt, &srchBtnR)) {
            gSearchModalOpen = true;
            gSearchQuery[0] = '\0';
            RunSearchQuery();
            RenderWindow();
            return;
        }
        if (PtInRect(localPt, &colBtnR)) {
            gSidebarCollapsed = true;
            RenderWindow();
            return;
        }
        if (PtInRect(localPt, &newChatR)) {
            StartNewChat();
            return;
        }

        /* Sidebar scrollbar or chat row selection */
        if (localPt.v >= 55 && localPt.v < gWinH - 8) {
            if (localPt.h >= 129) {
                short maxVis = (gWinH - 65) / 15;
                short maxScroll;
                if (maxVis < 1) maxVis = 1;
                maxScroll = gChatCount - maxVis;
                if (maxScroll > 0) {
                    short trackTop = 55;
                    short trackBot = gWinH - 8;
                    short trackH   = trackBot - trackTop;
                    short thumbH   = (trackH * maxVis) / gChatCount;
                    short thumbY;
                    if (thumbH < 18) thumbH = 18;
                    if (thumbH > trackH) thumbH = trackH;
                    thumbY = trackTop + (short)(((long)(trackH - thumbH) * gChatScroll) / maxScroll);

                    if (localPt.v >= thumbY && localPt.v <= thumbY + thumbH) {
                        short grabOff = localPt.v - thumbY;
                        unsigned long lastTick = 0;
                        while (StillDown()) {
                            Point pt;
                            short newThumbY, newScroll;
                            GetMouse(&pt);
                            newThumbY = pt.v - grabOff;
                            if (newThumbY < trackTop) newThumbY = trackTop;
                            if (newThumbY > trackBot - thumbH) newThumbY = trackBot - thumbH;
                            newScroll = (trackH > thumbH)
                                ? (short)(((long)(newThumbY - trackTop) * maxScroll + (trackH - thumbH) / 2) / (trackH - thumbH))
                                : 0;
                            if (newScroll != gChatScroll && TickCount() != lastTick) {
                                gChatScroll = newScroll;
                                lastTick = TickCount();
                                RenderWindow();
                            }
                        }
                    } else {
                        if (localPt.v < thumbY) gChatScroll -= maxVis;
                        else gChatScroll += maxVis;
                        if (gChatScroll < 0) gChatScroll = 0;
                        if (gChatScroll > maxScroll) gChatScroll = maxScroll;
                        RenderWindow();
                    }
                }
            } else {
                short row = (localPt.v - 55) / 15;
                short idx = gChatScroll + row;
                if (idx >= 0 && idx < gChatCount) {
                    gSelectedChatIdx = idx;
                    LoadChatById(gChats[idx].id);
                }
            }
        }
        return;
    }

    /* 7. Bottom Prompt Bar, Tier Selector, Mic Button, Scrollbars, and AI Bubble Actions */
    {
        Rect pillR, tierR, micR;
        short totalLines = 1, maxScrollLine = 0;
        GetPromptBarGeometry(&pillR, &tierR, &micR);
        totalLines = WrapPromptInputLines(NULL, 32, NULL);
        maxScrollLine = totalLines - MAX_PROMPT_VIS_LINES;
        if (maxScrollLine < 0) maxScrollLine = 0;

        if (PtInRect(localPt, &tierR)) {
            gTierMenuOpen = !gTierMenuOpen;
            RenderWindow();
            return;
        }
        if (gHasMicHardware && PtInRect(localPt, &micR)) {
            ToggleVoiceRecording();
            return;
        }

        /* Prompt Input Field & Internal Scrollbar (when > 5 lines) */
        if (PtInRect(localPt, &pillR)) {
            if (maxScrollLine > 0 && localPt.h >= tierR.left - 12 && localPt.h <= tierR.left - 2) {
                short sbTop  = pillR.top + 4;
                short sbBot  = pillR.bottom - 4;
                short sbH    = sbBot - sbTop;
                short thumbH = (sbH * MAX_PROMPT_VIS_LINES) / totalLines;
                short thumbY;
                if (thumbH < 12) thumbH = 12;
                if (thumbH > sbH) thumbH = sbH;
                thumbY = sbTop + (short)(((long)(sbH - thumbH) * gPromptScrollLine) / maxScrollLine);

                if (localPt.v >= thumbY && localPt.v <= thumbY + thumbH) {
                    short grabOff = localPt.v - thumbY;
                    unsigned long lastTick = 0;
                    while (StillDown()) {
                        Point pt;
                        short newThumbY, newScroll;
                        GetMouse(&pt);
                        newThumbY = pt.v - grabOff;
                        if (newThumbY < sbTop) newThumbY = sbTop;
                        if (newThumbY > sbBot - thumbH) newThumbY = sbBot - thumbH;
                        newScroll = (sbH > thumbH)
                            ? (short)(((long)(newThumbY - sbTop) * maxScrollLine + (sbH - thumbH) / 2) / (sbH - thumbH))
                            : 0;
                        if (newScroll != gPromptScrollLine && TickCount() != lastTick) {
                            gPromptScrollLine = newScroll;
                            lastTick = TickCount();
                            RenderPromptBarOnly();
                        }
                    }
                } else {
                    if (localPt.v < thumbY) gPromptScrollLine -= 2;
                    else gPromptScrollLine += 2;
                    if (gPromptScrollLine < 0) gPromptScrollLine = 0;
                    if (gPromptScrollLine > maxScrollLine) gPromptScrollLine = maxScrollLine;
                    RenderPromptBarOnly();
                }
                return;
            }

            if (!gPromptFocused) {
                gPromptFocused = true;
                gCaretVisible  = true;
                gLastCaretTick = TickCount();
                RenderPromptBarOnly();
            }
            return;
        }

        if (gPromptFocused && gPromptBuf[0] == '\0') {
            gPromptFocused = false;
            RenderPromptBarOnly();
        }

        /* Main Chat Area Draggable Scrollbar */
        if (gMsgMaxScrollY > 0 && localPt.h >= gWinW - 14 && localPt.v >= 10 && localPt.v < pillR.top - 6) {
            short sbTop    = 10;
            short sbBot    = pillR.top - 6;
            short trackH   = sbBot - sbTop;
            short msgAreaH = pillR.top - 12;
            short totalH   = msgAreaH + gMsgMaxScrollY;
            short thumbH   = (totalH > 0) ? (short)(((long)trackH * msgAreaH) / totalH) : trackH;
            short thumbY;
            if (thumbH < 20) thumbH = 20;
            if (thumbH > trackH) thumbH = trackH;
            thumbY = sbTop + (short)(((long)(trackH - thumbH) * gMsgScrollY) / gMsgMaxScrollY);

            if (localPt.v >= thumbY && localPt.v <= thumbY + thumbH) {
                /* Smooth thumb drag throttled to VBL ticks */
                short grabOff = localPt.v - thumbY;
                unsigned long lastTick = 0;
                while (StillDown()) {
                    Point pt;
                    short newThumbY, newScrollY;
                    GetMouse(&pt);
                    newThumbY = pt.v - grabOff;
                    if (newThumbY < sbTop) newThumbY = sbTop;
                    if (newThumbY > sbBot - thumbH) newThumbY = sbBot - thumbH;
                    newScrollY = (trackH > thumbH)
                        ? (short)(((long)(newThumbY - sbTop) * gMsgMaxScrollY) / (trackH - thumbH))
                        : 0;
                    if (newScrollY < 0) newScrollY = 0;
                    if (newScrollY > gMsgMaxScrollY) newScrollY = gMsgMaxScrollY;
                    if (newScrollY != gMsgScrollY && TickCount() != lastTick) {
                        gMsgScrollY = newScrollY;
                        lastTick = TickCount();
                        RenderChatAreaOnly();
                    }
                }
            } else {
                /* Page up / down click & hold in track */
                Boolean pageUp = (localPt.v < thumbY);
                unsigned long lastStepTick = 0;
                do {
                    if ((TickCount() - lastStepTick) >= 6) {
                        lastStepTick = TickCount();
                        if (pageUp) gMsgScrollY -= 80;
                        else gMsgScrollY += 80;
                        if (gMsgScrollY < 0) gMsgScrollY = 0;
                        if (gMsgScrollY > gMsgMaxScrollY) gMsgScrollY = gMsgMaxScrollY;
                        RenderChatAreaOnly();
                    }
                } while (StillDown());
            }
            return;
        }

        /* Hit-test Copy & Retry buttons under AI response bubbles */
        if (localPt.v >= 8 && localPt.v < pillR.top - 4) {
            short chatLeft  = gSidebarCollapsed ? 0 : SIDEBAR_W;
            short chatW     = gWinW - chatLeft;
            short maxBubbleTextW = (chatW * 68) / 100;
            short curY = 14;
            short i;
            if (maxBubbleTextW < 150) maxBubbleTextW = 150;

            for (i = 0; i < gMessageCount; i++) {
                short bubbleH, drawY;
                EnsureMessageMetrics(&gMessages[i], maxBubbleTextW);
                bubbleH = gMessages[i].cachedBubbleH;
                drawY   = curY - gMsgScrollY;

                if (!gMessages[i].isUser) {
                    short bx = chatLeft + (gSidebarCollapsed ? 38 : 16);
                    short actTop = drawY + bubbleH + 1;
                    short actBot = drawY + bubbleH + 15;
                    Rect copyHitR, retryHitR;

                    SetRect(&copyHitR,  bx - 2,      actTop, bx + 42, actBot);
                    SetRect(&retryHitR, bx + 43,     actTop, bx + 94, actBot);

                    if (PtInRect(localPt, &copyHitR)) {
                        CopyMessageToClipboard(i);
                        return;
                    }
                    if (PtInRect(localPt, &retryHitR)) {
                        RetryAIResponseAtIndex(i);
                        return;
                    }
                    curY += bubbleH + 22;
                } else {
                    curY += bubbleH + 10;
                }
            }
        }
    }
}

static void HandleKeyPress(char key, short keyCode, short modifiers)
{
    (void)keyCode;

    /* 0. If About Modal is open, any key closes it */
    if (gAboutModalOpen) {
        gAboutModalOpen = false;
        RenderWindow();
        return;
    }

    /* Command-key shortcuts */
    if (modifiers & cmdKey) {
        if (key == 'q' || key == 'Q') {
            gRunning = false;
            return;
        }
        if (key == 'n' || key == 'N') {
            StartNewChat();
            return;
        }
        if (key == 'f' || key == 'F') {
            if (gServerConnected && !gAwaitingLogin) {
                gSearchModalOpen = !gSearchModalOpen;
                if (gSearchModalOpen) {
                    gSearchQuery[0] = '\0';
                    RunSearchQuery();
                }
                RenderWindow();
            }
            return;
        }
        if (key == 's' || key == 'S') {
            gSidebarCollapsed = !gSidebarCollapsed;
            RenderWindow();
            return;
        }
        if (key == 'c' || key == 'C') {
            /* Cmd+C copies the latest AI response to the Clipboard */
            short i;
            for (i = gMessageCount - 1; i >= 0; i--) {
                if (!gMessages[i].isUser) {
                    CopyMessageToClipboard(i);
                    break;
                }
            }
            return;
        }
        if (key == 'v' || key == 'V') {
            /* Cmd+V pastes text from Clipboard into prompt input */
            if (gServerConnected && !gAwaitingLogin && !gSearchModalOpen) {
                Handle hScrap = NewHandle(0);
                long offset = 0;
                long scrapLen = GetScrap(hScrap, 'TEXT', &offset);
                if (scrapLen > 0 && hScrap && *hScrap) {
                    short curLen = (short)strlen(gPromptBuf);
                    short i;
                    HLock(hScrap);
                    for (i = 0; i < scrapLen && curLen < (short)(sizeof(gPromptBuf) - 2); i++) {
                        char ch = (*hScrap)[i];
                        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
                        if (ch >= 32 && ch < 127) {
                            gPromptBuf[curLen++] = ch;
                        }
                    }
                    gPromptBuf[curLen] = '\0';
                    HUnlock(hScrap);
                    gPromptFocused = true;
                    gCaretVisible  = true;
                    gLastCaretTick = TickCount();
                    SyncPromptExpansionWithScroll();
                    RenderChatAreaOnly();
                }
                if (hScrap) DisposeHandle(hScrap);
            }
            return;
        }
        if (key == '1') { SelectProviderOrTier("gemini", NULL); return; }
        if (key == '2') { SelectProviderOrTier("chatgpt", NULL); return; }
        if (key == '3') { SelectProviderOrTier("claude", NULL); return; }
        if (key == 't' || key == 'T') {
            gReadRepliesTTS = !gReadRepliesTTS;
            SyncAIMenuState();
            return;
        }
    }

    /* Pre-connection manual IP entry */
    if (!gServerConnected) {
        short len = (short)strlen(gManualIPBuf);
        if (key == '\r' || key == 3) {
            ip_addr manualIP = 0;
            if (ParseIPAddress(gManualIPBuf, &manualIP)) {
                gScanningServer = true;
                RenderWindow();
                NetSetServer(manualIP, DEFAULT_HTTP_PORT);
                CheckServerAndLoginStatus();
                gScanningServer = false;
                RenderWindow();
            }
        } else if (key == 8 && len > 0) {
            gManualIPBuf[len - 1] = '\0';
            RenderWindow();
        } else if (((key >= '0' && key <= '9') || key == '.') && len < 20) {
            gManualIPBuf[len] = key;
            gManualIPBuf[len + 1] = '\0';
            RenderWindow();
        }
        return;
    }

    /* Search Modal typing */
    if (gSearchModalOpen) {
        short len = (short)strlen(gSearchQuery);
        if (key == 27) { /* Escape */
            gSearchModalOpen = false;
            RenderWindow();
        } else if (key == '\r' || key == 3) {
            RunSearchQuery();
            RenderWindow();
        } else if (key == 8 && len > 0) {
            gSearchQuery[len - 1] = '\0';
            RunSearchQuery();
            RenderWindow();
        } else if (key >= 32 && key < 127 && len < (short)(sizeof(gSearchQuery) - 2)) {
            gSearchQuery[len] = key;
            gSearchQuery[len + 1] = '\0';
            RunSearchQuery();
            RenderWindow();
        }
        return;
    }

    /* Arrow keys for scrolling chat messages */
    if (key == 30) { /* Up arrow */
        gMsgScrollY -= 24;
        if (gMsgScrollY < 0) gMsgScrollY = 0;
        RenderChatAreaOnly();
        return;
    }
    if (key == 31) { /* Down arrow */
        gMsgScrollY += 24;
        if (gMsgScrollY > gMsgMaxScrollY) gMsgScrollY = gMsgMaxScrollY;
        RenderChatAreaOnly();
        return;
    }

    /* Prompt input typing (fast partial redraws!) */
    if (!gAwaitingLogin) {
        short len = (short)strlen(gPromptBuf);
        if (key == '\r' || key == 3) {
            if (gIsRecordingVoice) {
                ToggleVoiceRecording();
            } else if (len > 0) {
                static char toSend[sizeof(gPromptBuf)];
                strncpy(toSend, gPromptBuf, sizeof(toSend) - 1);
                toSend[sizeof(toSend) - 1] = '\0';
                gPromptBuf[0]       = '\0';
                gPromptScrollLine   = 0;
                gPrevPromptVisLines = 1;
                gCaretVisible       = true;
                gLastCaretTick      = TickCount();
                SendPromptToAI(toSend, false);
            }
        } else if (key == 8 && len > 0) {
            short prevVis = gPrevPromptVisLines;
            gPromptFocused = true;
            gCaretVisible  = true;
            gLastCaretTick = TickCount();
            gPromptBuf[len - 1] = '\0';
            SyncPromptExpansionWithScroll();
            if (gPrevPromptVisLines != prevVis) {
                RenderChatAreaOnly();
            } else {
                RenderPromptBarOnly();
            }
        } else if (key >= 32 && key < 127 && len < (short)(sizeof(gPromptBuf) - 2)) {
            short prevVis = gPrevPromptVisLines;
            gPromptFocused = true;
            gCaretVisible  = true;
            gLastCaretTick = TickCount();
            gPromptBuf[len] = key;
            gPromptBuf[len + 1] = '\0';
            SyncPromptExpansionWithScroll();
            if (gPrevPromptVisLines != prevVis) {
                RenderChatAreaOnly();
            } else {
                RenderPromptBarOnly();
            }
        }
    }
}

static void HandleMenuChoice(long menuChoice)
{
    short menuID = HiWord(menuChoice);
    short itemID = LoWord(menuChoice);

    if (menuID == mApple) {
        if (itemID == iAbout) {
            gAboutModalOpen = true;
            RenderWindow();
        } else {
            Str255 daName;
            GetMenuItemText(GetMenuHandle(mApple), itemID, daName);
            OpenDeskAcc(daName);
        }
    } else if (menuID == mFile) {
        switch (itemID) {
            case iNewChat:
                StartNewChat();
                break;
            case iSearchChats:
                if (gServerConnected && !gAwaitingLogin) {
                    gSearchModalOpen = !gSearchModalOpen;
                    if (gSearchModalOpen) {
                        gSearchQuery[0] = '\0';
                        RunSearchQuery();
                    }
                    RenderWindow();
                }
                break;
            case iToggleSidebar:
                gSidebarCollapsed = !gSidebarCollapsed;
                RenderWindow();
                break;
            case iReconnect:
                gServerConnected = false;
                gScanningServer  = false;
                RenderWindow();
                break;
            case iQuit:
                gRunning = false;
                break;
        }
    } else if (menuID == mAI) {
        switch (itemID) {
            case iUseGemini:    SelectProviderOrTier("gemini", NULL); break;
            case iUseOpenAI:    SelectProviderOrTier("chatgpt", NULL); break;
            case iUseClaude:    SelectProviderOrTier("claude", NULL); break;
            case iTierFast:     SelectProviderOrTier(NULL, "Fast"); break;
            case iTierPro:      SelectProviderOrTier(NULL, "Pro"); break;
            case iTierThinking: SelectProviderOrTier(NULL, "Thinking"); break;
            case iToggleTTS:
                gReadRepliesTTS = !gReadRepliesTTS;
                SyncAIMenuState();
                break;
        }
    }
    HiliteMenu(0);
}

/* ============================================================================
 * Main Application Entry Point
 * ============================================================================ */

int main(void)
{
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

    gNetBuf = NewPtrClear(NET_BUF_SIZE);

    /* Set up Menus */
    menuBar = GetNewMBar(128);
    if (menuBar) {
        SetMenuBar(menuBar);
        AppendResMenu(GetMenuHandle(mApple), 'DRVR');
        SyncAIMenuState();
        DrawMenuBar();
    }

    /* Detect Sound Input Hardware (hides Mic button on compact Macs without sound input) */
    gHasMicHardware = SoundInputHasHardware();

    /* Center 512x322 resizable window on screen (fits 512x342 Compact Mac screen cleanly) */
    scrW = qd.screenBits.bounds.right - qd.screenBits.bounds.left;
    scrH = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
    winLeft = (scrW - DEFAULT_WIN_W) / 2;
    winTop  = 20 + ((scrH - 20) - DEFAULT_WIN_H) / 2;
    if (winLeft < 0) winLeft = 0;
    if (winTop < 20) winTop = 20;

    SetRect(&winBounds, winLeft, winTop, winLeft + DEFAULT_WIN_W, winTop + DEFAULT_WIN_H);
    gMainWindow = NewWindow(NULL, &winBounds, "\pMacAI", true, zoomDocProc, (WindowPtr)-1L, true, 0);
    SetPort(gMainWindow);

    EnsureOffscreenBuffer(gWinW, gWinH);

    /* Discover or reconnect to MacCompanion Middleware Server */
    AttemptDiscoveryOrSavedConnect();

    while (gRunning) {
        long sleepTicks = 6;
        SystemTask();

        if (AudioStreamIsRunning()) {
            AudioStreamPump();
            sleepTicks = 1;
        }
        if (gIsRecordingVoice) {
            sleepTicks = 1;
            if (!SoundInputIsRecording()) {
                /* Buffer filled (~4s), automatically finish and send voice clip */
                ToggleVoiceRecording();
            }
        }
        if (gCopiedMsgIdx >= 0 && (TickCount() - gCopiedTick) >= 90) {
            gCopiedMsgIdx = -1;
            if (gServerConnected && !gAwaitingLogin && !gSearchModalOpen && !gAboutModalOpen && !gTierMenuOpen && !gProviderMenuOpen) {
                RenderChatAreaOnly();
            }
        }
        if (gPromptFocused && gServerConnected && !gAwaitingLogin && !gSearchModalOpen && !gAboutModalOpen && !gTierMenuOpen && !gProviderMenuOpen) {
            unsigned long nowTicks = TickCount();
            if ((nowTicks - gLastCaretTick) >= 30) {
                gLastCaretTick = nowTicks;
                gCaretVisible = !gCaretVisible;
                RenderPromptBarOnly();
            }
        }

        if (WaitNextEvent(everyEvent, &ev, sleepTicks, NULL)) {
            switch (ev.what) {
                case mouseDown: {
                    WindowPtr hitWin = NULL;
                    short part = FindWindow(ev.where, &hitWin);
                    if (part == inMenuBar) {
                        SyncAIMenuState();
                        HandleMenuChoice(MenuSelect(ev.where));
                    } else if (part == inSysWindow) {
                        SystemClick(&ev, hitWin);
                    } else if (hitWin == gMainWindow) {
                        if (part == inDrag) {
                            DragWindow(gMainWindow, ev.where, &qd.screenBits.bounds);
                        } else if (part == inGoAway) {
                            if (TrackGoAway(gMainWindow, ev.where)) {
                                gRunning = false;
                            }
                        } else if (part == inGrow) {
                            Rect sizeLimits;
                            long newSize;
                            SetRect(&sizeLimits, MIN_WIN_W, MIN_WIN_H, MAX_WIN_W, MAX_WIN_H);
                            newSize = GrowWindow(gMainWindow, ev.where, &sizeLimits);
                            if (newSize != 0) {
                                gWinW = LoWord(newSize);
                                gWinH = HiWord(newSize);
                                SizeWindow(gMainWindow, gWinW, gWinH, true);
                                EnsureOffscreenBuffer(gWinW, gWinH);
                                RenderWindow();
                            }
                        } else if (part == inZoomIn || part == inZoomOut) {
                            if (TrackBox(gMainWindow, ev.where, part)) {
                                ZoomWindow(gMainWindow, part, false);
                                gWinW = gMainWindow->portRect.right - gMainWindow->portRect.left;
                                gWinH = gMainWindow->portRect.bottom - gMainWindow->portRect.top;
                                EnsureOffscreenBuffer(gWinW, gWinH);
                                RenderWindow();
                            }
                        } else if (part == inContent) {
                            Point localPt = ev.where;
                            SetPort(gMainWindow);
                            GlobalToLocal(&localPt);
                            HandleContentClick(localPt);
                        }
                    }
                    break;
                }
                case keyDown:
                case autoKey: {
                    char key = (char)(ev.message & charCodeMask);
                    short keyCode = (short)((ev.message & keyCodeMask) >> 8);
                    HandleKeyPress(key, keyCode, ev.modifiers);
                    break;
                }
                case updateEvt: {
                    WindowPtr updWin = (WindowPtr)ev.message;
                    if (updWin == gMainWindow) {
                        BeginUpdate(gMainWindow);
                        RenderWindow();
                        EndUpdate(gMainWindow);
                    }
                    break;
                }
            }
        }
    }

    AudioStreamStop();
    if (gOffscreenMem) DisposePtr(gOffscreenMem);
    if (gNetBuf) DisposePtr(gNetBuf);
    return 0;
}
