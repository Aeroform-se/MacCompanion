#include "net_internal.h"
#include <Files.h>
#include <Folders.h>
#include <Gestalt.h>
#include <Memory.h>
#include <OSUtils.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PREFS_FILENAME       "\pNetFlim Prefs"
#define TCP_RECV_BUF_SIZE    4096L
#define ORBSTACK_BRIDGE_IP   0xC0A86102UL /* 192.168.97.2 */

#define NETFLIM_PREFS_MAGIC  0x4D434E31UL /* 'MCN1' */
#define SPOTIMAC_PREFS_MAGIC 0x4D435331UL /* 'MCS1' */
#define MACAI_PREFS_MAGIC    0x4D434131UL /* 'MCA1' */

typedef struct {
    unsigned long  magic;
    ip_addr        serverIp;
    unsigned short serverPort;
    short          profileMode;
} NetFlimPrefsRecord;

typedef struct {
    unsigned long  magic;
    ip_addr        serverIp;
} SiblingMagicPrefsHeader;

/* Exact layout matching macdrop/src/file_io.c MacDropPrefsRecord */
typedef struct {
    char           folderName[64];
    Boolean        soundEnabled;
    Boolean        notifyEnabled;
    short          prefVersion;
    char           clientID[64];
    char           clientName[64];
    short          destVRefNum;
    long           destDirID;
    unsigned long  serverIP;
    unsigned short serverPort;
} SiblingMacDropPrefsRecord;

/* ============================================================================
 * Endian & Unaligned Helpers (68k Big-Endian Safe)
 * ============================================================================ */

