#include "network.h"
#include <Devices.h>
#include <Memory.h>
#include <Events.h>
#include <Sound.h>
#include <Timer.h>
#include <OSUtils.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define FAST_TICKS() (*(volatile unsigned long *)0x016A)

static short          gMacTCPRef      = 0;
static Boolean        gMacTCPReady    = false;
static ip_addr        gOurIP          = 0;
static long           gOurNetMask     = (long)0xFFFFFF00;

static ip_addr        gServerIP       = 0;
static unsigned short gServerHttpPort = DEFAULT_HTTP_PORT;
static void         (*gIdleCallback)(void) = NULL;

/* Persistent HTTP Receive Buffer (avoids per-request NewPtr/DisposePtr heap churn) */
#define HTTP_TCP_RCV_SIZE   8192
static Ptr            gHttpTcpRcvBuf  = NULL;

/* ============================================================================
 * Native 68k Audio Streaming State (6-Buffer Interrupt-Driven Sound Manager)
 * ============================================================================ */

#define AUDIO_TCP_RCV_SIZE  16384
#define AUDIO_CHUNK_SIZE    4096
#define NUM_AUDIO_BUFFERS   6

#define PUMP_IDLE           0
#define PUMP_WAITING_STATUS 1
#define PUMP_WAITING_RCV    2

static StreamPtr      gAudioStream       = 0;
static Ptr            gAudioTcpRcvBuf    = NULL;
static volatile short gAudioRunning      = false;
static volatile short gAudioNeedsStop    = false;
static Boolean        gAudioInBackground = false;
static unsigned short gAudioSampleRateHz = 22255;

static volatile short gAudioPumpLock          = 0;
static volatile short gAudioPumpState         = PUMP_IDLE;
static short          gAudioPendingRcvBufIdx  = 0;
static TCPiopb        gAudioPumpPb;

static TMTask         gAudioTmTask;
static Boolean        gAudioTmInstalled       = false;
static ProcPtr        gPrevMenuHook           = NULL;
static ProcPtr        gPrevDragHook           = NULL;
static Boolean        gHooksInstalled         = false;

static SndChannelPtr  gSndChan           = NULL;
static SndCallBackUPP gSndCallBackUPP    = NULL;
static SoundHeader    gSndHeaders[NUM_AUDIO_BUFFERS];
static Ptr            gPcmBuffers[NUM_AUDIO_BUFFERS]      = { NULL, NULL, NULL, NULL, NULL, NULL };
static long           gPcmFilled[NUM_AUDIO_BUFFERS]       = { 0, 0, 0, 0, 0, 0 };
static long           gBufferQueuedLen[NUM_AUDIO_BUFFERS] = { 0, 0, 0, 0, 0, 0 };
static unsigned long  gBufferQueuedTick[NUM_AUDIO_BUFFERS]= { 0, 0, 0, 0, 0, 0 };
static volatile short gBufferBusy[NUM_AUDIO_BUFFERS]      = { 0, 0, 0, 0, 0, 0 };
static short          gNextFillBuf       = 0;
static Boolean        gPrebuffering      = true;
static unsigned long  gLastEmptyStatusTick = 0;
static unsigned long  gLastAudioPumpTick   = 0;

void AudioInterruptPumpC(void);
void AudioHookPumpC(void);
extern void AudioSaveCurrentA5(void);
extern void AudioTimerAsmEntry(void);
extern void AudioMenuDragHookAsmEntry(void);

