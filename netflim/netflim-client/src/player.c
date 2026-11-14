#include "player.h"
#include "codec.h"

#include <Memory.h>
#include <OSUtils.h>
#include <Quickdraw.h>
#include <Sound.h>
#include <stdio.h>
#include <string.h>

#define SND_NUM_BUFS          10
#define SND_BUF_SIZE          4096
#define SND_FLUSH_THRESHOLD   2960  /* ~8 ticks (133ms) low-overhead audio batching */
#define PREBUFFER_FRAMES      8
#define OPEN_TIMEOUT_TICKS    240   /* 4 seconds */

/* ============================================================================
 * Globals
 * ============================================================================ */

static Boolean         gPlayerInited     = false;
static PlayerState     gState            = kPlayerStopped;
static Boolean         gFullscreen       = false;
static Boolean         gMuted            = false;
static Boolean         gLooping          = false;
static Boolean         gCanDirectVRAM    = false;
static Boolean         gUsingDirectVRAM  = false;

static unsigned char  *gOffscreenFB      = NULL;
static BitMap          gOffscreenBM;
static WindowPtr       gFsWindow         = NULL;

/* Sound Manager state (22,254.5 Hz 8-bit unsigned mono PCM) */
static SndChannelPtr   gSndChan          = NULL;
static SndCallBackUPP  gSndCallBackUPP   = NULL;
static unsigned char  *gSndBufs[SND_NUM_BUFS];
static SoundHeader     gSndHdrs[SND_NUM_BUFS];
static volatile short  gSndBufBusy[SND_NUM_BUFS];
static short           gSndWriteSlot     = -1;
static unsigned short  gSndWritePos      = 0;
static short           gSndNextSlot      = 0;

/* Active playback metadata & clock */
static char            gVideoId[18];
static char            gVideoTitle[48];
static char            gActiveProfile[10];
static char            gStatusMsg[96]    = "Ready";
static unsigned long   gStartReqTick     = 0;
static unsigned long   gCurrentFrame     = 0;
static unsigned long   gTotalFrames      = 0;
static unsigned long   gElapsedTicks     = 0;
static unsigned long   gTotalTicks       = 0;
static unsigned long   gNextFrameDueTick = 0;
static unsigned short  gByterate         = 0;
static Boolean         gVodStreamSilent  = false;
static short           gLastHudSecond    = -1;
static short           gLastHudBuf       = -1;

/* ============================================================================
 * Sound Manager Interrupt Callback & Single-Copy Audio Pipeline
 * ============================================================================ */

static pascal void SoundBufferCallback(SndChannelPtr chan, SndCommand *cmd) {
    (void)chan;
    if (cmd && cmd->param2) {
        *(volatile short *)cmd->param2 = 0;
    }
}

static short CountBusySoundBuffers(void) {
    short i, busy = 0;
    for (i = 0; i < SND_NUM_BUFS; i++) {
        if (gSndBufBusy[i]) busy++;
    }
    return busy;
}

static void FlushAudioChannel(void) {
    SndCommand cmd;
    short i;

    if (gSndChan) {
        cmd.cmd    = flushCmd;
        cmd.param1 = 0;
        cmd.param2 = 0;
        SndDoImmediate(gSndChan, &cmd);
        cmd.cmd    = quietCmd;
        SndDoImmediate(gSndChan, &cmd);
    }

    for (i = 0; i < SND_NUM_BUFS; i++) {
        gSndBufBusy[i] = 0;
    }
    gSndWriteSlot = -1;
    gSndWritePos  = 0;
    gSndNextSlot  = 0;
}

static void EnsureSoundChannelOpen(void) {
    OSErr err;
    if (gSndChan != NULL) return;
    if (!gSndCallBackUPP) {
        gSndCallBackUPP = NewSndCallBackProc(SoundBufferCallback);
    }
    err = SndNewChannel(&gSndChan, sampledSynth, initMono, gSndCallBackUPP);
    if (err != noErr) {
        gSndChan = NULL;
    }
}

static void CloseSoundChannel(void) {
    FlushAudioChannel();
    if (gSndChan) {
        SndDisposeChannel(gSndChan, true);
        gSndChan = NULL;
    }
}

static short AcquireAudioWriteSlot(void) {
    short tries;
    if (gSndWriteSlot >= 0) return gSndWriteSlot;

    for (tries = 0; tries < SND_NUM_BUFS; tries++) {
        short cand = (gSndNextSlot + tries) % SND_NUM_BUFS;
        if (!gSndBufBusy[cand] && gSndBufs[cand] != NULL) {
            gSndWriteSlot = cand;
            gSndWritePos  = 0;
            gSndNextSlot  = (cand + 1) % SND_NUM_BUFS;
            return cand;
        }
    }
    return -1;
}

