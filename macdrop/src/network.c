#include "network.h"
#include <Devices.h>
#include <Memory.h>
#include <Events.h>
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
char            gActiveTransferName[64] = "";
unsigned long   gActiveTransferBytes = 0;
unsigned long   gActiveTransferTotal = 0;
char            gCurrentTransferId[64] = "";

#define LISTEN_RCV_BUF_SIZE 32768
#define CHAN_RCV_BUF_SIZE   32768
#define CHUNK_BUF_SIZE      8192

/* Passive listener on port 6803 */
static Ptr          gListenRcvBuf    = NULL;
static StreamPtr    gListenStream    = 0;
static TCPiopb      gPassivePB;
static Boolean      gListenerActive  = false;
static MacBinaryRx  gPassiveRx;
static unsigned char gListenChunkBuf[CHUNK_BUF_SIZE];

/* Reverse push channel to Node.js server */
typedef enum {
    CHAN_DISCONNECTED = 0,
    CHAN_CONNECTING,
    CHAN_CONNECTED,
    CHAN_STREAMING
} ChannelState;

static ChannelState gChannelState    = CHAN_DISCONNECTED;
static Ptr          gChannelRcvBuf   = NULL;
static StreamPtr    gChannelStream   = 0;
static TCPiopb      gChanOpenPB;
static TCPiopb      gChanRcvPB;
static Boolean      gChanRcvPending  = false;
static unsigned long gLastChannelAttemptTick = 0;
static MacBinaryRx  gChannelRx;
static unsigned char gChanChunkBuf[CHUNK_BUF_SIZE];

/* Header buffer for HTTP parsing on reverse channel */
static char         gHttpHeaderBuf[1024];
static short        gHttpHeaderLen   = 0;
static Boolean      gHeaderParsed    = false;

static NMRec        gNMRec;

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
    if (gSoundEnabled) {
        SysBeep(30);
    }

    if (gNotifyEnabled) {
        static Str255 notifyMsg;
        short len = sprintf((char *)&notifyMsg[1], "Received: %s", fileName);
        if (len > 255) len = 255;
        notifyMsg[0] = (unsigned char)len;

        memset(&gNMRec, 0, sizeof(gNMRec));
        gNMRec.qType   = 8; /* nmType */
        gNMRec.nmMark  = 1; /* flash Apple icon in menu bar */
        gNMRec.nmStr   = notifyMsg;
        gNMRec.nmResp  = (NMUPP)-1; /* auto-remove on display */
        NMInstall(&gNMRec);
    }
}

Boolean NetInit(void)
{
    OSErr err;
    GetAddrParamBlock addrPB;

    if (gNetReady) return true;

    err = OpenDriver("\p.ipp", &gMacTCPRef);
    if (err != noErr) {
        gNetReady = false;
        return false;
    }

    memset(&addrPB, 0, sizeof(addrPB));
    addrPB.ioCRefNum = gMacTCPRef;
    addrPB.csCode    = ipctlGetAddr;
    err = PBControlSync((ParmBlkPtr)&addrPB);
    if (err == noErr) {
        gOurIP      = addrPB.ourAddress;
        gOurNetMask = addrPB.ourNetMask ? addrPB.ourNetMask : (long)0xFFFFFF00;
    } else {
        gOurIP      = 0x0A00020F; /* 10.0.2.15 */
        gOurNetMask = (long)0xFFFFFF00;
    }

    gNetReady = true;
    return true;
}

