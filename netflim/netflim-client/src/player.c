#include "player_internal.h"
#include <Files.h>
#include <LowMem.h>
#include <Memory.h>
#include <Traps.h>
#include <stdio.h>
#include <string.h>

#define PREBUFFER_FRAMES      12

/* ============================================================================
 * Globals
 * ============================================================================ */

static Boolean         gPlayerInited     = false;
PlayerState            gState            = kPlayerStopped;
Boolean                gFullscreen       = false;
Boolean                gMuted            = false;
Boolean                gHalfAudio        = false;
Boolean                gLooping          = false;
Boolean                gCanDirectVRAM    = false;
Boolean                gUsingDirectVRAM  = false;
Boolean                gUsingWindowVRAM  = false;

unsigned char         *gScreenBaseAddr   = NULL;
unsigned char         *gOffscreenFB      = NULL;
static BitMap          gOffscreenBM;
static WindowPtr       gFsWindow         = NULL;

/* Direct .Sound (-4) interrupt-driven zero-copy audio & Sound Manager fallback */
Boolean                gUseDirectSndDrv  = true;
static Boolean         gDirectSndOpen    = false;
static short           gDirectSndRef     = SND_DRIVER_REFNUM;
DirectSoundPB          gDirectSndPB;
static IOCompletionUPP gDirectSndCompUPP = NULL;
unsigned char          gSilenceFFBuf[SND_SILENCE_MAX_BYTES];
long                   gSilenceReqByTicks[9];
volatile Boolean       gDirectActive     = false;
volatile Boolean       gDirectStopReq    = false;
volatile Boolean       gDirectFailed     = false;
volatile Boolean       gDirectEosReached = false;
volatile Boolean       gDirectNeedsBlit  = false;
volatile unsigned long gDirectCurFrame   = 0;
volatile unsigned long gDirectNextFrame  = 0;
volatile unsigned long gDirectCallbacks  = 0;
volatile unsigned long gDirectLastTick   = 0;

static SndChannelPtr   gSndChan          = NULL;
static SndCallBackUPP  gSndCallBackUPP   = NULL;
static unsigned char  *gSndBufs[SND_NUM_BUFS];
static SoundHeader     gSndHdrs[SND_NUM_BUFS];
static volatile short  gSndBufBusy[SND_NUM_BUFS];
short                  gSndWriteSlot     = -1;
unsigned short         gSndWritePos      = 0;
static short           gSndNextSlot      = 0;

/* Active playback metadata & clock */
static char            gVideoId[18];
char                   gVideoTitle[48];
char                   gActiveProfile[10];
char                   gStatusMsg[96]    = "Ready";
unsigned long          gStartReqTick     = 0;
volatile unsigned long gCurrentFrame     = 0;
unsigned long          gTotalFrames      = 0;
volatile unsigned long gElapsedTicks     = 0;
unsigned long          gTotalTicks       = 0;
unsigned long          gNextFrameDueTick = 0;
unsigned long          gNextHudTick      = 0;
unsigned short         gByterate         = 0;
Boolean                gVodStreamSilent  = false;
short                  gLastHudBuf       = -1;

typedef Boolean (*PlayerTickFn)(WindowPtr mainWin, const Rect *viewportRect, Boolean *outUiChanged);
static PlayerTickFn    gPlayerTickFn     = PlayerTick_68k;

/* ============================================================================
 * Direct .Sound Driver (ffMode + ioCompletion) & Sound Manager Fallback Pipeline
 * ============================================================================ */

static void UpdateSilenceBufferRate(void) {
    unsigned long rate = gHalfAudio ? SND_FF_RATE_11K : SND_FF_RATE_22K;
    long perTick = gHalfAudio ? 185L : 370L;
    short i;

    gSilenceFFBuf[0] = 0x00;
    gSilenceFFBuf[1] = 0x00;
    gSilenceFFBuf[2] = (unsigned char)((rate >> 24) & 0xFFU);
    gSilenceFFBuf[3] = (unsigned char)((rate >> 16) & 0xFFU);
    gSilenceFFBuf[4] = (unsigned char)((rate >> 8) & 0xFFU);
    gSilenceFFBuf[5] = (unsigned char)(rate & 0xFFU);

    for (i = 0; i <= 8; i++) {
        gSilenceReqByTicks[i] = (long)SND_FF_HDR_BYTES + ((long)(i ? i : 1) * perTick);
    }
}

/*
 * Classic Mac .Sound (-4) PBWrite takes ioBuffer = FFSynthRec* ([mode(2)][rate(4)][PCM...])
 * and expects ioReqCount = 6 (header) + PCM sample byte count.
 */
static long CalcSilenceFFBytes(unsigned short ticks) {
    unsigned long perTick = gHalfAudio ? 185UL : 370UL;
    unsigned long pcmBytes = (unsigned long)(ticks ? ticks : 1) * perTick;
    unsigned long maxPcm = (unsigned long)(SND_SILENCE_MAX_BYTES - SND_FF_HDR_BYTES);
    if (pcmBytes > maxPcm) pcmBytes = maxPcm;
    return (long)(SND_FF_HDR_BYTES + pcmBytes);
}

static void BlitCurrentFrame(WindowPtr mainWin, const Rect *viewportRect);

/*
 * Direct .Sound (-4) Device Manager ioCompletion interrupt helper:
 * Saves A5 into PB header for NetFlimDirectSoundCompletionAsm_{68k,020}.
 */
