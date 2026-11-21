#include "network.h"
#include "BoltFastCompat.h"
#include <Devices.h>
#include <Memory.h>
#include <Events.h>
#include <LowMem.h>
#include <OSUtils.h>
#include <Sound.h>
#include <Notification.h>
#include <Gestalt.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "version.h"

Boolean         gNetReady            = false;
ip_addr         gOurIP               = 0;
static long     gOurNetMask          = (long)0xFFFFFF00;
static short    gMacTCPRef           = 0;

ip_addr         gServerIP            = 0;
unsigned short  gServerPort          = DEFAULT_HTTP_PORT;
Boolean         gServerConnected     = false;
Boolean         gChannelConnected    = false;

Boolean         gTransferActive      = false;
Boolean         gTransferIsUpload    = false;
char            gActiveTransferName[64] = "";
unsigned long   gActiveTransferBytes = 0;
unsigned long   gActiveTransferTotal = 0;
unsigned long   gTransferSpeedBps    = 0;
char            gCurrentTransferId[64] = "";
Rect            gBtnCancelTransferRect;
static Boolean  gCancelUpload        = false;

MacDropPeer     gOnlinePeers[MAX_MACDROP_PEERS];
short           gOnlinePeerCount     = 0;
char            gActiveUploadTargetName[48] = "MacCompanion Server";
char            gServerDisplayName[48]      = "MacCompanion Server";
char            gServerDisplaySub[48]       = "Modern Browser / Web UI";
static Boolean  gInitialPeersFetched = false;
static long     gCachedProcType      = gestalt68000;
static BoltDropContext gBoltCtx;
static BoltDropStream  gChannelBolt;

static void PumpBoltPollHook(void)
{
    if (gBoltCtx.hasBoltFast && gBoltCtx.api.Poll != NULL) {
        gBoltCtx.api.Poll();
    }
}

#define LISTEN_RCV_BUF_SIZE 32768
#define CHAN_RCV_BUF_SIZE   32768
#define FALLBACK_RCV_SIZE   16384
#define CHUNK_BUF_SIZE      32768
#define UI_REDRAW_TICKS     24

/* Shared Chunk Ping-Pong Buffers for all transfers (inbound reverse channel, inbound listener, outbound upload)
 * Explicit 4-byte alignment ensures safety on 68000 CPUs */
static unsigned char gChunkBufA[CHUNK_BUF_SIZE] __attribute__((aligned(4)));
static unsigned char gChunkBufB[CHUNK_BUF_SIZE] __attribute__((aligned(4)));

typedef struct {
    unsigned char *curBuf;
    long           curFill;
    long           writeQuantum;
} RxAccumState;

/* Passive listener on port 6803 */
static Ptr          gListenRcvBuf    = NULL;
static long         gListenRcvBufLen = 0;
static StreamPtr    gListenStream    = 0;
static TCPiopb      gPassivePB;
static Boolean      gListenerActive  = false;
static MacBinaryRx  gPassiveRx;

/* Reverse push channel to Node.js server */
typedef enum {
    CHAN_DISCONNECTED = 0,
    CHAN_CONNECTING,
    CHAN_CONNECTED,
    CHAN_STREAMING
} ChannelState;

static ChannelState gChannelState    = CHAN_DISCONNECTED;
static Ptr          gChannelRcvBuf   = NULL;
static long         gChannelRcvBufLen = 0;
static StreamPtr    gChannelStream   = 0;
static TCPiopb      gChanOpenPB;
static TCPiopb      gChanRcvPB;
static Boolean      gChanRcvPending  = false;
static unsigned long gLastChannelAttemptTick = 0;
static unsigned long gLastSiblingCheckTick   = 0;
static unsigned long gRxLastSpeedTicks       = 0;
static unsigned long gRxLastSpeedBytes       = 0;
static MacBinaryRx  gChannelRx;
static RxAccumState gChannelRxAccum;

/* Header buffer for HTTP parsing on reverse channel */
static char         gHttpHeaderBuf[1024];
static short        gHttpHeaderLen   = 0;
static Boolean      gHeaderParsed    = false;

static Boolean NetIsEmulatorSLIRP(void)
{
    return (gOurIP != 0 && (gOurIP & 0xFFFFFF00UL) == 0x0A000200UL);
}

static long GetDiskWriteQuantum(void)
{
    if (gCachedProcType >= gestalt68040 || NetIsEmulatorSLIRP() ||
        (gBoltCtx.hasBoltFast &&
         (gBoltCtx.api.link_tier == BOLT_TIER_SONIC || gBoltCtx.api.version >= 0x0031))) {
        return CHUNK_BUF_SIZE;
    }
    return 16384L;
}

static short GetSystemTaskInterval(void)
{
    return (gCachedProcType >= gestalt68020) ? 6 : 2;
}

static void SyncRxProgressGlobals(const MacBinaryRx *rx)
{
    if (gActiveTransferName[0] == '\0' && rx->fileName[0] != '\0') {
        snprintf(gActiveTransferName, sizeof(gActiveTransferName), "%s", rx->fileName);
    }
    gActiveTransferTotal = rx->dataLen + rx->rsrcLen;
    gActiveTransferBytes = rx->dataBytesWritten + rx->rsrcBytesWritten;
}

static void UpdateTransferSpeed(unsigned long currentBytes, unsigned long *ioLastTicks, unsigned long *ioLastBytes)
{
    unsigned long nowTicks = LMGetTicks();
    if ((nowTicks - *ioLastTicks) >= 20) {
        unsigned long elapsedTicks = nowTicks - *ioLastTicks;
        unsigned long bytesDelta = currentBytes - *ioLastBytes;
        gTransferSpeedBps = (bytesDelta * 60) / elapsedTicks;
        *ioLastTicks = nowTicks;
        *ioLastBytes = currentBytes;
    }
}

static Boolean PollTransferUIAndCancel(unsigned long *ioLastUiTick)
{
    extern void UpdateProgressUI(void);
    extern WindowRef gWindow;
    unsigned long nowTick = LMGetTicks();

    if ((nowTick - *ioLastUiTick) < UI_REDRAW_TICKS) {
        return false;
    }
    *ioLastUiTick = nowTick;
    UpdateProgressUI();

    if (GetEvQHdr()->qHead != NULL) {
        EventRecord evt;
        if (GetNextEvent(mDownMask | keyDownMask | autoKeyMask, &evt)) {
            if (evt.what == mouseDown) {
                WindowPtr win;
                short part = FindWindow(evt.where, &win);
                if (part == inContent && win == gWindow) {
                    Point localPt = evt.where;
                    SetPort(gWindow);
                    GlobalToLocal(&localPt);
                    if (PtInRect(localPt, &gBtnCancelTransferRect)) {
                        return true;
                    }
                }
            } else if (evt.what == keyDown || evt.what == autoKey) {
                char key = (char)(evt.message & charCodeMask);
                if (((evt.modifiers & cmdKey) && key == '.') || key == 27) {
                    return true;
                }
            }
        }
    }
    return false;
}

void FormatIP(ip_addr addr, char *outBuf)
{
    unsigned char b0 = (unsigned char)((addr >> 24) & 0xFF);
    unsigned char b1 = (unsigned char)((addr >> 16) & 0xFF);
    unsigned char b2 = (unsigned char)((addr >> 8)  & 0xFF);
    unsigned char b3 = (unsigned char)(addr & 0xFF);
    sprintf(outBuf, "%u.%u.%u.%u", b0, b1, b2, b3);
}

Boolean ParseIPAddress(const char *str, ip_addr *outAddr)
{
    unsigned int a = 0, b = 0, c = 0, d = 0;
    const char *p = str;
    int part = 0;
    unsigned int val = 0;
    Boolean hasDigit = false;

    if (!str || !outAddr) return false;
    while (*p == ' ') p++;
    if (*p == '\0') return false;

    while (*p) {
        if (*p >= '0' && *p <= '9') {
            val = val * 10 + (unsigned int)(*p - '0');
            if (val > 255) return false;
            hasDigit = true;
        } else if (*p == '.') {
            if (!hasDigit || part >= 3) return false;
            if (part == 0) a = val;
            else if (part == 1) b = val;
            else if (part == 2) c = val;
            part++;
            val = 0;
            hasDigit = false;
        } else if (*p == ' ' || *p == '\r' || *p == '\n') {
            break;
        } else {
            return false;
        }
        p++;
    }
    if (!hasDigit || part != 3) return false;
    d = val;

    *outAddr = ((ip_addr)a << 24) | ((ip_addr)b << 16) | ((ip_addr)c << 8) | (ip_addr)d;
    return true;
}

static void URLEncode(const char *src, char *dst, short dstMax)
{
    short out = 0;
    while (*src && out < dstMax - 4) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[out++] = c;
        } else if (c == ' ') {
            dst[out++] = '%';
            dst[out++] = '2';
            dst[out++] = '0';
        } else {
            sprintf(&dst[out], "%%%02X", c);
            out += 3;
        }
        src++;
    }
    dst[out] = '\0';
}

static void TriggerTransferNotification(const char *fileName)
{
    ShowTransferNotifyAlert(fileName, false);
}

static void TriggerUploadNotification(const char *fileName)
{
    ShowTransferNotifyAlert(fileName, true);
}

void NetRefreshOurIP(void)
{
    GetAddrParamBlock addrPB;
    OSErr err;

    if (gMacTCPRef == 0) return;

    memset(&addrPB, 0, sizeof(addrPB));
    addrPB.ioCRefNum = gMacTCPRef;
    addrPB.csCode    = ipctlGetAddr;
    err = PBControlSync((ParmBlkPtr)&addrPB);
    if (err == noErr && addrPB.ourAddress != 0) {
        gOurIP      = addrPB.ourAddress;
        gOurNetMask = addrPB.ourNetMask ? addrPB.ourNetMask : (long)0xFFFFFF00;
    }
}

Boolean NetInit(void)
{
    OSErr err;

    if (gNetReady) return true;

    if (Gestalt(gestaltProcessorType, &gCachedProcType) != noErr) {
        gCachedProcType = gestalt68000;
    }

    err = OpenDriver("\p.ipp", &gMacTCPRef);
    if (err != noErr) {
        gNetReady = false;
        return false;
    }

    BoltDrop_Probe(&gBoltCtx, gMacTCPRef);

    gOurIP      = 0;
    gOurNetMask = (long)0xFFFFFF00;
    NetRefreshOurIP();

    gNetReady = true;
    return true;
}

static void ResetServerPeerState(Boolean notifyUI)
{
    Boolean changed = (gOnlinePeerCount != 0 ||
                       strcmp(gServerDisplayName, "MacCompanion Server") != 0 ||
                       strcmp(gServerDisplaySub, "Modern Browser / Web UI") != 0);
    gOnlinePeerCount = 0;
    memset(gOnlinePeers, 0, sizeof(gOnlinePeers));
    strcpy(gServerDisplayName, "MacCompanion Server");
    strcpy(gServerDisplaySub, "Modern Browser / Web UI");
    gInitialPeersFetched = false;
    if (notifyUI && changed) {
        extern void UpdateProgressUI(void);
        UpdateProgressUI();
    }
}

