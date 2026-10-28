#include "network.h"
#include <Devices.h>
#include <Memory.h>
#include <Events.h>
#include <Sound.h>
#include <Gestalt.h>
#include <OSUtils.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static short          gMacTCPRef      = 0;
static Boolean        gMacTCPReady    = false;
static ip_addr        gOurIP          = 0;
static long           gOurNetMask     = (long)0xFFFFFF00;

static ip_addr        gServerIP       = 0;
static unsigned short gServerHttpPort = DEFAULT_HTTP_PORT;
static void         (*gIdleCallback)(void) = NULL;
static Boolean        gAbortRequested = false;

/* ============================================================================
 * Native 68k Audio Streaming State (Double-Buffered Sound Manager for TTS)
 * ============================================================================ */

#define AUDIO_TCP_RCV_SIZE  32768
#define AUDIO_CHUNK_SIZE    8192
#define NUM_AUDIO_BUFFERS   2

static StreamPtr      gAudioStream       = 0;
static Ptr            gAudioTcpRcvBuf    = NULL;
static Boolean        gAudioRunning      = false;
static unsigned short gAudioSampleRateHz = 22255;

static SndChannelPtr  gSndChan           = NULL;
static SndCallBackUPP gSndCallBackUPP    = NULL;
static SoundHeader    gSndHeaders[NUM_AUDIO_BUFFERS];
static Ptr            gPcmBuffers[NUM_AUDIO_BUFFERS]      = { NULL, NULL };
static long           gPcmFilled[NUM_AUDIO_BUFFERS]       = { 0, 0 };
static volatile short gBufferBusy[NUM_AUDIO_BUFFERS]      = { 0, 0 };
static short          gNextFillBuf       = 0;
static Boolean        gPrebuffering      = true;
static unsigned long  gLastEmptyStatusTick = 0;

/* ============================================================================
 * Sound Input Manager (Microphone) State
 * ============================================================================ */

#define VOICE_MAX_BYTES     45000L  /* ~4 seconds at 11127 Hz 8-bit mono */
#define kSIReadPermission   0
#define kSIWritePermission  1

typedef struct MacAISPB {
    long            inRefNum;
    unsigned long   count;
    unsigned long   milliseconds;
    unsigned long   bufferLength;
    Ptr             bufferPtr;
    ProcPtr         completionRoutine;
    ProcPtr         interruptRoutine;
    long            userLong;
    OSErr           error;
    long            unused1;
} MacAISPB;

static Boolean        gSoundInputChecked = false;
static Boolean        gSoundInputPresent = false;
static Boolean        gVoiceRecording    = false;
static long           gVoiceDevRef       = 0;
static Ptr            gVoiceBuffer       = NULL;
static unsigned short gVoiceSampleRate   = 11127;
static unsigned long  gVoiceStartTick    = 0;
static MacAISPB       gVoiceSPB;

static pascal void SoundBufferCallback(SndChannelPtr chan, SndCommand *cmd)
{
    (void)chan;
    if (cmd && cmd->param2 != 0) {
        volatile short *busyFlag = (volatile short *)cmd->param2;
        *busyFlag = 0;
    }
}

Boolean NetInit(void)
{
    OSErr err;
    GetAddrParamBlock addrPB;

    if (gMacTCPReady) return true;

    err = OpenDriver("\p.ipp", &gMacTCPRef);
    if (err != noErr) {
        gMacTCPReady = false;
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
        gOurIP      = 0x0A00020F;
        gOurNetMask = (long)0xFFFFFF00;
    }

    gMacTCPReady = true;
    return true;
}

void FormatIPAddress(ip_addr addr, char *outBuf)
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
    return (*outAddr != 0);
}

void NetSetServer(ip_addr ip, unsigned short httpPort)
{
    gServerIP       = ip;
    gServerHttpPort = (httpPort > 0) ? httpPort : DEFAULT_HTTP_PORT;
}