static void DispatchCurrentAudioSlot(void) {
    short slot = gSndWriteSlot;
    SndCommand cmd;

    if (slot < 0 || gSndWritePos == 0 || !gSndChan || gMuted || gVodStreamSilent) {
        gSndWriteSlot = -1;
        gSndWritePos  = 0;
        return;
    }

    gSndHdrs[slot].samplePtr = (Ptr)gSndBufs[slot];
    gSndHdrs[slot].length    = gSndWritePos;
    gSndBufBusy[slot]        = 1;

    gSndWriteSlot = -1;
    gSndWritePos  = 0;

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

    if (gMuted || gVodStreamSilent || !pcm || pcmBytes == 0 || !gSndChan) return;

    while (offset < pcmBytes) {
        short slot = AcquireAudioWriteSlot();
        unsigned short avail;
        unsigned short take;

        if (slot < 0) return;

        avail = (unsigned short)(SND_BUF_SIZE - gSndWritePos);
        take  = (unsigned short)(pcmBytes - offset);
        if (take > avail) take = avail;

        NetFlimFastCopy(gSndBufs[slot] + gSndWritePos, pcm + offset, take);
        gSndWritePos = (unsigned short)(gSndWritePos + take);
        offset       = (unsigned short)(offset + take);

        if ((CountBusySoundBuffers() == 0 && gSndWritePos >= 1110) ||
            gSndWritePos >= SND_FLUSH_THRESHOLD ||
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

static void EnterFullscreenVisual(void) {
    Rect scr = qd.screenBits.bounds;

    if (gCanDirectVRAM) {
        /* 1:1 Compact Mac (SE / SE/30 512x342 monochrome CRT, rowBytes=64): zero CopyBits overhead! */
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
    }
}

static void ExitFullscreenVisual(WindowPtr mainWin) {
    if (gUsingDirectVRAM) {
        /* Save current CRT frame back into offscreen buffer before restoring desktop */
        NetFlimFastCopy(gOffscreenFB, (const void *)qd.screenBits.baseAddr, NETFLIM_FB_BYTES);
        gUsingDirectVRAM = false;
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

Boolean PlayerInit(void) {
    short i;
    SysEnvRec env;

    if (gPlayerInited) return true;

    gOffscreenFB = (unsigned char *)NewPtrClear(NETFLIM_FB_BYTES);
    if (!gOffscreenFB) return false;
    NetFlimResetFramebuffer(gOffscreenFB);

    gOffscreenBM.baseAddr = (Ptr)gOffscreenFB;
    gOffscreenBM.rowBytes = NETFLIM_ROW_BYTES;
    SetRect(&gOffscreenBM.bounds, 0, 0, NETFLIM_WIDTH, NETFLIM_HEIGHT);

    for (i = 0; i < SND_NUM_BUFS; i++) {
        gSndBufs[i] = (unsigned char *)NewPtrClear(SND_BUF_SIZE);
        gSndBufBusy[i] = 0;
        memset(&gSndHdrs[i], 0, sizeof(SoundHeader));
        gSndHdrs[i].samplePtr     = (Ptr)gSndBufs[i];
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

    gPlayerInited = true;
    return true;
}

void PlayerShutdown(void) {
    short i;
    PlayerStop(NULL);
    CloseSoundChannel();

    if (gSndCallBackUPP) {
        DisposeRoutineDescriptor((UniversalProcPtr)gSndCallBackUPP);
        gSndCallBackUPP = NULL;
    }
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
    if (!gMuted) {
        EnsureSoundChannelOpen();
    }

    if (!NetVodOpen(gVideoId, gActiveProfile, gMuted, 0)) {
        gState = kPlayerError;
        strcpy(gStatusMsg, "Failed to open UDP stream (port 6804)");
        return false;
    }

    gState            = kPlayerBuffering;
    gCurrentFrame     = 0;
    gTotalFrames      = item->frameCount;
    gElapsedTicks     = 0;
    gTotalTicks       = item->durationSec * 60UL;
    gByterate         = 0;
    gVodStreamSilent  = gMuted || item->silent;
    gStartReqTick     = TickCount();
    gNextFrameDueTick = TickCount();
    gLastHudSecond    = -1;
    gLastHudBuf       = -1;
    sprintf(gStatusMsg, "Buffering %s (%s)...", gVideoTitle, gActiveProfile);

    if (startFullscreen) {
        gFullscreen = true;
        EnterFullscreenVisual();
    } else {
        gFullscreen = false;
    }

    return true;
}

void PlayerStop(WindowPtr mainWin) {
    Boolean wasFs = gFullscreen;

    FlushAudioChannel();
    NetVodClose();
    gState = kPlayerStopped;
    gFullscreen = false;

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
        gState = kPlayerPaused;
        FlushAudioChannel();
        NetVodSetPaused(true);
        strcpy(gStatusMsg, "Paused");
    } else if (gState == kPlayerPaused) {
        if (!gMuted && !gVodStreamSilent) {
            EnsureSoundChannelOpen();
        }
        gState = kPlayerPlaying;
        gNextFrameDueTick = TickCount();
        NetVodSetPaused(false);
        sprintf(gStatusMsg, "Playing %s (%s)", gVideoTitle, gActiveProfile);
    }
}

void PlayerToggleLoop(void) {
    gLooping = !gLooping;
}

void PlayerToggleMute(void) {
    gMuted = !gMuted;
    if (gMuted) {
        FlushAudioChannel();
    } else if (gState == kPlayerPlaying || gState == kPlayerPaused) {
        EnsureSoundChannelOpen();
    }

    /* Re-open active VOD stream at current frame so server toggles audio stripping */
    if (gState == kPlayerPlaying || gState == kPlayerPaused || gState == kPlayerBuffering) {
        Boolean wasPaused = (gState == kPlayerPaused);
        unsigned long resumeFrame = gCurrentFrame;
        if (NetVodOpen(gVideoId, gActiveProfile, gMuted, resumeFrame)) {
            if (wasPaused) NetVodSetPaused(true);
            gNextFrameDueTick = TickCount();
        }
    }
}

void PlayerSetFullscreen(Boolean fullscreen, WindowPtr mainWin) {
    if (gState == kPlayerStopped || gState == kPlayerError) {
        gFullscreen = false;
        return;
    }
    if (fullscreen == gFullscreen) return;

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
 * Main Streaming Pump & Frame Scheduler
 * ============================================================================ */

Boolean PlayerTick(WindowPtr mainWin, const Rect *viewportRect, Boolean *outUiChanged) {
    NetVodInfo info;
    unsigned long now;
    const unsigned char *frameData;
    unsigned short frameBytes = 0;
    unsigned long frameIdx = 0;

    if (outUiChanged) *outUiChanged = false;
    if (gState == kPlayerStopped || gState == kPlayerError) {
        return false;
    }

    NetVodPump();
    NetVodGetInfo(&info);

    if (gState == kPlayerPaused) {
        return false;
    }

    now = TickCount();

    if (info.openStatus != NF_OPEN_OK) {
        NetVodClose();
        CloseSoundChannel();
        if (gFullscreen) {
            gFullscreen = false;
            ExitFullscreenVisual(mainWin);
        }
        gState = kPlayerError;
        if (info.openStatus == NF_OPEN_NO_PROFILE) {
            strcpy(gStatusMsg, "No encoded profile found on server");
        } else {
            strcpy(gStatusMsg, "Video stream unavailable on server");
        }
        if (outUiChanged) *outUiChanged = true;
        return false;
    }

    if (gState == kPlayerBuffering) {
        short minPrebuf = (info.numSlots > 32) ? 10 : 6;
        short halfRing = (info.numSlots > 2) ? (short)(info.numSlots / 2) : info.numSlots;
        if (!info.ackReceived) {
            if ((now - gStartReqTick) > OPEN_TIMEOUT_TICKS) {
                NetVodClose();
                if (gFullscreen) {
                    gFullscreen = false;
                    ExitFullscreenVisual(mainWin);
                }
                gState = kPlayerError;
                strcpy(gStatusMsg, "TCP stream timeout (check server / port 6800)");
                if (outUiChanged) *outUiChanged = true;
                return false;
            }
            return true;
        }

        gTotalFrames = info.frameCount;
        gTotalTicks = info.totalTicks;
        gByterate = info.byterate;
        gVodStreamSilent = info.silent;

        if (halfRing < minPrebuf) minPrebuf = halfRing;
        if (info.frameCount > 0 && (long)info.frameCount < (long)minPrebuf) {
            minPrebuf = (short)info.frameCount;
        }

        if (info.bufferedReady >= minPrebuf || (info.eosReached && info.bufferedReady > 0)) {
            gState = kPlayerPlaying;
            gNextFrameDueTick = now;
            sprintf(gStatusMsg, "Playing %s (%s)", gVideoTitle, gActiveProfile);
            if (outUiChanged) *outUiChanged = true;
        } else if (info.eosReached && info.bufferedReady == 0) {
            PlayerStop(mainWin);
            if (outUiChanged) *outUiChanged = true;
            return false;
        } else {
            return true;
        }
    }

    /* Check end-of-stream completion */
    if (gTotalFrames > 0 && info.nextNeededFrame >= gTotalFrames) {
        if (gLooping) {
            NetFlimResetFramebuffer(gOffscreenFB);
            if (gUsingDirectVRAM) {
                NetFlimFastCopy((void *)qd.screenBits.baseAddr, gOffscreenFB, NETFLIM_FB_BYTES);
            }
            FlushAudioChannel();
            gCurrentFrame = 0;
            gElapsedTicks = 0;
            gNextFrameDueTick = now;
            NetVodSeek(0);
            if (outUiChanged) *outUiChanged = true;
            return true;
        } else {
            PlayerStop(mainWin);
            if (outUiChanged) *outUiChanged = true;
            return false;
        }
    }

    /* Decode all VBL-due frames (up to 4) and blit the latest frame once */
    {
        Boolean needBlit = false;
        short steps = 0;

        while (now >= gNextFrameDueTick && steps < 4) {
            frameData = NetVodPeekNextFrame(&frameBytes, &frameIdx);
            if (frameData != NULL) {
                NetFlimParsedFrame parsed;
                if (NetFlimParseFrameSlice(frameData, frameBytes, &parsed)) {
                    unsigned char *targetFB = gUsingDirectVRAM
                        ? (unsigned char *)qd.screenBits.baseAddr
                        : gOffscreenFB;

                    if (parsed.videoBytes >= 4 && parsed.codecId != NETFLIM_CODEC_NULL) {
                        int decRes = NetFlimDecodeVideo(targetFB, parsed.videoPtr, parsed.videoBytes);
                        if (decRes == 2 || (decRes > 0 && gUsingDirectVRAM)) {
                            needBlit = true;
                        }
                    }

                    if (parsed.pcmBytes > 0 && !gMuted && !gVodStreamSilent) {
                        FeedFrameAudio(parsed.pcmPtr, parsed.pcmBytes);
                    }

                    gCurrentFrame = frameIdx + 1;
                    gElapsedTicks += parsed.ticks;
                    gNextFrameDueTick += (unsigned long)parsed.ticks;

                    if (!gFullscreen) {
                        short sec = (short)(gElapsedTicks / 60UL);
                        if (sec != gLastHudSecond) {
                            gLastHudSecond = sec;
                            gLastHudBuf    = info.bufferedReady;
                            if (outUiChanged) *outUiChanged = true;
                        }
                    }
                }
                NetVodConsumeNextFrame();
                steps++;
                if (gTotalFrames > 0 && gCurrentFrame >= gTotalFrames) {
                    break;
                }
            } else {
                if (gSndWriteSlot >= 0 && gSndWritePos > 0 && CountBusySoundBuffers() == 0) {
                    DispatchCurrentAudioSlot();
                }
                if (!gFullscreen && gLastHudBuf != info.bufferedReady) {
                    gLastHudBuf = info.bufferedReady;
                    if (outUiChanged) *outUiChanged = true;
                }
                /* Minor underrun: wait briefly before shifting schedule clock */
                if ((long)(now - gNextFrameDueTick) > 6L) {
                    gNextFrameDueTick = now;
                }
                break;
            }
        }

        if ((long)(now - gNextFrameDueTick) > 12L) {
            gNextFrameDueTick = now;
        }

        if (needBlit) {
            BlitCurrentFrame(mainWin, viewportRect);
            NetVodPump();
        }
    }

    return true;
}

Boolean PlayerWantsZeroSleep(void) {
    if (gState == kPlayerBuffering) return true;
    if (gState != kPlayerPlaying) return false;
    if (NetVodNeedsFastPump()) return true;
    if ((long)(gNextFrameDueTick - TickCount()) <= 1L) return true;
    return false;
}

PlayerState PlayerGetState(void)    { return gState; }
Boolean     PlayerIsFullscreen(void){ return gFullscreen; }
Boolean     PlayerIsMuted(void)     { return gMuted; }
Boolean     PlayerIsLooping(void)   { return gLooping; }

void PlayerGetSnapshot(PlayerSnapshot *out) {
    NetVodInfo v;
    if (!out) return;
    NetVodGetInfo(&v);
    out->state         = gState;
    out->fullscreen    = gFullscreen;
    out->muted         = gMuted;
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