void NetSetServer(ip_addr ip, unsigned short port)
{
    unsigned short targetPort = port ? port : DEFAULT_HTTP_PORT;
    if (gServerIP != ip || gServerPort != targetPort) {
        ResetServerPeerState(false);
    }
    gServerIP        = ip;
    gServerPort      = targetPort;
    gServerConnected = true;
    gSavedServerIP   = ip;
    gSavedServerPort = gServerPort;
    SavePreferences();
}

long NetHttpGet(const char *path, char *outBody, long maxLen)
{
    TCPiopb pb;
    StreamPtr stream;
    Ptr tcpRcvBuf;
    char req[512];
    char hostStr[32];
    wdsEntry wds[2];
    OSErr err;
    long totalRead = 0;
    Boolean headerStripped = false;
    unsigned long startTicks;

    if (!NetInit() || gServerIP == 0) return -1;

    tcpRcvBuf = NewPtr(4096);
    if (!tcpRcvBuf) return -1;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                 = gMacTCPRef;
    pb.csCode                    = TCPCreate;
    pb.csParam.create.rcvBuff    = tcpRcvBuf;
    pb.csParam.create.rcvBuffLen = 4096;
    pb.csParam.create.notifyProc = NULL;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
        DisposePtr(tcpRcvBuf);
        return -1;
    }
    stream = pb.tcpStream;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                        = gMacTCPRef;
    pb.csCode                           = TCPActiveOpen;
    pb.tcpStream                        = stream;
    pb.csParam.open.ulpTimeoutValue     = 3;
    pb.csParam.open.ulpTimeoutAction    = 1;
    pb.csParam.open.validityFlags       = timeoutValue | timeoutAction;
    pb.csParam.open.commandTimeoutValue = 3;
    pb.csParam.open.remoteHost          = gServerIP;
    pb.csParam.open.remotePort          = gServerPort;
    pb.csParam.open.localPort           = 0;

    err = PBControlAsync((ParmBlkPtr)&pb);
    startTicks = TickCount();
    while (pb.ioResult > 0 && (TickCount() - startTicks) < 180) {
        SystemTask();
    }
    if (pb.ioResult != noErr) {
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode    = TCPAbort;
        pb.tcpStream = stream;
        PBControlSync((ParmBlkPtr)&pb);

        pb.csCode    = TCPRelease;
        PBControlSync((ParmBlkPtr)&pb);
        DisposePtr(tcpRcvBuf);
        return -1;
    }

    FormatIP(gServerIP, hostStr);
    sprintf(req, "GET %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: MacDrop-68k\r\nConnection: close\r\n\r\n",
            path, hostStr, gServerPort);

    wds[0].length = (unsigned short)strlen(req);
    wds[0].ptr    = (Ptr)req;
    wds[1].length = 0;
    wds[1].ptr    = NULL;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                        = gMacTCPRef;
    pb.csCode                           = TCPSend;
    pb.tcpStream                        = stream;
    pb.csParam.send.ulpTimeoutValue     = 3;
    pb.csParam.send.ulpTimeoutAction    = 1;
    pb.csParam.send.validityFlags       = timeoutValue | timeoutAction;
    pb.csParam.send.pushFlag            = true;
    pb.csParam.send.urgentFlag          = false;
    pb.csParam.send.wdsPtr              = (Ptr)wds;
    PBControlSync((ParmBlkPtr)&pb);

    /* Read response (accept connectionClosing since short HTTP/1.0 replies often arrive with FIN) */
    startTicks = LMGetTicks();
    while (totalRead < (maxLen - 1) && (LMGetTicks() - startTicks) < 180) {
        short chunkAsk = (short)((maxLen - 1 - totalRead) > 1024 ? 1024 : (maxLen - 1 - totalRead));
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum                           = gMacTCPRef;
        pb.csCode                              = TCPRcv;
        pb.tcpStream                           = stream;
        pb.csParam.receive.commandTimeoutValue = 2;
        pb.csParam.receive.rcvBuff             = outBody + totalRead;
        pb.csParam.receive.rcvBuffLen          = chunkAsk;
        err = PBControlSync((ParmBlkPtr)&pb);
        if ((err == noErr || err == connectionClosing) && pb.csParam.receive.rcvBuffLen > 0) {
            totalRead += pb.csParam.receive.rcvBuffLen;
            outBody[totalRead] = '\0';
            if (!headerStripped) {
                char *delim = strstr(outBody, "\r\n\r\n");
                short delimLen = 4;
                if (!delim) {
                    delim = strstr(outBody, "\n\n");
                    delimLen = 2;
                }
                if (delim) {
                    long headerBytes = (delim - outBody) + delimLen;
                    long rem = totalRead - headerBytes;
                    if (rem > 0) {
                        memmove(outBody, outBody + headerBytes, rem);
                    }
                    totalRead = rem;
                    outBody[totalRead] = '\0';
                    headerStripped = true;
                }
            }
            if (err == connectionClosing || (headerStripped && totalRead > 0)) {
                break;
            }
        } else {
            break;
        }
    }

    if (gOurIP == 0) {
        NetRefreshOurIP();
    }

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode    = TCPAbort;
    pb.tcpStream = stream;
    PBControlSync((ParmBlkPtr)&pb);

    pb.csCode    = TCPRelease;
    PBControlSync((ParmBlkPtr)&pb);
    DisposePtr(tcpRcvBuf);

    return totalRead;
}

Boolean NetVerifyServer(ip_addr ip, unsigned short port)
{
    char buf[256];
    ip_addr oldIP = gServerIP;
    unsigned short oldPort = gServerPort;
    long n;

    gServerIP = ip;
    gServerPort = port ? port : DEFAULT_HTTP_PORT;

    n = NetHttpGet("/api/ping", buf, sizeof(buf));
    if (n > 0 && strncmp(buf, "MACCOMPANION_SERVER|", 20) == 0) {
        char *endLn = strpbrk(buf, "\r\n");
        if (endLn) *endLn = '\0';
        NetSetServer(ip, port);
        if (strstr(buf, "|macdrop") != NULL) {
            strcpy(gServerDisplayName, "MacDrop X");
            strcpy(gServerDisplaySub, "Modern Macintosh");
        }
        return true;
    }

    gServerIP = oldIP;
    gServerPort = oldPort;
    return false;
}

void NetSendTransferAck(const char *transferId, const char *status, unsigned long bytesReceived)
{
    char path[256];
    char reply[128];
    char encId[128];
    if (!transferId || transferId[0] == '\0') return;
    URLEncode(gClientID, encId, sizeof(encId));
    snprintf(path, sizeof(path), "/api/macdrop/ack?id=%s&transfer_id=%s&status=%s&bytes=%lu",
             encId, transferId, status ? status : "OK", bytesReceived);
    NetHttpGet(path, reply, sizeof(reply));
}

extern void UpdateProgressUI(void);

void NetCancelActiveTransfer(void)
{
    if (!gTransferActive) return;

    if (gTransferIsUpload) {
        gCancelUpload = true;
    } else {
        if (gChannelState == CHAN_STREAMING) {
            char cancelId[64];
            strncpy(cancelId, gCurrentTransferId, sizeof(cancelId) - 1);
            cancelId[sizeof(cancelId) - 1] = '\0';
            gCurrentTransferId[0] = '\0';
            if (gChannelBolt.stream != 0) {
                BoltDrop_EndTransfer(&gChannelBolt);
                gChannelBolt.stream = 0;
            }
            MacBinaryRx_SetWaitPollHook(NULL);
            MacBinaryRx_Abort(&gChannelRx);
            NetStopChannel();
            gLastChannelAttemptTick = 0; /* prompt immediate reconnect for next transfer */
            if (cancelId[0] != '\0') {
                NetSendTransferAck(cancelId, "CANCELLED", 0);
            }
        } else {
            MacBinaryRx_Abort(&gPassiveRx);
            NetStopListener();
            NetStartListener();
        }
        gTransferActive   = false;
        gTransferIsUpload = false;
        gActiveTransferBytes = 0;
        gActiveTransferTotal = 0;
        gActiveTransferName[0] = '\0';
        SysBeep(1);
        UpdateProgressUI();
    }
}