ip_addr NetGetServerIP(void)
{
    return gServerIP;
}

ip_addr NetGetOurIP(void)
{
    if (!gMacTCPReady) NetInit();
    return gOurIP;
}

unsigned short NetGetServerPort(void)
{
    return gServerHttpPort;
}

Boolean NetHasServer(void)
{
    return (gServerIP != 0);
}

void NetSetIdleCallback(void (*callback)(void))
{
    gIdleCallback = callback;
}

void NetAbortCurrentOperation(void)
{
    gAbortRequested = true;
}

static OSErr RunTCPControl(TCPiopb *pb, Boolean runIdle, unsigned long maxWaitTicks)
{
    unsigned long startTicks = TickCount();
    unsigned long lastIdleTick = startTicks;
    OSErr err;

    if (pb->csCode != TCPAbort && pb->csCode != TCPRelease) {
        gAbortRequested = false;
    }

    pb->ioCRefNum    = gMacTCPRef;
    pb->ioCompletion = NULL;
    err = PBControlAsync((ParmBlkPtr)pb);
    if (err != noErr && err != inProgress) {
        return err;
    }

    while (pb->ioResult == inProgress) {
        unsigned long nowTicks = TickCount();
        if (nowTicks != lastIdleTick) {
            lastIdleTick = nowTicks;
            SystemTask();
            if (runIdle && gIdleCallback) {
                gIdleCallback();
            }
            if (gAudioRunning) {
                AudioStreamPump();
            }
        }
        if ((nowTicks - startTicks) > maxWaitTicks ||
            (gAbortRequested && pb->csCode != TCPAbort && pb->csCode != TCPRelease)) {
            if (pb->tcpStream != 0 && pb->csCode != TCPAbort && pb->csCode != TCPRelease) {
                TCPiopb abortPb;
                memset(&abortPb, 0, sizeof(abortPb));
                abortPb.ioCRefNum = gMacTCPRef;
                abortPb.csCode    = TCPAbort;
                abortPb.tcpStream = pb->tcpStream;
                PBControlSync((ParmBlkPtr)&abortPb);
            }
            break;
        }
    }
    return (pb->ioResult == inProgress) ? commandTimeout : pb->ioResult;
}

Boolean NetDiscoverServer(void)
{
    UDPiopb pb;
    Ptr udpRcvBuf;
    StreamPtr udpStream = 0;
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

    /* Candidate discovery destinations on local subnet + broadcast (matches SpotiMac):
     *  0: Subnet host .2 (10.0.2.2 in Basilisk II / QEMU slirp)
     *  1: Subnet gateway .1
     *  2: Subnet broadcast (e.g. 192.168.1.255)
     *  3: Global broadcast (255.255.255.255)
     */
    targets[0] = (gOurIP & (ip_addr)gOurNetMask) | 2;
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

    /* Wait up to 2 seconds for MACCOMPANION_SERVER|<ip>|<port>|<modules> */
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

        /* Return the UDP buffer to MacTCP (preserving pb.csParam.receive.rcvBuff) */
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

static long ParseContentLength(const char *hdr, long hdrLen)
{
    long i;
    for (i = 0; i + 15 < hdrLen; i++) {
        if ((hdr[i] == 'C' || hdr[i] == 'c') &&
            (hdr[i+1] == 'o' || hdr[i+1] == 'O') &&
            (hdr[i+7] == '-') &&
            (hdr[i+8] == 'L' || hdr[i+8] == 'l') &&
            (hdr[i+14] == ':')) {
            const char *p = hdr + i + 15;
            long val = 0;
            while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (*p - '0');
                p++;
            }
            return val;
        }
    }
    return -1;
}