static unsigned short read_u16be(const unsigned char *p) {
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static unsigned long read_u32be(const unsigned char *p) {
    return ((unsigned long)p[0] << 24) |
           ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |
           (unsigned long)p[3];
}

/* ============================================================================
 * Globals
 * ============================================================================ */

short                    gMacTCPRef       = 0;
static Boolean           gNetReady        = false;
static Boolean           gHasNativeOT     = false;
Boolean                  gUseNativeOT     = false;
static Boolean           gOTInited        = false;
static ip_addr           gOurIP           = 0;
static ip_addr           gOurNetMask      = 0xFFFFFF00UL;
ip_addr                  gServerIP        = 0;
unsigned short           gServerHttpPort  = DEFAULT_HTTP_PORT;
static NetFlimProfileMode gProfileMode    = kProfileAuto;
static Boolean           gHas68020OrLater = false;
static Boolean           gHasSystem7      = false;

static char             *gHttpScratch     = NULL;
static char              gLastErrorNote[128] = "";
static char              gPrefsSource[32]    = "";
OSErr                    gLastTcpErr      = noErr;
static Boolean           gSawMacDropOnly  = false;
static ip_addr           gMacDropHintIP   = 0;
static unsigned long     gHttpConnectTimeoutTicks = 120UL;

/* Zero-Copy Flat Circular Stream Arena & TCP VOD State */
#define NF_ARENA_ULTRA        1048576UL /* 1024 KB on >= 1.35 MB free */
#define NF_ARENA_HIGH         524288UL  /* 512 KB on >= 620 KB free */
#define NF_ARENA_MID          262144UL  /* 256 KB on >= 360 KB free */
#define NF_ARENA_LOW          131072UL  /* 128 KB on compact Macs */

static unsigned char  *gArenaRaw           = NULL;
unsigned char         *gArenaMem           = NULL;
unsigned long          gArenaCap           = 0;
unsigned long          gArenaWrite         = 0;
unsigned long          gArenaParse         = 0;
unsigned long          gArenaUsed          = 0;
unsigned long          gArenaAvailToParse  = 0;
short                  gRingNumSlots       = 0;
short                  gBufferedCount      = 0;
unsigned long          gReclaimCursor      = 0;
volatile unsigned long gIrqCompletedUpTo   = 0;
NetVodFrameDesc        gDescRing[NF_INDEX_SLOTS];

static unsigned char  *gTcpCreateBuf       = NULL;
unsigned long          gTcpCreateBufSize   = 0;
StreamPtr              gVodTcpStream       = 0;
TCPiopb                gVodOpenPB;
TCPiopb                gVodSendPB;
TCPiopb                gVodRxPB[2];
Boolean                gTcpRxArmed[2];
short                  gTcpRxActiveSlot    = 0;
VodTcpConnState        gTcpConnState       = kVodTcpClosed;
char                   gVodHttpReq[384];
wdsEntry               gVodWds[2];

Boolean                gTcpRemoteEof       = false;
VodParsePhase          gParsePhase         = kParseHttpHdr;
static unsigned char   gHdrAccum[512];
static unsigned short  gHdrAccumLen        = 0;

Boolean                gVodActive          = false;
Boolean                gVodAckReceived     = false;
unsigned char          gVodOpenStatus      = 0;
Boolean                gVodEosReached      = false;
static Boolean         gVodSilent          = false;
Boolean                gVodPaused          = false;
static Boolean         gVodStripAudio      = false;
static Boolean         gVodHalfAudio       = false;
static char            gVodVideoId[18];
static char            gVodProfile[10];
static unsigned short  gVodWidth           = 512;
static unsigned short  gVodHeight          = 342;
static unsigned short  gVodByterate        = 0;
unsigned long          gVodFrameCount      = 0;
static unsigned long   gVodTotalTicks      = 0;
unsigned long          gNextNeededFrame    = 0;

/* Native Open Transport HTTP & Zero-Copy VOD State */
typedef struct {
    EndpointRef      ep;
    unsigned long    gen;
    volatile Boolean openDone;
    volatile OSStatus openErr;
    volatile Boolean connectDone;
    volatile Boolean discon;
    volatile Boolean ordRel;
    volatile Boolean hasData;
    volatile Boolean goData;
    volatile Boolean aborted;
    InetAddress      addr;
    TCall            call;
} OtHttpState;

static OtHttpState      gOtHttp;
static unsigned long    gOtHttpGen          = 0;
static EndpointRef      gOtHttpOrphanEp     = kOTInvalidEndpointRef;
static OTNotifyUPP      gOtHttpNotifyUPP    = NULL;

EndpointRef             gOtVodEp            = kOTInvalidEndpointRef;
static unsigned long    gOtVodGen           = 0;
static volatile EndpointRef gOtVodOrphanEp  = kOTInvalidEndpointRef;
static volatile Boolean gOtVodOrphanDone    = false;
static OTNotifyUPP      gOtVodNotifyUPP     = NULL;
InetAddress             gOtVodCallAddr;
TCall                   gOtVodCall;
unsigned short          gOtVodSendOff       = 0;
unsigned short          gOtVodSendLen       = 0;
volatile Boolean        gOtVodOpenDone      = false;
volatile OSStatus       gOtVodOpenErr       = noErr;
volatile EndpointRef    gOtVodOpenEp        = kOTInvalidEndpointRef;
volatile Boolean        gOtVodConnectDone   = false;
volatile Boolean        gOtVodHasData       = false;
volatile Boolean        gOtVodNeedLook      = false;
volatile Boolean        gOtVodGoData        = false;
Boolean                 gOtVodPausedForRoom = false;
OTBuffer               *gOtPendingHead      = NULL;
OTBuffer               *gOtPendingCur       = NULL;
unsigned long           gOtPendingOff       = 0;

typedef void (*VodHotVoidFn)(void);
typedef const NetVodFrameDesc *(*VodHotIrqPeekFn)(unsigned long frameIdx);
typedef const NetVodFrameDesc *(*VodHotPeekNextFn)(void);

static VodHotVoidFn     gScanArenaFramesFn = ScanArenaFrames_68k;
static VodHotVoidFn     gVodPumpFn         = NetVodPump_68k;
static VodHotVoidFn     gVodConsumeFn      = NetVodConsumeNextFrame_68k;
static VodHotIrqPeekFn  gVodIrqPeekSlotFn  = NetVodIrqPeekSlot_68k;
static VodHotPeekNextFn gVodPeekNextSlotFn = NetVodPeekNextSlot_68k;

static void InitNetHotDispatch(void) {
    if (NetFlimCodecIs020()) {
        gScanArenaFramesFn = ScanArenaFrames_020;
        gVodPumpFn         = NetVodPump_020;
        gVodConsumeFn      = NetVodConsumeNextFrame_020;
        gVodIrqPeekSlotFn  = NetVodIrqPeekSlot_020;
        gVodPeekNextSlotFn = NetVodPeekNextSlot_020;
    } else {
        gScanArenaFramesFn = ScanArenaFrames_68k;
        gVodPumpFn         = NetVodPump_68k;
        gVodConsumeFn      = NetVodConsumeNextFrame_68k;
        gVodIrqPeekSlotFn  = NetVodIrqPeekSlot_68k;
        gVodPeekNextSlotFn = NetVodPeekNextSlot_68k;
    }
}

/* ============================================================================
 * System Detection & Preferences
 * ============================================================================ */

static void DetectEnvironment(void) {
    SysEnvRec env;
    if (SysEnvirons(1, &env) == noErr) {
        gHas68020OrLater = (env.processor >= env68020);
        gHasSystem7 = (env.systemVersion >= 0x0700);
    }
}

Boolean NetIsHighSpeedCPU(void) {
    return gHas68020OrLater;
}

NetFlimProfileMode NetGetProfileMode(void) {
    return gProfileMode;
}

void NetSetProfileMode(NetFlimProfileMode mode) {
    if (mode >= kProfileAuto && mode <= kProfilePlus) {
        gProfileMode = mode;
    }
}

const char *NetResolveProfileName(void) {
    switch (gProfileMode) {
        case kProfileSE30: return "se30";
        case kProfileSE:   return "se";
        case kProfilePlus: return "plus";
        case kProfileAuto:
        default:
            return gHas68020OrLater ? "se30" : "se";
    }
}

static void GetPrefsLocation(short *outVRef, long *outDirID, Boolean createDir) {
    *outVRef = 0;
    *outDirID = 0;

    if (gHasSystem7) {
        if (FindFolder(kOnSystemDisk, kPreferencesFolderType, createDir, outVRef, outDirID) == noErr) {
            return;
        }
    } else {
        SysEnvRec env;
        if (SysEnvirons(1, &env) == noErr) {
            *outVRef = env.sysVRefNum;
            *outDirID = 0;
            return;
        }
    }
}

static Boolean IsValidServerIP(ip_addr ip) {
    return (ip != 0 && ip != 0xFFFFFFFFUL);
}

static unsigned long GetPrefsFileModDate(short vRefNum, long dirID, ConstStr255Param macName) {
    HParamBlockRec hpb;
    Str255 nameCopy;
    if (!macName || macName[0] == 0) return 0;
    memcpy(nameCopy, macName, (size_t)macName[0] + 1);
    memset(&hpb, 0, sizeof(hpb));
    hpb.fileParam.ioNamePtr   = nameCopy;
    hpb.fileParam.ioVRefNum   = vRefNum;
    hpb.fileParam.ioDirID     = dirID;
    hpb.fileParam.ioFDirIndex = 0;
    if (PBHGetFInfoSync(&hpb) == noErr) {
        return hpb.fileParam.ioFlMdDat;
    }
    return 0;
}

static Boolean ReadPrefsBlobEx(ConstStr255Param macName, const char *cName, void *outBuf, long maxLen,
                               long *outReadLen, unsigned long *outModDate) {
    short vRef = 0;
    long dirID = 0;
    short refNum = 0;
    long count = maxLen;
    OSErr err;

    if (outReadLen) *outReadLen = 0;
    if (outModDate) *outModDate = 0;
    if (!outBuf || maxLen <= 0) return false;

    /* 1. Try System Folder:Preferences (System 7+) or SysEnv System Folder */
    GetPrefsLocation(&vRef, &dirID, false);
    if (macName && HOpen(vRef, dirID, macName, fsRdPerm, &refNum) == noErr) {
        err = FSRead(refNum, &count, outBuf);
        FSClose(refNum);
        if ((err == noErr || err == eofErr) && count > 0) {
            if (outReadLen) *outReadLen = count;
            if (outModDate) *outModDate = GetPrefsFileModDate(vRef, dirID, macName);
            return true;
        }
    }

    /* 2. Try root / System Folder dirID 0 (matches System 6 / SpotiMac fallback) */
    if (macName) {
        SysEnvRec env;
        if (SysEnvirons(1, &env) == noErr) {
            count = maxLen;
            if (HOpen(env.sysVRefNum, 0, macName, fsRdPerm, &refNum) == noErr) {
                err = FSRead(refNum, &count, outBuf);
                FSClose(refNum);
                if ((err == noErr || err == eofErr) && count > 0) {
                    if (outReadLen) *outReadLen = count;
                    if (outModDate) *outModDate = GetPrefsFileModDate(env.sysVRefNum, 0, macName);
                    return true;
                }
            }
        }
    }

    /* 3. Try current working directory (Mini vMac / BasiliskII portable launch) */
    if (cName) {
        FILE *f = fopen(cName, "rb");
        if (f) {
            size_t nr = fread(outBuf, 1, (size_t)maxLen, f);
            fclose(f);
            if (nr > 0) {
                if (outReadLen) *outReadLen = (long)nr;
                return true;
            }
        }
    }

    return false;
}

static Boolean ReadMagicSiblingServerEx(ConstStr255Param macName, const char *cName,
                                        unsigned long expectedMagic, ip_addr *outIp,
                                        unsigned long *outModDate) {
    SiblingMagicPrefsHeader hdr;
    long gotLen = 0;
    if (!ReadPrefsBlobEx(macName, cName, &hdr, (long)sizeof(hdr), &gotLen, outModDate)) return false;
    if (gotLen >= (long)sizeof(hdr) && hdr.magic == expectedMagic && IsValidServerIP(hdr.serverIp)) {
        if (outIp) *outIp = hdr.serverIp;
        return true;
    }
    return false;
}

static Boolean ReadMacDropSiblingServerEx(ip_addr *outIp, unsigned short *outPort, unsigned long *outModDate) {
    SiblingMacDropPrefsRecord rec;
    long gotLen = 0;
    if (!ReadPrefsBlobEx("\pMacDrop Prefs", "MacDrop Prefs", &rec, (long)sizeof(rec), &gotLen, outModDate)) return false;
    /* macdrop/src/file_io.c checks bytesRead >= 68 with serverIP appended inside record */
    if (gotLen >= (long)sizeof(rec) && IsValidServerIP((ip_addr)rec.serverIP)) {
        if (outIp) *outIp = (ip_addr)rec.serverIP;
        if (outPort) {
            *outPort = (rec.serverPort > 0) ? rec.serverPort : DEFAULT_HTTP_PORT;
        }
        return true;
    }
    return false;
}

static Boolean LoadOwnPrefsFileEx(ip_addr *outIp, unsigned short *outPort,
                                  NetFlimProfileMode *outProf, unsigned long *outModDate) {
    unsigned char buf[128];
    long gotLen = 0;

    if (!ReadPrefsBlobEx(PREFS_FILENAME, "NetFlim Prefs", buf, (long)(sizeof(buf) - 1), &gotLen, outModDate)) {
        return false;
    }

    if (gotLen >= (long)sizeof(NetFlimPrefsRecord)) {
        NetFlimPrefsRecord rec;
        memcpy(&rec, buf, sizeof(rec));
        if (rec.magic == NETFLIM_PREFS_MAGIC) {
            if (outIp && IsValidServerIP(rec.serverIp)) *outIp = rec.serverIp;
            if (outPort && rec.serverPort > 0) *outPort = rec.serverPort;
            if (outProf && rec.profileMode >= kProfileAuto && rec.profileMode <= kProfilePlus) {
                *outProf = (NetFlimProfileMode)rec.profileMode;
            }
            return (outIp ? IsValidServerIP(*outIp) : true);
        }
    }

    /* Backwards compatibility with v0.1.0 ASCII "NetFlim Prefs" */
    buf[gotLen] = '\0';
    {
        char ipStr[32];
        unsigned int portVal = DEFAULT_HTTP_PORT;
        int profVal = 0;
        ip_addr parsed = 0;
        ipStr[0] = '\0';
        if (sscanf((const char *)buf, "%31s %u %d", ipStr, &portVal, &profVal) >= 1) {
            if (outProf && profVal >= kProfileAuto && profVal <= kProfilePlus) {
                *outProf = (NetFlimProfileMode)profVal;
            }
            if (outPort && portVal > 0 && portVal <= 65535) {
                *outPort = (unsigned short)portVal;
            }
            if (ParseIPAddress(ipStr, &parsed) && IsValidServerIP(parsed)) {
                if (outIp) *outIp = parsed;
                return true;
            }
        }
    }
    return false;
}

static Boolean PickBestSiblingServerIP(ip_addr *outIp, unsigned short *outPort,
                                       char *outSource, int srcMax, unsigned long *outModDate) {
    ip_addr bestIp = 0;
    unsigned short bestPort = DEFAULT_HTTP_PORT;
    const char *bestSrc = NULL;
    unsigned long bestMod = 0;
    ip_addr ip;
    unsigned short port;
    unsigned long mod;

    /* Check MacDrop Prefs first so ties in modification timestamp favor MacDrop's live daemon */
    ip = 0; port = DEFAULT_HTTP_PORT; mod = 0;
    if (ReadMacDropSiblingServerEx(&ip, &port, &mod) && IsValidServerIP(ip)) {
        bestIp = ip;
        bestPort = port;
        bestSrc = "MacDrop Prefs";
        bestMod = mod;
    }

    ip = 0; mod = 0;
    if (ReadMagicSiblingServerEx("\pSpotiMac Prefs", "SpotiMac Prefs", SPOTIMAC_PREFS_MAGIC, &ip, &mod) &&
        IsValidServerIP(ip) && (!bestSrc || mod > bestMod)) {
        bestIp = ip;
        bestPort = DEFAULT_HTTP_PORT;
        bestSrc = "SpotiMac Prefs";
        bestMod = mod;
    }

    ip = 0; mod = 0;
    if (ReadMagicSiblingServerEx("\pMacAI Prefs", "MacAI Prefs", MACAI_PREFS_MAGIC, &ip, &mod) &&
        IsValidServerIP(ip) && (!bestSrc || mod > bestMod)) {
        bestIp = ip;
        bestPort = DEFAULT_HTTP_PORT;
        bestSrc = "MacAI Prefs";
        bestMod = mod;
    }

    ip = 0; mod = 0;
    if (ReadMagicSiblingServerEx("\pMaciSpot Prefs", "MaciSpot Prefs", SPOTIMAC_PREFS_MAGIC, &ip, &mod) &&
        IsValidServerIP(ip) && (!bestSrc || mod > bestMod)) {
        bestIp = ip;
        bestPort = DEFAULT_HTTP_PORT;
        bestSrc = "MaciSpot Prefs";
        bestMod = mod;
    }

    if (bestSrc && IsValidServerIP(bestIp)) {
        if (outIp) *outIp = bestIp;
        if (outPort) *outPort = bestPort;
        if (outModDate) *outModDate = bestMod;
        if (outSource && srcMax > 0) {
            strncpy(outSource, bestSrc, srcMax - 1);
            outSource[srcMax - 1] = '\0';
        }
        return true;
    }
    return false;
}

Boolean NetReadSiblingServerIP(ip_addr *outIp, unsigned short *outPort, char *outSource, int srcMax) {
    return PickBestSiblingServerIP(outIp, outPort, outSource, srcMax, NULL);
}

static void LoadPrefs(void) {
    ip_addr ownIp = 0;
    unsigned short ownPort = DEFAULT_HTTP_PORT;
    NetFlimProfileMode savedProf = gProfileMode;
    unsigned long ownMod = 0;
    Boolean hasOwn = LoadOwnPrefsFileEx(&ownIp, &ownPort, &savedProf, &ownMod);

    ip_addr sibIp = 0;
    unsigned short sibPort = DEFAULT_HTTP_PORT;
    char sibSource[32] = "";
    unsigned long sibMod = 0;
    Boolean hasSib = PickBestSiblingServerIP(&sibIp, &sibPort, sibSource, sizeof(sibSource), &sibMod);

    gProfileMode = savedProf;

    /* If a sibling app (such as MacDrop) has updated its prefs more recently than NetFlim Prefs,
     * adopt that newer server IP immediately at startup so we don't display or dial a stale IP! */
    if (hasSib && (!hasOwn || (sibMod > 0 && sibMod >= ownMod && sibIp != ownIp))) {
        gServerIP = sibIp;
        gServerHttpPort = sibPort;
        strncpy(gPrefsSource, sibSource, sizeof(gPrefsSource) - 1);
        gPrefsSource[sizeof(gPrefsSource) - 1] = '\0';
        return;
    }

    if (hasOwn) {
        gServerIP = ownIp;
        gServerHttpPort = ownPort;
        strcpy(gPrefsSource, "NetFlim Prefs");
        return;
    }

    if (hasSib) {
        gServerIP = sibIp;
        gServerHttpPort = sibPort;
        strncpy(gPrefsSource, sibSource, sizeof(gPrefsSource) - 1);
        gPrefsSource[sizeof(gPrefsSource) - 1] = '\0';
    }
}

void NetSavePrefs(void) {
    short vRef = 0;
    long dirID = 0;
    short refNum = 0;
    NetFlimPrefsRecord rec;
    long len;

    memset(&rec, 0, sizeof(rec));
    rec.magic       = NETFLIM_PREFS_MAGIC;
    rec.serverIp    = gServerIP;
    rec.serverPort  = gServerHttpPort ? gServerHttpPort : DEFAULT_HTTP_PORT;
    rec.profileMode = (short)gProfileMode;

    GetPrefsLocation(&vRef, &dirID, true);
    if (HOpen(vRef, dirID, PREFS_FILENAME, fsWrPerm, &refNum) != noErr) {
        HCreate(vRef, dirID, PREFS_FILENAME, 'NFLM', 'pref');
        if (HOpen(vRef, dirID, PREFS_FILENAME, fsWrPerm, &refNum) != noErr) {
            FILE *f = fopen("NetFlim Prefs", "wb");
            if (f) {
                fwrite(&rec, sizeof(rec), 1, f);
                fclose(f);
            }
            return;
        }
    }

    len = (long)sizeof(rec);
    SetEOF(refNum, 0);
    FSWrite(refNum, &len, &rec);
    FSClose(refNum);
}

const char *NetGetLastErrorNote(void) {
    return gLastErrorNote;
}

const char *NetGetPrefsSourceName(void) {
    return gPrefsSource;
}

Boolean NetHasOpenTransport(void) {
    long otAttr = 0;
    const long needMask = gestaltOpenTptPresentMask | gestaltOpenTptTCPPresentMask;
    if (Gestalt(gestaltOpenTpt, &otAttr) == noErr) {
        return ((otAttr & needMask) == needMask);
    }
    return false;
}

Boolean NetIsUsingNativeOT(void) {
    return gUseNativeOT;
}

/* ============================================================================
 * IP Helpers & MacTCP Initialization
 * ============================================================================ */

void FormatIPAddress(ip_addr addr, char *outBuf) {
    unsigned char b0 = (unsigned char)((addr >> 24) & 0xFF);
    unsigned char b1 = (unsigned char)((addr >> 16) & 0xFF);
    unsigned char b2 = (unsigned char)((addr >> 8)  & 0xFF);
    unsigned char b3 = (unsigned char)(addr & 0xFF);
    sprintf(outBuf, "%u.%u.%u.%u", b0, b1, b2, b3);
}

Boolean ParseIPAddress(const char *str, ip_addr *outAddr) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    const char *p = str;
    int part = 0;
    unsigned int val = 0;
    Boolean hasDigit = false;

    if (!str || !outAddr) return false;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return false;

    while (*p) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10U + (unsigned int)(*p - '0');
            if (val > 255U) return false;
            hasDigit = true;
        } else if (*p == '.') {
            if (!hasDigit || part >= 3) return false;
            if (part == 0) a = val;
            else if (part == 1) b = val;
            else if (part == 2) c = val;
            part++;
            val = 0;
            hasDigit = false;
        } else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            break;
        } else {
            return false;
        }
        p++;
    }
    if (!hasDigit || part != 3) return false;
    d = val;

    *outAddr = ((ip_addr)a << 24) | ((ip_addr)b << 16) | ((ip_addr)c << 8) | (ip_addr)d;
    return (*outAddr != 0);
}

