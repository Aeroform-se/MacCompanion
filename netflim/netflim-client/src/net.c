#include "net.h"
#include "codec.h"

#include <Devices.h>
#include <Errors.h>
#include <Files.h>
#include <Folders.h>
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

static short             gMacTCPRef       = 0;
static Boolean           gMacTCPReady     = false;
static ip_addr           gOurIP           = 0;
static ip_addr           gOurNetMask      = 0xFFFFFF00UL;
static ip_addr           gServerIP        = 0;
static unsigned short    gServerHttpPort  = DEFAULT_HTTP_PORT;
static NetFlimProfileMode gProfileMode    = kProfileAuto;
static Boolean           gHas68020OrLater = false;
static Boolean           gHasSystem7      = false;

static char             *gHttpScratch     = NULL;
static char              gLastErrorNote[128] = "";
static char              gPrefsSource[32]    = "";
static OSErr             gLastTcpErr      = noErr;
static Boolean           gSawMacDropOnly  = false;
static ip_addr           gMacDropHintIP   = 0;
static unsigned long     gHttpConnectTimeoutTicks = 120UL;

/* Zero-Copy Flat Circular Stream Arena & TCP VOD State */
#define NF_ARENA_HIGH         524288UL  /* 512 KB on >= 900 KB free */
#define NF_ARENA_MID          262144UL  /* 256 KB on >= 500 KB free */
#define NF_ARENA_LOW          131072UL  /* 128 KB on compact Macs */
#define NF_ARENA_TAIL         (NF_MAX_FRAME_BYTES + 16UL)
#define NF_INDEX_SLOTS        64
#define TCP_VOD_CREATE_MAX    65535L
#define TCP_VOD_CHUNK_MAX     24576UL

typedef struct {
    Boolean        ready;
    unsigned long  frameIdx;
    unsigned char *dataPtr;
    unsigned short payloadLen;
    unsigned long  recordBytes;
} VodFrameDesc;

typedef enum {
    kVodTcpClosed = 0,
    kVodTcpOpening,
    kVodTcpSending,
    kVodTcpStreaming
} VodTcpConnState;

typedef enum {
    kParseHttpHdr = 0,
    kParseStreamHdr,
    kParseFrames
} VodParsePhase;

static unsigned char  *gArenaMem           = NULL;
static unsigned long   gArenaCap           = 0;
static unsigned long   gArenaWrite         = 0;
static unsigned long   gArenaParse         = 0;
static unsigned long   gArenaUsed          = 0;
static unsigned long   gArenaAvailToParse  = 0;
static short           gRingNumSlots       = 0;
static short           gBufferedCount      = 0;
static VodFrameDesc    gDescRing[NF_INDEX_SLOTS];

static unsigned char  *gTcpCreateBuf       = NULL;
static unsigned long   gTcpCreateBufSize   = 0;
static StreamPtr       gVodTcpStream       = 0;
static TCPiopb         gVodOpenPB;
static TCPiopb         gVodSendPB;
static TCPiopb         gVodRxPB;
static Boolean         gTcpRxArmed         = false;
static VodTcpConnState gTcpConnState       = kVodTcpClosed;
static char            gVodHttpReq[384];
static wdsEntry        gVodWds[2];

static Boolean         gTcpRemoteEof       = false;
static VodParsePhase   gParsePhase         = kParseHttpHdr;
static unsigned char   gHdrAccum[512];
static unsigned short  gHdrAccumLen        = 0;

static Boolean         gVodActive          = false;
static Boolean         gVodAckReceived     = false;
static unsigned char   gVodOpenStatus      = 0;
static Boolean         gVodEosReached      = false;
static Boolean         gVodSilent          = false;
static Boolean         gVodPaused          = false;
static Boolean         gVodStripAudio      = false;
static char            gVodVideoId[18];
static char            gVodProfile[10];
static unsigned short  gVodWidth           = 512;
static unsigned short  gVodHeight          = 342;
static unsigned short  gVodByterate        = 0;
static unsigned long   gVodFrameCount      = 0;
static unsigned long   gVodTotalTicks      = 0;
static unsigned long   gNextNeededFrame    = 0;

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