static long NetHttpRequestInternal(const char *method, const char *path,
                                   const char *contentType,
                                   const void *postBody, long postLen,
                                   char *outBody, long maxLen,
                                   Byte openTimeoutSec, Byte rcvTimeoutSec,
                                   NetStatusCallback onStatus)
{
    TCPiopb pb;
    StreamPtr stream = 0;
    Ptr tcpRcvBuf = NULL;
    static char reqHeader[1536];
    char hostStr[32];
    wdsEntry wds[3];
    OSErr err;
    long totalRead = 0;
    long bodyLen = -1;
    Boolean headerStripped = false;
    long expectedBodyLen = -1;
    unsigned long openMaxTicks = ((unsigned long)openTimeoutSec + 1UL) * 60UL;
    unsigned long rcvMaxTicks  = ((unsigned long)rcvTimeoutSec + 2UL) * 60UL;

    if (!outBody || maxLen <= 1) return -1;
    outBody[0] = '\0';

    if (!NetInit() || gServerIP == 0) return -1;

    tcpRcvBuf = NewPtr(8192);
    if (!tcpRcvBuf) return -1;

    memset(&pb, 0, sizeof(pb));
    pb.csCode                    = TCPCreate;
    pb.csParam.create.rcvBuff    = tcpRcvBuf;
    pb.csParam.create.rcvBuffLen = 8192;
    pb.csParam.create.notifyProc = NULL;
    err = RunTCPControl(&pb, true, 180);
    if (err != noErr) {
        DisposePtr(tcpRcvBuf);
        return -1;
    }
    stream = pb.tcpStream;

    memset(&pb, 0, sizeof(pb));
    pb.csCode                           = TCPActiveOpen;
    pb.tcpStream                        = stream;
    pb.csParam.open.ulpTimeoutValue     = openTimeoutSec;
    pb.csParam.open.ulpTimeoutAction    = 1;
    pb.csParam.open.validityFlags       = timeoutValue | timeoutAction;
    pb.csParam.open.commandTimeoutValue = openTimeoutSec;
    pb.csParam.open.remoteHost          = gServerIP;
    pb.csParam.open.remotePort          = gServerHttpPort;
    pb.csParam.open.localPort           = 0;

    err = RunTCPControl(&pb, true, openMaxTicks);
    if (err != noErr) {
        goto cleanup;
    }

    FormatIPAddress(gServerIP, hostStr);
    if (postBody && postLen > 0) {
        sprintf(reqHeader,
                "%s %s HTTP/1.0\r\n"
                "Host: %s:%u\r\n"
                "User-Agent: MacAI/68k\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %ld\r\n"
                "Connection: close\r\n\r\n",
                method, path, hostStr, gServerHttpPort,
                contentType ? contentType : "application/octet-stream",
                postLen);
        wds[0].length = (unsigned short)strlen(reqHeader);
        wds[0].ptr    = (Ptr)reqHeader;
        wds[1].length = (unsigned short)(postLen > 60000L ? 60000L : postLen);
        wds[1].ptr    = (Ptr)postBody;
        wds[2].length = 0;
        wds[2].ptr    = NULL;
    } else {
        sprintf(reqHeader,
                "%s %s HTTP/1.0\r\n"
                "Host: %s:%u\r\n"
                "User-Agent: MacAI/68k\r\n"
                "Connection: close\r\n\r\n",
                method, path, hostStr, gServerHttpPort);
        wds[0].length = (unsigned short)strlen(reqHeader);
        wds[0].ptr    = (Ptr)reqHeader;
        wds[1].length = 0;
        wds[1].ptr    = NULL;
    }

    memset(&pb, 0, sizeof(pb));
    pb.csCode                     = TCPSend;
    pb.tcpStream                  = stream;
    pb.csParam.send.ulpTimeoutValue = openTimeoutSec;
    pb.csParam.send.ulpTimeoutAction = 1;
    pb.csParam.send.validityFlags = timeoutValue | timeoutAction;
    pb.csParam.send.pushFlag      = 1;
    pb.csParam.send.urgentFlag    = 0;
    pb.csParam.send.wdsPtr        = (Ptr)wds;

    err = RunTCPControl(&pb, true, openMaxTicks);
    if (err != noErr) {
        goto close_conn;
    }

    while (totalRead < (maxLen - 1)) {
        unsigned short chunkAsk = (unsigned short)((maxLen - 1 - totalRead) > 4096 ? 4096 : (maxLen - 1 - totalRead));
        memset(&pb, 0, sizeof(pb));
        pb.csCode                              = TCPRcv;
        pb.tcpStream                           = stream;
        pb.csParam.receive.commandTimeoutValue = rcvTimeoutSec;
        pb.csParam.receive.rcvBuff             = outBody + totalRead;
        pb.csParam.receive.rcvBuffLen          = chunkAsk;

        err = RunTCPControl(&pb, true, rcvMaxTicks);
        if (err == noErr && pb.csParam.receive.rcvBuffLen > 0) {
            totalRead += pb.csParam.receive.rcvBuffLen;

            if (!headerStripped) {
                long k;
                for (k = 0; k <= totalRead - 4; k++) {
                    if (outBody[k] == '\r' && outBody[k+1] == '\n' &&
                        outBody[k+2] == '\r' && outBody[k+3] == '\n') {
                        long headerBytes = k + 4;
                        long rem;
                        expectedBodyLen = ParseContentLength(outBody, headerBytes);
                        rem = totalRead - headerBytes;
                        if (rem > 0) {
                            memmove(outBody, outBody + headerBytes, rem);
                        }
                        totalRead = rem;
                        headerStripped = true;
                        break;
                    }
                }
            }

            if (headerStripped && expectedBodyLen >= 0 && totalRead >= expectedBodyLen) {
                break;
            }

            if (headerStripped && onStatus) {
                Boolean gotFinalReply = false;
                char *nl;
                while ((nl = memchr(outBody, '\n', totalRead)) != NULL) {
                    long lineLen = (long)(nl - outBody + 1);
                    if (strncmp(outBody, "STATUS|", 7) == 0) {
                        char stBuf[128];
                        short stLen = (short)(lineLen - 7 - 1);
                        if (stLen > 0 && outBody[7 + stLen - 1] == '\r') stLen--;
                        if (stLen > (short)(sizeof(stBuf) - 1)) stLen = sizeof(stBuf) - 1;
                        if (stLen > 0) {
                            memcpy(stBuf, outBody + 7, stLen);
                            stBuf[stLen] = '\0';
                            onStatus(stBuf);
                        }
                        memmove(outBody, nl + 1, totalRead - lineLen);
                        totalRead -= lineLen;
                    } else if (strncmp(outBody, "REPLY|", 6) == 0 || strncmp(outBody, "ERR|", 4) == 0) {
                        /* Final payload or error encountered; truncate at line end and exit immediately */
                        outBody[lineLen] = '\0';
                        totalRead = lineLen;
                        gotFinalReply = true;
                        break;
                    } else {
                        /* Consume unknown keep-alive line */
                        memmove(outBody, nl + 1, totalRead - lineLen);
                        totalRead -= lineLen;
                    }
                }
                if (gotFinalReply) {
                    break;
                }
            }
        } else {
            break;
        }
    }

    outBody[totalRead] = '\0';
    bodyLen = totalRead;

close_conn:
    memset(&pb, 0, sizeof(pb));
    pb.csCode    = TCPAbort;
    pb.tcpStream = stream;
    RunTCPControl(&pb, true, 120);

cleanup:
    memset(&pb, 0, sizeof(pb));
    pb.csCode    = TCPRelease;
    pb.tcpStream = stream;
    RunTCPControl(&pb, true, 120);

    DisposePtr(tcpRcvBuf);
    return bodyLen;
}