void NetSetServer(ip_addr ip, unsigned short httpPort) {
    gServerIP = ip;
    gServerHttpPort = httpPort ? httpPort : DEFAULT_HTTP_PORT;
    strcpy(gPrefsSource, "Manual");
}

ip_addr NetGetServerIP(void)        { return gServerIP; }
unsigned short NetGetServerPort(void) { return gServerHttpPort; }
ip_addr NetGetOurIP(void)           { return gOurIP; }
Boolean NetHasServer(void)          { return (gServerIP != 0); }

static void AllocVodMemoryOnce(Boolean allocCreateBuf) {
    long freeBytes;
    unsigned long targetCap;
    long createAlloc;

    if (gArenaMem == NULL) {
        freeBytes = FreeMem();
        if (freeBytes >= 1350000L) {
            targetCap = NF_ARENA_ULTRA;
        } else if (freeBytes >= 620000L) {
            targetCap = NF_ARENA_HIGH;
        } else if (freeBytes >= 360000L) {
            targetCap = NF_ARENA_MID;
        } else {
            targetCap = NF_ARENA_LOW;
        }

        while (targetCap >= 65536UL) {
            if (FreeMem() >= (long)(targetCap + NF_ARENA_TAIL + 81920UL)) {
                gArenaRaw = (unsigned char *)NewPtrClear((Size)(targetCap + NF_ARENA_TAIL + 4UL));
                if (gArenaRaw != NULL) {
                    gArenaMem = (unsigned char *)(((unsigned long)gArenaRaw + 3UL) & ~3UL);
                    gArenaCap = targetCap;
                    if (targetCap >= NF_ARENA_ULTRA) {
                        gRingNumSlots = NF_INDEX_SLOTS - 1;
                    } else if (targetCap >= NF_ARENA_HIGH) {
                        gRingNumSlots = 63;
                    } else {
                        gRingNumSlots = 32;
                    }
                    break;
                }
            }
            targetCap /= 2UL;
        }
    }

    if (allocCreateBuf && !gTcpCreateBuf) {
        createAlloc = (FreeMem() >= 90000L) ? TCP_VOD_CREATE_MAX : 16384L;
        while (createAlloc >= 16384L && !gTcpCreateBuf) {
            gTcpCreateBuf = (unsigned char *)NewPtrClear(createAlloc);
            if (gTcpCreateBuf) {
                gTcpCreateBufSize = (unsigned long)createAlloc;
                break;
            }
            createAlloc /= 2L;
        }
    }
}

static OSStatus OtSetFourByteOption(EndpointRef ep, OTXTILevel level, OTXTIName name, UInt32 val) {
    return NetInternalOtSetFourByteOption(ep, level, name, val);
}

OSStatus NetInternalOtSetFourByteOption(EndpointRef ep, OTXTILevel level, OTXTIName name, UInt32 val) {
    TOption optReq;
    TOption optRet;
    TOptMgmt req;
    TOptMgmt ret;

    optReq.len      = kOTFourByteOptionSize;
    optReq.level    = level;
    optReq.name     = name;
    optReq.status   = 0;
    optReq.value[0] = val;

    req.opt.buf    = (UInt8 *)&optReq;
    req.opt.len    = kOTFourByteOptionSize;
    req.opt.maxlen = kOTFourByteOptionSize;
    req.flags      = T_NEGOTIATE;

    memset(&optRet, 0, sizeof(optRet));
    ret.opt.buf    = (UInt8 *)&optRet;
    ret.opt.len    = 0;
    ret.opt.maxlen = sizeof(optRet);
    ret.flags      = 0;

    return OTOptionManagement(ep, &req, &ret);
}

void NetInternalOtCloseEndpointSync(EndpointRef ep) {
    if (ep != kOTInvalidEndpointRef) {
        OTSetSynchronous(ep);
        OTCloseProvider(ep);
    }
}

static void OtCloseEndpointSync(EndpointRef ep) {
    NetInternalOtCloseEndpointSync(ep);
}

static void OtCleanupOrphans(void) {
    if (gOtHttpOrphanEp != kOTInvalidEndpointRef) {
        EndpointRef ep = gOtHttpOrphanEp;
        gOtHttpOrphanEp = kOTInvalidEndpointRef;
        OtCloseEndpointSync(ep);
    }
    if (gOtVodOrphanEp != kOTInvalidEndpointRef) {
        EndpointRef ep = (EndpointRef)gOtVodOrphanEp;
        gOtVodOrphanEp = kOTInvalidEndpointRef;
        OtCloseEndpointSync(ep);
    }
}

static pascal void OtHttpNotify(void *contextPtr, OTEventCode code, OTResult result, void *cookie) {
    unsigned long gen = (unsigned long)contextPtr;

    if (code == T_OPENCOMPLETE) {
        if (gen == gOtHttpGen && !gOtHttp.aborted) {
            gOtHttp.ep       = (EndpointRef)cookie;
            gOtHttp.openErr  = result;
            gOtHttp.openDone = true;
        } else if (result == noErr && cookie != NULL) {
            if (gOtHttpOrphanEp == kOTInvalidEndpointRef) {
                gOtHttpOrphanEp = (EndpointRef)cookie;
            }
        }
        return;
    }

    if (gen != gOtHttpGen) return;

    switch (code) {
        case T_CONNECT:
            gOtHttp.connectDone = true;
            break;
        case T_DATA:
            gOtHttp.hasData = true;
            break;
        case T_GODATA:
            gOtHttp.goData = true;
            break;
        case T_ORDREL:
            gOtHttp.ordRel = true;
            break;
        case T_DISCONNECT:
            gOtHttp.discon      = true;
            gOtHttp.connectDone = true;
            break;
        default:
            break;
    }
}

static pascal void OtVodNotify(void *contextPtr, OTEventCode code, OTResult result, void *cookie) {
    unsigned long gen = (unsigned long)contextPtr;

    if (code == T_OPENCOMPLETE) {
        if (gen == gOtVodGen && gTcpConnState == kVodTcpOpening) {
            gOtVodOpenEp   = (EndpointRef)cookie;
            gOtVodOpenErr  = result;
            gOtVodOpenDone = true;
        } else {
            if (result == noErr && cookie != NULL && gOtVodOrphanEp == kOTInvalidEndpointRef) {
                gOtVodOrphanEp = (EndpointRef)cookie;
            }
            gOtVodOrphanDone = true;
        }
        return;
    }

    if (gen != gOtVodGen) return;

    switch (code) {
        case T_CONNECT:
            gOtVodConnectDone = true;
            break;
        case T_DATA:
            gOtVodHasData = true;
            break;
        case T_GODATA:
            gOtVodGoData = true;
            break;
        case T_ORDREL:
        case T_DISCONNECT:
            gOtVodNeedLook    = true;
            gOtVodConnectDone = true;
            break;
        default:
            break;
    }
}

static Boolean InitNativeOT(void) {
    InetInterfaceInfo ifInfo;
    OSStatus err;

    err = InitOpenTransport();
    if (err != noErr) {
        return false;
    }
    gOTInited = true;

    gOtHttpNotifyUPP = NewOTNotifyUPP(OtHttpNotify);
    gOtVodNotifyUPP  = NewOTNotifyUPP(OtVodNotify);

    memset(&ifInfo, 0, sizeof(ifInfo));
    err = OTInetGetInterfaceInfo(&ifInfo, kDefaultInetInterface);
    if (err != noErr) {
        /* If OT TCP/IP is set to "Load only when needed", opening kUDPName triggers stack load */
        OTConfigurationRef cfg = OTCreateConfiguration(kUDPName);
        if (cfg != NULL && cfg != kOTInvalidConfigurationPtr) {
            OSStatus openErr = noErr;
            EndpointRef wakeEp = OTOpenEndpoint(cfg, 0, NULL, &openErr);
            if (wakeEp != kOTInvalidEndpointRef && openErr == noErr) {
                err = OTInetGetInterfaceInfo(&ifInfo, kDefaultInetInterface);
                OTCloseProvider(wakeEp);
            } else {
                CloseOpenTransport();
                gOTInited = false;
                return false;
            }
        } else {
            CloseOpenTransport();
            gOTInited = false;
            return false;
        }
    }

    if (err == noErr && ifInfo.fAddress != 0) {
        gOurIP      = (ip_addr)ifInfo.fAddress;
        gOurNetMask = (ifInfo.fNetmask != 0) ? (ip_addr)ifInfo.fNetmask : 0xFFFFFF00UL;
    } else {
        gOurIP      = 0;
        gOurNetMask = 0xFFFFFF00UL;
    }
    return true;
}

Boolean NetInit(void) {
    struct GetAddrParamBlock addrPB;
    OSErr err;

    if (gNetReady) return true;

    DetectEnvironment();
    InitNetHotDispatch();
    LoadPrefs();

    if (!gHttpScratch) {
        gHttpScratch = NewPtrClear(16384L);
    }

    gHasNativeOT = NetHasOpenTransport();
    gUseNativeOT = false;

    if (gHasNativeOT && InitNativeOT()) {
        gUseNativeOT = true;
        /* Zero-copy OTBuffer receives go directly to gArenaMem; skip 24 KB MacTCP buffer */
        AllocVodMemoryOnce(false);
        gNetReady = true;
        return true;
    }

    /* Classic MacTCP (.IPP) path or fallback when OT TCP/IP cannot initialize */
    AllocVodMemoryOnce(true);

    err = OpenDriver("\p.IPP", &gMacTCPRef);
    if (err != noErr) {
        return false;
    }

    memset(&addrPB, 0, sizeof(addrPB));
    addrPB.ioCRefNum = gMacTCPRef;
    addrPB.csCode = ipctlGetAddr;
    err = PBControlSync((ParmBlkPtr)&addrPB);
    if (err != noErr) {
        return false;
    }

    gOurIP = addrPB.ourAddress;
    gOurNetMask = addrPB.ourNetMask;
    if (gOurNetMask == 0) {
        gOurNetMask = 0xFFFFFF00UL;
    }

    gNetReady = true;
    return true;
}

void NetShutdown(void) {
    NetVodClose();
    OtCleanupOrphans();
    if (gTcpCreateBuf) {
        DisposePtr((Ptr)gTcpCreateBuf);
        gTcpCreateBuf = NULL;
        gTcpCreateBufSize = 0;
    }
    if (gArenaRaw) {
        DisposePtr((Ptr)gArenaRaw);
        gArenaRaw = NULL;
        gArenaMem = NULL;
        gArenaCap = 0;
        gRingNumSlots = 0;
    }
    if (gHttpScratch) {
        DisposePtr(gHttpScratch);
        gHttpScratch = NULL;
    }
    if (gOTInited) {
        CloseOpenTransport();
        gOTInited = false;
    }
    gUseNativeOT = false;
    gNetReady    = false;
}

/* ============================================================================
 * LAN Server Discovery (6801/udp) - Native Open Transport & Classic MacTCP
 * ============================================================================ */

static int BuildDiscoveryTargets(ip_addr targets[4]) {
    int tCount = 0;
    /* Probe SLiRP (.2), gateway (.1), subnet broadcast, and global broadcast */
    if (gOurIP != 0) {
        ip_addr subnetBcast = gOurIP | ~gOurNetMask;
        targets[tCount++] = (gOurIP & gOurNetMask) | 2UL;
        targets[tCount++] = (gOurIP & gOurNetMask) | 1UL;
        targets[tCount++] = subnetBcast;
    }
    targets[tCount++] = 0xFFFFFFFFUL;
    return tCount;
}