static void AllocVodMemoryOnce(void) {
    long freeBytes;
    unsigned long targetCap;
    long createAlloc;

    if (gArenaMem != NULL) return;

    freeBytes = FreeMem();
    if (freeBytes >= 750000L) {
        targetCap = NF_ARENA_HIGH;
    } else if (freeBytes >= 400000L) {
        targetCap = NF_ARENA_MID;
    } else {
        targetCap = NF_ARENA_LOW;
    }

    while (targetCap >= 65536UL) {
        if (FreeMem() >= (long)(targetCap + NF_ARENA_TAIL + 81920UL)) {
            gArenaMem = (unsigned char *)NewPtrClear((Size)(targetCap + NF_ARENA_TAIL));
            if (gArenaMem != NULL) {
                gArenaCap = targetCap;
                gRingNumSlots = (targetCap >= NF_ARENA_MID) ? (NF_INDEX_SLOTS - 1) : 32;
                break;
            }
        }
        targetCap /= 2UL;
    }

    createAlloc = (FreeMem() >= 180000L) ? 65536L : 32768L;
    while (createAlloc >= 16384L && !gTcpCreateBuf) {
        gTcpCreateBuf = (unsigned char *)NewPtrClear(createAlloc);
        if (gTcpCreateBuf) {
            gTcpCreateBufSize = (createAlloc >= 65536L) ? 65535UL : (unsigned long)createAlloc;
            break;
        }
        createAlloc /= 2L;
    }
}

Boolean NetInit(void) {
    struct GetAddrParamBlock addrPB;
    OSErr err;

    if (gMacTCPReady) return true;

    DetectEnvironment();
    LoadPrefs();

    if (!gHttpScratch) {
        gHttpScratch = NewPtrClear(16384L);
    }

    AllocVodMemoryOnce();

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

    gMacTCPReady = true;
    return true;
}

void NetShutdown(void) {
    NetVodClose();
    if (gTcpCreateBuf) {
        DisposePtr((Ptr)gTcpCreateBuf);
        gTcpCreateBuf = NULL;
        gTcpCreateBufSize = 0;
    }
    if (gArenaMem) {
        DisposePtr((Ptr)gArenaMem);
        gArenaMem = NULL;
        gArenaCap = 0;
        gRingNumSlots = 0;
    }
    if (gHttpScratch) {
        DisposePtr(gHttpScratch);
        gHttpScratch = NULL;
    }
}

/* ============================================================================
 * TCP Control Helper & Discovery (6801/udp)
 * ============================================================================ */

/* ============================================================================
 * LAN Server Discovery (6801/udp) - Synchronous MacDrop / SpotiMac Pattern
 * ============================================================================ */