asm(
    ".text\n"
    ".even\n"
    "gPcRelSavedA5:\n"
    "    .long 0\n"
    ".globl AudioSaveCurrentA5\n"
    "AudioSaveCurrentA5:\n"
    "    lea gPcRelSavedA5(%pc), %a0\n"
    "    move.l %a5, (%a0)\n"
    "    rts\n"
    "\n"
    ".globl AudioTimerAsmEntry\n"
    "AudioTimerAsmEntry:\n"
    "    movem.l %d0-%d2/%a0-%a1/%a5, -(%sp)\n"
    "    move.l gPcRelSavedA5(%pc), %d0\n"
    "    beq.s 1f\n"
    "    movea.l %d0, %a5\n"
    "    jsr AudioInterruptPumpC\n"
    "1:\n"
    "    movem.l (%sp)+, %d0-%d2/%a0-%a1/%a5\n"
    "    rts\n"
    "\n"
    ".globl AudioMenuDragHookAsmEntry\n"
    "AudioMenuDragHookAsmEntry:\n"
    "    movem.l %d0-%d2/%a0-%a1/%a5, -(%sp)\n"
    "    move.l gPcRelSavedA5(%pc), %d0\n"
    "    beq.s 2f\n"
    "    movea.l %d0, %a5\n"
    "    jsr AudioHookPumpC\n"
    "2:\n"
    "    movem.l (%sp)+, %d0-%d2/%a0-%a1/%a5\n"
    "    rts\n"
);

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

void NetSetIdleCallback(void (*callback)(void))
{
    gIdleCallback = callback;
}

/*
 * Wait for an asynchronous MacTCP PBControl call while keeping the audio stream
 * and visualizer fed so playback and 30 FPS visuals never pause during HTTP calls.
 */