static Boolean ProcessDiscoveryReply(char *reply, ip_addr responderIP,
                                     char *outModules, int maxLen,
                                     Boolean *outSawMacDropHere, char *macDropNote) {
    char *p;
    char *sep1;
    ip_addr parsedIP = 0;
    ip_addr candIP = 0;
    unsigned int portVal = DEFAULT_HTTP_PORT;
    char *sep2;
    const char *mods = "";
    Boolean found = false;

    if (strncmp(reply, "MACCOMPANION_SERVER|", 20) != 0) {
        return false;
    }

    p = reply + 20;
    sep1 = strchr(p, '|');
    if (!sep1) return false;

    *sep1 = '\0';
    sep2 = strchr(sep1 + 1, '|');
    if (sep2) {
        *sep2 = '\0';
        mods = sep2 + 1;
    }

    ParseIPAddress(p, &parsedIP);
    candIP = IsValidServerIP(responderIP) ? responderIP : parsedIP;
    portVal = (unsigned int)atoi(sep1 + 1);
    if (portVal == 0 || portVal > 65535) portVal = DEFAULT_HTTP_PORT;

    /* Skip MacDrop X (macdrop-only responder) so a real MacCompanion reply can win */
    if (mods[0] != '\0' && strstr(mods, "netflim") == NULL) {
        char candStr[32];
        gSawMacDropOnly = true;
        if (outSawMacDropHere) *outSawMacDropHere = true;
        if (IsValidServerIP(parsedIP)) gMacDropHintIP = parsedIP;
        FormatIPAddress(candIP, candStr);
        sprintf(macDropNote, "Found MacDrop X on %s (no NetFlim)", candStr);
        return false;
    }

    if (IsValidServerIP(candIP)) {
        if (NetProbeServer(candIP, (unsigned short)portVal, NULL, 0)) {
            found = true;
        } else if (IsValidServerIP(parsedIP) && parsedIP != candIP &&
                   NetProbeServer(parsedIP, (unsigned short)portVal, NULL, 0)) {
            found = true;
        }
        if (found && outModules && maxLen > 0) {
            strncpy(outModules, mods, maxLen - 1);
            outModules[maxLen - 1] = '\0';
        }
    }

    return found;
}

static void FinishDiscoveryFallback(Boolean *ioFound, Boolean sawMacDropHere,
                                    const char *macDropNote,
                                    char *outModules, int maxLen) {
    /* If only MacDrop X answered on SLiRP 10.0.2.2, probe OrbStack MacCompanion VM IP */
    if (!(*ioFound) && sawMacDropHere && ((gOurIP & 0xFFFFFF00UL) == 0x0A000200UL)) {
        if (NetProbeServer(ORBSTACK_BRIDGE_IP, DEFAULT_HTTP_PORT, NULL, 0)) {
            if (outModules && maxLen > 0) {
                strncpy(outModules, "netflim", maxLen - 1);
                outModules[maxLen - 1] = '\0';
            }
            *ioFound = true;
        } else if (macDropNote[0] != '\0') {
            strncpy(gLastErrorNote, macDropNote, sizeof(gLastErrorNote) - 1);
            gLastErrorNote[sizeof(gLastErrorNote) - 1] = '\0';
        }
    } else if (!(*ioFound) && macDropNote[0] != '\0' && gLastErrorNote[0] == '\0') {
        strncpy(gLastErrorNote, macDropNote, sizeof(gLastErrorNote) - 1);
        gLastErrorNote[sizeof(gLastErrorNote) - 1] = '\0';
    }

    if (*ioFound) {
        strcpy(gPrefsSource, "LAN Discovery");
    }
}

static Boolean OtNetDiscoverServer(char *outModules, int maxLen) {
    OTConfigurationRef cfg;
    EndpointRef udpEp = kOTInvalidEndpointRef;
    char reqStr[] = "MACCOMPANION_DISCOVER";
    ip_addr targets[4];
    int tCount;
    int tIdx;
    Boolean found = false;
    Boolean sawMacDropHere = false;
    char macDropNote[128];
    InetAddress bindAddr;
    TBind bindReq;
    unsigned long startTicks;
    OSStatus err = noErr;

    macDropNote[0] = '\0';

    cfg = OTCreateConfiguration(kUDPName);
    if (cfg == NULL || cfg == kOTInvalidConfigurationPtr) {
        return false;
    }
    udpEp = OTOpenEndpoint(cfg, 0, NULL, &err);
    if (udpEp == kOTInvalidEndpointRef || err != noErr) {
        return false;
    }

    if (gOurIP == 0) {
        InetInterfaceInfo ifInfo;
        memset(&ifInfo, 0, sizeof(ifInfo));
        if (OTInetGetInterfaceInfo(&ifInfo, kDefaultInetInterface) == noErr && ifInfo.fAddress != 0) {
            gOurIP      = (ip_addr)ifInfo.fAddress;
            gOurNetMask = (ifInfo.fNetmask != 0) ? (ip_addr)ifInfo.fNetmask : 0xFFFFFF00UL;
        }
    }

    OTInitInetAddress(&bindAddr, 0, 0);
    memset(&bindReq, 0, sizeof(bindReq));
    bindReq.addr.buf    = (UInt8 *)&bindAddr;
    bindReq.addr.len    = sizeof(bindAddr);
    bindReq.addr.maxlen = sizeof(bindAddr);
    err = OTBind(udpEp, &bindReq, NULL);
    if (err != noErr) {
        OTCloseProvider(udpEp);
        return false;
    }

    (void)OtSetFourByteOption(udpEp, INET_IP, IP_BROADCAST, T_YES);
    OTSetNonBlocking(udpEp);

    tCount = BuildDiscoveryTargets(targets);
    for (tIdx = 0; tIdx < tCount; tIdx++) {
        InetAddress dstAddr;
        TUnitData ud;
        OTInitInetAddress(&dstAddr, DEFAULT_DISCOVERY_PORT, (InetHost)targets[tIdx]);
        memset(&ud, 0, sizeof(ud));
        ud.addr.buf  = (UInt8 *)&dstAddr;
        ud.addr.len  = sizeof(dstAddr);
        ud.udata.buf = (UInt8 *)reqStr;
        ud.udata.len = (OTByteCount)strlen(reqStr);
        if (OTSndUData(udpEp, &ud) == kOTLookErr) {
            if (OTLook(udpEp) == T_UDERR) {
                OTRcvUDErr(udpEp, NULL);
            }
        }
    }

    startTicks = TickCount();
    while (!found && (TickCount() - startTicks) < 120UL) {
        InetAddress srcAddr;
        char reply[256];
        TUnitData ud;
        OTFlags flags = 0;
        OSStatus rxErr;

        memset(&srcAddr, 0, sizeof(srcAddr));
        memset(&ud, 0, sizeof(ud));
        ud.addr.buf     = (UInt8 *)&srcAddr;
        ud.addr.maxlen  = sizeof(srcAddr);
        ud.udata.buf    = (UInt8 *)reply;
        ud.udata.maxlen = sizeof(reply) - 1;

        rxErr = OTRcvUData(udpEp, &ud, &flags);
        if (rxErr == noErr && ud.udata.len > 0) {
            int rLen = (int)ud.udata.len;
            if (rLen > 255) rLen = 255;
            reply[rLen] = '\0';
            if (ProcessDiscoveryReply(reply, (ip_addr)srcAddr.fHost,
                                      outModules, maxLen, &sawMacDropHere, macDropNote)) {
                found = true;
            }
        } else if (rxErr == kOTLookErr) {
            if (OTLook(udpEp) == T_UDERR) {
                OTRcvUDErr(udpEp, NULL);
            }
        } else {
            SystemTask();
        }
    }

    OTSetBlocking(udpEp);
    OTSetSynchronous(udpEp);
    OTUnbind(udpEp);
    OTCloseProvider(udpEp);

    FinishDiscoveryFallback(&found, sawMacDropHere, macDropNote, outModules, maxLen);
    return found;
}

static Boolean MacTcpDiscoverServer(char *outModules, int maxLen) {
    Ptr udpBuf;
    UDPiopb pb;
    StreamPtr udpStream = 0;
    char reqStr[] = "MACCOMPANION_DISCOVER";
    wdsEntry wds[2];
    ip_addr targets[4];
    int tCount;
    int tIdx;
    int readTry;
    Boolean found = false;
    Boolean sawMacDropHere = false;
    char macDropNote[128];
    OSErr err;

    macDropNote[0] = '\0';

    udpBuf = NewPtrClear(4096L);
    if (!udpBuf) return false;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode = UDPCreate;
    pb.csParam.create.rcvBuff = udpBuf;
    pb.csParam.create.rcvBuffLen = 4096;
    pb.csParam.create.notifyProc = NULL;
    pb.csParam.create.localPort = 0;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) {
        DisposePtr(udpBuf);
        return false;
    }
    udpStream = pb.udpStream;

    tCount = BuildDiscoveryTargets(targets);
    for (tIdx = 0; tIdx < tCount; tIdx++) {
        memset(&pb, 0, sizeof(pb));
        wds[0].length = (unsigned short)strlen(reqStr);
        wds[0].ptr = reqStr;
        wds[1].length = 0;
        wds[1].ptr = NULL;

        pb.ioCRefNum = gMacTCPRef;
        pb.csCode = UDPWrite;
        pb.udpStream = udpStream;
        pb.csParam.send.remoteHost = targets[tIdx];
        pb.csParam.send.remotePort = DEFAULT_DISCOVERY_PORT;
        pb.csParam.send.wdsPtr = (Ptr)wds;
        pb.csParam.send.checkSum = true;
        pb.csParam.send.userDataPtr = NULL;
        PBControlSync((ParmBlkPtr)&pb);
    }

    /* Listen synchronously like MacDrop/MacAI (preferring responder remoteHost over payload IP) */
    for (readTry = 0; readTry < 2 && !found; readTry++) {
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode = UDPRead;
        pb.udpStream = udpStream;
        pb.csParam.receive.timeOut = 1;
        pb.csParam.receive.secondTimeStamp = 0;
        err = PBControlSync((ParmBlkPtr)&pb);

        if (err == noErr && pb.csParam.receive.rcvBuffLen > 0) {
            ip_addr responderIP = pb.csParam.receive.remoteHost;
            char reply[256];
            int rLen = pb.csParam.receive.rcvBuffLen;
            if (rLen > 255) rLen = 255;
            memcpy(reply, pb.csParam.receive.rcvBuff, rLen);
            reply[rLen] = '\0';

            /* Return UDP buffer immediately before parsing/probing */
            pb.csCode = UDPBfrReturn;
            PBControlSync((ParmBlkPtr)&pb);

            if (ProcessDiscoveryReply(reply, responderIP, outModules, maxLen,
                                      &sawMacDropHere, macDropNote)) {
                found = true;
            }
        } else {
            break;
        }
    }

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode = UDPRelease;
    pb.udpStream = udpStream;
    PBControlSync((ParmBlkPtr)&pb);
    DisposePtr(udpBuf);

    FinishDiscoveryFallback(&found, sawMacDropHere, macDropNote, outModules, maxLen);
    return found;
}

Boolean NetDiscoverServer(char *outModules, int maxLen) {
    if (!gNetReady && !NetInit()) return false;
    if (outModules && maxLen > 0) outModules[0] = '\0';

    if (gUseNativeOT) {
        return OtNetDiscoverServer(outModules, maxLen);
    }
    return MacTcpDiscoverServer(outModules, maxLen);
}

/* ============================================================================
 * HTTP GET Client (Binary Safe for 1376B Raw Posters + Text Catalog)
 * ============================================================================ */

static long FindHeaderEnd(const char *buf, long len) {
    long i;
    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return i + 4;
        }
    }
    return -1;
}

static Boolean MatchPrefixCI(const char *s, const char *prefix, int len) {
    int i;
    for (i = 0; i < len; i++) {
        char a = s[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + ('a' - 'A'));
        if (b >= 'A' && b <= 'Z') b = (char)(b + ('a' - 'A'));
        if (a != b) return false;
    }
    return true;
}

