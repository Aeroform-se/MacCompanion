#include "network.h"
#include <Devices.h>
#include <Memory.h>
#include <Events.h>
#include <Sound.h>
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

/* ============================================================================
 * Native 68k Audio Streaming State (Double-Buffered Sound Manager)
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
static long           gBufferQueuedLen[NUM_AUDIO_BUFFERS] = { 0, 0 };
static unsigned long  gBufferQueuedTick[NUM_AUDIO_BUFFERS]= { 0, 0 };
static volatile short gBufferBusy[NUM_AUDIO_BUFFERS]      = { 0, 0 };
static short          gNextFillBuf       = 0;
static Boolean        gPrebuffering      = true;

/*
 * Interrupt-safe Sound Manager callback:
 * `cmd->param2` holds the direct memory address of `gBufferBusy[bufIdx]`,
 * avoiding any reliance on register A5 at interrupt time.
 */
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
        gOurIP      = 0x0A00020F; /* 10.0.2.15 fallback */
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
    gServerHttpPort = httpPort ? httpPort : DEFAULT_HTTP_PORT;
}

ip_addr NetGetServerIP(void)
{
    return gServerIP;
}

ip_addr NetGetOurIP(void)
{
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

/*
 * Wait for an asynchronous MacTCP PBControl call while keeping the audio stream
 * fed so playback never glitches during HTTP calls.
 */
static OSErr RunTCPControl(TCPiopb *pb, Boolean pumpAudioWhileWaiting)
{
    OSErr err;
    unsigned long startTicks = TickCount();

    pb->ioCompletion = NULL;
    pb->ioCRefNum    = gMacTCPRef;
    err = PBControlAsync((ParmBlkPtr)pb);
    if (err != noErr && err != inProgress) {
        return err;
    }

    while (pb->ioResult == inProgress) {
        if (pumpAudioWhileWaiting && gAudioRunning) {
            AudioStreamPump();
        }
        /* Safety timeout (8 seconds) */
        if ((TickCount() - startTicks) > 480) {
            break;
        }
    }
    return pb->ioResult;
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
    pb.ioCRefNum               = gMacTCPRef;
    pb.csCode                  = UDPCreate;
    pb.csParam.create.rcvBuff    = udpRcvBuf;
    pb.csParam.create.rcvBuffLen = 2048;
    pb.csParam.create.notifyProc = NULL;
    pb.csParam.create.localPort  = 0;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
        DisposePtr(udpRcvBuf);
        return false;
    }
    udpStream = pb.udpStream;

    /* Candidate discovery destinations:
     *  0: Subnet host .2 (10.0.2.2 in Basilisk II / QEMU slirp!)
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

        /* Return the UDP buffer to MacTCP */
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

/*
 * Case-insensitive search for "Content-Length:" inside HTTP headers
 */
static long ParseContentLength(const char *headers, long headerLen)
{
    long i;
    for (i = 0; i <= headerLen - 15; i++) {
        if ((headers[i] == 'C' || headers[i] == 'c') &&
            (headers[i+1] == 'o' || headers[i+1] == 'O') &&
            (headers[i+7] == '-') &&
            (headers[i+8] == 'L' || headers[i+8] == 'l') &&
            (headers[i+14] == ':')) {
            const char *p = headers + i + 15;
            while (*p == ' ') p++;
            return atol(p);
        }
    }
    return -1;
}

long NetHttpGet(const char *path, char *outBody, long maxLen)
{
    TCPiopb pb;
    Ptr tcpRcvBuf = NULL;
    StreamPtr stream = 0;
    wdsEntry wds[2];
    char reqBuf[512];
    long totalRead = 0;
    long bodyLen = -1;
    long expectedBodyLen = -1;
    Boolean headerStripped = false;
    OSErr err;

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
    if (RunTCPControl(&pb, true) != noErr) {
        DisposePtr(tcpRcvBuf);
        return -1;
    }
    stream = pb.tcpStream;

    /* Active Open to gServerIP : gServerHttpPort */
    memset(&pb, 0, sizeof(pb));
    pb.csCode                           = TCPActiveOpen;
    pb.tcpStream                        = stream;
    pb.csParam.open.ulpTimeoutValue     = 4;
    pb.csParam.open.ulpTimeoutAction    = 1;
    pb.csParam.open.validityFlags       = timeoutValue | timeoutAction;
    pb.csParam.open.commandTimeoutValue = 4;
    pb.csParam.open.remoteHost          = gServerIP;
    pb.csParam.open.remotePort          = gServerHttpPort;
    pb.csParam.open.localPort           = 0;
    if (RunTCPControl(&pb, true) != noErr) {
        goto cleanup;
    }

    /* Send HTTP/1.0 GET request */
    sprintf(reqBuf,
        "GET %s HTTP/1.0\r\n"
        "Host: mac68k\r\n"
        "Connection: close\r\n\r\n",
        path);

    wds[0].length = (unsigned short)strlen(reqBuf);
    wds[0].ptr    = reqBuf;
    wds[1].length = 0;
    wds[1].ptr    = NULL;

    memset(&pb, 0, sizeof(pb));
    pb.csCode                  = TCPSend;
    pb.tcpStream               = stream;
    pb.csParam.send.wdsPtr     = (Ptr)wds;
    pb.csParam.send.pushFlag   = true;
    if (RunTCPControl(&pb, true) != noErr) {
        goto close_conn;
    }

    /* Read response until Content-Length is satisfied or EOF */
    while (totalRead < (maxLen - 1)) {
        long spaceLeft = (maxLen - 1) - totalRead;
        unsigned short chunkAsk = (spaceLeft > 4096) ? 4096 : (unsigned short)spaceLeft;

        if (headerStripped && expectedBodyLen >= 0 && totalRead >= expectedBodyLen) {
            break;
        }

        memset(&pb, 0, sizeof(pb));
        pb.csCode                              = TCPRcv;
        pb.tcpStream                           = stream;
        pb.csParam.receive.commandTimeoutValue = 4;
        pb.csParam.receive.rcvBuff             = outBody + totalRead;
        pb.csParam.receive.rcvBuffLen          = chunkAsk;

        err = RunTCPControl(&pb, true);
        if (err == noErr && pb.csParam.receive.rcvBuffLen > 0) {
            totalRead += pb.csParam.receive.rcvBuffLen;

            /* Strip HTTP headers as soon as "\r\n\r\n" is received */
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
        } else {
            break;
        }
    }

    outBody[totalRead] = '\0';
    bodyLen = totalRead;

close_conn:
    /* Immediate TCPAbort + TCPRelease avoids waiting several seconds on FIN timeout */
    memset(&pb, 0, sizeof(pb));
    pb.csCode    = TCPAbort;
    pb.tcpStream = stream;
    RunTCPControl(&pb, true);

cleanup:
    memset(&pb, 0, sizeof(pb));
    pb.csCode    = TCPRelease;
    pb.tcpStream = stream;
    RunTCPControl(&pb, true);

    DisposePtr(tcpRcvBuf);
    return bodyLen;
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
 * Native 68k Audio Streaming Implementation
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

    gBufferQueuedLen[idx]  = gPcmFilled[idx];
    gBufferQueuedTick[idx] = TickCount();
    gBufferBusy[idx]       = 1;
    gPcmFilled[idx]        = 0;

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
    if (RunTCPControl(&pb, false) != noErr) {
        SndDisposeChannel(gSndChan, true);
        gSndChan = NULL;
        return false;
    }
    gAudioStream = pb.tcpStream;

    memset(&pb, 0, sizeof(pb));
    pb.csCode                           = TCPActiveOpen;
    pb.tcpStream                        = gAudioStream;
    pb.csParam.open.ulpTimeoutValue     = 10;
    pb.csParam.open.ulpTimeoutAction    = 1;
    pb.csParam.open.validityFlags       = timeoutValue | timeoutAction;
    pb.csParam.open.commandTimeoutValue = 5;
    pb.csParam.open.remoteHost          = gServerIP;
    pb.csParam.open.remotePort          = DEFAULT_AUDIO_PORT;
    pb.csParam.open.localPort           = 0;
    if (RunTCPControl(&pb, false) != noErr) {
        memset(&pb, 0, sizeof(pb));
        pb.csCode    = TCPRelease;
        pb.tcpStream = gAudioStream;
        RunTCPControl(&pb, false);
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
        RunTCPControl(&pb, false);

        memset(&pb, 0, sizeof(pb));
        pb.csCode    = TCPRelease;
        pb.tcpStream = gAudioStream;
        RunTCPControl(&pb, false);
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

Boolean AudioGetLiveSamples(unsigned char *outSamples, short count)
{
    short playBuf = -1;
    long  bufLen;
    long  elapsedTicks;
    long  samplesPerTick;
    long  offset;

    if (!outSamples || count <= 0 || !gAudioRunning) return false;

    if (gBufferBusy[0] && gBufferBusy[1]) {
        playBuf = ((long)(gBufferQueuedTick[1] - gBufferQueuedTick[0]) >= 0) ? 0 : 1;
    } else if (gBufferBusy[0]) {
        playBuf = 0;
    } else if (gBufferBusy[1]) {
        playBuf = 1;
    } else {
        return false;
    }

    if (!gPcmBuffers[playBuf] || gBufferQueuedLen[playBuf] < count) {
        return false;
    }

    bufLen = gBufferQueuedLen[playBuf];
    elapsedTicks = (long)(TickCount() - gBufferQueuedTick[playBuf]);
    if (elapsedTicks < 0) elapsedTicks = 0;
    if (elapsedTicks > 20) elapsedTicks = 20;

    samplesPerTick = (gAudioSampleRateHz <= 12000) ? 185L : 371L;
    offset = (elapsedTicks * samplesPerTick) % (bufLen - count + 1);
    if (offset < 0 || offset + count > bufLen) {
        offset = 0;
    }

    memcpy(outSamples, gPcmBuffers[playBuf] + offset, (size_t)count);
    return true;
}

void AudioStreamPump(void)
{
    TCPiopb pb;
    short attempts;

    if (!gAudioRunning || gAudioStream == 0 || !gSndChan) return;

    /*
     * If the Sound Manager currently has no buffers playing and both buffers are
     * empty (e.g., fresh start, resume after pause, or recovery after a seek/skip),
     * re-arm prebuffering so Buffer 0 only starts playing once Buffer 1 also has a
     * >= 2,048-byte (~92ms) cushion ready.
     */
    if (!gBufferBusy[0] && !gBufferBusy[1] && gPcmFilled[0] == 0 && gPcmFilled[1] == 0) {
        gPrebuffering = true;
        gNextFillBuf  = 0;
    }

    for (attempts = 0; attempts < NUM_AUDIO_BUFFERS; attempts++) {
        short idx;
        unsigned short unread = 0;
        long targetSize;
        long needed;
        unsigned short toRead;

        if (gPrebuffering) {
            idx = (gPcmFilled[0] < 4096) ? 0 : 1;
            targetSize = 4096;
        } else {
            idx = gNextFillBuf;
            targetSize = AUDIO_CHUNK_SIZE;
        }

        if (gBufferBusy[idx]) {
            break;
        }

        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum = gMacTCPRef;
        pb.csCode    = TCPStatus;
        pb.tcpStream = gAudioStream;
        if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
            AudioStreamStop();
            return;
        }

        unread = pb.csParam.status.amtUnreadData;
        if (unread == 0) {
            break;
        }

        needed = targetSize - gPcmFilled[idx];
        if (needed <= 0) {
            break;
        }
        toRead = (unread > (unsigned short)needed) ? (unsigned short)needed : unread;

        memset(&pb, 0, sizeof(pb));
        pb.ioCRefNum                           = gMacTCPRef;
        pb.csCode                              = TCPRcv;
        pb.tcpStream                           = gAudioStream;
        pb.csParam.receive.commandTimeoutValue = 1;
        pb.csParam.receive.rcvBuff             = gPcmBuffers[idx] + gPcmFilled[idx];
        pb.csParam.receive.rcvBuffLen          = toRead;

        if (PBControlSync((ParmBlkPtr)&pb) == noErr && pb.csParam.receive.rcvBuffLen > 0) {
            gPcmFilled[idx] += pb.csParam.receive.rcvBuffLen;
        } else {
            break;
        }

        if (gPrebuffering) {
            if (gPcmFilled[0] >= 4096 && gPcmFilled[1] >= 2048) {
                gPrebuffering = false;
                QueueSoundBuffer(0);
                if (gPcmFilled[1] >= 4096) {
                    QueueSoundBuffer(1);
                    gNextFillBuf = 0;
                } else {
                    gNextFillBuf = 1;
                }
            }
        } else if (gPcmFilled[idx] >= 4096) {
            QueueSoundBuffer(idx);
            gNextFillBuf = (idx + 1) % NUM_AUDIO_BUFFERS;
        } else {
            break;
        }
    }
}