Boolean NetDiscoverServer(char *outModules, int maxLen) {
    Ptr udpBuf;
    UDPiopb pb;
    StreamPtr udpStream = 0;
    char reqStr[] = "MACCOMPANION_DISCOVER";
    wdsEntry wds[2];
    ip_addr targets[4];
    int tCount = 0;
    int tIdx;
    int readTry;
    Boolean found = false;
    Boolean sawMacDropHere = false;
    char macDropNote[128];
    OSErr err;

    if (!gMacTCPReady && !NetInit()) return false;
    if (outModules && maxLen > 0) outModules[0] = '\0';
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

    /* Probe SLiRP (.2), gateway (.1), subnet broadcast, and global broadcast */
    if (gOurIP != 0) {
        ip_addr subnetBcast = gOurIP | ~gOurNetMask;
        targets[tCount++] = (gOurIP & gOurNetMask) | 2UL;
        targets[tCount++] = (gOurIP & gOurNetMask) | 1UL;
        targets[tCount++] = subnetBcast;
    }
    targets[tCount++] = 0xFFFFFFFFUL;

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

            if (strncmp(reply, "MACCOMPANION_SERVER|", 20) == 0) {
                char *p = reply + 20;
                char *sep1 = strchr(p, '|');
                if (sep1) {
                    ip_addr parsedIP = 0;
                    ip_addr candIP = 0;
                    unsigned int portVal = DEFAULT_HTTP_PORT;
                    char *sep2;
                    const char *mods = "";

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
                        sawMacDropHere = true;
                        if (IsValidServerIP(parsedIP)) gMacDropHintIP = parsedIP;
                        FormatIPAddress(candIP, candStr);
                        sprintf(macDropNote, "Found MacDrop X on %s (no NetFlim)", candStr);
                        continue;
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
                }
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

    /* If only MacDrop X answered on SLiRP 10.0.2.2, probe OrbStack MacCompanion VM IP */
    if (!found && sawMacDropHere && ((gOurIP & 0xFFFFFF00UL) == 0x0A000200UL)) {
        if (NetProbeServer(ORBSTACK_BRIDGE_IP, DEFAULT_HTTP_PORT, NULL, 0)) {
            if (outModules && maxLen > 0) {
                strncpy(outModules, "netflim", maxLen - 1);
                outModules[maxLen - 1] = '\0';
            }
            found = true;
        } else if (macDropNote[0] != '\0') {
            strncpy(gLastErrorNote, macDropNote, sizeof(gLastErrorNote) - 1);
            gLastErrorNote[sizeof(gLastErrorNote) - 1] = '\0';
        }
    } else if (!found && macDropNote[0] != '\0' && gLastErrorNote[0] == '\0') {
        strncpy(gLastErrorNote, macDropNote, sizeof(gLastErrorNote) - 1);
        gLastErrorNote[sizeof(gLastErrorNote) - 1] = '\0';
    }

    if (found) {
        strcpy(gPrefsSource, "LAN Discovery");
    }
    return found;
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

Boolean NetHttpGet(const char *path, char *outBuf, long outMax, long *outLen, short *outHttpStatus) {
    Ptr tcpBuf = NULL;
    TCPiopb openPB;
    TCPiopb pb;
    StreamPtr stream = 0;
    char reqHdr[384];
    char hostStr[32];
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

    gLastTcpErr = noErr;
    if (outLen) *outLen = 0;
    if (outHttpStatus) *outHttpStatus = 0;
    if (!gMacTCPReady && !NetInit()) return false;
    if (gServerIP == 0 || !path || !outBuf || outMax <= 0) return false;
    if (!gHttpScratch) return false;

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

    FormatIPAddress(gServerIP, hostStr);
    sprintf(reqHdr,
            "GET %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: NetFlim/0.1\r\nConnection: close\r\n\r\n",
            path, hostStr, (unsigned int)gServerHttpPort);

    wds[0].length = (unsigned short)strlen(reqHdr);
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
            if (hdrEnd < 0) {
                hdrEnd = FindHeaderEnd(gHttpScratch, rawLen);
                if (hdrEnd > 0) {
                    if (strncmp(gHttpScratch, "HTTP/", 5) == 0) {
                        const char *sp = strchr(gHttpScratch, ' ');
                        if (sp) httpCode = (short)atoi(sp + 1);
                    }
                    contentLen = ParseContentLength(gHttpScratch, hdrEnd);
                }
            }
            if (hdrEnd > 0 && contentLen >= 0 && (rawLen - hdrEnd) >= contentLen) {
                break;
            }
            if (err == connectionClosing) {
                break;
            }
        } else {
            break;
        }
    }

    if (hdrEnd > 0) {
        long bodyAvail = rawLen - hdrEnd;
        long copyLen = (bodyAvail < outMax) ? bodyAvail : outMax;
        if (copyLen > 0) {
            memcpy(outBuf, gHttpScratch + hdrEnd, copyLen);
        }
        if (outLen) *outLen = copyLen;
        if (outHttpStatus) *outHttpStatus = httpCode;
        ok = (httpCode >= 200 && httpCode < 300);
    }

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
    if (!gMacTCPReady && !NetInit()) return false;

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

static void ResetRingSlots(void) {
    memset(gDescRing, 0, sizeof(gDescRing));
    gArenaWrite        = 0;
    gArenaParse        = 0;
    gArenaUsed         = 0;
    gArenaAvailToParse = 0;
    gBufferedCount     = 0;
}

static Boolean RingCanQueueMore(void) {
    if (gArenaCap == 0 || gRingNumSlots <= 0) return false;
    if (gVodEosReached || gTcpRemoteEof) return false;
    if (gParsePhase != kParseFrames) return true;
    return (gBufferedCount < gRingNumSlots) && ((gArenaCap - gArenaUsed) >= 2048UL);
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
                (gTcpRxArmed && gVodRxPB.ioResult == 1)) &&
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

    gTcpConnState = kVodTcpClosed;
    gTcpRxArmed   = false;
    memset(&gVodOpenPB, 0, sizeof(gVodOpenPB));
    memset(&gVodSendPB, 0, sizeof(gVodSendPB));
    memset(&gVodRxPB, 0, sizeof(gVodRxPB));
}