long NetHttpGetStream(const char *path, char *outBody, long maxLen, NetStatusCallback onStatus, short timeoutSec)
{
    Byte rcvTimeout = (timeoutSec > 0 && timeoutSec < 120) ? (Byte)timeoutSec : 45;
    return NetHttpRequestInternal("GET", path, NULL, NULL, 0, outBody, maxLen, 4, rcvTimeout, onStatus);
}

long NetHttpGet(const char *path, char *outBody, long maxLen)
{
    Boolean isPing = (strstr(path, "/ping") != NULL);
    Boolean isSlowAI = (strstr(path, "/ask") != NULL || strstr(path, "/tts") != NULL);
    Byte openTimeout = isPing ? 3 : 4;
    Byte rcvTimeout  = isSlowAI ? 45 : (isPing ? 3 : 6);
    return NetHttpRequestInternal("GET", path, NULL, NULL, 0, outBody, maxLen, openTimeout, rcvTimeout, NULL);
}

long NetHttpPost(const char *path, const char *contentType,
                 const void *postBody, long postLen,
                 char *outBody, long maxLen)
{
    return NetHttpRequestInternal("POST", path, contentType, postBody, postLen, outBody, maxLen, 5, 30, NULL);
}

void UrlEncode(const char *src, char *dst, short maxDst)
{
    static const char *hex = "0123456789ABCDEF";
    short pos = 0;
    if (!src || !dst || maxDst <= 1) return;

    while (*src && pos < (maxDst - 4)) {
        unsigned char c = (unsigned char)*src++;
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == ':') {
            dst[pos++] = (char)c;
        } else if (c == ' ') {
            dst[pos++] = '%';
            dst[pos++] = '2';
            dst[pos++] = '0';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[(c >> 4) & 0x0F];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
}

/* ============================================================================
 * Native 68k Audio Streaming Implementation (TCP Port 6802 -> Sound Manager)
 * ============================================================================ */

void AudioStreamInit(void)
{
    short i;
    if (!gSndCallBackUPP) {
        gSndCallBackUPP = NewSndCallBackUPP(SoundBufferCallback);
    }
    for (i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        if (!gPcmBuffers[i]) {
            gPcmBuffers[i] = NewPtrClear(AUDIO_CHUNK_SIZE);
        }
    }
}

static void QueueSoundBuffer(short idx)
{
    SndCommand cmd;
    SoundHeader *sh = &gSndHeaders[idx];

    if (!gSndChan || gPcmFilled[idx] <= 0) return;

    memset(sh, 0, sizeof(SoundHeader));
    sh->samplePtr     = gPcmBuffers[idx];
    sh->length        = (unsigned long)gPcmFilled[idx];
    sh->sampleRate    = (gAudioSampleRateHz <= 12000) ? rate11khz : rate22khz;
    sh->loopStart     = 0;
    sh->loopEnd       = 0;
    sh->encode        = stdSH;
    sh->baseFrequency = kMiddleC;

    gBufferBusy[idx]  = 1;
    gPcmFilled[idx]   = 0;

    cmd.cmd    = bufferCmd;
    cmd.param1 = 0;
    cmd.param2 = (long)sh;
    SndDoCommand(gSndChan, &cmd, true);

    cmd.cmd    = callBackCmd;
    cmd.param1 = idx;
    cmd.param2 = (long)&gBufferBusy[idx];
    SndDoCommand(gSndChan, &cmd, true);
}

Boolean AudioStreamStart(unsigned short sampleRateHz)
{
    TCPiopb pb;
    short i;

    AudioStreamInit();
    if (gAudioRunning) {
        if (sampleRateHz > 0 && sampleRateHz != gAudioSampleRateHz) {
            gAudioSampleRateHz = sampleRateHz;
        }
        return true;
    }

    if (!NetInit() || gServerIP == 0) return false;
    if (sampleRateHz > 0) gAudioSampleRateHz = sampleRateHz;

    gSndChan = NULL;
    if (SndNewChannel(&gSndChan, sampledSynth, initMono, gSndCallBackUPP) != noErr || !gSndChan) {
        gSndChan = NULL;
        return false;
    }

    if (!gAudioTcpRcvBuf) {
        gAudioTcpRcvBuf = NewPtr(AUDIO_TCP_RCV_SIZE);
        if (!gAudioTcpRcvBuf) {
            SndDisposeChannel(gSndChan, true);
            gSndChan = NULL;
            return false;
        }
    }

    memset(&pb, 0, sizeof(pb));
    pb.csCode                    = TCPCreate;
    pb.csParam.create.rcvBuff    = gAudioTcpRcvBuf;
    pb.csParam.create.rcvBuffLen = AUDIO_TCP_RCV_SIZE;
    pb.csParam.create.notifyProc = NULL;
    if (RunTCPControl(&pb, false, 180) != noErr) {
        SndDisposeChannel(gSndChan, true);
        gSndChan = NULL;
        return false;
    }
    gAudioStream = pb.tcpStream;

    memset(&pb, 0, sizeof(pb));
    pb.csCode                           = TCPActiveOpen;
    pb.tcpStream                        = gAudioStream;
    pb.csParam.open.ulpTimeoutValue     = 5;
    pb.csParam.open.ulpTimeoutAction    = 1;
    pb.csParam.open.validityFlags       = timeoutValue | timeoutAction;
    pb.csParam.open.commandTimeoutValue = 5;
    pb.csParam.open.remoteHost          = gServerIP;
    pb.csParam.open.remotePort          = DEFAULT_AUDIO_PORT;
    pb.csParam.open.localPort           = 0;
    if (RunTCPControl(&pb, false, 300) != noErr) {
        memset(&pb, 0, sizeof(pb));
        pb.csCode    = TCPRelease;
        pb.tcpStream = gAudioStream;
        RunTCPControl(&pb, false, 120);
        gAudioStream = 0;
        SndDisposeChannel(gSndChan, true);
        gSndChan = NULL;
        return false;
    }

    for (i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        gBufferBusy[i] = 0;
        gPcmFilled[i]  = 0;
    }
    gNextFillBuf  = 0;
    gPrebuffering = true;
    gAudioRunning = true;
    return true;
}

void AudioStreamStop(void)
{
    TCPiopb pb;
    short i;

    if (!gAudioRunning && gAudioStream == 0 && !gSndChan) return;
    gAudioRunning = false;
    gPrebuffering = true;

    if (gSndChan) {
        SndCommand cmd;
        cmd.cmd = flushCmd; cmd.param1 = 0; cmd.param2 = 0;
        SndDoImmediate(gSndChan, &cmd);
        cmd.cmd = quietCmd; cmd.param1 = 0; cmd.param2 = 0;
        SndDoImmediate(gSndChan, &cmd);
        SndDisposeChannel(gSndChan, true);
        gSndChan = NULL;
    }

    if (gAudioStream != 0) {
        memset(&pb, 0, sizeof(pb));
        pb.csCode    = TCPAbort;
        pb.tcpStream = gAudioStream;
        RunTCPControl(&pb, false, 120);

        memset(&pb, 0, sizeof(pb));
        pb.csCode    = TCPRelease;
        pb.tcpStream = gAudioStream;
        RunTCPControl(&pb, false, 120);
        gAudioStream = 0;
    }

    for (i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        gBufferBusy[i] = 0;
        gPcmFilled[i]  = 0;
    }
}

Boolean AudioStreamIsRunning(void)
{
    return gAudioRunning;
}

void AudioStreamPump(void)
{
    TCPiopb pb;
    unsigned short avail = 0;
    short attempts;

    if (!gAudioRunning || gAudioStream == 0 || !gSndChan) return;

    if (gBufferBusy[0] && gBufferBusy[1]) return;

    if (TickCount() == gLastEmptyStatusTick) return;

    for (attempts = 0; attempts < 2; attempts++) {
        short target = gNextFillBuf;
        unsigned short spaceLeft;
        unsigned short toRead;

        if (gBufferBusy[target]) break;
        if (!gPcmBuffers[target]) break;

        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode    = TCPStatus;
        pb.tcpStream = gAudioStream;
        if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
            AudioStreamStop();
            return;
        }

        if (pb.csParam.status.connectionState != 8) {
            AudioStreamStop();
            return;
        }

        avail = pb.csParam.status.amtUnreadData;
        if (avail == 0) {
            gLastEmptyStatusTick = TickCount();
            /* If we have a partial TTS tail sitting in target buffer and neither buffer is busy, flush it! */
            if (gPcmFilled[target] > 0 && !gBufferBusy[0] && !gBufferBusy[1]) {
                QueueSoundBuffer(target);
                gNextFillBuf = (target + 1) % NUM_AUDIO_BUFFERS;
            }
            break;
        }

        spaceLeft = (unsigned short)(AUDIO_CHUNK_SIZE - gPcmFilled[target]);
        toRead    = (avail < spaceLeft) ? avail : spaceLeft;

        if (toRead > 0) {
            memset(&pb, 0, sizeof(pb));
            pb.ioCRefNum                           = gMacTCPRef;
            pb.csCode                              = TCPRcv;
            pb.tcpStream                           = gAudioStream;
            pb.csParam.receive.commandTimeoutValue = 1;
            pb.csParam.receive.rcvBuff             = gPcmBuffers[target] + gPcmFilled[target];
            pb.csParam.receive.rcvBuffLen          = toRead;

            if (PBControlSync((ParmBlkPtr)&pb) == noErr && pb.csParam.receive.rcvBuffLen > 0) {
                gPcmFilled[target] += pb.csParam.receive.rcvBuffLen;
            } else {
                break;
            }
        }

        if (gPcmFilled[target] >= AUDIO_CHUNK_SIZE || (avail < spaceLeft && gPcmFilled[target] >= 2048)) {
            QueueSoundBuffer(target);
            gNextFillBuf = (target + 1) % NUM_AUDIO_BUFFERS;
        } else {
            break;
        }
    }
}