static void ParsePeersHeaderString(const char *rawHdr)
{
    MacDropPeer newPeers[MAX_MACDROP_PEERS];
    short newCount = 0;
    char buf[512];
    char *tok;
    extern void UpdateProgressUI(void);

    memset(newPeers, 0, sizeof(newPeers));

    if (rawHdr && rawHdr[0] != '\0' && strncmp(rawHdr, "NONE", 4) != 0) {
        strncpy(buf, rawHdr, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        tok = buf;
        while (tok && *tok && newCount < MAX_MACDROP_PEERS) {
            char *nextSemi = strchr(tok, ';');
            char *p1, *p2;
            if (nextSemi) {
                *nextSemi = '\0';
            }
            p1 = strchr(tok, '|');
            if (p1) {
                *p1 = '\0';
                p2 = strchr(p1 + 1, '|');
                if (p2) {
                    *p2 = '\0';
                }
                if (tok[0] != '\0' && strcmp(tok, gClientID) != 0 && strcmp(tok, "SERVER") != 0) {
                    strncpy(newPeers[newCount].id, tok, sizeof(newPeers[0].id) - 1);
                    strncpy(newPeers[newCount].name, p1 + 1, sizeof(newPeers[0].name) - 1);
                    if (p2 && *(p2 + 1)) {
                        strncpy(newPeers[newCount].machine, p2 + 1, sizeof(newPeers[0].machine) - 1);
                    } else {
                        strcpy(newPeers[newCount].machine, "Macintosh 68k");
                    }
                    newCount++;
                }
            }
            tok = nextSemi ? (nextSemi + 1) : NULL;
        }
    }

    if (newCount != gOnlinePeerCount || memcmp(newPeers, gOnlinePeers, sizeof(newPeers)) != 0) {
        gOnlinePeerCount = newCount;
        memcpy(gOnlinePeers, newPeers, sizeof(gOnlinePeers));
        UpdateProgressUI();
    }
}

Boolean NetRefreshPeers(void)
{
    char path[256];
    char body[1024];
    char encId[96];
    char ourIpStr[32];
    long n;
    MacDropPeer newPeers[MAX_MACDROP_PEERS];
    short newCount = 0;
    char *line;

    if (!NetInit() || gServerIP == 0) return false;
    if (gOurIP == 0) NetRefreshOurIP();

    FormatIP(gOurIP, ourIpStr);
    URLEncode(gClientID, encId, sizeof(encId));
    snprintf(path, sizeof(path), "/api/macdrop/peers?id=%s&ip=%s", encId, ourIpStr);

    n = NetHttpGet(path, body, sizeof(body));
    if (n <= 0) {
        ResetServerPeerState(true);
        return false;
    }

    memset(newPeers, 0, sizeof(newPeers));
    line = body;
    while (line && *line) {
        char *nextLine = strchr(line, '\n');
        if (nextLine) {
            *nextLine = '\0';
        }
        /* Strip trailing \r */
        {
            short l = (short)strlen(line);
            if (l > 0 && line[l - 1] == '\r') line[l - 1] = '\0';
        }
        if (strncmp(line, "SERVER|", 7) == 0) {
            char *sName = line + 7;
            char *sSub = strchr(sName, '|');
            if (sSub) {
                *sSub = '\0';
                sSub++;
            }
            if (sName[0] != '\0') {
                strncpy(gServerDisplayName, sName, sizeof(gServerDisplayName) - 1);
                gServerDisplayName[sizeof(gServerDisplayName) - 1] = '\0';
            }
            if (sSub && sSub[0] != '\0') {
                strncpy(gServerDisplaySub, sSub, sizeof(gServerDisplaySub) - 1);
                gServerDisplaySub[sizeof(gServerDisplaySub) - 1] = '\0';
            }
        } else if (strncmp(line, "PEER|", 5) == 0 && newCount < MAX_MACDROP_PEERS) {
            char *pId = line + 5;
            char *pName = strchr(pId, '|');
            if (pName) {
                char *pMach;
                *pName = '\0';
                pName++;
                pMach = strchr(pName, '|');
                if (pMach) {
                    *pMach = '\0';
                    pMach++;
                }
                if (pId[0] != '\0' && strcmp(pId, gClientID) != 0 && strcmp(pId, "SERVER") != 0) {
                    strncpy(newPeers[newCount].id, pId, sizeof(newPeers[0].id) - 1);
                    strncpy(newPeers[newCount].name, pName, sizeof(newPeers[0].name) - 1);
                    if (pMach && pMach[0]) {
                        strncpy(newPeers[newCount].machine, pMach, sizeof(newPeers[0].machine) - 1);
                    } else {
                        strcpy(newPeers[newCount].machine, "Macintosh 68k");
                    }
                    newCount++;
                }
            }
        }
        line = nextLine ? (nextLine + 1) : NULL;
    }

    gOnlinePeerCount = newCount;
    memcpy(gOnlinePeers, newPeers, sizeof(gOnlinePeers));
    gInitialPeersFetched = true;
    return true;
}

Boolean NetSendFile(const FSSpec *spec)
{
    return NetSendFileToTarget(spec, "SERVER", "MacCompanion Server");
}

Boolean NetSendFileToTarget(const FSSpec *spec, const char *targetId, const char *targetName)
{
    MacBinaryTx tx;
    TCPiopb pb;
    StreamPtr stream = 0;
    Ptr tcpRcvBuf = NULL;
    char req[1024];
    char hostStr[32];
    char ourIpStr[32];
    char encId[128];
    char encName[128];
    char encTarget[128];
    const char *cleanTarget = (targetId && targetId[0]) ? targetId : "SERVER";
    const char *cleanTargetName = (targetName && targetName[0]) ? targetName : "MacCompanion Server";
    OSErr err;
    unsigned long startTicks, lastSpeedTicks, lastUiTicks, lastSysTicks, lastSpeedBytes;
    unsigned long totalSize;
    long chunkBufSize;
    short sysTickInterval;
    extern void UpdateProgressUI(void);

    /* Pipelined double-buffering structures */
    static TCPiopb sendPB[2];
    static wdsEntry sendWds[2][3];
    static Boolean pbInFlight[2] = {false, false};
    BoltDropStream txBolt;
    unsigned char *chunkBuffers[2] = {gChunkBufA, gChunkBufB};
    long bytesInBuf[2] = {0, 0};
    int curSlot = 0;

    memset(&txBolt, 0, sizeof(txBolt));

    if (!spec || !NetInit() || gServerIP == 0) return false;

    /* Initialize MacBinaryTx encoder */
    err = MacBinaryTx_Init(&tx, spec);
    if (err != noErr) {
        return false;
    }

    totalSize = 128 + tx.dataBytesTarget + tx.rsrcBytesTarget;
    chunkBufSize = (gCachedProcType >= gestalt68020) ? 32768L : 16384L;
    sysTickInterval = GetSystemTaskInterval();

    /* Free Slot 1 (reverse channel) so upload runs inside BoltTcp's pre-allocated fast pool (i < 2) */
    NetStopChannel();

    tcpRcvBuf = NewPtr(16384);
    if (!tcpRcvBuf) {
        tcpRcvBuf = NewPtr(4096);
        if (!tcpRcvBuf) {
            MacBinaryTx_Abort(&tx);
            gLastChannelAttemptTick = 0;
            NetConnectChannel();
            return false;
        }
    }

    /* Create TCP stream */
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                 = gMacTCPRef;
    pb.csCode                    = TCPCreate;
    pb.csParam.create.rcvBuff    = tcpRcvBuf;
    pb.csParam.create.rcvBuffLen = GetPtrSize(tcpRcvBuf);
    pb.csParam.create.notifyProc = NULL;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
        DisposePtr(tcpRcvBuf);
        MacBinaryTx_Abort(&tx);
        gLastChannelAttemptTick = 0;
        NetConnectChannel();
        return false;
    }
    stream = pb.tcpStream;

    if (gOurIP == 0) {
        NetRefreshOurIP();
    }

    /* Open connection to server */
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                        = gMacTCPRef;
    pb.csCode                           = TCPActiveOpen;
    pb.tcpStream                        = stream;
    pb.csParam.open.ulpTimeoutValue     = 10;
    pb.csParam.open.ulpTimeoutAction    = 1;
    pb.csParam.open.validityFlags       = timeoutValue | timeoutAction;
    pb.csParam.open.commandTimeoutValue = 6;
    pb.csParam.open.remoteHost          = gServerIP;
    pb.csParam.open.remotePort          = gServerPort;
    pb.csParam.open.localPort           = 0;

    err = PBControlAsync((ParmBlkPtr)&pb);
    startTicks = LMGetTicks();
    lastSysTicks = startTicks;
    while (pb.ioResult > 0 && (LMGetTicks() - startTicks) < 360) {
        unsigned long t = LMGetTicks();
        if ((t - lastSysTicks) >= (unsigned long)sysTickInterval) {
            lastSysTicks = t;
            SystemTask();
        }
    }
    if (pb.ioResult != noErr) {
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode    = TCPAbort;
        pb.tcpStream = stream;
        PBControlSync((ParmBlkPtr)&pb);
        pb.csCode    = TCPRelease;
        PBControlSync((ParmBlkPtr)&pb);
        DisposePtr(tcpRcvBuf);
        MacBinaryTx_Abort(&tx);
        gLastChannelAttemptTick = 0;
        NetConnectChannel();
        return false;
    }

    FormatIP(gServerIP, hostStr);
    FormatIP(gOurIP, ourIpStr);
    URLEncode(gClientID, encId, sizeof(encId));
    URLEncode(gClientName, encName, sizeof(encName));
    URLEncode(cleanTarget, encTarget, sizeof(encTarget));

    sprintf(req,
        "POST /api/macdrop/inbound/upload?id=%s&name=%s&ip=%s&target_id=%s HTTP/1.0\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: MacDrop-68k\r\n"
        "Content-Type: application/x-macbinary\r\n"
        "Content-Length: %lu\r\n"
        "X-Client-Id: %s\r\n"
        "X-Client-Name: %s\r\n"
        "X-Client-IP: %s\r\n"
        "X-Target-Id: %s\r\n"
        "X-Filename: %s\r\n"
        "Connection: close\r\n\r\n",
        encId, encName, ourIpStr, encTarget, hostStr, gServerPort, totalSize,
        gClientID, gClientName, ourIpStr, cleanTarget, tx.fileName);

    /* Gather-send HTTP header + 128-byte MacBinary II header together so FSRead starts at offset 0 (0 mod 512) */
    sendWds[0][0].length = (unsigned short)strlen(req);
    sendWds[0][0].ptr    = (Ptr)req;
    sendWds[0][1].length = 128;
    sendWds[0][1].ptr    = (Ptr)tx.headerBuf;
    sendWds[0][2].length = 0;
    sendWds[0][2].ptr    = NULL;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                        = gMacTCPRef;
    pb.csCode                           = TCPSend;
    pb.tcpStream                        = stream;
    pb.csParam.send.ulpTimeoutValue     = 10;
    pb.csParam.send.ulpTimeoutAction    = 1;
    pb.csParam.send.validityFlags       = timeoutValue | timeoutAction;
    pb.csParam.send.pushFlag            = true;
    pb.csParam.send.urgentFlag          = false;
    pb.csParam.send.wdsPtr              = (Ptr)sendWds[0];
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
        goto abort_transfer;
    }

    tx.headerBytesSent = 128;
    if (tx.dataBytesTarget > 0)      tx.state = TX_DATA;
    else if (tx.rsrcBytesTarget > 0) tx.state = TX_RSRC;
    else                             tx.state = TX_COMPLETE;

    /* Set global transfer state */
    gTransferActive      = true;
    gTransferIsUpload    = true;
    gCancelUpload        = false;
    gActiveTransferBytes = 128;
    gActiveTransferTotal = totalSize;
    gTransferSpeedBps    = 0;
    strncpy(gActiveTransferName, tx.fileName, sizeof(gActiveTransferName) - 1);
    gActiveTransferName[sizeof(gActiveTransferName) - 1] = '\0';
    strncpy(gActiveUploadTargetName, cleanTargetName, sizeof(gActiveUploadTargetName) - 1);
    gActiveUploadTargetName[sizeof(gActiveUploadTargetName) - 1] = '\0';

    OnTransferStarted(true);

    lastSpeedTicks = LMGetTicks();
    lastUiTicks    = lastSpeedTicks;
    lastSysTicks   = lastSpeedTicks;
    lastSpeedBytes = gActiveTransferBytes;

    UpdateProgressUI();

    /* Prime Slot 0 from disk (512-byte sector aligned, 4-byte buffer aligned) */
    pbInFlight[0] = false;
    pbInFlight[1] = false;
    curSlot = 0;
    bytesInBuf[0] = MacBinaryTx_ReadBytes(&tx, chunkBuffers[0], chunkBufSize);

    if (gBoltCtx.hasBoltFast) {
        BoltDrop_BeginTransfer(&txBolt, &gBoltCtx, stream, true /* tickless */, true /* skipRxCsum */);

        while (bytesInBuf[curSlot] > 0) {
            BoltDropSendJob sendJob;
            Boolean sendDone = false;
            Boolean aborted = false;
            uint32_t lastReportedOff = 0;
            unsigned long txProgressTicks = LMGetTicks();
            int nextSlot = 1 - curSlot;

            if (gCancelUpload) {
                BoltDrop_EndTransfer(&txBolt);
                txBolt.stream = 0;
                goto abort_transfer;
            }

            BoltDrop_BeginSend(&sendJob, &txBolt, chunkBuffers[curSlot],
                               (uint32_t)bytesInBuf[curSlot], (tx.state == TX_COMPLETE));

            /* Push initial slice(s) into BoltTcp tx_buf before blocking on next disk read */
            if (BoltDrop_PollSend(&sendJob, &sendDone) != noErr) {
                BoltDrop_EndTransfer(&txBolt);
                txBolt.stream = 0;
                goto abort_transfer;
            }
            if (sendJob.off > lastReportedOff) {
                gActiveTransferBytes += (unsigned long)(sendJob.off - lastReportedOff);
                lastReportedOff = sendJob.off;
                txProgressTicks = LMGetTicks();
            }

            /* Overlap next disk read into alternate ping-pong buffer */
            if (tx.state != TX_COMPLETE && tx.state != TX_ERROR) {
                bytesInBuf[nextSlot] = MacBinaryTx_ReadBytes(&tx, chunkBuffers[nextSlot], chunkBufSize);
                if (bytesInBuf[nextSlot] < 0) {
                    BoltDrop_EndTransfer(&txBolt);
                    txBolt.stream = 0;
                    goto abort_transfer;
                }
            } else {
                bytesInBuf[nextSlot] = 0;
            }

            while (!sendDone) {
                OSErr sErr = BoltDrop_PollSend(&sendJob, &sendDone);
                if (sErr != noErr) {
                    aborted = true;
                    break;
                }
                if (sendJob.off > lastReportedOff) {
                    gActiveTransferBytes += (unsigned long)(sendJob.off - lastReportedOff);
                    lastReportedOff = sendJob.off;
                    txProgressTicks = LMGetTicks();
                }
                UpdateTransferSpeed(gActiveTransferBytes, &lastSpeedTicks, &lastSpeedBytes);

                if ((LMGetTicks() - lastUiTicks) >= UI_REDRAW_TICKS) {
                    BoltDrop_YieldTickless(&txBolt, false);
                    SystemTask();
                    if (PollTransferUIAndCancel(&lastUiTicks) || gCancelUpload) {
                        aborted = true;
                        break;
                    }
                    BoltDrop_YieldTickless(&txBolt, true);
                } else if (gCancelUpload || (LMGetTicks() - txProgressTicks) > 1200) {
                    aborted = true;
                    break;
                }
            }

            if (aborted) {
                BoltDrop_EndTransfer(&txBolt);
                txBolt.stream = 0;
                goto abort_transfer;
            }

            if (sendJob.off > lastReportedOff) {
                gActiveTransferBytes += (unsigned long)(sendJob.off - lastReportedOff);
            }
            UpdateTransferSpeed(gActiveTransferBytes, &lastSpeedTicks, &lastSpeedBytes);

            if (bytesInBuf[nextSlot] == 0 && tx.state == TX_COMPLETE) {
                BoltDrop_YieldTickless(&txBolt, false);
                UpdateProgressUI();
                BoltDrop_YieldTickless(&txBolt, true);
            }

            curSlot = nextSlot;
        }

        BoltDrop_EndTransfer(&txBolt);
        txBolt.stream = 0;
    } else {
        if (bytesInBuf[0] > 0) {
            sendWds[0][0].length = (unsigned short)bytesInBuf[0];
            sendWds[0][0].ptr    = (Ptr)chunkBuffers[0];
            sendWds[0][1].length = 0;
            sendWds[0][1].ptr    = NULL;

            memset(&sendPB[0], 0, sizeof(sendPB[0]));
            sendPB[0].ioCRefNum                     = gMacTCPRef;
            sendPB[0].csCode                        = TCPSend;
            sendPB[0].tcpStream                     = stream;
            sendPB[0].csParam.send.ulpTimeoutValue  = 15;
            sendPB[0].csParam.send.ulpTimeoutAction = 1;
            sendPB[0].csParam.send.validityFlags    = timeoutValue | timeoutAction;
            sendPB[0].csParam.send.pushFlag         = true;
            sendPB[0].csParam.send.urgentFlag       = false;
            sendPB[0].csParam.send.wdsPtr           = (Ptr)sendWds[0];

            err = PBControlAsync((ParmBlkPtr)&sendPB[0]);
            if (err == noErr || sendPB[0].ioResult == inProgress) {
                pbInFlight[0] = true;
            } else {
                goto abort_transfer;
            }
        }

        /* Pipelined double-buffered streaming loop */
        while (pbInFlight[0] || pbInFlight[1]) {
            int nextSlot = 1 - curSlot;

            if (gCancelUpload) {
                goto abort_transfer;
            }

            /* Read next chunk from disk into nextSlot and queue it with MacTCP immediately */
            if (!pbInFlight[nextSlot] && tx.state != TX_COMPLETE && tx.state != TX_ERROR) {
                bytesInBuf[nextSlot] = MacBinaryTx_ReadBytes(&tx, chunkBuffers[nextSlot], chunkBufSize);
                if (bytesInBuf[nextSlot] > 0) {
                    sendWds[nextSlot][0].length = (unsigned short)bytesInBuf[nextSlot];
                    sendWds[nextSlot][0].ptr    = (Ptr)chunkBuffers[nextSlot];
                    sendWds[nextSlot][1].length = 0;
                    sendWds[nextSlot][1].ptr    = NULL;

                    memset(&sendPB[nextSlot], 0, sizeof(sendPB[nextSlot]));
                    sendPB[nextSlot].ioCRefNum                     = gMacTCPRef;
                    sendPB[nextSlot].csCode                        = TCPSend;
                    sendPB[nextSlot].tcpStream                     = stream;
                    sendPB[nextSlot].csParam.send.ulpTimeoutValue  = 15;
                    sendPB[nextSlot].csParam.send.ulpTimeoutAction = 1;
                    sendPB[nextSlot].csParam.send.validityFlags    = timeoutValue | timeoutAction;
                    sendPB[nextSlot].csParam.send.pushFlag         = true;
                    sendPB[nextSlot].csParam.send.urgentFlag       = false;
                    sendPB[nextSlot].csParam.send.wdsPtr           = (Ptr)sendWds[nextSlot];

                    err = PBControlAsync((ParmBlkPtr)&sendPB[nextSlot]);
                    if (err == noErr || sendPB[nextSlot].ioResult == inProgress) {
                        pbInFlight[nextSlot] = true;
                    } else {
                        goto abort_transfer;
                    }
                }
            }

            /* Wait for curSlot transmission to complete */
            if (pbInFlight[curSlot]) {
                unsigned long sendStartTicks = LMGetTicks();
                while (sendPB[curSlot].ioResult > 0) {
                    unsigned long t = LMGetTicks();
                    if (gCancelUpload || (t - sendStartTicks) > 900) {
                        goto abort_transfer;
                    }
                    if ((t - lastSysTicks) >= (unsigned long)sysTickInterval) {
                        lastSysTicks = t;
                        SystemTask();
                    }
                }

                if (sendPB[curSlot].ioResult != noErr) {
                    goto abort_transfer;
                }

                pbInFlight[curSlot] = false;
                gActiveTransferBytes += (unsigned long)bytesInBuf[curSlot];
            }

            UpdateTransferSpeed(gActiveTransferBytes, &lastSpeedTicks, &lastSpeedBytes);

            if (PollTransferUIAndCancel(&lastUiTicks)) {
                goto abort_transfer;
            }
            if (!pbInFlight[nextSlot] && tx.state == TX_COMPLETE) {
                UpdateProgressUI();
            }

            curSlot = nextSlot;
        }
    }

    if (tx.state == TX_ERROR) {
        goto abort_transfer;
    }

    MacBinaryTx_Finish(&tx);

    /* Read server response */
    {
        char respBuf[512];
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum                           = gMacTCPRef;
        pb.csCode                              = TCPRcv;
        pb.tcpStream                           = stream;
        pb.csParam.receive.commandTimeoutValue = 5;
        pb.csParam.receive.rcvBuff             = respBuf;
        pb.csParam.receive.rcvBuffLen          = sizeof(respBuf) - 1;
        PBControlSync((ParmBlkPtr)&pb);
    }

    /* Clean close TCP */
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                        = gMacTCPRef;
    pb.csCode                           = TCPClose;
    pb.tcpStream                        = stream;
    pb.csParam.close.ulpTimeoutValue     = 5;
    pb.csParam.close.ulpTimeoutAction    = 1;
    pb.csParam.close.validityFlags       = timeoutValue | timeoutAction;
    PBControlSync((ParmBlkPtr)&pb);

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode    = TCPRelease;
    pb.tcpStream = stream;
    PBControlSync((ParmBlkPtr)&pb);

    DisposePtr(tcpRcvBuf);

    gTransferActive   = false;
    gTransferIsUpload = false;

    /* Refresh reverse push channel immediately so client stays online after upload */
    gLastChannelAttemptTick = 0;
    NetConnectChannel();

    TriggerUploadNotification(tx.fileName);
    UpdateProgressUI();
    return true;