static long ParseContentLength(const char *hdr, long hdrLen) {
    long i = 0;
    while (i + 15 < hdrLen) {
        if ((hdr[i] == 'C' || hdr[i] == 'c') &&
            MatchPrefixCI(hdr + i, "Content-Length:", 15)) {
            return atol(hdr + i + 15);
        }
        while (i < hdrLen && hdr[i] != '\n') i++;
        i++;
    }
    return -1;
}

static unsigned short FormatHttpGetRequest(const char *path, char *reqHdr, long reqHdrCap) {
    char hostStr[32];
    int len;
    FormatIPAddress(gServerIP, hostStr);
    len = snprintf(reqHdr, (size_t)reqHdrCap,
                   "GET %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: NetFlim/0.1\r\nConnection: close\r\n\r\n",
                   path, hostStr, (unsigned int)gServerHttpPort);
    if (len < 0) return 0;
    if (len >= reqHdrCap) return (unsigned short)(reqHdrCap - 1);
    return (unsigned short)len;
}

static Boolean UpdateHttpHeaderScan(long rawLen, long *ioHdrEnd, short *ioHttpCode, long *ioContentLen) {
    if (*ioHdrEnd < 0) {
        *ioHdrEnd = FindHeaderEnd(gHttpScratch, rawLen);
        if (*ioHdrEnd > 0) {
            if (strncmp(gHttpScratch, "HTTP/", 5) == 0) {
                const char *sp = strchr(gHttpScratch, ' ');
                if (sp) *ioHttpCode = (short)atoi(sp + 1);
            }
            *ioContentLen = ParseContentLength(gHttpScratch, *ioHdrEnd);
        }
    }
    return (*ioHdrEnd > 0 && *ioContentLen >= 0 && (rawLen - *ioHdrEnd) >= *ioContentLen);
}

static Boolean ExtractHttpBody(long rawLen, long hdrEnd, short httpCode,
                               char *outBuf, long outMax, long *outLen, short *outHttpStatus) {
    if (hdrEnd > 0) {
        long bodyAvail = rawLen - hdrEnd;
        long copyLen = (bodyAvail < outMax) ? bodyAvail : outMax;
        if (copyLen > 0) {
            memcpy(outBuf, gHttpScratch + hdrEnd, copyLen);
        }
        if (outLen) *outLen = copyLen;
        if (outHttpStatus) *outHttpStatus = httpCode;
        return (httpCode >= 200 && httpCode < 300);
    }
    return false;
}

static Boolean OtNetHttpGet(const char *path, char *outBuf, long outMax, long *outLen, short *outHttpStatus) {
    EndpointRef ep = kOTInvalidEndpointRef;
    OTConfigurationRef cfg;
    char reqHdr[384];
    unsigned short reqLen;
    unsigned short sentOff = 0;
    long rawLen = 0;
    long rawCap = 16384L;
    long hdrEnd = -1;
    long contentLen = -1;
    short httpCode = 0;
    Boolean ok = false;
    OSStatus err;
    unsigned long startTicks;
    unsigned long lastRxTicks;
    unsigned long waitLimit = (gHttpConnectTimeoutTicks > 0) ? gHttpConnectTimeoutTicks : 120UL;

    OtCleanupOrphans();

    gOtHttpGen++;
    memset(&gOtHttp, 0, sizeof(gOtHttp));
    gOtHttp.ep  = kOTInvalidEndpointRef;
    gOtHttp.gen = gOtHttpGen;

    cfg = OTCreateConfiguration(kTCPName);
    if (cfg == NULL || cfg == kOTInvalidConfigurationPtr) {
        return false;
    }

    err = OTAsyncOpenEndpoint(cfg, 0, NULL, gOtHttpNotifyUPP, (void *)gOtHttpGen);
    if (err != noErr) {
        gLastTcpErr = (OSErr)err;
        return false;
    }

    startTicks = TickCount();
    while (!gOtHttp.openDone && (TickCount() - startTicks) < waitLimit) {
        SystemTask();
    }
    if (!gOtHttp.openDone) {
        gOtHttp.aborted = true;
        gLastTcpErr = -23016;
        goto cleanup;
    }
    if (gOtHttp.openErr != noErr || gOtHttp.ep == kOTInvalidEndpointRef) {
        gLastTcpErr = (OSErr)(gOtHttp.openErr ? gOtHttp.openErr : -23016);
        goto cleanup;
    }
    ep = gOtHttp.ep;

    OTSetSynchronous(ep);
    err = OTBind(ep, NULL, NULL);
    if (err != noErr) {
        gLastTcpErr = (OSErr)err;
        goto cleanup;
    }
    OtSetFourByteOption(ep, INET_TCP, TCP_NODELAY, T_YES);

    OTSetAsynchronous(ep);
    OTSetNonBlocking(ep);

    OTInitInetAddress(&gOtHttp.addr, gServerHttpPort, gServerIP);
    memset(&gOtHttp.call, 0, sizeof(gOtHttp.call));
    gOtHttp.call.addr.buf = (UInt8 *)&gOtHttp.addr;
    gOtHttp.call.addr.len = sizeof(gOtHttp.addr);

    err = OTConnect(ep, &gOtHttp.call, NULL);
    if (err == kOTNoDataErr) {
        startTicks = TickCount();
        while (!gOtHttp.connectDone && (TickCount() - startTicks) < waitLimit) {
            OTResult look = OTLook(ep);
            if (look == T_CONNECT) {
                gOtHttp.connectDone = true;
                break;
            }
            if (look == T_DISCONNECT) {
                gOtHttp.discon = true;
                gOtHttp.connectDone = true;
                break;
            }
            SystemTask();
        }
        if (!gOtHttp.connectDone) {
            gLastTcpErr = -23016;
            goto cleanup;
        }
        if (gOtHttp.discon) {
            OTRcvDisconnect(ep, NULL);
            gLastTcpErr = connectionTerminated;
            goto cleanup;
        }
        err = OTRcvConnect(ep, NULL);
        if (err != noErr) {
            if (err == kOTLookErr && OTLook(ep) == T_DISCONNECT) {
                OTRcvDisconnect(ep, NULL);
            }
            gLastTcpErr = (OSErr)err;
            goto cleanup;
        }
    } else if (err != noErr) {
        if (err == kOTLookErr && OTLook(ep) == T_DISCONNECT) {
            OTRcvDisconnect(ep, NULL);
            gLastTcpErr = connectionTerminated;
        } else {
            gLastTcpErr = (OSErr)err;
        }
        goto cleanup;
    }

    reqLen = FormatHttpGetRequest(path, reqHdr, (long)sizeof(reqHdr));
    startTicks = TickCount();
    while (sentOff < reqLen && (TickCount() - startTicks) < 180UL) {
        OTResult n = OTSnd(ep, reqHdr + sentOff, (OTByteCount)(reqLen - sentOff), 0);
        if (n > 0) {
            sentOff = (unsigned short)(sentOff + (unsigned short)n);
        } else if (n == kOTFlowErr || n == kOTNoDataErr) {
            SystemTask();
        } else if (n == kOTLookErr) {
            OTResult look = OTLook(ep);
            if (look == T_DISCONNECT) {
                OTRcvDisconnect(ep, NULL);
                gLastTcpErr = connectionTerminated;
                goto cleanup;
            } else if (look == T_GODATA) {
                gOtHttp.goData = false;
            } else {
                break;
            }
        } else {
            gLastTcpErr = (OSErr)n;
            goto cleanup;
        }
    }
    if (sentOff < reqLen) {
        gLastTcpErr = -23016;
        goto cleanup;
    }

    lastRxTicks = TickCount();
    while (rawLen < rawCap) {
        OTFlags rxFlags = 0;
        OTByteCount chunkMax = (OTByteCount)((rawCap - rawLen > 2048L) ? 2048L : (rawCap - rawLen));
        OTResult n = OTRcv(ep, gHttpScratch + rawLen, chunkMax, &rxFlags);
        if (n > 0) {
            rawLen += (long)n;
            lastRxTicks = TickCount();
            if (UpdateHttpHeaderScan(rawLen, &hdrEnd, &httpCode, &contentLen)) {
                break;
            }
        } else if (n == kOTNoDataErr) {
            OTResult look;
            if (gOtHttp.discon) {
                break;
            }
            look = OTLook(ep);
            if (look == T_ORDREL) {
                OTRcvOrderlyDisconnect(ep);
                gOtHttp.ordRel = true;
                break;
            }
            if (look == T_DISCONNECT) {
                OTRcvDisconnect(ep, NULL);
                gOtHttp.discon = true;
                break;
            }
            if (gOtHttp.ordRel || (TickCount() - lastRxTicks) >= 180UL) {
                break;
            }
            SystemTask();
        } else if (n == kOTLookErr) {
            OTResult look = OTLook(ep);
            if (look == T_ORDREL) {
                OTRcvOrderlyDisconnect(ep);
                break;
            } else if (look == T_DISCONNECT) {
                OTRcvDisconnect(ep, NULL);
                break;
            } else {
                lastRxTicks = TickCount();
            }
        } else {
            break;
        }
    }

    ok = ExtractHttpBody(rawLen, hdrEnd, httpCode, outBuf, outMax, outLen, outHttpStatus);

cleanup:
    gOtHttpGen++;
    if (ep != kOTInvalidEndpointRef) {
        gOtHttp.ep = kOTInvalidEndpointRef;
        OtCloseEndpointSync(ep);
    }
    OtCleanupOrphans();
    return ok;
}

static Boolean MacTcpHttpGet(const char *path, char *outBuf, long outMax, long *outLen, short *outHttpStatus) {
    Ptr tcpBuf = NULL;
    TCPiopb openPB;
    TCPiopb pb;
    StreamPtr stream = 0;
    char reqHdr[384];
    unsigned short reqLen;
    wdsEntry wds[2];
    long rawLen = 0;
    long rawCap = 16384L;
    long hdrEnd = -1;
    long contentLen = -1;
    short httpCode = 0;
    Boolean ok = false;
    OSErr err;
    unsigned long startTicks;
    unsigned long waitLimit = (gHttpConnectTimeoutTicks > 0) ? gHttpConnectTimeoutTicks : 120UL;

    tcpBuf = NewPtrClear(TCP_RECV_BUF_SIZE);
    if (!tcpBuf) return false;

    memset(&openPB, 0, sizeof(openPB));
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode = TCPCreate;
    pb.csParam.create.rcvBuff = tcpBuf;
    pb.csParam.create.rcvBuffLen = TCP_RECV_BUF_SIZE;
    pb.csParam.create.notifyProc = NULL;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) {
        gLastTcpErr = err;
        DisposePtr(tcpBuf);
        return false;
    }
    stream = pb.tcpStream;

    /* Use a dedicated openPB for async TCPActiveOpen so cleanup never zeroes an in-flight PB */
    openPB.ioCompletion = NULL;
    openPB.ioCRefNum = gMacTCPRef;
    openPB.csCode = TCPActiveOpen;
    openPB.tcpStream = stream;
    openPB.csParam.open.ulpTimeoutValue = 2;
    openPB.csParam.open.ulpTimeoutAction = 1;
    openPB.csParam.open.validityFlags = timeoutValue | timeoutAction;
    openPB.csParam.open.remoteHost = gServerIP;
    openPB.csParam.open.remotePort = gServerHttpPort;
    openPB.csParam.open.localPort = 0;
    openPB.csParam.open.commandTimeoutValue = 2;
    err = PBControlAsync((ParmBlkPtr)&openPB);
    if (err == noErr) {
        startTicks = TickCount();
        while (openPB.ioResult > 0 && (TickCount() - startTicks) < waitLimit) {
            SystemTask();
        }
        err = (openPB.ioResult > 0) ? -23016 : openPB.ioResult;
    }
    if (err != noErr) {
        gLastTcpErr = err;
        goto cleanup;
    }

    reqLen = FormatHttpGetRequest(path, reqHdr, (long)sizeof(reqHdr));

    wds[0].length = reqLen;
    wds[0].ptr = reqHdr;
    wds[1].length = 0;
    wds[1].ptr = NULL;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode = TCPSend;
    pb.tcpStream = stream;
    pb.csParam.send.ulpTimeoutValue = 3;
    pb.csParam.send.ulpTimeoutAction = 1;
    pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.send.pushFlag = true;
    pb.csParam.send.urgentFlag = false;
    pb.csParam.send.wdsPtr = (Ptr)wds;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) {
        gLastTcpErr = err;
        goto cleanup;
    }

    /* Read response into gHttpScratch (first 16KB holds headers + body) */
    while (rawLen < rawCap) {
        unsigned short chunkMax = (unsigned short)((rawCap - rawLen > 2048L) ? 2048L : (rawCap - rawLen));
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode = TCPRcv;
        pb.tcpStream = stream;
        pb.csParam.receive.commandTimeoutValue = 3;
        pb.csParam.receive.rcvBuff = gHttpScratch + rawLen;
        pb.csParam.receive.rcvBuffLen = chunkMax;
        err = PBControlSync((ParmBlkPtr)&pb);

        if ((err == noErr || err == connectionClosing) && pb.csParam.receive.rcvBuffLen > 0) {
            rawLen += (long)pb.csParam.receive.rcvBuffLen;
            if (UpdateHttpHeaderScan(rawLen, &hdrEnd, &httpCode, &contentLen)) {
                break;
            }
            if (err == connectionClosing) {
                break;
            }
        } else {
            break;
        }
    }

    ok = ExtractHttpBody(rawLen, hdrEnd, httpCode, outBuf, outMax, outLen, outHttpStatus);