static void PlayerSaveCurrentA5(void) {
    unsigned long a5 = 0;
#ifdef __m68k__
    __asm__ volatile ("move.l %%a5,%0" : "=g"(a5));
#endif
    gDirectSndPB.magic   = DIRECT_SND_PB_MAGIC;
    gDirectSndPB.savedA5 = a5;
}

static Boolean EnsureDirectSoundDriverOpen(void) {
    DCtlHandle dctl;
    OSErr err;

    if (gDirectSndOpen) return true;

    /*
     * Check Unit Table entry for .Sound (-4) first WITHOUT calling OpenDriver
     * unnecessarily: MacFlim writes straight to refNum -4, and re-opening an
     * already-open .Sound driver runs dOpen again which can reset/stall it on
     * System 7 / Sound Manager 3.x Macs.
     */
    dctl = GetDCtlEntry(SND_DRIVER_REFNUM);
    if (!dctl || !*dctl || !(**dctl).dCtlDriver) {
        return false;
    }

    if ((**dctl).dCtlFlags & dOpenedMask) {
        gDirectSndRef = SND_DRIVER_REFNUM;
        gDirectSndOpen = true;
        return true;
    }

    err = OpenDriver("\p.Sound", &gDirectSndRef);
    if (err != noErr) {
        return false;
    }
    gDirectSndOpen = true;
    return true;
}

static Boolean StartDirectAudioAtFrame(unsigned long startIdx, WindowPtr mainWin, const Rect *viewportRect) {
    const NetVodFrameDesc *slot;
    Ptr bufPtr;
    long reqCount;
    unsigned short ticks;
    OSErr err;

    if (!gUseDirectSndDrv || !gDirectSndCompUPP) return false;
    if (!EnsureDirectSoundDriverOpen()) return false;

    slot = NetVodIrqPeekSlot(startIdx);
    if (!slot) {
        return false;
    }

    UpdateSilenceBufferRate();

    /* Decode initial frame video first so audio and frame image begin in lockstep */
    if (slot->videoBytes >= 4 && slot->codecId != NETFLIM_CODEC_NULL) {
        if (gUsingDirectVRAM && gScreenBaseAddr) {
            NetFlimDecodeVideo(gScreenBaseAddr, slot->videoPtr, slot->videoBytes);
        } else {
            if (NetFlimDecodeVideo(gOffscreenFB, slot->videoPtr, slot->videoBytes) > 0) {
                BlitCurrentFrame(mainWin, viewportRect);
            }
        }
    }

    ticks = slot->ticks;
    if (!gMuted && !gVodStreamSilent && slot->ffReqBytes > 0 && slot->ffSynthPtr != NULL) {
        bufPtr   = (Ptr)slot->ffSynthPtr;
        reqCount = (long)slot->ffReqBytes;
    } else {
        bufPtr   = (Ptr)gSilenceFFBuf;
        reqCount = (ticks <= 8U) ? gSilenceReqByTicks[ticks] : CalcSilenceFFBytes(ticks);
    }

    memset(&gDirectSndPB, 0, sizeof(gDirectSndPB));
    PlayerSaveCurrentA5();
    gDirectSndPB.pb.ioParam.ioCompletion = gDirectSndCompUPP;
    gDirectSndPB.pb.ioParam.ioRefNum     = gDirectSndRef;
    gDirectSndPB.pb.ioParam.ioBuffer     = bufPtr;
    gDirectSndPB.pb.ioParam.ioReqCount   = reqCount;
    gDirectSndPB.pb.ioParam.ioPosMode    = 0;
    gDirectSndPB.pb.ioParam.ioPosOffset  = 0;

    gDirectStopReq    = false;
    gDirectFailed     = false;
    gDirectEosReached = false;
    gDirectNeedsBlit  = false;
    gDirectCurFrame   = startIdx;
    gDirectNextFrame  = startIdx + 1UL;
    gDirectCallbacks  = 0;
    gDirectLastTick   = FAST_TICKS();
    gDirectActive     = true;
    gCurrentFrame     = startIdx + 1UL;
    gElapsedTicks    += (unsigned long)ticks;
    __asm__ volatile ("" ::: "memory");

    err = PBWriteAsync((ParmBlkPtr)&gDirectSndPB.pb);
    if (err != noErr) {
        gDirectActive     = false;
        gDirectCurFrame   = 0xFFFFFFFFUL;
        gDirectNextFrame  = startIdx;
        gCurrentFrame     = startIdx;
        if (gElapsedTicks >= (unsigned long)ticks) {
            gElapsedTicks -= (unsigned long)ticks;
        }
        return false;
    }

    return true;
}

static pascal void SoundBufferCallback(SndChannelPtr chan, SndCommand *cmd) {
    (void)chan;
    if (cmd && cmd->param2) {
        *(volatile short *)cmd->param2 = 0;
    }
}

static void EnsureSoundChannelOpen(void) {
    OSErr err;
    if (gSndChan != NULL) return;
    if (!gSndCallBackUPP) {
        gSndCallBackUPP = NewSndCallBackProc(SoundBufferCallback);
    }
    /* initNoInterp disables Sound Manager software linear interpolation on 68030 */
    err = SndNewChannel(&gSndChan, sampledSynth, initMono | initNoInterp, gSndCallBackUPP);
    if (err != noErr) {
        gSndChan = NULL;
    }
}