abort_transfer:
    if (txBolt.stream != 0) {
        BoltDrop_EndTransfer(&txBolt);
        txBolt.stream = 0;
    }
    MacBinaryTx_Abort(&tx);
    if (stream != 0) {
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode    = TCPAbort;
        pb.tcpStream = stream;
        PBControlSync((ParmBlkPtr)&pb);

        if (pbInFlight[0]) {
            while (sendPB[0].ioResult > 0) {}
            pbInFlight[0] = false;
        }
        if (pbInFlight[1]) {
            while (sendPB[1].ioResult > 0) {}
            pbInFlight[1] = false;
        }

        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode    = TCPRelease;
        pb.tcpStream = stream;
        PBControlSync((ParmBlkPtr)&pb);
    }

    if (tcpRcvBuf) DisposePtr(tcpRcvBuf);

    gTransferActive   = false;
    gTransferIsUpload = false;

    gLastChannelAttemptTick = 0;
    NetConnectChannel();
    UpdateProgressUI();
    return false;
}

Boolean NetDiscoverServer(void)
{
    UDPiopb pb;
    Ptr udpRcvBuf;
    StreamPtr udpStream;
    wdsEntry wds[2];
    const char *magic = "MACCOMPANION_DISCOVER";
    ip_addr targets[4];
    short numTargets = 0;
    short i;
    Boolean found = false;

    if (!NetInit()) return false;

    udpRcvBuf = NewPtr(2048);
    if (!udpRcvBuf) return false;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                 = gMacTCPRef;
    pb.csCode                    = UDPCreate;
    pb.csParam.create.rcvBuff    = udpRcvBuf;
    pb.csParam.create.rcvBuffLen = 2048;
    pb.csParam.create.notifyProc = NULL;
    pb.csParam.create.localPort  = 0;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
        DisposePtr(udpRcvBuf);
        return false;
    }
    udpStream = pb.udpStream;

    if (gOurIP == 0) {
        NetRefreshOurIP();
    }

    if (gOurIP != 0) {
        if (NetIsEmulatorSLIRP()) {
            targets[numTargets++] = (gOurIP & (ip_addr)gOurNetMask) | 2; /* 10.0.2.2 SLIRP host */
        }
        targets[numTargets++] = (gOurIP & (ip_addr)gOurNetMask) | 1;
        targets[numTargets++] = gOurIP | ~(ip_addr)gOurNetMask;
    }
    targets[numTargets++] = (ip_addr)0xFFFFFFFF;

    for (i = 0; i < numTargets; i++) {
        memset(&pb, 0, sizeof(pb));
        wds[0].length = (unsigned short)strlen(magic);
        wds[0].ptr    = (Ptr)magic;
        wds[1].length = 0;
        wds[1].ptr    = NULL;

        pb.ioCRefNum               = gMacTCPRef;
        pb.csCode                  = UDPWrite;
        pb.udpStream               = udpStream;
        pb.csParam.send.remoteHost = targets[i];
        pb.csParam.send.remotePort = DEFAULT_UDP_PORT;
        pb.csParam.send.wdsPtr     = (Ptr)wds;
        pb.csParam.send.checkSum   = true;
        pb.csParam.send.sendLength = 0;
        PBControlSync((ParmBlkPtr)&pb);
    }

    /* Wait up to 1 second for reply */
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum               = gMacTCPRef;
    pb.csCode                  = UDPRead;
    pb.udpStream               = udpStream;
    pb.csParam.receive.timeOut = 1;
    if (PBControlSync((ParmBlkPtr)&pb) == noErr && pb.csParam.receive.rcvBuffLen > 0) {
        char reply[256];
        short copyLen = pb.csParam.receive.rcvBuffLen;
        ip_addr responderIP = pb.csParam.receive.remoteHost;

        if (copyLen > (short)(sizeof(reply) - 1)) copyLen = sizeof(reply) - 1;
        memcpy(reply, pb.csParam.receive.rcvBuff, copyLen);
        reply[copyLen] = '\0';

        pb.csCode = UDPBfrReturn;
        PBControlSync((ParmBlkPtr)&pb);

        if (strncmp(reply, "MACCOMPANION_SERVER|", 20) == 0) {
            char *p1 = strchr(reply + 20, '|');
            unsigned short port = DEFAULT_HTTP_PORT;
            if (p1) {
                int parsedPort = atoi(p1 + 1);
                if (parsedPort > 0 && parsedPort < 65536) {
                    port = (unsigned short)parsedPort;
                }
            }
            if (responderIP != 0) {
                NetSetServer(responderIP, port);
                found = true;
            }
        }
    }

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode    = UDPRelease;
    pb.udpStream = udpStream;
    PBControlSync((ParmBlkPtr)&pb);

    DisposePtr(udpRcvBuf);
    return found;
}

