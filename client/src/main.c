/*
 * SpotiMac — Native 68k Macintosh Spotify Client
 * Version 0.3.8 ("Partylizer II")
 * Compiled with Retro68 + Apple Universal Interfaces 3.4.2
 *
 * UI Design: 512 x 322 flush canvas below the 20px Mac OS menu bar
 * (exact 512 x 342 Compact Mac screen dimensions) + 192 x 68 Mini-Player.
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
#include <Files.h>
#include <Folders.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "version.h"
#include "assets.h"
#include "network.h"
#include "visualizer.h"

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

/* Pre-connection Server Screen states (Server1.png, Server2.png, Server3.png, login-error.png) */
#define CONN_SEARCHING      1
#define CONN_FAILED         2
#define CONN_FOUND          3
#define CONN_READY          4
#define CONN_LOGIN_ERROR    5

/* Hover / Pressed button identifiers (button-states-2.png + Mini-player.png) */
#define BTN_NONE            0
#define BTN_SHUFFLE         1
#define BTN_PREV            2
#define BTN_PLAY            3
#define BTN_NEXT            4
#define BTN_REPEAT          5
#define BTN_SPEAKER         6
#define BTN_TIMELINE        7
#define BTN_VOLUME          8
#define BTN_LIKE            9
#define BTN_COLLAPSE        10
#define BTN_VIZ_TOGGLE      11
#define BTN_VIZ_1           12
#define BTN_VIZ_2           13
#define BTN_VIZ_3           14
#define BTN_VIZ_4           15
#define BTN_VIZ_5           16
#define BTN_VIZ_6           17
#define BTN_VIZ_7           18
#define BTN_VIZ_8           19
#define BTN_VIZ_9           20
#define BTN_VIZ_10          21

#define MINI_W              192
#define MINI_H              68
#define MINI_FS_X           160  /* (512 - 192) / 2: centered horizontally in fullscreen */
#define MINI_FS_Y           239  /* 322 - 15 - 68: 15px from bottom in fullscreen */

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
    long durationMs;
    char durationMmSs[12];
    char uri[96];
} RightItem;

typedef struct {
    char kind[16];     /* "TRACK", "ALBUM", "PLAYLIST" */
    char id[64];
    char title[64];
    char subtitle[64]; /* Artist or Owner */
    long durationMs;
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
    char slotId[24];
    char title[32];
    char line1[32];
    char line2[32];
    char imageUrl[160];
} ForYouItem;

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
static WindowPtr       gMiniWindow        = NULL;
static Boolean         gMiniPlayerMode    = false;
static Boolean         gMiniWinPositioned = false;
static GrafPort        gOffscreenPort;
static Ptr             gOffscreenBits     = NULL;
static Boolean         gOffscreenReady    = false;

static Boolean         gRunning           = true;
static short           gConnState         = CONN_SEARCHING;
static unsigned long   gFoundStartTicks   = 0;
static short           gFoundCountdownSec = 3;
static Boolean         gInitialDataLoaded = false;

static Boolean         gHomeMode          = true;  /* false = Playlists (note icon), true = Home (home icon) */

/* Search state */
#define MAX_SEARCH_HISTORY  5
static Boolean         gSearchFocused         = false;
static Boolean         gSearchViewOpen        = false;
static Boolean         gSearchHistoryOpen     = false;
static char            gSearchQuery[48]       = "";
static SearchItem      gSearchItems[MAX_SEARCH_ITEMS];
static short           gSearchCount           = 0;
static short           gSearchScroll          = 0;
static short           gSearchHoverRow        = -1;
static char            gSearchHistory[MAX_SEARCH_HISTORY][48];
static short           gSearchHistoryCount    = 0;
static short           gSearchHistoryHoverRow = -1;

/* Speaker Chooser Popover & Persisted Speaker state */
static Boolean         gSpeakerPopupOpen      = false;
static DeviceItem      gDevices[MAX_DEVICES];
static short           gDeviceCount           = 0;
static char            gSavedSpeakerId[64]    = "";
static char            gSavedSpeakerName[48]  = "";
static unsigned long   gSpeakerLockUntilTicks = 0;

/* Server IP Input state */
static char            gIpInput[32]           = "10.0.2.2";

/* About Box Modal state */
static Boolean         gAboutModalOpen        = false;

/* Button Hover & Pressed states */
static short           gHoverBtn              = BTN_NONE;
static short           gPressedBtn            = BTN_NONE;
static Boolean         gSuppressPlayHover     = false;

/* Left & Right List state */
static LeftItem        gLeftItems[MAX_LEFT_ITEMS];
static short           gLeftCount             = 0;
static short           gLeftSelected          = 0;
static short           gLeftScroll            = 0;
static Boolean         gHasPinnedLeftItem     = false;
static LeftItem        gPinnedLeftItem;

static RightItem       gRightItems[MAX_RIGHT_ITEMS];
static short           gRightCount        = 0;
static short           gRightSelected     = -1;
static short           gRightScroll       = 0;
static char            gActiveContextUri[96] = "";
static char            gRightHeaderLabel[24] = "Tracks";
static Boolean         gHasQueueSeparator        = false;
static short           gQueueTailFetchedForCount = -1;
static unsigned long   gLastQueueTailFetchTicks  = 0;

/* Double-click detection */
static unsigned long   gLastClickTicks     = 0;
static short           gLastClickedRow     = -1;
static unsigned long   gLastLeftClickTicks = 0;
static short           gLastLeftClickedRow = -1;

/* Now Playing, Artwork, "For You" & Visualizer state */
static NowPlayingState gNowPlaying;
static Boolean         gTimelineFrozen       = false;
static unsigned long   gTrackLockUntilTicks  = 0;
static char            gLoadedArtTrackId[64] = "";
static unsigned char   gCurrentArtworkBits[5200];
static unsigned char   gVisualizerBits[5200];
static Boolean         gVisualizerOpen       = false;
static Boolean         gVizFullscreen        = false;
static Boolean         gVizHover             = false;
static short           gVizMode              = VIZ_MODE_EQ;
static unsigned long   gLastVizTicks         = 0;
static unsigned short  gAudioSampleRateHz    = 22255;

#define FOR_YOU_COUNT  4
static Boolean         gForYouViewOpen       = true;
static ForYouItem      gForYouItems[FOR_YOU_COUNT];
static unsigned char   gForYouIconBits[FOR_YOU_COUNT][282];

/* Timing for periodic state & tick-anchored timeline updates */
static unsigned long   gLastPollTicks        = 0;
static unsigned long   gTimelineAnchorTicks  = 0;
static long            gTimelineAnchorMs     = 0;
static unsigned long   gSeekLockUntilTicks   = 0;
static Boolean         gPauseSynced          = false;

/* Shared scratch buffer for HTTP responses */
#define HTTP_BUF_SIZE  16384
static char           *gHttpBuf           = NULL;

/* Forward declarations */
static void RenderCanvas(void);
static void ConnectAndLoadAll(void);

/* ============================================================================
 * Persistent Server Preferences ("SpotiMac Prefs" in System Folder:Preferences)
 * ============================================================================ */

#define PREFS_MAGIC         0x4D435331UL /* 'MCS1' */

typedef struct {
    unsigned long magic;
    ip_addr       serverIp;
    unsigned char seenServerScreen;
    unsigned char lastVizMode;
    unsigned char historyCount;
    unsigned char reserved;
    char          lastSpeakerId[64];
    char          lastSpeakerName[48];
    char          searchHistory[MAX_SEARCH_HISTORY][48];
} SpotiMacPrefs;

static void ApplyLoadedPrefs(const SpotiMacPrefs *prefs, long bytesRead, ip_addr *outServerIp)
{
    short i;
    *outServerIp = prefs->serverIp;
    if (prefs->lastVizMode < VIZ_MODE_COUNT) {
        gVizMode = prefs->lastVizMode;
    }
    if (bytesRead >= (long)sizeof(SpotiMacPrefs)) {
        if (prefs->lastSpeakerId[0] != '\0') {
            memcpy(gSavedSpeakerId, prefs->lastSpeakerId, sizeof(gSavedSpeakerId) - 1);
            gSavedSpeakerId[sizeof(gSavedSpeakerId) - 1] = '\0';
        }
        if (prefs->lastSpeakerName[0] != '\0') {
            memcpy(gSavedSpeakerName, prefs->lastSpeakerName, sizeof(gSavedSpeakerName) - 1);
            gSavedSpeakerName[sizeof(gSavedSpeakerName) - 1] = '\0';
            memcpy(gNowPlaying.deviceName, gSavedSpeakerName, sizeof(gNowPlaying.deviceName));
        }
        gSearchHistoryCount = 0;
        for (i = 0; i < (short)prefs->historyCount && i < MAX_SEARCH_HISTORY; i++) {
            if (prefs->searchHistory[i][0] != '\0') {
                memcpy(gSearchHistory[gSearchHistoryCount], prefs->searchHistory[i], 47);
                gSearchHistory[gSearchHistoryCount][47] = '\0';
                gSearchHistoryCount++;
            }
        }
    }
}

static Boolean GetPrefsFileSpec(FSSpec *outSpec, Boolean legacyName)
{
    short vRefNum = 0;
    long  dirID   = 0;
    OSErr err;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &vRefNum, &dirID);
    if (err != noErr) return false;

    err = FSMakeFSSpec(vRefNum, dirID, legacyName ? "\pMaciSpot Prefs" : "\pSpotiMac Prefs", outSpec);
    return (err == noErr || err == fnfErr);
}

static Boolean LoadSavedServerPrefs(ip_addr *outServerIp)
{
    FSSpec        spec;
    short         refNum = 0;
    long          count  = sizeof(SpotiMacPrefs);
    SpotiMacPrefs prefs;
    OSErr         err;
    FILE         *fp;
    short         attempt;

    if (!outServerIp) return false;
    *outServerIp = 0;

    for (attempt = 0; attempt < 2; attempt++) {
        if (GetPrefsFileSpec(&spec, attempt == 1)) {
            err = FSpOpenDF(&spec, fsRdPerm, &refNum);
            if (err == noErr) {
                count = sizeof(SpotiMacPrefs);
                memset(&prefs, 0, sizeof(prefs));
                err = FSRead(refNum, &count, &prefs);
                FSClose(refNum);
                if ((err == noErr || err == eofErr) &&
                    count >= 12 &&
                    prefs.magic == PREFS_MAGIC &&
                    prefs.seenServerScreen != 0 &&
                    prefs.serverIp != 0) {
                    ApplyLoadedPrefs(&prefs, count, outServerIp);
                    return true;
                }
            }
        }
    }

    /* Fallback: check local directory file if Preferences folder was unavailable */
    fp = fopen("SpotiMac Prefs", "rb");
    if (!fp) fp = fopen("MaciSpot Prefs", "rb");
    if (fp) {
        size_t nRead;
        memset(&prefs, 0, sizeof(prefs));
        nRead = fread(&prefs, 1, sizeof(SpotiMacPrefs), fp);
        fclose(fp);
        if (nRead >= 12 &&
            prefs.magic == PREFS_MAGIC &&
            prefs.seenServerScreen != 0 &&
            prefs.serverIp != 0) {
            ApplyLoadedPrefs(&prefs, (long)nRead, outServerIp);
            return true;
        }
    }
    return false;
}

static void SaveServerPrefs(ip_addr serverIp)
{
    FSSpec        spec;
    short         refNum = 0;
    long          count  = sizeof(SpotiMacPrefs);
    SpotiMacPrefs prefs;
    OSErr         err;
    FILE         *fp;
    short         i;

    if (serverIp == 0) return;

    memset(&prefs, 0, sizeof(prefs));
    prefs.magic            = PREFS_MAGIC;
    prefs.serverIp         = serverIp;
    prefs.seenServerScreen = 1;
    prefs.lastVizMode      = (unsigned char)(gVizMode % VIZ_MODE_COUNT);
    prefs.historyCount     = (unsigned char)gSearchHistoryCount;
    memcpy(prefs.lastSpeakerId,   gSavedSpeakerId,   sizeof(prefs.lastSpeakerId) - 1);
    memcpy(prefs.lastSpeakerName, gSavedSpeakerName, sizeof(prefs.lastSpeakerName) - 1);
    for (i = 0; i < gSearchHistoryCount && i < MAX_SEARCH_HISTORY; i++) {
        memcpy(prefs.searchHistory[i], gSearchHistory[i], 47);
    }

    if (GetPrefsFileSpec(&spec, false)) {
        FSpCreate(&spec, 'MCSP', 'pref', smSystemScript);
        err = FSpOpenDF(&spec, fsWrPerm, &refNum);
        if (err == noErr) {
            SetEOF(refNum, 0);
            FSWrite(refNum, &count, &prefs);
            FSClose(refNum);
            return;
        }
    }

    fp = fopen("SpotiMac Prefs", "wb");
    if (fp) {
        fwrite(&prefs, 1, sizeof(SpotiMacPrefs), fp);
        fclose(fp);
    }
}

static Boolean StrCaseEqual(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb + 32);
        if (ca != cb) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static void AddQueryToSearchHistory(const char *query)
{
    short i, existingIdx = -1;
    char clean[48];

    if (!query || query[0] == '\0') return;
    strncpy(clean, query, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';

    for (i = 0; i < gSearchHistoryCount; i++) {
        if (StrCaseEqual(gSearchHistory[i], clean)) {
            existingIdx = i;
            break;
        }
    }

    if (existingIdx == 0) {
        memcpy(gSearchHistory[0], clean, sizeof(clean));
        SaveServerPrefs(NetGetServerIP());
        return;
    }

    if (existingIdx > 0) {
        for (i = existingIdx; i > 0; i--) {
            memcpy(gSearchHistory[i], gSearchHistory[i - 1], 48);
        }
        memcpy(gSearchHistory[0], clean, 48);
    } else {
        if (gSearchHistoryCount < MAX_SEARCH_HISTORY) {
            gSearchHistoryCount++;
        }
        for (i = gSearchHistoryCount - 1; i > 0; i--) {
            memcpy(gSearchHistory[i], gSearchHistory[i - 1], 48);
        }
        memcpy(gSearchHistory[0], clean, 48);
    }

    SaveServerPrefs(NetGetServerIP());
}

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
        if (c < 0x20 || c == 0x7F) {
            dst[i] = ' ';
        } else {
            dst[i] = (char)c;
        }
        i++;
    }
    dst[i] = '\0';
}

static char MapUnicodeToMacRomanOrAscii(unsigned int cp)
{
    switch (cp) {
        /* Swedish / Nordic / German / French / Spanish MacRoman characters */
        case 0x00C4: return (char)0x80; /* Ä */
        case 0x00C5: return (char)0x81; /* Å */
        case 0x00C7: return (char)0x82; /* Ç */
        case 0x00C9: return (char)0x83; /* É */
        case 0x00D1: return (char)0x84; /* Ñ */
        case 0x00D6: return (char)0x85; /* Ö */
        case 0x00DC: return (char)0x86; /* Ü */
        case 0x00E1: return (char)0x87; /* á */
        case 0x00E0: return (char)0x88; /* à */
        case 0x00E2: return (char)0x89; /* â */
        case 0x00E4: return (char)0x8A; /* ä */
        case 0x00E3: return (char)0x8B; /* ã */
        case 0x00E5: return (char)0x8C; /* å */
        case 0x00E7: return (char)0x8D; /* ç */
        case 0x00E9: return (char)0x8E; /* é */
        case 0x00E8: return (char)0x8F; /* è */
        case 0x00EA: return (char)0x90; /* ê */
        case 0x00EB: return (char)0x91; /* ë */
        case 0x00ED: return (char)0x92; /* í */
        case 0x00EC: return (char)0x93; /* ì */
        case 0x00EE: return (char)0x94; /* î */
        case 0x00EF: return (char)0x95; /* ï */
        case 0x00F1: return (char)0x96; /* ñ */
        case 0x00F3: return (char)0x97; /* ó */
        case 0x00F2: return (char)0x98; /* ò */
        case 0x00F4: return (char)0x99; /* ô */
        case 0x00F6: return (char)0x9A; /* ö */
        case 0x00F5: return (char)0x9B; /* õ */
        case 0x00FA: return (char)0x9C; /* ú */
        case 0x00F9: return (char)0x9D; /* ù */
        case 0x00FB: return (char)0x9E; /* û */
        case 0x00FC: return (char)0x9F; /* ü */
        case 0x00DF: return (char)0xA7; /* ß */
        case 0x00C6: return (char)0xAE; /* Æ */
        case 0x00D8: return (char)0xAF; /* Ø */
        case 0x00E6: return (char)0xBE; /* æ */
        case 0x00F8: return (char)0xBF; /* ø */
        case 0x00C0: return (char)0xCB; /* À */
        case 0x00C3: return (char)0xCC; /* Ã */
        case 0x00D5: return (char)0xCD; /* Õ */
        case 0x0152: return (char)0xCE; /* Œ */
        case 0x0153: return (char)0xCF; /* œ */
        case 0x00D2: return (char)0xF1; /* Ò */
        case 0x00DA: return (char)0xF2; /* Ú */
        case 0x00DB: return (char)0xF3; /* Û */
        case 0x00D9: return (char)0xF4; /* Ù */
        /* Additional Latin-1 & Latin Extended-A transliterations */
        case 0x00C1: case 0x00C2: case 0x0100: case 0x0102: case 0x0104: return 'A';
        case 0x0101: case 0x0103: case 0x0105: return 'a';
        case 0x0106: case 0x0108: case 0x010A: case 0x010C: return 'C';
        case 0x0107: case 0x0109: case 0x010B: case 0x010D: return 'c';
        case 0x00D0: case 0x010E: case 0x0110: return 'D';
        case 0x00F0: case 0x010F: case 0x0111: return 'd';
        case 0x00C8: case 0x00CA: case 0x00CB: case 0x0112: case 0x0116: case 0x0118: case 0x011A: return 'E';
        case 0x0113: case 0x0117: case 0x0119: case 0x011B: return 'e';
        case 0x011E: return 'G';
        case 0x011F: return 'g';
        case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF: case 0x012A: case 0x0130: return 'I';
        case 0x012B: case 0x0131: return 'i';
        case 0x0141: return 'L';
        case 0x0142: return 'l';
        case 0x0143: case 0x0147: return 'N';
        case 0x0144: case 0x0148: return 'n';
        case 0x00D3: case 0x00D4: case 0x014C: case 0x0150: return 'O';
        case 0x014D: case 0x0151: return 'o';
        case 0x0158: return 'R';
        case 0x0159: return 'r';
        case 0x015A: case 0x015E: case 0x0160: return 'S';
        case 0x015B: case 0x015F: case 0x0161: return 's';
        case 0x0162: case 0x0164: return 'T';
        case 0x0163: case 0x0165: return 't';
        case 0x016A: case 0x016E: case 0x0170: return 'U';
        case 0x016B: case 0x016F: case 0x0171: return 'u';
        case 0x00DD: case 0x0178: return 'Y';
        case 0x00FD: case 0x00FF: return 'y';
        case 0x0179: case 0x017B: case 0x017D: return 'Z';
        case 0x017A: case 0x017C: case 0x017E: return 'z';
        case 0x2013: case 0x2014: return '-';
        case 0x2018: case 0x2019: return '\'';
        case 0x201C: case 0x201D: return '"';
        default: return 0;
    }
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
                unsigned int cp = ((c & 0x1F) << 6) | ((unsigned char)p[1] & 0x3F);
                char mapped = MapUnicodeToMacRomanOrAscii(cp);
                if (mapped != 0) {
                    outBuf[outIdx++] = mapped;
                }
                p++;
            } else if ((c & 0xF0) == 0xE0 &&
                       ((unsigned char)p[1] & 0xC0) == 0x80 &&
                       ((unsigned char)p[2] & 0xC0) == 0x80) {
                unsigned int cp = ((c & 0x0F) << 12) |
                                  (((unsigned char)p[1] & 0x3F) << 6) |
                                  ((unsigned char)p[2] & 0x3F);
                if (cp == 0x2026) {
                    if (outIdx < (maxOut - 3)) {
                        outBuf[outIdx++] = '.';
                        outBuf[outIdx++] = '.';
                        outBuf[outIdx++] = '.';
                    }
                } else {
                    char mapped = MapUnicodeToMacRomanOrAscii(cp);
                    if (mapped != 0) {
                        outBuf[outIdx++] = mapped;
                    }
                }
                p += 2;
            } else if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
                p += 3;
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
 * Playing Track Match & Sync Helpers
 * ============================================================================ */

static Boolean DoesRightItemMatchPlayingTrack(short idx)
{
    RightItem *it;
    if (idx < 0 || idx >= gRightCount) return false;
    it = &gRightItems[idx];

    if (strcmp(it->kind, "TRACK") != 0) return false;

    /* Match uniquely by Spotify track ID (or URI containing track ID) whenever available */
    if (gNowPlaying.trackId[0] != '\0' && (it->id[0] != '\0' || it->uri[0] != '\0')) {
        if (it->id[0] != '\0' && strcmp(it->id, gNowPlaying.trackId) == 0) {
            return true;
        }
        if (it->uri[0] != '\0' && strstr(it->uri, gNowPlaying.trackId) != NULL) {
            return true;
        }
        return false;
    }

    /* Fallback only when track ID is unavailable */
    if (gNowPlaying.trackId[0] == '\0' &&
        gNowPlaying.trackName[0] != '\0' &&
        it->name[0] != '\0' &&
        strcmp(it->name, gNowPlaying.trackName) == 0) {
        if (gNowPlaying.artistName[0] == '\0' ||
            it->artist[0] == '\0' ||
            strcmp(it->artist, gNowPlaying.artistName) == 0) {
            return true;
        }
    }
    return false;
}

static short FindPlayingTrackIndexInRightList(void)
{
    short i;
    /* If the user's selected row matches the playing track ID, prefer that exact row */
    if (gRightSelected >= 0 && gRightSelected < gRightCount &&
        DoesRightItemMatchPlayingTrack(gRightSelected)) {
        return gRightSelected;
    }
    for (i = 0; i < gRightCount; i++) {
        if (DoesRightItemMatchPlayingTrack(i)) {
            return i;
        }
    }
    return -1;
}