static OSErr RunTCPControl(TCPiopb *pb, Boolean pumpAudioWhileWaiting)
{
    OSErr err;
    unsigned long startTicks = TickCount();
    unsigned long lastIdleTick = startTicks;

    pb->ioCompletion = NULL;
    pb->ioCRefNum    = gMacTCPRef;
    gAudioPumpLock   = 1;
    err = PBControlAsync((ParmBlkPtr)pb);
    gAudioPumpLock   = 0;
    if (err != noErr && err != inProgress) {
        return err;
    }

    while (pb->ioResult == inProgress) {
        unsigned long nowTicks = TickCount();
        /*
         * Throttle nested audio & visualizer pumps to at most once per VBL tick (60.15 Hz)
         * so we never hammer MacTCP's internal queues or starve Ethernet/LocalTalk interrupts
         * while waiting for an async TCP operation!
         */
        if (nowTicks != lastIdleTick) {
            lastIdleTick = nowTicks;
            if (pumpAudioWhileWaiting && gAudioRunning) {
                AudioStreamPump();
            }
            if (pumpAudioWhileWaiting && gIdleCallback) {
                gIdleCallback();
            }
        }
        /* Safety timeout (8 seconds): abort via a separate parameter block so the pending
         * async pb finishes cleanly before its stack frame is reused or returned! */
        if ((nowTicks - startTicks) > 480) {
            if (pb->tcpStream != 0 && pb->csCode != TCPAbort && pb->csCode != TCPRelease) {
                TCPiopb abortPb;
                unsigned long abortStart = TickCount();
                memset(&abortPb, 0, sizeof(abortPb));
                abortPb.ioCRefNum = gMacTCPRef;
                abortPb.csCode    = TCPAbort;
                abortPb.tcpStream = pb->tcpStream;
                PBControlSync((ParmBlkPtr)&abortPb);
                while (pb->ioResult == inProgress && (TickCount() - abortStart) < 60) {
                    SystemTask();
                }
            }
            return (pb->ioResult == inProgress) ? commandTimeout : pb->ioResult;
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

    if (!gHttpTcpRcvBuf) {
        gHttpTcpRcvBuf = NewPtr(HTTP_TCP_RCV_SIZE);
        if (!gHttpTcpRcvBuf) return -1;
    }

    memset(&pb, 0, sizeof(pb));
    pb.csCode                    = TCPCreate;
    pb.csParam.create.rcvBuff    = gHttpTcpRcvBuf;
    pb.csParam.create.rcvBuffLen = HTTP_TCP_RCV_SIZE;
    pb.csParam.create.notifyProc = NULL;
    if (RunTCPControl(&pb, true) != noErr) {
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
        if ((err == noErr || err == connectionClosing || err == connectionTerminated) &&
            pb.csParam.receive.rcvBuffLen > 0) {
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
            if (err == connectionClosing || err == connectionTerminated) {
                break;
            }
        } else {
            break;
        }
    }

    if (headerStripped) {
        outBody[totalRead] = '\0';
        bodyLen = totalRead;
    } else {
        outBody[0] = '\0';
        bodyLen = -1;
    }

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
    AudioSaveCurrentA5();
    if (!gSndCallBackUPP) {
        gSndCallBackUPP = NewSndCallBackUPP(SoundBufferCallback);
    }
    for (i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        if (!gPcmBuffers[i]) {
            gPcmBuffers[i] = NewPtrClear(AUDIO_CHUNK_SIZE);
        }
    }
    if (!gHooksInstalled) {
        gPrevMenuHook = *(ProcPtr *)0x0A30;
        gPrevDragHook = *(ProcPtr *)0x09F6;
        *(ProcPtr *)0x0A30 = (ProcPtr)AudioMenuDragHookAsmEntry;
        *(ProcPtr *)0x09F6 = (ProcPtr)AudioMenuDragHookAsmEntry;
        gHooksInstalled = true;
    }
    if (!gAudioTmInstalled) {
        memset(&gAudioTmTask, 0, sizeof(gAudioTmTask));
        gAudioTmTask.tmAddr = (TimerUPP)AudioTimerAsmEntry;
        InsXTime((QElemPtr)&gAudioTmTask);
        gAudioTmInstalled = true;
        PrimeTime((QElemPtr)&gAudioTmTask, 20);
    }
}

void AudioStreamShutdown(void)
{
    if (gAudioTmInstalled) {
        RmvTime((QElemPtr)&gAudioTmTask);
        gAudioTmInstalled = false;
    }
    if (gHooksInstalled) {
        *(ProcPtr *)0x0A30 = gPrevMenuHook;
        *(ProcPtr *)0x09F6 = gPrevDragHook;
        gHooksInstalled = false;
    }
    AudioStreamStop();
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
    gBufferQueuedTick[idx] = FAST_TICKS();
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
        if (gAudioStream != 0) {
            memset(&pb, 0, sizeof(pb));
            pb.ioCRefNum = gMacTCPRef;
            pb.csCode    = TCPStatus;
            pb.tcpStream = gAudioStream;
            if (PBControlSync((ParmBlkPtr)&pb) != noErr || pb.csParam.status.connectionState != 8) {
                AudioStreamStop();
            } else {
                if (sampleRateHz > 0 && sampleRateHz != gAudioSampleRateHz) {
                    gAudioSampleRateHz = sampleRateHz;
                }
                return true;
            }
        } else {
            AudioStreamStop();
        }
    }

    if (!NetInit() || gServerIP == 0) return false;
    if (sampleRateHz > 0) gAudioSampleRateHz = sampleRateHz;

    if (!gSndChan) {
        if (SndNewChannel(&gSndChan, sampledSynth, initMono, gSndCallBackUPP) != noErr || !gSndChan) {
            gSndChan = NULL;
            return false;
        }
    }

    if (!gAudioTcpRcvBuf) {
        gAudioTcpRcvBuf = NewPtr(AUDIO_TCP_RCV_SIZE);
        if (!gAudioTcpRcvBuf) {
            return false;
        }
    }

    if (gAudioStream == 0) {
        memset(&pb, 0, sizeof(pb));
        pb.csCode                    = TCPCreate;
        pb.csParam.create.rcvBuff    = gAudioTcpRcvBuf;
        pb.csParam.create.rcvBuffLen = AUDIO_TCP_RCV_SIZE;
        pb.csParam.create.notifyProc = NULL;
        if (RunTCPControl(&pb, false) != noErr) {
            return false;
        }
        gAudioStream = pb.tcpStream;
    }

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
        /* Recreate stream once if previous abort left it in a non-reusable state */
        memset(&pb, 0, sizeof(pb));
        pb.csCode    = TCPRelease;
        pb.tcpStream = gAudioStream;
        RunTCPControl(&pb, false);
        gAudioStream = 0;

        memset(&pb, 0, sizeof(pb));
        pb.csCode                    = TCPCreate;
        pb.csParam.create.rcvBuff    = gAudioTcpRcvBuf;
        pb.csParam.create.rcvBuffLen = AUDIO_TCP_RCV_SIZE;
        pb.csParam.create.notifyProc = NULL;
        if (RunTCPControl(&pb, false) != noErr) {
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
            pb.csCode    = TCPAbort;
            pb.tcpStream = gAudioStream;
            RunTCPControl(&pb, false);
            return false;
        }
    }

    for (i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        gBufferBusy[i] = 0;
        gPcmFilled[i]  = 0;
    }
    gNextFillBuf      = 0;
    gPrebuffering     = true;
    gLastEmptyStatusTick = 0;
    gLastAudioPumpTick   = 0;
    gAudioPumpState   = PUMP_IDLE;
    gAudioNeedsStop   = false;
    memset(&gAudioPumpPb, 0, sizeof(gAudioPumpPb));
    gAudioRunning     = true;
    return true;
}