/* --- Shared Helpers for Async TCPRcv & Sector-Aligned Ping-Pong Accumulation --- */

typedef void (*ArmRcvFn)(void *ctx, unsigned char *buf, unsigned short askLen);

typedef struct {
    TCPiopb *pb;
    StreamPtr stream;
    Boolean *pendingFlag;
    unsigned short timeoutSec;
} TcpRcvArmCtx;

static void ArmAsyncTcpRcv(void *rawCtx, unsigned char *buf, unsigned short askLen)
{
    TcpRcvArmCtx *ctx = (TcpRcvArmCtx *)rawCtx;
    OSErr err;

    if (!ctx || ctx->stream == 0 || *ctx->pendingFlag || askLen == 0) return;

    memset(ctx->pb, 0, sizeof(*ctx->pb));
    ctx->pb->ioCRefNum                     = gMacTCPRef;
    ctx->pb->csCode                        = TCPRcv;
    ctx->pb->tcpStream                     = ctx->stream;
    ctx->pb->csParam.receive.rcvBuff       = (Ptr)buf;
    ctx->pb->csParam.receive.rcvBuffLen    = askLen;
    ctx->pb->csParam.receive.commandTimeoutValue = ctx->timeoutSec;
    ctx->pb->csParam.receive.secondTimeStamp = 0;
    ctx->pb->csParam.receive.urgentFlag    = false;
    ctx->pb->csParam.receive.markFlag      = false;

    err = PBControlAsync((ParmBlkPtr)ctx->pb);
    if (err == noErr || ctx->pb->ioResult == inProgress) {
        *ctx->pendingFlag = true;
    } else {
        *ctx->pendingFlag = false;
    }
}

static unsigned short RxAccumAskLen(const MacBinaryRx *rx, const RxAccumState *st)
{
    unsigned long forkRem;
    long targetFill;
    long ask;

    if (rx->state == RX_HEADER) {
        ask = CHUNK_BUF_SIZE - st->curFill;
        return (unsigned short)((ask > 0) ? ask : 0);
    }

    forkRem = (rx->state == RX_DATA)
              ? (rx->dataBytesTarget - rx->dataBytesPadded)
              : (rx->rsrcBytesTarget - rx->rsrcBytesPadded);
    targetFill = (forkRem < (unsigned long)st->writeQuantum) ? (long)forkRem : st->writeQuantum;
    ask = targetFill - st->curFill;
    if (ask < 0) ask = 0;
    if (ask > (CHUNK_BUF_SIZE - st->curFill)) {
        ask = CHUNK_BUF_SIZE - st->curFill;
    }
    return (unsigned short)ask;
}

static Boolean RxAccumProcessReadyData(MacBinaryRx *rx,
                                       RxAccumState *st,
                                       long newlyArrived,
                                       ArmRcvFn armFn,
                                       void *armCtx)
{
    st->curFill += newlyArrived;

    if (rx->state == RX_HEADER) {
        long consumed = MacBinaryRx_ConsumeHeader(rx, st->curBuf, st->curFill);
        if (consumed < 0) {
            return false;
        }
        if (rx->state == RX_HEADER) {
            return true;
        }
        if (st->curFill > consumed) {
            long rem = st->curFill - consumed;
            memmove(st->curBuf, st->curBuf + consumed, (size_t)rem);
            st->curFill = rem;
        } else {
            st->curFill = 0;
        }
        SyncRxProgressGlobals(rx);
    }

    while (rx->state != RX_COMPLETE && rx->state != RX_ERROR && st->curFill > 0) {
        unsigned long forkRem = (rx->state == RX_DATA)
                                ? (rx->dataBytesTarget - rx->dataBytesPadded)
                                : (rx->rsrcBytesTarget - rx->rsrcBytesPadded);

        if ((unsigned long)st->curFill >= forkRem) {
            long extra = st->curFill - (long)forkRem;
            if (MacBinaryRx_WaitWrite(rx) != noErr) return false;
            if (MacBinaryRx_ProcessBytes(rx, st->curBuf, (long)forkRem) != noErr) return false;
            if (extra > 0) {
                memmove(st->curBuf, st->curBuf + forkRem, (size_t)extra);
            }
            st->curFill = extra;
            SyncRxProgressGlobals(rx);
        } else if (st->curFill >= st->writeQuantum) {
            long writeLen = st->writeQuantum;
            long leftover = st->curFill - writeLen;
            unsigned char *readyBuf = st->curBuf;
            unsigned char *otherBuf = (readyBuf == gChunkBufA) ? gChunkBufB : gChunkBufA;
            unsigned long remAfter = forkRem - (unsigned long)writeLen;
            long nextTarget = (remAfter < (unsigned long)st->writeQuantum) ? (long)remAfter : st->writeQuantum;
            long nextAsk = nextTarget - leftover;

            if (MacBinaryRx_WaitWrite(rx) != noErr) return false;
            if (leftover > 0) {
                BoltDrop_FastCopy(otherBuf, readyBuf + writeLen, (uint32_t)leftover, gBoltCtx.has68020Plus);
            }
            st->curBuf = otherBuf;
            st->curFill = leftover;

            if (nextAsk > 0 && armFn) {
                if (nextAsk > (CHUNK_BUF_SIZE - st->curFill)) {
                    nextAsk = CHUNK_BUF_SIZE - st->curFill;
                }
                armFn(armCtx, st->curBuf + st->curFill, (unsigned short)nextAsk);
            }

            if (MacBinaryRx_ProcessBytes(rx, readyBuf, writeLen) != noErr) return false;
            SyncRxProgressGlobals(rx);
            return true;
        } else {
            break;
        }
    }

    return (rx->state != RX_ERROR);
}

static Boolean RunBoltFastRecvLoop(BoltDropStream *boltStream,
                                   MacBinaryRx *rx,
                                   RxAccumState *accum,
                                   unsigned long *ioLastDataTick,
                                   unsigned long *ioLastSpeedTicks,
                                   unsigned long *ioLastSpeedBytes,
                                   unsigned long *ioLastUiTick,
                                   Boolean *outCancelled)
{
    if (outCancelled) *outCancelled = false;

    while (rx->state != RX_COMPLETE && rx->state != RX_ERROR) {
        long askLen;
        uint32_t got = 0;
        Boolean eof = false;

        if (accum->curFill >= accum->writeQuantum) {
            if (!RxAccumProcessReadyData(rx, accum, 0, NULL, NULL)) {
                return false;
            }
            if (rx->state == RX_COMPLETE || rx->state == RX_ERROR) {
                break;
            }
        }

        askLen = (long)RxAccumAskLen(rx, accum);
        if (askLen > 0) {
            OSErr rErr = BoltDrop_RecvChunk(boltStream,
                                            accum->curBuf + accum->curFill,
                                            (uint32_t)askLen,
                                            &got,
                                            &eof);
            if (rErr != noErr) {
                return false;
            }
            if (got > 0) {
                *ioLastDataTick = LMGetTicks();
                if (!RxAccumProcessReadyData(rx, accum, (long)got, NULL, NULL)) {
                    return false;
                }
                if (rx->state == RX_COMPLETE || rx->state == RX_ERROR) {
                    break;
                }
                UpdateTransferSpeed(gActiveTransferBytes, ioLastSpeedTicks, ioLastSpeedBytes);
            }
        }

        if (eof) {
            break;
        }

        if ((LMGetTicks() - *ioLastUiTick) >= UI_REDRAW_TICKS) {
            Boolean cancelled;
            BoltDrop_YieldTickless(boltStream, false);
            SystemTask();
            cancelled = PollTransferUIAndCancel(ioLastUiTick);
            if (cancelled) {
                if (outCancelled) *outCancelled = true;
                return false;
            }
            BoltDrop_YieldTickless(boltStream, true);
        } else if (got == 0 && (LMGetTicks() - *ioLastDataTick) > 1200) {
            return false;
        }
    }

    return (rx->state == RX_COMPLETE);
}

/* --- Reverse Push Channel --- */

static void StartChannelRcv(void)
{
    TcpRcvArmCtx ctx;
    unsigned short askLen;

    if (gChannelStream == 0 || gChanRcvPending) return;

    if (gChannelState == CHAN_STREAMING) {
        askLen = RxAccumAskLen(&gChannelRx, &gChannelRxAccum);
        if (askLen == 0) return;
        ctx.pb = &gChanRcvPB;
        ctx.stream = gChannelStream;
        ctx.pendingFlag = &gChanRcvPending;
        ctx.timeoutSec = 0;
        ArmAsyncTcpRcv(&ctx, gChannelRxAccum.curBuf + gChannelRxAccum.curFill, askLen);
    } else {
        ctx.pb = &gChanRcvPB;
        ctx.stream = gChannelStream;
        ctx.pendingFlag = &gChanRcvPending;
        ctx.timeoutSec = 0;
        ArmAsyncTcpRcv(&ctx, gChunkBufA, CHUNK_BUF_SIZE);
    }
}

static void NetCloseAndStopChannel(Boolean graceful)
{
    gChannelConnected = false;
    gChanRcvPending   = false;

    if (gChannelStream != 0) {
        TCPiopb pb;
        OSErr closeErr = -1;
        if (graceful) {
            memset(&pb, 0, sizeof(pb));
            pb.ioCRefNum                      = gMacTCPRef;
            pb.csCode                         = TCPClose;
            pb.tcpStream                      = gChannelStream;
            pb.csParam.close.ulpTimeoutValue  = 1;
            pb.csParam.close.ulpTimeoutAction = 1;
            pb.csParam.close.validityFlags    = timeoutValue | timeoutAction;
            closeErr = PBControlSync((ParmBlkPtr)&pb);
        }
        if (!graceful || closeErr != noErr) {
            memset(&pb, 0, sizeof(pb));
            pb.ioCRefNum = gMacTCPRef;
            pb.csCode    = TCPAbort;
            pb.tcpStream = gChannelStream;
            PBControlSync((ParmBlkPtr)&pb);
        }

        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode    = TCPRelease;
        pb.tcpStream = gChannelStream;
        PBControlSync((ParmBlkPtr)&pb);
        gChannelStream = 0;
    }

    gChannelState  = CHAN_DISCONNECTED;
    gHeaderParsed  = false;
    gHttpHeaderLen = 0;
}