static void FlushAudioChannel(void) {
    SndCommand cmd;
    short i;

    if (gDirectActive) {
        ParamBlockRec killPB;
        gDirectStopReq = true;
        __asm__ volatile ("" ::: "memory");
        memset(&killPB, 0, sizeof(killPB));
        killPB.ioParam.ioRefNum = gDirectSndRef;
        PBKillIO(&killPB, false);
        gDirectActive = false;
    } else {
        gDirectStopReq = true;
    }
    gDirectNeedsBlit  = false;
    gDirectEosReached = false;
    gDirectFailed     = false;

    if (gSndChan) {
        cmd.cmd    = flushCmd;
        cmd.param1 = 0;
        cmd.param2 = 0;
        SndDoImmediate(gSndChan, &cmd);
        cmd.cmd    = quietCmd;
        cmd.param1 = 0;
        cmd.param2 = 0;
        SndDoImmediate(gSndChan, &cmd);
    }

    for (i = 0; i < SND_NUM_BUFS; i++) {
        gSndBufBusy[i] = 0;
    }
    gSndWriteSlot = -1;
    gSndWritePos  = 0;
    gSndNextSlot  = 0;
}

static void CloseSoundChannel(void) {
    FlushAudioChannel();
    if (gSndChan) {
        SndDisposeChannel(gSndChan, true);
        gSndChan = NULL;
    }
}

static Boolean IsAudioSlotBusy(short slot) {
    return (gSndBufBusy[slot] != 0);
}

static short CountBusySoundBuffers(void) {
    short i, busy = 0;
    for (i = 0; i < SND_NUM_BUFS; i++) {
        if (gSndBufBusy[i] != 0) busy++;
    }
    return busy;
}

static short AcquireAudioWriteSlot(void) {
    short tries;
    if (gSndWriteSlot >= 0) return gSndWriteSlot;

    for (tries = 0; tries < SND_NUM_BUFS; tries++) {
        short cand = (short)((gSndNextSlot + tries) & SND_BUF_MASK);
        if (!IsAudioSlotBusy(cand) && gSndBufs[cand] != NULL) {
            gSndWriteSlot = cand;
            gSndWritePos  = 0;
            gSndNextSlot  = (short)((cand + 1) & SND_BUF_MASK);
            return cand;
        }
    }
    return -1;
}

static void DispatchCurrentAudioSlot(void) {
    short slot = gSndWriteSlot;
    SndCommand cmd;
    unsigned long activeRate = gHalfAudio ? rate11khz : rate22khz;
    unsigned short primeLen = gHalfAudio ? (unsigned short)(SND_PRIME_SILENCE / 2) : (unsigned short)SND_PRIME_SILENCE;

    if (slot < 0 || gSndWritePos == 0 || gMuted || gVodStreamSilent) {
        gSndWriteSlot = -1;
        gSndWritePos  = 0;
        return;
    }

    if (!gSndChan) {
        EnsureSoundChannelOpen();
        if (!gSndChan) {
            gSndWriteSlot = -1;
            gSndWritePos  = 0;
            return;
        }
    }

    if (CountBusySoundBuffers() == 0) {
        short tries;
        for (tries = 0; tries < SND_NUM_BUFS; tries++) {
            short pSlot = (short)((slot + 1 + tries) & SND_BUF_MASK);
            if (pSlot != slot && !IsAudioSlotBusy(pSlot) && gSndBufs[pSlot] != NULL) {
                SndCommand pCmd;
                memset(gSndBufs[pSlot] + SND_FF_HDR_BYTES, 0x80, primeLen);
                gSndHdrs[pSlot].samplePtr  = (Ptr)(gSndBufs[pSlot] + SND_FF_HDR_BYTES);
                gSndHdrs[pSlot].sampleRate = activeRate;
                gSndHdrs[pSlot].length     = primeLen;
                gSndBufBusy[pSlot]         = 1;

                pCmd.cmd    = bufferCmd;
                pCmd.param1 = 0;
                pCmd.param2 = (long)&gSndHdrs[pSlot];
                if (SndDoCommand(gSndChan, &pCmd, true) == noErr) {
                    pCmd.cmd    = callBackCmd;
                    pCmd.param1 = 0;
                    pCmd.param2 = (long)&gSndBufBusy[pSlot];
                    if (SndDoCommand(gSndChan, &pCmd, true) != noErr) {
                        gSndBufBusy[pSlot] = 0;
                    }
                } else {
                    gSndBufBusy[pSlot] = 0;
                }
                break;
            }
        }
    }

    gSndHdrs[slot].samplePtr  = (Ptr)(gSndBufs[slot] + SND_FF_HDR_BYTES);
    gSndHdrs[slot].sampleRate = activeRate;
    gSndHdrs[slot].length     = gSndWritePos;
    gSndBufBusy[slot]         = 1;
    gSndWriteSlot             = -1;
    gSndWritePos              = 0;

    cmd.cmd    = bufferCmd;
    cmd.param1 = 0;
    cmd.param2 = (long)&gSndHdrs[slot];
    if (SndDoCommand(gSndChan, &cmd, true) != noErr) {
        gSndBufBusy[slot] = 0;
        return;
    }

    cmd.cmd    = callBackCmd;
    cmd.param1 = 0;
    cmd.param2 = (long)&gSndBufBusy[slot];
    if (SndDoCommand(gSndChan, &cmd, true) != noErr) {
        gSndBufBusy[slot] = 0;
    }
}