void AudioStreamStop(void)
{
    TCPiopb pb;
    short i;

    if (!gAudioRunning && !gAudioNeedsStop && gAudioStream == 0 && !gSndChan) return;
    gAudioPumpLock = 1;
    gAudioRunning   = false;
    gAudioNeedsStop = false;
    gPrebuffering   = true;
    gAudioPumpState = PUMP_IDLE;
    gAudioPumpLock = 0;

    /* Flush and quiet Sound Manager channel immediately, but keep gSndChan & gAudioStream
     * alive so resuming playback avoids SndNewChannel/SndDisposeChannel and TCPCreate/TCPRelease! */
    if (gSndChan) {
        SndCommand cmd;
        cmd.cmd = flushCmd; cmd.param1 = 0; cmd.param2 = 0;
        SndDoImmediate(gSndChan, &cmd);
        cmd.cmd = quietCmd; cmd.param1 = 0; cmd.param2 = 0;
        SndDoImmediate(gSndChan, &cmd);
    }

    if (gAudioStream != 0) {
        memset(&pb, 0, sizeof(pb));
        pb.csCode    = TCPAbort;
        pb.tcpStream = gAudioStream;
        RunTCPControl(&pb, false);
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

void AudioSetBackgroundMode(Boolean inBackground)
{
    gAudioInBackground = inBackground;
}

Boolean AudioGetLiveSamples(unsigned char *outSamples, short count)
{
    short playBuf = -1;
    short i;
    long  bufLen;
    long  elapsedTicks;
    long  samplesPerTick;
    long  offset;

    if (!outSamples || count <= 0 || !gAudioRunning) return false;

    /* Find the earliest-queued busy Sound Manager buffer (the one currently playing) */
    for (i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        if (gBufferBusy[i]) {
            if (playBuf < 0 || (long)(gBufferQueuedTick[playBuf] - gBufferQueuedTick[i]) > 0) {
                playBuf = i;
            }
        }
    }
    if (playBuf < 0) {
        return false;
    }

    if (!gPcmBuffers[playBuf] || gBufferQueuedLen[playBuf] < count) {
        return false;
    }

    bufLen = gBufferQueuedLen[playBuf];
    elapsedTicks = (long)(TickCount() - gBufferQueuedTick[playBuf]);
    if (elapsedTicks < 0) elapsedTicks = 0;
    if (elapsedTicks > 24) elapsedTicks = 24;

    samplesPerTick = (gAudioSampleRateHz <= 12000) ? 185L : 371L;
    offset = elapsedTicks * samplesPerTick;
    if (offset > bufLen - count) {
        offset = bufLen - count;
    }
    if (offset < 0) {
        offset = 0;
    }

    memcpy(outSamples, gPcmBuffers[playBuf] + offset, (size_t)count);
    return true;
}

static void AudioStreamPumpCore(void)
{
    short attempts;
    short i;
    short busyCount = 0;
    long  minQueueSize;
    unsigned long nowTicks;

    if (!gAudioRunning || gAudioNeedsStop || gAudioStream == 0 || !gSndChan) return;
    if (gAudioPumpLock) return;
    gAudioPumpLock = 1;

    for (i = 0; i < NUM_AUDIO_BUFFERS; i++) {
        if (gBufferBusy[i]) {
            busyCount++;
        }
    }

    /* Fast path: if all 6 Sound Manager buffers are busy playing, return immediately */
    if (!gPrebuffering && busyCount == NUM_AUDIO_BUFFERS && gAudioPumpState == PUMP_IDLE) {
        goto unlock_out;
    }

    nowTicks = FAST_TICKS();
    if (gAudioPumpState == PUMP_IDLE && gLastEmptyStatusTick != 0 && nowTicks == gLastEmptyStatusTick) {
        goto unlock_out;
    }
    gLastAudioPumpTick = nowTicks;

    if (busyCount == 0 && gAudioPumpState == PUMP_IDLE) {
        Boolean allEmpty = true;
        for (i = 0; i < NUM_AUDIO_BUFFERS; i++) {
            if (gPcmFilled[i] != 0) {
                allEmpty = false;
                break;
            }
        }
        if (allEmpty) {
            gPrebuffering = true;
            gNextFillBuf  = 0;
        }
    }

    minQueueSize = AUDIO_CHUNK_SIZE;

    for (attempts = 0; attempts < NUM_AUDIO_BUFFERS; attempts++) {
        short idx;
        unsigned short unread = 0;
        unsigned char connState = 8;
        long targetSize = AUDIO_CHUNK_SIZE;
        long needed;
        unsigned short toRead;

        if (gAudioPumpState == PUMP_WAITING_STATUS) {
            if (gAudioPumpPb.ioResult == inProgress) {
                goto unlock_out;
            }
            gAudioPumpState = PUMP_IDLE;
            if (gAudioPumpPb.ioResult != noErr) {
                gAudioNeedsStop = true;
                goto unlock_out;
            }
            unread    = gAudioPumpPb.csParam.status.amtUnreadData;
            connState = gAudioPumpPb.csParam.status.connectionState;
            idx       = gPrebuffering ? ((gPcmFilled[0] < AUDIO_CHUNK_SIZE) ? 0 : 1) : gNextFillBuf;
            goto have_status;
        }

        if (gAudioPumpState == PUMP_WAITING_RCV) {
            if (gAudioPumpPb.ioResult == inProgress) {
                goto unlock_out;
            }
            gAudioPumpState = PUMP_IDLE;
            idx = gAudioPendingRcvBufIdx;
            if (gAudioPumpPb.ioResult == noErr && gAudioPumpPb.csParam.receive.rcvBuffLen > 0) {
                gPcmFilled[idx] += gAudioPumpPb.csParam.receive.rcvBuffLen;
                goto check_queue_buf;
            } else {
                goto unlock_out;
            }
        }

        idx = gPrebuffering ? ((gPcmFilled[0] < AUDIO_CHUNK_SIZE) ? 0 : 1) : gNextFillBuf;
        if (gBufferBusy[idx]) {
            break;
        }

        memset(&gAudioPumpPb, 0, sizeof(gAudioPumpPb));
        gAudioPumpPb.ioCRefNum = gMacTCPRef;
        gAudioPumpPb.csCode    = TCPStatus;
        gAudioPumpPb.tcpStream = gAudioStream;
        PBControlAsync((ParmBlkPtr)&gAudioPumpPb);
        if (gAudioPumpPb.ioResult == inProgress) {
            gAudioPumpState = PUMP_WAITING_STATUS;
            goto unlock_out;
        }
        if (gAudioPumpPb.ioResult != noErr) {
            gAudioNeedsStop = true;
            goto unlock_out;
        }

        unread    = gAudioPumpPb.csParam.status.amtUnreadData;
        connState = gAudioPumpPb.csParam.status.connectionState;

have_status:
        if (gBufferBusy[idx]) {
            break;
        }
        if (unread == 0) {
            if (connState != 8 && busyCount == 0) {
                gAudioNeedsStop = true;
                goto unlock_out;
            }
            gLastEmptyStatusTick = nowTicks;
            break;
        }
        gLastEmptyStatusTick = 0;

        needed = targetSize - gPcmFilled[idx];
        if (needed <= 0) {
            goto check_queue_buf;
        }
        toRead = (unread > (unsigned short)needed) ? (unsigned short)needed : unread;

        memset(&gAudioPumpPb, 0, sizeof(gAudioPumpPb));
        gAudioPumpPb.ioCRefNum                           = gMacTCPRef;
        gAudioPumpPb.csCode                              = TCPRcv;
        gAudioPumpPb.tcpStream                           = gAudioStream;
        gAudioPumpPb.csParam.receive.commandTimeoutValue = 1;
        gAudioPumpPb.csParam.receive.rcvBuff             = gPcmBuffers[idx] + gPcmFilled[idx];
        gAudioPumpPb.csParam.receive.rcvBuffLen          = toRead;
        gAudioPendingRcvBufIdx                           = idx;

        PBControlAsync((ParmBlkPtr)&gAudioPumpPb);
        if (gAudioPumpPb.ioResult == inProgress) {
            gAudioPumpState = PUMP_WAITING_RCV;
            goto unlock_out;
        }
        if (gAudioPumpPb.ioResult == noErr && gAudioPumpPb.csParam.receive.rcvBuffLen > 0) {
            gPcmFilled[idx] += gAudioPumpPb.csParam.receive.rcvBuffLen;
        } else {
            break;
        }

check_queue_buf:
        if (gPrebuffering) {
            if (gPcmFilled[0] >= AUDIO_CHUNK_SIZE && gPcmFilled[1] >= (AUDIO_CHUNK_SIZE / 2)) {
                gPrebuffering = false;
                QueueSoundBuffer(0);
                busyCount++;
                if (gPcmFilled[1] >= AUDIO_CHUNK_SIZE) {
                    QueueSoundBuffer(1);
                    busyCount++;
                    gNextFillBuf = 2;
                } else {
                    gNextFillBuf = 1;
                }
            }
        } else if (gPcmFilled[idx] >= minQueueSize || (busyCount <= 1 && gPcmFilled[idx] >= (AUDIO_CHUNK_SIZE / 2))) {
            QueueSoundBuffer(idx);
            busyCount++;
            gNextFillBuf = (idx + 1) % NUM_AUDIO_BUFFERS;
        } else {
            break;
        }
    }

unlock_out:
    gAudioPumpLock = 0;
}

void AudioInterruptPumpC(void)
{
    if (gAudioRunning && !gAudioNeedsStop && !gAudioPumpLock) {
        AudioStreamPumpCore();
    }
    if (gAudioTmInstalled) {
        PrimeTime((QElemPtr)&gAudioTmTask, 20);
    }
}

void AudioHookPumpC(void)
{
    if (gAudioRunning && !gAudioNeedsStop && !gAudioPumpLock) {
        AudioStreamPumpCore();
    }
}

void AudioStreamPump(void)
{
    if (gAudioNeedsStop) {
        AudioStreamStop();
        return;
    }
    AudioStreamPumpCore();
    if (gAudioNeedsStop) {
        AudioStreamStop();
    }
}