void NetStopChannel(void)
{
    NetCloseAndStopChannel(false);
}

static long GetOptimalRcvBufSize(Boolean isChannel)
{
    if (gCachedProcType >= gestalt68040 || NetIsEmulatorSLIRP() ||
        (gBoltCtx.hasBoltFast && gBoltCtx.api.link_tier == BOLT_TIER_SONIC)) {
        return 65535L;
    }
    if (gCachedProcType >= gestalt68020) {
        /* 68020/68030 non-SONIC hardware: 32 KB window matches BolleEthernet/MacCon 30si 64 KB SRAM
         * (and BoltTcp 0.1.33+ automatically clamps 16K/32K DP8390 cards to 8K/24K in TCPCreate) */
        return 32768L;
    }
    return isChannel ? 32768L : 16384L;
}

Boolean NetConnectChannel(void)
{
    TCPiopb pb;
    OSErr err;

    if (!NetInit() || gServerIP == 0) return false;

    NetStopChannel();

    if (!gInitialPeersFetched) {
        if (!NetRefreshPeers()) {
            return false;
        }
    }

    if (!gChannelRcvBuf) {
        long desiredSize = GetOptimalRcvBufSize(true);
        gChannelRcvBufLen = desiredSize;
        gChannelRcvBuf = NewPtr(gChannelRcvBufLen);
        if (!gChannelRcvBuf && desiredSize > 32768) {
            gChannelRcvBufLen = 32768;
            gChannelRcvBuf = NewPtr(gChannelRcvBufLen);
        }
        if (!gChannelRcvBuf) {
            gChannelRcvBufLen = FALLBACK_RCV_SIZE;
            gChannelRcvBuf = NewPtr(gChannelRcvBufLen);
            if (!gChannelRcvBuf) return false;
        }
    }

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                 = gMacTCPRef;
    pb.csCode                    = TCPCreate;
    pb.csParam.create.rcvBuff    = gChannelRcvBuf;
    pb.csParam.create.rcvBuffLen = gChannelRcvBufLen;
    pb.csParam.create.notifyProc = NULL;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) return false;

    gChannelStream = pb.tcpStream;

    if (gOurIP == 0) {
        NetRefreshOurIP();
    }

    memset(&gChanOpenPB, 0, sizeof(gChanOpenPB));
    gChanOpenPB.ioCRefNum                        = gMacTCPRef;
    gChanOpenPB.csCode                           = TCPActiveOpen;
    gChanOpenPB.tcpStream                        = gChannelStream;
    gChanOpenPB.csParam.open.ulpTimeoutValue     = 10;
    gChanOpenPB.csParam.open.ulpTimeoutAction    = 1;
    gChanOpenPB.csParam.open.validityFlags       = timeoutValue | timeoutAction;
    gChanOpenPB.csParam.open.commandTimeoutValue = 5;
    gChanOpenPB.csParam.open.remoteHost          = gServerIP;
    gChanOpenPB.csParam.open.remotePort          = gServerPort;
    gChanOpenPB.csParam.open.localPort           = 0;

    err = PBControlAsync((ParmBlkPtr)&gChanOpenPB);
    if (err != noErr && gChanOpenPB.ioResult > 0) {
        NetStopChannel();
        return false;
    }

    gChannelState = CHAN_CONNECTING;
    return true;
}

/* --- Direct Passive TCP Listener (Port 6803) --- */

Boolean NetStartListener(void)
{
    TCPiopb pb;
    OSErr err;

    if (gListenerActive) return true;
    if (!NetInit()) return false;

    if (!gListenRcvBuf) {
        long desiredSize = GetOptimalRcvBufSize(false);
        gListenRcvBufLen = desiredSize;
        gListenRcvBuf = NewPtr(gListenRcvBufLen);
        if (!gListenRcvBuf && desiredSize > 32768) {
            gListenRcvBufLen = 32768;
            gListenRcvBuf = NewPtr(gListenRcvBufLen);
        }
        if (!gListenRcvBuf) {
            gListenRcvBufLen = FALLBACK_RCV_SIZE;
            gListenRcvBuf = NewPtr(gListenRcvBufLen);
            if (!gListenRcvBuf) return false;
        }
    }

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                 = gMacTCPRef;
    pb.csCode                    = TCPCreate;
    pb.csParam.create.rcvBuff    = gListenRcvBuf;
    pb.csParam.create.rcvBuffLen = gListenRcvBufLen;
    pb.csParam.create.notifyProc = NULL;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) return false;

    gListenStream = pb.tcpStream;
    if (gOurIP == 0) {
        NetRefreshOurIP();
    }

    memset(&gPassivePB, 0, sizeof(gPassivePB));
    gPassivePB.ioCRefNum                         = gMacTCPRef;
    gPassivePB.csCode                            = TCPPassiveOpen;
    gPassivePB.tcpStream                         = gListenStream;
    gPassivePB.csParam.open.ulpTimeoutValue      = 0;
    gPassivePB.csParam.open.ulpTimeoutAction     = 0;
    gPassivePB.csParam.open.validityFlags        = timeoutValue | timeoutAction;
    gPassivePB.csParam.open.commandTimeoutValue  = 0;
    gPassivePB.csParam.open.remoteHost           = 0;
    gPassivePB.csParam.open.remotePort           = 0;
    gPassivePB.csParam.open.localPort            = MACDROP_TCP_PORT;

    err = PBControlAsync((ParmBlkPtr)&gPassivePB);
    if (err == noErr || gPassivePB.ioResult == inProgress) {
        gListenerActive = true;
        return true;
    }

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode    = TCPRelease;
    pb.tcpStream = gListenStream;
    PBControlSync((ParmBlkPtr)&pb);
    gListenStream = 0;
    return false;
}

void NetStopListener(void)
{
    TCPiopb pb;

    if (!gListenerActive) return;

    if (gListenStream != 0) {
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode    = TCPAbort;
        pb.tcpStream = gListenStream;
        PBControlSync((ParmBlkPtr)&pb);

        pb.csCode    = TCPRelease;
        PBControlSync((ParmBlkPtr)&pb);
        gListenStream = 0;
    }

    gListenerActive = false;
}

/* --- Main Event Loop Network Polling --- */

static void PollChannelConnecting(void)
{
    if (gChannelState != CHAN_CONNECTING) return;

    if (gChanOpenPB.ioResult <= 0) {
        if (gChanOpenPB.ioResult == noErr) {
            /* Connection opened! Send reverse channel HTTP GET request */
            char encName[128];
            char encMach[128];
            char req[768];
            char hostStr[32];
            char ourIpStr[32];
            wdsEntry wds[2];
            TCPiopb sendPB;

            if (gOurIP == 0) {
                NetRefreshOurIP();
            }
            FormatIP(gServerIP, hostStr);
            FormatIP(gOurIP, ourIpStr);
            URLEncode(gClientName, encName, sizeof(encName));
            URLEncode(GetMacMachineName(), encMach, sizeof(encMach));

            snprintf(req, sizeof(req),
                "GET /api/macdrop/channel?id=%s&name=%s&machine=%s&ver=%s&ip=%s HTTP/1.0\r\n"
                "Host: %s:%u\r\n"
                "User-Agent: MacDrop-68k/%s\r\n"
                "X-Client-IP: %s\r\n"
                "Connection: close\r\n\r\n",
                gClientID, encName, encMach, APP_VERSION_STR, ourIpStr,
                hostStr, gServerPort, APP_VERSION_STR, ourIpStr);

            wds[0].length = (unsigned short)strlen(req);
            wds[0].ptr    = (Ptr)req;
            wds[1].length = 0;
            wds[1].ptr    = NULL;

            memset(&sendPB, 0, sizeof(sendPB));
            sendPB.ioCRefNum                     = gMacTCPRef;
            sendPB.csCode                        = TCPSend;
            sendPB.tcpStream                     = gChannelStream;
            sendPB.csParam.send.ulpTimeoutValue  = 5;
            sendPB.csParam.send.ulpTimeoutAction = 1;
            sendPB.csParam.send.validityFlags    = timeoutValue | timeoutAction;
            sendPB.csParam.send.pushFlag         = true;
            sendPB.csParam.send.urgentFlag       = false;
            sendPB.csParam.send.wdsPtr           = (Ptr)wds;
            PBControlSync((ParmBlkPtr)&sendPB);

            gChannelConnected = true;
            gChannelState     = CHAN_CONNECTED;
            gHeaderParsed     = false;
            gHttpHeaderLen    = 0;

            StartChannelRcv();
            {
                extern void UpdateProgressUI(void);
                UpdateProgressUI();
            }
        } else {
            /* Connection failed, clear stale server/peer cards and retry later */
            NetStopChannel();
            ResetServerPeerState(true);
            gLastChannelAttemptTick = LMGetTicks();
        }
    }
}

static void CompleteChannelDownload(Boolean ok)
{
    char savedTransferId[64];
    unsigned long totalWritten = gChannelRx.dataBytesWritten + gChannelRx.rsrcBytesWritten;
    extern void UpdateProgressUI(void);

    if (gChannelBolt.stream != 0) {
        BoltDrop_EndTransfer(&gChannelBolt);
        gChannelBolt.stream = 0;
    }
    MacBinaryRx_SetWaitPollHook(NULL);

    strncpy(savedTransferId, gCurrentTransferId, sizeof(savedTransferId) - 1);
    savedTransferId[sizeof(savedTransferId) - 1] = '\0';

    if (ok) {
        MacBinaryRx_Finish(&gChannelRx);
    } else {
        MacBinaryRx_Abort(&gChannelRx);
    }

    /* Close Slot 1 gracefully on success (avoids RST during small-file drain) before ACK or flushing */
    NetCloseAndStopChannel(ok);
    gLastChannelAttemptTick = 0;

    if (savedTransferId[0] != '\0') {
        NetSendTransferAck(savedTransferId, ok ? "OK" : "ERROR", ok ? totalWritten : 0);
    }
    if (ok) {
        MacBinaryRx_FlushDestVolume();
    }

    gTransferActive = false;
    gCurrentTransferId[0] = '\0';
    NetConnectChannel();
    PollChannelConnecting();

    if (ok) {
        TriggerTransferNotification(gChannelRx.fileName);
    }
    UpdateProgressUI();
}