static void ArmTcpRecv(void) {
    OSErr err;
    unsigned long freeBytes;
    unsigned long toEnd;
    unsigned long chunk;

    if (gVodTcpStream == 0 || gTcpConnState != kVodTcpStreaming ||
        gTcpRxArmed || gTcpRemoteEof || gArenaCap == 0 || !gArenaMem) {
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
        if (chunk > TCP_VOD_CHUNK_MAX) chunk = TCP_VOD_CHUNK_MAX;
        if (chunk == 0) return;
    }

    memset(&gVodRxPB, 0, sizeof(gVodRxPB));
    gVodRxPB.ioCompletion = NULL;
    gVodRxPB.ioCRefNum = gMacTCPRef;
    gVodRxPB.csCode = TCPRcv;
    gVodRxPB.tcpStream = gVodTcpStream;
    gVodRxPB.csParam.receive.commandTimeoutValue = 3;
    gVodRxPB.csParam.receive.rcvBuff = (Ptr)(gArenaMem + gArenaWrite);
    gVodRxPB.csParam.receive.rcvBuffLen = (unsigned short)chunk;

    err = PBControlAsync((ParmBlkPtr)&gVodRxPB);
    if (err == noErr) {
        gTcpRxArmed = true;
    }
}

/*
 * ScanArenaFrames: Zero-copy in-place 8-byte aligned frame scanner.
 * Reads [u32 frameIdx][u16 payloadLen][u16 padBytes] directly inside gArenaMem.
 * Only mirrors bytes into gArenaMem[gArenaCap..] when a frame straddles the
 * wrap boundary (<1.5% of frames).
 */
static void ScanArenaFrames(void) {
    while (gArenaAvailToParse >= 8UL && gBufferedCount < gRingNumSlots) {
        const unsigned char *hdr;
        unsigned long frameIdx;
        unsigned short payloadLen;
        unsigned short padBytes;
        unsigned long recordBytes;

        if (gArenaParse + 8UL > gArenaCap) {
            unsigned long hWrap = (gArenaParse + 8UL) - gArenaCap;
            memcpy(gArenaMem + gArenaCap, gArenaMem, hWrap);
        }

        hdr        = gArenaMem + gArenaParse;
        frameIdx   = read_u32be(hdr);
        payloadLen = read_u16be(hdr + 4);
        padBytes   = (unsigned short)(read_u16be(hdr + 6) & 3U);

        if (payloadLen == 0xFFFFU) {
            gVodEosReached     = true;
            gVodFrameCount     = frameIdx;
            gTcpRemoteEof      = true;
            gArenaAvailToParse = 0;
            break;
        }

        recordBytes = 8UL + (unsigned long)payloadLen + (unsigned long)padBytes;
        if (gArenaAvailToParse < recordBytes) {
            /* Full frame payload has not arrived in the arena yet */
            break;
        }

        if (frameIdx >= gNextNeededFrame + (unsigned long)NF_INDEX_SLOTS) {
            /* Descriptor ring index full; wait for consumer to advance */
            break;
        }

        if (gArenaParse + recordBytes > gArenaCap) {
            unsigned long overflow = (gArenaParse + recordBytes) - gArenaCap;
            if (overflow <= NF_ARENA_TAIL) {
                NetFlimFastCopy(gArenaMem + gArenaCap, gArenaMem, overflow);
            }
        }

        if (frameIdx < gNextNeededFrame || payloadLen > NF_MAX_FRAME_BYTES) {
            if (gArenaUsed >= recordBytes) gArenaUsed -= recordBytes;
            else gArenaUsed = 0;
        } else {
            VodFrameDesc *d = &gDescRing[frameIdx % NF_INDEX_SLOTS];
            d->ready       = true;
            d->frameIdx    = frameIdx;
            d->dataPtr     = gArenaMem + gArenaParse + 8UL;
            d->payloadLen  = payloadLen;
            d->recordBytes = recordBytes;
            gBufferedCount++;
        }

        gArenaParse += recordBytes;
        if (gArenaParse >= gArenaCap) {
            gArenaParse -= gArenaCap;
        }
        gArenaAvailToParse -= recordBytes;
    }
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
            ScanArenaFrames();
            break;
        }
    }
}