/* ============================================================================
 * Sound Input Manager Hardware Detection & Voice Recording Implementation
 * ============================================================================ */

Boolean SoundInputHasHardware(void)
{
    long sndAttr = 0;

    if (gSoundInputChecked) {
        return gSoundInputPresent;
    }
    gSoundInputChecked = true;
    gSoundInputPresent = false;

    /* Check Gestalt sound attributes for both Sound Input Manager and a Sound Input device */
    if (Gestalt(gestaltSoundAttr, &sndAttr) != noErr) {
        return false;
    }
    if ((sndAttr & (1L << gestaltSoundIOMgrPresent)) == 0 ||
        (sndAttr & (1L << gestaltHasSoundInputDevice)) == 0) {
        return false;
    }

    gSoundInputPresent = true;
    return true;
}

Boolean SoundInputStartRecording(unsigned short *outSampleRateHz)
{
    OSErr err;
    short numChans = 1;
    short sampleSize = 8;
    UnsignedFixed sampleRate = 0;

    if (!SoundInputHasHardware()) return false;
    if (gVoiceRecording) return true;

    if (!gVoiceBuffer) {
        gVoiceBuffer = NewPtrClear(VOICE_MAX_BYTES);
        if (!gVoiceBuffer) return false;
    }

    gVoiceDevRef = 0;
    err = SPBOpenDevice("\p", kSIWritePermission, &gVoiceDevRef);
    if (err != noErr || gVoiceDevRef == 0) {
        return false;
    }

    /* Configure 1 channel, 8-bit unsigned PCM */
    SPBSetDeviceInfo(gVoiceDevRef, 'chan', (Ptr)&numChans);
    SPBSetDeviceInfo(gVoiceDevRef, 'ssiz', (Ptr)&sampleSize);

    /* Query active sample rate from Sound Input device */
    gVoiceSampleRate = 11127;
    if (SPBGetDeviceInfo(gVoiceDevRef, 'srat', (Ptr)&sampleRate) == noErr && sampleRate != 0) {
        unsigned short hz = (unsigned short)((sampleRate >> 16) & 0xFFFF);
        if (hz >= 8000 && hz <= 48000) {
            gVoiceSampleRate = hz;
        }
    }
    if (outSampleRateHz) *outSampleRateHz = gVoiceSampleRate;

    memset(&gVoiceSPB, 0, sizeof(gVoiceSPB));
    gVoiceSPB.inRefNum          = gVoiceDevRef;
    gVoiceSPB.count             = VOICE_MAX_BYTES;
    gVoiceSPB.milliseconds      = 0;
    gVoiceSPB.bufferLength      = VOICE_MAX_BYTES;
    gVoiceSPB.bufferPtr         = gVoiceBuffer;
    gVoiceSPB.completionRoutine = NULL;
    gVoiceSPB.interruptRoutine  = NULL;
    gVoiceSPB.userLong          = 0;
    gVoiceSPB.error             = noErr;
    gVoiceSPB.unused1           = 0;

    err = SPBRecord((SPBPtr)&gVoiceSPB, true);
    if (err != noErr) {
        SPBCloseDevice(gVoiceDevRef);
        gVoiceDevRef = 0;
        return false;
    }

    gVoiceStartTick = TickCount();
    gVoiceRecording = true;
    return true;
}