void NetIdle(void)
{
    extern void UpdateProgressUI(void);

    if (!gNetReady) return;

    /* 1. Handle Reverse Push Channel connection lifecycle */
    if (gChannelState == CHAN_DISCONNECTED) {
        unsigned long nowTicks = LMGetTicks();

        /* Periodically check if SpotiMac or MacAI saved a server IP in Preferences when unconfigured */
        if (gServerIP == 0 && !gChannelConnected && (nowTicks - gLastSiblingCheckTick > 240)) {
            unsigned long sibIP = 0;
            gLastSiblingCheckTick = nowTicks;
            if (TryLoadSiblingServerIP(&sibIP) && sibIP != 0) {
                NetSetServer(sibIP, DEFAULT_HTTP_PORT);
                gLastChannelAttemptTick = 0;
            }
        }

        if (gServerIP != 0 && (nowTicks - gLastChannelAttemptTick > 180)) {
            gLastChannelAttemptTick = nowTicks;
            NetConnectChannel();
        }
    } else if (gChannelState == CHAN_CONNECTING) {
        PollChannelConnecting();
    } else if (gChannelState == CHAN_CONNECTED || gChannelState == CHAN_STREAMING) {
        unsigned long lastDataTick = LMGetTicks();
        unsigned long lastSysTick  = lastDataTick;
        unsigned long lastUiTick   = lastDataTick;
        short sysTickInterval = GetSystemTaskInterval();
        TcpRcvArmCtx chanArmCtx = { &gChanRcvPB, gChannelStream, &gChanRcvPending, 0 };

        while (gChannelState == CHAN_CONNECTED || gChannelState == CHAN_STREAMING) {
            Boolean peerClosed = false;

            if (!gChanRcvPending) {
                break;
            }
            if (gChanRcvPB.ioResult > 0) {
                /* While actively streaming a file, stay inside NetIdle without yielding to WaitNextEvent() */
                if (gChannelState == CHAN_STREAMING) {
                    unsigned long nowTick = LMGetTicks();
                    if ((nowTick - lastDataTick) > 1200) {
                        /* Stalled for >20s with no packets */
                        break;
                    }
                    if ((nowTick - lastSysTick) >= (unsigned long)sysTickInterval) {
                        lastSysTick = nowTick;
                        SystemTask();
                    }
                    if (PollTransferUIAndCancel(&lastUiTick)) {
                        NetCancelActiveTransfer();
                        UpdateProgressUI();
                        return;
                    }
                    continue;
                }
                break;
            }
            gChanRcvPending = false;
            lastDataTick = LMGetTicks();
            peerClosed = (gChanRcvPB.ioResult == connectionClosing);

            if ((gChanRcvPB.ioResult == noErr || peerClosed) &&
                gChanRcvPB.csParam.receive.rcvBuffLen > 0) {
                long nBytes = (unsigned short)gChanRcvPB.csParam.receive.rcvBuffLen;

                if (!gHeaderParsed) {
                    unsigned char *pData = gChunkBufA;
                    long prevHttpLen = gHttpHeaderLen;
                    long copyLen = nBytes;
                    char *delim;
                    short delimLen = 4;

                    if (gHttpHeaderLen + copyLen > (long)(sizeof(gHttpHeaderBuf) - 1)) {
                        copyLen = (long)(sizeof(gHttpHeaderBuf) - 1 - gHttpHeaderLen);
                    }
                    if (copyLen > 0) {
                        memcpy(gHttpHeaderBuf + gHttpHeaderLen, pData, (size_t)copyLen);
                        gHttpHeaderLen += (short)copyLen;
                    }
                    gHttpHeaderBuf[gHttpHeaderLen] = '\0';

                    delim = strstr(gHttpHeaderBuf, "\r\n\r\n");
                    if (!delim) {
                        delim = strstr(gHttpHeaderBuf, "\n\n");
                        delimLen = 2;
                    }

                    if (delim) {
                        /* Check if this is an idle keepalive poll response (204 No Content or X-MacDrop-Status: POLL) */
                        if (strstr(gHttpHeaderBuf, " 204 ") != NULL ||
                            strstr(gHttpHeaderBuf, "X-MacDrop-Status: POLL") != NULL) {
                            char *pPeers = strstr(gHttpHeaderBuf, "X-MacDrop-Peers: ");
                            if (pPeers) {
                                char peersVal[512];
                                char *endP = strstr(pPeers + 17, "\r\n");
                                if (!endP) endP = strstr(pPeers + 17, "\n");
                                if (endP) {
                                    short pLen = (short)(endP - (pPeers + 17));
                                    if (pLen > (short)(sizeof(peersVal) - 1)) pLen = sizeof(peersVal) - 1;
                                    memcpy(peersVal, pPeers + 17, pLen);
                                    peersVal[pLen] = '\0';
                                    ParsePeersHeaderString(peersVal);
                                }
                            } else {
                                /* Servers that omit X-MacDrop-Peers on 204 POLL (e.g. MacDrop X) refresh via /peers */
                                gInitialPeersFetched = false;
                            }
                            NetStopChannel();
                            gLastChannelAttemptTick = 0; /* reconnect immediately for next poll */
                            NetConnectChannel();
                            PollChannelConnecting();
                            return;
                        }

                        gHeaderParsed = true;
                        gChannelState = CHAN_STREAMING;
                        gTransferActive = true;
                        gTransferIsUpload = false;
                        MacBinaryRx_Init(&gChannelRx);
                        gChannelRxAccum.curBuf = gChunkBufA;
                        gChannelRxAccum.curFill = 0;
                        gChannelRxAccum.writeQuantum = GetDiskWriteQuantum();
                        gActiveTransferName[0] = '\0';
                        gActiveTransferBytes = 0;
                        gActiveTransferTotal = 0;
                        gTransferSpeedBps    = 0;
                        gRxLastSpeedTicks    = LMGetTicks();
                        gRxLastSpeedBytes    = 0;
                        gCurrentTransferId[0] = '\0';

                        OnTransferStarted(false);

                        /* Extract Transfer ID for ACK */
                        {
                            char *pId = strstr(gHttpHeaderBuf, "X-Transfer-Id: ");
                            if (pId) {
                                char *endId = strstr(pId + 15, "\r\n");
                                if (!endId) endId = strstr(pId + 15, "\n");
                                if (endId) {
                                    short idLen = (short)(endId - (pId + 15));
                                    if (idLen > 63) idLen = 63;
                                    memcpy(gCurrentTransferId, pId + 15, idLen);
                                    gCurrentTransferId[idLen] = '\0';
                                }
                            }
                        }

                        {
                            long totalHttpHdrBytes = (long)(delim - gHttpHeaderBuf) + delimLen;
                            long hdrBytesInPacket = totalHttpHdrBytes - prevHttpLen;
                            long payloadBytes;

                            if (hdrBytesInPacket < 0) hdrBytesInPacket = 0;
                            if (hdrBytesInPacket > nBytes) hdrBytesInPacket = nBytes;
                            payloadBytes = nBytes - hdrBytesInPacket;

                            if (payloadBytes > 0) {
                                memmove(gChunkBufA, pData + hdrBytesInPacket, (size_t)payloadBytes);
                                RxAccumProcessReadyData(&gChannelRx, &gChannelRxAccum, payloadBytes,
                                                        gBoltCtx.hasBoltFast ? NULL : ArmAsyncTcpRcv, &chanArmCtx);
                            }
                        }

                        UpdateProgressUI();
                        lastUiTick = LMGetTicks();

                        if (gBoltCtx.hasBoltFast) {
                            Boolean cancelled = false;
                            Boolean okDone;

                            if (!peerClosed && gChannelRx.state != RX_COMPLETE && gChannelRx.state != RX_ERROR) {
                                BoltDrop_BeginTransfer(&gChannelBolt, &gBoltCtx, gChannelStream, true, true);
                                MacBinaryRx_SetWaitPollHook(PumpBoltPollHook);
                                RunBoltFastRecvLoop(&gChannelBolt, &gChannelRx, &gChannelRxAccum,
                                                    &lastDataTick, &gRxLastSpeedTicks, &gRxLastSpeedBytes,
                                                    &lastUiTick, &cancelled);
                                BoltDrop_EndTransfer(&gChannelBolt);
                                gChannelBolt.stream = 0;
                                MacBinaryRx_SetWaitPollHook(NULL);

                                if (cancelled) {
                                    NetCancelActiveTransfer();
                                    UpdateProgressUI();
                                    return;
                                }
                            }

                            okDone = (gChannelRx.state == RX_COMPLETE ||
                                      (gChannelRx.dataLen > 0 && gChannelRx.dataBytesWritten >= gChannelRx.dataLen));
                            CompleteChannelDownload(okDone);
                            return;
                        }
                    } else if (!peerClosed) {
                        StartChannelRcv();
                    }
                } else {
                    RxAccumProcessReadyData(&gChannelRx, &gChannelRxAccum, nBytes,
                                            ArmAsyncTcpRcv, &chanArmCtx);
                }

                if (gChannelState == CHAN_STREAMING) {
                    UpdateTransferSpeed(gActiveTransferBytes, &gRxLastSpeedTicks, &gRxLastSpeedBytes);
                    if (PollTransferUIAndCancel(&lastUiTick)) {
                        NetCancelActiveTransfer();
                        UpdateProgressUI();
                        return;
                    }
                }

                if (gChannelState == CHAN_STREAMING && gChannelRx.state == RX_COMPLETE) {
                    CompleteChannelDownload(true);
                    return;
                }

                if (peerClosed) {
                    if (gTransferActive) {
                        Boolean okDone = (gChannelRx.state == RX_COMPLETE ||
                                          (gChannelRx.dataLen > 0 && gChannelRx.dataBytesWritten >= gChannelRx.dataLen));
                        CompleteChannelDownload(okDone);
                    } else {
                        NetStopChannel();
                        ResetServerPeerState(true);
                        gLastChannelAttemptTick = 0;
                        NetConnectChannel();
                        PollChannelConnecting();
                    }
                    break;
                }

                if (!gChanRcvPending && gChannelRx.state != RX_ERROR) {
                    StartChannelRcv();
                } else if (gChannelRx.state == RX_ERROR) {
                    CompleteChannelDownload(false);
                    return;
                }
            } else {
                /* Connection closed by server or error occurred */
                if (gTransferActive) {
                    Boolean okDone = (gChannelRx.state == RX_COMPLETE ||
                                      (gChannelRx.dataLen > 0 && gChannelRx.dataBytesWritten >= gChannelRx.dataLen));
                    CompleteChannelDownload(okDone);
                } else {
                    NetStopChannel();
                    ResetServerPeerState(true);
                    gLastChannelAttemptTick = 0;
                    NetConnectChannel();
                    PollChannelConnecting();
                }
                break;
            }
        }
    }

    /* 2. Handle Direct TCP Passive Listener on Port 6803 */
    if (gListenerActive && !gTransferActive) {
        if (gPassivePB.ioResult <= 0) {
            if (gPassivePB.ioResult == noErr) {
                ip_addr remotePeerIP = gPassivePB.csParam.open.remoteHost;
                TCPiopb rcvPB;
                OSErr rErr;
                unsigned short initBytes = 0;
                Boolean initPeerClosed = false;

                /* Read first chunk into gChunkBufA to distinguish a server ping/discovery probe from a file transfer */
                memset(&rcvPB, 0, sizeof(rcvPB));
                rcvPB.ioCRefNum                        = gMacTCPRef;
                rcvPB.csCode                           = TCPRcv;
                rcvPB.tcpStream                        = gListenStream;
                rcvPB.csParam.receive.rcvBuff          = (Ptr)gChunkBufA;
                rcvPB.csParam.receive.rcvBuffLen       = CHUNK_BUF_SIZE;
                rcvPB.csParam.receive.commandTimeoutValue = 1;
                rcvPB.csParam.receive.secondTimeStamp  = 0;
                rcvPB.csParam.receive.urgentFlag       = false;
                rcvPB.csParam.receive.markFlag         = false;

                rErr = PBControlSync((ParmBlkPtr)&rcvPB);
                initBytes = (unsigned short)rcvPB.csParam.receive.rcvBuffLen;
                initPeerClosed = (rErr == connectionClosing);

                if ((rErr != noErr && rErr != connectionClosing) || initBytes == 0 ||
                    (initBytes >= 13 && strncmp((char *)gChunkBufA, "MACDROP_HELLO", 13) == 0)) {
                    /* Server Ping / Auto-Discovery Probe! Reply with MACDROP_PONG and auto-pair only if unconfigured */
                    ip_addr discoveredIP = remotePeerIP;
                    unsigned short discoveredPort = DEFAULT_HTTP_PORT;

                    if ((rErr == noErr || rErr == connectionClosing) && initBytes >= 14 &&
                        strncmp((char *)gChunkBufA, "MACDROP_HELLO|", 14) == 0) {
                        char helloBuf[128];
                        short hLen = (short)initBytes;
                        if (hLen > (short)(sizeof(helloBuf) - 1)) hLen = sizeof(helloBuf) - 1;
                        memcpy(helloBuf, gChunkBufA, hLen);
                        helloBuf[hLen] = '\0';

                        char *pIp = helloBuf + 14;
                        char *pSep = strchr(pIp, '|');
                        if (pSep) {
                            *pSep = '\0';
                            int portVal = atoi(pSep + 1);
                            if (portVal > 0 && portVal < 65536) {
                                discoveredPort = (unsigned short)portVal;
                            }
                        }
                        ip_addr parsedHelloIP = 0;
                        if (ParseIPAddress(pIp, &parsedHelloIP) && parsedHelloIP != 0 && parsedHelloIP != 0x7F000001UL) {
                            if (!(NetIsEmulatorSLIRP() && (remotePeerIP & 0xFFFFFF00UL) == 0x0A000200UL)) {
                                discoveredIP = parsedHelloIP;
                            }
                        }
                    }

                    /* Send MACDROP_PONG|<id>|<name>|<machine>|<ver>|<ourIp> */
                    {
                        TCPiopb sendPB;
                        wdsEntry wds[2];
                        char pongStr[256];
                        char ourIpStr[32];
                        if (gOurIP == 0) NetRefreshOurIP();
                        FormatIP(gOurIP, ourIpStr);
                        snprintf(pongStr, sizeof(pongStr), "MACDROP_PONG|%s|%s|%s|%s|%s\n",
                                 gClientID, gClientName, GetMacMachineName(), APP_VERSION_STR, ourIpStr);

                        wds[0].length = (unsigned short)strlen(pongStr);
                        wds[0].ptr    = (Ptr)pongStr;
                        wds[1].length = 0;
                        wds[1].ptr    = NULL;

                        memset(&sendPB, 0, sizeof(sendPB));
                        sendPB.ioCRefNum                     = gMacTCPRef;
                        sendPB.csCode                        = TCPSend;
                        sendPB.tcpStream                     = gListenStream;
                        sendPB.csParam.send.ulpTimeoutValue  = 2;
                        sendPB.csParam.send.ulpTimeoutAction = 1;
                        sendPB.csParam.send.validityFlags    = timeoutValue | timeoutAction;
                        sendPB.csParam.send.pushFlag         = true;
                        sendPB.csParam.send.urgentFlag       = false;
                        sendPB.csParam.send.wdsPtr           = (Ptr)wds;
                        PBControlSync((ParmBlkPtr)&sendPB);
                    }

                    NetStopListener();
                    NetStartListener();

                    /* Only auto-pair from MACDROP_HELLO when no Server IP is configured yet */
                    if (gServerIP == 0 && discoveredIP != 0) {
                        NetSetServer(discoveredIP, discoveredPort);
                        NetConnectChannel();
                    }
                    return;
                }

                /* Direct incoming MacBinary file transfer on s=0: pause reverse channel (s=1) while streaming */
                NetStopChannel();

                gTransferActive = true;
                gTransferIsUpload = false;
                MacBinaryRx_Init(&gPassiveRx);
                gActiveTransferName[0] = '\0';
                gActiveTransferBytes = 0;
                gActiveTransferTotal = 0;
                gTransferSpeedBps    = 0;
                gRxLastSpeedTicks    = LMGetTicks();
                gRxLastSpeedBytes    = 0;

                OnTransferStarted(false);

                {
                    RxAccumState passiveAccum;
                    TCPiopb activeRcvPB;
                    Boolean rcvPending = false;
                    TcpRcvArmCtx listenArmCtx = { &activeRcvPB, gListenStream, &rcvPending, 10 };
                    unsigned long lastDataTick = LMGetTicks();
                    unsigned long lastSysTick  = lastDataTick;
                    unsigned long lastUiTick   = lastDataTick;
                    short listenSysTickInterval = GetSystemTaskInterval();

                    passiveAccum.curBuf = gChunkBufA;
                    passiveAccum.curFill = 0;
                    passiveAccum.writeQuantum = GetDiskWriteQuantum();

                    RxAccumProcessReadyData(&gPassiveRx, &passiveAccum, (long)initBytes,
                                            gBoltCtx.hasBoltFast ? NULL : ArmAsyncTcpRcv, &listenArmCtx);
                    UpdateProgressUI();
                    lastUiTick = LMGetTicks();

                    if (gBoltCtx.hasBoltFast) {
                        if (!initPeerClosed && gPassiveRx.state != RX_COMPLETE && gPassiveRx.state != RX_ERROR) {
                            BoltDropStream passiveBolt;
                            Boolean cancelled = false;
                            BoltDrop_BeginTransfer(&passiveBolt, &gBoltCtx, gListenStream, true, true);
                            MacBinaryRx_SetWaitPollHook(PumpBoltPollHook);
                            RunBoltFastRecvLoop(&passiveBolt, &gPassiveRx, &passiveAccum,
                                                &lastDataTick, &gRxLastSpeedTicks, &gRxLastSpeedBytes,
                                                &lastUiTick, &cancelled);
                            BoltDrop_EndTransfer(&passiveBolt);
                            MacBinaryRx_SetWaitPollHook(NULL);
                            if (cancelled) {
                                SysBeep(1);
                                gPassiveRx.state = RX_ERROR;
                            }
                        }
                    } else {
                        while (!initPeerClosed && gPassiveRx.state != RX_COMPLETE && gPassiveRx.state != RX_ERROR) {
                            Boolean peerClosed = false;

                            if (!rcvPending) {
                                unsigned short askLen = RxAccumAskLen(&gPassiveRx, &passiveAccum);
                                if (askLen > 0) {
                                    ArmAsyncTcpRcv(&listenArmCtx,
                                                   passiveAccum.curBuf + passiveAccum.curFill,
                                                   askLen);
                                }
                                if (!rcvPending) {
                                    break;
                                }
                            }

                            if (activeRcvPB.ioResult > 0) {
                                unsigned long nowTick = LMGetTicks();
                                if ((nowTick - lastDataTick) > 1200) {
                                    break;
                                }
                                if ((nowTick - lastSysTick) >= (unsigned long)listenSysTickInterval) {
                                    lastSysTick = nowTick;
                                    SystemTask();
                                }
                                if (PollTransferUIAndCancel(&lastUiTick)) {
                                    SysBeep(1);
                                    gPassiveRx.state = RX_ERROR;
                                    break;
                                }
                                continue;
                            }

                            rcvPending = false;
                            lastDataTick = LMGetTicks();
                            peerClosed = (activeRcvPB.ioResult == connectionClosing);

                            if ((activeRcvPB.ioResult == noErr || peerClosed) &&
                                activeRcvPB.csParam.receive.rcvBuffLen > 0) {
                                long bytes = (unsigned short)activeRcvPB.csParam.receive.rcvBuffLen;
                                RxAccumProcessReadyData(&gPassiveRx, &passiveAccum, bytes,
                                                        ArmAsyncTcpRcv, &listenArmCtx);
                                UpdateTransferSpeed(gActiveTransferBytes, &gRxLastSpeedTicks, &gRxLastSpeedBytes);
                                if (PollTransferUIAndCancel(&lastUiTick)) {
                                    SysBeep(1);
                                    gPassiveRx.state = RX_ERROR;
                                    break;
                                }
                                if (peerClosed) {
                                    break;
                                }
                            } else {
                                break;
                            }
                        }

                        if (rcvPending && activeRcvPB.ioResult > 0) {
                            TCPiopb abortPB;
                            memset(&abortPB, 0, sizeof(abortPB));
                            abortPB.ioCRefNum = gMacTCPRef;
                            abortPB.csCode    = TCPAbort;
                            abortPB.tcpStream = gListenStream;
                            PBControlSync((ParmBlkPtr)&abortPB);
                        }
                    }
                }

                if (gPassiveRx.state == RX_COMPLETE) {
                    MacBinaryRx_Finish(&gPassiveRx);

                    /* Send ACK back to server over the direct TCP socket */
                    {
                        TCPiopb sendPB;
                        wdsEntry wds[2];
                        char ackStr[64];
                        sprintf(ackStr, "MACDROP_ACK %lu\n", gPassiveRx.dataBytesWritten + gPassiveRx.rsrcBytesWritten);
                        wds[0].length = (unsigned short)strlen(ackStr);
                        wds[0].ptr    = (Ptr)ackStr;
                        wds[1].length = 0;
                        wds[1].ptr    = NULL;

                        memset(&sendPB, 0, sizeof(sendPB));
                        sendPB.ioCRefNum                     = gMacTCPRef;
                        sendPB.csCode                        = TCPSend;
                        sendPB.tcpStream                     = gListenStream;
                        sendPB.csParam.send.ulpTimeoutValue  = 3;
                        sendPB.csParam.send.ulpTimeoutAction = 1;
                        sendPB.csParam.send.validityFlags    = timeoutValue | timeoutAction;
                        sendPB.csParam.send.pushFlag         = true;
                        sendPB.csParam.send.urgentFlag       = false;
                        sendPB.csParam.send.wdsPtr           = (Ptr)wds;
                        PBControlSync((ParmBlkPtr)&sendPB);
                    }

                    MacBinaryRx_FlushDestVolume();
                    gTransferActive = false;
                    TriggerTransferNotification(gPassiveRx.fileName);
                    UpdateProgressUI();
                } else {
                    MacBinaryRx_Abort(&gPassiveRx);
                    gTransferActive = false;
                    UpdateProgressUI();
                }

                /* Close listener socket, restart listener (s=0), then reconnect reverse channel (s=1) */
                NetStopListener();
                NetStartListener();
                gLastChannelAttemptTick = 0;
                NetConnectChannel();
            } else {
                /* Passive open failed or aborted, restart */
                NetStopListener();
                NetStartListener();
            }
        }
    }
}