static Boolean StartTcpVodConnection(unsigned long startFrame) {
    TCPiopb pb;
    char hostStr[32];
    OSErr err;

    CloseTcpStreamOnly();
    ResetRingSlots();

    if (!gMacTCPReady && !NetInit()) return false;
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

    FormatIPAddress(gServerIP, hostStr);
    sprintf(gVodHttpReq,
            "GET /api/netflim/stream/%s?profile=%s&start=%lu&maxBytes=%u%s HTTP/1.0\r\n"
            "Host: %s:%u\r\nUser-Agent: NetFlim/0.1\r\nConnection: close\r\n\r\n",
            gVodVideoId,
            gVodProfile,
            startFrame,
            (unsigned int)NF_MAX_FRAME_BYTES,
            gVodStripAudio ? "&stripAudio=1" : "",
            hostStr,
            (unsigned int)gServerHttpPort);

    gNextNeededFrame = startFrame;
    gVodEosReached   = false;
    gTcpRemoteEof    = false;
    gParsePhase      = kParseHttpHdr;
    gHdrAccumLen     = 0;
    gTcpRxArmed      = false;

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

Boolean NetVodOpen(const char *videoId, const char *profile, Boolean stripAudio, unsigned long startFrame) {
    NetVodClose();

    if (!gMacTCPReady && !NetInit()) return false;
    if (!gArenaMem || gArenaCap == 0 || gRingNumSlots <= 0 || !gTcpCreateBuf ||
        gServerIP == 0 || !videoId) {
        return false;
    }

    strncpy(gVodVideoId, videoId, sizeof(gVodVideoId) - 1);
    gVodVideoId[sizeof(gVodVideoId) - 1] = '\0';
    strncpy(gVodProfile, profile ? profile : NetResolveProfileName(), sizeof(gVodProfile) - 1);
    gVodProfile[sizeof(gVodProfile) - 1] = '\0';

    gVodStripAudio     = stripAudio;
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

    if (!StartTcpVodConnection(startFrame)) {
        gVodActive = false;
        return false;
    }

    NetVodPump();
    return true;
}

void NetVodClose(void) {
    CloseTcpStreamOnly();
    gVodActive      = false;
    gVodAckReceived = false;
    ResetRingSlots();
}

void NetVodPump(void) {
    int budget = 8;

    if (!gVodActive || gVodTcpStream == 0) return;

    if (gTcpConnState == kVodTcpOpening) {
        if (gVodOpenPB.ioResult == 1) return;
        if (gVodOpenPB.ioResult != noErr) {
            gLastTcpErr    = gVodOpenPB.ioResult;
            gVodOpenStatus = NF_OPEN_ERROR;
            gTcpRemoteEof  = true;
            gVodEosReached = true;
            gTcpConnState  = kVodTcpClosed;
            return;
        }

        gVodWds[0].length = (unsigned short)strlen(gVodHttpReq);
        gVodWds[0].ptr    = gVodHttpReq;
        gVodWds[1].length = 0;
        gVodWds[1].ptr    = NULL;

        memset(&gVodSendPB, 0, sizeof(gVodSendPB));
        gVodSendPB.ioCompletion = NULL;
        gVodSendPB.ioCRefNum = gMacTCPRef;
        gVodSendPB.csCode = TCPSend;
        gVodSendPB.tcpStream = gVodTcpStream;
        gVodSendPB.csParam.send.ulpTimeoutValue = 3;
        gVodSendPB.csParam.send.ulpTimeoutAction = 1;
        gVodSendPB.csParam.send.validityFlags = timeoutValue | timeoutAction;
        gVodSendPB.csParam.send.pushFlag = true;
        gVodSendPB.csParam.send.urgentFlag = false;
        gVodSendPB.csParam.send.wdsPtr = (Ptr)gVodWds;

        if (PBControlAsync((ParmBlkPtr)&gVodSendPB) != noErr) {
            gVodOpenStatus = NF_OPEN_ERROR;
            gTcpRemoteEof  = true;
            gVodEosReached = true;
            gTcpConnState  = kVodTcpClosed;
            return;
        }
        gTcpConnState = kVodTcpSending;
    }

    if (gTcpConnState == kVodTcpSending) {
        if (gVodSendPB.ioResult == 1) return;
        if (gVodSendPB.ioResult != noErr) {
            gLastTcpErr    = gVodSendPB.ioResult;
            gVodOpenStatus = NF_OPEN_ERROR;
            gTcpRemoteEof  = true;
            gVodEosReached = true;
            gTcpConnState  = kVodTcpClosed;
            return;
        }
        gTcpConnState = kVodTcpStreaming;
        ArmTcpRecv();
    }

    if (gTcpConnState != kVodTcpStreaming) return;

    if (gParsePhase == kParseFrames && gArenaAvailToParse >= 8UL && gBufferedCount < gRingNumSlots) {
        ScanArenaFrames();
    }

    while (budget-- > 0) {
        if (gTcpRxArmed && gVodRxPB.ioResult != 1) {
            OSErr res = gVodRxPB.ioResult;
            unsigned short got = (res == noErr || res == connectionClosing)
                ? gVodRxPB.csParam.receive.rcvBuffLen
                : 0;
            gTcpRxArmed = false;

            if (res == connectionClosing) {
                gTcpRemoteEof = true;
            } else if (res != noErr && res != -23016) {
                gLastTcpErr   = res;
                gTcpRemoteEof = true;
                if (!gVodAckReceived) {
                    gVodOpenStatus = NF_OPEN_ERROR;
                }
            }

            if (got > 0) {
                if (gParsePhase != kParseFrames) {
                    gArenaWrite += (unsigned long)got;
                    ParseInitialHandshake();
                } else {
                    gArenaWrite += (unsigned long)got;
                    if (gArenaWrite >= gArenaCap) {
                        gArenaWrite -= gArenaCap;
                    }
                    gArenaUsed         += (unsigned long)got;
                    gArenaAvailToParse += (unsigned long)got;
                    ScanArenaFrames();
                }
            }

            if (!gTcpRemoteEof && (res == noErr || res == -23016) && RingCanQueueMore()) {
                ArmTcpRecv();
            }

            if (gTcpRemoteEof && gVodAckReceived && !gVodEosReached) {
                if (gArenaAvailToParse < 8UL) {
                    gVodEosReached = true;
                }
            }
            if (gTcpRemoteEof && !gVodAckReceived && gVodOpenStatus == NF_OPEN_OK) {
                gVodOpenStatus = NF_OPEN_ERROR;
                gVodEosReached = true;
            }
        } else {
            break;
        }
    }

    if (!gTcpRxArmed && !gTcpRemoteEof && !gVodEosReached && RingCanQueueMore()) {
        ArmTcpRecv();
    }
}

void NetVodSeek(unsigned long targetFrame) {
    if (!gVodActive) return;
    if (!StartTcpVodConnection(targetFrame)) {
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
    if (!gVodActive) return false;
    if (gTcpConnState == kVodTcpOpening || gTcpConnState == kVodTcpSending) return true;
    if (!gVodAckReceived) return true;
    if (gTcpRxArmed && gVodRxPB.ioResult != 1) return true;
    if (gVodPaused || gVodEosReached || gTcpRemoteEof) return false;
    return (gBufferedCount < 16);
}

const unsigned char *NetVodPeekNextFrame(unsigned short *outBytes, unsigned long *outFrameIdx) {
    VodFrameDesc *d;
    if (!gVodActive || !gVodAckReceived || gRingNumSlots <= 0) return NULL;
    if (gVodFrameCount > 0 && gNextNeededFrame >= gVodFrameCount) return NULL;

    d = &gDescRing[gNextNeededFrame % NF_INDEX_SLOTS];
    if (!d->ready || d->frameIdx != gNextNeededFrame) {
        return NULL;
    }

    if (outBytes) *outBytes = d->payloadLen;
    if (outFrameIdx) *outFrameIdx = d->frameIdx;
    return d->dataPtr;
}

void NetVodConsumeNextFrame(void) {
    VodFrameDesc *d;
    if (!gVodActive || gRingNumSlots <= 0) return;

    d = &gDescRing[gNextNeededFrame % NF_INDEX_SLOTS];
    if (d->ready && d->frameIdx == gNextNeededFrame) {
        if (gArenaUsed >= d->recordBytes) gArenaUsed -= d->recordBytes;
        else gArenaUsed = 0;
        d->ready = false;
        if (gBufferedCount > 0) gBufferedCount--;
    }
    gNextNeededFrame++;

    if (gParsePhase == kParseFrames && gArenaAvailToParse >= 8UL && gBufferedCount < 8) {
        ScanArenaFrames();
    }
    if (!gTcpRxArmed && !gTcpRemoteEof && !gVodEosReached && RingCanQueueMore()) {
        ArmTcpRecv();
    }
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
