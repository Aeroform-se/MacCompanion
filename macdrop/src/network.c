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
Boolean         gTransferIsUpload    = false;
char            gActiveTransferName[64] = "";
unsigned long   gActiveTransferBytes = 0;
unsigned long   gActiveTransferTotal = 0;
unsigned long   gTransferSpeedBps    = 0;
char            gCurrentTransferId[64] = "";
Rect            gBtnCancelTransferRect;
static Boolean  gCancelUpload        = false;

#define LISTEN_RCV_BUF_SIZE 65536
#define CHAN_RCV_BUF_SIZE   65536
#define FALLBACK_RCV_SIZE   16384
#define CHUNK_BUF_SIZE      32768

/* Passive listener on port 6803 */
static Ptr          gListenRcvBuf    = NULL;
static long         gListenRcvBufLen = 0;
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

static void TriggerUploadNotification(const char *fileName)
{
    if (gSoundEnabled) {
        SysBeep(30);
    }

    if (gNotifyEnabled) {
        static Str255 notifyMsg;
        short len = sprintf((char *)&notifyMsg[1], "Sent: %s", fileName);
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
            if (headerStripped && totalRead > 0) {
                break;
            }
        } else {
            break;
        }
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

extern void UpdateProgressUI(void);

void NetCancelActiveTransfer(void)
{
    if (!gTransferActive) return;

    if (gTransferIsUpload) {
        gCancelUpload = true;
    } else {
        if (gChannelState == CHAN_STREAMING) {
            MacBinaryRx_Abort(&gChannelRx);
            if (gCurrentTransferId[0] != '\0') {
                NetSendTransferAck(gCurrentTransferId, "CANCELLED", 0);
                gCurrentTransferId[0] = '\0';
            }
            NetStopChannel();
            gLastChannelAttemptTick = 0; /* prompt immediate reconnect for next transfer */
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

Boolean NetSendFile(const FSSpec *spec)
{
    MacBinaryTx tx;
    TCPiopb pb;
    TCPiopb sendPB;
    StreamPtr stream;
    Ptr tcpRcvBuf;
    char req[1024];
    char hostStr[32];
    char ourIpStr[32];
    char encId[128];
    char encName[128];
    wdsEntry wds[2];
    OSErr err;
    unsigned char *chunkBufA = NULL;
    unsigned char *chunkBufB = NULL;
    long chunkBufSize = 24576;
    unsigned long startTicks, lastSpeedTicks, lastUiTicks, lastSysTicks, speedBytes;
    unsigned long totalSize;
    long curBytes = 0, nextBytes = 0;
    unsigned char *curPtr = NULL;
    unsigned char *nextPtr = NULL;
    extern void UpdateProgressUI(void);

    if (!spec || !NetInit() || gServerIP == 0) return false;

    /* Initialize MacBinaryTx encoder */
    err = MacBinaryTx_Init(&tx, spec);
    if (err != noErr) {
        return false;
    }

    totalSize = 128 + tx.dataBytesTarget + tx.rsrcBytesTarget;

    tcpRcvBuf = NewPtr(32768);
    if (!tcpRcvBuf) {
        tcpRcvBuf = NewPtr(16384);
        if (!tcpRcvBuf) {
            tcpRcvBuf = NewPtr(4096);
            if (!tcpRcvBuf) {
                MacBinaryTx_Abort(&tx);
                return false;
            }
        }
    }

    chunkBufA = (unsigned char *)NewPtr(24576);
    chunkBufB = (unsigned char *)NewPtr(24576);
    if (!chunkBufA || !chunkBufB) {
        if (chunkBufA) { DisposePtr((Ptr)chunkBufA); chunkBufA = NULL; }
        if (chunkBufB) { DisposePtr((Ptr)chunkBufB); chunkBufB = NULL; }
        chunkBufSize = 16384;
        chunkBufA = (unsigned char *)NewPtr(16384);
        chunkBufB = (unsigned char *)NewPtr(16384);
        if (!chunkBufA || !chunkBufB) {
            if (chunkBufA) { DisposePtr((Ptr)chunkBufA); chunkBufA = NULL; }
            if (chunkBufB) { DisposePtr((Ptr)chunkBufB); chunkBufB = NULL; }
            chunkBufSize = 8192;
            chunkBufA = (unsigned char *)NewPtr(8192);
            if (!chunkBufA) {
                DisposePtr(tcpRcvBuf);
                MacBinaryTx_Abort(&tx);
                return false;
            }
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
        if (chunkBufA) DisposePtr((Ptr)chunkBufA);
        if (chunkBufB) DisposePtr((Ptr)chunkBufB);
        DisposePtr(tcpRcvBuf);
        MacBinaryTx_Abort(&tx);
        return false;
    }
    stream = pb.tcpStream;

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
    startTicks = TickCount();
    lastSysTicks = startTicks;
    while (pb.ioResult > 0 && (TickCount() - startTicks) < 360) {
        unsigned long t = TickCount();
        if ((t - lastSysTicks) >= 2) {
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
        if (chunkBufA) DisposePtr((Ptr)chunkBufA);
        if (chunkBufB) DisposePtr((Ptr)chunkBufB);
        DisposePtr(tcpRcvBuf);
        MacBinaryTx_Abort(&tx);
        return false;
    }

    FormatIP(gServerIP, hostStr);
    FormatIP(gOurIP, ourIpStr);
    URLEncode(gClientID, encId, sizeof(encId));
    URLEncode(gClientName, encName, sizeof(encName));

    sprintf(req,
        "POST /api/macdrop/inbound/upload?id=%s&name=%s&ip=%s HTTP/1.0\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: MacDrop-68k\r\n"
        "Content-Type: application/x-macbinary\r\n"
        "Content-Length: %lu\r\n"
        "X-Client-Id: %s\r\n"
        "X-Client-Name: %s\r\n"
        "X-Client-IP: %s\r\n"
        "X-Filename: %s\r\n"
        "Connection: close\r\n\r\n",
        encId, encName, ourIpStr, hostStr, gServerPort, totalSize,
        gClientID, gClientName, ourIpStr, tx.fileName);

    wds[0].length = (unsigned short)strlen(req);
    wds[0].ptr    = (Ptr)req;
    wds[1].length = 0;
    wds[1].ptr    = NULL;

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum                        = gMacTCPRef;
    pb.csCode                           = TCPSend;
    pb.tcpStream                        = stream;
    pb.csParam.send.ulpTimeoutValue     = 10;
    pb.csParam.send.ulpTimeoutAction    = 1;
    pb.csParam.send.validityFlags       = timeoutValue | timeoutAction;
    pb.csParam.send.pushFlag            = true;
    pb.csParam.send.urgentFlag          = false;
    pb.csParam.send.wdsPtr              = (Ptr)wds;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
        goto abort_transfer;
    }

    /* Set global transfer state */
    gTransferActive      = true;
    gTransferIsUpload    = true;
    gCancelUpload        = false;
    gActiveTransferBytes = 0;
    gActiveTransferTotal = totalSize;
    gTransferSpeedBps    = 0;
    strncpy(gActiveTransferName, tx.fileName, sizeof(gActiveTransferName) - 1);
    gActiveTransferName[sizeof(gActiveTransferName) - 1] = '\0';

    lastSpeedTicks = TickCount();
    lastUiTicks    = lastSpeedTicks;
    lastSysTicks   = lastSpeedTicks;
    speedBytes     = 0;

    UpdateProgressUI();

    /* Prime first buffer from disk */
    curPtr = chunkBufA;
    nextPtr = chunkBufB ? chunkBufB : chunkBufA;
    curBytes = MacBinaryTx_ReadBytes(&tx, curPtr, chunkBufSize);

    /* Pipelined double-buffered streaming loop:
     * Overlaps asynchronous MacTCP transmission of curPtr with HFS disk read into nextPtr */
    while (curBytes > 0) {
        unsigned long sendStartTicks;

        if (gCancelUpload) {
            goto abort_transfer;
        }

        wds[0].length = (unsigned short)curBytes;
        wds[0].ptr    = (Ptr)curPtr;
        wds[1].length = 0;
        wds[1].ptr    = NULL;

        memset(&sendPB, 0, sizeof(sendPB));
        sendPB.ioCRefNum                        = gMacTCPRef;
        sendPB.csCode                           = TCPSend;
        sendPB.tcpStream                        = stream;
        sendPB.csParam.send.ulpTimeoutValue     = 15;
        sendPB.csParam.send.ulpTimeoutAction    = 1;
        sendPB.csParam.send.validityFlags       = timeoutValue | timeoutAction;
        sendPB.csParam.send.pushFlag            = true; /* Immediate PSH avoids 200ms delayed-ACK stall */
        sendPB.csParam.send.urgentFlag          = false;
        sendPB.csParam.send.wdsPtr              = (Ptr)wds;

        err = PBControlAsync((ParmBlkPtr)&sendPB);

        /* While MacTCP transmits curPtr in the background, read the next chunk from HFS disk if double-buffered! */
        nextBytes = 0;
        if (chunkBufB && tx.state != TX_COMPLETE && tx.state != TX_ERROR) {
            nextBytes = MacBinaryTx_ReadBytes(&tx, nextPtr, chunkBufSize);
        }

        /* Wait for current TCPSend to complete without hammering SystemTask on every spin */
        sendStartTicks = TickCount();
        while (sendPB.ioResult > 0) {
            unsigned long t = TickCount();
            if ((t - sendStartTicks) > 900) {
                goto abort_transfer;
            }
            if ((t - lastSysTicks) >= 2) {
                lastSysTicks = t;
                SystemTask();
            }
        }
        if (sendPB.ioResult != noErr) {
            goto abort_transfer;
        }

        /* If single-buffered fallback, read next chunk now */
        if (!chunkBufB && tx.state != TX_COMPLETE && tx.state != TX_ERROR) {
            nextBytes = MacBinaryTx_ReadBytes(&tx, curPtr, chunkBufSize);
        }

        gActiveTransferBytes += curBytes;
        speedBytes += curBytes;

        unsigned long nowTicks = TickCount();
        if ((nowTicks - lastSpeedTicks) >= 20) {
            unsigned long ticks = nowTicks - lastSpeedTicks;
            gTransferSpeedBps = (speedBytes * 60) / ticks;
            lastSpeedTicks = nowTicks;
            speedBytes = 0;
        }

        /* Throttle UI redraw & event polling to ~5 Hz (every 12 ticks) so CPU stays on I/O */
        if ((nowTicks - lastUiTicks) >= 12 || nextBytes <= 0) {
            EventRecord evt;
            lastUiTicks = nowTicks;
            UpdateProgressUI();
            if (GetNextEvent(mDownMask | keyDownMask | autoKeyMask, &evt)) {
                if (evt.what == mouseDown) {
                    extern WindowRef gWindow;
                    WindowPtr win;
                    short part = FindWindow(evt.where, &win);
                    if (part == inContent && win == gWindow) {
                        Point localPt = evt.where;
                        SetPort(gWindow);
                        GlobalToLocal(&localPt);
                        if (PtInRect(localPt, &gBtnCancelTransferRect)) {
                            goto abort_transfer;
                        }
                    }
                } else if (evt.what == keyDown || evt.what == autoKey) {
                    char key = (char)(evt.message & charCodeMask);
                    if (((evt.modifiers & cmdKey) && key == '.') || key == 27) {
                        goto abort_transfer;
                    }
                }
            }
        }

        if (chunkBufB) {
            unsigned char *tmp = curPtr;
            curPtr = nextPtr;
            nextPtr = tmp;
        }
        curBytes = nextBytes;
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

    if (chunkBufA) DisposePtr((Ptr)chunkBufA);
    if (chunkBufB) DisposePtr((Ptr)chunkBufB);
    DisposePtr(tcpRcvBuf);

    gTransferActive   = false;
    gTransferIsUpload = false;

    /* Refresh reverse push channel immediately so client stays online after long uploads */
    NetConnectChannel();

    TriggerUploadNotification(tx.fileName);
    UpdateProgressUI();
    return true;

abort_transfer:
    MacBinaryTx_Abort(&tx);
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode    = TCPAbort;
    pb.tcpStream = stream;
    PBControlSync((ParmBlkPtr)&pb);

    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = gMacTCPRef;
    pb.csCode    = TCPRelease;
    pb.tcpStream = stream;
    PBControlSync((ParmBlkPtr)&pb);

    if (chunkBufA) DisposePtr((Ptr)chunkBufA);
    if (chunkBufB) DisposePtr((Ptr)chunkBufB);
    DisposePtr(tcpRcvBuf);

    gTransferActive   = false;
    gTransferIsUpload = false;

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

    short numTargets = 0;
    if ((gOurIP & (ip_addr)0xFFFFFF00UL) == (ip_addr)0x0A000200UL) {
        targets[numTargets++] = (gOurIP & (ip_addr)gOurNetMask) | 2; /* 10.0.2.2 SLIRP host */
    }
    targets[numTargets++] = (gOurIP & (ip_addr)gOurNetMask) | 1;
    targets[numTargets++] = gOurIP | ~(ip_addr)gOurNetMask;
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

/* --- Reverse Push Channel --- */

static void StartChannelRcv(void)
{
    unsigned short askLen = CHUNK_BUF_SIZE;

    if (gChannelStream == 0 || gChanRcvPending) return;

    if (gChannelState == CHAN_STREAMING && gChannelRx.headerBytesRead == 128) {
        unsigned long remData = (gChannelRx.dataBytesTarget > gChannelRx.dataBytesPadded)
                                ? (gChannelRx.dataBytesTarget - gChannelRx.dataBytesPadded) : 0;
        unsigned long remRsrc = (gChannelRx.rsrcBytesTarget > gChannelRx.rsrcBytesPadded)
                                ? (gChannelRx.rsrcBytesTarget - gChannelRx.rsrcBytesPadded) : 0;
        unsigned long remTotal = remData + remRsrc;
        if (remTotal > 0 && remTotal < (unsigned long)CHUNK_BUF_SIZE) {
            askLen = (unsigned short)remTotal;
        }
    }

    memset(&gChanRcvPB, 0, sizeof(gChanRcvPB));
    gChanRcvPB.ioCRefNum                  = gMacTCPRef;
    gChanRcvPB.csCode                     = TCPRcv;
    gChanRcvPB.tcpStream                  = gChannelStream;
    gChanRcvPB.csParam.receive.rcvBuff    = (Ptr)gChanChunkBuf;
    gChanRcvPB.csParam.receive.rcvBuffLen = askLen;
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
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode    = TCPAbort;
        pb.tcpStream = gChannelStream;
        PBControlSync((ParmBlkPtr)&pb);

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
        gChannelRcvBufLen = CHAN_RCV_BUF_SIZE;
        gChannelRcvBuf = NewPtr(gChannelRcvBufLen);
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
        gListenRcvBufLen = LISTEN_RCV_BUF_SIZE;
        gListenRcvBuf = NewPtr(gListenRcvBufLen);
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
        unsigned long nowTicks = TickCount();

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
            } else {
                /* Connection failed, retry later */
                NetStopChannel();
                gLastChannelAttemptTick = TickCount();
            }
        }
    } else if (gChannelState == CHAN_CONNECTED || gChannelState == CHAN_STREAMING) {
        unsigned long lastDataTick = TickCount();
        unsigned long lastSysTick  = lastDataTick;
        unsigned long lastUiTick   = lastDataTick;
        extern void UpdateProgressUI(void);
        extern WindowRef gWindow;

        while (gChannelState == CHAN_CONNECTED || gChannelState == CHAN_STREAMING) {
            if (!gChanRcvPending) {
                break;
            }
            if (gChanRcvPB.ioResult > 0) {
                /* While actively streaming a file, stay inside NetIdle without yielding to
                 * WaitNextEvent() (which incurs a 16.6ms MultiFinder context switch per chunk!) */
                if (gChannelState == CHAN_STREAMING) {
                    unsigned long nowTick = TickCount();
                    if ((nowTick - lastDataTick) > 1200) {
                        /* Stalled for >20s with no packets */
                        break;
                    }
                    if ((nowTick - lastSysTick) >= 2) {
                        lastSysTick = nowTick;
                        SystemTask();
                    }
                    if ((nowTick - lastUiTick) >= 12) {
                        EventRecord evt;
                        lastUiTick = nowTick;
                        UpdateProgressUI();
                        if (GetNextEvent(mDownMask | keyDownMask | autoKeyMask, &evt)) {
                            if (evt.what == mouseDown) {
                                WindowPtr win;
                                short part = FindWindow(evt.where, &win);
                                if (part == inContent && win == gWindow) {
                                    Point localPt = evt.where;
                                    SetPort(gWindow);
                                    GlobalToLocal(&localPt);
                                    if (PtInRect(localPt, &gBtnCancelTransferRect)) {
                                        NetCancelActiveTransfer();
                                        UpdateProgressUI();
                                        return;
                                    }
                                }
                            } else if (evt.what == keyDown || evt.what == autoKey) {
                                char key = (char)(evt.message & charCodeMask);
                                if (((evt.modifiers & cmdKey) && key == '.') || key == 27) {
                                    NetCancelActiveTransfer();
                                    UpdateProgressUI();
                                    return;
                                }
                            }
                        }
                    }
                    continue;
                }
                break;
            }
            gChanRcvPending = false;
            lastDataTick = TickCount();

            if (gChanRcvPB.ioResult == noErr && gChanRcvPB.csParam.receive.rcvBuffLen > 0) {
                long nBytes = (unsigned short)gChanRcvPB.csParam.receive.rcvBuffLen;
                unsigned char *pData = gChanChunkBuf;

                if (!gHeaderParsed) {
                    long copyLen = nBytes;
                    if (gHttpHeaderLen + copyLen > (long)(sizeof(gHttpHeaderBuf) - 1)) {
                        copyLen = (long)(sizeof(gHttpHeaderBuf) - 1 - gHttpHeaderLen);
                    }
                    memcpy(gHttpHeaderBuf + gHttpHeaderLen, pData, (size_t)copyLen);
                    gHttpHeaderLen += (short)copyLen;
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
                        gTransferIsUpload = false;
                        MacBinaryRx_Init(&gChannelRx);
                        gActiveTransferName[0] = '\0';
                        gActiveTransferBytes = 0;
                        gActiveTransferTotal = 0;
                        gTransferSpeedBps    = 0;
                        gRxLastSpeedTicks    = TickCount();
                        gRxLastSpeedBytes    = 0;
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

                        if (gWindow && !IsWindowVisible(gWindow)) {
                            ShowWindow(gWindow);
                            SelectWindow(gWindow);
                        }
                        UpdateProgressUI();
                        lastUiTick = TickCount();
                    }
                } else {
                    MacBinaryRx_ProcessBytes(&gChannelRx, pData, nBytes);
                    if (gChannelRx.fileName[0] != '\0') {
                        snprintf(gActiveTransferName, sizeof(gActiveTransferName), "%s", gChannelRx.fileName);
                    }
                    gActiveTransferTotal = gChannelRx.dataLen + gChannelRx.rsrcLen;
                    gActiveTransferBytes = gChannelRx.dataBytesWritten + gChannelRx.rsrcBytesWritten;
                }

                if (gChannelState == CHAN_STREAMING) {
                    unsigned long nowTicks = TickCount();
                    if ((nowTicks - gRxLastSpeedTicks) >= 20) {
                        unsigned long elapsedTicks = nowTicks - gRxLastSpeedTicks;
                        unsigned long bytesDelta = gActiveTransferBytes - gRxLastSpeedBytes;
                        gTransferSpeedBps = (bytesDelta * 60) / elapsedTicks;
                        gRxLastSpeedTicks = nowTicks;
                        gRxLastSpeedBytes = gActiveTransferBytes;
                    }

                    if ((nowTicks - lastUiTick) >= 12) {
                        EventRecord evt;
                        lastUiTick = nowTicks;
                        UpdateProgressUI();
                        if (GetNextEvent(mDownMask | keyDownMask | autoKeyMask, &evt)) {
                            if (evt.what == mouseDown) {
                                WindowPtr win;
                                short part = FindWindow(evt.where, &win);
                                if (part == inContent && win == gWindow) {
                                    Point localPt = evt.where;
                                    SetPort(gWindow);
                                    GlobalToLocal(&localPt);
                                    if (PtInRect(localPt, &gBtnCancelTransferRect)) {
                                        NetCancelActiveTransfer();
                                        UpdateProgressUI();
                                        return;
                                    }
                                }
                            } else if (evt.what == keyDown || evt.what == autoKey) {
                                char key = (char)(evt.message & charCodeMask);
                                if (((evt.modifiers & cmdKey) && key == '.') || key == 27) {
                                    NetCancelActiveTransfer();
                                    UpdateProgressUI();
                                    return;
                                }
                            }
                        }
                    }
                }

                if (gChannelState == CHAN_STREAMING && gChannelRx.state == RX_COMPLETE) {
                    MacBinaryRx_Finish(&gChannelRx);
                    NetStopChannel();
                    NetSendTransferAck(gCurrentTransferId, "OK", gChannelRx.dataBytesWritten + gChannelRx.rsrcBytesWritten);
                    MacBinaryRx_FlushDestVolume();
                    TriggerTransferNotification(gChannelRx.fileName);
                    gTransferActive = false;
                    gCurrentTransferId[0] = '\0';
                    gLastChannelAttemptTick = 0;
                    NetConnectChannel(); /* reconnect immediately for next file */
                    UpdateProgressUI();
                    return;
                }

                StartChannelRcv();
            } else {
                /* Connection closed by server or error occurred */
                if (gTransferActive) {
                    if (gChannelRx.state == RX_COMPLETE ||
                        (gChannelRx.dataLen > 0 && gChannelRx.dataBytesWritten >= gChannelRx.dataLen)) {
                        MacBinaryRx_Finish(&gChannelRx);
                        NetStopChannel();
                        NetSendTransferAck(gCurrentTransferId, "OK", gChannelRx.dataBytesWritten + gChannelRx.rsrcBytesWritten);
                        MacBinaryRx_FlushDestVolume();
                        TriggerTransferNotification(gChannelRx.fileName);
                    } else {
                        MacBinaryRx_Abort(&gChannelRx);
                        NetStopChannel();
                        NetSendTransferAck(gCurrentTransferId, "ERROR", 0);
                    }
                    gCurrentTransferId[0] = '\0';
                    gTransferActive = false;
                } else {
                    NetStopChannel();
                }
                gLastChannelAttemptTick = 0; /* reconnect immediately */
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

                /* Read first chunk to distinguish a server ping/discovery probe from a file transfer */
                memset(&rcvPB, 0, sizeof(rcvPB));
                rcvPB.ioCRefNum                        = gMacTCPRef;
                rcvPB.csCode                           = TCPRcv;
                rcvPB.tcpStream                        = gListenStream;
                rcvPB.csParam.receive.rcvBuff          = (Ptr)gListenChunkBuf;
                rcvPB.csParam.receive.rcvBuffLen       = CHUNK_BUF_SIZE;
                rcvPB.csParam.receive.commandTimeoutValue = 1;
                rcvPB.csParam.receive.secondTimeStamp  = 0;
                rcvPB.csParam.receive.urgentFlag       = false;
                rcvPB.csParam.receive.markFlag         = false;

                rErr = PBControlSync((ParmBlkPtr)&rcvPB);

                if (rErr != noErr || rcvPB.csParam.receive.rcvBuffLen == 0 ||
                    (rcvPB.csParam.receive.rcvBuffLen >= 13 && strncmp((char *)gListenChunkBuf, "MACDROP_HELLO", 13) == 0)) {
                    /* Server Ping / Auto-Discovery Probe! Reply with MACDROP_PONG and auto-pair if needed */
                    ip_addr discoveredIP = remotePeerIP;
                    unsigned short discoveredPort = DEFAULT_HTTP_PORT;

                    if (rErr == noErr && rcvPB.csParam.receive.rcvBuffLen >= 14 &&
                        strncmp((char *)gListenChunkBuf, "MACDROP_HELLO|", 14) == 0) {
                        char helloBuf[128];
                        short hLen = rcvPB.csParam.receive.rcvBuffLen;
                        if (hLen > (short)(sizeof(helloBuf) - 1)) hLen = sizeof(helloBuf) - 1;
                        memcpy(helloBuf, gListenChunkBuf, hLen);
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
                            /* In Basilisk II SLIRP (10.0.2.x), keep the on-subnet 10.0.2.2 peer IP */
                            if (!((gOurIP & 0xFFFFFF00UL) == 0x0A000200UL && (remotePeerIP & 0xFFFFFF00UL) == 0x0A000200UL)) {
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

                    /* If we aren't connected to the server yet, auto-connect our reverse channel to this server! */
                    if (!gChannelConnected && discoveredIP != 0) {
                        NetSetServer(discoveredIP, discoveredPort);
                        NetConnectChannel();
                    }
                    return;
                }

                /* Direct incoming MacBinary file transfer */
                gTransferActive = true;
                gTransferIsUpload = false;
                MacBinaryRx_Init(&gPassiveRx);
                gActiveTransferName[0] = '\0';
                gActiveTransferBytes = 0;
                gActiveTransferTotal = 0;
                gTransferSpeedBps    = 0;
                gRxLastSpeedTicks    = TickCount();
                gRxLastSpeedBytes    = 0;

                /* Process the first chunk already read */
                MacBinaryRx_ProcessBytes(&gPassiveRx, gListenChunkBuf, (unsigned short)rcvPB.csParam.receive.rcvBuffLen);
                if (gPassiveRx.fileName[0] != '\0') {
                    snprintf(gActiveTransferName, sizeof(gActiveTransferName), "%s", gPassiveRx.fileName);
                }
                gActiveTransferTotal = gPassiveRx.dataLen + gPassiveRx.rsrcLen;
                gActiveTransferBytes = gPassiveRx.dataBytesWritten + gPassiveRx.rsrcBytesWritten;

                /* Read remaining chunks from direct connection until done */
                while (gPassiveRx.state != RX_COMPLETE && gPassiveRx.state != RX_ERROR) {
                    unsigned short askLen = CHUNK_BUF_SIZE;
                    if (gPassiveRx.headerBytesRead == 128) {
                        unsigned long remData = (gPassiveRx.dataBytesTarget > gPassiveRx.dataBytesPadded)
                                                ? (gPassiveRx.dataBytesTarget - gPassiveRx.dataBytesPadded) : 0;
                        unsigned long remRsrc = (gPassiveRx.rsrcBytesTarget > gPassiveRx.rsrcBytesPadded)
                                                ? (gPassiveRx.rsrcBytesTarget - gPassiveRx.rsrcBytesPadded) : 0;
                        unsigned long remTotal = remData + remRsrc;
                        if (remTotal > 0 && remTotal < (unsigned long)CHUNK_BUF_SIZE) {
                            askLen = (unsigned short)remTotal;
                        }
                    }

                    memset(&rcvPB, 0, sizeof(rcvPB));
                    rcvPB.ioCRefNum                     = gMacTCPRef;
                    rcvPB.csCode                        = TCPRcv;
                    rcvPB.tcpStream                     = gListenStream;
                    rcvPB.csParam.receive.rcvBuff       = (Ptr)gListenChunkBuf;
                    rcvPB.csParam.receive.rcvBuffLen    = askLen;
                    rcvPB.csParam.receive.commandTimeoutValue = 2;
                    rcvPB.csParam.receive.secondTimeStamp = 0;
                    rcvPB.csParam.receive.urgentFlag    = false;
                    rcvPB.csParam.receive.markFlag      = false;

                    rErr = PBControlSync((ParmBlkPtr)&rcvPB);
                    if (rErr == noErr && rcvPB.csParam.receive.rcvBuffLen > 0) {
                        long bytes = (unsigned short)rcvPB.csParam.receive.rcvBuffLen;
                        MacBinaryRx_ProcessBytes(&gPassiveRx, gListenChunkBuf, bytes);
                        if (gPassiveRx.fileName[0] != '\0') {
                            snprintf(gActiveTransferName, sizeof(gActiveTransferName), "%s", gPassiveRx.fileName);
                        }
                        gActiveTransferTotal = gPassiveRx.dataLen + gPassiveRx.rsrcLen;
                        gActiveTransferBytes = gPassiveRx.dataBytesWritten + gPassiveRx.rsrcBytesWritten;
                    } else {
                        break;
                    }
                }

                if (gPassiveRx.state == RX_COMPLETE) {
                    MacBinaryRx_Finish(&gPassiveRx);

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

                    MacBinaryRx_FlushDestVolume();
                    TriggerTransferNotification(gPassiveRx.fileName);
                    gTransferActive = false;
                } else {
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