static void FeedFrameAudio(const unsigned char *pcm, unsigned short pcmBytes) {
    unsigned short offset = 0;
    unsigned short flushThresh = gHalfAudio ? (unsigned short)(SND_FLUSH_THRESHOLD / 2) : (unsigned short)SND_FLUSH_THRESHOLD;
    unsigned short minCold     = gHalfAudio ? 925U : 1850U;

    if (gMuted || gVodStreamSilent || !pcm || pcmBytes == 0) return;
    if (!gSndChan) return;

    while (offset < pcmBytes) {
        short slot = AcquireAudioWriteSlot();
        unsigned short avail;
        unsigned short take;

        if (slot < 0) return;

        avail = (unsigned short)(SND_BUF_SIZE - gSndWritePos);
        take  = (unsigned short)(pcmBytes - offset);
        if (take > avail) take = avail;

        NetFlimFastCopy(gSndBufs[slot] + SND_FF_HDR_BYTES + gSndWritePos, pcm + offset, take);
        gSndWritePos = (unsigned short)(gSndWritePos + take);
        offset       = (unsigned short)(offset + take);

        if ((CountBusySoundBuffers() == 0 && gSndWritePos >= minCold) ||
            gSndWritePos >= flushThresh ||
            gSndWritePos >= SND_BUF_SIZE) {
            DispatchCurrentAudioSlot();
        }
    }
}

/* ============================================================================
 * Video Rendering (1:1 Unscaled Windowed & Direct-VRAM Fullscreen)
 * ============================================================================ */

static void Compute1To1VideoRects(const Rect *bounds, Rect *outSrc, Rect *outDst) {
    short bw = bounds->right - bounds->left;
    short bh = bounds->bottom - bounds->top;
    short copyW, copyH;
    short srcLeft, srcTop;
    short dstLeft, dstTop;

    if (bw <= 0 || bh <= 0) {
        SetRect(outSrc, 0, 0, NETFLIM_WIDTH, NETFLIM_HEIGHT);
        *outDst = *bounds;
        return;
    }

    /* Strictly 1:1 unscaled crop/center with 16-pixel horizontal word alignment */
    copyW = (bw < NETFLIM_WIDTH) ? (bw & ~15) : NETFLIM_WIDTH;
    if (copyW <= 0) copyW = bw;
    copyH = (bh < NETFLIM_HEIGHT) ? bh : NETFLIM_HEIGHT;

    srcLeft = ((NETFLIM_WIDTH - copyW) / 2) & ~15;
    srcTop  = (NETFLIM_HEIGHT - copyH) / 2;

    dstLeft = (bounds->left + (bw - copyW) / 2) & ~15;
    if (dstLeft < bounds->left) dstLeft = bounds->left;
    dstTop  = bounds->top + (bh - copyH) / 2;

    SetRect(outSrc, srcLeft, srcTop, srcLeft + copyW, srcTop + copyH);
    SetRect(outDst, dstLeft, dstTop, dstLeft + copyW, dstTop + copyH);
}

static void ClearRectBlack(const Rect *r) {
    ForeColor(blackColor);
    BackColor(whiteColor);
    PaintRect(r);
}

static void SyncWindowVRAMToOffscreen(void) {
    if (gUsingWindowVRAM && gOffscreenFB) {
        /* Copy the 256 visible windowed scanlines (screen y=64..319 -> flim y=43..298) */
        NetFlimFastCopy(gOffscreenFB + (43UL * NETFLIM_ROW_BYTES),
                        (const unsigned char *)qd.screenBits.baseAddr + (64UL * NETFLIM_ROW_BYTES),
                        256UL * NETFLIM_ROW_BYTES);
    }
}

static void EnterFullscreenVisual(void) {
    Rect scr = qd.screenBits.bounds;

    if (gCanDirectVRAM) {
        /* 1:1 Compact Mac (SE / SE/30 512x342 monochrome CRT, rowBytes=64): zero CopyBits overhead! */
        SyncWindowVRAMToOffscreen();
        gUsingWindowVRAM = false;
        HideCursor();
        NetFlimFastCopy((void *)qd.screenBits.baseAddr, gOffscreenFB, NETFLIM_FB_BYTES);
        gUsingDirectVRAM = true;
    } else {
        if (!gFsWindow) {
            gFsWindow = NewWindow(NULL, &scr, "\p", true, plainDBox, (WindowPtr)-1L, false, 0);
        }
        if (gFsWindow) {
            GrafPtr savedPort;
            Rect srcR, dstR;

            GetPort(&savedPort);
            SetPort(gFsWindow);
            ClearRectBlack(&gFsWindow->portRect);

            Compute1To1VideoRects(&gFsWindow->portRect, &srcR, &dstR);
            ForeColor(blackColor);
            BackColor(whiteColor);
            CopyBits(&gOffscreenBM, &gFsWindow->portBits, &srcR, &dstR, srcCopy, NULL);
            HideCursor();
            SetPort(savedPort);
        }
        gUsingDirectVRAM = false;
        gUsingWindowVRAM = false;
    }
}