static Boolean IsRightItemCurrentlyPlaying(short idx)
{
    RightItem *it;
    if (idx < 0 || idx >= gRightCount) return false;
    it = &gRightItems[idx];

    /* For Album browsing lists, highlight the selected album row */
    if (strcmp(it->kind, "ALBUM") == 0) {
        return (idx == gRightSelected);
    }

    return (idx == FindPlayingTrackIndexInRightList());
}

static void MaybeAppendSmartQueueToRightList(Boolean isInitialLoad);
static void PlayRightItemAtIndex(short idx);

static void SyncRightSelectionToPlayingTrack(void)
{
    short idx = FindPlayingTrackIndexInRightList();
    if (idx >= 0) {
        gRightSelected = idx;
        if (idx < gRightScroll) {
            gRightScroll = idx;
        } else if (idx >= gRightScroll + VISIBLE_ROWS) {
            gRightScroll = idx - VISIBLE_ROWS + 1;
        }
    }
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

/* Draws a centered line of white text on black background around x = 256 */
static void DrawCenteredLineBelowArt(const char *str, short yBaseline)
{
    char truncBuf[40];
    short len;
    short pixelW;
    short startX;

    if (!str || str[0] == '\0') return;
    len = (short)strlen(str);
    if (len > 33) {
        memcpy(truncBuf, str, 30);
        truncBuf[30] = '.';
        truncBuf[31] = '.';
        truncBuf[32] = '\0';
        len = 32;
    } else {
        SafeCopy(truncBuf, str, sizeof(truncBuf));
    }
    pixelW = TextWidth(truncBuf, 0, len);
    startX = 256 - (pixelW / 2);
    if (startX < 152) startX = 152;
    TextMode(srcBic); /* White text on black background */
    MoveTo(startX, yBaseline);
    DrawText(truncBuf, 0, len);
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
 * Draws the 9x9 slider thumb dot centered at (cx, cy) matching `button-states-2.png`:
 * - 1px white gap on left & right (`cx - 4`, `cx + 4`)
 * - 7x7 black circle (`cx - 3..cx + 3`, `cy - 3..cy + 3`)
 * - On hover (`hollow == true`): 3x3 cross cutout in center (`(cx, cy-1)`, `(cx-1..cx+1, cy)`, `(cx, cy+1)`).
 */
static void DrawSliderDot(short cx, short cy, Boolean hollow)
{
    Rect r;
    PenNormal();
    /* Clear 9x9 area around dot so left/right have 1px white separation from track bar */
    SetRect(&r, cx - 4, cy - 4, cx + 5, cy + 5);
    EraseRect(&r);

    /* Paint exact 7x7 circle rows (widths 3, 5, 7, 7, 7, 5, 3) */
    SetRect(&r, cx - 1, cy - 3, cx + 2, cy + 4);
    PaintRect(&r);
    SetRect(&r, cx - 2, cy - 2, cx + 3, cy + 3);
    PaintRect(&r);
    SetRect(&r, cx - 3, cy - 1, cx + 4, cy + 2);
    PaintRect(&r);

    if (hollow) {
        /* 3x3 white cross cutout in center (Row 2 Hover state in button-states-2.png) */
        SetRect(&r, cx - 1, cy, cx + 2, cy + 1);
        EraseRect(&r);
        SetRect(&r, cx, cy - 1, cx + 1, cy + 2);
        EraseRect(&r);
    }
}

/*
 * Draws the 4-dot horizontal hover indicator (`#.#.#.#`, 7px wide) below a button
 * as shown in Row 2 of `button-states-2.png`.
 */
static void DrawButtonHoverDots(short xFirstDot, short y, Boolean isHover)
{
    short i;
    if (!isHover) return;
    PenNormal();
    PenMode(patCopy);
    for (i = 0; i < 4; i++) {
        MoveTo(xFirstDot + i * 2, y);
        LineTo(xFirstDot + i * 2, y);
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
    return 359 + w + 6;
}

/*
 * Returns the 9x9 1-bit icon bitmap from `ui/library-icons.psd` for a LeftItem.
 * Falls back to `gIconLibPlaylist` if an icon type is missing.
 */
static const unsigned char *GetLeftItemIconBits(const LeftItem *it)
{
    if (!it) return gIconLibPlaylist;
    if (strcmp(it->kind, "LIKED") == 0) {
        return gIconLibLiked;
    }
    if (strcmp(it->kind, "ALBUM") == 0) {
        return gIconLibAlbum;
    }
    if (strcmp(it->kind, "PLAYLIST") == 0) {
        return gIconLibPlaylist;
    }
    if (strcmp(it->kind, "SHOW") == 0 || strcmp(it->kind, "PODCAST") == 0) {
        return gIconLibPodcast;
    }
    if (strcmp(it->kind, "ARTIST") == 0) {
        return gIconLibArtist;
    }
    if (strcmp(it->kind, "FOR_YOU") == 0) {
        return gIconLibForYou;
    }
    if (strcmp(it->kind, "HOME_CAT") == 0) {
        if (strcmp(it->id, "recent") == 0) return gIconLibRecent;
        if (strcmp(it->id, "top") == 0)    return gIconLibTopTracks;
        if (strcmp(it->id, "albums") == 0) return gIconLibSavedAlbums;
        if (strcmp(it->id, "new") == 0)    return gIconLibNewReleases;
    }
    return gIconLibPlaylist;
}

/*
 * Renders the Search History dropdown box below the search bar (x = 156..356, y = 17..128)
 * matching `ui/search-history.png` without overlapping the search bar at y = 5..16.
 */
static void RenderSearchHistoryDropdown(void)
{
    Rect dropRect, neckRect, hdrPill, hiRect;
    short row;

    /* 1. Draw rounded dropdown body in the middle content area (y = 22..128) */
    SetRect(&dropRect, 156, 22, 356, 128);
    PenNormal();
    PaintRoundRect(&dropRect, 12, 12);
    PenMode(patBic);
    FrameRoundRect(&dropRect, 12, 12);
    PenNormal();

    /* 2. Connect straight up from y = 28 to the bottom of the search pill at y = 17
     *    (erasing the top rounded corners at y = 22..28 and extending the black neck
     *    through the white header at y = 17..21 while preserving the 1px white side walls) */
    SetRect(&neckRect, 157, 17, 355, 29);
    PaintRect(&neckRect);
    PenMode(patBic);
    MoveTo(156, 22);
    LineTo(156, 28);
    MoveTo(355, 22);
    LineTo(355, 28);
    PenNormal();

    /* 3. Outer corner transition pixels at y = 16 + 2px staggered dotted divider at y = 17..18 */
    MoveTo(158, 16);
    LineTo(158, 16);
    MoveTo(352, 16);
    LineTo(352, 16);
    MoveTo(354, 16);
    LineTo(354, 16);
    DrawHorizontalDottedLine(158, 352, 17, true);
    DrawHorizontalDottedLine(159, 352, 18, true);

    /* "Search history:" white pill at (161, 24)-(266, 36) */
    SetRect(&hdrPill, 161, 24, 266, 36);
    EraseRoundRect(&hdrPill, 8, 8);
    TextMode(srcOr);
    MoveTo(166, 33);
    DrawCStringTruncated("Search history:", 16);

    if (gSearchHistoryCount <= 0) {
        TextMode(srcBic);
        MoveTo(166, 51);
        DrawCStringTruncated("No recent searches", 26);
        TextMode(srcOr);
        return;
    }

    for (row = 0; row < MAX_SEARCH_HISTORY; row++) {
        short rowTop = 40 + row * 17;
        if (row < gSearchHistoryCount) {
            if (row == gSearchHistoryHoverRow) {
                SetRect(&hiRect, 160, rowTop, 352, rowTop + 14);
                EraseRoundRect(&hiRect, 6, 6);
                TextMode(srcOr);
            } else {
                TextMode(srcBic);
            }
            MoveTo(166, rowTop + 11);
            DrawCStringTruncated(gSearchHistory[row], 29);
        }
        if (row < 4) {
            DrawHorizontalDottedLine(162, 349, rowTop + 16, true);
        }
    }
    TextMode(srcOr);
}

/* ============================================================================
 * Main UI Rendering (Exact 512 x 322 PSD / UI-update.png Layout)
 * ============================================================================ */

static void DrawCenteredCardText(const char *str, short yBaseline)
{
    short len = (short)strlen(str);
    short w   = TextWidth(str, 0, len);
    MoveTo(256 - (w / 2), yBaseline);
    DrawText(str, 0, len);
}

static void RenderPreConnectionScreen(void)
{
    Rect fullRect, ipBox, countBox, innerBox;
    char ipField[40];
    char secStr[16];
    char visitLine[64];

    SetRect(&fullRect, 0, 0, CANVAS_W, CANVAS_H);
    FillRect(&fullRect, &qd.gray);

    if (gConnState == CONN_SEARCHING) {
        BlitIcon1Bit(gServerCard1Bits, 236, 199, 140, 63, srcCopy);
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
        SetRect(&countBox, 247, 226, 256, 238);
        EraseRect(&countBox);
        sprintf(secStr, "%d", (int)gFoundCountdownSec);
        TextMode(srcOr);
        MoveTo(248, 236);
        DrawText(secStr, 0, 1);
    } else if (gConnState == CONN_LOGIN_ERROR) {
        /* Middleware login status error screen (ui/login-error.png) */
        BlitIcon1Bit(gServerCard1Bits, 236, 199, 140, 63, srcCopy);
        SetRect(&innerBox, 150, 128, 366, 248);
        EraseRect(&innerBox);
        TextMode(srcOr);
        DrawCenteredCardText("You have not logged", 141);
        DrawCenteredCardText("in to Spotify.", 152);
        sprintf(visitLine, "Please visit %s:6800", gIpInput[0] ? gIpInput : "127.0.0.1");
        DrawCenteredCardText(visitLine, 176);
        DrawCenteredCardText("from a modern browser to login.", 188);
        BlitIcon1Bit(gLoginErrorBtnBits, 108, 27, 204, 203, srcCopy);
    }
}

/*
 * Renders the "For You" center panel (Discover Weekly, DJ, Daily Mix 01, Daily Mix 02)
 * matching `ui/foryou.png` with four 45x45 icons inside a 1px white outline (47x47 total).
 */
static void RenderForYouCenterPanel(void)
{
    static const short kRowTops[FOR_YOU_COUNT] = { 56, 110, 164, 218 };
    short row;

    /* 1. "Tune in" rounded white header pill at (222, 38) 68x12 */
    BlitIcon1Bit(gForYouHeaderPillBits, 68, 12, 222, 38, srcCopy);

    /* 2. Close (X) button at (337, 38) 12x12 — hidden on initial view if nothing is playing */
    if (gNowPlaying.trackId[0] != '\0' || gVisualizerOpen) {
        BlitIcon1Bit(gForYouCloseCircleBits, 12, 12, 337, 38, srcCopy);
    }

    /* 3. Four 47x47 (45x45 + 1px white outline) icons + Heading & 2-line description (ui/tune-in.png) */
    TextMode(srcBic); /* White text on black background */
    for (row = 0; row < FOR_YOU_COUNT; row++) {
        short yTop = kRowTops[row];
        BlitIcon1Bit(gForYouIconBits[row], 47, 47, 156, yTop, srcCopy);

        MoveTo(210, yTop + 15);
        DrawCStringTruncated(gForYouItems[row].title, 25);

        if (gForYouItems[row].line1[0] != '\0') {
            MoveTo(210, yTop + 27);
            DrawCStringTruncated(gForYouItems[row].line1, 25);
        }
        if (gForYouItems[row].line2[0] != '\0') {
            MoveTo(210, yTop + 39);
            DrawCStringTruncated(gForYouItems[row].line2, 25);
        }
    }
    TextMode(srcOr);
}

static void RenderSearchCenterPanel(void)
{
    short row;
    Rect r, badgeRect, thumbRect;

    /* Top-right circular (X) close button (485, 34) 15x15 */
    BlitIcon1Bit(gSearchCloseCircleBits, 15, 15, 485, 34, srcCopy);

    /* Search Scrollbar at x = 469, y = 35..266 (only shown when results exceed visible rows) */
    if (gSearchCount > SEARCH_VISIBLE_ROWS) {
        short trackH = 231;
        short thumbH = (short)((long)trackH * SEARCH_VISIBLE_ROWS / gSearchCount);
        short maxScroll = gSearchCount - SEARCH_VISIBLE_ROWS;
        short thumbY;
        if (thumbH < 20) thumbH = 20;
        thumbY = 35 + (short)((long)(trackH - thumbH) * gSearchScroll / maxScroll);
        DrawVerticalDottedLine(469, 35, 266, true);
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
            EraseRect(&r);

            BlitIcon1Bit(gSearchPlayIconBits, 12, 12, 355, yTop + 8, srcCopy);

            SetRect(&badgeRect, 371, yTop + 8, 371 + badgeW, yTop + 20);
            PaintRoundRect(&badgeRect, 6, 6);
            TextMode(srcBic);
            MoveTo(377, yTop + 17);
            DrawCStringTruncated(badgeLabel, 10);

            TextMode(srcOr);
            MoveTo(78, yTop + 17);
            DrawCStringTruncated(it->title, 42);
            MoveTo(78, yTop + 29);
            DrawCStringTruncated(it->subtitle, 42);
        } else {
            TextMode(srcBic);
            MoveTo(78, yTop + 17);
            DrawCStringTruncated(it->title, 42);
            MoveTo(78, yTop + 29);
            DrawCStringTruncated(it->subtitle, 42);

            SetRect(&badgeRect, 371, yTop + 8, 371 + badgeW, yTop + 20);
            PenMode(patBic);
            FrameRoundRect(&badgeRect, 6, 6);
            PenNormal();
            MoveTo(377, yTop + 17);
            DrawCStringTruncated(badgeLabel, 10);
        }

        DrawHorizontalDottedLine(61, 452, yTop + SEARCH_ROW_H - 1, true);
    }
    TextMode(srcOr);
}

/*
 * Custom Window Definition Procedure (WDEF) for the 192x68 Mini-Player window.
 * Masks both `contRgn` and `strucRgn` to the exact rounded-corner silhouette of
 * Layer 1 in `Mini-player.psd` so there is no black background behind the rounded
 * corners — only the white rounded window masked directly against the background.
 */
static pascal long MiniPlayerWDEF(short varCode, WindowPtr window, short message, long param)
{
    WindowPeek wp = (WindowPeek)window;
    (void)varCode;

    switch (message) {
        case wDraw:
            return 0;

        case wHit: {
            Point hitPt;
            hitPt.v = HiWord(param);
            hitPt.h = LoWord(param);
            if (PtInRgn(hitPt, wp->contRgn)) {
                return wInContent;
            }
            return wNoHit;
        }

        case wCalcRgns: {
            GrafPtr   savePort;
            Point     topLeft;
            short     gx, gy;
            RgnHandle tmpRgn = NewRgn();

            GetPort(&savePort);
            SetPort(window);
            topLeft.h = window->portRect.left;
            topLeft.v = window->portRect.top;
            LocalToGlobal(&topLeft);
            SetPort(savePort);

            gx = topLeft.h;
            gy = topLeft.v;

            /* Exact 192x68 rounded-corner alpha mask from Layer 1 of Mini-player.psd */
            SetRectRgn(wp->contRgn, gx + 4, gy + 0, gx + MINI_W - 4, gy + MINI_H);
            if (tmpRgn) {
                SetRectRgn(tmpRgn, gx + 2, gy + 1, gx + MINI_W - 2, gy + MINI_H - 1);
                UnionRgn(wp->contRgn, tmpRgn, wp->contRgn);
                SetRectRgn(tmpRgn, gx + 1, gy + 2, gx + MINI_W - 1, gy + MINI_H - 2);
                UnionRgn(wp->contRgn, tmpRgn, wp->contRgn);
                SetRectRgn(tmpRgn, gx + 0, gy + 4, gx + MINI_W,     gy + MINI_H - 4);
                UnionRgn(wp->contRgn, tmpRgn, wp->contRgn);
                DisposeRgn(tmpRgn);
            }
            CopyRgn(wp->contRgn, wp->strucRgn);
            return 0;
        }

        case wNew:
        case wDispose:
        case wGrow:
        case wDrawGIcon:
            return 0;
    }
    return 0;
}

static void InstallMiniPlayerWDEF(WindowPtr win)
{
    Handle stub;
    if (!win) return;
    stub = NewHandle(6);
    if (stub && *stub) {
        unsigned char *p = (unsigned char *)*stub;
        long procAddr = (long)MiniPlayerWDEF;
        HLock(stub);
        p[0] = 0x4E; /* 68000 JMP.L absolute */
        p[1] = 0xF9;
        *(long *)(p + 2) = procAddr;
        ((WindowPeek)win)->windowDefProc = stub;
        MiniPlayerWDEF(0, win, wCalcRgns, 0);
    }
}

/*
 * Non-blocking window drag loop that continuously pumps `AudioStreamPump()`
 * while the mouse button is held down so music playback never stops while
 * moving the Mini-Player or Main Window around the screen.
 */
static void DragWindowWithAudio(WindowPtr win, Point startGlobalPt)
{
    GrafPtr   savePort, wmgrPort;
    RgnHandle savedClip;
    Point     winOrigin, curGlobalPt;
    short     winW, winH, origLeft, origTop, curLeft, curTop;
    Rect      outlineRect;
    Boolean   isMini = (win == gMiniWindow);

    if (!win) return;
    GetPort(&savePort);
    SetPort(win);
    winOrigin.h = win->portRect.left;
    winOrigin.v = win->portRect.top;
    LocalToGlobal(&winOrigin);
    winW = win->portRect.right - win->portRect.left;
    winH = win->portRect.bottom - win->portRect.top;

    origLeft = winOrigin.h;
    origTop  = winOrigin.v;
    curLeft  = origLeft;
    curTop   = origTop;

    GetWMgrPort(&wmgrPort);
    SetPort(wmgrPort);
    savedClip = NewRgn();
    if (savedClip) {
        GetClip(savedClip);
    }
    ClipRect(&qd.screenBits.bounds);
    PenNormal();
    PenPat(&qd.gray);
    PenMode(patXor);

    SetRect(&outlineRect, curLeft, curTop, curLeft + winW, curTop + winH);
    if (isMini) {
        FrameRoundRect(&outlineRect, 8, 8);
    } else {
        FrameRect(&outlineRect);
    }

    while (StillDown()) {
        short newLeft, newTop;
        if (AudioStreamIsRunning()) {
            AudioStreamPump();
        }
        GetMouse(&curGlobalPt);
        newLeft = origLeft + (curGlobalPt.h - startGlobalPt.h);
        newTop  = origTop  + (curGlobalPt.v - startGlobalPt.v);

        if (newTop < 20) newTop = 20;
        if (newTop > qd.screenBits.bounds.bottom - 16) {
            newTop = qd.screenBits.bounds.bottom - 16;
        }
        if (newLeft < qd.screenBits.bounds.left - winW + 24) {
            newLeft = qd.screenBits.bounds.left - winW + 24;
        }
        if (newLeft > qd.screenBits.bounds.right - 24) {
            newLeft = qd.screenBits.bounds.right - 24;
        }

        if (newLeft != curLeft || newTop != curTop) {
            /* Erase old XOR outline */
            SetRect(&outlineRect, curLeft, curTop, curLeft + winW, curTop + winH);
            if (isMini) {
                FrameRoundRect(&outlineRect, 8, 8);
            } else {
                FrameRect(&outlineRect);
            }

            curLeft = newLeft;
            curTop  = newTop;

            /* Draw new XOR outline */
            SetRect(&outlineRect, curLeft, curTop, curLeft + winW, curTop + winH);
            if (isMini) {
                FrameRoundRect(&outlineRect, 8, 8);
            } else {
                FrameRect(&outlineRect);
            }
        }
    }

    /* Erase final XOR outline before moving the window */
    SetRect(&outlineRect, curLeft, curTop, curLeft + winW, curTop + winH);
    if (isMini) {
        FrameRoundRect(&outlineRect, 8, 8);
    } else {
        FrameRect(&outlineRect);
    }

    PenNormal();
    if (savedClip) {
        SetClip(savedClip);
        DisposeRgn(savedClip);
    }
    SetPort(savePort);

    if (curLeft != origLeft || curTop != origTop) {
        MoveWindow(win, curLeft, curTop, false);
        if (AudioStreamIsRunning()) {
            AudioStreamPump();
        }
    }
}

static void DrawVizFullscreenHoverIcon(short x, short y, Boolean isShrinkIcon)
{
    BlitIcon1Bit(gIconVizFullscreenMask, 12, 12, x, y, srcBic);
    BlitIcon1Bit(isShrinkIcon ? gIconVizFullscreenDown : gIconVizFullscreen, 12, 12, x, y, srcOr);
}

static void DrawMiniPlayerContentsAt(short ox, short oy, Boolean drawOverlayFrame)
{
    Rect r, barRect;
    char titleLine[160];
    char truncTitle[36];
    char timeLine[32];
    short len, w, startX;

    TextFont(kFontIDMonaco);
    TextSize(9);
    TextFace(normal);
    TextMode(srcOr);
    PenNormal();

    /* 1. Interior & Frame (192x68) */
    if (drawOverlayFrame) {
        /* 1px white outer halo + 2px black rounded border + white interior on top of visualizer */
        SetRect(&r, ox - 1, oy - 1, ox + MINI_W + 1, oy + MINI_H + 1);
        EraseRoundRect(&r, 12, 12);
        SetRect(&r, ox, oy, ox + MINI_W, oy + MINI_H);
        FrameRoundRect(&r, 10, 10);
        InsetRect(&r, 1, 1);
        FrameRoundRect(&r, 8, 8);
    } else {
        SetRect(&r, ox, oy, ox + MINI_W, oy + MINI_H);
        EraseRect(&r);
    }

    /* 2. Centered "Artist - Song title" at top (baseline y = oy + 15) */
    if (gNowPlaying.trackName[0] != '\0') {
        if (gNowPlaying.artistName[0] != '\0') {
            sprintf(titleLine, "%s - %s", gNowPlaying.artistName, gNowPlaying.trackName);
        } else {
            SafeCopy(titleLine, gNowPlaying.trackName, sizeof(titleLine));
        }
    } else {
        SafeCopy(titleLine, "SpotiMac - 1984", sizeof(titleLine));
    }
    len = (short)strlen(titleLine);
    if (len > 29) {
        memcpy(truncTitle, titleLine, 26);
        truncTitle[26] = '.';
        truncTitle[27] = '.';
        truncTitle[28] = '\0';
        len = 28;
    } else {
        SafeCopy(truncTitle, titleLine, sizeof(truncTitle));
    }
    w = TextWidth(truncTitle, 0, len);
    startX = (MINI_W / 2) - (w / 2);
    if (startX < 6) startX = 6;
    MoveTo(ox + startX, oy + 15);
    DrawText(truncTitle, 0, len);

    /* 3. Transport Row (Mini-player.psd exact coordinates offset by ox, oy) */
    /* Like / Save Heart (11, 31) 11x10 + hover dots at (13, 44) */
    {
        Boolean hov = (gHoverBtn == BTN_LIKE) && (gPressedBtn != BTN_LIKE);
        BlitIcon1Bit(gNowPlaying.isLiked ? gIconHeartFilled : gIconHeartOutline, 11, 10, ox + 11, oy + 31, srcCopy);
        DrawButtonHoverDots(ox + 13, oy + 44, hov);
    }
    DrawVerticalDottedLine(ox + 33, oy + 28, oy + 42, false);

    /* Shuffle (43, 30) 14x11 + hover dots at (46, 44) */
    {
        Boolean hov = (gHoverBtn == BTN_SHUFFLE) && (gPressedBtn != BTN_SHUFFLE);
        if (strcmp(gNowPlaying.shuffleMode, "smart") == 0) {
            BlitIcon1Bit(gIconShuffleSmart, 14, 11, ox + 43, oy + 30, srcCopy);
        } else if (strcmp(gNowPlaying.shuffleMode, "on") == 0) {
            BlitIcon1Bit(gIconShuffleOn, 14, 11, ox + 43, oy + 30, srcCopy);
        } else {
            BlitIcon1Bit(gIconShuffleOff, 14, 11, ox + 43, oy + 30, srcCopy);
        }
        DrawButtonHoverDots(ox + 46, oy + 44, hov);
    }

    /* Previous (68, 31) 6x9 + hover dots at (67, 44) */
    {
        Boolean hov = (gHoverBtn == BTN_PREV) && (gPressedBtn != BTN_PREV);
        Boolean prs = (gPressedBtn == BTN_PREV);
        BlitIcon1Bit(prs ? gIconPrevClick : gIconPrev, 6, 9, ox + 68, oy + 31, srcCopy);
        DrawButtonHoverDots(ox + 67, oy + 44, hov);
    }

    /* Play / Pause Circle (85, 24) 22x22 */
    {
        Boolean hov = (gHoverBtn == BTN_PLAY) && !gSuppressPlayHover;
        Boolean prs = (gPressedBtn == BTN_PLAY);
        const unsigned char *iconBits;
        if (prs) {
            iconBits = gNowPlaying.isPlaying ? gIconPauseCircle22 : gIconPlayCircle22;
        } else if (hov) {
            iconBits = gNowPlaying.isPlaying ? gIconPauseCircleInv22 : gIconPlayCircleInv22;
        } else {
            iconBits = gNowPlaying.isPlaying ? gIconPauseCircle22 : gIconPlayCircle22;
        }
        BlitIcon1Bit(iconBits, 22, 22, ox + 85, oy + 24, srcCopy);
    }

    /* Next (118, 31) 6x9 + hover dots at (118, 44) */
    {
        Boolean hov = (gHoverBtn == BTN_NEXT) && (gPressedBtn != BTN_NEXT);
        Boolean prs = (gPressedBtn == BTN_NEXT);
        BlitIcon1Bit(prs ? gIconNextClick : gIconNext, 6, 9, ox + 118, oy + 31, srcCopy);
        DrawButtonHoverDots(ox + 118, oy + 44, hov);
    }

    /* Repeat (135, 30) 13x12 + hover dots at (138, 44) */
    {
        Boolean hov = (gHoverBtn == BTN_REPEAT) && (gPressedBtn != BTN_REPEAT);
        if (strcmp(gNowPlaying.repeatState, "track") == 0) {
            BlitIcon1Bit(gIconRepeatTrack, 13, 12, ox + 135, oy + 30, srcCopy);
        } else if (strcmp(gNowPlaying.repeatState, "context") == 0) {
            BlitIcon1Bit(gIconRepeatAll, 13, 12, ox + 135, oy + 30, srcCopy);
        } else {
            BlitIcon1Bit(gIconRepeatOff, 13, 12, ox + 135, oy + 30, srcCopy);
        }
        DrawButtonHoverDots(ox + 138, oy + 44, hov);
    }

    DrawVerticalDottedLine(ox + 158, oy + 28, oy + 42, false);

    /* Enlarge / Expand Button (169, 30) 11x11 + hover dots at (172, 44) */
    {
        Boolean hov = (gHoverBtn == BTN_COLLAPSE) && (gPressedBtn != BTN_COLLAPSE);
        Boolean prs = (gPressedBtn == BTN_COLLAPSE);
        BlitIcon1Bit(prs ? gIconEnlargeClick : gIconEnlargeNormal, 11, 11, ox + 169, oy + 30, srcCopy);
        DrawButtonHoverDots(ox + 172, oy + 44, hov);
    }

    /* 4. Bottom Row: Timecode (12, 59) + Volume Icon (112, 52) + Volume Bar (126, 54)-(181, 57) */
    sprintf(
        timeLine,
        "%s/%s",
        gNowPlaying.progressMmSs[0] ? gNowPlaying.progressMmSs : "00:00",
        gNowPlaying.durationMmSs[0] ? gNowPlaying.durationMmSs : "00:00"
    );
    MoveTo(ox + 12, oy + 59);
    DrawCStringTruncated(timeLine, 11);

    BlitIcon1Bit(gIconVolume, 10, 7, ox + 112, oy + 52, srcCopy);
    SetRect(&barRect, ox + 126, oy + 54, ox + 181, oy + 57);
    FrameRect(&barRect);
    {
        short vol = gNowPlaying.volumePct;
        short fillW;
        short dotX;
        Boolean hollowDot = (gHoverBtn == BTN_VOLUME) && (gPressedBtn != BTN_VOLUME);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        fillW = (short)((vol * 55L) / 100L);
        if (fillW > 0) {
            SetRect(&r, ox + 126, oy + 54, ox + 126 + fillW, oy + 57);
            PaintRect(&r);
        }
        dotX = ox + 126 + fillW;
        if (dotX < ox + 130) dotX = ox + 130;
        if (dotX > ox + 177) dotX = ox + 177;
        DrawSliderDot(dotX, oy + 55, hollowDot);
    }
}

static void RenderMiniPlayer(void)
{
    GrafPtr savePort;

    if (!gMiniWindow) return;
    GetPort(&savePort);

    if (gOffscreenReady) {
        SetPort(&gOffscreenPort);
    } else {
        SetPort(gMiniWindow);
    }

    DrawMiniPlayerContentsAt(0, 0, false);

    if (gOffscreenReady) {
        Rect miniRect;
        SetRect(&miniRect, 0, 0, MINI_W, MINI_H);
        SetPort(gMiniWindow);
        CopyBits(&gOffscreenPort.portBits, &gMiniWindow->portBits, &miniRect, &miniRect, srcCopy, NULL);
    }

    SetPort(savePort);
}

static void EnterMiniPlayerMode(void)
{
    if (!gMiniWindow || gMiniPlayerMode) return;
    gSpeakerPopupOpen  = false;
    gAboutModalOpen    = false;
    gHoverBtn          = BTN_NONE;
    gPressedBtn        = BTN_NONE;
    gSuppressPlayHover = false;

    if (!gMiniWinPositioned && gMainWindow) {
        GrafPtr savePort;
        Point topLeft;
        GetPort(&savePort);
        SetPort(gMainWindow);
        topLeft.h = 160;
        topLeft.v = 119;
        LocalToGlobal(&topLeft);
        SetPort(savePort);
        MoveWindow(gMiniWindow, topLeft.h, topLeft.v, false);
        gMiniWinPositioned = true;
    }

    gMiniPlayerMode = true;
    if (gMainWindow) HideWindow(gMainWindow);
    ShowWindow(gMiniWindow);
    SelectWindow(gMiniWindow);
    RenderMiniPlayer();
}

static void ExitMiniPlayerMode(void)
{
    if (!gMainWindow || !gMiniPlayerMode) return;
    gHoverBtn          = BTN_NONE;
    gPressedBtn        = BTN_NONE;
    gSuppressPlayHover = false;
    gMiniPlayerMode    = false;

    if (gMiniWindow) HideWindow(gMiniWindow);
    ShowWindow(gMainWindow);
    SelectWindow(gMainWindow);
    RenderCanvas();
}

static void RenderCanvas(void)
{
    GrafPtr savePort;
    Rect r, sqRect, barRect, thumbRect;
    short i;

    if (gMiniPlayerMode) {
        RenderMiniPlayer();
        return;
    }

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
     * 0B. Fullscreen Visualizer Mode (512x322 + Locked Mini-Player Overlay)
     * -------------------------------------------------------------------- */
    if (gVisualizerOpen && gVizFullscreen && !gSearchViewOpen && !gAboutModalOpen) {
        if (gOffscreenReady && gOffscreenBits) {
            VisualizerBlitFullscreenCached((unsigned char *)gOffscreenBits);
        } else {
            SetRect(&r, 0, 0, CANVAS_W, CANVAS_H);
            PaintRect(&r);
        }

        /* Locked Mini-Player overlay centered at bottom (15px from bottom: x=160, y=239) */
        DrawMiniPlayerContentsAt(MINI_FS_X, MINI_FS_Y, true);

        /* Shrink-down icon in top-right corner (8px from top and 8px from right: 492, 8) */
        DrawVizFullscreenHoverIcon(CANVAS_W - 8 - 12, 8, true);

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

    /* Search Pill (156, 5)-(356, 17) */
    if (gSearchViewOpen || gSearchQuery[0] != '\0') {
        BlitIcon1Bit(gSearchPillClearBits, 200, 12, 156, 5, srcCopy);
    } else {
        BlitIcon1Bit(gSearchPillBits, 200, 12, 156, 5, srcCopy);
    }
    if (gSearchFocused || gSearchQuery[0] != '\0') {
        char searchDisplay[56];
        if (gSearchFocused && gSearchQuery[0] != '\0') {
            sprintf(searchDisplay, "%s_", gSearchQuery);
        } else {
            SafeCopy(searchDisplay, gSearchQuery, sizeof(searchDisplay));
        }
        if (searchDisplay[0] != '\0') {
            TextMode(srcBic);
            MoveTo(171, 14);
            DrawCStringTruncated(searchDisplay, 26);
            TextMode(srcOr);
        }
    }

    /* Right column header ("Tracks" or "Albums") */
    MoveTo(372, 14);
    DrawCStringTruncated(gRightHeaderLabel, 18);

    /* --------------------------------------------------------------------
     * 2. Middle Main Content Area (y = 22..273, Black background)
     * -------------------------------------------------------------------- */
    SetRect(&r, 0, 22, CANVAS_W, 273);
    PaintRect(&r);

    if (!gInitialDataLoaded) {
        /* Before library & playback status finish loading: show `ui/loading.png` */
        BlitIcon1Bit(gLoadingCardBits, 66, 66, 224, 93, srcCopy);
        BlitIcon1Bit(gLoadingTaglineBits, 238, 23, 138, 169, srcCopy);
    } else if (gSearchViewOpen) {
        RenderSearchCenterPanel();
    } else {
        /* 2A. Left Column: Library / Home Items with 9x9 Icons (ui/library.png & library-icons.psd) */
        for (i = 0; i < VISIBLE_ROWS; i++) {
            short itemIdx = gLeftScroll + i;
            short rowTop  = LIST_TOP_Y + i * ROW_HEIGHT;
            const unsigned char *iconBits;
            if (itemIdx >= gLeftCount) break;
            iconBits = GetLeftItemIconBits(&gLeftItems[itemIdx]);

            if (itemIdx == gLeftSelected) {
                /* White highlight pill with rounded right edge + black icon + right arrow */
                SetRect(&r, 7, rowTop, 137, rowTop + 12);
                EraseRoundRect(&r, 8, 8);
                SetRect(&sqRect, 7, rowTop, 15, rowTop + 12);
                EraseRect(&sqRect);
                BlitIcon1Bit(iconBits, 9, 9, 9, rowTop + 2, srcOr);
                BlitIcon1Bit(gIconListPlayArrow, 3, 6, 131, rowTop + 3, srcCopy);
                TextMode(srcOr); /* Black text */
            } else {
                BlitIcon1Bit(iconBits, 9, 9, 9, rowTop + 2, srcBic);
                TextMode(srcBic); /* White text on black background */
            }
            MoveTo(24, rowTop + 9);
            DrawCStringTruncated(gLeftItems[itemIdx].name, 17);
        }

        /* Left Scrollbar (x = 144, y = 35..266) — hidden when list fits without scrolling */
        if (gLeftCount > VISIBLE_ROWS) {
            short trackH = 231;
            short thumbH = (short)((long)trackH * VISIBLE_ROWS / gLeftCount);
            short maxScroll = gLeftCount - VISIBLE_ROWS;
            short thumbY;
            if (thumbH < 20) thumbH = 20;
            thumbY = 35 + (short)((long)(trackH - thumbH) * gLeftScroll / maxScroll);
            DrawVerticalDottedLine(144, 35, 266, true);
            SetRect(&thumbRect, 143, thumbY, 146, thumbY + thumbH);
            FillRect(&thumbRect, &qd.gray);
        }

        /* 2B. Center Column: "For You" Page, Visualizer, or 200x200 1-Bit Album Artwork */
        if (gForYouViewOpen || (!gVisualizerOpen && gNowPlaying.trackId[0] == '\0')) {
            RenderForYouCenterPanel();
        } else {
            if (gVisualizerOpen) {
                BlitIcon1Bit(gVisualizerBits, 200, 200, 156, 35, srcCopy);
                if (gVizHover && !gSearchHistoryOpen) {
                    /* Fullscreen icon on hover: 8px down from top (35+8=43) and 8px in from right (356-8-12=336) */
                    DrawVizFullscreenHoverIcon(356 - 8 - 12, 35 + 8, false);
                }
            } else {
                BlitIcon1Bit(gCurrentArtworkBits, 200, 200, 156, 35, srcCopy);
            }

            /* Two-line Artist (y = 249) and Track Title (y = 261) centered below artwork */
            if (gNowPlaying.trackName[0] != '\0') {
                DrawCenteredLineBelowArt(
                    gNowPlaying.artistName[0] != '\0' ? gNowPlaying.artistName : "SpotiMac",
                    249
                );
                DrawCenteredLineBelowArt(gNowPlaying.trackName, 261);
            } else {
                DrawCenteredLineBelowArt("SpotiMac", 249);
                DrawCenteredLineBelowArt("Party like it's 1984", 261);
            }
        }

        /* 2C. Right Column: Tracks List (x = 362..499, y = 35..243) */
        for (i = 0; i < VISIBLE_ROWS; i++) {
            short itemIdx = gRightScroll + i;
            short rowTop  = LIST_TOP_Y + i * ROW_HEIGHT;
            if (itemIdx >= gRightCount) break;

            if (strcmp(gRightItems[itemIdx].kind, "QUEUE_SEP") == 0) {
                /* Smart queue separator line + 8x7 sparkle icon (ui/add-queue-separator.png) */
                DrawHorizontalDottedLine(363, 419, rowTop + 6, true);
                BlitIcon1Bit(gIconQueueSeparator, 8, 7, 424, rowTop + 2, srcBic);
                DrawHorizontalDottedLine(435, 497, rowTop + 6, true);
                continue;
            }

            if (IsRightItemCurrentlyPlaying(itemIdx)) {
                /* Rounded highlight pill + left play arrow icon on currently playing track (UI-update.png) */
                SetRect(&r, 362, rowTop, 499, rowTop + 12);
                EraseRoundRect(&r, 10, 10);
                BlitIcon1Bit(gIconListPlayArrow, 3, 6, 367, rowTop + 3, srcCopy);
                TextMode(srcOr); /* Black text */
            } else {
                TextMode(srcBic); /* White text on black background */
            }
            MoveTo(373, rowTop + 9);
            DrawCStringTruncated(gRightItems[itemIdx].name, 19);
        }

        /* Right Scrollbar (x = 504, y = 35..266) — hidden when list fits without scrolling */
        if (gRightCount > VISIBLE_ROWS) {
            short trackH = 231;
            short thumbH = (short)((long)trackH * VISIBLE_ROWS / gRightCount);
            short maxScroll = gRightCount - VISIBLE_ROWS;
            short thumbY;
            if (thumbH < 20) thumbH = 20;
            thumbY = 35 + (short)((long)(trackH - thumbH) * gRightScroll / maxScroll);
            DrawVerticalDottedLine(504, 35, 266, true);
            SetRect(&thumbRect, 503, thumbY, 506, thumbY + thumbH);
            FillRect(&thumbRect, &qd.gray);
        }

        /* 2D. Search History Dropdown Overlay (ui/search-history.png) */
        if (gSearchHistoryOpen) {
            RenderSearchHistoryDropdown();
        }
    }

    /* --------------------------------------------------------------------
     * 3. Bottom Play Bar (y = 273..322, White background, button-states-2.png)
     * -------------------------------------------------------------------- */
    SetRect(&r, 0, 273, CANVAS_W, CANVAS_H);
    EraseRect(&r);
    TextMode(srcOr);
    PenNormal();

    /* Album Art / Visualizations Toggle (19, 285) 49x10 (visuals-toggle-select-updated.png) */
    {
        Boolean hov = (gHoverBtn == BTN_VIZ_TOGGLE) && (gPressedBtn != BTN_VIZ_TOGGLE);
        BlitIcon1Bit(gVisualizerOpen ? gIconVizToggleOn : gIconVizToggleOff, 49, 10, 19, 285, srcCopy);
        DrawButtonHoverDots(40, 298, hov);
    }

    /* Visualizer 1..7 Selector Row (19, 305) 49x6 + 3x2 Active Indicator */
    if (gVisualizerOpen) {
        static const short kVizIndX[VIZ_MODE_COUNT] = { 21, 28, 35, 42, 49, 56, 63 };
        short modeIdx = gVizMode % VIZ_MODE_COUNT;
        short indX = kVizIndX[modeIdx];
        SetRect(&r, indX, 301, indX + 3, 303);
        PaintRect(&r);
        BlitIcon1Bit(gIconVizNumbers, 49, 6, 19, 305, srcCopy);
    }

    /* Like / Save Heart button (155, 285) 11x10 + hover dots at (157, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_LIKE) && (gPressedBtn != BTN_LIKE);
        if (gNowPlaying.isLiked) {
            BlitIcon1Bit(gIconHeartFilled, 11, 10, 155, 285, srcCopy);
        } else {
            BlitIcon1Bit(gIconHeartOutline, 11, 10, 155, 285, srcCopy);
        }
        DrawButtonHoverDots(157, 298, hov);
    }
    DrawVerticalDottedLine(181, 280, 298, false);

    /* Shuffle button (195, 284) 14x11 + hover dots at (198, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_SHUFFLE) && (gPressedBtn != BTN_SHUFFLE);
        if (strcmp(gNowPlaying.shuffleMode, "smart") == 0) {
            BlitIcon1Bit(gIconShuffleSmart, 14, 11, 195, 284, srcCopy);
        } else if (strcmp(gNowPlaying.shuffleMode, "on") == 0) {
            BlitIcon1Bit(gIconShuffleOn, 14, 11, 195, 284, srcCopy);
        } else {
            BlitIcon1Bit(gIconShuffleOff, 14, 11, 195, 284, srcCopy);
        }
        DrawButtonHoverDots(198, 298, hov);
    }

    /* Previous button (224, 285) 6x9 + hover dots at (223, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_PREV) && (gPressedBtn != BTN_PREV);
        Boolean prs = (gPressedBtn == BTN_PREV);
        BlitIcon1Bit(prs ? gIconPrevClick : gIconPrev, 6, 9, 224, 285, srcCopy);
        DrawButtonHoverDots(223, 298, hov);
    }

    /* Play / Pause circle (245, 278) 22x22 */
    {
        Boolean hov = (gHoverBtn == BTN_PLAY) && !gSuppressPlayHover;
        Boolean prs = (gPressedBtn == BTN_PLAY);
        const unsigned char *iconBits;
        if (prs) {
            /* Mouse down: gNowPlaying.isPlaying was toggled to next state; show next state in filled black circle */
            iconBits = gNowPlaying.isPlaying ? gIconPauseCircle22 : gIconPlayCircle22;
        } else if (hov) {
            /* Hover: show current state in inverted circle with black icon */
            iconBits = gNowPlaying.isPlaying ? gIconPauseCircleInv22 : gIconPlayCircleInv22;
        } else {
            /* Normal: show current state in filled black circle */
            iconBits = gNowPlaying.isPlaying ? gIconPauseCircle22 : gIconPlayCircle22;
        }
        BlitIcon1Bit(iconBits, 22, 22, 245, 278, srcCopy);
    }

    /* Next button (282, 285) 6x9 + hover dots at (282, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_NEXT) && (gPressedBtn != BTN_NEXT);
        Boolean prs = (gPressedBtn == BTN_NEXT);
        BlitIcon1Bit(prs ? gIconNextClick : gIconNext, 6, 9, 282, 285, srcCopy);
        DrawButtonHoverDots(282, 298, hov);
    }

    /* Repeat button (303, 284) 13x12 + hover dots at (306, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_REPEAT) && (gPressedBtn != BTN_REPEAT);
        if (strcmp(gNowPlaying.repeatState, "track") == 0) {
            BlitIcon1Bit(gIconRepeatTrack, 13, 12, 303, 284, srcCopy);
        } else if (strcmp(gNowPlaying.repeatState, "context") == 0) {
            BlitIcon1Bit(gIconRepeatAll, 13, 12, 303, 284, srcCopy);
        } else {
            BlitIcon1Bit(gIconRepeatOff, 13, 12, 303, 284, srcCopy);
        }
        DrawButtonHoverDots(306, 298, hov);
    }

    /* Vertical dotted divider (330, 280..298) */
    DrawVerticalDottedLine(330, 280, 298, false);

    /* Speaker Icon (346, 284) 8x11 + Active Speaker Name (359, 293) + hover dots at (347, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_SPEAKER) && (gPressedBtn != BTN_SPEAKER) && !gSpeakerPopupOpen;
        BlitIcon1Bit(gIconSpeaker, 8, 11, 346, 284, srcCopy);
        DrawButtonHoverDots(347, 298, hov);
        MoveTo(359, 293);
        if (gNowPlaying.deviceName[0] != '\0') {
            DrawCStringTruncated(gNowPlaying.deviceName, 18);
        } else {
            DrawCStringTruncated("Choose Speaker", 18);
        }
    }

    /* Collapse to Mini-Player Button (482, 284) 11x11 + hover dots at (485, 298) */
    {
        Boolean hov = (gHoverBtn == BTN_COLLAPSE) && (gPressedBtn != BTN_COLLAPSE);
        Boolean prs = (gPressedBtn == BTN_COLLAPSE);
        BlitIcon1Bit(prs ? gIconCollapseClick : gIconCollapseNormal, 11, 11, 482, 284, srcCopy);
        DrawButtonHoverDots(485, 298, hov);
    }

    /* Timeline Bar: elapsed (109, 312), bar (143, 308)-(369, 311), total (374, 312) */
    MoveTo(109, 312);
    DrawCStringTruncated(gNowPlaying.progressMmSs[0] ? gNowPlaying.progressMmSs : "00:00", 5);

    SetRect(&barRect, 143, 308, 369, 311);
    FrameRect(&barRect);
    {
        short fillW = 0;
        short dotX  = 143;
        Boolean hollowDot = (gHoverBtn == BTN_TIMELINE) && (gPressedBtn != BTN_TIMELINE);
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
        Boolean hollowDot = (gHoverBtn == BTN_VOLUME) && (gPressedBtn != BTN_VOLUME);
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

        SetRect(&shadowRect, 333, popTop + 3, 468, 275);
        FillRoundRect(&shadowRect, 12, 12, &qd.gray);

        SetRect(&popRect, 330, popTop, 466, 272);
        EraseRoundRect(&popRect, 12, 12);
        FrameRoundRect(&popRect, 12, 12);

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
     * 5. About SpotiMac Modal Dialog (with 58x58 ~2x2cm Buy Me a Coffee QR)
     * -------------------------------------------------------------------- */
    if (gAboutModalOpen) {
        Rect dlgRect, shadowRect, btnOk;
        const char *verLine    = "SpotiMac " APP_VERSION_SHORT;
        const char *codeLine   = "\"" APP_VERSION_CODENAME "\"";
        const char *coffeeLine = "Like SpotiMac? Buy me a coffee!";
        short w;

        SetRect(&shadowRect, 140, 38, 380, 254);
        FillRoundRect(&shadowRect, 10, 10, &qd.gray);

        SetRect(&dlgRect, 136, 34, 376, 250);
        EraseRoundRect(&dlgRect, 10, 10);
        FrameRoundRect(&dlgRect, 10, 10);
        InsetRect(&dlgRect, 2, 2);
        FrameRoundRect(&dlgRect, 8, 8);

        /* SpotiMac Logo (49x44) centered at top */
        BlitIcon1Bit(gSpotiMacLogoBits, 49, 44, 232, 44, srcCopy);

        TextMode(srcOr);
        w = TextWidth(verLine, 0, (short)strlen(verLine));
        MoveTo(256 - (w / 2), 102);
        DrawText(verLine, 0, (short)strlen(verLine));

        w = TextWidth(codeLine, 0, (short)strlen(codeLine));
        MoveTo(256 - (w / 2), 116);
        DrawText(codeLine, 0, (short)strlen(codeLine));

        w = TextWidth(coffeeLine, 0, (short)strlen(coffeeLine));
        MoveTo(256 - (w / 2), 134);
        DrawText(coffeeLine, 0, (short)strlen(coffeeLine));

        /* 58x58 QR code (~2x2cm on 9" Compact Mac CRT) centered at (227, 144) */
        BlitIcon1Bit(gAboutQrCodeBits, 58, 58, 227, 144, srcCopy);

        SetRect(&btnOk, 216, 212, 296, 236);
        PaintRoundRect(&btnOk, 6, 6);
        TextMode(srcBic);
        MoveTo(248, 227);
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

/*
 * Fast 30 FPS Visualizer Blit:
 * Renders the next 200x200 visualizer frame into `gVisualizerBits` and blits ONLY
 * the 200x200 center square `(156, 35)-(356, 235)` directly to `gMainWindow`
 * (and `gOffscreenPort`), avoiding full-screen 512x322 redraw overhead.
 */
static void RenderVisualizerFastFrame(void)
{
    GrafPtr savePort;
    if (!gVisualizerOpen || !gInitialDataLoaded || gForYouViewOpen || gMiniPlayerMode || gSearchViewOpen || gConnState != CONN_READY) return;
    if (!gMainWindow) return;

    if (gSpeakerPopupOpen || gAboutModalOpen) {
        RenderCanvas();
        return;
    }

    if (gSearchHistoryOpen) {
        VisualizerRenderFrame(
            gVizMode,
            gNowPlaying.isPlaying,
            gNowPlaying.progressMs,
            gNowPlaying.trackId,
            gVisualizerBits
        );
        RenderCanvas();
        return;
    }

    if (gVizFullscreen) {
        VisualizerRenderFrameFullscreen(
            gVizMode,
            gNowPlaying.isPlaying,
            gNowPlaying.progressMs,
            gNowPlaying.trackId,
            NULL
        );
        RenderCanvas();
        return;
    }

    VisualizerRenderFrame(
        gVizMode,
        gNowPlaying.isPlaying,
        gNowPlaying.progressMs,
        gNowPlaying.trackId,
        gVisualizerBits
    );

    GetPort(&savePort);
    if (gOffscreenReady) {
        Rect vizRect;
        SetPort(&gOffscreenPort);
        BlitIcon1Bit(gVisualizerBits, 200, 200, 156, 35, srcCopy);
        if (gVizHover) {
            DrawVizFullscreenHoverIcon(356 - 8 - 12, 35 + 8, false);
        }
        SetRect(&vizRect, 156, 35, 356, 235);
        SetPort(gMainWindow);
        CopyBits(&gOffscreenPort.portBits, &gMainWindow->portBits, &vizRect, &vizRect, srcCopy, NULL);
    } else {
        SetPort(gMainWindow);
        BlitIcon1Bit(gVisualizerBits, 200, 200, 156, 35, srcCopy);
        if (gVizHover) {
            DrawVizFullscreenHoverIcon(356 - 8 - 12, 35 + 8, false);
        }
    }
    SetPort(savePort);
}

/*
 * Network Idle Callback:
 * Invoked inside `RunTCPControl` while waiting for MacTCP HTTP requests
 * (such as the periodic `/api/spotify/now-playing` poll every 4 seconds)
 * so the 30 FPS visualizer never pauses or locks up during network I/O!
 */
static void VisualizerNetworkIdlePump(void)
{
    unsigned long nowTicks;
    if (!gVisualizerOpen || !gInitialDataLoaded || gForYouViewOpen || gMiniPlayerMode || gSearchViewOpen || gAboutModalOpen || gConnState != CONN_READY) {
        return;
    }
    nowTicks = TickCount();
    if ((nowTicks - gLastVizTicks) >= 2) {
        gLastVizTicks = nowTicks;
        RenderVisualizerFastFrame();
    }
}

/* ============================================================================
 * Middleware API Actions & Data Loading
 * ============================================================================ */

static void SetTimelinePosition(long progressMs)
{
    if (progressMs < 0) progressMs = 0;
    if (gNowPlaying.durationMs > 0 && progressMs > gNowPlaying.durationMs) {
        progressMs = gNowPlaying.durationMs;
    }
    gNowPlaying.progressMs = progressMs;
    FormatMmSs(progressMs, gNowPlaying.progressMmSs);
    gTimelineAnchorMs    = progressMs;
    gTimelineAnchorTicks = TickCount();
}

static long GetCurrentInterpolatedProgressMs(void)
{
    unsigned long elapsedTicks;
    long curMs;
    if (!gNowPlaying.isPlaying || gTimelineFrozen) {
        return gNowPlaying.progressMs;
    }
    elapsedTicks = TickCount() - gTimelineAnchorTicks;
    /* Mac VBL tick rate is 60.15 Hz (60 ticks = 1000 ms -> elapsedTicks * 50 / 3) */
    curMs = gTimelineAnchorMs + (long)((elapsedTicks * 50UL) / 3UL);
    if (gNowPlaying.durationMs > 0 && curMs > gNowPlaying.durationMs) {
        curMs = gNowPlaying.durationMs;
    }
    return curMs;
}

static void ResetPollTimersAfterAction(void)
{
    /* Only defer the next 4-second background poll; never reset the timeline anchor! */
    gLastPollTicks = TickCount();
}

/* Immediately turns off the displayed album artwork (pure black in 1-bit QuickDraw) while waiting for new art */
static void BlankCurrentArtwork(void)
{
    memset(gCurrentArtworkBits, 0xFF, 5200);
    gLoadedArtTrackId[0] = '\0';
}

/*
 * Immediately switches the UI to the chosen track (highlighting it in the track list,
 * showing its Artist & Title under the artwork, resetting timeline to 00:00, and turning
 * off the old artwork) and locks it so background polls never jump back.
 */
static void LockOptimisticTrackSelection(const char *id, const char *name, const char *artist, long durationMs)
{
    gTimelineFrozen       = false;
    gPauseSynced          = false;
    gSeekLockUntilTicks   = 0;
    gNowPlaying.isPlaying = true;
    gForYouViewOpen       = false;

    if (durationMs > 0) {
        gNowPlaying.durationMs = durationMs;
        FormatMmSs(durationMs, gNowPlaying.durationMmSs);
    }
    SetTimelinePosition(0);

    if (id && id[0] != '\0') {
        SafeCopy(gNowPlaying.trackId, id, sizeof(gNowPlaying.trackId));
    }
    if (name && name[0] != '\0') {
        SafeCopy(gNowPlaying.trackName, name, sizeof(gNowPlaying.trackName));
    }
    if (artist && artist[0] != '\0') {
        SafeCopy(gNowPlaying.artistName, artist, sizeof(gNowPlaying.artistName));
    }

    gTrackLockUntilTicks = TickCount() + 360; /* 6-second lock against stale cloud polls */
    BlankCurrentArtwork();
    SyncRightSelectionToPlayingTrack();
    ResetPollTimersAfterAction();
}

/* Per-Mac network identity so multiple Macs on the same LAN each have their own speaker name */
static char gMySpeakerName[40] = "Macintosh 68k";
static char gMyClientId[32]    = "mac68k";

/*
 * Reads the Macintosh's own Machine Name ('STR ' -16413 in System 7 Sharing Setup)
 * or Chooser User Name ('STR ' -16096 in System 6/7) to give each Mac on a LAN its
 * own speaker name in MaciSpot's speaker chooser.
 */
static void InitMyMacSpeakerName(void)
{
    StringHandle sh;
    char rawName[40];
    short i, len = 0;

    rawName[0] = '\0';
    sh = GetString(-16413); /* Machine Name */
    if (!sh || *sh == NULL || (*sh)[0] == 0) {
        sh = GetString(-16096); /* Chooser User Name */
    }
    if (sh && *sh != NULL && (*sh)[0] > 0) {
        short pLen = (unsigned char)(*sh)[0];
        if (pLen > 31) pLen = 31;
        for (i = 1; i <= pLen; i++) {
            unsigned char c = (unsigned char)(*sh)[i];
            if (c >= 32 && c < 127 && c != '|') {
                rawName[len++] = (char)c;
            } else if (c == 0xD4 || c == 0xD5) {
                /* MacRoman curly apostrophe -> ASCII apostrophe */
                rawName[len++] = '\'';
            }
        }
        rawName[len] = '\0';
    }

    if (rawName[0] != '\0') {
        SafeCopy(gMySpeakerName, rawName, sizeof(gMySpeakerName));
    } else {
        SafeCopy(gMySpeakerName, "Macintosh 68k", sizeof(gMySpeakerName));
    }

    if (NetGetOurIP() != 0) {
        FormatIPAddress(NetGetOurIP(), gMyClientId);
    } else {
        SafeCopy(gMyClientId, "mac68k", sizeof(gMyClientId));
    }
}

static Boolean IsThisMacSelectedSpeaker(void)
{
    char suffix[16];
    unsigned char lastOctet = (unsigned char)(NetGetOurIP() & 0xFF);

    if (gNowPlaying.deviceName[0] == '\0') return false;

    if (strcmp(gNowPlaying.deviceName, gMySpeakerName) == 0) {
        return true;
    }

    /* Check disambiguated name when two Macs share the same base name: "<Name> (.<octet>)" */
    sprintf(suffix, " (.%u)", (unsigned int)lastOctet);
    if (strncmp(gNowPlaying.deviceName, gMySpeakerName, strlen(gMySpeakerName)) == 0 &&
        strstr(gNowPlaying.deviceName, suffix) != NULL) {
        return true;
    }

    /* Fallback if server reports generic "Macintosh 68k" and we are using default name */
    if (strcmp(gMySpeakerName, "Macintosh 68k") == 0 &&
        (strcmp(gNowPlaying.deviceName, "Macintosh 68k") == 0 ||
         strcmp(gNowPlaying.deviceName, "macintosh 68k") == 0)) {
        return true;
    }

    return false;
}

static void SyncLocalAudioWithSpeaker(void)
{
    Boolean is68kSpeaker = IsThisMacSelectedSpeaker();

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
    char path[160];
    if (!NetHasServer() || !gHttpBuf) return;

    if (gNowPlaying.trackId[0] != '\0') {
        char encId[96];
        UrlEncode(gNowPlaying.trackId, encId, sizeof(encId));
        sprintf(path, "/api/spotify/artwork?size=200&format=raw&track_id=%s", encId);
    } else {
        SafeCopy(path, "/api/spotify/artwork?size=200&format=raw", sizeof(path));
    }

    n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
    if (n >= 5208) {
        memcpy(gCurrentArtworkBits, gHttpBuf + 8, 5200);
        SafeCopy(gLoadedArtTrackId, gNowPlaying.trackId, sizeof(gLoadedArtTrackId));
    }
}

/*
 * Fast direct micro-call to fetch a track's liked (heart) status without waiting
 * for a full /now-playing payload. Backed by batch pre-warming on the server.
 */
static void FetchTrackLikedStatus(const char *trackId)
{
    long n;
    char encId[96], path[160], field[16];
    if (!NetHasServer() || !gHttpBuf || !trackId || trackId[0] == '\0') return;

    UrlEncode(trackId, encId, sizeof(encId));
    sprintf(path, "/api/spotify/liked-status?track_id=%s", encId);
    n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
    if (n > 0 && strncmp(gHttpBuf, "LIKED|", 6) == 0) {
        GetPipeField(gHttpBuf, 1, field, sizeof(field));
        gNowPlaying.isLiked = (field[0] == '1');
    }
}

/*
 * Fast direct micro-call to poll the authoritative playback timecode immediately
 * after Pause, Play, or Seek.
 */
static void PollDirectTimeline(void)
{
    long n, apiProgressMs, apiDurMs;
    unsigned long reqStartTicks, rttTicks;
    char field[64];
    Boolean seekLocked;

    if (!NetHasServer() || !gHttpBuf) return;

    reqStartTicks = TickCount();
    n = NetHttpGet("/api/spotify/timeline?direct=1", gHttpBuf, HTTP_BUF_SIZE);
    rttTicks = TickCount() - reqStartTicks;
    if (n <= 0 || strncmp(gHttpBuf, "TIMELINE|", 9) != 0) return;

    GetPipeField(gHttpBuf, 2, field, sizeof(field));
    apiProgressMs = atol(field);

    GetPipeField(gHttpBuf, 3, field, sizeof(field));
    apiDurMs = atol(field);
    if (apiDurMs > 0) {
        gNowPlaying.durationMs = apiDurMs;
        FormatMmSs(gNowPlaying.durationMs, gNowPlaying.durationMmSs);
    }

    if (gNowPlaying.isPlaying) {
        /* Add half-RTT compensation for response transit time */
        apiProgressMs += (long)((rttTicks * 25UL) / 3UL);
    }

    seekLocked = (gSeekLockUntilTicks != 0 && (long)(gSeekLockUntilTicks - TickCount()) > 0);
    if (seekLocked) {
        long curLocalMs = GetCurrentInterpolatedProgressMs();
        long diff = apiProgressMs - curLocalMs;
        if (diff < 0) diff = -diff;
        if (diff <= 2000) {
            SetTimelinePosition(apiProgressMs);
        }
    } else {
        SetTimelinePosition(apiProgressMs);
    }

    if (!gNowPlaying.isPlaying) {
        gPauseSynced = true;
    }
}

static void RefreshNowPlaying(Boolean fetchArtIfChanged)
{
    long n, apiProgressMs, apiDurMs, prevProgressMs;
    unsigned long reqStartTicks, rttTicks;
    char field[96];
    char incomingTrackId[64];
    char prevTrackId[64];
    char encTrack[64], encId[64], encName[96], path[280];
    Boolean trackLocked, seekLocked, sameTrack, prevWasPlaying;

    if (!NetHasServer() || !gHttpBuf) return;

    prevWasPlaying = gNowPlaying.isPlaying;
    prevProgressMs = GetCurrentInterpolatedProgressMs();
    SafeCopy(prevTrackId, gNowPlaying.trackId, sizeof(prevTrackId));
    UrlEncode((gNowPlaying.trackName[0] != '\0') ? gNowPlaying.trackId : "", encTrack, sizeof(encTrack));
    UrlEncode(gMyClientId, encId, sizeof(encId));
    UrlEncode(gMySpeakerName, encName, sizeof(encName));
    sprintf(
        path,
        "/api/spotify/poll?track_id=%s&client_id=%s&client_name=%s",
        encTrack,
        encId,
        encName
    );

    reqStartTicks = TickCount();
    n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
    rttTicks = TickCount() - reqStartTicks;
    if (n <= 0 || strncmp(gHttpBuf, "POLL|", 5) != 0) {
        return;
    }

    GetPipeField(gHttpBuf, 2, incomingTrackId, sizeof(incomingTrackId));
    trackLocked = (gTrackLockUntilTicks != 0 && (long)(gTrackLockUntilTicks - TickCount()) > 0);

    /*
     * If the user recently clicked a track and Spotify's cloud is still reporting the
     * previous track ID, keep our locked track selection / Artist / Title / timeline!
     */
    if (trackLocked &&
        gNowPlaying.trackId[0] != '\0' &&
        incomingTrackId[0] != '\0' &&
        strcmp(incomingTrackId, gNowPlaying.trackId) != 0) {
        SyncLocalAudioWithSpeaker();
        return;
    }
    gTrackLockUntilTicks = 0;

    sameTrack = (prevTrackId[0] != '\0' && strcmp(incomingTrackId, prevTrackId) == 0);
    if (!sameTrack) {
        gSeekLockUntilTicks = 0;
        gPauseSynced        = false;
    }

    GetPipeField(gHttpBuf, 1, field, sizeof(field));
    gNowPlaying.isPlaying = (field[0] == '1');
    if (gNowPlaying.isPlaying) {
        gTimelineFrozen = false;
        gPauseSynced    = false;
    }

    SafeCopy(gNowPlaying.trackId, incomingTrackId, sizeof(gNowPlaying.trackId));
    if (!sameTrack || gNowPlaying.trackName[0] == '\0') {
        GetPipeField(gHttpBuf, 10, field, sizeof(field));
        if (field[0] != '\0' || !sameTrack) {
            SafeCopy(gNowPlaying.trackName, field, sizeof(gNowPlaying.trackName));
        }
        GetPipeField(gHttpBuf, 11, field, sizeof(field));
        if (field[0] != '\0' || !sameTrack) {
            SafeCopy(gNowPlaying.artistName, field, sizeof(gNowPlaying.artistName));
        }
    }

    GetPipeField(gHttpBuf, 4, field, sizeof(field));
    apiDurMs = atol(field);
    if (apiDurMs != gNowPlaying.durationMs) {
        gNowPlaying.durationMs = apiDurMs;
        FormatMmSs(gNowPlaying.durationMs, gNowPlaying.durationMmSs);
    }

    GetPipeField(gHttpBuf, 3, field, sizeof(field));
    apiProgressMs = atol(field);
    if (gNowPlaying.isPlaying) {
        /* Add half-RTT compensation for response transit time */
        apiProgressMs += (long)((rttTicks * 25UL) / 3UL);
    }

    seekLocked = (sameTrack && gSeekLockUntilTicks != 0 && (long)(gSeekLockUntilTicks - TickCount()) > 0);
    if (!gNowPlaying.isPlaying && gPauseSynced && sameTrack) {
        /* Keep exact paused timecode already synced by PollDirectTimeline() */
        FormatMmSs(gNowPlaying.progressMs, gNowPlaying.progressMmSs);
    } else if (seekLocked) {
        /* During the 15s seek lock, never bounce back to a stale pre-seek timecode */
        long curLocalMs = GetCurrentInterpolatedProgressMs();
        long diff = apiProgressMs - curLocalMs;
        if (diff < 0) diff = -diff;
        if (diff <= 2000) {
            SetTimelinePosition(apiProgressMs);
        } else {
            gNowPlaying.progressMs = curLocalMs;
            FormatMmSs(gNowPlaying.progressMs, gNowPlaying.progressMmSs);
        }
    } else if (sameTrack && gNowPlaying.isPlaying && prevWasPlaying) {
        /* While playing the same track, keep our drift-free 60.15 Hz hardware tick clock
         * unless the cloud timecode differs by >1.5s (e.g. external seek). */
        long curLocalMs = GetCurrentInterpolatedProgressMs();
        long diff = apiProgressMs - curLocalMs;
        if (diff < 0) diff = -diff;
        if (diff > 1500) {
            SetTimelinePosition(apiProgressMs);
        } else {
            gNowPlaying.progressMs = curLocalMs;
            FormatMmSs(gNowPlaying.progressMs, gNowPlaying.progressMmSs);
        }
    } else {
        SetTimelinePosition(apiProgressMs);
    }

    GetPipeField(gHttpBuf, 5, field, sizeof(field));
    gNowPlaying.volumePct = (short)atoi(field);

    GetPipeField(gHttpBuf, 6, gNowPlaying.shuffleMode, sizeof(gNowPlaying.shuffleMode));
    GetPipeField(gHttpBuf, 7, gNowPlaying.repeatState, sizeof(gNowPlaying.repeatState));

    GetPipeField(gHttpBuf, 8, field, sizeof(field));
    gNowPlaying.isLiked = (field[0] == '1');

    GetPipeField(gHttpBuf, 9, field, sizeof(field));
    if (gSpeakerLockUntilTicks != 0 && (long)(gSpeakerLockUntilTicks - TickCount()) > 0) {
        /* Keep user-selected / startup-restored speaker locked for 5s against cloud lag */
        if (gSavedSpeakerName[0] != '\0') {
            SafeCopy(gNowPlaying.deviceName, gSavedSpeakerName, sizeof(gNowPlaying.deviceName));
        }
    } else if (field[0] != '\0' && strcmp(field, "No Active Device") != 0) {
        SafeCopy(gNowPlaying.deviceName, field, sizeof(gNowPlaying.deviceName));
        if (strcmp(field, gSavedSpeakerName) != 0) {
            /* Another device changed the active speaker — respect & remember it! */
            SafeCopy(gSavedSpeakerName, field, sizeof(gSavedSpeakerName));
            gSavedSpeakerId[0] = '\0';
            SaveServerPrefs(NetGetServerIP());
        }
    } else if (gNowPlaying.deviceName[0] == '\0' && gSavedSpeakerName[0] != '\0') {
        SafeCopy(gNowPlaying.deviceName, gSavedSpeakerName, sizeof(gNowPlaying.deviceName));
    }

    if (gNowPlaying.trackId[0] == '\0') {
        memset(gCurrentArtworkBits, 0xFF, 5200);
        gLoadedArtTrackId[0] = '\0';
    } else {
        if (prevTrackId[0] == '\0') {
            /* If API reports a playing track on launch (or playback starts from idle), show artwork instead of For You */
            gForYouViewOpen = false;
        }
        if (fetchArtIfChanged && !gVisualizerOpen && strcmp(gNowPlaying.trackId, gLoadedArtTrackId) != 0) {
            FetchCurrentArtwork();
        }
    }

    SyncRightSelectionToPlayingTrack();
    MaybeAppendSmartQueueToRightList(false);

    /*
     * If a track in `gRightItems` just finished playing at the end of a queued batch
     * (Spotify transitions from playing near the end of the track to stopped at 0:00
     * on the same track while more smart-queue tracks exist below it), automatically
     * continue playback into the next track in `gRightItems`!
     */
    if (prevWasPlaying && !gNowPlaying.isPlaying && !gTimelineFrozen &&
        sameTrack && apiProgressMs <= 1000 && prevProgressMs >= 5000) {
        short curIdx = FindPlayingTrackIndexInRightList();
        if (curIdx >= 0) {
            short nextIdx = curIdx + 1;
            while (nextIdx < gRightCount && strcmp(gRightItems[nextIdx].kind, "QUEUE_SEP") == 0) {
                nextIdx++;
            }
            if (nextIdx < gRightCount && strcmp(gRightItems[nextIdx].kind, "TRACK") == 0) {
                PlayRightItemAtIndex(nextIdx);
                return;
            }
        }
    }

    SyncLocalAudioWithSpeaker();
}

/*
 * Appends the smart queue separator (`QUEUE_SEP`) and 6 upcoming queue/recommended tracks
 * to the end of `gRightItems` when either:
 *   1. A track list is loaded with fewer than 8 entries (`isInitialLoad && gRightCount < 8`), or
 *   2. Playback is approaching the end of a longer track list (`<= 4` songs left to play).
 */
static void MaybeAppendSmartQueueToRightList(Boolean isInitialLoad)
{
    short i, curIdx, remTracks = 0, lastTrackIdx = -1;
    Boolean shouldFetch = false;
    Boolean playingInList = false;
    unsigned long nowTicks;
    char encId[64], encArtist[96], encTrack[96], path[384];
    long n;

    if (!NetHasServer() || !gHttpBuf) return;
    if (gRightCount <= 0 || gRightCount + 2 >= MAX_RIGHT_ITEMS) return;
    if (strcmp(gRightItems[0].kind, "TRACK") != 0) return;

    for (i = gRightCount - 1; i >= 0; i--) {
        if (strcmp(gRightItems[i].kind, "TRACK") == 0) {
            lastTrackIdx = i;
            break;
        }
    }
    if (lastTrackIdx < 0) return;

    curIdx = FindPlayingTrackIndexInRightList();
    if (curIdx >= 0) {
        playingInList = true;
        for (i = curIdx + 1; i < gRightCount; i++) {
            if (strcmp(gRightItems[i].kind, "TRACK") == 0) {
                remTracks++;
            }
        }
    }

    if (isInitialLoad && gRightCount < 8) {
        shouldFetch = true;
    } else if (!isInitialLoad && curIdx >= 0 && remTracks <= 4) {
        shouldFetch = true;
    }

    if (!shouldFetch) return;
    if (gQueueTailFetchedForCount == gRightCount) return;

    nowTicks = TickCount();
    if (!isInitialLoad && gLastQueueTailFetchTicks != 0 && (nowTicks - gLastQueueTailFetchTicks) < 120) {
        return;
    }

    gQueueTailFetchedForCount = gRightCount;
    gLastQueueTailFetchTicks  = nowTicks;

    UrlEncode(gRightItems[lastTrackIdx].id,     encId,     sizeof(encId));
    UrlEncode(gRightItems[lastTrackIdx].artist, encArtist, sizeof(encArtist));
    UrlEncode(gRightItems[lastTrackIdx].name,   encTrack,  sizeof(encTrack));
    sprintf(
        path,
        "/api/spotify/queue-tail?limit=6&playing_in_list=%d&last_id=%s&seed_artist=%s&seed_track=%s",
        playingInList ? 1 : 0,
        encId,
        encArtist,
        encTrack
    );

    n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
    if (n > 0 && strncmp(gHttpBuf, "QUEUE|", 6) == 0) {
        const char *line = gHttpBuf;

        if (!gHasQueueSeparator && gRightCount < MAX_RIGHT_ITEMS) {
            RightItem *sep = &gRightItems[gRightCount++];
            memset(sep, 0, sizeof(*sep));
            SafeCopy(sep->kind, "QUEUE_SEP", sizeof(sep->kind));
            gHasQueueSeparator = true;
        }

        while (line && *line && gRightCount < MAX_RIGHT_ITEMS) {
            const char *nextLine = strchr(line, '\n');
            char rowKind[16];
            GetPipeField(line, 0, rowKind, sizeof(rowKind));
            if (strcmp(rowKind, "QUEUE") == 0 || strcmp(rowKind, "TRACK") == 0) {
                char durStr[24];
                RightItem *it = &gRightItems[gRightCount++];
                SafeCopy(it->kind, "TRACK", sizeof(it->kind));
                GetPipeField(line, 1, it->id, sizeof(it->id));
                GetPipeField(line, 2, it->name, sizeof(it->name));
                GetPipeField(line, 3, it->artist, sizeof(it->artist));
                GetPipeField(line, 5, durStr, sizeof(durStr));
                it->durationMs = atol(durStr);
                FormatMmSs(it->durationMs, it->durationMmSs);
                GetPipeField(line, 7, it->uri, sizeof(it->uri));
            }
            if (!nextLine) break;
            line = nextLine + 1;
        }
    }
}

static void ParseTracksResponseIntoRightList(const char *buf)
{
    const char *line = buf;
    gRightCount               = 0;
    gRightSelected            = -1;
    gRightScroll              = 0;
    gLastClickedRow           = -1;
    gLastClickTicks           = 0;
    gHasQueueSeparator        = false;
    gQueueTailFetchedForCount = -1;

    while (line && *line && gRightCount < MAX_RIGHT_ITEMS) {
        const char *nextLine = strchr(line, '\n');
        char rowKind[16];
        GetPipeField(line, 0, rowKind, sizeof(rowKind));

        if (strcmp(rowKind, "TRACK") == 0 ||
            strcmp(rowKind, "RECENT") == 0 ||
            strcmp(rowKind, "TOP_TRACK") == 0 ||
            strcmp(rowKind, "QUEUE") == 0) {
            char durStr[24];
            RightItem *it = &gRightItems[gRightCount++];
            SafeCopy(it->kind, "TRACK", sizeof(it->kind));
            GetPipeField(line, 1, it->id, sizeof(it->id));
            GetPipeField(line, 2, it->name, sizeof(it->name));
            GetPipeField(line, 3, it->artist, sizeof(it->artist));
            GetPipeField(line, 5, durStr, sizeof(durStr));
            it->durationMs = atol(durStr);
            FormatMmSs(it->durationMs, it->durationMmSs);
            GetPipeField(line, 7, it->uri, sizeof(it->uri));
        } else if (strcmp(rowKind, "ALBUM") == 0) {
            RightItem *it = &gRightItems[gRightCount++];
            SafeCopy(it->kind, "ALBUM", sizeof(it->kind));
            GetPipeField(line, 1, it->id, sizeof(it->id));
            GetPipeField(line, 2, it->name, sizeof(it->name));
            GetPipeField(line, 3, it->artist, sizeof(it->artist));
            it->durationMs = 0;
            it->durationMmSs[0] = '\0';
            GetPipeField(line, 6, it->uri, sizeof(it->uri));
        }

        if (!nextLine) break;
        line = nextLine + 1;
    }

    SyncRightSelectionToPlayingTrack();
    MaybeAppendSmartQueueToRightList(true);
}

static void LoadSelectedLeftItemIntoRightList(void)
{
    LeftItem *sel;
    char path[200];
    char encName[96];
    long n;

    if (!NetHasServer() || gLeftSelected < 0 || gLeftSelected >= gLeftCount) return;
    sel = &gLeftItems[gLeftSelected];

    SafeCopy(gRightHeaderLabel, "Tracks", sizeof(gRightHeaderLabel));
    gActiveContextUri[0] = '\0';

    if (strcmp(sel->kind, "LIKED") == 0) {
        n = NetHttpGet("/api/spotify/liked?limit=40", gHttpBuf, HTTP_BUF_SIZE);
        if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
    } else if (strcmp(sel->kind, "ALBUM") == 0) {
        SafeCopy(gActiveContextUri, sel->uri, sizeof(gActiveContextUri));
        sprintf(path, "/api/spotify/album/%s/tracks?limit=50", sel->id);
        n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
    } else if (strcmp(sel->kind, "PLAYLIST") == 0) {
        SafeCopy(gActiveContextUri, sel->uri, sizeof(gActiveContextUri));
        sprintf(path, "/api/spotify/playlist/%s/tracks?limit=40", sel->id);
        n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
        if (gRightCount == 0 && sel->uri[0] != '\0') {
            RightItem *it = &gRightItems[0];
            SafeCopy(it->kind, "PLAYLIST", sizeof(it->kind));
            SafeCopy(it->id, sel->id, sizeof(it->id));
            SafeCopy(it->name, sel->name, sizeof(it->name));
            SafeCopy(it->uri, sel->uri, sizeof(it->uri));
            gRightCount    = 1;
            gRightSelected = 0;
        }
    } else if (strcmp(sel->kind, "ARTIST") == 0) {
        /* Leave gActiveContextUri empty so clicking an artist's popular track plays that exact track
         * immediately and queues the artist's popular tracks (Spotify rejects offset on artist context). */
        UrlEncode(sel->name, encName, sizeof(encName));
        sprintf(path, "/api/spotify/artist/%s/top-tracks?name=%s", sel->id, encName);
        n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
    } else if (strcmp(sel->kind, "SHOW") == 0 || strcmp(sel->kind, "PODCAST") == 0) {
        sprintf(path, "/api/spotify/show/%s/episodes?limit=30", sel->id);
        n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        if (n > 0) ParseTracksResponseIntoRightList(gHttpBuf);
    } else if (strcmp(sel->kind, "FOR_YOU") == 0) {
        sprintf(path, "/api/spotify/for-you/%s/tracks", sel->id);
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

static Boolean LeftItemsMatch(const LeftItem *a, const LeftItem *b)
{
    if (!a || !b) return false;
    if (strcmp(a->kind, b->kind) != 0) return false;
    if (a->id[0] != '\0' && b->id[0] != '\0' && strcmp(a->id, b->id) == 0) return true;
    if (a->uri[0] != '\0' && b->uri[0] != '\0' && strcmp(a->uri, b->uri) == 0) return true;
    return false;
}

static void AppendLeftItemUnique(const char *kind, const char *id, const char *name, const char *uri)
{
    LeftItem cand;
    short i;
    if (gLeftCount >= MAX_LEFT_ITEMS) return;
    SafeCopy(cand.kind, kind ? kind : "PLAYLIST", sizeof(cand.kind));
    SafeCopy(cand.id,   id   ? id   : "",         sizeof(cand.id));
    SafeCopy(cand.name, name ? name : "Untitled", sizeof(cand.name));
    SafeCopy(cand.uri,  uri  ? uri  : "",         sizeof(cand.uri));

    for (i = 0; i < gLeftCount; i++) {
        if (LeftItemsMatch(&gLeftItems[i], &cand)) {
            return;
        }
    }
    gLeftItems[gLeftCount++] = cand;
}

static void PopulateLeftColumnItems(void)
{
    long n;
    gLeftCount = 0;

    /* Dedicated top slot for the actively entered Album or Playlist (regardless of tab) */
    if (gHasPinnedLeftItem && gPinnedLeftItem.name[0] != '\0') {
        gLeftItems[gLeftCount++] = gPinnedLeftItem;
    }

    if (gHomeMode) {
        /* Pin "Liked Songs" at the top of the Home library view */
        AppendLeftItemUnique("LIKED",    "liked",  "Liked Songs",     "");
        AppendLeftItemUnique("HOME_CAT", "recent", "Recently Played", "");
        AppendLeftItemUnique("HOME_CAT", "top",    "Top Tracks",      "");
        AppendLeftItemUnique("HOME_CAT", "albums", "Saved Albums",    "");
        AppendLeftItemUnique("HOME_CAT", "new",    "New Releases",    "");

        if (NetHasServer() && gHttpBuf) {
            n = NetHttpGet("/api/spotify/home-suggestions", gHttpBuf, HTTP_BUF_SIZE);
            if (n > 0) {
                const char *line = gHttpBuf;
                while (line && *line && gLeftCount < MAX_LEFT_ITEMS) {
                    const char *nextLine = strchr(line, '\n');
                    char rowKind[16], idBuf[64], nameBuf[64], uriBuf[96];
                    GetPipeField(line, 0, rowKind, sizeof(rowKind));
                    if (strcmp(rowKind, "PLAYLIST") == 0 ||
                        strcmp(rowKind, "ALBUM") == 0 ||
                        strcmp(rowKind, "ARTIST") == 0 ||
                        strcmp(rowKind, "SHOW") == 0) {
                        GetPipeField(line, 1, idBuf, sizeof(idBuf));
                        GetPipeField(line, 2, nameBuf, sizeof(nameBuf));
                        GetPipeField(line, 5, uriBuf, sizeof(uriBuf));
                        AppendLeftItemUnique(rowKind, idBuf, nameBuf, uriBuf);
                    }
                    if (!nextLine) break;
                    line = nextLine + 1;
                }
            }
        }
    } else {
        if (NetHasServer() && gHttpBuf) {
            n = NetHttpGet("/api/spotify/playlists?limit=35", gHttpBuf, HTTP_BUF_SIZE);
            if (n > 0) {
                const char *line = gHttpBuf;
                while (line && *line && gLeftCount < MAX_LEFT_ITEMS) {
                    const char *nextLine = strchr(line, '\n');
                    char rowKind[16], idBuf[64], nameBuf[64], uriBuf[96];
                    GetPipeField(line, 0, rowKind, sizeof(rowKind));
                    if (strcmp(rowKind, "PLAYLIST") == 0) {
                        GetPipeField(line, 1, idBuf, sizeof(idBuf));
                        GetPipeField(line, 2, nameBuf, sizeof(nameBuf));
                        GetPipeField(line, 5, uriBuf, sizeof(uriBuf));
                        AppendLeftItemUnique("PLAYLIST", idBuf, nameBuf, uriBuf);
                    }
                    if (!nextLine) break;
                    line = nextLine + 1;
                }
            }
        }
    }
}

/*
 * Dedicates the top position (index 0) on the left side — regardless of whether the user
 * is in the Home tab or Playlists tab — to the actively entered Album or Playlist!
 */
static void PinActiveContextAtTopOfLeftColumn(const char *kind, const char *id, const char *name, const char *uri)
{
    short i, dupIdx = -1;

    gHasPinnedLeftItem = true;
    SafeCopy(gPinnedLeftItem.kind, kind ? kind : "PLAYLIST", sizeof(gPinnedLeftItem.kind));
    SafeCopy(gPinnedLeftItem.id,   id   ? id   : "",         sizeof(gPinnedLeftItem.id));
    SafeCopy(gPinnedLeftItem.name, name ? name : "Untitled", sizeof(gPinnedLeftItem.name));
    SafeCopy(gPinnedLeftItem.uri,  uri  ? uri  : "",         sizeof(gPinnedLeftItem.uri));

    if (gLeftCount == 0) {
        PopulateLeftColumnItems();
    } else {
        /* If an existing row matches, remove it before placing at index 0 */
        for (i = 0; i < gLeftCount; i++) {
            if (LeftItemsMatch(&gLeftItems[i], &gPinnedLeftItem)) {
                dupIdx = i;
                break;
            }
        }
        if (dupIdx == 0) {
            gLeftItems[0] = gPinnedLeftItem;
        } else if (dupIdx > 0) {
            for (i = dupIdx; i > 0; i--) {
                gLeftItems[i] = gLeftItems[i - 1];
            }
            gLeftItems[0] = gPinnedLeftItem;
        } else {
            if (gLeftCount < MAX_LEFT_ITEMS) {
                gLeftCount++;
            }
            for (i = gLeftCount - 1; i > 0; i--) {
                gLeftItems[i] = gLeftItems[i - 1];
            }
            gLeftItems[0] = gPinnedLeftItem;
        }
    }

    gLeftSelected = 0;
    gLeftScroll   = 0;
}

static void LoadLeftColumn(void)
{
    gLeftSelected = 0;
    gLeftScroll   = 0;
    PopulateLeftColumnItems();
    LoadSelectedLeftItemIntoRightList();
}

static void PlayRightItemAtIndex(short idx)
{
    RightItem *it;
    char path[320];
    char encUri[128];

    if (!NetHasServer() || idx < 0 || idx >= gRightCount) return;
    it = &gRightItems[idx];
    if (strcmp(it->kind, "QUEUE_SEP") == 0) return;
    gRightSelected = idx;

    if (strcmp(it->kind, "ALBUM") == 0) {
        char albId[64], albName[64], albUri[96];
        SafeCopy(albId,   it->id,   sizeof(albId));
        SafeCopy(albName, it->name, sizeof(albName));
        SafeCopy(albUri,  it->uri,  sizeof(albUri));

        PinActiveContextAtTopOfLeftColumn("ALBUM", albId, albName, albUri);
        SafeCopy(gActiveContextUri, albUri, sizeof(gActiveContextUri));
        SafeCopy(gRightHeaderLabel, "Tracks", sizeof(gRightHeaderLabel));
        sprintf(path, "/api/spotify/album/%s/tracks?limit=50", albId);
        if (NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE) > 0) {
            ParseTracksResponseIntoRightList(gHttpBuf);
            RenderCanvas();
        }
        return;
    }

    /*
     * Prioritized New Song Load Sequence:
     * 0. Optimistic UI lock (instant highlight, title/artist, 00:00 timeline, blank old art)
     * 1. Audio playback first (/api/spotify/play + local audio stream)
     * 2. Album artwork second (direct /api/spotify/artwork)
     * 3. Heart / Liked status third (direct /api/spotify/liked-status)
     */
    LockOptimisticTrackSelection(it->id, it->name, it->artist, it->durationMs);
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

    /* 1. Audio playback first */
    NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
    SetTimelinePosition(0);
    SyncLocalAudioWithSpeaker();

    if (strcmp(it->kind, "PLAYLIST") == 0) {
        RefreshNowPlaying(true);
    } else {
        /* 2. Album artwork second */
        if (!gVisualizerOpen) {
            FetchCurrentArtwork();
            RenderCanvas();
        }
        /* 3. Heart / Liked status third */
        FetchTrackLikedStatus(gNowPlaying.trackId);
        /* 4. If within 4 songs of the end of the track list, append 6 more smart-queue tracks */
        MaybeAppendSmartQueueToRightList(false);
    }
    ResetPollTimersAfterAction();
    RenderCanvas();
}

static void TriggerNextTrack(void)
{
    short curIdx = FindPlayingTrackIndexInRightList();

    /*
     * If we know the next track in `gRightItems` (either in a tracklist without context_uri,
     * or in a playlist/album when shuffle is off), step directly to `nextIdx` so the track
     * list highlight, Artist/Title, and blanked artwork switch immediately on click!
     */
    if (curIdx >= 0 && gRightCount > 0 &&
        (gActiveContextUri[0] == '\0' || strcmp(gNowPlaying.shuffleMode, "off") == 0)) {
        short nextIdx = curIdx + 1;
        while (nextIdx < gRightCount && strcmp(gRightItems[nextIdx].kind, "QUEUE_SEP") == 0) {
            nextIdx++;
        }
        if (nextIdx >= gRightCount && strcmp(gNowPlaying.repeatState, "off") != 0) {
            nextIdx = 0;
        }
        if (nextIdx < gRightCount && strcmp(gRightItems[nextIdx].kind, "TRACK") == 0) {
            PlayRightItemAtIndex(nextIdx);
            return;
        }
    }

    /* When shuffle is active, blank old artwork and reset timeline immediately while server picks next track */
    gTimelineFrozen       = true;
    gPauseSynced          = false;
    gSeekLockUntilTicks   = 0;
    gNowPlaying.isPlaying = true;
    SetTimelinePosition(0);
    gTrackLockUntilTicks  = 0;
    BlankCurrentArtwork();
    RenderCanvas();

    NetHttpGet("/api/spotify/next", gHttpBuf, HTTP_BUF_SIZE);
    SetTimelinePosition(0);
    RefreshNowPlaying(true);
    ResetPollTimersAfterAction();
    RenderCanvas();
}

static void TriggerPreviousTrack(void)
{
    short curIdx = FindPlayingTrackIndexInRightList();

    if (curIdx >= 0 && gRightCount > 0 &&
        (gActiveContextUri[0] == '\0' || strcmp(gNowPlaying.shuffleMode, "off") == 0)) {
        short prevIdx = (curIdx > 0 && gNowPlaying.progressMs < 4000) ? (curIdx - 1) : curIdx;
        while (prevIdx > 0 && strcmp(gRightItems[prevIdx].kind, "QUEUE_SEP") == 0) {
            prevIdx--;
        }
        if (prevIdx >= 0 && strcmp(gRightItems[prevIdx].kind, "TRACK") == 0) {
            PlayRightItemAtIndex(prevIdx);
            return;
        }
    }

    gTimelineFrozen       = true;
    gPauseSynced          = false;
    gSeekLockUntilTicks   = 0;
    gNowPlaying.isPlaying = true;
    SetTimelinePosition(0);
    gTrackLockUntilTicks  = 0;
    BlankCurrentArtwork();
    RenderCanvas();

    NetHttpGet("/api/spotify/previous", gHttpBuf, HTTP_BUF_SIZE);
    SetTimelinePosition(0);
    RefreshNowPlaying(true);
    ResetPollTimersAfterAction();
    RenderCanvas();
}

static void TriggerToggleLike(void)
{
    Boolean wantLiked;
    if (!NetHasServer() || gNowPlaying.trackId[0] == '\0') return;

    wantLiked = !gNowPlaying.isLiked;
    gNowPlaying.isLiked = wantLiked;
    RenderCanvas();

    NetHttpGet("/api/spotify/toggle-like", gHttpBuf, HTTP_BUF_SIZE);
    ResetPollTimersAfterAction();

    /* If "Liked" is currently selected in the left column, refresh the right column tracklist */
    if (!gHomeMode && gLeftSelected == 0) {
        LoadSelectedLeftItemIntoRightList();
        RenderCanvas();
    }
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
            char durStr[24];
            SearchItem *s = &gSearchItems[gSearchCount++];
            SafeCopy(s->kind, "TRACK", sizeof(s->kind));
            GetPipeField(line, 1, s->id, sizeof(s->id));
            GetPipeField(line, 2, s->title, sizeof(s->title));
            GetPipeField(line, 3, s->subtitle, sizeof(s->subtitle));
            GetPipeField(line, 5, durStr, sizeof(durStr));
            s->durationMs = atol(durStr);
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
            GetPipeField(line, 4, s->subtitle, sizeof(s->subtitle));
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
    AddQueryToSearchHistory(gSearchQuery);
    UrlEncode(gSearchQuery, encQ, sizeof(encQ));
    sprintf(path, "/api/spotify/search?q=%s&type=all&limit=10", encQ);

    if (NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE) > 0) {
        ParseSearchResults(gHttpBuf);
    } else {
        gSearchCount = 0;
    }
    gSearchFocused     = false;
    gSearchHistoryOpen = false;
    gSearchViewOpen    = true;
    RenderCanvas();
}

static void ActivateSearchResultRow(short idx)
{
    SearchItem *s;
    char encUri[128];
    char path[256];
    short i;

    if (!NetHasServer() || idx < 0 || idx >= gSearchCount) return;
    s = &gSearchItems[idx];

    /* Single-click plays immediately and closes search view back to main UI */
    gSearchViewOpen    = false;
    gSearchHistoryOpen = false;
    gSearchFocused     = false;

    UrlEncode(s->uri[0] ? s->uri : s->id, encUri, sizeof(encUri));
    if (strcmp(s->kind, "ALBUM") == 0 || strcmp(s->kind, "PLAYLIST") == 0) {
        Boolean lockedFirstTrack = false;
        Boolean isPlaylist       = (strcmp(s->kind, "PLAYLIST") == 0);

        /* Dedicate top position on the left side (regardless of current tab) to the active Album/Playlist! */
        PinActiveContextAtTopOfLeftColumn(s->kind, s->id, s->title, s->uri);

        SafeCopy(gActiveContextUri, s->uri, sizeof(gActiveContextUri));
        SafeCopy(gRightHeaderLabel, "Tracks", sizeof(gRightHeaderLabel));

        if (!isPlaylist) {
            sprintf(path, "/api/spotify/album/%s/tracks?limit=50", s->id);
        } else {
            sprintf(path, "/api/spotify/playlist/%s/tracks?limit=40", s->id);
        }
        if (NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE) > 0) {
            ParseTracksResponseIntoRightList(gHttpBuf);
        }

        if (gRightCount > 0 &&
            strcmp(gRightItems[0].kind, "TRACK") == 0 &&
            strcmp(gNowPlaying.shuffleMode, "off") == 0) {
            gRightSelected = 0;
            LockOptimisticTrackSelection(
                gRightItems[0].id,
                gRightItems[0].name,
                gRightItems[0].artist,
                gRightItems[0].durationMs
            );
            lockedFirstTrack = true;
        } else {
            gTrackLockUntilTicks  = 0;
            gTimelineFrozen       = true;
            gPauseSynced          = false;
            gSeekLockUntilTicks   = 0;
            gNowPlaying.isPlaying = true;
            SetTimelinePosition(0);
            SafeCopy(gNowPlaying.trackName, s->title, sizeof(gNowPlaying.trackName));
            SafeCopy(gNowPlaying.artistName, s->subtitle, sizeof(gNowPlaying.artistName));
            BlankCurrentArtwork();
        }
        RenderCanvas();

        /* 1. Audio playback first */
        sprintf(path, "/api/spotify/play?context_uri=%s", encUri);
        NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        SetTimelinePosition(0);
        SyncLocalAudioWithSpeaker();

        if (lockedFirstTrack) {
            /* 2. Album artwork second */
            if (!gVisualizerOpen) {
                FetchCurrentArtwork();
                RenderCanvas();
            }
            /* 3. Heart / Liked status third */
            FetchTrackLikedStatus(gNowPlaying.trackId);
        } else {
            RefreshNowPlaying(true);
        }
        ResetPollTimersAfterAction();
        RenderCanvas();
    } else {
        /* Single song from search: leave nothing selected in the left panel! */
        gHasPinnedLeftItem        = false;
        gLeftSelected             = -1;
        gActiveContextUri[0]      = '\0';
        gHasQueueSeparator        = false;
        gQueueTailFetchedForCount = -1;
        SafeCopy(gRightHeaderLabel, "Tracks", sizeof(gRightHeaderLabel));
        gRightCount = 0;
        for (i = 0; i < gSearchCount && gRightCount < MAX_RIGHT_ITEMS; i++) {
            if (strcmp(gSearchItems[i].kind, "TRACK") == 0) {
                RightItem *rt = &gRightItems[gRightCount++];
                SafeCopy(rt->kind, "TRACK", sizeof(rt->kind));
                SafeCopy(rt->id, gSearchItems[i].id, sizeof(rt->id));
                SafeCopy(rt->name, gSearchItems[i].title, sizeof(rt->name));
                SafeCopy(rt->artist, gSearchItems[i].subtitle, sizeof(rt->artist));
                rt->durationMs = gSearchItems[i].durationMs;
                FormatMmSs(rt->durationMs, rt->durationMmSs);
                SafeCopy(rt->uri, gSearchItems[i].uri, sizeof(rt->uri));
            }
        }
        LockOptimisticTrackSelection(s->id, s->title, s->subtitle, s->durationMs);
        RenderCanvas();

        /* 1. Audio playback first */
        sprintf(path, "/api/spotify/play?uri=%s", encUri);
        NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        SetTimelinePosition(0);
        SyncLocalAudioWithSpeaker();

        /* 2. Album artwork second */
        if (!gVisualizerOpen) {
            FetchCurrentArtwork();
            RenderCanvas();
        }

        /* 3. Heart / Liked status third */
        FetchTrackLikedStatus(gNowPlaying.trackId);
        ResetPollTimersAfterAction();
        RenderCanvas();
    }
}

static void LoadDevicesAndTogglePopover(void)
{
    long n;
    char encId[64], encName[96], path[220];
    if (gSpeakerPopupOpen) {
        gSpeakerPopupOpen = false;
        RenderCanvas();
        return;
    }

    if (!NetHasServer() || !gHttpBuf) return;
    gDeviceCount = 0;

    UrlEncode(gMyClientId, encId, sizeof(encId));
    UrlEncode(gMySpeakerName, encName, sizeof(encName));
    sprintf(path, "/api/spotify/devices?client_id=%s&client_name=%s", encId, encName);

    n = NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
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

static Boolean gStartupSpeakerSent = false;

static void InitForYouDefaults(void)
{
    short i;
    /* 1. Hot 100: https://open.spotify.com/playlist/6UeSakyzhiEt4NB3UAd6NQ */
    SafeCopy(gForYouItems[0].slotId,   "6UeSakyzhiEt4NB3UAd6NQ", sizeof(gForYouItems[0].slotId));
    SafeCopy(gForYouItems[0].title,    "Hot 100", sizeof(gForYouItems[0].title));
    SafeCopy(gForYouItems[0].line1,    "This weeks most popular", sizeof(gForYouItems[0].line1));
    SafeCopy(gForYouItems[0].line2,    "songs.", sizeof(gForYouItems[0].line2));
    SafeCopy(gForYouItems[0].imageUrl, "spotify:playlist:6UeSakyzhiEt4NB3UAd6NQ", sizeof(gForYouItems[0].imageUrl));

    /* 2. Release Radar: https://open.spotify.com/playlist/5MZ9NB8TxCE9MVeHM0NAl1 */
    SafeCopy(gForYouItems[1].slotId,   "5MZ9NB8TxCE9MVeHM0NAl1", sizeof(gForYouItems[1].slotId));
    SafeCopy(gForYouItems[1].title,    "Release Radar", sizeof(gForYouItems[1].title));
    SafeCopy(gForYouItems[1].line1,    "The latest releases,", sizeof(gForYouItems[1].line1));
    SafeCopy(gForYouItems[1].line2,    "updated each Friday.", sizeof(gForYouItems[1].line2));
    SafeCopy(gForYouItems[1].imageUrl, "spotify:playlist:5MZ9NB8TxCE9MVeHM0NAl1", sizeof(gForYouItems[1].imageUrl));

    /* 3. Trending: https://open.spotify.com/playlist/3QSmfNR2XtpoADu0QPGVJK */
    SafeCopy(gForYouItems[2].slotId,   "3QSmfNR2XtpoADu0QPGVJK", sizeof(gForYouItems[2].slotId));
    SafeCopy(gForYouItems[2].title,    "Trending", sizeof(gForYouItems[2].title));
    SafeCopy(gForYouItems[2].line1,    "Find the future chart", sizeof(gForYouItems[2].line1));
    SafeCopy(gForYouItems[2].line2,    "smashers here.", sizeof(gForYouItems[2].line2));
    SafeCopy(gForYouItems[2].imageUrl, "spotify:playlist:3QSmfNR2XtpoADu0QPGVJK", sizeof(gForYouItems[2].imageUrl));

    /* 4. Hit songs: https://open.spotify.com/playlist/4ANVDtJVtVMVc2Nk79VU1M */
    SafeCopy(gForYouItems[3].slotId,   "4ANVDtJVtVMVc2Nk79VU1M", sizeof(gForYouItems[3].slotId));
    SafeCopy(gForYouItems[3].title,    "Hit songs", sizeof(gForYouItems[3].title));
    SafeCopy(gForYouItems[3].line1,    "The latest hit songs", sizeof(gForYouItems[3].line1));
    SafeCopy(gForYouItems[3].line2,    "right now.", sizeof(gForYouItems[3].line2));
    SafeCopy(gForYouItems[3].imageUrl, "spotify:playlist:4ANVDtJVtVMVc2Nk79VU1M", sizeof(gForYouItems[3].imageUrl));

    for (i = 0; i < 4; i++) {
        memcpy(gForYouIconBits[i], gForYouDefaultIconBits[i], 282);
    }
}

static void ConnectAndLoadAll(void)
{
    /* Only set the Spotify Connect speaker to the saved (last-used) speaker ONCE, directly at launch! */
    if (!gStartupSpeakerSent) {
        gStartupSpeakerSent = true;
        if (gSavedSpeakerId[0] != '\0' || gSavedSpeakerName[0] != '\0') {
            char encId[96], encName[96], encClientId[64], encClientName[96], path[512];
            if (gSavedSpeakerName[0] != '\0') {
                SafeCopy(gNowPlaying.deviceName, gSavedSpeakerName, sizeof(gNowPlaying.deviceName));
            }
            gSpeakerLockUntilTicks = TickCount() + 300; /* 5s initial startup lock */
            UrlEncode(gSavedSpeakerId, encId, sizeof(encId));
            UrlEncode(gSavedSpeakerName, encName, sizeof(encName));
            UrlEncode(gMyClientId, encClientId, sizeof(encClientId));
            UrlEncode(gMySpeakerName, encClientName, sizeof(encClientName));
            sprintf(
                path,
                "/api/spotify/transfer?device_id=%s&device_name=%s&startup=1&client_id=%s&client_name=%s",
                encId,
                encName,
                encClientId,
                encClientName
            );
            NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        }
    }

    /* Load server-side search history if local prefs didn't have any yet */
    if (gSearchHistoryCount == 0 && NetHasServer() && gHttpBuf) {
        if (NetHttpGet("/api/spotify/search-history", gHttpBuf, HTTP_BUF_SIZE) > 0) {
            const char *line = gHttpBuf;
            while (line && *line && gSearchHistoryCount < MAX_SEARCH_HISTORY) {
                const char *nextLine = strchr(line, '\n');
                char rowKind[16], qBuf[48];
                GetPipeField(line, 0, rowKind, sizeof(rowKind));
                if (strcmp(rowKind, "HISTORY") == 0) {
                    GetPipeField(line, 1, qBuf, sizeof(qBuf));
                    if (qBuf[0] != '\0') {
                        SafeCopy(gSearchHistory[gSearchHistoryCount++], qBuf, 48);
                    }
                }
                if (!nextLine) break;
                line = nextLine + 1;
            }
        }
    }

    InitForYouDefaults();
    RefreshNowPlaying(true);
    /* Show "Tune in" center panel upon launch ONLY when nothing is currently playing */
    gForYouViewOpen = (gNowPlaying.trackId[0] == '\0');
    LoadLeftColumn();
    gInitialDataLoaded = true;
}

static Boolean VerifyServerReachable(void)
{
    long n;
    if (!NetHasServer() || !gHttpBuf) return false;
    n = NetHttpGet("/api/ping", gHttpBuf, HTTP_BUF_SIZE);
    return (n > 0 && strncmp(gHttpBuf, "MACCOMPANION_SERVER|", 20) == 0);
}

static Boolean VerifySpotifyLoggedIn(void)
{
    long n;
    if (!NetHasServer() || !gHttpBuf) return false;
    n = NetHttpGet("/api/spotify/ping", gHttpBuf, HTTP_BUF_SIZE);
    return (n > 0 && strncmp(gHttpBuf, "SPOTIFY|1|", 10) == 0);
}

static void TransitionToServerFound(void)
{
    if (!VerifySpotifyLoggedIn()) {
        gConnState = CONN_LOGIN_ERROR;
        RenderCanvas();
        return;
    }
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
    if (pt.v >= 278 && pt.v <= 300) {
        if (pt.h >= 16 && pt.h <= 70) return BTN_VIZ_TOGGLE;
        if (pt.h >= 150 && pt.h <= 172) return BTN_LIKE;
        if (pt.h >= 191 && pt.h <= 213) return BTN_SHUFFLE;
        if (pt.h >= 216 && pt.h <= 238) return BTN_PREV;
        if (pt.h >= 242 && pt.h <= 270) return BTN_PLAY;
        if (pt.h >= 273 && pt.h <= 295) return BTN_NEXT;
        if (pt.h >= 297 && pt.h <= 319) return BTN_REPEAT;
        if (pt.h >= 342 && pt.h <= GetSpeakerButtonRightX()) return BTN_SPEAKER;
        if (pt.h >= 478 && pt.h <= 498) return BTN_COLLAPSE;
    }
    if (pt.v >= 300 && pt.v <= 318) {
        if (gVisualizerOpen && pt.h >= 18 && pt.h <= 69) {
            short idx = (pt.h - 19) / 7;
            if (idx < 0) idx = 0;
            if (idx >= VIZ_MODE_COUNT) idx = VIZ_MODE_COUNT - 1;
            return (short)(BTN_VIZ_1 + idx);
        }
        if (pt.h >= 140 && pt.h <= 372) return BTN_TIMELINE;
        if (pt.h >= 435 && pt.h <= 496) return BTN_VOLUME;
    }
    return BTN_NONE;
}

static short HitTestMiniPlayerHover(Point pt)
{
    if (pt.v >= 22 && pt.v <= 47) {
        if (pt.h >= 7 && pt.h <= 25) return BTN_LIKE;
        if (pt.h >= 39 && pt.h <= 59) return BTN_SHUFFLE;
        if (pt.h >= 62 && pt.h <= 79) return BTN_PREV;
        if (pt.h >= 82 && pt.h <= 110) return BTN_PLAY;
        if (pt.h >= 113 && pt.h <= 129) return BTN_NEXT;
        if (pt.h >= 131 && pt.h <= 151) return BTN_REPEAT;
        if (pt.h >= 164 && pt.h <= 184) return BTN_COLLAPSE;
    }
    if (pt.v >= 49 && pt.v <= 63 && pt.h >= 122 && pt.h <= 185) {
        return BTN_VOLUME;
    }
    return BTN_NONE;
}

static void UpdateHoverState(void)
{
    Point pt;
    short newBtn;
    short newSearchRow = -1;
    Boolean newVizHover = false;
    Boolean changed = false;

    if (gConnState != CONN_READY) return;

    if (gMiniPlayerMode) {
        if (!gMiniWindow) return;
        SetPort(gMiniWindow);
        GetMouse(&pt);
        newBtn = HitTestMiniPlayerHover(pt);
        if (newBtn != BTN_PLAY && gSuppressPlayHover) {
            gSuppressPlayHover = false;
            changed = true;
        }
        if (newBtn != gHoverBtn) {
            gHoverBtn = newBtn;
            changed = true;
        }
        if (changed) {
            RenderMiniPlayer();
        }
        return;
    }

    if (!gMainWindow) return;
    SetPort(gMainWindow);
    GetMouse(&pt);

    if (gVisualizerOpen && gVizFullscreen && !gSearchViewOpen && !gAboutModalOpen) {
        Point miniPt;
        miniPt.h = pt.h - MINI_FS_X;
        miniPt.v = pt.v - MINI_FS_Y;
        if (miniPt.h >= 0 && miniPt.h < MINI_W && miniPt.v >= 0 && miniPt.v < MINI_H) {
            newBtn = HitTestMiniPlayerHover(miniPt);
        } else {
            newBtn = BTN_NONE;
        }
        newVizHover = (pt.h >= 0 && pt.h < CANVAS_W && pt.v >= 0 && pt.v < CANVAS_H);
    } else {
        newBtn = HitTestTransportHover(pt);
        if (gVisualizerOpen && !gSearchViewOpen && !gAboutModalOpen) {
            newVizHover = (pt.h >= 156 && pt.h < 356 && pt.v >= 35 && pt.v < 235);
        }
    }

    if (newVizHover != gVizHover) {
        gVizHover = newVizHover;
        changed = true;
    }
    if (newBtn != BTN_PLAY && gSuppressPlayHover) {
        gSuppressPlayHover = false;
        changed = true;
    }
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
    } else if (gSearchHistoryOpen) {
        short newHistRow = -1;
        if (pt.h >= 158 && pt.h <= 354 && pt.v >= 40 && pt.v < (40 + MAX_SEARCH_HISTORY * 17)) {
            short r = (pt.v - 40) / 17;
            if (r >= 0 && r < gSearchHistoryCount) {
                newHistRow = r;
            }
        }
        if (newHistRow != gSearchHistoryHoverRow) {
            gSearchHistoryHoverRow = newHistRow;
            changed = true;
        }
    }

    if (changed) {
        RenderCanvas();
    }
}

/* Tracks a transport button press with visual down-state (`button-states-2.png` Row 3) */
static Boolean TrackTransportButtonPress(short btnId)
{
    Point pt;
    gPressedBtn = btnId;
    if (btnId == BTN_PLAY) {
        gSuppressPlayHover = true;
    }
    RenderCanvas();
    while (StillDown()) {
        if (AudioStreamIsRunning()) {
            AudioStreamPump();
        }
        GetMouse(&pt);
    }
    gPressedBtn = BTN_NONE;
    RenderCanvas();
    return true;
}

/*
 * Unified Play / Pause Toggle with Optimistic UI + Immediate Direct Timecode Sync:
 * - On Pause: immediately freezes the timeline, stops local audio, sends /pause,
 *   then directly polls /api/spotify/timeline?direct=1 and locks gPauseSynced so
 *   later background polls never jump the paused timeline.
 * - On Play: immediately unfreezes & re-anchors the timeline, sends /play, starts
 *   local audio, and directly polls /api/spotify/timeline?direct=1.
 */
static void PerformTogglePlayPause(Boolean useButtonTracking)
{
    Boolean wantPlay = !gNowPlaying.isPlaying;

    if (!wantPlay) {
        long curMs = GetCurrentInterpolatedProgressMs();
        gNowPlaying.isPlaying  = false;
        gTimelineFrozen        = true;
        gPauseSynced           = false;
        SetTimelinePosition(curMs);
    } else {
        gNowPlaying.isPlaying  = true;
        gTimelineFrozen        = false;
        gPauseSynced           = false;
        SetTimelinePosition(gNowPlaying.progressMs);
    }

    if (useButtonTracking) {
        TrackTransportButtonPress(BTN_PLAY);
    } else {
        RenderCanvas();
    }

    if (wantPlay) {
        NetHttpGet("/api/spotify/play", gHttpBuf, HTTP_BUF_SIZE);
        SetTimelinePosition(gNowPlaying.progressMs);
        SyncLocalAudioWithSpeaker();
        PollDirectTimeline();
    } else {
        SyncLocalAudioWithSpeaker();
        NetHttpGet("/api/spotify/pause", gHttpBuf, HTTP_BUF_SIZE);
        PollDirectTimeline();
        gPauseSynced = true;
    }

    ResetPollTimersAfterAction();
    RenderCanvas();
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

    /* 1. Pre-Connection Server Screens (Server1.png, Server2.png, Server3.png, login-error.png) */
    if (gConnState != CONN_READY) {
        if (gConnState == CONN_LOGIN_ERROR) {
            /* "Done! Retry!" button (204, 203)-(311, 229) in login-error.png */
            if (pt.h >= 200 && pt.h <= 315 && pt.v >= 199 && pt.v <= 233) {
                if (VerifyServerReachable()) {
                    if (VerifySpotifyLoggedIn()) {
                        SaveServerPrefs(NetGetServerIP());
                        gConnState         = CONN_READY;
                        gInitialDataLoaded = false;
                        RenderCanvas();
                        ConnectAndLoadAll();
                        RenderCanvas();
                    } else {
                        gConnState = CONN_LOGIN_ERROR;
                        RenderCanvas();
                    }
                } else {
                    gConnState = CONN_FAILED;
                    RenderCanvas();
                }
            }
            return;
        }

        if (gConnState == CONN_FOUND) {
            /* "Let's party!" button (204, 154)-(311, 180) skips remaining countdown */
            if (pt.h >= 200 && pt.h <= 315 && pt.v >= 150 && pt.v <= 184) {
                SaveServerPrefs(NetGetServerIP());
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

    /* 1B. Fullscreen Visualizer Mode (512x322 + Locked Mini-Player Overlay) */
    if (gVisualizerOpen && gVizFullscreen && !gSearchViewOpen && !gAboutModalOpen) {
        /* Top-right shrink icon (fullscreen-vis-down.png at 492, 8; 12x12) */
        if (pt.h >= (CANVAS_W - 8 - 12 - 6) && pt.h <= (CANVAS_W - 2) &&
            pt.v >= 2 && pt.v <= (8 + 12 + 6)) {
            gVizFullscreen = false;
            gHoverBtn      = BTN_NONE;
            VisualizerRenderFrame(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, gVisualizerBits);
            gLastVizTicks  = TickCount();
            RenderCanvas();
            return;
        }

        /* Locked Mini-Player Overlay (160, 239)-(352, 307) */
        if (pt.h >= MINI_FS_X && pt.h < (MINI_FS_X + MINI_W) &&
            pt.v >= MINI_FS_Y && pt.v < (MINI_FS_Y + MINI_H)) {
            Point miniPt;
            short btn;
            miniPt.h = pt.h - MINI_FS_X;
            miniPt.v = pt.v - MINI_FS_Y;
            btn = HitTestMiniPlayerHover(miniPt);

            if (btn == BTN_LIKE) {
                if (gNowPlaying.trackId[0] != '\0') {
                    gNowPlaying.isLiked = !gNowPlaying.isLiked;
                    TrackTransportButtonPress(BTN_LIKE);
                    NetHttpGet("/api/spotify/toggle-like", gHttpBuf, HTTP_BUF_SIZE);
                    ResetPollTimersAfterAction();
                }
                return;
            }
            if (btn == BTN_SHUFFLE) {
                char path[96];
                if (strcmp(gNowPlaying.shuffleMode, "off") == 0) {
                    SafeCopy(gNowPlaying.shuffleMode, "on", sizeof(gNowPlaying.shuffleMode));
                } else if (strcmp(gNowPlaying.shuffleMode, "on") == 0) {
                    SafeCopy(gNowPlaying.shuffleMode, "smart", sizeof(gNowPlaying.shuffleMode));
                } else {
                    SafeCopy(gNowPlaying.shuffleMode, "off", sizeof(gNowPlaying.shuffleMode));
                }
                TrackTransportButtonPress(BTN_SHUFFLE);
                sprintf(path, "/api/spotify/shuffle?mode=%s", gNowPlaying.shuffleMode);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                ResetPollTimersAfterAction();
                return;
            }
            if (btn == BTN_PREV) {
                gTimelineFrozen = true;
                SetTimelinePosition(0);
                TrackTransportButtonPress(BTN_PREV);
                TriggerPreviousTrack();
                return;
            }
            if (btn == BTN_PLAY) {
                PerformTogglePlayPause(true);
                return;
            }
            if (btn == BTN_NEXT) {
                gTimelineFrozen = true;
                SetTimelinePosition(0);
                TrackTransportButtonPress(BTN_NEXT);
                TriggerNextTrack();
                return;
            }
            if (btn == BTN_REPEAT) {
                char path[96];
                if (strcmp(gNowPlaying.repeatState, "off") == 0) {
                    SafeCopy(gNowPlaying.repeatState, "context", sizeof(gNowPlaying.repeatState));
                } else if (strcmp(gNowPlaying.repeatState, "context") == 0) {
                    SafeCopy(gNowPlaying.repeatState, "track", sizeof(gNowPlaying.repeatState));
                } else {
                    SafeCopy(gNowPlaying.repeatState, "off", sizeof(gNowPlaying.repeatState));
                }
                TrackTransportButtonPress(BTN_REPEAT);
                sprintf(path, "/api/spotify/repeat?state=%s", gNowPlaying.repeatState);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                ResetPollTimersAfterAction();
                return;
            }
            if (btn == BTN_COLLAPSE) {
                TrackTransportButtonPress(BTN_COLLAPSE);
                gVizFullscreen = false;
                gHoverBtn      = BTN_NONE;
                VisualizerRenderFrame(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, gVisualizerBits);
                gLastVizTicks  = TickCount();
                RenderCanvas();
                return;
            }
            if (btn == BTN_VOLUME) {
                char path[96];
                short vol = gNowPlaying.volumePct;
                Point curPt = pt;
                gPressedBtn = BTN_VOLUME;
                do {
                    vol = (short)(((long)(curPt.h - (MINI_FS_X + 126)) * 100L) / 55L);
                    if (vol < 0) vol = 0;
                    if (vol > 100) vol = 100;
                    if (vol != gNowPlaying.volumePct) {
                        gNowPlaying.volumePct = vol;
                        RenderCanvas();
                    }
                    if (AudioStreamIsRunning()) {
                        AudioStreamPump();
                    }
                    GetMouse(&curPt);
                } while (StillDown());
                gPressedBtn = BTN_NONE;
                RenderCanvas();

                sprintf(path, "/api/spotify/volume?volume_percent=%d", vol);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                ResetPollTimersAfterAction();
                return;
            }
            /* Position is locked in fullscreen mode: clicking empty space in Mini-Player does not drag */
            return;
        }

        /* Clicking anywhere else on the fullscreen visualizer cycles presets (1..7) */
        gVizMode = (gVizMode + 1) % VIZ_MODE_COUNT;
        SaveServerPrefs(NetGetServerIP());
        VisualizerRenderFrameFullscreen(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, NULL);
        RenderCanvas();
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
                char path[240];
                char encId[96], encName[96];
                SafeCopy(gNowPlaying.deviceName, gDevices[row].name, sizeof(gNowPlaying.deviceName));
                SafeCopy(gSavedSpeakerId,   gDevices[row].id,   sizeof(gSavedSpeakerId));
                SafeCopy(gSavedSpeakerName, gDevices[row].name, sizeof(gSavedSpeakerName));
                gSpeakerLockUntilTicks = TickCount() + 300; /* 5s speaker lock after explicit switch */
                SaveServerPrefs(NetGetServerIP());
                gSpeakerPopupOpen = false;
                RenderCanvas();

                UrlEncode(gDevices[row].id, encId, sizeof(encId));
                UrlEncode(gDevices[row].name, encName, sizeof(encName));
                sprintf(path, "/api/spotify/transfer?device_id=%s&device_name=%s", encId, encName);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                ResetPollTimersAfterAction();
                SyncLocalAudioWithSpeaker();
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
            gHomeMode          = !gHomeMode;
            gSearchFocused     = false;
            gSearchViewOpen    = false;
            gSearchHistoryOpen = false;
            LoadLeftColumn();
            RenderCanvas();
            return;
        }
        /* Search Pill (156..356) */
        if (pt.h >= 156 && pt.h <= 356) {
            /* Clear (X) icon on right side of search pill (340..356) */
            if (pt.h >= 340 && (gSearchViewOpen || gSearchQuery[0] != '\0')) {
                gSearchQuery[0]        = '\0';
                gSearchViewOpen        = false;
                gSearchFocused         = true;
                gSearchHistoryOpen     = true;
                gSearchHistoryHoverRow = -1;
                RenderCanvas();
                return;
            }
            /* Left smiley icon inside search pill (156..170) opens/toggles the "For You" view (ui/foryou.png) */
            if (pt.h <= 170) {
                gSearchFocused     = false;
                gSearchHistoryOpen = false;
                if (gSearchViewOpen || !gForYouViewOpen) {
                    gSearchViewOpen = false;
                    gForYouViewOpen = true;
                } else if (gNowPlaying.trackId[0] != '\0' || gVisualizerOpen) {
                    gForYouViewOpen = false;
                }
                RenderCanvas();
                return;
            }
            gSearchFocused = true;
            if (gSearchQuery[0] != '\0') {
                /* Clicking the search bar when a query is present immediately opens the search results window! */
                gSearchHistoryOpen = false;
                if (gSearchCount > 0) {
                    gSearchViewOpen = true;
                    RenderCanvas();
                } else {
                    ExecuteSearchQuery();
                }
            } else {
                /* Empty search bar: open the 5-item Search History dropdown (ui/search-history.png) */
                gSearchViewOpen        = false;
                gSearchHistoryOpen     = true;
                gSearchHistoryHoverRow = -1;
                RenderCanvas();
            }
            return;
        }
        gSearchFocused     = false;
        gSearchHistoryOpen = false;
        RenderCanvas();
        return;
    }

    gSearchFocused = false;

    /* 3B. Search History Dropdown Click (x = 156..356, y = 22..128) */
    if (gSearchHistoryOpen && !gSearchViewOpen) {
        if (pt.h >= 156 && pt.h <= 356 && pt.v >= 22 && pt.v <= 128) {
            if (pt.v >= 40 && pt.v < (40 + MAX_SEARCH_HISTORY * 17)) {
                short clickedHist = (pt.v - 40) / 17;
                if (clickedHist >= 0 && clickedHist < gSearchHistoryCount &&
                    gSearchHistory[clickedHist][0] != '\0') {
                    SafeCopy(gSearchQuery, gSearchHistory[clickedHist], sizeof(gSearchQuery));
                    gSearchHistoryOpen = false;
                    ExecuteSearchQuery();
                }
            }
            return;
        }
        gSearchHistoryOpen = false;
        RenderCanvas();
    }

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
                    if (AudioStreamIsRunning()) {
                        AudioStreamPump();
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
                if (AudioStreamIsRunning()) {
                    AudioStreamPump();
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

                if (isDblClick && gLeftItems[clickedRow].uri[0] != '\0') {
                    char encUri[128], path[256];
                    Boolean lockedFirstTrack = false;
                    if (gRightCount > 0 &&
                        strcmp(gRightItems[0].kind, "TRACK") == 0 &&
                        strcmp(gNowPlaying.shuffleMode, "off") == 0) {
                        gRightSelected = 0;
                        LockOptimisticTrackSelection(
                            gRightItems[0].id,
                            gRightItems[0].name,
                            gRightItems[0].artist,
                            gRightItems[0].durationMs
                        );
                        lockedFirstTrack = true;
                    } else {
                        gTimelineFrozen       = true;
                        gPauseSynced          = false;
                        gSeekLockUntilTicks   = 0;
                        gNowPlaying.isPlaying = true;
                        SetTimelinePosition(0);
                    }
                    RenderCanvas();

                    UrlEncode(gLeftItems[clickedRow].uri, encUri, sizeof(encUri));
                    sprintf(path, "/api/spotify/play?context_uri=%s", encUri);
                    NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                    SetTimelinePosition(0);
                    SyncLocalAudioWithSpeaker();

                    if (lockedFirstTrack) {
                        if (!gVisualizerOpen) {
                            FetchCurrentArtwork();
                            RenderCanvas();
                        }
                        FetchTrackLikedStatus(gNowPlaying.trackId);
                    } else {
                        RefreshNowPlaying(true);
                    }
                    ResetPollTimersAfterAction();
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
                if (AudioStreamIsRunning()) {
                    AudioStreamPump();
                }
                GetMouse(&curPt);
            } while (StillDown());
            return;
        }
        /* Right Column Items (x = 362..498, y = 35..243) */
        if (pt.h >= 362 && pt.h <= 498 && pt.v >= LIST_TOP_Y && pt.v < LIST_BOTTOM_Y) {
            short clickedRow = gRightScroll + (pt.v - LIST_TOP_Y) / ROW_HEIGHT;
            if (clickedRow >= 0 && clickedRow < gRightCount) {
                unsigned long now = TickCount();
                if (strcmp(gRightItems[clickedRow].kind, "QUEUE_SEP") == 0) {
                    return;
                }
                /* Gracefully ignore the second click of a double-click on the same track row
                 * (gLastClickTicks is stamped AFTER PlayRightItemAtIndex finishes its HTTP calls,
                 * so a second click queued during network I/O or within ~600ms after is ignored). */
                if (clickedRow == gLastClickedRow &&
                    gLastClickTicks != 0 &&
                    (now - gLastClickTicks) <= 36 &&
                    strcmp(gRightItems[clickedRow].kind, "TRACK") == 0) {
                    return;
                }
                gLastClickedRow = clickedRow;
                gLastClickTicks = now;
                PlayRightItemAtIndex(clickedRow);
                gLastClickTicks = TickCount();
            }
            return;
        }
        /* Center Panel: "Tune in" View (ui/tune-in.png) */
        if (gForYouViewOpen || (!gVisualizerOpen && gNowPlaying.trackId[0] == '\0')) {
            /* Top-right Close Circle (X) at (337, 38) 12x12 (only active when a track is playing or visualizer is open) */
            if ((gNowPlaying.trackId[0] != '\0' || gVisualizerOpen) &&
                pt.h >= 333 && pt.h <= 353 && pt.v >= 34 && pt.v <= 54) {
                gForYouViewOpen = false;
                RenderCanvas();
                return;
            }
            /* Clicking one of the 4 "Tune in" playlists (x = 156..350, y = 56..265) */
            if (pt.h >= 156 && pt.h <= 350 && pt.v >= 56 && pt.v <= 265) {
                short slot = (pt.v < 106) ? 0 : (pt.v < 160) ? 1 : (pt.v < 214) ? 2 : 3;
                ForYouItem *fy = &gForYouItems[slot];
                PinActiveContextAtTopOfLeftColumn("PLAYLIST", fy->slotId, fy->title, fy->imageUrl);
                gLeftSelected = 0;
                gLeftScroll   = 0;
                RenderCanvas();
                LoadSelectedLeftItemIntoRightList();
                RenderCanvas();
                if (gRightCount > 0 && strcmp(gRightItems[0].kind, "TRACK") == 0) {
                    PlayRightItemAtIndex(0);
                }
                return;
            }
            return;
        }
        /* Clicking inside the 200x200 Visualizer box: top-right icon enters fullscreen, else cycles preset (1..7) */
        if (gVisualizerOpen && !gForYouViewOpen && pt.h >= 156 && pt.h < 356 && pt.v >= 35 && pt.v < 235) {
            /* Top-right fullscreen icon at (336, 43) 12x12 (8px down from 35, 8px in from 356) */
            if (pt.h >= (356 - 8 - 12 - 4) && pt.h <= 354 &&
                pt.v >= (35 + 8 - 4) && pt.v <= (35 + 8 + 12 + 4)) {
                gVizFullscreen = true;
                gHoverBtn      = BTN_NONE;
                gLastVizTicks  = TickCount();
                VisualizerRenderFrameFullscreen(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, NULL);
                RenderCanvas();
                return;
            }
            gVizMode = (gVizMode + 1) % VIZ_MODE_COUNT;
            SaveServerPrefs(NetGetServerIP());
            VisualizerRenderFrame(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, gVisualizerBits);
            RenderCanvas();
            return;
        }
        return;
    }

    /* 5. Bottom Play Bar (y = 273..322) */
    if (pt.v >= 273) {
        /* Transport Row (y = 276..300) */
        if (pt.v >= 276 && pt.v <= 300) {
            /* Album Art / Visualizations Toggle (16..70) */
            if (pt.h >= 16 && pt.h <= 70) {
                gForYouViewOpen = false;
                if (pt.h <= 30 && gVisualizerOpen) {
                    gVisualizerOpen = false;
                } else if (pt.h >= 54 && !gVisualizerOpen) {
                    gVisualizerOpen = true;
                } else {
                    gVisualizerOpen = !gVisualizerOpen;
                }
                if (gVisualizerOpen) {
                    VisualizerRenderFrame(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, gVisualizerBits);
                    gLastVizTicks = TickCount();
                } else if (gNowPlaying.trackId[0] != '\0' && strcmp(gNowPlaying.trackId, gLoadedArtTrackId) != 0) {
                    FetchCurrentArtwork();
                }
                TrackTransportButtonPress(BTN_VIZ_TOGGLE);
                return;
            }
            /* Like / Save Heart (150..172) */
            if (pt.h >= 150 && pt.h <= 172) {
                if (gNowPlaying.trackId[0] != '\0') {
                    gNowPlaying.isLiked = !gNowPlaying.isLiked;
                    TrackTransportButtonPress(BTN_LIKE);
                    NetHttpGet("/api/spotify/toggle-like", gHttpBuf, HTTP_BUF_SIZE);
                    ResetPollTimersAfterAction();
                    if (gLeftSelected >= 0 && gLeftSelected < gLeftCount &&
                        strcmp(gLeftItems[gLeftSelected].kind, "LIKED") == 0) {
                        LoadSelectedLeftItemIntoRightList();
                        RenderCanvas();
                    }
                }
                return;
            }
            /* Shuffle (191..213) */
            if (pt.h >= 191 && pt.h <= 213) {
                char path[96];
                /* Update state BEFORE TrackTransportButtonPress so it locks immediately */
                if (strcmp(gNowPlaying.shuffleMode, "off") == 0) {
                    SafeCopy(gNowPlaying.shuffleMode, "on", sizeof(gNowPlaying.shuffleMode));
                } else if (strcmp(gNowPlaying.shuffleMode, "on") == 0) {
                    SafeCopy(gNowPlaying.shuffleMode, "smart", sizeof(gNowPlaying.shuffleMode));
                } else {
                    SafeCopy(gNowPlaying.shuffleMode, "off", sizeof(gNowPlaying.shuffleMode));
                }
                TrackTransportButtonPress(BTN_SHUFFLE);

                sprintf(path, "/api/spotify/shuffle?mode=%s", gNowPlaying.shuffleMode);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                ResetPollTimersAfterAction();
                return;
            }
            /* Previous (216..238) */
            if (pt.h >= 216 && pt.h <= 238) {
                gTimelineFrozen = true;
                SetTimelinePosition(0);
                TrackTransportButtonPress(BTN_PREV);
                TriggerPreviousTrack();
                return;
            }
            /* Play / Pause Circle (242..270) */
            if (pt.h >= 242 && pt.h <= 270) {
                PerformTogglePlayPause(true);
                return;
            }
            /* Next (273..295) */
            if (pt.h >= 273 && pt.h <= 295) {
                gTimelineFrozen = true;
                SetTimelinePosition(0);
                TrackTransportButtonPress(BTN_NEXT);
                TriggerNextTrack();
                return;
            }
            /* Repeat (297..319) */
            if (pt.h >= 297 && pt.h <= 319) {
                char path[96];
                if (strcmp(gNowPlaying.repeatState, "off") == 0) {
                    SafeCopy(gNowPlaying.repeatState, "context", sizeof(gNowPlaying.repeatState));
                } else if (strcmp(gNowPlaying.repeatState, "context") == 0) {
                    SafeCopy(gNowPlaying.repeatState, "track", sizeof(gNowPlaying.repeatState));
                } else {
                    SafeCopy(gNowPlaying.repeatState, "off", sizeof(gNowPlaying.repeatState));
                }
                TrackTransportButtonPress(BTN_REPEAT);

                sprintf(path, "/api/spotify/repeat?state=%s", gNowPlaying.repeatState);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                ResetPollTimersAfterAction();
                return;
            }
            /* Speaker Selector (342..rightX) — mouse down immediately opens/toggles the select menu */
            if (pt.h >= 342 && pt.h <= GetSpeakerButtonRightX()) {
                LoadDevicesAndTogglePopover();
                return;
            }
            /* Collapse to Mini-Player (478..498) */
            if (pt.h >= 478 && pt.h <= 498) {
                TrackTransportButtonPress(BTN_COLLAPSE);
                EnterMiniPlayerMode();
                return;
            }
        }

        /* Visualizer 1..7 Selector + Draggable Timeline & Volume Row (y = 300..320) */
        if (pt.v >= 300 && pt.v <= 320) {
            /* Visualizer Preset Selector 1 2 3 4 5 6 7 (18..69) */
            if (gVisualizerOpen && pt.h >= 18 && pt.h <= 69) {
                short clickedMode = (pt.h - 19) / 7;
                if (clickedMode < 0) clickedMode = 0;
                if (clickedMode >= VIZ_MODE_COUNT) clickedMode = VIZ_MODE_COUNT - 1;
                gVizMode = clickedMode;
                SaveServerPrefs(NetGetServerIP());

                VisualizerRenderFrame(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, gVisualizerBits);
                gLastVizTicks = TickCount();
                RenderCanvas();
                return;
            }
            /* Timeline Scrubber (140..372) */
            if (pt.h >= 140 && pt.h <= 372) {
                char path[96];
                short pct = 0;
                long targetMs;
                Point curPt = pt;
                gPressedBtn     = BTN_TIMELINE;
                gTimelineFrozen = true;
                do {
                    long newProg = gNowPlaying.progressMs;
                    pct = (short)(((long)(curPt.h - 143) * 100L) / 226L);
                    if (pct < 0) pct = 0;
                    if (pct > 100) pct = 100;
                    if (gNowPlaying.durationMs > 0) {
                        newProg = (gNowPlaying.durationMs * pct) / 100L;
                    }
                    if (newProg != gNowPlaying.progressMs) {
                        gNowPlaying.progressMs = newProg;
                        FormatMmSs(gNowPlaying.progressMs, gNowPlaying.progressMmSs);
                        RenderCanvas();
                    }
                    if (AudioStreamIsRunning()) {
                        AudioStreamPump();
                    }
                    GetMouse(&curPt);
                } while (StillDown());
                gPressedBtn = BTN_NONE;
                targetMs    = gNowPlaying.progressMs;

                /*
                 * 1. Keep timeline frozen on the clicked/dragged target position and lock
                 *    15s seek authority so background polls never bounce back to pre-seek time.
                 */
                SetTimelinePosition(targetMs);
                gSeekLockUntilTicks = TickCount() + 900; /* 15-second client seek authority */
                RenderCanvas();

                /* 2. Request audio seek directly */
                sprintf(path, "/api/spotify/seek?position_ms=%ld", targetMs);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);

                /* 3. Re-anchor timeline right when seek finishes & directly poll timecode */
                SetTimelinePosition(targetMs);
                gTimelineFrozen = !gNowPlaying.isPlaying;
                PollDirectTimeline();
                ResetPollTimersAfterAction();
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
                    if (AudioStreamIsRunning()) {
                        AudioStreamPump();
                    }
                    GetMouse(&curPt);
                } while (StillDown());
                gPressedBtn = BTN_NONE;
                RenderCanvas();

                sprintf(path, "/api/spotify/volume?volume_percent=%d", vol);
                NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
                ResetPollTimersAfterAction();
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
        if (gConnState == CONN_LOGIN_ERROR) {
            if (ch == '\r' || ch == 3 || ch == ' ') {
                if (VerifyServerReachable()) {
                    if (VerifySpotifyLoggedIn()) {
                        SaveServerPrefs(NetGetServerIP());
                        gConnState         = CONN_READY;
                        gInitialDataLoaded = false;
                        RenderCanvas();
                        ConnectAndLoadAll();
                        RenderCanvas();
                    } else {
                        gConnState = CONN_LOGIN_ERROR;
                        RenderCanvas();
                    }
                } else {
                    gConnState = CONN_FAILED;
                    RenderCanvas();
                }
            }
            return;
        }
        if (gConnState == CONN_FOUND) {
            if (ch == '\r' || ch == 3 || ch == ' ') {
                SaveServerPrefs(NetGetServerIP());
                if (!gInitialDataLoaded) {
                    ConnectAndLoadAll();
                }
                gConnState = CONN_READY;
                RenderCanvas();
            }
            return;
        }
        if (ch == '\r' || ch == 3) {
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
        } else if (ch == 8 && len > 0) {
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
        } else if (ch == 27) {
            gSearchFocused     = false;
            gSearchViewOpen    = false;
            gSearchHistoryOpen = false;
            RenderCanvas();
        } else if (ch == 8 && len > 0) {
            gSearchQuery[len - 1] = '\0';
            if (gSearchQuery[0] == '\0') {
                gSearchViewOpen    = false;
                gSearchHistoryOpen = true;
            }
            RenderCanvas();
        } else if (ch >= 32 && ch < 127 && len < (short)(sizeof(gSearchQuery) - 2)) {
            gSearchQuery[len]     = ch;
            gSearchQuery[len + 1] = '\0';
            gSearchHistoryOpen    = false;
            RenderCanvas();
        }
        return;
    }

    /* 3. Global Keyboard Shortcuts */
    if (ch == 27 && (gSearchViewOpen || gSearchHistoryOpen)) {
        gSearchViewOpen    = false;
        gSearchHistoryOpen = false;
        RenderCanvas();
        return;
    }
    if (ch == 27 && gVisualizerOpen && gVizFullscreen) {
        gVizFullscreen = false;
        gHoverBtn      = BTN_NONE;
        VisualizerRenderFrame(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, gVisualizerBits);
        gLastVizTicks  = TickCount();
        RenderCanvas();
        return;
    }
    if (ch == 27 && gForYouViewOpen && (gNowPlaying.trackId[0] != '\0' || gVisualizerOpen)) {
        gForYouViewOpen = false;
        RenderCanvas();
        return;
    }

    if (ch == ' ') {
        PerformTogglePlayPause(false);
    } else if (ch == '\r' || ch == 3) {
        if (gSearchViewOpen && gSearchHoverRow >= 0 && gSearchHoverRow < gSearchCount) {
            ActivateSearchResultRow(gSearchHoverRow);
        } else if (gRightSelected >= 0 && gRightSelected < gRightCount) {
            PlayRightItemAtIndex(gRightSelected);
        }
    } else if (ch == 30 || ch == 31) {
        /* Up / Down Arrow (and emulator mouse-wheel scroll):
         * ONLY scroll the visible list — NEVER trigger track changes or playback! */
        short delta = (ch == 30) ? -1 : 1;
        Point mousePt;
        if (gMainWindow) {
            SetPort(gMainWindow);
            GetMouse(&mousePt);
        } else {
            mousePt.h = 400;
            mousePt.v = 100;
        }
        if (gSearchViewOpen) {
            short maxScroll = gSearchCount - SEARCH_VISIBLE_ROWS;
            if (maxScroll > 0) {
                short newScroll = gSearchScroll + delta;
                if (newScroll < 0) newScroll = 0;
                if (newScroll > maxScroll) newScroll = maxScroll;
                if (newScroll != gSearchScroll) {
                    gSearchScroll = newScroll;
                    RenderCanvas();
                }
            }
        } else if (mousePt.h < 156) {
            short maxScroll = gLeftCount - VISIBLE_ROWS;
            if (maxScroll > 0) {
                short newScroll = gLeftScroll + delta;
                if (newScroll < 0) newScroll = 0;
                if (newScroll > maxScroll) newScroll = maxScroll;
                if (newScroll != gLeftScroll) {
                    gLeftScroll = newScroll;
                    RenderCanvas();
                }
            }
        } else {
            short maxScroll = gRightCount - VISIBLE_ROWS;
            if (maxScroll > 0) {
                short newScroll = gRightScroll + delta;
                if (newScroll < 0) newScroll = 0;
                if (newScroll > maxScroll) newScroll = maxScroll;
                if (newScroll != gRightScroll) {
                    gRightScroll = newScroll;
                    RenderCanvas();
                }
            }
        }
    } else if (ch >= '1' && ch <= '7') {
        gForYouViewOpen = false;
        gVisualizerOpen = true;
        gVizMode = (short)(ch - '1');
        SaveServerPrefs(NetGetServerIP());
        if (gVizFullscreen) {
            VisualizerRenderFrameFullscreen(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, NULL);
        } else {
            VisualizerRenderFrame(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, gVisualizerBits);
        }
        gLastVizTicks = TickCount();
        RenderCanvas();
    } else if (ch == 'f' || ch == 'F') {
        if (gVisualizerOpen) {
            gVizFullscreen = !gVizFullscreen;
            gHoverBtn      = BTN_NONE;
            if (gVizFullscreen) {
                VisualizerRenderFrameFullscreen(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, NULL);
            } else {
                VisualizerRenderFrame(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, gVisualizerBits);
            }
            gLastVizTicks = TickCount();
            RenderCanvas();
        }
    } else if (ch == 'v' || ch == 'V') {
        gForYouViewOpen = false;
        gVisualizerOpen = !gVisualizerOpen;
        if (!gVisualizerOpen) {
            gVizFullscreen = false;
        }
        if (gVisualizerOpen) {
            VisualizerRenderFrame(gVizMode, gNowPlaying.isPlaying, gNowPlaying.progressMs, gNowPlaying.trackId, gVisualizerBits);
            gLastVizTicks = TickCount();
        } else if (gNowPlaying.trackId[0] != '\0' && strcmp(gNowPlaying.trackId, gLoadedArtTrackId) != 0) {
            FetchCurrentArtwork();
        }
        RenderCanvas();
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
                if (gMiniPlayerMode) ExitMiniPlayerMode();
                gAboutModalOpen = true;
                RenderCanvas();
            } else {
                GetMenuItemText(GetMenuHandle(mApple), itemID, daName);
                OpenDeskAcc(daName);
            }
            break;

        case mFile:
            if (itemID == iServerIP) {
                if (gMiniPlayerMode) ExitMiniPlayerMode();
                if (NetHasServer()) {
                    FormatIPAddress(NetGetServerIP(), gIpInput);
                }
                gConnState = CONN_FAILED;
                RenderCanvas();
            } else if (itemID == iFindSearch) {
                if (gConnState == CONN_READY) {
                    if (gMiniPlayerMode) ExitMiniPlayerMode();
                    gSearchFocused = true;
                    if (gSearchQuery[0] != '\0') {
                        gSearchHistoryOpen = false;
                        if (gSearchCount > 0) {
                            gSearchViewOpen = true;
                        } else {
                            ExecuteSearchQuery();
                        }
                    } else {
                        gSearchViewOpen    = false;
                        gSearchHistoryOpen = true;
                    }
                    RenderCanvas();
                }
            } else if (itemID == iQuit) {
                SaveServerPrefs(NetGetServerIP());
                gRunning = false;
            }
            break;

        case mPlayback:
            if (gConnState != CONN_READY) break;
            if (itemID == iPlayPause) {
                PerformTogglePlayPause(false);
            } else if (itemID == iNextTrack) {
                TriggerNextTrack();
            } else if (itemID == iPrevTrack) {
                TriggerPreviousTrack();
            } else if (itemID == iLikeTrack) {
                TriggerToggleLike();
            } else if (itemID == iChooseSpeaker) {
                if (gMiniPlayerMode) ExitMiniPlayerMode();
                LoadDevicesAndTogglePopover();
            }
            break;
    }
    HiliteMenu(0);
}

static void HandleMiniPlayerClick(EventRecord *ev, Point pt)
{
    short btn = HitTestMiniPlayerHover(pt);

    if (btn == BTN_LIKE) {
        if (gNowPlaying.trackId[0] != '\0') {
            gNowPlaying.isLiked = !gNowPlaying.isLiked;
            TrackTransportButtonPress(BTN_LIKE);
            NetHttpGet("/api/spotify/toggle-like", gHttpBuf, HTTP_BUF_SIZE);
            ResetPollTimersAfterAction();
        }
        return;
    }
    if (btn == BTN_SHUFFLE) {
        char path[96];
        if (strcmp(gNowPlaying.shuffleMode, "off") == 0) {
            SafeCopy(gNowPlaying.shuffleMode, "on", sizeof(gNowPlaying.shuffleMode));
        } else if (strcmp(gNowPlaying.shuffleMode, "on") == 0) {
            SafeCopy(gNowPlaying.shuffleMode, "smart", sizeof(gNowPlaying.shuffleMode));
        } else {
            SafeCopy(gNowPlaying.shuffleMode, "off", sizeof(gNowPlaying.shuffleMode));
        }
        TrackTransportButtonPress(BTN_SHUFFLE);
        sprintf(path, "/api/spotify/shuffle?mode=%s", gNowPlaying.shuffleMode);
        NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        ResetPollTimersAfterAction();
        return;
    }
    if (btn == BTN_PREV) {
        gTimelineFrozen = true;
        SetTimelinePosition(0);
        TrackTransportButtonPress(BTN_PREV);
        TriggerPreviousTrack();
        return;
    }
    if (btn == BTN_PLAY) {
        PerformTogglePlayPause(true);
        return;
    }
    if (btn == BTN_NEXT) {
        gTimelineFrozen = true;
        SetTimelinePosition(0);
        TrackTransportButtonPress(BTN_NEXT);
        TriggerNextTrack();
        return;
    }
    if (btn == BTN_REPEAT) {
        char path[96];
        if (strcmp(gNowPlaying.repeatState, "off") == 0) {
            SafeCopy(gNowPlaying.repeatState, "context", sizeof(gNowPlaying.repeatState));
        } else if (strcmp(gNowPlaying.repeatState, "context") == 0) {
            SafeCopy(gNowPlaying.repeatState, "track", sizeof(gNowPlaying.repeatState));
        } else {
            SafeCopy(gNowPlaying.repeatState, "off", sizeof(gNowPlaying.repeatState));
        }
        TrackTransportButtonPress(BTN_REPEAT);
        sprintf(path, "/api/spotify/repeat?state=%s", gNowPlaying.repeatState);
        NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        ResetPollTimersAfterAction();
        return;
    }
    if (btn == BTN_COLLAPSE) {
        TrackTransportButtonPress(BTN_COLLAPSE);
        ExitMiniPlayerMode();
        return;
    }
    if (btn == BTN_VOLUME) {
        char path[96];
        short vol = gNowPlaying.volumePct;
        Point curPt = pt;
        gPressedBtn = BTN_VOLUME;
        do {
            vol = (short)(((long)(curPt.h - 126) * 100L) / 55L);
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            if (vol != gNowPlaying.volumePct) {
                gNowPlaying.volumePct = vol;
                RenderMiniPlayer();
            }
            if (AudioStreamIsRunning()) {
                AudioStreamPump();
            }
            GetMouse(&curPt);
        } while (StillDown());
        gPressedBtn = BTN_NONE;
        RenderMiniPlayer();

        sprintf(path, "/api/spotify/volume?volume_percent=%d", vol);
        NetHttpGet(path, gHttpBuf, HTTP_BUF_SIZE);
        ResetPollTimersAfterAction();
        return;
    }

    /* Clicking anywhere else on the Mini-Player drags the window freely across the screen */
    if (gMiniWindow && ev) {
        DragWindowWithAudio(gMiniWindow, ev->where);
    }
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
                DragWindowWithAudio(whichWindow, ev->where);
            }
            break;

        case inContent:
            if (whichWindow != FrontWindow()) {
                SelectWindow(whichWindow);
            } else if (whichWindow == gMiniWindow && gMiniPlayerMode) {
                Point pt = ev->where;
                SetPort(gMiniWindow);
                GlobalToLocal(&pt);
                HandleMiniPlayerClick(ev, pt);
            } else if (whichWindow == gMainWindow) {
                Point pt = ev->where;
                SetPort(gMainWindow);
                GlobalToLocal(&pt);
                if ((ev->modifiers & cmdKey) != 0 && pt.v < 22) {
                    DragWindowWithAudio(gMainWindow, ev->where);
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
    InitMyMacSpeakerName();
    AudioStreamInit();
    VisualizerInit();
    memset(gVisualizerBits, 0xFF, sizeof(gVisualizerBits));
    NetSetIdleCallback(VisualizerNetworkIdlePump);

    gHttpBuf = NewPtrClear(HTTP_BUF_SIZE);
    memset(gCurrentArtworkBits, 0xFF, sizeof(gCurrentArtworkBits));
    InitForYouDefaults();
    memset(&gNowPlaying, 0, sizeof(gNowPlaying));
    SafeCopy(gNowPlaying.progressMmSs, "00:00", sizeof(gNowPlaying.progressMmSs));
    SafeCopy(gNowPlaying.durationMmSs, "00:00", sizeof(gNowPlaying.durationMmSs));
    SafeCopy(gNowPlaying.shuffleMode,  "off",   sizeof(gNowPlaying.shuffleMode));
    SafeCopy(gNowPlaying.repeatState,  "off",   sizeof(gNowPlaying.repeatState));
    gNowPlaying.volumePct = 70;
    gTimelineAnchorTicks  = TickCount();

    InitOffscreenCanvas();

    /*
     * Window Framing & Positioning:
     * - On 512x342 Compact Macs (Plus / SE / Classic): Flush borderless window (`plainDBox`) at (0, 20)
     * - On higher-resolution screens (> 512x342): Standard movable Mac OS window (`noGrowDocProc`)
     */
    screenW = qd.screenBits.bounds.right - qd.screenBits.bounds.left;
    screenH = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
    {
        short winProc;
        if (screenW <= 512 && screenH <= 342) {
            winLeft = 0;
            winTop  = 20;
            winProc = plainDBox;
        } else {
            winLeft = (screenW - CANVAS_W) / 2;
            winTop  = 40 + ((screenH - 40 - CANVAS_H) / 2);
            if (winTop < 40) winTop = 40;
            winProc = noGrowDocProc;
        }

        SetRect(&winBounds, winLeft, winTop, winLeft + CANVAS_W, winTop + CANVAS_H);
        gMainWindow = NewWindow(
            NULL,
            &winBounds,
            "\pSpotiMac",
            true,
            winProc,
            (WindowPtr)-1L,
            false,
            0
        );

        /* Create the 192x68 frameless Mini-Player window (hidden until Collapse is clicked) */
        SetRect(&winBounds, winLeft + 160, winTop + 119, winLeft + 160 + MINI_W, winTop + 119 + MINI_H);
        gMiniWindow = NewWindow(
            NULL,
            &winBounds,
            "\pSpotiMac Mini",
            false,
            plainDBox,
            (WindowPtr)-1L,
            false,
            0
        );
        InstallMiniPlayerWDEF(gMiniWindow);
    }

    /*
     * Startup Server Connection Flow:
     * If the user has already seen the server screen at least once and we have a saved
     * Server IP in "System Folder:Preferences:MaciSpot Prefs", immediately open the
     * loading screen (`loading.png`) on that IP and verify/load in the background.
     * If the server is reachable but Spotify is not logged in, display `login-error.png`.
     */
    {
        ip_addr savedIp = 0;
        if (LoadSavedServerPrefs(&savedIp)) {
            FormatIPAddress(savedIp, gIpInput);
            NetSetServer(savedIp, DEFAULT_HTTP_PORT);
            gConnState         = CONN_READY;
            gInitialDataLoaded = false;
            RenderCanvas();

            if (VerifyServerReachable()) {
                if (VerifySpotifyLoggedIn()) {
                    ConnectAndLoadAll();
                    RenderCanvas();
                } else {
                    gConnState = CONN_LOGIN_ERROR;
                    RenderCanvas();
                }
            } else {
                RunInitialDiscovery();
            }
        } else {
            RunInitialDiscovery();
        }
    }

    gLastPollTicks = TickCount();

    while (gRunning) {
        unsigned long nowTicks;

        SystemTask();

        /* Pump local 68k Sound Manager TCP stream (:6802) every iteration */
        if (AudioStreamIsRunning()) {
            AudioStreamPump();
        }

        if (WaitNextEvent(
                everyEvent,
                &ev,
                (AudioStreamIsRunning() || (gVisualizerOpen && !gMiniPlayerMode && !gSearchViewOpen)) ? 0 : 2,
                NULL
            )) {
            switch (ev.what) {
                case mouseDown:
                    HandleMouseDown(&ev);
                    break;

                case keyDown:
                case autoKey:
                    if ((ev.modifiers & cmdKey) != 0) {
                        if (ev.what == keyDown) {
                            HandleMenuChoice(MenuKey((char)(ev.message & charCodeMask)));
                        }
                    } else {
                        char ch = (char)(ev.message & charCodeMask);
                        /* Only allow autoKey repeat for Backspace (8) and Scroll/Arrow keys (30, 31) */
                        if (ev.what == keyDown || ch == 8 || ch == 30 || ch == 31 || gSearchFocused || gConnState != CONN_READY) {
                            HandleKeyPress(ch);
                        }
                    }
                    break;

                case updateEvt:
                    if ((WindowPtr)ev.message == gMainWindow) {
                        BeginUpdate(gMainWindow);
                        RenderCanvas();
                        EndUpdate(gMainWindow);
                    } else if ((WindowPtr)ev.message == gMiniWindow) {
                        BeginUpdate(gMiniWindow);
                        RenderMiniPlayer();
                        EndUpdate(gMiniWindow);
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
                SaveServerPrefs(NetGetServerIP());
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

        /* Real-Time 30 FPS Visualizer Pump (every 2 ticks = 30.075 FPS on 60.15 Hz Mac VBL) */
        if (gVisualizerOpen && !gMiniPlayerMode && !gSearchViewOpen && !gAboutModalOpen &&
            (nowTicks - gLastVizTicks) >= 2) {
            gLastVizTicks = nowTicks;
            RenderVisualizerFastFrame();
        }

        /* Tick-anchored local timeline progression (drift-free 60.15 Hz hardware tick clock) */
        if (gNowPlaying.isPlaying && !gTimelineFrozen) {
            long curMs = GetCurrentInterpolatedProgressMs();
            if ((curMs / 1000L) != (gNowPlaying.progressMs / 1000L)) {
                gNowPlaying.progressMs = curMs;
                FormatMmSs(gNowPlaying.progressMs, gNowPlaying.progressMmSs);
                if (!gSpeakerPopupOpen && !gAboutModalOpen) {
                    RenderCanvas();
                }
                /* If a playing track reaches its end, trigger an immediate poll so the next track loads fast */
                if (gNowPlaying.durationMs > 0 && curMs >= gNowPlaying.durationMs &&
                    (nowTicks - gLastPollTicks) >= 30) {
                    gLastPollTicks = nowTicks - 60;
                }
            }
        }

        /* Poll /api/spotify/poll every ~1 second (60 ticks) */
        if (NetHasServer() && !gAboutModalOpen && (nowTicks - gLastPollTicks) >= 60) {
            NowPlayingState prevState = gNowPlaying;
            gLastPollTicks = nowTicks;
            RefreshNowPlaying(true);
            if (prevState.isPlaying != gNowPlaying.isPlaying ||
                prevState.isLiked != gNowPlaying.isLiked ||
                prevState.volumePct != gNowPlaying.volumePct ||
                strcmp(prevState.trackId, gNowPlaying.trackId) != 0 ||
                strcmp(prevState.trackName, gNowPlaying.trackName) != 0 ||
                strcmp(prevState.artistName, gNowPlaying.artistName) != 0 ||
                strcmp(prevState.progressMmSs, gNowPlaying.progressMmSs) != 0 ||
                strcmp(prevState.durationMmSs, gNowPlaying.durationMmSs) != 0 ||
                strcmp(prevState.shuffleMode, gNowPlaying.shuffleMode) != 0 ||
                strcmp(prevState.repeatState, gNowPlaying.repeatState) != 0 ||
                strcmp(prevState.deviceName, gNowPlaying.deviceName) != 0) {
                RenderCanvas();
            }
        }
    }

    AudioStreamStop();
    if (gHttpBuf) DisposePtr(gHttpBuf);
    if (gOffscreenBits) DisposePtr(gOffscreenBits);
    return 0;
}