cleanup:
    if (stream != 0) {
        unsigned long waitStart;
        /* Abort immediately using separate pb (never call TCPClose on a timed-out SYN_SENT stream!) */
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode = TCPAbort;
        pb.tcpStream = stream;
        PBControlSync((ParmBlkPtr)&pb);

        /* Ensure Open Transport .IPP / MacTCP has finished with openPB before it leaves stack scope */
        waitStart = TickCount();
        while (openPB.ioResult == 1 && (TickCount() - waitStart) < 60UL) {
            SystemTask();
        }

        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode = TCPRelease;
        pb.tcpStream = stream;
        PBControlSync((ParmBlkPtr)&pb);
    }

    DisposePtr(tcpBuf);
    return ok;
}

Boolean NetHttpGet(const char *path, char *outBuf, long outMax, long *outLen, short *outHttpStatus) {
    gLastTcpErr = noErr;
    if (outLen) *outLen = 0;
    if (outHttpStatus) *outHttpStatus = 0;
    if (!gNetReady && !NetInit()) return false;
    if (gServerIP == 0 || !path || !outBuf || outMax <= 0) return false;
    if (!gHttpScratch) return false;

    if (gUseNativeOT) {
        return OtNetHttpGet(path, outBuf, outMax, outLen, outHttpStatus);
    }
    return MacTcpHttpGet(path, outBuf, outMax, outLen, outHttpStatus);
}

Boolean NetProbeServer(ip_addr ip, unsigned short port, char *outStatus, int maxLen) {
    char resp[128];
    long rLen = 0;
    short httpCode = 0;
    ip_addr savedIP = gServerIP;
    unsigned short savedPort = gServerHttpPort;
    unsigned short probePort = port ? port : DEFAULT_HTTP_PORT;
    unsigned long prevTimeout = gHttpConnectTimeoutTicks;
    char ipStr[32];
    Boolean ok;

    gLastErrorNote[0] = '\0';
    FormatIPAddress(ip, ipStr);

    gServerIP = ip;
    gServerHttpPort = probePort;
    /* Fast 55-tick (~0.9s) connect timeout when probing candidate IPs on LAN */
    gHttpConnectTimeoutTicks = 55UL;
    ok = NetHttpGet("/api/netflim/ping", resp, sizeof(resp) - 1, &rLen, &httpCode);
    gHttpConnectTimeoutTicks = prevTimeout;
    if (ok && rLen > 0) {
        resp[rLen] = '\0';
        if (strncmp(resp, "OK|netflim|", 11) == 0) {
            if (outStatus && maxLen > 0) {
                strncpy(outStatus, resp, maxLen - 1);
                outStatus[maxLen - 1] = '\0';
            }
            return true;
        }
    }

    if (httpCode > 0) {
        /* Identify whether MacDrop X or a partial server occupies this port */
        char pingBuf[160];
        long pLen = 0;
        short pCode = 0;
        gHttpConnectTimeoutTicks = 55UL;
        if (NetHttpGet("/api/ping", pingBuf, sizeof(pingBuf) - 1, &pLen, &pCode) &&
            pCode == 200 && pLen > 0) {
            pingBuf[pLen] = '\0';
            if (strncmp(pingBuf, "MACCOMPANION_SERVER|", 20) == 0) {
                char *p = pingBuf + 20;
                char *sep1 = strchr(p, '|');
                char *sep2 = sep1 ? strchr(sep1 + 1, '|') : NULL;
                const char *mods = sep2 ? (sep2 + 1) : "";
                if (sep1) {
                    ip_addr hint = 0;
                    *sep1 = '\0';
                    if (ParseIPAddress(p, &hint) && IsValidServerIP(hint)) {
                        gMacDropHintIP = hint;
                    }
                }
                if (strstr(mods, "netflim") == NULL) {
                    if (strstr(mods, "macdrop") != NULL) {
                        gSawMacDropOnly = true;
                        sprintf(gLastErrorNote, "%s:%u is MacDrop X (no NetFlim API)", ipStr, (unsigned int)probePort);
                    } else {
                        sprintf(gLastErrorNote, "%s:%u missing NetFlim module", ipStr, (unsigned int)probePort);
                    }
                }
            }
        }
        gHttpConnectTimeoutTicks = prevTimeout;
        if (gLastErrorNote[0] == '\0') {
            sprintf(gLastErrorNote, "HTTP %d from %s:%u", (int)httpCode, ipStr, (unsigned int)probePort);
        }
    } else if (gLastTcpErr == -23016) {
        sprintf(gLastErrorNote, "Timeout connecting to %s:%u", ipStr, (unsigned int)probePort);
    } else if (gLastTcpErr != noErr) {
        sprintf(gLastErrorNote, "TCP err %d connecting to %s:%u", (int)gLastTcpErr, ipStr, (unsigned int)probePort);
    } else {
        sprintf(gLastErrorNote, "Could not reach %s:%u", ipStr, (unsigned int)probePort);
    }

    gServerIP = savedIP;
    gServerHttpPort = savedPort;
    return false;
}

typedef struct {
    ip_addr        ip;
    unsigned short port;
    const char    *source;
    unsigned long  modDate;
} CandidateServer;

static void AddCandidateUnique(CandidateServer *list, int *count, int maxCount,
                               ip_addr ip, unsigned short port, const char *source,
                               unsigned long modDate) {
    int i;
    if (!IsValidServerIP(ip) || *count >= maxCount) return;
    if (port == 0) port = DEFAULT_HTTP_PORT;
    for (i = 0; i < *count; i++) {
        if (list[i].ip == ip && list[i].port == port) {
            if (modDate > list[i].modDate) {
                list[i].modDate = modDate;
                list[i].source  = source;
            }
            return;
        }
    }
    list[*count].ip = ip;
    list[*count].port = port;
    list[*count].source = source;
    list[*count].modDate = modDate;
    (*count)++;
}

static void SortCandidatesByNewest(CandidateServer *list, int count) {
    int i, j;
    for (i = 0; i + 1 < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (list[j].modDate > list[i].modDate) {
                CandidateServer tmp = list[i];
                list[i] = list[j];
                list[j] = tmp;
            }
        }
    }
}

Boolean NetAutoConnect(char *outStatus, int statusMax) {
    CandidateServer cands[6];
    int cCount = 0;
    int i;
    ip_addr ip = 0;
    unsigned short port = DEFAULT_HTTP_PORT;
    unsigned long mod = 0;
    NetFlimProfileMode prof = gProfileMode;
    char ipStr[32];

    if (outStatus && statusMax > 0) outStatus[0] = '\0';
    if (!gNetReady && !NetInit()) return false;

    gSawMacDropOnly = false;
    gMacDropHintIP = 0;

    /* Check MacDrop Prefs first so equal-timestamp ties favor MacDrop's live background daemon */
    ip = 0; port = DEFAULT_HTTP_PORT; mod = 0;
    if (ReadMacDropSiblingServerEx(&ip, &port, &mod)) {
        AddCandidateUnique(cands, &cCount, 6, ip, port, "MacDrop Prefs", mod);
    }
    ip = 0; port = DEFAULT_HTTP_PORT; mod = 0;
    if (LoadOwnPrefsFileEx(&ip, &port, &prof, &mod)) {
        AddCandidateUnique(cands, &cCount, 6, ip, port, "NetFlim Prefs", mod);
    }
    ip = 0; mod = 0;
    if (ReadMagicSiblingServerEx("\pSpotiMac Prefs", "SpotiMac Prefs", SPOTIMAC_PREFS_MAGIC, &ip, &mod)) {
        AddCandidateUnique(cands, &cCount, 6, ip, DEFAULT_HTTP_PORT, "SpotiMac Prefs", mod);
    }
    ip = 0; mod = 0;
    if (ReadMagicSiblingServerEx("\pMacAI Prefs", "MacAI Prefs", MACAI_PREFS_MAGIC, &ip, &mod)) {
        AddCandidateUnique(cands, &cCount, 6, ip, DEFAULT_HTTP_PORT, "MacAI Prefs", mod);
    }
    ip = 0; mod = 0;
    if (ReadMagicSiblingServerEx("\pMaciSpot Prefs", "MaciSpot Prefs", SPOTIMAC_PREFS_MAGIC, &ip, &mod)) {
        AddCandidateUnique(cands, &cCount, 6, ip, DEFAULT_HTTP_PORT, "MaciSpot Prefs", mod);
    }
    if (IsValidServerIP(gServerIP)) {
        AddCandidateUnique(cands, &cCount, 6, gServerIP, gServerHttpPort,
                           gPrefsSource[0] ? gPrefsSource : "Saved Prefs", 0);
    }

    SortCandidatesByNewest(cands, cCount);

    for (i = 0; i < cCount; i++) {
        if (NetProbeServer(cands[i].ip, cands[i].port, NULL, 0)) {
            strncpy(gPrefsSource, cands[i].source, sizeof(gPrefsSource) - 1);
            gPrefsSource[sizeof(gPrefsSource) - 1] = '\0';
            NetSavePrefs();
            if (outStatus && statusMax > 0) {
                FormatIPAddress(cands[i].ip, ipStr);
                snprintf(outStatus, statusMax, "Connected to %s (%s)", ipStr, cands[i].source);
            }
            return true;
        }
    }

    /* If a sibling/saved IP hit MacDrop X on SLiRP 10.0.2.2, check OrbStack bridge directly */
    if (gSawMacDropOnly && ((gOurIP & 0xFFFFFF00UL) == 0x0A000200UL)) {
        if (NetProbeServer(ORBSTACK_BRIDGE_IP, DEFAULT_HTTP_PORT, NULL, 0)) {
            strcpy(gPrefsSource, "OrbStack Bridge");
            NetSavePrefs();
            if (outStatus && statusMax > 0) {
                FormatIPAddress(ORBSTACK_BRIDGE_IP, ipStr);
                snprintf(outStatus, statusMax, "Connected to %s (OrbStack)", ipStr);
            }
            return true;
        }
    }

    if (NetDiscoverServer(NULL, 0)) {
        NetSavePrefs();
        if (outStatus && statusMax > 0) {
            FormatIPAddress(gServerIP, ipStr);
            snprintf(outStatus, statusMax, "Found %s via Scan LAN", ipStr);
        }
        return true;
    }

    if (outStatus && statusMax > 0 && gLastErrorNote[0] != '\0') {
        strncpy(outStatus, gLastErrorNote, statusMax - 1);
        outStatus[statusMax - 1] = '\0';
    }
    return false;
}