static void ExitFullscreenVisual(WindowPtr mainWin) {
    if (gUsingDirectVRAM) {
        /* Save current CRT frame back into offscreen buffer before restoring desktop */
        NetFlimFastCopy(gOffscreenFB, (const void *)qd.screenBits.baseAddr, NETFLIM_FB_BYTES);
        gUsingDirectVRAM = false;
        gUsingWindowVRAM = false;
        ShowCursor();
        DrawMenuBar();
        if (mainWin) {
            GrafPtr savedPort;
            GetPort(&savedPort);
            SetPort(mainWin);
            InvalRect(&mainWin->portRect);
            PaintBehind(mainWin, GetGrayRgn());
            SetPort(savedPort);
        } else {
            PaintBehind(FrontWindow(), GetGrayRgn());
        }
    } else {
        ShowCursor();
        if (gFsWindow) {
            DisposeWindow(gFsWindow);
            gFsWindow = NULL;
        }
        DrawMenuBar();
        if (mainWin) {
            GrafPtr savedPort;
            GetPort(&savedPort);
            SetPort(mainWin);
            SelectWindow(mainWin);
            InvalRect(&mainWin->portRect);
            SetPort(savedPort);
        }
    }
}

void PlayerDrawViewport(WindowPtr mainWin, const Rect *viewportRect) {
    GrafPtr savedPort;
    Rect srcR, dstR;

    if (!mainWin || !viewportRect || !gOffscreenFB) return;
    if (gFullscreen && gUsingDirectVRAM) return;

    GetPort(&savedPort);
    SetPort(mainWin);

    Compute1To1VideoRects(viewportRect, &srcR, &dstR);
    if (dstR.left > viewportRect->left || dstR.top > viewportRect->top ||
        dstR.right < viewportRect->right || dstR.bottom < viewportRect->bottom) {
        ClearRectBlack(viewportRect);
    }
    ForeColor(blackColor);
    BackColor(whiteColor);
    CopyBits(&gOffscreenBM, &mainWin->portBits, &srcR, &dstR, srcCopy, NULL);

    SetPort(savedPort);
}

static void BlitCurrentFrame(WindowPtr mainWin, const Rect *viewportRect) {
    Rect srcR, dstR;
    GrafPtr savedPort;

    if (gFullscreen) {
        if (gUsingDirectVRAM) {
            /* Direct mode decoded straight into screenBits.baseAddr! */
            return;
        }
        if (gFsWindow) {
            GetPort(&savedPort);
            SetPort(gFsWindow);
            Compute1To1VideoRects(&gFsWindow->portRect, &srcR, &dstR);
            ForeColor(blackColor);
            BackColor(whiteColor);
            CopyBits(&gOffscreenBM, &gFsWindow->portBits, &srcR, &dstR, srcCopy, NULL);
            SetPort(savedPort);
        }
        return;
    }

    if (gUsingWindowVRAM) {
        /* Windowed Direct-VRAM mode decoded visible deltas straight into screenBits.baseAddr! */
        return;
    }

    if (mainWin && viewportRect) {
        Compute1To1VideoRects(viewportRect, &srcR, &dstR);

        /* Direct-VRAM fast path for active 512px-wide window on SE / SE/30 */
        if (gCanDirectVRAM && mainWin == FrontWindow() &&
            srcR.left == 0 && (srcR.right - srcR.left) == NETFLIM_WIDTH &&
            dstR.left == 0 && mainWin->portBits.bounds.left == 0) {
            short globalTop = (short)(dstR.top - mainWin->portBits.bounds.top);
            short copyH     = (short)(dstR.bottom - dstR.top);
            if (globalTop >= 0 && (globalTop + copyH) <= NETFLIM_HEIGHT) {
                unsigned char *vramDst = (unsigned char *)qd.screenBits.baseAddr +
                                         ((unsigned long)globalTop * NETFLIM_ROW_BYTES);
                const unsigned char *fbSrc = gOffscreenFB +
                                             ((unsigned long)srcR.top * NETFLIM_ROW_BYTES);
                NetFlimFastCopy(vramDst, fbSrc, (unsigned long)copyH * NETFLIM_ROW_BYTES);
                return;
            }
        }

        GetPort(&savedPort);
        SetPort(mainWin);
        ForeColor(blackColor);
        BackColor(whiteColor);
        CopyBits(&gOffscreenBM, &mainWin->portBits, &srcR, &dstR, srcCopy, NULL);
        SetPort(savedPort);
    }
}

/* ============================================================================
 * Lifecycle & State Controls
 * ============================================================================ */