void NetSetServer(ip_addr ip, unsigned short port)
{
    gServerIP        = ip;
    gServerPort      = port ? port : DEFAULT_HTTP_PORT;
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

    /* Read response */
    startTicks = TickCount();
    while (totalRead < (maxLen - 1) && (TickCount() - startTicks) < 180) {
        short chunkAsk = (short)((maxLen - 1 - totalRead) > 1024 ? 1024 : (maxLen - 1 - totalRead));
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum                           = gMacTCPRef;
        pb.csCode                              = TCPRcv;
        pb.tcpStream                           = stream;
        pb.csParam.receive.commandTimeoutValue = 2;
        pb.csParam.receive.rcvBuff             = outBody + totalRead;
        pb.csParam.receive.rcvBuffLen          = chunkAsk;
        err = PBControlSync((ParmBlkPtr)&pb);
        if (err == noErr && pb.csParam.receive.rcvBuffLen > 0) {
            totalRead += pb.csParam.receive.rcvBuffLen;
            outBody[totalRead] = '\0';
            if (!headerStripped) {
                char *delim = strstr(outBody, "\r\n\r\n");
                if (delim) {
                    long headerBytes = (delim - outBody) + 4;
                    long rem = totalRead - headerBytes;
                    if (rem > 0) {
                        memmove(outBody, outBody + headerBytes, rem);
                    }
                    totalRead = rem;
                    outBody[totalRead] = '\0';
                    headerStripped = true;
                }
            }
        } else {
            break;
        }
    }

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode    = TCPClose;
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
        NetSetServer(ip, port);
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

Boolean NetDiscoverServer(void)
{
    UDPiopb pb;
    Ptr udpRcvBuf;
    StreamPtr udpStream;
    wdsEntry wds[2];
    const char *magic = "MACCOMPANION_DISCOVER";
    ip_addr targets[4];
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

    targets[0] = (gOurIP & (ip_addr)gOurNetMask) | 2; /* 10.0.2.2 */
    targets[1] = (gOurIP & (ip_addr)gOurNetMask) | 1;
    targets[2] = gOurIP | ~(ip_addr)gOurNetMask;
    targets[3] = (ip_addr)0xFFFFFFFF;

    for (i = 0; i < 4; i++) {
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

    /* Wait up to 2 seconds for reply */
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum               = gMacTCPRef;
    pb.csCode                  = UDPRead;
    pb.udpStream               = udpStream;
    pb.csParam.receive.timeOut = 2;
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

/* --- Reverse Push Channel --- */

static void StartChannelRcv(void)
{
    if (gChannelStream == 0 || gChanRcvPending) return;

    memset(&gChanRcvPB, 0, sizeof(gChanRcvPB));
    gChanRcvPB.ioCRefNum                  = gMacTCPRef;
    gChanRcvPB.csCode                     = TCPRcv;
    gChanRcvPB.tcpStream                  = gChannelStream;
    gChanRcvPB.csParam.receive.rcvBuff    = (Ptr)gChanChunkBuf;
    gChanRcvPB.csParam.receive.rcvBuffLen = CHUNK_BUF_SIZE;
    gChanRcvPB.csParam.receive.commandTimeoutValue = 0; /* non-blocking background wait */
    gChanRcvPB.csParam.receive.secondTimeStamp = 0;
    gChanRcvPB.csParam.receive.urgentFlag = false;
    gChanRcvPB.csParam.receive.markFlag   = false;

    OSErr err = PBControlAsync((ParmBlkPtr)&gChanRcvPB);
    if (err == noErr || gChanRcvPB.ioResult == inProgress) {
        gChanRcvPending = true;
    } else {
        gChanRcvPending = false;
    }
}

void NetStopChannel(void)
{
    gChannelConnected = false;
    gChanRcvPending   = false;

    if (gChannelStream != 0) {
        TCPiopb pb;
        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum                     = gMacTCPRef;
        pb.csCode                        = TCPClose;
        pb.tcpStream                     = gChannelStream;
        pb.csParam.close.ulpTimeoutValue = 1;
        pb.csParam.close.ulpTimeoutAction = 1;
        pb.csParam.close.validityFlags   = timeoutValue | timeoutAction;
        if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
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

Boolean NetConnectChannel(void)
{
    TCPiopb pb;
    OSErr err;

    if (!NetInit() || gServerIP == 0) return false;

    NetStopChannel();

    if (!gChannelRcvBuf) {
        gChannelRcvBuf = NewPtr(CHAN_RCV_BUF_SIZE);
        if (!gChannelRcvBuf) return false;
    }

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                 = gMacTCPRef;
    pb.csCode                    = TCPCreate;
    pb.csParam.create.rcvBuff    = gChannelRcvBuf;
    pb.csParam.create.rcvBuffLen = CHAN_RCV_BUF_SIZE;
    pb.csParam.create.notifyProc = NULL;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) return false;

    gChannelStream = pb.tcpStream;

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
        gListenRcvBuf = NewPtr(LISTEN_RCV_BUF_SIZE);
        if (!gListenRcvBuf) return false;
    }

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                 = gMacTCPRef;
    pb.csCode                    = TCPCreate;
    pb.csParam.create.rcvBuff    = gListenRcvBuf;
    pb.csParam.create.rcvBuffLen = LISTEN_RCV_BUF_SIZE;
    pb.csParam.create.notifyProc = NULL;
    err = PBControlSync((ParmBlkPtr)&pb);
    if (err != noErr) return false;

    gListenStream = pb.tcpStream;

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

void NetIdle(void)
{
    if (!gNetReady) return;

    /* 1. Handle Reverse Push Channel connection lifecycle */
    if (gChannelState == CHAN_DISCONNECTED) {
        if (gServerIP != 0 && (TickCount() - gLastChannelAttemptTick > 180)) {
            gLastChannelAttemptTick = TickCount();
            NetConnectChannel();
        }
    } else if (gChannelState == CHAN_CONNECTING) {
        if (gChanOpenPB.ioResult <= 0) {
            if (gChanOpenPB.ioResult == noErr) {
                /* Connection opened! Send reverse channel HTTP GET request */
                char encName[128];
                char encMach[128];
                char req[512];
                char hostStr[32];
                wdsEntry wds[2];
                TCPiopb sendPB;

                FormatIP(gServerIP, hostStr);
                URLEncode(gClientName, encName, sizeof(encName));
                URLEncode(GetMacMachineName(), encMach, sizeof(encMach));

                sprintf(req,
                    "GET /api/macdrop/channel?id=%s&name=%s&machine=%s&ver=%s HTTP/1.0\r\n"
                    "Host: %s:%u\r\n"
                    "User-Agent: MacDrop-68k/%s\r\n"
                    "Connection: keep-alive\r\n\r\n",
                    gClientID, encName, encMach, APP_VERSION_STR,
                    hostStr, gServerPort, APP_VERSION_STR);

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
            } else {
                /* Connection failed, retry later */
                NetStopChannel();
                gLastChannelAttemptTick = TickCount();
            }
        }
    } else if (gChannelState == CHAN_CONNECTED || gChannelState == CHAN_STREAMING) {
        if (gChanRcvPending && gChanRcvPB.ioResult <= 0) {
            gChanRcvPending = false;

            if (gChanRcvPB.ioResult == noErr && gChanRcvPB.csParam.receive.rcvBuffLen > 0) {
                short nBytes = gChanRcvPB.csParam.receive.rcvBuffLen;
                unsigned char *pData = gChanChunkBuf;

                if (!gHeaderParsed) {
                    short copyLen = nBytes;
                    if (gHttpHeaderLen + copyLen > (short)(sizeof(gHttpHeaderBuf) - 1)) {
                        copyLen = (short)(sizeof(gHttpHeaderBuf) - 1 - gHttpHeaderLen);
                    }
                    memcpy(gHttpHeaderBuf + gHttpHeaderLen, pData, copyLen);
                    gHttpHeaderLen += copyLen;
                    gHttpHeaderBuf[gHttpHeaderLen] = '\0';

                    char *delim = strstr(gHttpHeaderBuf, "\r\n\r\n");
                    short delimLen = 4;
                    if (!delim) {
                        delim = strstr(gHttpHeaderBuf, "\n\n");
                        delimLen = 2;
                    }

                    if (delim) {
                        /* Check if this is an idle keepalive poll response (204 No Content or X-MacDrop-Status: POLL) */
                        if (strstr(gHttpHeaderBuf, " 204 ") != NULL ||
                            strstr(gHttpHeaderBuf, "X-MacDrop-Status: POLL") != NULL) {
                            NetStopChannel();
                            gLastChannelAttemptTick = 0; /* reconnect immediately for next poll */
                            return;
                        }

                        gHeaderParsed = true;
                        gChannelState = CHAN_STREAMING;
                        gTransferActive = true;
                        MacBinaryRx_Init(&gChannelRx);
                        gActiveTransferName[0] = '\0';
                        gActiveTransferBytes = 0;
                        gActiveTransferTotal = 0;
                        gCurrentTransferId[0] = '\0';

                        /* Extract Transfer ID for ACK */
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

                        long headerBytes = (delim - gHttpHeaderBuf) + delimLen;
                        if (headerBytes < nBytes) {
                            long payloadBytes = nBytes - headerBytes;
                            MacBinaryRx_ProcessBytes(&gChannelRx, pData + headerBytes, payloadBytes);
                            if (gChannelRx.fileName[0] != '\0') {
                                snprintf(gActiveTransferName, sizeof(gActiveTransferName), "%s", gChannelRx.fileName);
                            }
                            gActiveTransferTotal = gChannelRx.dataLen + gChannelRx.rsrcLen;
                            gActiveTransferBytes = gChannelRx.dataBytesWritten + gChannelRx.rsrcBytesWritten;
                        }
                    }
                } else {
                    MacBinaryRx_ProcessBytes(&gChannelRx, pData, nBytes);
                    if (gChannelRx.fileName[0] != '\0') {
                        snprintf(gActiveTransferName, sizeof(gActiveTransferName), "%s", gChannelRx.fileName);
                    }
                    gActiveTransferTotal = gChannelRx.dataLen + gChannelRx.rsrcLen;
                    gActiveTransferBytes = gChannelRx.dataBytesWritten + gChannelRx.rsrcBytesWritten;

                    if (gChannelRx.state == RX_COMPLETE) {
                        MacBinaryRx_Finish(&gChannelRx);
                        TriggerTransferNotification(gChannelRx.fileName);
                        gTransferActive = false;
                        NetSendTransferAck(gCurrentTransferId, "OK", gChannelRx.dataBytesWritten + gChannelRx.rsrcBytesWritten);
                        gCurrentTransferId[0] = '\0';
                        NetStopChannel();
                        gLastChannelAttemptTick = 0; /* reconnect immediately for next file */
                        return;
                    }
                }

                StartChannelRcv();
            } else {
                /* Connection closed by server or error occurred */
                if (gTransferActive) {
                    if (gChannelRx.state == RX_COMPLETE ||
                        (gChannelRx.dataLen > 0 && gChannelRx.dataBytesWritten >= gChannelRx.dataLen)) {
                        MacBinaryRx_Finish(&gChannelRx);
                        TriggerTransferNotification(gChannelRx.fileName);
                        NetSendTransferAck(gCurrentTransferId, "OK", gChannelRx.dataBytesWritten + gChannelRx.rsrcBytesWritten);
                    } else {
                        MacBinaryRx_Abort(&gChannelRx);
                        NetSendTransferAck(gCurrentTransferId, "ERROR", 0);
                    }
                    gCurrentTransferId[0] = '\0';
                    gTransferActive = false;
                }
                NetStopChannel();
                gLastChannelAttemptTick = 0; /* reconnect immediately */
            }
        }
    }

    /* 2. Handle Direct TCP Passive Listener on Port 6803 */
    if (gListenerActive && !gTransferActive) {
        if (gPassivePB.ioResult <= 0) {
            if (gPassivePB.ioResult == noErr) {
                /* Direct incoming connection */
                gTransferActive = true;
                MacBinaryRx_Init(&gPassiveRx);
                gActiveTransferName[0] = '\0';
                gActiveTransferBytes = 0;
                gActiveTransferTotal = 0;

                /* Read from direct connection until done */
                while (1) {
                    TCPiopb rcvPB;
                    memset(&rcvPB, 0, sizeof(rcvPB));
                    rcvPB.ioCRefNum                     = gMacTCPRef;
                    rcvPB.csCode                        = TCPRcv;
                    rcvPB.tcpStream                     = gListenStream;
                    rcvPB.csParam.receive.rcvBuff       = (Ptr)gListenChunkBuf;
                    rcvPB.csParam.receive.rcvBuffLen    = CHUNK_BUF_SIZE;
                    rcvPB.csParam.receive.commandTimeoutValue = 1;
                    rcvPB.csParam.receive.secondTimeStamp = 0;
                    rcvPB.csParam.receive.urgentFlag    = false;
                    rcvPB.csParam.receive.markFlag      = false;

                    OSErr rErr = PBControlSync((ParmBlkPtr)&rcvPB);
                    if (rErr == noErr && rcvPB.csParam.receive.rcvBuffLen > 0) {
                        short bytes = rcvPB.csParam.receive.rcvBuffLen;
                        MacBinaryRx_ProcessBytes(&gPassiveRx, gListenChunkBuf, bytes);
                        if (gPassiveRx.fileName[0] != '\0') {
                            snprintf(gActiveTransferName, sizeof(gActiveTransferName), "%s", gPassiveRx.fileName);
                        }
                        gActiveTransferTotal = gPassiveRx.dataLen + gPassiveRx.rsrcLen;
                        gActiveTransferBytes = gPassiveRx.dataBytesWritten + gPassiveRx.rsrcBytesWritten;

                        if (gPassiveRx.state == RX_COMPLETE) {
                            MacBinaryRx_Finish(&gPassiveRx);
                            TriggerTransferNotification(gPassiveRx.fileName);
                            gTransferActive = false;

                            /* Send ACK back to server over the direct TCP socket */
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
                            break;
                        }
                    } else {
                        break;
                    }
                }

                if (gPassiveRx.state != RX_COMPLETE) {
                    MacBinaryRx_Abort(&gPassiveRx);
                    gTransferActive = false;
                }

                /* Close socket and restart passive listening */
                NetStopListener();
                NetStartListener();
            } else {
                /* Passive open failed or aborted, restart */
                NetStopListener();
                NetStartListener();
            }
        }
    }
}