/* ============================================================================
 * HTTP Catalog & Poster Fetching
 * ============================================================================ */

static char *SplitNextPipeField(char **cursor) {
    char *start = *cursor;
    char *sep;
    if (!start) return "";
    sep = strchr(start, '|');
    if (sep) {
        *sep = '\0';
        *cursor = sep + 1;
    } else {
        *cursor = start + strlen(start);
    }
    return start;
}

short NetFetchCatalog(NetFlimVideoItem *items, short maxItems, char *outErr, int errMax) {
    static char catBuf[12288];
    long gotLen = 0;
    short httpStatus = 0;
    char *lineStart;
    short count = 0;

    if (outErr && errMax > 0) outErr[0] = '\0';
    if (!items || maxItems <= 0) return -1;

    if (!NetHttpGet("/api/netflim/catalog?encoding=macroman", catBuf, sizeof(catBuf) - 1, &gotLen, &httpStatus)) {
        if (outErr && errMax > 0) {
            sprintf(outErr, "HTTP error (%d) connecting to server", httpStatus);
        }
        return -1;
    }

    catBuf[gotLen] = '\0';
    lineStart = catBuf;

    while (*lineStart && count < maxItems) {
        char *lineEnd = strchr(lineStart, '\n');
        char *nextLine;
        char *cur;
        char *fId, *fTitle, *fDur, *fProf, *fSize, *fPoster, *fFrames, *fSilent;

        if (lineEnd) {
            if (lineEnd > lineStart && *(lineEnd - 1) == '\r') *(lineEnd - 1) = '\0';
            *lineEnd = '\0';
            nextLine = lineEnd + 1;
        } else {
            nextLine = lineStart + strlen(lineStart);
        }

        if (*lineStart != '\0') {
            NetFlimVideoItem *it = &items[count];
            memset(it, 0, sizeof(*it));
            cur = lineStart;

            fId     = SplitNextPipeField(&cur);
            fTitle  = SplitNextPipeField(&cur);
            fDur    = SplitNextPipeField(&cur);
            fProf   = SplitNextPipeField(&cur);
            fSize   = SplitNextPipeField(&cur);
            fPoster = SplitNextPipeField(&cur);
            fFrames = SplitNextPipeField(&cur);
            fSilent = SplitNextPipeField(&cur);

            if (fId[0] != '\0') {
                strncpy(it->id, fId, sizeof(it->id) - 1);
                strncpy(it->title, fTitle[0] ? fTitle : fId, sizeof(it->title) - 1);
                it->durationSec = (unsigned long)atol(fDur);
                strncpy(it->profilesCsv, fProf, sizeof(it->profilesCsv) - 1);
                it->sizeBytes   = (unsigned long)atol(fSize);
                it->hasPoster   = (atoi(fPoster) != 0);
                it->frameCount  = (unsigned long)atol(fFrames);
                it->silent      = (atoi(fSilent) != 0);
                count++;
            }
        }
        lineStart = nextLine;
    }

    return count;
}

Boolean NetFetchPosterRaw(const char *videoId, unsigned char *out1376) {
    char path[96];
    long gotLen = 0;
    short httpStatus = 0;

    if (!videoId || !out1376) return false;
    sprintf(path, "/api/netflim/poster/%s/raw", videoId);
    if (!NetHttpGet(path, (char *)out1376, NF_POSTER_BYTES, &gotLen, &httpStatus)) {
        return false;
    }
    return (gotLen >= NF_POSTER_BYTES);
}

/* ============================================================================
 * Zero-Copy Flat Circular Stream Arena VOD Client (6800/tcp - /api/netflim/stream)
 * ============================================================================ */

static void ResetRingSlots(unsigned long startFrame) {
    memset(gDescRing, 0, sizeof(gDescRing));
    gArenaWrite        = 0;
    gArenaParse        = 0;
    gArenaUsed         = 0;
    gArenaAvailToParse = 0;
    gBufferedCount     = 0;
    gReclaimCursor     = startFrame;
    gIrqCompletedUpTo  = startFrame;
}

static Boolean RingCanQueueMore(void) {
    if (gArenaCap == 0 || gRingNumSlots <= 0) return false;
    if (gVodEosReached || gTcpRemoteEof) return false;
    if (gParsePhase != kParseFrames) return true;
    return (gBufferedCount < gRingNumSlots) && ((gArenaCap - gArenaUsed) >= 4096UL);
}

Boolean NetInternalRingCanQueueMore(void) {
    return RingCanQueueMore();
}

static void FormatVodHttpRequest(unsigned long startFrame) {
    char hostStr[32];
    FormatIPAddress(gServerIP, hostStr);
    snprintf(gVodHttpReq, sizeof(gVodHttpReq),
             "GET /api/netflim/stream/%s?profile=%s&start=%lu&maxBytes=%u&align4=1%s HTTP/1.0\r\n"
             "Host: %s:%u\r\nUser-Agent: NetFlim/0.1\r\nConnection: close\r\n\r\n",
             gVodVideoId,
             gVodProfile,
             startFrame,
             (unsigned int)NF_MAX_FRAME_BYTES,
             gVodStripAudio ? "&stripAudio=1" : (gVodHalfAudio ? "&audioRate=11k" : ""),
             hostStr,
             (unsigned int)gServerHttpPort);
}

static void OtCloseVodStream(void) {
    Boolean needWaitOrphan = (gTcpConnState == kVodTcpOpening && !gOtVodOpenDone);

    gOtVodGen++;

    if (needWaitOrphan) {
        unsigned long waitStart = TickCount();
        gOtVodOrphanDone = false;
        while (!gOtVodOrphanDone && gOtVodOrphanEp == kOTInvalidEndpointRef && (TickCount() - waitStart) < 45UL) {
            SystemTask();
        }
        OtCleanupOrphans();
    }

    if (gOtPendingHead != NULL) {
        OTReleaseBuffer(gOtPendingHead);
        gOtPendingHead = NULL;
        gOtPendingCur  = NULL;
        gOtPendingOff  = 0UL;
    }

    if (gOtVodOpenEp != kOTInvalidEndpointRef) {
        EndpointRef ep = (EndpointRef)gOtVodOpenEp;
        gOtVodOpenEp = kOTInvalidEndpointRef;
        if (ep != gOtVodEp) {
            OtCloseEndpointSync(ep);
        }
    }

    if (gOtVodEp != kOTInvalidEndpointRef) {
        EndpointRef ep = gOtVodEp;
        gOtVodEp = kOTInvalidEndpointRef;
        OtCloseEndpointSync(ep);
    }

    OtCleanupOrphans();

    gTcpConnState       = kVodTcpClosed;
    gOtVodOpenDone      = false;
    gOtVodOpenErr       = noErr;
    gOtVodConnectDone   = false;
    gOtVodHasData       = false;
    gOtVodNeedLook    = false;
    gOtVodGoData        = false;
    gOtVodPausedForRoom = false;
    gOtVodSendOff       = 0;
    gOtVodSendLen       = 0;
}

static void CloseTcpStreamOnly(void) {
    TCPiopb pb;
    unsigned long waitStart;

    if (gVodTcpStream != 0) {
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode = TCPAbort;
        pb.tcpStream = gVodTcpStream;
        PBControlSync((ParmBlkPtr)&pb);

        /* Ensure any async TCPActiveOpen/TCPSend/TCPRcv has settled before release/reuse */
        waitStart = TickCount();
        while ((gVodOpenPB.ioResult == 1 ||
                gVodSendPB.ioResult == 1 ||
                (gTcpRxArmed[0] && gVodRxPB[0].ioResult == 1) ||
                (gTcpRxArmed[1] && gVodRxPB[1].ioResult == 1)) &&
               (TickCount() - waitStart) < 60UL) {
            SystemTask();
        }

        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode = TCPRelease;
        pb.tcpStream = gVodTcpStream;
        PBControlSync((ParmBlkPtr)&pb);

        gVodTcpStream = 0;
    }

    gTcpConnState    = kVodTcpClosed;
    gTcpRxArmed[0]   = false;
    gTcpRxArmed[1]   = false;
    gTcpRxActiveSlot = 0;
    memset(&gVodOpenPB, 0, sizeof(gVodOpenPB));
    memset(&gVodSendPB, 0, sizeof(gVodSendPB));
    memset(gVodRxPB, 0, sizeof(gVodRxPB));
}

static void ArmTcpRecvSlot(short slot) {
    OSErr err;
    unsigned long freeBytes;
    unsigned long toEnd;
    unsigned long chunk;

    if (gVodTcpStream == 0 || gTcpConnState != kVodTcpStreaming ||
        gTcpRxArmed[0] || gTcpRxArmed[1] || gTcpRemoteEof || gArenaCap == 0 || !gArenaMem) {
        return;
    }

    if (gParsePhase != kParseFrames) {
        toEnd = (gArenaCap > gArenaWrite) ? (gArenaCap - gArenaWrite) : 0UL;
        chunk = (toEnd > 16384UL) ? 16384UL : toEnd;
        if (chunk == 0) return;
    } else {
        if (!RingCanQueueMore()) return;
        freeBytes = (gArenaCap > gArenaUsed) ? (gArenaCap - gArenaUsed) : 0UL;
        toEnd     = gArenaCap - gArenaWrite;
        chunk     = (freeBytes < toEnd) ? freeBytes : toEnd;
        {
            unsigned long maxChunk = (gTcpCreateBufSize >= 24576UL) ? TCP_VOD_CHUNK_MAX : 8192UL;
            if (chunk > maxChunk) chunk = maxChunk;
        }
        if (chunk == 0) return;
    }

    memset(&gVodRxPB[slot], 0, sizeof(TCPiopb));
    gVodRxPB[slot].ioCompletion = NULL;
    gVodRxPB[slot].ioCRefNum = gMacTCPRef;
    gVodRxPB[slot].csCode = TCPRcv;
    gVodRxPB[slot].tcpStream = gVodTcpStream;
    gVodRxPB[slot].csParam.receive.commandTimeoutValue = 3;
    gVodRxPB[slot].csParam.receive.rcvBuff = (Ptr)(gArenaMem + gArenaWrite);
    gVodRxPB[slot].csParam.receive.rcvBuffLen = (unsigned short)chunk;

    err = PBControlAsync((ParmBlkPtr)&gVodRxPB[slot]);
    if (err == noErr) {
        gTcpRxArmed[slot] = true;
        gTcpRxActiveSlot  = slot;
    }
}

static void ArmTcpRecv(void) {
    ArmTcpRecvSlot(gTcpRxActiveSlot);
}

void NetInternalArmTcpRecvSlot(short slot) {
    ArmTcpRecvSlot(slot);
}

void NetInternalArmTcpRecv(void) {
    ArmTcpRecv();
}

void NetInternalOtCloseVodStream(void) {
    OtCloseVodStream();
}