static Boolean LowerBufContains(const unsigned char *buf, short len, const char *needle) {
    short nlen = (short)strlen(needle);
    short i, j;
    if (!buf || nlen <= 0 || len < nlen) return false;
    for (i = 0; i <= len - nlen; i++) {
        Boolean match = true;
        for (j = 0; j < nlen; j++) {
            unsigned char c = buf[i + j];
            if (c >= 'A' && c <= 'Z') c = (unsigned char)(c + ('a' - 'A'));
            if (c != (unsigned char)needle[j]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/*
 * Classic 68k emulators (Basilisk II / SheepShaver) stub legacy ".Sound" (-4)
 * PBWrite as a silent no-op while only implementing Sound Manager 3.x ('sdev').
 * Scan the Device Manager Unit Table once at init to detect emulator drivers
 * or a missing -4 DCE so Sound Manager (initMono | initNoInterp) is selected
 * immediately instead of playing silence.
 */
static Boolean ShouldDefaultDirectSoundDriver(void) {
    DCtlHandle sndDctl;
    short count;
    short unit;

    sndDctl = GetDCtlEntry(SND_DRIVER_REFNUM);
    if (!sndDctl || !*sndDctl || !(**sndDctl).dCtlDriver) {
        return false;
    }

    count = LMGetUnitTableEntryCount();
    if (count <= 0 || count > 128) count = 64;

    for (unit = 0; unit < count; unit++) {
        DCtlHandle dctl = GetDCtlEntry((short)(-1 - unit));
        Ptr drvrPtr;
        DRVRHeaderPtr hdr;
        short flags;
        unsigned char nameLen;

        if (!dctl || !*dctl) continue;
        drvrPtr = (**dctl).dCtlDriver;
        if (!drvrPtr) continue;
        flags = (**dctl).dCtlFlags;

        if (flags & dRAMBasedMask) {
            Handle h = (Handle)drvrPtr;
            if (!h || !*h) continue;
            hdr = (DRVRHeaderPtr)*h;
        } else {
            hdr = (DRVRHeaderPtr)drvrPtr;
        }

        if (!hdr || (((unsigned long)hdr & 1UL) != 0)) continue;
        nameLen = hdr->drvrName[0];
        if (nameLen == 0 || nameLen > 63) continue;

        if (LowerBufContains(&hdr->drvrName[1], (short)nameLen, "basilisk") ||
            LowerBufContains(&hdr->drvrName[1], (short)nameLen, "sheepshaver")) {
            return false;
        }
    }

    return true;
}

Boolean PlayerInit(void) {
    short i;
    SysEnvRec env;

    if (gPlayerInited) return true;

    gScreenBaseAddr = (unsigned char *)qd.screenBits.baseAddr;
    gOffscreenFB = (unsigned char *)NewPtrClear(NETFLIM_FB_BYTES);
    if (!gOffscreenFB) return false;
    NetFlimResetFramebuffer(gOffscreenFB);

    gOffscreenBM.baseAddr = (Ptr)gOffscreenFB;
    gOffscreenBM.rowBytes = NETFLIM_ROW_BYTES;
    SetRect(&gOffscreenBM.bounds, 0, 0, NETFLIM_WIDTH, NETFLIM_HEIGHT);

    memset(gSilenceFFBuf + SND_FF_HDR_BYTES, 0x80, SND_SILENCE_MAX_BYTES - SND_FF_HDR_BYTES);
    UpdateSilenceBufferRate();
    gPlayerTickFn = NetFlimCodecIs020() ? PlayerTick_020 : PlayerTick_68k;
    gDirectSndCompUPP = NewIOCompletionUPP((IOCompletionProcPtr)(
        NetFlimCodecIs020() ? NetFlimDirectSoundCompletionAsm_020
                            : NetFlimDirectSoundCompletionAsm_68k));

    for (i = 0; i < SND_NUM_BUFS; i++) {
        gSndBufs[i] = (unsigned char *)NewPtrClear(SND_FF_HDR_BYTES + SND_BUF_SIZE);
        if (gSndBufs[i]) {
            /* Pre-populate 6-byte FFSynthRec header: mode = ffMode(0), count = 1.0 (0x00010000) */
            gSndBufs[i][0] = 0x00;
            gSndBufs[i][1] = 0x00;
            gSndBufs[i][2] = 0x00;
            gSndBufs[i][3] = 0x01;
            gSndBufs[i][4] = 0x00;
            gSndBufs[i][5] = 0x00;
        }
        gSndBufBusy[i] = 0;
        memset(&gSndHdrs[i], 0, sizeof(SoundHeader));
        gSndHdrs[i].samplePtr     = gSndBufs[i] ? (Ptr)(gSndBufs[i] + SND_FF_HDR_BYTES) : NULL;
        gSndHdrs[i].sampleRate    = rate22khz;
        gSndHdrs[i].encode        = stdSH;
        gSndHdrs[i].baseFrequency = kMiddleC;
    }

    gCanDirectVRAM = false;
    if (SysEnvirons(1, &env) == noErr) {
        short sw = qd.screenBits.bounds.right - qd.screenBits.bounds.left;
        short sh = qd.screenBits.bounds.bottom - qd.screenBits.bounds.top;
        if (sw == NETFLIM_WIDTH && sh == NETFLIM_HEIGHT &&
            (qd.screenBits.rowBytes & 0x3FFF) == NETFLIM_ROW_BYTES) {
            if (!env.hasColorQD) {
                gCanDirectVRAM = true;
            } else {
                GDHandle gd = GetMainDevice();
                if (gd && *gd && (**gd).gdPMap && *(**gd).gdPMap &&
                    (*(**gd).gdPMap)->pixelSize == 1) {
                    gCanDirectVRAM = true;
                }
            }
        }
    }

    gUseDirectSndDrv = ShouldDefaultDirectSoundDriver();
    gPlayerInited = true;
    return true;
}

void PlayerShutdown(void) {
    short i;
    PlayerStop(NULL);
    CloseSoundChannel();

#if TARGET_RT_MAC_CFM
    if (gDirectSndCompUPP) {
        DisposeRoutineDescriptor((UniversalProcPtr)gDirectSndCompUPP);
    }
    if (gSndCallBackUPP) {
        DisposeRoutineDescriptor((UniversalProcPtr)gSndCallBackUPP);
    }
#endif
    gDirectSndCompUPP = NULL;
    gSndCallBackUPP = NULL;
    for (i = 0; i < SND_NUM_BUFS; i++) {
        if (gSndBufs[i]) {
            DisposePtr((Ptr)gSndBufs[i]);
            gSndBufs[i] = NULL;
        }
    }
    if (gOffscreenFB) {
        DisposePtr((Ptr)gOffscreenFB);
        gOffscreenFB = NULL;
    }
    gPlayerInited = false;
}

Boolean PlayerStart(const NetFlimVideoItem *item, WindowPtr mainWin, Boolean startFullscreen) {
    const char *prof;

    if (!gPlayerInited && !PlayerInit()) return false;
    if (!item || item->id[0] == '\0') return false;

    PlayerStop(mainWin);

    strncpy(gVideoId, item->id, sizeof(gVideoId) - 1);
    gVideoId[sizeof(gVideoId) - 1] = '\0';
    strncpy(gVideoTitle, item->title, sizeof(gVideoTitle) - 1);
    gVideoTitle[sizeof(gVideoTitle) - 1] = '\0';

    prof = NetResolveProfileName();
    strncpy(gActiveProfile, prof, sizeof(gActiveProfile) - 1);
    gActiveProfile[sizeof(gActiveProfile) - 1] = '\0';

    NetFlimResetFramebuffer(gOffscreenFB);
    if (!gMuted && !gUseDirectSndDrv) {
        EnsureSoundChannelOpen();
    }

    if (!NetVodOpen(gVideoId, gActiveProfile, gMuted, gHalfAudio, 0)) {
        gState = kPlayerError;
        strcpy(gStatusMsg, "Failed to open TCP stream (port 6800)");
        return false;
    }

    gState            = kPlayerBuffering;
    gCurrentFrame     = 0;
    gTotalFrames      = item->frameCount;
    gElapsedTicks     = 0;
    gTotalTicks       = item->durationSec * 60UL;
    gByterate         = 0;
    gVodStreamSilent  = gMuted || item->silent;
    gStartReqTick     = FAST_TICKS();
    gNextFrameDueTick = FAST_TICKS();
    gNextHudTick      = 0UL;
    gLastHudBuf       = -1;
    sprintf(gStatusMsg, "Buffering %s (%s)...", gVideoTitle, gActiveProfile);

    if (startFullscreen) {
        gFullscreen = true;
        EnterFullscreenVisual();
    } else {
        gFullscreen = false;
        gUsingWindowVRAM = false;
    }

    return true;
}

void PlayerStop(WindowPtr mainWin) {
    Boolean wasFs = gFullscreen;

    FlushAudioChannel();
    NetVodClose();
    gState = kPlayerStopped;
    gFullscreen = false;
    gUsingWindowVRAM = false;

    if (wasFs) {
        ExitFullscreenVisual(mainWin);
    } else if (mainWin) {
        GrafPtr savedPort;
        GetPort(&savedPort);
        SetPort(mainWin);
        InvalRect(&mainWin->portRect);
        SetPort(savedPort);
    }
    strcpy(gStatusMsg, "Stopped");
}

void PlayerTogglePause(void) {
    if (gState == kPlayerPlaying) {
        Boolean wasDirect = gDirectActive;
        SyncWindowVRAMToOffscreen();
        gState = kPlayerPaused;
        FlushAudioChannel();
        if (wasDirect) {
            unsigned long resumeAt = gDirectNextFrame;
            NetVodIrqSetCompletedUpTo(resumeAt);
            gCurrentFrame = resumeAt;
            NetVodPump();
        }
        NetVodSetPaused(true);
        strcpy(gStatusMsg, "Paused");
    } else if (gState == kPlayerPaused) {
        if (!gMuted && !gVodStreamSilent && !gUseDirectSndDrv) {
            EnsureSoundChannelOpen();
        }
        gState = kPlayerPlaying;
        gNextFrameDueTick = FAST_TICKS();
        NetVodSetPaused(false);
        sprintf(gStatusMsg, "Playing %s (%s)", gVideoTitle, gActiveProfile);
    }
}

void PlayerToggleLoop(void) {
    gLooping = !gLooping;
}

static void ReopenStreamForAudioChange(void) {
    if (gState == kPlayerPlaying || gState == kPlayerPaused || gState == kPlayerBuffering) {
        Boolean wasPaused = (gState == kPlayerPaused);
        Boolean wasDirect = (gState == kPlayerPlaying && gDirectActive);
        unsigned long resumeFrame;
        FlushAudioChannel();
        if (wasDirect) {
            NetVodIrqSetCompletedUpTo(gDirectNextFrame);
            gCurrentFrame = gDirectNextFrame;
        }
        resumeFrame = gCurrentFrame;
        if (NetVodOpen(gVideoId, gActiveProfile, gMuted, gHalfAudio, resumeFrame)) {
            gVodStreamSilent = gMuted;
            if (wasPaused) {
                NetVodSetPaused(true);
            } else {
                gState        = kPlayerBuffering;
                gStartReqTick = FAST_TICKS();
                sprintf(gStatusMsg, "Buffering %s (%s)...", gVideoTitle, gActiveProfile);
            }
            gNextFrameDueTick = FAST_TICKS();
        }
    }
}

void PlayerToggleMute(void) {
    gMuted = !gMuted;
    if (gMuted) {
        FlushAudioChannel();
    } else if ((gState == kPlayerPlaying || gState == kPlayerPaused) && !gUseDirectSndDrv) {
        EnsureSoundChannelOpen();
    }

    /* Re-open active VOD stream at current frame so server toggles audio stripping */
    ReopenStreamForAudioChange();
}

void PlayerToggleHalfAudio(void) {
    gHalfAudio = !gHalfAudio;
    UpdateSilenceBufferRate();
    ReopenStreamForAudioChange();
}

void PlayerToggleDirectAudio(void) {
    Boolean activeStream = (gState == kPlayerPlaying || gState == kPlayerPaused || gState == kPlayerBuffering);
    Boolean wasDirect = (gState == kPlayerPlaying && gDirectActive);

    if (gUseDirectSndDrv) {
        if (activeStream) {
            SyncWindowVRAMToOffscreen();
            FlushAudioChannel();
            if (wasDirect) {
                NetVodIrqSetCompletedUpTo(gDirectNextFrame);
                gCurrentFrame = gDirectNextFrame;
            }
        }
        gUseDirectSndDrv = false;
        if (!gMuted && !gVodStreamSilent && (gState == kPlayerPlaying || gState == kPlayerPaused)) {
            EnsureSoundChannelOpen();
        }
    } else {
        if (!EnsureDirectSoundDriverOpen()) {
            return;
        }
        if (activeStream) {
            CloseSoundChannel();
        }
        gUseDirectSndDrv = true;
        gDirectFailed    = false;
    }

    if (activeStream) {
        ReopenStreamForAudioChange();
    }
}

Boolean PlayerIsDirectAudio(void) {
    return gUseDirectSndDrv;
}

void PlayerSetFullscreen(Boolean fullscreen, WindowPtr mainWin) {
    Boolean wasDirect;

    if (gState == kPlayerStopped || gState == kPlayerError) {
        gFullscreen = false;
        return;
    }
    if (fullscreen == gFullscreen) return;

    wasDirect = (gState == kPlayerPlaying && gDirectActive);
    if (wasDirect) {
        unsigned long resumeAt;
        FlushAudioChannel();
        resumeAt = gDirectNextFrame;
        NetVodIrqSetCompletedUpTo(resumeAt);
        gCurrentFrame = resumeAt;
        NetVodPump();
    }

    gFullscreen = fullscreen;
    if (gFullscreen) {
        EnterFullscreenVisual();
    } else {
        ExitFullscreenVisual(mainWin);
    }
}

void PlayerToggleFullscreen(WindowPtr mainWin) {
    PlayerSetFullscreen(!gFullscreen, mainWin);
}

/* ============================================================================
 * Cold Helper Forwarders & Runtime Hot Dispatcher
 * ============================================================================ */

long PlayerInternalCalcSilenceFFBytes(unsigned short ticks) {
    return CalcSilenceFFBytes(ticks);
}

Boolean PlayerInternalStartDirectAudioAtFrame(unsigned long startIdx, WindowPtr mainWin, const Rect *viewportRect) {
    return StartDirectAudioAtFrame(startIdx, mainWin, viewportRect);
}

void PlayerInternalFlushAudioChannel(void) {
    FlushAudioChannel();
}

void PlayerInternalCloseSoundChannel(void) {
    CloseSoundChannel();
}

void PlayerInternalEnsureSoundChannelOpen(void) {
    EnsureSoundChannelOpen();
}

short PlayerInternalCountBusySoundBuffers(void) {
    return CountBusySoundBuffers();
}

void PlayerInternalDispatchCurrentAudioSlot(void) {
    DispatchCurrentAudioSlot();
}

void PlayerInternalFeedFrameAudio(const unsigned char *pcm, unsigned short pcmBytes) {
    FeedFrameAudio(pcm, pcmBytes);
}

void PlayerInternalExitFullscreenVisual(WindowPtr mainWin) {
    ExitFullscreenVisual(mainWin);
}

void PlayerInternalBlitCurrentFrame(WindowPtr mainWin, const Rect *viewportRect) {
    BlitCurrentFrame(mainWin, viewportRect);
}

Boolean PlayerTick(WindowPtr mainWin, const Rect *viewportRect, Boolean *outUiChanged) {
    return gPlayerTickFn(mainWin, viewportRect, outUiChanged);
}

Boolean PlayerWantsZeroSleep(void) {
    return (gState == kPlayerPlaying || gState == kPlayerBuffering);
}

PlayerState PlayerGetState(void)    { return gState; }
Boolean     PlayerIsFullscreen(void){ return gFullscreen; }
Boolean     PlayerIsMuted(void)     { return gMuted; }
Boolean     PlayerIsHalfAudio(void) { return gHalfAudio; }
Boolean     PlayerIsLooping(void)   { return gLooping; }

void PlayerGetSnapshot(PlayerSnapshot *out) {
    NetVodInfo v;
    if (!out) return;
    NetVodGetInfo(&v);
    out->state         = gState;
    out->fullscreen    = gFullscreen;
    out->muted         = gMuted;
    out->halfAudio     = gHalfAudio;
    out->looping       = gLooping;
    strncpy(out->videoId, gVideoId, sizeof(out->videoId));
    strncpy(out->videoTitle, gVideoTitle, sizeof(out->videoTitle));
    strncpy(out->activeProfile, gActiveProfile, sizeof(out->activeProfile));
    strncpy(out->statusMsg, gStatusMsg, sizeof(out->statusMsg));
    out->currentFrame  = gCurrentFrame;
    out->totalFrames   = gTotalFrames;
    out->elapsedTicks  = gElapsedTicks;
    out->totalTicks    = gTotalTicks;
    out->byterate      = gByterate;
    out->bufferedReady = v.bufferedReady;
    out->numSlots      = v.numSlots;
}
