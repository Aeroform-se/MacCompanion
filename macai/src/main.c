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
#include <StandardFile.h>
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
#define iQuit               5

#define mSettings           130
#define mAI                 130
#define iServerConnection   1
#define iUseGemini          3
#define iUseOpenAI          4
#define iUseClaude          5
#define iUseGroq            6
#define iModel1             8
#define iModel2             9
#define iModel3             10
#define iModel4             11
#define iModelCustom        12
#define iThinkingFast       14
#define iThinkingBalanced   15
#define iThinkingThinking   16

#define mSpeech             131
#define iSpeechReadAloud    1
#define iSpeechVoiceAlloy   3
#define iSpeechVoiceEcho    4
#define iSpeechVoiceFable   5
#define iSpeechVoiceOnyx    6
#define iSpeechVoiceNova    7
#define iSpeechVoiceShimmer 8
#define iSpeechVoiceRetro   9

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
    char    imageId[36];
    short   imageW;
    short   imageH;
    short   imageRowBytes;
    Handle  imageHandle;
} ChatMessage;

typedef struct {
    char name[32];
    char id[48];
} ModelChoice;

typedef struct {
    char        id[12];
    char        name[16];
    Boolean     enabled;
    short       activeModelIdx;
    ModelChoice models[4];
    char        customModelName[48];
    Boolean     hasCustomModel;
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

/* AI Provider, Models & Thinking State */
static ProviderInfo   gProviders[4] = {
    {
        "gemini", "Gemini", false, 1, /* Default: 3.8 Flash (idx 1) */
        {
            { "3.5 Flash-Lite", "gemini-3.5-flash-lite" },
            { "3.8 Flash",      "gemini-3.8-flash" },
            { "3.1 Pro",        "gemini-3.1-pro-preview" },
            { "4 Argon",        "gemini-4-argon" }
        },
        "", false
    },
    {
        "chatgpt", "ChatGPT", false, 1, /* Default: GPT-4o (idx 1) */
        {
            { "GPT-4o Mini", "gpt-4o-mini" },
            { "GPT-4o",      "gpt-4o" },
            { "o3-mini",     "o3-mini" },
            { "o1",          "o1" }
        },
        "", false
    },
    {
        "claude", "Claude", false, 1, /* Default: Claude 3.7 Sonnet (idx 1) */
        {
            { "Claude 3.5 Haiku",  "claude-3-5-haiku-latest" },
            { "Claude 3.7 Sonnet", "claude-3-7-sonnet-latest" },
            { "Claude 3.5 Sonnet", "claude-3-5-sonnet-latest" },
            { "Claude 3 Opus",     "claude-3-opus-latest" }
        },
        "", false
    },
    {
        "groq", "Groq", false, 1, /* Default: Llama 3.3 70B (idx 1) */
        {
            { "Llama 3.1 8B",  "llama-3.1-8b-instant" },
            { "Llama 3.3 70B", "llama-3.3-70b-versatile" },
            { "GPT-OSS 120B",  "openai/gpt-oss-120b" },
            { "Qwen 3.8 27B",  "qwen/qwen3.8-27b" }
        },
        "", false
    }
};
static char           gActiveProvider[12] = "gemini";
static char           gActiveTier[12]     = "Balanced";
static const char    *gThinkingLevels[3]  = { "Fast", "Balanced", "Thinking" };
static short          gActiveThinkingIdx  = 1; /* Default: Balanced */
static const char    *gTtsVoiceNames[7]   = { "alloy", "echo", "fable", "onyx", "nova", "shimmer", "retro" };
static short          gActiveVoiceIdx     = 0; /* Default: alloy (idx 0) */
static char           gWaitStatusText[96] = "Thinking...";
static unsigned long  gWaitStartTick      = 0;
static Boolean        gUserCancelledWait  = false;
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
static short          gSaveSuccessMsgIdx = -1;
static unsigned long  gSaveSuccessTick   = 0;

/* Bottom Prompt Input State (Multi-line auto-expanding up to 5 lines) */
static char           gPromptBuf[1024]    = "";
static Boolean        gPromptFocused      = false;
static Boolean        gShowPlaceholder    = true;
static short          gCursorPos          = 0;
static short          gPromptSelAnchor    = 0;
static short          gPromptLineStarts[32];
static Boolean        gCaretVisible       = true;
static unsigned long  gLastCaretTick      = 0;
static short          gPromptScrollLine   = 0;
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

/* Custom 1-bit QuickDraw Patterns matching ui/MacAI v2 PNGs */
static const Pattern kPatLightDither = { { 0x00, 0x44, 0x00, 0x11, 0x00, 0x44, 0x00, 0x11 } };
static const Pattern kPatGrid50      = { { 0xAA, 0x00, 0xAA, 0x00, 0xAA, 0x00, 0xAA, 0x00 } };
static const Pattern kPatModalDim    = { { 0xAA, 0x00, 0x55, 0x00, 0xAA, 0x00, 0x55, 0x00 } };

/* Mouse Hover & Speech Bubble Inline Edit State */
static short          gHoverBubbleIdx     = -1;
static short          gEditingMsgIdx      = -1;
static char           gEditBuf[1024]      = "";
static short          gEditBufLen         = 0;
static short          gEditCursorPos      = 0;
static short          gEditSelAnchor      = 0;
static short          gEditLineStarts[MAX_WRAPPED_LINES];

/* Forward Declarations */
static void SafeCopyString(char *dest, const char *src, short destSize);
static void RenderWindow(void);
static void RenderChatAreaOnly(void);
static void RenderPromptBarOnly(void);
static void SyncAIMenuState(void);
static void SyncSpeechMenuState(void);
static Boolean ActiveModelSupportsThinking(void);
static void SelectVoice(short vIdx);
static Boolean IsProviderEnabled(const char *id);
static void FetchProvidersAndChats(void);
static void LoadChatById(const char *chatId);
static void SendPromptToAI(const char *promptText, Boolean isRetry);
static short HitTestMessageBubble(Point pt);
static void SubmitEditedMessage(short editIdx);

/* ============================================================================
 * Preferences File ("MacAI Prefs" in System Folder:Preferences)
 * ============================================================================ */

#define MACAI_PREFS_MAGIC     0x4D434131UL /* 'MCA1' */
#define SPOTIMAC_PREFS_MAGIC  0x4D435331UL /* 'MCS1' */

typedef struct {
    unsigned long magic;
    ip_addr       serverIp;
    char          activeProvider[12];
    short         activeThinkingIdx;
    short         activeModelIdx[4];
    short         activeVoiceIdx;
    short         readRepliesTTS;
    char          reserved[30];
} MacAIPrefs;

static Boolean gUserHasSavedPrefs = false;
static ip_addr gLastSavedServerIP = 0;

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

static void SaveMacAIPrefs(void)
{
    short refNum = 0;
    long count = sizeof(MacAIPrefs);
    FSSpec spec;
    MacAIPrefs prefs;
    ip_addr ip = NetGetServerIP();
    short k;
    OSErr err;

    if (ip != 0) {
        gLastSavedServerIP = ip;
    } else {
        ip = gLastSavedServerIP;
    }
    if (ip == 0) return;

    memset(&prefs, 0, sizeof(prefs));
    prefs.magic = MACAI_PREFS_MAGIC;
    prefs.serverIp = ip;
    SafeCopyString(prefs.activeProvider, gActiveProvider, sizeof(prefs.activeProvider));
    prefs.activeThinkingIdx = gActiveThinkingIdx;
    for (k = 0; k < 4; k++) {
        prefs.activeModelIdx[k] = gProviders[k].activeModelIdx;
    }
    prefs.activeVoiceIdx = gActiveVoiceIdx;
    prefs.readRepliesTTS = gReadRepliesTTS ? 1 : 0;

    if (GetPrefsFileSpec(&spec, false)) {
        FSpCreate(&spec, 'MCAI', 'pref', smSystemScript);
        err = FSpOpenDF(&spec, fsWrPerm, &refNum);
        if (err == noErr) {
            SetEOF(refNum, 0);
            FSWrite(refNum, &count, &prefs);
            FSClose(refNum);
            gUserHasSavedPrefs = true;
        }
    }
}

static void SaveLastServerIP(ip_addr ip)
{
    if (ip != 0) gLastSavedServerIP = ip;
    SaveMacAIPrefs();
}

static Boolean LoadMacAIPrefs(ip_addr *outIP)
{
    short refNum = 0;
    long count;
    FSSpec spec;
    MacAIPrefs prefs;
    short attempt;
    short k;
    OSErr err;

    if (outIP) *outIP = 0;

    /* Remove any legacy "MacAI Prefs" file sitting in the application folder from v0.1.0 */
    FSDelete("\pMacAI Prefs", 0);

    for (attempt = 0; attempt < 2; attempt++) {
        if (GetPrefsFileSpec(&spec, attempt == 1)) {
            err = FSpOpenDF(&spec, fsRdPerm, &refNum);
            if (err == noErr) {
                count = sizeof(MacAIPrefs);
                memset(&prefs, 0, sizeof(prefs));
                err = FSRead(refNum, &count, &prefs);
                FSClose(refNum);
                if ((err == noErr || err == eofErr) &&
                    count >= 8 &&
                    (prefs.magic == MACAI_PREFS_MAGIC || prefs.magic == SPOTIMAC_PREFS_MAGIC) &&
                    prefs.serverIp != 0) {
                    if (outIP) *outIP = prefs.serverIp;
                    gLastSavedServerIP = prefs.serverIp;

                    if (count >= 28 && prefs.activeProvider[0] != '\0') {
                        if (strcmp(prefs.activeProvider, "gemini") == 0 ||
                            strcmp(prefs.activeProvider, "chatgpt") == 0 ||
                            strcmp(prefs.activeProvider, "claude") == 0 ||
                            strcmp(prefs.activeProvider, "groq") == 0) {
                            SafeCopyString(gActiveProvider, prefs.activeProvider, sizeof(gActiveProvider));
                        }
                        if (prefs.activeThinkingIdx >= 0 && prefs.activeThinkingIdx < 3) {
                            gActiveThinkingIdx = prefs.activeThinkingIdx;
                            SafeCopyString(gActiveTier, gThinkingLevels[gActiveThinkingIdx], sizeof(gActiveTier));
                        }
                        for (k = 0; k < 4; k++) {
                            if (prefs.activeModelIdx[k] >= 0 && prefs.activeModelIdx[k] <= 4) {
                                gProviders[k].activeModelIdx = prefs.activeModelIdx[k];
                            }
                        }
                        if (prefs.activeVoiceIdx >= 0 && prefs.activeVoiceIdx < 7) {
                            gActiveVoiceIdx = prefs.activeVoiceIdx;
                        }
                        if (prefs.readRepliesTTS >= 0 && prefs.readRepliesTTS <= 1) {
                            gReadRepliesTTS = (prefs.readRepliesTTS == 1);
                        }
                        gUserHasSavedPrefs = true;
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

static Boolean LoadLastServerIP(ip_addr *outIP)
{
    return LoadMacAIPrefs(outIP);
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

static void SafeCopyString(char *dest, const char *src, short destSize)
{
    short i = 0;
    if (!dest || destSize <= 0) return;
    if (src) {
        while (src[i] != '\0' && i < destSize - 1) {
            dest[i] = src[i];
            i++;
        }
    }
    dest[i] = '\0';
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
    if (strcmp(id, "groq") == 0) return "Groq";
    return "Gemini";
}

static const char *GetTierDisplayName(const char *tier)
{
    if (!tier) return "Balanced";
    if (tier[0] == 'f' || tier[0] == 'F' || tier[0] == 'l' || tier[0] == 'L') return "Fast";
    if (tier[0] == 't' || tier[0] == 'T' || tier[0] == 'h' || tier[0] == 'H') return "Thinking";
    return "Balanced";
}

/* ============================================================================
 * Fast Monospace Word Wrapping & Cached Bubble Metrics
 * ============================================================================ */

static short WrapMessageLinesEx(const char *text, short maxWidthPx,
                                char lines[][96], short lineStarts[], short maxLines)
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

        if (lineStarts) lineStarts[lineCount] = (short)(p - text);

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
                if (lineStarts) lineStarts[lineCount] = (short)(p - text);
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

static short WrapMessageLines(const char *text, short maxWidthPx,
                              char lines[][96], short maxLines)
{
    return WrapMessageLinesEx(text, maxWidthPx, lines, NULL, maxLines);
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

    if (msg->imageH > 0) {
        if (msg->imageW > widestPx) widestPx = msg->imageW;
        if (widestPx < 180) widestPx = 180;
        msg->cachedBubbleH = (nLines > 0 ? (nLines * 12 + 10) : 6) + msg->imageH + 6 + 18 + 8;
    } else {
        msg->cachedBubbleH = nLines * 12 + 12;
    }

    msg->cachedMaxW     = maxW;
    msg->cachedLines    = nLines;
    msg->cachedWidestPx = widestPx;
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
    short maxPromptPx = GetPromptTextMaxWidthPx();
    short totalLines;
    short visLines;

    if (gIsRecordingVoice) {
        if (outLines) strcpy(outLines[0], "Listening... (click mic)");
        if (outVisLines) *outVisLines = 1;
        gPromptLineStarts[0] = 0;
        return 1;
    }
    if (gShowPlaceholder && !gPromptFocused && gPromptBuf[0] == '\0') {
        if (outLines) strcpy(outLines[0], "Ask MacAI");
        if (outVisLines) *outVisLines = 1;
        gPromptLineStarts[0] = 0;
        return 1;
    }

    if (gPromptBuf[0] == '\0') {
        if (outLines) outLines[0][0] = '\0';
        if (outVisLines) *outVisLines = 1;
        gPromptLineStarts[0] = 0;
        return 1;
    }

    totalLines = WrapMessageLinesEx(gPromptBuf, maxPromptPx, outLines, gPromptLineStarts, maxLines);
    if (totalLines < 1) totalLines = 1;
    visLines = (totalLines > MAX_PROMPT_VIS_LINES) ? MAX_PROMPT_VIS_LINES : totalLines;
    if (outVisLines) *outVisLines = visLines;
    return totalLines;
}

static void GetPromptCaretPos(short *outLine, short *outColPx, char promptLines[][96], short totalLines)
{
    short i;
    short targetLine = 0;
    short colChars = 0;
    char tmp[96];

    if (gCursorPos < 0) gCursorPos = 0;
    if (gCursorPos > (short)strlen(gPromptBuf)) gCursorPos = (short)strlen(gPromptBuf);

    for (i = 0; i < totalLines; i++) {
        short startIdx = gPromptLineStarts[i];
        short lineLen = (short)strlen(promptLines[i]);
        if (gCursorPos >= startIdx && (gCursorPos <= startIdx + lineLen || i == totalLines - 1)) {
            targetLine = i;
            colChars = gCursorPos - startIdx;
            if (colChars < 0) colChars = 0;
            if (colChars > lineLen) colChars = lineLen;
            break;
        }
        targetLine = i;
    }

    *outLine = targetLine;
    if (colChars > 0 && promptLines[targetLine][0] != '\0') {
        strncpy(tmp, promptLines[targetLine], colChars);
        tmp[colChars] = '\0';
        *outColPx = MeasureCString(tmp);
    } else {
        *outColPx = 0;
    }
}

static void GetPromptSelection(short *outSelStart, short *outSelEnd)
{
    short len = (short)strlen(gPromptBuf);
    short s = gPromptSelAnchor;
    short e = gCursorPos;
    if (s < 0) s = 0;
    if (s > len) s = len;
    if (e < 0) e = 0;
    if (e > len) e = len;
    if (s <= e) {
        *outSelStart = s;
        *outSelEnd   = e;
    } else {
        *outSelStart = e;
        *outSelEnd   = s;
    }
}

static Boolean DeletePromptSelection(void)
{
    short selStart, selEnd, curLen;
    GetPromptSelection(&selStart, &selEnd);
    if (selStart >= selEnd) return false;

    curLen = (short)strlen(gPromptBuf);
    memmove(&gPromptBuf[selStart], &gPromptBuf[selEnd], curLen - selEnd + 1);
    gCursorPos       = selStart;
    gPromptSelAnchor = selStart;
    return true;
}

static short GetPromptCharPosAtPoint(Point pt, const Rect *pillR)
{
    static char pLines[32][96];
    short visLines = 1;
    short totalL = WrapPromptInputLines(pLines, 32, &visLines);
    short clickRelY = pt.v - (pillR->top + 4);
    short clickLine = clickRelY / 12;
    short targetLine, lineLen, relX, col, pos;

    if (gPromptBuf[0] == '\0') return 0;

    if (clickLine < 0) clickLine = 0;
    if (clickLine >= visLines) clickLine = visLines - 1;
    targetLine = gPromptScrollLine + clickLine;
    if (targetLine < 0) targetLine = 0;
    if (targetLine >= totalL) targetLine = totalL - 1;

    lineLen = (short)strlen(pLines[targetLine]);
    relX = pt.h - (pillR->left + 12);
    if (relX <= 0) {
        col = 0;
    } else {
        col = (relX + (MONACO_CHAR_W / 2)) / MONACO_CHAR_W;
        if (col > lineLen) col = lineLen;
    }

    pos = gPromptLineStarts[targetLine] + col;
    if (pos < 0) pos = 0;
    if (pos > (short)strlen(gPromptBuf)) pos = (short)strlen(gPromptBuf);
    return pos;
}

static void GetEditSelection(short *outSelStart, short *outSelEnd)
{
    short len = gEditBufLen;
    short s = gEditSelAnchor;
    short e = gEditCursorPos;
    if (s < 0) s = 0;
    if (s > len) s = len;
    if (e < 0) e = 0;
    if (e > len) e = len;
    if (s <= e) {
        *outSelStart = s;
        *outSelEnd   = e;
    } else {
        *outSelStart = e;
        *outSelEnd   = s;
    }
}

static Boolean DeleteEditSelection(void)
{
    short selStart, selEnd;
    GetEditSelection(&selStart, &selEnd);
    if (selStart >= selEnd) return false;

    memmove(&gEditBuf[selStart], &gEditBuf[selEnd], gEditBufLen - selEnd + 1);
    gEditBufLen    -= (selEnd - selStart);
    gEditCursorPos  = selStart;
    gEditSelAnchor  = selStart;
    return true;
}

static short GetEditCharPosAtPoint(Point pt, const Rect *bubbleR, short curY, short nLines, char wrapped[][96])
{
    short clickRelY = pt.v - (curY + 4);
    short clickLine = clickRelY / 12;
    short lineLen, relX, col, pos;

    if (gEditBufLen == 0) return 0;

    if (clickLine < 0) clickLine = 0;
    if (clickLine >= nLines) clickLine = nLines - 1;
    lineLen = (short)strlen(wrapped[clickLine]);

    relX = pt.h - (bubbleR->left + 10);
    if (relX <= 0) col = 0;
    else {
        col = (relX + (MONACO_CHAR_W / 2)) / MONACO_CHAR_W;
        if (col > lineLen) col = lineLen;
    }

    pos = gEditLineStarts[clickLine] + col;
    if (pos < 0) pos = 0;
    if (pos > gEditBufLen) pos = gEditBufLen;
    return pos;
}

static void EnsureCaretVisible(void)
{
    static char lines[32][96];
    short visLines = 1;
    short totalLines = WrapPromptInputLines(lines, 32, &visLines);
    short cLine, colPx;

    GetPromptCaretPos(&cLine, &colPx, lines, totalLines);
    if (cLine < gPromptScrollLine) {
        gPromptScrollLine = cLine;
    } else if (cLine >= gPromptScrollLine + visLines) {
        gPromptScrollLine = cLine - visLines + 1;
    }
    if (gPromptScrollLine > totalLines - visLines) {
        gPromptScrollLine = totalLines - visLines;
    }
    if (gPromptScrollLine < 0) gPromptScrollLine = 0;
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

    /* Keep prompt scroll pinned to bottom when typing */
    if (totalLines > MAX_PROMPT_VIS_LINES) {
        gPromptScrollLine = totalLines - MAX_PROMPT_VIS_LINES;
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

    /* 50% dither shadow curving up sides, NO solid black outline (input-style-shadow-example.png) */
    {
        Rect pillShadow = *pillR;
        OffsetRect(&pillShadow, 0, 1);
        FillRoundRect(&pillShadow, 20, 20, &qd.gray);
        EraseRoundRect(pillR, 20, 20);
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

    /* Caret indicator or selection highlight */
    if (gPromptFocused && gEditingMsgIdx < 0) {
        short selStart, selEnd;
        GetPromptSelection(&selStart, &selEnd);

        if (selStart < selEnd) {
            for (i = 0; i < visLines; i++) {
                short lineIdx = gPromptScrollLine + i;
                if (lineIdx >= 0 && lineIdx < totalLines) {
                    short startIdx = gPromptLineStarts[lineIdx];
                    short lineLen  = (short)strlen(promptLines[lineIdx]);
                    short endIdx   = startIdx + lineLen;
                    short s0 = (selStart > startIdx) ? selStart : startIdx;
                    short s1 = (selEnd   < endIdx)   ? selEnd   : endIdx;

                    if (s1 > s0) {
                        short col0 = s0 - startIdx;
                        short col1 = s1 - startIdx;
                        Rect selR;
                        SetRect(&selR, pillR->left + 12 + col0 * MONACO_CHAR_W,
                                       pillR->top + 4 + i * 12,
                                       pillR->left + 12 + col1 * MONACO_CHAR_W,
                                       pillR->top + 16 + i * 12);
                        InvertRect(&selR);
                    }
                }
            }
        } else if (gCaretVisible) {
            short cLine, colPx;
            GetPromptCaretPos(&cLine, &colPx, promptLines, totalLines);
            if (cLine >= gPromptScrollLine && cLine < gPromptScrollLine + visLines) {
                short vIdx = cLine - gPromptScrollLine;
                short caretX = pillR->left + 12 + colPx;
                short caretY = pillR->top + 14 + vIdx * 12;
                MoveTo(caretX, caretY - 9);
                LineTo(caretX, caretY + 1);
            }
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
        if (ActiveModelSupportsThinking()) {
            const char *tierName = GetTierDisplayName(gActiveTier);
            short tw = MeasureCString(tierName);
            MoveTo(tierR->left + 4, pillR->bottom - 8);
            DrawCString(tierName);
            DrawChevronDown(tierR->left + 4 + tw + 5, pillR->bottom - 12);
        } else {
            /* Dimmed / greyed out indicator when thinking levels are not supported */
            MoveTo(tierR->left + 4, pillR->bottom - 8);
            PenPat(&qd.gray);
            PenMode(patBic);
            DrawCString("—");
            PenNormal();
        }
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

    /* Vertical 1px 50% dotted column + 1px solid white column matching sidebar-right-border.png */
    PenNormal();
    {
        Rect rDot, rWhite;
        SetRect(&rDot, SIDEBAR_W, 0, SIDEBAR_W + 1, gWinH);
        FillRect(&rDot, &qd.gray);
        SetRect(&rWhite, SIDEBAR_W + 1, 0, SIDEBAR_W + 2, gWinH);
        FillRect(&rWhite, &qd.white);
    }

    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);

    /* Top Left: Active AI Provider ("Gemini v") */
    MoveTo(12, 15);
    DrawCString(provName);
    provW = MeasureCString(provName);
    DrawChevronDown(12 + provW + 6, 11);

    /* Top Right of Sidebar: Search icon (114, 6) and Collapse Sidebar icon (127, 6) */
    BlitBitmap(kIconSearch12x12,   2, 12, 12, 114, 6, srcOr);
    BlitBitmap(kIconCollapse12x12, 2, 12, 12, 127, 6, srcOr);

    /* "New chat" button row */
    BlitBitmap(kIconNewChat12x12, 2, 12, 12, 11, 29, srcOr);
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

        Boolean isCurrentChat = (gActiveChatId[0] != '\0' && strcmp(gChats[idx].id, gActiveChatId) == 0);
        if (isCurrentChat) {
            Rect selR;
            SetRect(&selR, 5, rowY - 1, SIDEBAR_W - 5, rowY + 14);
            FillRoundRect(&selR, 12, 12, &kPatGrid50);

            /* 1px white halo around text using srcBic */
            TextMode(srcBic);
            MoveTo(10, rowY + 10); DrawCStringTruncated(gChats[idx].title, 122);
            MoveTo(12, rowY + 10); DrawCStringTruncated(gChats[idx].title, 122);
            MoveTo(11, rowY + 9);  DrawCStringTruncated(gChats[idx].title, 122);
            MoveTo(11, rowY + 11); DrawCStringTruncated(gChats[idx].title, 122);
            MoveTo(10, rowY + 9);  DrawCStringTruncated(gChats[idx].title, 122);
            MoveTo(12, rowY + 9);  DrawCStringTruncated(gChats[idx].title, 122);
            MoveTo(10, rowY + 11); DrawCStringTruncated(gChats[idx].title, 122);
            MoveTo(12, rowY + 11); DrawCStringTruncated(gChats[idx].title, 122);
            TextMode(srcOr);
        }

        MoveTo(11, rowY + 10);
        DrawCStringTruncated(gChats[idx].title, 122);
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
    /* 3 individual 12x12 rounded white badges matching Collapsed-icon-bg.png */
    Rect b0, b1, b2;
    SetRect(&b0, 12, 7, 24, 19);
    EraseRoundRect(&b0, 4, 4);
    BlitBitmap(kIconExtend12x12, 2, 12, 12, 12, 7, srcOr);

    SetRect(&b1, 12, 22, 24, 34);
    EraseRoundRect(&b1, 4, 4);
    BlitBitmap(kIconSearch12x12, 2, 12, 12, 12, 22, srcOr);

    SetRect(&b2, 12, 37, 24, 49);
    EraseRoundRect(&b2, 4, 4);
    BlitBitmap(kIconNewChat12x12, 2, 12, 12, 12, 37, srcOr);
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
        short bubbleH, stepH;
        if (gEditingMsgIdx == i) {
            short nLines = WrapMessageLines(gEditBuf, maxW, wrapped, MAX_WRAPPED_LINES);
            bubbleH = nLines * 12 + 14;
            if (bubbleH < 24) bubbleH = 24;
            stepH = bubbleH + 24;
        } else {
            EnsureMessageMetrics(&gMessages[i], maxW);
            bubbleH = gMessages[i].cachedBubbleH;
            stepH = bubbleH + (gMessages[i].isUser ? 18 : 22);
        }
        totalContentH += stepH;
    }
    if (gWaitingForAI) {
        short nWaitLines = WrapMessageLines(gWaitStatusText, aiMaxW - 24, NULL, 8);
        short waitH = nWaitLines * 12 + 14;
        if (waitH < 24) waitH = 24;
        totalContentH += waitH + 10;
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
        const char *welcome = "Let's dive in!";
        Rect badgeR;
        short tw = MeasureCString(welcome);
        SetRect(&badgeR,
                chatLeft + (chatWidth - tw) / 2 - 10,
                (msgAreaBot / 2) - 12,
                chatLeft + (chatWidth + tw) / 2 + 10,
                (msgAreaBot / 2) + 10);
        EraseRoundRect(&badgeR, 12, 12);
        MoveTo(badgeR.left + 6, badgeR.bottom);
        LineTo(badgeR.right - 6, badgeR.bottom);
        MoveTo(badgeR.left + 10, badgeR.top + 14);
        DrawCString(welcome);
    }

    curY = msgAreaTop - gMsgScrollY;

    /* Pass 2: Render ONLY visible message bubbles */
    for (i = 0; i < gMessageCount; i++) {
        short maxW      = gMessages[i].isUser ? userMaxW : aiMaxW;
        short bubbleH, stepH, widestLine;
        Boolean isEditingThis = (gEditingMsgIdx == i);

        if (isEditingThis) {
            short nLines = WrapMessageLinesEx(gEditBuf, maxW, wrapped, gEditLineStarts, MAX_WRAPPED_LINES);
            short L;
            widestLine = 0;
            for (L = 0; L < nLines; L++) {
                short w = MeasureCString(wrapped[L]);
                if (w > widestLine) widestLine = w;
            }
            bubbleH = nLines * 12 + 14;
            if (bubbleH < 24) bubbleH = 24;
            stepH = bubbleH + 24;

            if (curY + stepH >= 0 && curY <= msgAreaBot) {
                short bw = widestLine + 20;
                short rightEdge = gWinW - 25;
                Rect bubbleR, cancelR, saveR;
                short selStart, selEnd;
                if (bw < 120) bw = 120;
                SetRect(&bubbleR, rightEdge - bw, curY, rightEdge, curY + bubbleH);
                EraseRoundRect(&bubbleR, 16, 16);

                for (L = 0; L < nLines; L++) {
                    MoveTo(bubbleR.left + 10, curY + 14 + L * 12);
                    DrawCString(wrapped[L]);
                }

                GetEditSelection(&selStart, &selEnd);

                if (selStart < selEnd) {
                    /* Highlight selected characters */
                    for (L = 0; L < nLines; L++) {
                        short startIdx = gEditLineStarts[L];
                        short lineLen  = (short)strlen(wrapped[L]);
                        short endIdx   = startIdx + lineLen;
                        short s0 = (selStart > startIdx) ? selStart : startIdx;
                        short s1 = (selEnd   < endIdx)   ? selEnd   : endIdx;

                        if (s1 > s0) {
                            short col0 = s0 - startIdx;
                            short col1 = s1 - startIdx;
                            Rect selR;
                            SetRect(&selR, bubbleR.left + 10 + col0 * MONACO_CHAR_W,
                                           curY + 4 + L * 12,
                                           bubbleR.left + 10 + col1 * MONACO_CHAR_W,
                                           curY + 16 + L * 12);
                            InvertRect(&selR);
                        }
                    }
                } else if (gCaretVisible && nLines > 0) {
                    /* Blinking caret inside the edit bubble at gEditCursorPos */
                    short cLine = 0, cCol = 0;
                    for (L = 0; L < nLines; L++) {
                        short sIdx = gEditLineStarts[L];
                        short lLen = (short)strlen(wrapped[L]);
                        if (gEditCursorPos >= sIdx && (gEditCursorPos <= sIdx + lLen || L == nLines - 1)) {
                            cLine = L;
                            cCol  = gEditCursorPos - sIdx;
                            if (cCol > lLen) cCol = lLen;
                            break;
                        }
                    }
                    {
                        short cX = bubbleR.left + 10 + cCol * MONACO_CHAR_W;
                        short cY = curY + 14 + cLine * 12;
                        MoveTo(cX, cY - 9);
                        LineTo(cX, cY + 1);
                    }
                }

                /* [Cancel] and [Save] buttons beneath the editable bubble */
                SetRect(&cancelR, bubbleR.right - 92, curY + bubbleH + 3, bubbleR.right - 48, curY + bubbleH + 17);
                EraseRoundRect(&cancelR, 6, 6);
                FrameRoundRect(&cancelR, 6, 6);
                MoveTo(cancelR.left + 5, cancelR.top + 10);
                DrawCString("Cancel");

                SetRect(&saveR, bubbleR.right - 44, curY + bubbleH + 3, bubbleR.right, curY + bubbleH + 17);
                FillRoundRect(&saveR, 6, 6, &qd.black);
                TextMode(srcBic);
                MoveTo(saveR.left + 10, saveR.top + 10);
                DrawCString("Save");
                TextMode(srcOr);
            }
        } else {
            EnsureMessageMetrics(&gMessages[i], maxW);
            bubbleH = gMessages[i].cachedBubbleH;
            widestLine = gMessages[i].cachedWidestPx;
            stepH = bubbleH + (gMessages[i].isUser ? 18 : 22);

            if (curY + stepH >= 0 && curY <= msgAreaBot) {
                short nLines = WrapMessageLines(gMessages[i].text, maxW, wrapped, MAX_WRAPPED_LINES);
                Rect bubbleR;

                if (gMessages[i].isUser) {
                    /* Right-aligned solid white pill bubble, NO outline, NO shadow (Main_Normal.png) */
                    short bw = widestLine + 20;
                    short rightEdge = gWinW - 25;
                    if (bw < 40) bw = 40;
                    SetRect(&bubbleR, rightEdge - bw, curY, rightEdge, curY + bubbleH);
                    EraseRoundRect(&bubbleR, 16, 16);

                    for (L = 0; L < nLines; L++) {
                        MoveTo(bubbleR.left + 10, curY + 14 + L * 12);
                        DrawCString(wrapped[L]);
                    }

                    /* Edit and Copy action icons on hover with pill background (retry-bg-example.png) */
                    if (gHoverBubbleIdx == i) {
                        short actionY = curY + bubbleH + 3;
                        Rect actPill;
                        const unsigned char *cIcon = (gCopiedMsgIdx == i && (TickCount() - gCopiedTick) < 120)
                            ? kIconCopyOk12x12 : kIconCopy12x12;
                        SetRect(&actPill, bubbleR.right - 31, actionY - 1, bubbleR.right + 1, actionY + 13);
                        EraseRoundRect(&actPill, 8, 8);
                        BlitBitmap(kIconEdit12x12, 2, 12, 12, bubbleR.right - 28, actionY, srcOr);
                        BlitBitmap(cIcon,          2, 12, 12, bubbleR.right - 14, actionY, srcOr);
                    }
                } else {
                    /* Left-aligned solid white bubble with curved 1px black shadow (bubble-shadow-example.png) */
                    short bw = widestLine + 24;
                    short actionY = curY + bubbleH + 4;
                    if (bw < 100) bw = 100;
                    SetRect(&bubbleR, aiLeftX, curY, aiLeftX + bw, curY + bubbleH);
                    {
                        Rect shadowR = bubbleR;
                        OffsetRect(&shadowR, 0, 1);
                        FillRoundRect(&shadowR, 16, 16, &qd.black);
                        EraseRoundRect(&bubbleR, 16, 16);
                    }

                    for (L = 0; L < nLines; L++) {
                        MoveTo(bubbleR.left + 12, curY + 14 + L * 12);
                        DrawCString(wrapped[L]);
                    }

                    if (gMessages[i].imageHandle && *(gMessages[i].imageHandle) && gMessages[i].imageH > 0) {
                        short imgX = bubbleR.left + 12;
                        short imgY = curY + (nLines > 0 ? (14 + nLines * 12 + 4) : 10);
                        BitMap bm;
                        Rect srcR, dstR, copyBtnR, saveBtnR;
                        SetRect(&srcR, 0, 0, gMessages[i].imageW, gMessages[i].imageH);
                        SetRect(&dstR, imgX, imgY, imgX + gMessages[i].imageW, imgY + gMessages[i].imageH);
                        bm.baseAddr = *(gMessages[i].imageHandle);
                        bm.rowBytes = gMessages[i].imageRowBytes;
                        bm.bounds   = srcR;
                        CopyBits(&bm, &qd.thePort->portBits, &srcR, &dstR, srcCopy, NULL);
                        FrameRect(&dstR);

                        short btnY = imgY + gMessages[i].imageH + 6;
                        SetRect(&copyBtnR, imgX, btnY, imgX + 54, btnY + 16);
                        EraseRoundRect(&copyBtnR, 6, 6);
                        FrameRoundRect(&copyBtnR, 6, 6);
                        MoveTo(copyBtnR.left + 8, copyBtnR.top + 11);
                        DrawCString((gCopiedMsgIdx == i && (TickCount() - gCopiedTick) < 120) ? "Copied" : "Copy");

                        SetRect(&saveBtnR, imgX + 60, btnY, imgX + 192, btnY + 16);
                        EraseRoundRect(&saveBtnR, 6, 6);
                        FrameRoundRect(&saveBtnR, 6, 6);
                        MoveTo(saveBtnR.left + 8, saveBtnR.top + 11);
                        if (gSaveSuccessMsgIdx == i && (TickCount() - gSaveSuccessTick) < 120) {
                            DrawCString("Save succesful");
                        } else {
                            DrawCString("Download full size");
                        }
                    }

                    /* Retry and Copy action icons on hover with pill background (retry-bg-example.png) */
                    if (gHoverBubbleIdx == i) {
                        Rect actPill;
                        const unsigned char *cIcon = (gCopiedMsgIdx == i && (TickCount() - gCopiedTick) < 120)
                            ? kIconCopyOk12x12 : kIconCopy12x12;
                        SetRect(&actPill, aiLeftX + 3, actionY - 1, aiLeftX + 37, actionY + 13);
                        EraseRoundRect(&actPill, 8, 8);
                        BlitBitmap(kIconRetry12x12, 2, 12, 12, aiLeftX + 6,  actionY, srcOr);
                        BlitBitmap(cIcon,          2, 12, 12, aiLeftX + 22, actionY, srcOr);
                    }
                }
            }
        }
        curY += stepH;
    }

    if (gWaitingForAI) {
        char waitLines[8][96];
        short nWaitLines = WrapMessageLines(gWaitStatusText, aiMaxW - 24, waitLines, 8);
        short widestWaitLine = 0;
        short waitL;
        short waitW, waitH;
        Rect waitR;

        for (waitL = 0; waitL < nWaitLines; waitL++) {
            short w = MeasureCString(waitLines[waitL]);
            if (w > widestWaitLine) widestWaitLine = w;
        }

        waitW = widestWaitLine + 24;
        if (waitW < 96) waitW = 96;
        if (waitW > aiMaxW) waitW = aiMaxW;

        waitH = nWaitLines * 12 + 14;
        if (waitH < 24) waitH = 24;

        SetRect(&waitR, aiLeftX, curY, aiLeftX + waitW, curY + waitH);
        {
            Rect shadowR = waitR;
            OffsetRect(&shadowR, 0, 1);
            FillRoundRect(&shadowR, 14, 14, &qd.black);
            EraseRoundRect(&waitR, 14, 14);
        }
        for (waitL = 0; waitL < nWaitLines; waitL++) {
            MoveTo(waitR.left + 12, curY + 14 + waitL * 12);
            DrawCString(waitLines[waitL]);
        }
    }

    /* Restore full window clip */
    SetRect(&savedClip, 0, 0, gWinW, gWinH);
    ClipRect(&savedClip);

    /* Right-edge Chat Scrollbar (matching SpotiMac and Main_Normal.png) */
    if (gMsgMaxScrollY > 0) {
        short sbX = gWinW - 11;
        short sbTop = 10;
        short sbBot = msgAreaBot - 4;
        short trackH = sbBot - sbTop;
        short msgAreaH = msgAreaBot - msgAreaTop;
        short thumbH = (totalContentH > 0) ? (short)(((long)trackH * msgAreaH) / totalContentH) : 28;
        short thumbTop;
        Rect trackR, thumbR;

        if (thumbH < 20) thumbH = 20;
        if (thumbH > trackH) thumbH = trackH;
        thumbTop = sbTop + (short)(((long)(trackH - thumbH) * gMsgScrollY) / gMsgMaxScrollY);

        /* 5px wide rounded white track */
        SetRect(&trackR, sbX - 2, sbTop, sbX + 3, sbBot);
        FillRoundRect(&trackR, 3, 3, &qd.white);

        /* Center vertical dotted track line */
        DrawDottedVLine(sbX, sbTop + 1, sbBot - 1);

        /* 3px wide thumb filled with qd.gray */
        SetRect(&thumbR, sbX - 1, thumbTop, sbX + 2, thumbTop + thumbH);
        FillRect(&thumbR, &qd.gray);
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
    SetRect(&popR, 8, 20, 102, 20 + 4 * 16 + 4);
    DrawShadowRoundRect(&popR, 8, 8, 2);

    TextFont(kFontIDMonaco);
    TextSize(9);
    for (i = 0; i < 4; i++) {
        short ry = popR.top + 2 + i * 16;
        Rect rowR;
        SetRect(&rowR, popR.left + 3, ry, popR.right - 3, ry + 15);
        if (strcmp(gProviders[i].id, gActiveProvider) == 0) {
            Rect dotR;
            SetRect(&dotR, popR.left + 7, ry + 5, popR.left + 11, ry + 9);
            PaintOval(&dotR);
        }
        MoveTo(popR.left + 16, ry + 11);
        DrawCString(gProviders[i].name);
        if (!gProviders[i].enabled) {
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
    SetRect(&popR, tierR.right - 84, pillR.top - 54, tierR.right, pillR.top - 2);
    DrawShadowRoundRect(&popR, 8, 8, 2);

    TextFont(kFontIDMonaco);
    TextSize(9);
    for (i = 0; i < 3; i++) {
        short ry = popR.top + 2 + i * 16;
        if (strcmp(GetTierDisplayName(gActiveTier), tiers[i]) == 0) {
            Rect dotR;
            SetRect(&dotR, popR.left + 7, ry + 5, popR.left + 11, ry + 9);
            PaintOval(&dotR);
        }
        MoveTo(popR.left + 16, ry + 11);
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
    BlitBitmap(kIconSearch12x12, 2, 12, 12, modalR.left + 12, modalR.top + 11, srcOr);
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

    for (k = 0; k < 4; k++) {
        if (strcmp(gProviders[k].id, normId) == 0) {
            return gProviders[k].enabled;
        }
    }
    return false;
}

static void NormalizeTierName(const char *inTier, char *outTier, short maxLen)
{
    const char *norm = GetTierDisplayName(inTier);
    if (!inTier || !*inTier || maxLen < 2) return;
    strncpy(outTier, norm, maxLen - 1);
    outTier[maxLen - 1] = '\0';
    if (norm[0] == 'F') gActiveThinkingIdx = 0;
    else if (norm[0] == 'T') gActiveThinkingIdx = 2;
    else gActiveThinkingIdx = 1;
}

static short GetActiveProviderIdx(void)
{
    if (strcmp(gActiveProvider, "chatgpt") == 0 || strcmp(gActiveProvider, "openai") == 0) return 1;
    if (strcmp(gActiveProvider, "claude") == 0) return 2;
    if (strcmp(gActiveProvider, "groq") == 0) return 3;
    return 0; /* gemini */
}

static Boolean ActiveModelSupportsThinking(void)
{
    short pIdx = GetActiveProviderIdx();
    short mIdx = gProviders[pIdx].activeModelIdx;
    const char *mId = (mIdx == 4 && gProviders[pIdx].hasCustomModel && gProviders[pIdx].customModelName[0])
                        ? gProviders[pIdx].customModelName
                        : gProviders[pIdx].models[mIdx].id;
    if (!mId || !*mId) return false;
    if (strcmp(gActiveProvider, "gemini") == 0) return true;
    if (strcmp(gActiveProvider, "chatgpt") == 0 || strcmp(gActiveProvider, "openai") == 0) {
        return (strncmp(mId, "o1", 2) == 0 || strncmp(mId, "o3", 2) == 0 || strncmp(mId, "o4", 2) == 0);
    }
    if (strcmp(gActiveProvider, "claude") == 0) {
        return (strstr(mId, "3-7-sonnet") != NULL || strstr(mId, "3.7-sonnet") != NULL);
    }
    if (strcmp(gActiveProvider, "groq") == 0) {
        return (strstr(mId, "gpt-oss") != NULL || strstr(mId, "qwen3") != NULL || strstr(mId, "qwen-3") != NULL);
    }
    return false;
}

static void SyncSpeechMenuState(void)
{
    MenuHandle speechMenu = GetMenuHandle(mSpeech);
    short v;
    if (!speechMenu) return;
    CheckItem(speechMenu, iSpeechReadAloud, gReadRepliesTTS);
    for (v = 0; v < 7; v++) {
        CheckItem(speechMenu, iSpeechVoiceAlloy + v, (gActiveVoiceIdx == v));
    }
}

static void SelectVoice(short vIdx)
{
    char url[64];
    if (vIdx < 0 || vIdx >= 7) return;
    gActiveVoiceIdx = vIdx;
    SyncSpeechMenuState();
    SaveMacAIPrefs();
    sprintf(url, "/api/macai/voice-settings?voice=%s", gTtsVoiceNames[gActiveVoiceIdx]);
    NetHttpGet(url, gNetBuf, NET_BUF_SIZE);
}

static void SyncAIMenuState(void)
{
    MenuHandle setMenu = GetMenuHandle(mSettings);
    short pIdx = GetActiveProviderIdx();
    short i;
    if (!setMenu) return;

    if (IsProviderEnabled("gemini")) EnableItem(setMenu, iUseGemini);
    else DisableItem(setMenu, iUseGemini);

    if (IsProviderEnabled("chatgpt")) EnableItem(setMenu, iUseOpenAI);
    else DisableItem(setMenu, iUseOpenAI);

    if (IsProviderEnabled("claude")) EnableItem(setMenu, iUseClaude);
    else DisableItem(setMenu, iUseClaude);

    if (IsProviderEnabled("groq")) EnableItem(setMenu, iUseGroq);
    else DisableItem(setMenu, iUseGroq);

    CheckItem(setMenu, iUseGemini, (pIdx == 0));
    CheckItem(setMenu, iUseOpenAI, (pIdx == 1));
    CheckItem(setMenu, iUseClaude, (pIdx == 2));
    CheckItem(setMenu, iUseGroq,   (pIdx == 3));

    /* Update Model 1..4 title and checkmark */
    for (i = 0; i < 4; i++) {
        Str255 pTitle;
        char cTitle[64];
        sprintf(cTitle, "Model: %s", gProviders[pIdx].models[i].name);
        c2pstrcpy(pTitle, cTitle);
        SetMenuItemText(setMenu, iModel1 + i, pTitle);
        CheckItem(setMenu, iModel1 + i, (gProviders[pIdx].activeModelIdx == i));
    }

    /* Update Model: Custom title and availability */
    {
        Str255 pTitle;
        char cTitle[64];
        if (gProviders[pIdx].hasCustomModel && gProviders[pIdx].customModelName[0]) {
            sprintf(cTitle, "Model: Custom: %s", gProviders[pIdx].customModelName);
            c2pstrcpy(pTitle, cTitle);
            SetMenuItemText(setMenu, iModelCustom, pTitle);
            EnableItem(setMenu, iModelCustom);
            CheckItem(setMenu, iModelCustom, (gProviders[pIdx].activeModelIdx == 4));
        } else {
            c2pstrcpy(pTitle, "Model: Custom");
            SetMenuItemText(setMenu, iModelCustom, pTitle);
            DisableItem(setMenu, iModelCustom);
            CheckItem(setMenu, iModelCustom, false);
        }
    }

    /* Thinking levels: enabled only if active model supports thinking */
    if (ActiveModelSupportsThinking()) {
        EnableItem(setMenu, iThinkingFast);
        EnableItem(setMenu, iThinkingBalanced);
        EnableItem(setMenu, iThinkingThinking);
        CheckItem(setMenu, iThinkingFast,     (gActiveThinkingIdx == 0));
        CheckItem(setMenu, iThinkingBalanced, (gActiveThinkingIdx == 1));
        CheckItem(setMenu, iThinkingThinking, (gActiveThinkingIdx == 2));
    } else {
        DisableItem(setMenu, iThinkingFast);
        DisableItem(setMenu, iThinkingBalanced);
        DisableItem(setMenu, iThinkingThinking);
        CheckItem(setMenu, iThinkingFast,     false);
        CheckItem(setMenu, iThinkingBalanced, false);
        CheckItem(setMenu, iThinkingThinking, false);
    }

    SyncSpeechMenuState();
}

static void CopyMessageToClipboard(short msgIdx)
{
    if (msgIdx < 0 || msgIdx >= gMessageCount) return;
    CopyTextToClipboard(gMessages[msgIdx].text);
    gCopiedMsgIdx = msgIdx;
    gCopiedTick   = TickCount();
    RenderChatAreaOnly();
}

static void CopyImageToClipboard(short msgIdx)
{
    PicHandle myPic;
    Rect picR;
    ChatMessage *msg;

    if (msgIdx < 0 || msgIdx >= gMessageCount) return;
    msg = &gMessages[msgIdx];
    if (!msg->imageHandle || !*(msg->imageHandle) || msg->imageH <= 0 || msg->imageW <= 0) return;

    SetRect(&picR, 0, 0, msg->imageW, msg->imageH);
    myPic = OpenPicture(&picR);
    if (myPic) {
        BitMap bm;
        bm.baseAddr = *(msg->imageHandle);
        bm.rowBytes = msg->imageRowBytes;
        bm.bounds   = picR;
        CopyBits(&bm, &qd.thePort->portBits, &picR, &picR, srcCopy, NULL);
        ClosePicture();

        ZeroScrap();
        HLock((Handle)myPic);
        PutScrap(GetHandleSize((Handle)myPic), 'PICT', *(Handle)myPic);
        HUnlock((Handle)myPic);
        KillPicture(myPic);

        gCopiedMsgIdx = msgIdx;
        gCopiedTick   = TickCount();
        RenderChatAreaOnly();
    }
}

static void SaveImagePictToFile(short msgIdx)
{
    StandardFileReply sfReply;
    short refNum = 0;
    OSErr err;
    char path[128];
    ChatMessage *msg;
    CursHandle watchH;

    if (msgIdx < 0 || msgIdx >= gMessageCount) return;
    msg = &gMessages[msgIdx];
    if (msg->imageId[0] == '\0') return;

    StandardPutFile("\pSave image as:", "\pMacAI Image.pict", &sfReply);
    if (!sfReply.sfGood) return;

    if (sfReply.sfReplacing) {
        FSpDelete(&sfReply.sfFile);
    }

    err = FSpCreate(&sfReply.sfFile, 'ttxt', 'PICT', sfReply.sfScript);
    if (err != noErr && err != dupFNErr) {
        SysBeep(1);
        return;
    }

    err = FSpOpenDF(&sfReply.sfFile, fsWrPerm, &refNum);
    if (err != noErr) {
        SysBeep(1);
        return;
    }

    watchH = GetCursor(watchCursor);
    if (watchH) SetCursor(*watchH);

    sprintf(path, "/api/macai/image/%s/pict", msg->imageId);
    NetHttpGetToFile(path, refNum);

    FSClose(refNum);
    InitCursor();
    SysBeep(1);

    gSaveSuccessMsgIdx = msgIdx;
    gSaveSuccessTick   = TickCount();
    RenderWindow();
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
    gMessageCount      = aiMsgIdx;
    gCopiedMsgIdx      = -1;
    gSaveSuccessMsgIdx = -1;
    gMsgScrollY        = 30000;

    SendPromptToAI(userPrompt, true);
}

static short HitTestMessageBubble(Point pt)
{
    short chatLeft  = gSidebarCollapsed ? 0 : (SIDEBAR_W + 1);
    short chatWidth = gWinW - chatLeft;
    short userMaxW  = chatWidth - 90;
    short aiMaxW;
    short aiLeftX   = chatLeft + (gSidebarCollapsed ? 34 : 18);
    short msgAreaTop = 8;
    Rect pillR, tierR, micR;
    short curY;
    short i;

    if (userMaxW > 240) userMaxW = 240;
    if (userMaxW < 120) userMaxW = 120;
    aiMaxW  = (gWinW - 22) - aiLeftX - 24;
    if (aiMaxW < 140) aiMaxW = 140;

    if (pt.h < chatLeft || pt.h > gWinW - 12) return -1;
    GetPromptBarGeometry(&pillR, &tierR, &micR);
    if (pt.v < msgAreaTop || pt.v >= pillR.top - 6) return -1;

    curY = msgAreaTop - gMsgScrollY;
    for (i = 0; i < gMessageCount; i++) {
        short maxW = gMessages[i].isUser ? userMaxW : aiMaxW;
        short bubbleH, widestLine, stepH;
        Rect hitR;

        if (gEditingMsgIdx == i) {
            short nLines = WrapMessageLines(gEditBuf, maxW, NULL, MAX_WRAPPED_LINES);
            bubbleH = nLines * 12 + 14;
            if (bubbleH < 24) bubbleH = 24;
            stepH = bubbleH + 24;
            widestLine = 120;
        } else {
            EnsureMessageMetrics(&gMessages[i], maxW);
            bubbleH = gMessages[i].cachedBubbleH;
            widestLine = gMessages[i].cachedWidestPx;
            stepH = bubbleH + (gMessages[i].isUser ? 18 : 22);
        }

        if (gMessages[i].isUser) {
            short bw = widestLine + 20;
            short rightEdge = gWinW - 25;
            if (bw < 40) bw = 40;
            SetRect(&hitR, rightEdge - bw - 4, curY, rightEdge + 4, curY + bubbleH + 18);
        } else {
            short bw = widestLine + 24;
            if (bw < 100) bw = 100;
            SetRect(&hitR, aiLeftX - 4, curY, aiLeftX + bw + 4, curY + bubbleH + 20);
        }

        if (PtInRect(pt, &hitR)) {
            return i;
        }
        curY += stepH;
    }
    return -1;
}

static void SubmitEditedMessage(short editIdx)
{
    if (editIdx < 0 || editIdx >= gMessageCount || gEditBufLen == 0) {
        gEditingMsgIdx = -1;
        gPromptFocused = true;
        RenderWindow();
        return;
    }

    snprintf(gMessages[editIdx].text, sizeof(gMessages[editIdx].text), "%s", gEditBuf);
    gMessages[editIdx].cachedBubbleH = 0;
    gMessages[editIdx].cachedWidestPx = 0;

    /* Truncate conversation to this message */
    gMessageCount = editIdx + 1;

    gEditingMsgIdx = -1;
    gPromptFocused = true;

    /* Trigger AI regeneration for edited prompt */
    SendPromptToAI(gMessages[editIdx].text, true);
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

                        SafeCopyString(outArr[count].id, p1, sizeof(outArr[count].id));
                        SafeCopyString(outArr[count].title, p2, sizeof(outArr[count].title));
                        SafeCopyString(outArr[count].provider, p3, sizeof(outArr[count].provider));
                        SafeCopyString(outArr[count].tier, p4, sizeof(outArr[count].tier));
                        SafeCopyString(outArr[count].age, p5 ? p5 : "Today", sizeof(outArr[count].age));
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
                        if (!gUserHasSavedPrefs) {
                            SafeCopyString(gActiveProvider, p1, sizeof(gActiveProvider));
                            NormalizeTierName(p3, gActiveTier, sizeof(gActiveTier));
                        }
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
                        char *p4;
                        short k;
                        *p3++ = '\0';
                        p4 = strchr(p3, '|');
                        if (p4) {
                            char *p5;
                            *p4++ = '\0';
                            p5 = strchr(p4, '|');
                            if (p5) {
                                *p5++ = '\0';
                                for (k = 0; k < 4; k++) {
                                    if (strcmp(gProviders[k].id, p1) == 0) {
                                        gProviders[k].enabled = (p3[0] == '1');
                                        if (p5 && *p5) {
                                            SafeCopyString(gProviders[k].customModelName, p5, sizeof(gProviders[k].customModelName));
                                            gProviders[k].hasCustomModel = true;
                                        } else {
                                            gProviders[k].customModelName[0] = '\0';
                                            gProviders[k].hasCustomModel = false;
                                            if (gProviders[k].activeModelIdx == 4) {
                                                gProviders[k].activeModelIdx = 1;
                                            }
                                        }
                                    }
                                }
                            } else {
                                for (k = 0; k < 4; k++) {
                                    if (strcmp(gProviders[k].id, p1) == 0) {
                                        gProviders[k].enabled = (p3[0] == '1');
                                    }
                                }
                            }
                        } else {
                            for (k = 0; k < 4; k++) {
                                if (strcmp(gProviders[k].id, p1) == 0) {
                                    gProviders[k].enabled = (p3[0] == '1');
                                }
                            }
                        }
                    }
                }
            } else if (strncmp(row, "VOICE|", 6) == 0) {
                char *p1 = row + 6;
                char *p2 = strchr(p1, '|');
                if (p2) {
                    short v;
                    char *p3;
                    *p2++ = '\0';
                    p3 = strchr(p2, '|');
                    if (p3) *p3 = '\0';
                    if (!gUserHasSavedPrefs) {
                        gReadRepliesTTS = (p1[0] == '1');
                        for (v = 0; v < 7; v++) {
                            if (strcmp(gTtsVoiceNames[v], p2) == 0) {
                                gActiveVoiceIdx = v;
                                break;
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
        for (k = 0; k < 4; k++) {
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

static void SelectProvider(const char *provider)
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
    SyncAIMenuState();
    SaveMacAIPrefs();
    RenderWindow();
}

static void SelectModel(short mIdx)
{
    short pIdx = GetActiveProviderIdx();
    if (mIdx == 4 && (!gProviders[pIdx].hasCustomModel || !gProviders[pIdx].customModelName[0])) {
        SysBeep(1);
        return;
    }
    if (mIdx >= 0 && mIdx <= 4) {
        gProviders[pIdx].activeModelIdx = mIdx;
        SyncAIMenuState();
        SaveMacAIPrefs();
        RenderWindow();
    }
}

static void SelectThinkingLevel(short tIdx)
{
    if (!ActiveModelSupportsThinking()) {
        SysBeep(1);
        return;
    }
    if (tIdx >= 0 && tIdx < 3) {
        gActiveThinkingIdx = tIdx;
        strncpy(gActiveTier, gThinkingLevels[tIdx], sizeof(gActiveTier) - 1);
        SyncAIMenuState();
        SaveMacAIPrefs();
        RenderWindow();
    }
}

static void SelectProviderOrTier(const char *provider, const char *tier)
{
    if (provider && *provider) {
        SelectProvider(provider);
    }
    if (tier && *tier) {
        if (tier[0] == 'f' || tier[0] == 'F' || tier[0] == 'l' || tier[0] == 'L') SelectThinkingLevel(0);
        else if (tier[0] == 't' || tier[0] == 'T' || tier[0] == 'h' || tier[0] == 'H') SelectThinkingLevel(2);
        else SelectThinkingLevel(1);
    }
}

static void ClearAllMessages(void)
{
    short i;
    for (i = 0; i < gMessageCount; i++) {
        if (gMessages[i].imageHandle) {
            DisposeHandle(gMessages[i].imageHandle);
            gMessages[i].imageHandle = NULL;
        }
        gMessages[i].imageId[0] = '\0';
        gMessages[i].imageH = 0;
        gMessages[i].imageW = 0;
    }
    gMessageCount = 0;
    gMsgScrollY   = 0;
}

static void AppendMessageEx(Boolean isUser, const char *text, const char *imgId, short imgW, short imgH, short imgRowBytes, Handle imgHdl)
{
    ChatMessage *msg;
    if (!text && !imgHdl) return;
    if (gMessageCount >= MAX_MESSAGES) {
        if (gMessages[0].imageHandle) {
            DisposeHandle(gMessages[0].imageHandle);
            gMessages[0].imageHandle = NULL;
        }
        memmove(&gMessages[0], &gMessages[1], sizeof(ChatMessage) * (MAX_MESSAGES - 1));
        gMessageCount = MAX_MESSAGES - 1;
    }
    msg = &gMessages[gMessageCount];
    msg->isUser        = isUser;
    msg->cachedMaxW    = 0;
    msg->cachedLines   = 0;
    msg->cachedWidestPx = 0;
    msg->cachedBubbleH = 0;
    if (text) {
        strncpy(msg->text, text, MAX_MSG_TEXT - 1);
        msg->text[MAX_MSG_TEXT - 1] = '\0';
    } else {
        msg->text[0] = '\0';
    }
    if (imgId && *imgId) {
        strncpy(msg->imageId, imgId, sizeof(msg->imageId) - 1);
        msg->imageId[sizeof(msg->imageId) - 1] = '\0';
    } else {
        msg->imageId[0] = '\0';
    }
    msg->imageW        = imgW;
    msg->imageH        = imgH;
    msg->imageRowBytes = imgRowBytes;
    msg->imageHandle   = imgHdl;
    gMessageCount++;
    gMsgScrollY = 30000;
}

static void AppendMessage(Boolean isUser, const char *text)
{
    AppendMessageEx(isUser, text, NULL, 0, 0, 0, NULL);
}

static void StartNewChat(void)
{
    gActiveChatId[0]    = '\0';
    gSelectedChatIdx    = -1;
    ClearAllMessages();
    gMsgMaxScrollY      = 0;
    gCopiedMsgIdx       = -1;
    gSaveSuccessMsgIdx  = -1;
    gPromptBuf[0]       = '\0';
    gCursorPos          = 0;
    gPromptSelAnchor    = 0;
    gShowPlaceholder    = true;
    gPromptFocused      = false;
    gPromptScrollLine   = 0;
    gPrevPromptVisLines = 1;
    RenderWindow();
}

static void LoadChatById(const char *chatId)
{
    static char row[MAX_MSG_TEXT + 128];
    char path[96];
    const char *line;
    short i;

    if (!chatId || !*chatId || !gNetBuf || !gServerConnected) return;

    sprintf(path, "/api/macai/chat/%s", chatId);
    if (NetHttpGet(path, gNetBuf, NET_BUF_SIZE) <= 0) return;

    SafeCopyString(gActiveChatId, chatId, sizeof(gActiveChatId));
    gSelectedChatIdx = -1;
    for (i = 0; i < gChatCount; i++) {
        if (strcmp(gChats[i].id, chatId) == 0) {
            gSelectedChatIdx = i;
            break;
        }
    }
    ClearAllMessages();
    gCopiedMsgIdx      = -1;
    gSaveSuccessMsgIdx = -1;

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
                        if (!gUserHasSavedPrefs && IsProviderEnabled(p3 + 1)) {
                            strncpy(gActiveProvider, p3 + 1, sizeof(gActiveProvider) - 1);
                        }
                        if (!gUserHasSavedPrefs) {
                            NormalizeTierName(p4 + 1, gActiveTier, sizeof(gActiveTier));
                            SyncAIMenuState();
                        }
                    }
                }
            }
        } else if (strncmp(row, "MSG|user|", 9) == 0) {
            AppendMessage(true, row + 9);
        } else if (strncmp(row, "MSG|assistant|", 14) == 0) {
            char *pText = row + 14;
            char *pImg = strchr(pText, '|');
            if (pImg) {
                char *pW, *pH, *pRb;
                *pImg++ = '\0';
                pW = strchr(pImg, '|');
                if (pW) {
                    *pW++ = '\0';
                    pH = strchr(pW, '|');
                    if (pH) {
                        *pH++ = '\0';
                        pRb = strchr(pH, '|');
                        if (pRb) {
                            *pRb++ = '\0';
                            AppendMessageEx(false, pText, pImg, (short)atoi(pW), (short)atoi(pH), (short)atoi(pRb), NULL);
                        } else {
                            AppendMessage(false, pText);
                        }
                    } else {
                        AppendMessage(false, pText);
                    }
                } else {
                    AppendMessage(false, pText);
                }
            } else {
                AppendMessage(false, pText);
            }
        }
        line = next ? (next + 1) : NULL;
    }

    /* Pass 2: Download any image previews (safe now since row parsing is done) */
    for (i = 0; i < gMessageCount; i++) {
        if (!gMessages[i].isUser && gMessages[i].imageId[0] != '\0' && !gMessages[i].imageHandle) {
            char imgPath[96];
            long imgBytes;
            sprintf(imgPath, "/api/macai/image/%s", gMessages[i].imageId);
            imgBytes = NetHttpGet(imgPath, gNetBuf, NET_BUF_SIZE);
            if (imgBytes > 8) {
                Handle h = NewHandle(imgBytes - 8);
                if (h) {
                    HLock(h);
                    BlockMove(gNetBuf + 8, *h, imgBytes - 8);
                    HUnlock(h);
                    gMessages[i].imageHandle = h;
                }
            }
        }
    }

    /* Scroll to bottom of loaded conversation */
    gMsgScrollY = 30000;
    RenderWindow();
}

static void OnAIStatusCallback(const char *statusText)
{
    if (statusText && *statusText) {
        SafeCopyString(gWaitStatusText, statusText, sizeof(gWaitStatusText));
        RenderWindow();
    }
}

static void SendPromptToAI(const char *promptText, Boolean isRetry)
{
    static char encPrompt[960];
    static char encModel[64];
    static char path[1200];
    long n;
    short pIdx = GetActiveProviderIdx();
    short mIdx = gProviders[pIdx].activeModelIdx;
    const char *modelId = (mIdx == 4 && gProviders[pIdx].hasCustomModel && gProviders[pIdx].customModelName[0])
                            ? gProviders[pIdx].customModelName
                            : gProviders[pIdx].models[mIdx].id;
    const char *thinkingName = gThinkingLevels[gActiveThinkingIdx];

    if (!promptText || !*promptText || !gNetBuf || !gServerConnected) return;

    if (!isRetry) {
        AppendMessage(true, promptText);
    }
    SafeCopyString(gWaitStatusText, "Thinking...", sizeof(gWaitStatusText));
    gWaitStartTick = TickCount();
    gUserCancelledWait = false;
    gWaitingForAI = true;
    RenderWindow();

    if (gReadRepliesTTS) {
        AudioStreamStart(22255);
    }

    UrlEncode(promptText, encPrompt, sizeof(encPrompt));
    UrlEncode(modelId, encModel, sizeof(encModel));
    sprintf(path, "/api/macai/ask?chat_id=%s&provider=%s&model=%s&thinking=%s&tts=%d&retry=%d&q=%s",
            gActiveChatId, gProviders[pIdx].id, encModel, thinkingName,
            gReadRepliesTTS ? 1 : 0, isRetry ? 1 : 0, encPrompt);

    n = NetHttpGetStream(path, gNetBuf, NET_BUF_SIZE, OnAIStatusCallback, 45);
    gWaitingForAI = false;

    if (!gRunning) return;

    if (n > 0 && strncmp(gNetBuf, "REPLY|", 6) == 0) {
        char *fields[12];
        short numFields = 0;
        char *scan = gNetBuf + 6;
        fields[numFields++] = scan;
        while ((scan = strchr(scan, '|')) != NULL && numFields < 12) {
            *scan++ = '\0';
            fields[numFields++] = scan;
        }

        if (numFields >= 5) {
            /* fields[0]=chatId, fields[1]=title, fields[2]=provider, fields[3]=thinking, fields[4]=replyText */
            strncpy(gActiveChatId, fields[0], sizeof(gActiveChatId) - 1);
            if (IsProviderEnabled(fields[2])) {
                strncpy(gActiveProvider, fields[2], sizeof(gActiveProvider) - 1);
            }
            NormalizeTierName(fields[3], gActiveTier, sizeof(gActiveTier));
            SyncAIMenuState();

            if (numFields >= 9 && fields[5] && *fields[5]) {
                /* Image included: fields[5]=imgId, fields[6]=w, fields[7]=h, fields[8]=rb */
                char imgPath[96];
                char replyCopy[MAX_MSG_TEXT];
                char imgIdCopy[36];
                short w  = (short)atoi(fields[6]);
                short h  = (short)atoi(fields[7]);
                short rb = (short)atoi(fields[8]);
                long imgBytes;

                SafeCopyString(replyCopy, fields[4], sizeof(replyCopy));
                SafeCopyString(imgIdCopy, fields[5], sizeof(imgIdCopy));

                sprintf(imgPath, "/api/macai/image/%s", imgIdCopy);
                imgBytes = NetHttpGet(imgPath, gNetBuf, NET_BUF_SIZE);
                if (imgBytes > 8) {
                    Handle imgH = NewHandle(imgBytes - 8);
                    if (imgH) {
                        HLock(imgH);
                        BlockMove(gNetBuf + 8, *imgH, imgBytes - 8);
                        HUnlock(imgH);
                        AppendMessageEx(false, replyCopy, imgIdCopy, w, h, rb, imgH);
                    } else {
                        AppendMessage(false, replyCopy);
                    }
                } else {
                    AppendMessage(false, replyCopy);
                }
            } else {
                AppendMessage(false, fields[4]);
            }
            RefreshChatList();
        }
    } else if (n > 0 && strncmp(gNetBuf, "ERR|", 4) == 0) {
        char *msg = strchr(gNetBuf + 4, '|');
        AppendMessage(false, msg ? (msg + 1) : (gNetBuf + 4));
    } else if (gUserCancelledWait) {
        AppendMessage(false, "Request cancelled.");
    } else {
        AppendMessage(false, "Network error: Request timed out or server unreachable.");
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
        SetRect(&popR, 8, 20, 102, 20 + 4 * 16 + 4);
        gProviderMenuOpen = false;
        if (PtInRect(localPt, &popR)) {
            short idx = (localPt.v - (popR.top + 2)) / 16;
            if (idx >= 0 && idx < 4) {
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
            if (!ActiveModelSupportsThinking()) {
                SysBeep(1);
                return;
            }
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

            gShowPlaceholder = false;
            gPromptFocused   = true;
            gCaretVisible    = true;
            gLastCaretTick   = TickCount();

            /* Set anchor and cursor at initial click point */
            short clickPos = GetPromptCharPosAtPoint(localPt, &pillR);
            gPromptSelAnchor = clickPos;
            gCursorPos       = clickPos;
            EnsureCaretVisible();
            RenderPromptBarOnly();

            /* Drag mouse to select text live */
            while (StillDown()) {
                Point curPt;
                GetMouse(&curPt);
                short dragPos = GetPromptCharPosAtPoint(curPt, &pillR);
                if (dragPos != gCursorPos) {
                    gCursorPos = dragPos;
                    EnsureCaretVisible();
                    RenderPromptBarOnly();
                }
            }
            return;
        }

        if (localPt.v < pillR.top - 6 && gPromptFocused) {
            gPromptFocused   = false;
            gPromptSelAnchor = gCursorPos;
            RenderPromptBarOnly();
        }

        /* Main Chat Area Draggable Scrollbar */
        {
            short sbX = gWinW - 11;
            short sbTop = 10;
            short sbBot = pillR.top - 6;
            if (gMsgMaxScrollY > 0 && localPt.h >= sbX - 4 && localPt.h <= gWinW && localPt.v >= sbTop && localPt.v <= sbBot) {
                short trackH   = sbBot - sbTop;
                short msgAreaH = pillR.top - 12;
                short totalH   = msgAreaH + gMsgMaxScrollY;
                short thumbH   = (totalH > 0) ? (short)(((long)trackH * msgAreaH) / totalH) : trackH;
                short thumbY;
                if (thumbH < 20) thumbH = 20;
                if (thumbH > trackH) thumbH = trackH;
                thumbY = sbTop + (short)(((long)(trackH - thumbH) * gMsgScrollY) / gMsgMaxScrollY);

                if (localPt.v >= thumbY && localPt.v <= thumbY + thumbH) {
                    /* Direct thumb drag matching SpotiMac (instant 60fps tracking) */
                    short grabOff = localPt.v - thumbY;
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
                        if (newScrollY != gMsgScrollY) {
                            gMsgScrollY = newScrollY;
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
        }

        /* Hit-test Action Buttons under bubbles or Edit buttons */
        if (localPt.v >= 8 && localPt.v < pillR.top - 4) {
            short chatLeft  = gSidebarCollapsed ? 0 : (SIDEBAR_W + 1);
            short chatW     = gWinW - chatLeft;
            short userMaxW  = chatW - 90;
            short aiMaxW;
            short aiLeftX   = chatLeft + (gSidebarCollapsed ? 34 : 18);
            short curY      = 8 - gMsgScrollY;
            short i;
            static char wrappedBuf[MAX_WRAPPED_LINES][96];

            if (userMaxW > 240) userMaxW = 240;
            if (userMaxW < 120) userMaxW = 120;
            aiMaxW  = (gWinW - 22) - aiLeftX - 24;
            if (aiMaxW < 140) aiMaxW = 140;

            for (i = 0; i < gMessageCount; i++) {
                short maxW = gMessages[i].isUser ? userMaxW : aiMaxW;
                short bubbleH, stepH, widestLine;
                Boolean isEditingThis = (gEditingMsgIdx == i);

                if (isEditingThis) {
                    short nLines = WrapMessageLines(gEditBuf, maxW, wrappedBuf, MAX_WRAPPED_LINES);
                    short L;
                    widestLine = 0;
                    for (L = 0; L < nLines; L++) {
                        short w = MeasureCString(wrappedBuf[L]);
                        if (w > widestLine) widestLine = w;
                    }
                    bubbleH = nLines * 12 + 14;
                    if (bubbleH < 24) bubbleH = 24;
                    stepH = bubbleH + 24;

                    {
                        short rightEdge = gWinW - 25;
                        Rect bubbleR, cancelR, saveR;
                        short bw = widestLine + 20;
                        if (bw < 120) bw = 120;
                        SetRect(&bubbleR, rightEdge - bw, curY, rightEdge, curY + bubbleH);
                        SetRect(&cancelR, rightEdge - 92, curY + bubbleH + 3, rightEdge - 48, curY + bubbleH + 17);
                        SetRect(&saveR,   rightEdge - 44, curY + bubbleH + 3, rightEdge,      curY + bubbleH + 17);

                        if (PtInRect(localPt, &cancelR)) {
                            gEditingMsgIdx = -1;
                            gPromptFocused = true;
                            RenderWindow();
                            return;
                        }
                        if (PtInRect(localPt, &saveR)) {
                            SubmitEditedMessage(gEditingMsgIdx);
                            return;
                        }
                        if (PtInRect(localPt, &bubbleR)) {
                            short clickPos = GetEditCharPosAtPoint(localPt, &bubbleR, curY, nLines, wrappedBuf);
                            gEditSelAnchor = clickPos;
                            gEditCursorPos = clickPos;
                            gCaretVisible  = true;
                            gLastCaretTick = TickCount();
                            RenderChatAreaOnly();

                            while (StillDown()) {
                                Point curPt;
                                GetMouse(&curPt);
                                short dragPos = GetEditCharPosAtPoint(curPt, &bubbleR, curY, nLines, wrappedBuf);
                                if (dragPos != gEditCursorPos) {
                                    gEditCursorPos = dragPos;
                                    RenderChatAreaOnly();
                                }
                            }
                            return;
                        }
                    }
                } else {
                    EnsureMessageMetrics(&gMessages[i], maxW);
                    bubbleH = gMessages[i].cachedBubbleH;
                    widestLine = gMessages[i].cachedWidestPx;
                    stepH = bubbleH + (gMessages[i].isUser ? 18 : 22);

                    if (!gMessages[i].isUser && gMessages[i].imageHandle && *(gMessages[i].imageHandle) && gMessages[i].imageH > 0) {
                        short nLines = gMessages[i].cachedLines;
                        short imgX = aiLeftX + 12;
                        short imgY = curY + (nLines > 0 ? (14 + nLines * 12 + 4) : 10);
                        short btnY = imgY + gMessages[i].imageH + 6;
                        Rect copyImgBtnR, saveImgBtnR;
                        SetRect(&copyImgBtnR, imgX, btnY, imgX + 54, btnY + 16);
                        SetRect(&saveImgBtnR, imgX + 60, btnY, imgX + 192, btnY + 16);

                        if (PtInRect(localPt, &copyImgBtnR)) {
                            CopyImageToClipboard(i);
                            return;
                        }
                        if (PtInRect(localPt, &saveImgBtnR)) {
                            SaveImagePictToFile(i);
                            return;
                        }
                    }

                    if (gHoverBubbleIdx == i) {
                        if (gMessages[i].isUser) {
                            short rightEdge = gWinW - 25;
                            short actionY = curY + bubbleH + 3;
                            Rect editHitR, copyHitR;
                            SetRect(&editHitR, rightEdge - 30, actionY - 2, rightEdge - 16, actionY + 14);
                            SetRect(&copyHitR, rightEdge - 16, actionY - 2, rightEdge + 2,  actionY + 14);

                            if (PtInRect(localPt, &editHitR)) {
                                gEditingMsgIdx = i;
                                SafeCopyString(gEditBuf, gMessages[i].text, sizeof(gEditBuf));
                                gEditBufLen = (short)strlen(gEditBuf);
                                gEditCursorPos = gEditBufLen;
                                gEditSelAnchor = gEditBufLen;
                                gPromptFocused = false;
                                gCaretVisible = true;
                                gLastCaretTick = TickCount();
                                RenderWindow();
                                return;
                            }
                            if (PtInRect(localPt, &copyHitR)) {
                                CopyMessageToClipboard(i);
                                return;
                            }
                        } else {
                            short actionY = curY + bubbleH + 4;
                            Rect retryHitR, copyHitR;
                            SetRect(&retryHitR, aiLeftX + 4,  actionY - 2, aiLeftX + 18, actionY + 14);
                            SetRect(&copyHitR,  aiLeftX + 20, actionY - 2, aiLeftX + 36, actionY + 14);

                            if (PtInRect(localPt, &retryHitR)) {
                                RetryAIResponseAtIndex(i);
                                return;
                            }
                            if (PtInRect(localPt, &copyHitR)) {
                                CopyMessageToClipboard(i);
                                return;
                            }
                        }
                    }
                }
                curY += stepH;
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
        if (key == 'r' || key == 'R') {
            gServerConnected = false;
            gScanningServer  = false;
            RenderWindow();
            return;
        }
        if (key == 'a' || key == 'A') {
            if (gEditingMsgIdx >= 0) {
                gEditSelAnchor = 0;
                gEditCursorPos = gEditBufLen;
                RenderChatAreaOnly();
                return;
            } else if (gPromptFocused && gPromptBuf[0] != '\0') {
                gPromptSelAnchor = 0;
                gCursorPos       = (short)strlen(gPromptBuf);
                RenderPromptBarOnly();
                return;
            }
        }
        if (key == 'c' || key == 'C') {
            if (gEditingMsgIdx >= 0) {
                short sStart, sEnd;
                GetEditSelection(&sStart, &sEnd);
                if (sStart < sEnd) {
                    char selText[1024];
                    short sLen = sEnd - sStart;
                    memcpy(selText, &gEditBuf[sStart], sLen);
                    selText[sLen] = '\0';
                    CopyTextToClipboard(selText);
                } else if (gEditBufLen > 0) {
                    CopyTextToClipboard(gEditBuf);
                }
                return;
            } else if (gPromptFocused) {
                short sStart, sEnd;
                GetPromptSelection(&sStart, &sEnd);
                if (sStart < sEnd) {
                    char selText[1024];
                    short sLen = sEnd - sStart;
                    memcpy(selText, &gPromptBuf[sStart], sLen);
                    selText[sLen] = '\0';
                    CopyTextToClipboard(selText);
                    return;
                }
            }
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
        if (key == 'x' || key == 'X') {
            if (gEditingMsgIdx >= 0) {
                short sStart, sEnd;
                GetEditSelection(&sStart, &sEnd);
                if (sStart < sEnd) {
                    char selText[1024];
                    short sLen = sEnd - sStart;
                    memcpy(selText, &gEditBuf[sStart], sLen);
                    selText[sLen] = '\0';
                    CopyTextToClipboard(selText);
                    DeleteEditSelection();
                    gCaretVisible  = true;
                    gLastCaretTick = TickCount();
                    RenderChatAreaOnly();
                }
                return;
            } else if (gPromptFocused) {
                short sStart, sEnd;
                GetPromptSelection(&sStart, &sEnd);
                if (sStart < sEnd) {
                    char selText[1024];
                    short sLen = sEnd - sStart;
                    short prevVis = gPrevPromptVisLines;
                    memcpy(selText, &gPromptBuf[sStart], sLen);
                    selText[sLen] = '\0';
                    CopyTextToClipboard(selText);
                    DeletePromptSelection();
                    SyncPromptExpansionWithScroll();
                    EnsureCaretVisible();
                    if (gPrevPromptVisLines != prevVis) {
                        RenderChatAreaOnly();
                    } else {
                        RenderPromptBarOnly();
                    }
                }
                return;
            }
        }
        if (key == 'v' || key == 'V') {
            /* Cmd+V pastes text from Clipboard */
            if (gServerConnected && !gAwaitingLogin && !gSearchModalOpen) {
                Handle hScrap = NewHandle(0);
                long offset = 0;
                long scrapLen = GetScrap(hScrap, 'TEXT', &offset);
                if (scrapLen > 0 && hScrap && *hScrap) {
                    char pasteChars[512];
                    short pLen = 0, i;
                    HLock(hScrap);
                    for (i = 0; i < scrapLen && pLen < (short)(sizeof(pasteChars) - 1); i++) {
                        char ch = (*hScrap)[i];
                        if (ch == '\r' || ch == '\n' || ch == '\t') ch = ' ';
                        if (ch >= 32 && ch < 127) {
                            pasteChars[pLen++] = ch;
                        }
                    }
                    pasteChars[pLen] = '\0';
                    HUnlock(hScrap);

                    if (gEditingMsgIdx >= 0) {
                        DeleteEditSelection();
                        if (pLen > 0 && gEditBufLen + pLen < (short)(sizeof(gEditBuf) - 2)) {
                            if (gEditCursorPos < gEditBufLen) {
                                memmove(&gEditBuf[gEditCursorPos + pLen], &gEditBuf[gEditCursorPos], gEditBufLen - gEditCursorPos + 1);
                            }
                            memcpy(&gEditBuf[gEditCursorPos], pasteChars, pLen);
                            gEditCursorPos += pLen;
                            gEditSelAnchor  = gEditCursorPos;
                            gEditBufLen    += pLen;
                            gEditBuf[gEditBufLen] = '\0';
                        }
                        gCaretVisible  = true;
                        gLastCaretTick = TickCount();
                        RenderChatAreaOnly();
                    } else if (gPromptFocused) {
                        DeletePromptSelection();
                        short curLen = (short)strlen(gPromptBuf);
                        if (pLen > 0 && curLen + pLen < (short)(sizeof(gPromptBuf) - 2)) {
                            short prevVis = gPrevPromptVisLines;
                            if (gCursorPos < curLen) {
                                memmove(&gPromptBuf[gCursorPos + pLen], &gPromptBuf[gCursorPos], curLen - gCursorPos + 1);
                            }
                            memcpy(&gPromptBuf[gCursorPos], pasteChars, pLen);
                            gCursorPos += pLen;
                            gPromptSelAnchor = gCursorPos;
                            gPromptBuf[curLen + pLen] = '\0';

                            gShowPlaceholder = false;
                            gCaretVisible    = true;
                            gLastCaretTick   = TickCount();
                            SyncPromptExpansionWithScroll();
                            EnsureCaretVisible();
                            if (gPrevPromptVisLines != prevVis) {
                                RenderChatAreaOnly();
                            } else {
                                RenderPromptBarOnly();
                            }
                        }
                    }
                }
                if (hScrap) DisposeHandle(hScrap);
            }
            return;
        }
        if (key == '1') { SelectProviderOrTier("gemini", NULL); return; }
        if (key == '2') { SelectProviderOrTier("chatgpt", NULL); return; }
        if (key == '3') { SelectProviderOrTier("claude", NULL); return; }
        if (key == '4') { SelectProviderOrTier("groq", NULL); return; }
        if (key == 't' || key == 'T') {
            char url[64];
            gReadRepliesTTS = !gReadRepliesTTS;
            SyncSpeechMenuState();
            SaveMacAIPrefs();
            sprintf(url, "/api/macai/voice-settings?enabled=%d", gReadRepliesTTS ? 1 : 0);
            NetHttpGet(url, gNetBuf, NET_BUF_SIZE);
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

    /* In-place Speech Bubble editing */
    if (gEditingMsgIdx >= 0) {
        if (key == 27) { /* Escape: Cancel edit */
            gEditingMsgIdx = -1;
            gPromptFocused = true;
            RenderWindow();
            return;
        }
        if (key == '\r' || key == 3) { /* Enter: Save and re-run */
            if (gEditBufLen > 0) {
                SubmitEditedMessage(gEditingMsgIdx);
            }
            return;
        }
        if (key == 28) { /* Left arrow */
            short sStart, sEnd;
            GetEditSelection(&sStart, &sEnd);
            if (sStart < sEnd) {
                gEditCursorPos = sStart;
                gEditSelAnchor = sStart;
            } else if (gEditCursorPos > 0) {
                gEditCursorPos--;
                gEditSelAnchor = gEditCursorPos;
            }
            gCaretVisible  = true;
            gLastCaretTick = TickCount();
            RenderChatAreaOnly();
            return;
        }
        if (key == 29) { /* Right arrow */
            short sStart, sEnd;
            GetEditSelection(&sStart, &sEnd);
            if (sStart < sEnd) {
                gEditCursorPos = sEnd;
                gEditSelAnchor = sEnd;
            } else if (gEditCursorPos < gEditBufLen) {
                gEditCursorPos++;
                gEditSelAnchor = gEditCursorPos;
            }
            gCaretVisible  = true;
            gLastCaretTick = TickCount();
            RenderChatAreaOnly();
            return;
        }
        if (key == 30 || key == 31) { /* Up/Down arrow */
            static char tmpW[MAX_WRAPPED_LINES][96];
            short userMaxW = (gWinW - (gSidebarCollapsed ? 0 : (SIDEBAR_W + 1))) - 90;
            short nLines, cLine = 0, cCol = 0, L;
            if (userMaxW > 240) userMaxW = 240;
            if (userMaxW < 120) userMaxW = 120;
            nLines = WrapMessageLinesEx(gEditBuf, userMaxW, tmpW, gEditLineStarts, MAX_WRAPPED_LINES);
            for (L = 0; L < nLines; L++) {
                short sIdx = gEditLineStarts[L];
                short lLen = (short)strlen(tmpW[L]);
                if (gEditCursorPos >= sIdx && (gEditCursorPos <= sIdx + lLen || L == nLines - 1)) {
                    cLine = L;
                    cCol  = gEditCursorPos - sIdx;
                    if (cCol > lLen) cCol = lLen;
                    break;
                }
            }
            if (key == 30) {
                if (cLine > 0) {
                    short targetCol = cCol;
                    short pLen = (short)strlen(tmpW[cLine - 1]);
                    if (targetCol > pLen) targetCol = pLen;
                    gEditCursorPos = gEditLineStarts[cLine - 1] + targetCol;
                } else {
                    gEditCursorPos = 0;
                }
            } else {
                if (cLine < nLines - 1) {
                    short targetCol = cCol;
                    short nextLen = (short)strlen(tmpW[cLine + 1]);
                    if (targetCol > nextLen) targetCol = nextLen;
                    gEditCursorPos = gEditLineStarts[cLine + 1] + targetCol;
                } else {
                    gEditCursorPos = gEditBufLen;
                }
            }
            gEditSelAnchor = gEditCursorPos;
            gCaretVisible  = true;
            gLastCaretTick = TickCount();
            RenderChatAreaOnly();
            return;
        }
        if (key == 8) { /* Backspace */
            if (DeleteEditSelection()) {
                gCaretVisible  = true;
                gLastCaretTick = TickCount();
                RenderChatAreaOnly();
                return;
            } else if (gEditCursorPos > 0) {
                memmove(&gEditBuf[gEditCursorPos - 1], &gEditBuf[gEditCursorPos], gEditBufLen - gEditCursorPos + 1);
                gEditCursorPos--;
                gEditSelAnchor = gEditCursorPos;
                gEditBufLen--;
                gCaretVisible  = true;
                gLastCaretTick = TickCount();
                RenderChatAreaOnly();
                return;
            }
            return;
        }
        if (key >= 32 && key < 127) {
            DeleteEditSelection();
            if (gEditBufLen < (short)(sizeof(gEditBuf) - 2)) {
                if (gEditCursorPos < gEditBufLen) {
                    memmove(&gEditBuf[gEditCursorPos + 1], &gEditBuf[gEditCursorPos], gEditBufLen - gEditCursorPos + 1);
                }
                gEditBuf[gEditCursorPos] = key;
                gEditCursorPos++;
                gEditSelAnchor = gEditCursorPos;
                gEditBufLen++;
                gEditBuf[gEditBufLen] = '\0';
                gCaretVisible  = true;
                gLastCaretTick = TickCount();
                RenderChatAreaOnly();
                return;
            }
            return;
        }
        return;
    }

    /* Arrow keys for prompt navigation or scrolling chat */
    if (key == 28) { /* Left arrow */
        if (gPromptFocused) {
            short sStart, sEnd;
            GetPromptSelection(&sStart, &sEnd);
            if (sStart < sEnd) {
                gCursorPos       = sStart;
                gPromptSelAnchor = sStart;
            } else if (gCursorPos > 0) {
                gCursorPos--;
                gPromptSelAnchor = gCursorPos;
            }
            gCaretVisible  = true;
            gLastCaretTick = TickCount();
            EnsureCaretVisible();
            RenderPromptBarOnly();
            return;
        }
    }
    if (key == 29) { /* Right arrow */
        if (gPromptFocused) {
            short bLen = (short)strlen(gPromptBuf);
            short sStart, sEnd;
            GetPromptSelection(&sStart, &sEnd);
            if (sStart < sEnd) {
                gCursorPos       = sEnd;
                gPromptSelAnchor = sEnd;
            } else if (gCursorPos < bLen) {
                gCursorPos++;
                gPromptSelAnchor = gCursorPos;
            }
            gCaretVisible  = true;
            gLastCaretTick = TickCount();
            EnsureCaretVisible();
            RenderPromptBarOnly();
            return;
        }
    }
    if (key == 30) { /* Up arrow */
        if (gPromptFocused) {
            static char pLines[32][96];
            short visLines = 1;
            short totalLines = WrapPromptInputLines(pLines, 32, &visLines);
            if (totalLines > 1) {
                short cLine, colPx;
                GetPromptCaretPos(&cLine, &colPx, pLines, totalLines);
                if (cLine > 0) {
                    short prevLineLen = (short)strlen(pLines[cLine - 1]);
                    short prevStart   = gPromptLineStarts[cLine - 1];
                    short targetCol   = colPx / MONACO_CHAR_W;
                    if (targetCol > prevLineLen) targetCol = prevLineLen;
                    gCursorPos = prevStart + targetCol;
                } else {
                    gCursorPos = 0;
                }
                gPromptSelAnchor = gCursorPos;
                gCaretVisible    = true;
                gLastCaretTick   = TickCount();
                EnsureCaretVisible();
                RenderPromptBarOnly();
                return;
            }
        }
        gMsgScrollY -= 24;
        if (gMsgScrollY < 0) gMsgScrollY = 0;
        RenderChatAreaOnly();
        return;
    }
    if (key == 31) { /* Down arrow */
        if (gPromptFocused) {
            static char pLines[32][96];
            short visLines = 1;
            short totalLines = WrapPromptInputLines(pLines, 32, &visLines);
            if (totalLines > 1) {
                short cLine, colPx;
                GetPromptCaretPos(&cLine, &colPx, pLines, totalLines);
                if (cLine < totalLines - 1) {
                    short nextLineLen = (short)strlen(pLines[cLine + 1]);
                    short nextStart   = gPromptLineStarts[cLine + 1];
                    short targetCol   = colPx / MONACO_CHAR_W;
                    if (targetCol > nextLineLen) targetCol = nextLineLen;
                    gCursorPos = nextStart + targetCol;
                } else {
                    gCursorPos = (short)strlen(gPromptBuf);
                }
                gPromptSelAnchor = gCursorPos;
                gCaretVisible    = true;
                gLastCaretTick   = TickCount();
                EnsureCaretVisible();
                RenderPromptBarOnly();
                return;
            }
        }
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
                SafeCopyString(toSend, gPromptBuf, sizeof(toSend));
                gPromptBuf[0]       = '\0';
                gCursorPos          = 0;
                gPromptSelAnchor    = 0;
                gPromptScrollLine   = 0;
                gPrevPromptVisLines = 1;
                gCaretVisible       = true;
                gLastCaretTick      = TickCount();
                SendPromptToAI(toSend, false);
            }
        } else if (key == 8) {
            short prevVis = gPrevPromptVisLines;
            if (DeletePromptSelection()) {
                gPromptFocused = true;
                gCaretVisible  = true;
                gLastCaretTick = TickCount();
                SyncPromptExpansionWithScroll();
                EnsureCaretVisible();
                if (gPrevPromptVisLines != prevVis) {
                    RenderChatAreaOnly();
                } else {
                    RenderPromptBarOnly();
                }
            } else if (gCursorPos > 0) {
                memmove(&gPromptBuf[gCursorPos - 1], &gPromptBuf[gCursorPos], len - gCursorPos + 1);
                gCursorPos--;
                gPromptSelAnchor = gCursorPos;
                gPromptFocused   = true;
                gCaretVisible    = true;
                gLastCaretTick   = TickCount();
                SyncPromptExpansionWithScroll();
                EnsureCaretVisible();
                if (gPrevPromptVisLines != prevVis) {
                    RenderChatAreaOnly();
                } else {
                    RenderPromptBarOnly();
                }
            }
        } else if (key >= 32 && key < 127) {
            short prevVis = gPrevPromptVisLines;
            DeletePromptSelection();
            len = (short)strlen(gPromptBuf);
            if (len < (short)(sizeof(gPromptBuf) - 2)) {
                if (gCursorPos < len) {
                    memmove(&gPromptBuf[gCursorPos + 1], &gPromptBuf[gCursorPos], len - gCursorPos + 1);
                }
                gPromptBuf[gCursorPos] = key;
                gCursorPos++;
                gPromptSelAnchor = gCursorPos;
                gPromptBuf[len + 1] = '\0';
            }
            gShowPlaceholder = false;
            gPromptFocused   = true;
            gCaretVisible    = true;
            gLastCaretTick   = TickCount();
            SyncPromptExpansionWithScroll();
            EnsureCaretVisible();
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
            case iQuit:
                gRunning = false;
                break;
        }
    } else if (menuID == mSettings) {
        switch (itemID) {
            case iServerConnection:
                gServerConnected = false;
                gScanningServer  = false;
                RenderWindow();
                break;
            case iUseGemini:        SelectProvider("gemini"); break;
            case iUseOpenAI:        SelectProvider("chatgpt"); break;
            case iUseClaude:        SelectProvider("claude"); break;
            case iUseGroq:          SelectProvider("groq"); break;
            case iModel1:           SelectModel(0); break;
            case iModel2:           SelectModel(1); break;
            case iModel3:           SelectModel(2); break;
            case iModel4:           SelectModel(3); break;
            case iModelCustom:      SelectModel(4); break;
            case iThinkingFast:     SelectThinkingLevel(0); break;
            case iThinkingBalanced: SelectThinkingLevel(1); break;
            case iThinkingThinking: SelectThinkingLevel(2); break;
        }
    } else if (menuID == mSpeech) {
        switch (itemID) {
            case iSpeechReadAloud: {
                char url[64];
                gReadRepliesTTS = !gReadRepliesTTS;
                SyncSpeechMenuState();
                SaveMacAIPrefs();
                sprintf(url, "/api/macai/voice-settings?enabled=%d", gReadRepliesTTS ? 1 : 0);
                NetHttpGet(url, gNetBuf, NET_BUF_SIZE);
                break;
            }
            case iSpeechVoiceAlloy:
            case iSpeechVoiceEcho:
            case iSpeechVoiceFable:
            case iSpeechVoiceOnyx:
            case iSpeechVoiceNova:
            case iSpeechVoiceShimmer:
            case iSpeechVoiceRetro:
                SelectVoice(itemID - iSpeechVoiceAlloy);
                break;
        }
    }
    HiliteMenu(0);
}

static void NetworkIdleProc(void)
{
    EventRecord ev;

    if (gWaitingForAI) {
        unsigned long elapsed = TickCount() - gWaitStartTick;
        const char *newStatus = NULL;
        char apiStatusBuf[96];

        if (elapsed >= 26UL * 60UL) {
            sprintf(apiStatusBuf, "Still thinking.. The %s API is still responding",
                    GetProviderDisplayName(gActiveProvider));
            newStatus = apiStatusBuf;
        } else if (elapsed >= 22UL * 60UL) {
            newStatus = "Hmm this is taking a while, MacCompanion server is still up!";
        } else if (elapsed >= 20UL * 60UL) {
            newStatus = "Still thinking...";
        }

        if (newStatus && strcmp(gWaitStatusText, newStatus) != 0) {
            SafeCopyString(gWaitStatusText, newStatus, sizeof(gWaitStatusText));
            RenderWindow();
        }
    }

    if (GetNextEvent(mDownMask | keyDownMask | updateMask | activMask, &ev)) {
        switch (ev.what) {
            case mouseDown: {
                WindowPtr hitWin = NULL;
                short part = FindWindow(ev.where, &hitWin);
                if (part == inMenuBar) {
                    long mChoice;
                    SyncAIMenuState();
                    mChoice = MenuSelect(ev.where);
                    if (HiWord(mChoice) == mFile && LoWord(mChoice) == iQuit) {
                        gRunning = false;
                        gUserCancelledWait = true;
                        NetAbortCurrentOperation();
                    }
                    HiliteMenu(0);
                } else if (part == inSysWindow) {
                    SystemClick(&ev, hitWin);
                } else if (hitWin == gMainWindow) {
                    if (part == inDrag) {
                        DragWindow(gMainWindow, ev.where, &qd.screenBits.bounds);
                    } else if (part == inGoAway) {
                        if (TrackGoAway(gMainWindow, ev.where)) {
                            gRunning = false;
                            gUserCancelledWait = true;
                            NetAbortCurrentOperation();
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
                    }
                }
                break;
            }
            case keyDown: {
                char key = (char)(ev.message & charCodeMask);
                if (ev.modifiers & cmdKey) {
                    if (key == 'q' || key == 'Q') {
                        gRunning = false;
                        gUserCancelledWait = true;
                        NetAbortCurrentOperation();
                    } else if (key == '.') {
                        gUserCancelledWait = true;
                        NetAbortCurrentOperation();
                    }
                }
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
    NetSetIdleCallback(NetworkIdleProc);

    /* Load saved preferences (IP, provider, model, thinking level) */
    LoadMacAIPrefs(NULL);

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
        if (gCopiedMsgIdx >= 0 && (TickCount() - gCopiedTick) >= 120) {
            gCopiedMsgIdx = -1;
            if (gServerConnected && !gAwaitingLogin && !gSearchModalOpen && !gAboutModalOpen && !gTierMenuOpen && !gProviderMenuOpen) {
                RenderChatAreaOnly();
            }
        }
        if (gSaveSuccessMsgIdx >= 0 && (TickCount() - gSaveSuccessTick) >= 120) {
            gSaveSuccessMsgIdx = -1;
            if (gServerConnected && !gAwaitingLogin && !gSearchModalOpen && !gAboutModalOpen && !gTierMenuOpen && !gProviderMenuOpen) {
                RenderChatAreaOnly();
            }
        }
        if ((gPromptFocused || gEditingMsgIdx >= 0) && gServerConnected && !gAwaitingLogin && !gSearchModalOpen && !gAboutModalOpen && !gTierMenuOpen && !gProviderMenuOpen) {
            unsigned long nowTicks = TickCount();
            if ((nowTicks - gLastCaretTick) >= 30) {
                gLastCaretTick = nowTicks;
                gCaretVisible = !gCaretVisible;
                if (gEditingMsgIdx >= 0) {
                    RenderChatAreaOnly();
                } else {
                    RenderPromptBarOnly();
                }
            }
        }

        /* Mouse hover tracking over message bubbles */
        if (gServerConnected && !gAwaitingLogin && !gSearchModalOpen && !gAboutModalOpen && !gTierMenuOpen && !gProviderMenuOpen && !Button()) {
            Point pt;
            GetMouse(&pt);
            short hov = HitTestMessageBubble(pt);
            if (hov != gHoverBubbleIdx) {
                gHoverBubbleIdx = hov;
                RenderChatAreaOnly();
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

    ClearAllMessages();
    AudioStreamStop();
    if (gOffscreenMem) DisposePtr(gOffscreenMem);
    if (gNetBuf) DisposePtr(gNetBuf);
    return 0;
}