static void ParseInitialHandshake(void) {
    while (gArenaParse < gArenaWrite && gParsePhase != kParseFrames) {
        if (gParsePhase == kParseHttpHdr) {
            while (gArenaParse < gArenaWrite) {
                if (gHdrAccumLen + 1 >= (unsigned short)sizeof(gHdrAccum)) {
                    gVodOpenStatus = NF_OPEN_ERROR;
                    gVodEosReached = true;
                    gTcpRemoteEof  = true;
                    return;
                }
                gHdrAccum[gHdrAccumLen++] = gArenaMem[gArenaParse++];
                if (gHdrAccumLen >= 4 &&
                    gHdrAccum[gHdrAccumLen - 4] == '\r' &&
                    gHdrAccum[gHdrAccumLen - 3] == '\n' &&
                    gHdrAccum[gHdrAccumLen - 2] == '\r' &&
                    gHdrAccum[gHdrAccumLen - 1] == '\n') {
                    short httpCode = 0;
                    const char *sp;
                    gHdrAccum[gHdrAccumLen] = '\0';
                    sp = strchr((const char *)gHdrAccum, ' ');
                    if (sp) httpCode = (short)atoi(sp + 1);
                    if (httpCode < 200 || httpCode >= 300) {
                        if (strstr((const char *)gHdrAccum, "NO_PROFILE") != NULL) {
                            gVodOpenStatus = NF_OPEN_NO_PROFILE;
                        } else if (httpCode == 404) {
                            gVodOpenStatus = NF_OPEN_NOT_FOUND;
                        } else {
                            gVodOpenStatus = NF_OPEN_ERROR;
                        }
                        gVodEosReached = true;
                        gTcpRemoteEof  = true;
                        return;
                    }
                    gParsePhase  = kParseStreamHdr;
                    gHdrAccumLen = 0;
                    break;
                }
            }
        } else if (gParsePhase == kParseStreamHdr) {
            unsigned long avail = gArenaWrite - gArenaParse;
            unsigned short need = (unsigned short)(24 - gHdrAccumLen);
            unsigned short take = (avail < (unsigned long)need) ? (unsigned short)avail : need;
            if (take > 0) {
                memcpy(gHdrAccum + gHdrAccumLen, gArenaMem + gArenaParse, take);
                gHdrAccumLen = (unsigned short)(gHdrAccumLen + take);
                gArenaParse += (unsigned long)take;
            }
            if (gHdrAccumLen < 24) break;

            if (read_u32be(gHdrAccum + 0) != NF_MAGIC_SERVER) {
                gVodOpenStatus = NF_OPEN_ERROR;
                gVodEosReached = true;
                gTcpRemoteEof  = true;
                return;
            }

            gVodWidth      = read_u16be(gHdrAccum + 4);
            gVodHeight     = read_u16be(gHdrAccum + 6);
            gVodSilent     = (read_u16be(gHdrAccum + 8) != 0);
            gVodByterate   = read_u16be(gHdrAccum + 10);
            gVodFrameCount = read_u32be(gHdrAccum + 12);
            gVodTotalTicks = read_u32be(gHdrAccum + 16);
            if (gVodStripAudio) gVodSilent = true;

            gVodOpenStatus  = NF_OPEN_OK;
            gVodAckReceived = true;
            gParsePhase     = kParseFrames;
            gHdrAccumLen    = 0;

            /* Align Frame 0 to offset 0 of gArenaMem (32-bit aligned) */
            {
                unsigned long rem = (gArenaWrite > gArenaParse) ? (gArenaWrite - gArenaParse) : 0UL;
                if (rem > 0 && gArenaParse > 0) {
                    memmove(gArenaMem, gArenaMem + gArenaParse, rem);
                }
                gArenaParse        = 0;
                gArenaWrite        = rem;
                gArenaUsed         = rem;
                gArenaAvailToParse = rem;
            }
            gScanArenaFramesFn();
            break;
        }
    }
}

void NetInternalParseInitialHandshake(void) {
    ParseInitialHandshake();
}

static Boolean StartMacTcpVodConnection(unsigned long startFrame) {
    TCPiopb pb;
    OSErr err;

    CloseTcpStreamOnly();
    ResetRingSlots(startFrame);

    if (!gNetReady && !NetInit()) return false;
    if (!gArenaMem || gArenaCap == 0 || gRingNumSlots <= 0 || !gTcpCreateBuf) {
        return false;
    }
    if (gServerIP == 0 || gVodVideoId[0] == '\0') return false;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode = TCPCreate;
    pb.csParam.create.rcvBuff = (Ptr)gTcpCreateBuf;
    pb.csParam.create.rcvBuffLen = gTcpCreateBufSize;
    pb.csParam.create.notifyProc = NULL;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) {
        gLastTcpErr = err;
        return false;
    }
    gVodTcpStream = pb.tcpStream;

    FormatVodHttpRequest(startFrame);

    gNextNeededFrame = startFrame;
    gVodEosReached   = false;
    gTcpRemoteEof    = false;
    gParsePhase      = kParseHttpHdr;
    gHdrAccumLen     = 0;
    gTcpRxArmed[0]   = false;
    gTcpRxArmed[1]   = false;
    gTcpRxActiveSlot = 0;

    memset(&gVodOpenPB, 0, sizeof(gVodOpenPB));
    gVodOpenPB.ioCompletion = NULL;
    gVodOpenPB.ioCRefNum = gMacTCPRef;
    gVodOpenPB.csCode = TCPActiveOpen;
    gVodOpenPB.tcpStream = gVodTcpStream;
    gVodOpenPB.csParam.open.ulpTimeoutValue = 4;
    gVodOpenPB.csParam.open.ulpTimeoutAction = 1;
    gVodOpenPB.csParam.open.validityFlags = timeoutValue | timeoutAction;
    gVodOpenPB.csParam.open.remoteHost = gServerIP;
    gVodOpenPB.csParam.open.remotePort = gServerHttpPort;
    gVodOpenPB.csParam.open.localPort = 0;
    gVodOpenPB.csParam.open.commandTimeoutValue = 4;

    err = PBControlAsync((ParmBlkPtr)&gVodOpenPB);
    if (err != noErr) {
        gLastTcpErr = err;
        CloseTcpStreamOnly();
        return false;
    }

    gTcpConnState = kVodTcpOpening;
    return true;
}

void NetInternalOtVodFailClosed(OSErr err) {
    if (err != noErr) {
        gLastTcpErr = err;
    }
    if (!gVodAckReceived) {
        gVodOpenStatus = NF_OPEN_ERROR;
    }
    gTcpRemoteEof  = true;
    gVodEosReached = true;
    gTcpConnState  = kVodTcpClosed;
}

static Boolean StartOtVodConnection(unsigned long startFrame) {
    OTConfigurationRef cfg;
    OSStatus err;

    OtCloseVodStream();
    ResetRingSlots(startFrame);

    if (!gNetReady && !NetInit()) return false;
    if (!gArenaMem || gArenaCap == 0 || gRingNumSlots <= 0) {
        return false;
    }
    if (gServerIP == 0 || gVodVideoId[0] == '\0') return false;

    FormatVodHttpRequest(startFrame);

    gNextNeededFrame    = startFrame;
    gVodEosReached      = false;
    gTcpRemoteEof       = false;
    gParsePhase         = kParseHttpHdr;
    gHdrAccumLen        = 0;
    gOtVodGen++;
    gOtVodOpenDone      = false;
    gOtVodOpenErr       = noErr;
    gOtVodOpenEp        = kOTInvalidEndpointRef;
    gOtVodConnectDone   = false;
    gOtVodHasData       = false;
    gOtVodNeedLook      = false;
    gOtVodGoData        = false;
    gOtVodPausedForRoom = false;
    gOtVodSendOff       = 0;
    gOtVodSendLen       = (unsigned short)strlen(gVodHttpReq);

    cfg = OTCreateConfiguration(kTCPName);
    if (cfg == NULL || cfg == kOTInvalidConfigurationPtr) {
        return false;
    }

    gTcpConnState = kVodTcpOpening;
    err = OTAsyncOpenEndpoint(cfg, 0, NULL, gOtVodNotifyUPP, (void *)gOtVodGen);
    if (err != noErr) {
        gLastTcpErr = (OSErr)err;
        gTcpConnState = kVodTcpClosed;
        return false;
    }

    return true;
}

static Boolean OtVodNeedsFastPump(void) {
    if (gTcpConnState == kVodTcpOpening || gTcpConnState == kVodTcpConnecting || gTcpConnState == kVodTcpSending) {
        return true;
    }
    if (!gVodAckReceived) return true;
    if (gOtPendingHead != NULL || gOtVodHasData || gOtVodNeedLook) return true;
    if (gVodPaused || gVodEosReached || gTcpRemoteEof) return false;
    return (gBufferedCount < 16);
}

Boolean NetVodOpen(const char *videoId, const char *profile, Boolean stripAudio, Boolean halfAudio, unsigned long startFrame) {
    Boolean started;

    NetVodClose();

    if (!gNetReady && !NetInit()) return false;
    if (!gArenaMem || gArenaCap == 0 || gRingNumSlots <= 0 ||
        (!gUseNativeOT && !gTcpCreateBuf) || gServerIP == 0 || !videoId) {
        return false;
    }

    strncpy(gVodVideoId, videoId, sizeof(gVodVideoId) - 1);
    gVodVideoId[sizeof(gVodVideoId) - 1] = '\0';
    strncpy(gVodProfile, profile ? profile : NetResolveProfileName(), sizeof(gVodProfile) - 1);
    gVodProfile[sizeof(gVodProfile) - 1] = '\0';

    gVodStripAudio     = stripAudio;
    gVodHalfAudio      = (!stripAudio && halfAudio);
    gVodPaused         = false;
    gVodAckReceived    = false;
    gVodOpenStatus     = NF_OPEN_OK;
    gVodEosReached     = false;
    gVodSilent         = stripAudio;
    gVodWidth          = 512;
    gVodHeight         = 342;
    gVodByterate       = 0;
    gVodFrameCount     = 0;
    gVodTotalTicks     = 0;
    gVodActive         = true;

    started = gUseNativeOT ? StartOtVodConnection(startFrame) : StartMacTcpVodConnection(startFrame);
    if (!started) {
        gVodActive = false;
        return false;
    }

    NetVodPump();
    return true;
}

void NetVodClose(void) {
    if (gUseNativeOT) {
        OtCloseVodStream();
    } else {
        CloseTcpStreamOnly();
    }
    gVodActive      = false;
    gVodAckReceived = false;
    ResetRingSlots(0UL);
}

void NetVodPump(void) {
    gVodPumpFn();
}

void NetVodSeek(unsigned long targetFrame) {
    Boolean started;
    if (!gVodActive) return;
    started = gUseNativeOT ? StartOtVodConnection(targetFrame) : StartMacTcpVodConnection(targetFrame);
    if (!started) {
        gVodOpenStatus = NF_OPEN_ERROR;
        gTcpRemoteEof  = true;
        return;
    }
    NetVodPump();
}

void NetVodSetPaused(Boolean paused) {
    if (!gVodActive) return;
    gVodPaused = paused;
}

Boolean NetVodNeedsFastPump(void) {
    short cur = gTcpRxActiveSlot;
    if (!gVodActive) return false;
    if (gUseNativeOT) {
        return OtVodNeedsFastPump();
    }
    if (gTcpConnState == kVodTcpOpening || gTcpConnState == kVodTcpSending) return true;
    if (!gVodAckReceived) return true;
    if (gTcpRxArmed[cur] && gVodRxPB[cur].ioResult != 1) return true;
    if (gVodPaused || gVodEosReached || gTcpRemoteEof) return false;
    return (gBufferedCount < 16);
}

const NetVodFrameDesc *NetVodPeekNextSlot(void) {
    return gVodPeekNextSlotFn();
}

void NetVodConsumeNextFrame(void) {
    gVodConsumeFn();
}

const NetVodFrameDesc *NetVodIrqPeekSlot(unsigned long frameIdx) {
    return gVodIrqPeekSlotFn(frameIdx);
}

void NetVodIrqSetCompletedUpTo(unsigned long completedUpTo) {
    gIrqCompletedUpTo = completedUpTo;
}

short NetVodGetBufferedReady(void) {
    return gBufferedCount;
}

void NetVodGetInfo(NetVodInfo *outInfo) {
    if (!outInfo) return;

    outInfo->active          = gVodActive;
    outInfo->ackReceived     = gVodAckReceived;
    outInfo->openStatus      = gVodOpenStatus;
    outInfo->eosReached      = gVodEosReached;
    outInfo->silent          = gVodSilent;
    outInfo->paused          = gVodPaused;
    outInfo->width           = gVodWidth;
    outInfo->height          = gVodHeight;
    outInfo->byterate        = gVodByterate;
    outInfo->frameCount      = gVodFrameCount;
    outInfo->totalTicks      = gVodTotalTicks;
    outInfo->nextNeededFrame = gNextNeededFrame;
    outInfo->numSlots        = gRingNumSlots;
    outInfo->bufferedReady   = gBufferedCount;
}