Boolean SoundInputIsRecording(void)
{
    short recStatus = 0;
    short meterLevel = 0;
    long totalSamps = 0, recSamps = 0, totalMsecs = 0, recMsecs = 0;

    if (!gVoiceRecording || gVoiceDevRef == 0) return false;

    if (SPBGetRecordingStatus(gVoiceDevRef, &recStatus, &meterLevel,
                              &totalSamps, &recSamps, &totalMsecs, &recMsecs) == noErr) {
        if (recStatus == 0 && (TickCount() - gVoiceStartTick) > 15) {
            return false;
        }
    }
    if (gVoiceSPB.error < 0 && gVoiceSPB.error != -227 /* siRecordingTerminated */) {
        return false;
    }
    return true;
}

Boolean SoundInputStopRecording(Ptr *outBuf, long *outLen, unsigned short *outSampleRateHz)
{
    short recStatus = 0;
    short meterLevel = 0;
    long totalSamps = 0, recSamps = 0, totalMsecs = 0, recMsecs = 0;
    long recordedBytes = 0;

    if (!gVoiceRecording && gVoiceDevRef == 0) return false;

    if (gVoiceDevRef != 0) {
        SPBGetRecordingStatus(gVoiceDevRef, &recStatus, &meterLevel,
                              &totalSamps, &recSamps, &totalMsecs, &recMsecs);
        SPBStopRecording(gVoiceDevRef);
        recordedBytes = (long)gVoiceSPB.count;
        if (recSamps > 0 && recSamps < VOICE_MAX_BYTES) {
            recordedBytes = recSamps;
        }
        SPBCloseDevice(gVoiceDevRef);
        gVoiceDevRef = 0;
    }
    gVoiceRecording = false;

    if (recordedBytes <= 0 || recordedBytes > VOICE_MAX_BYTES) {
        recordedBytes = (long)gVoiceSPB.count;
    }
    if (recordedBytes <= 256 || !gVoiceBuffer) {
        return false;
    }

    if (outBuf) *outBuf = gVoiceBuffer;
    if (outLen) *outLen = recordedBytes;
    if (outSampleRateHz) *outSampleRateHz = gVoiceSampleRate;
    return true;
}
