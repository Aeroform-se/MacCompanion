#include "player_internal.h"
#include <stdio.h>
#include <string.h>

#ifdef NETFLIM_HOT_020
#define NetFlimDirectSoundCompletionC NetFlimDirectSoundCompletionC_020
#define PlayerTickHot                 PlayerTick_020
#define VodPumpHot                    NetVodPump_020
#define VodIrqPeekSlotHot             NetVodIrqPeekSlot_020
#define VodPeekNextSlotHot            NetVodPeekNextSlot_020
#define VodConsumeNextFrameHot        NetVodConsumeNextFrame_020
#define DecodeVideoHot                NetFlimDecodeVideo_020
#define DecodeVideoClippedHot         NetFlimDecodeVideoClipped_020
#define FastCopyHot                   NetFlimFastCopy_020
#define ResetFbHot                    NetFlimResetFramebuffer_020
#else
#define NetFlimDirectSoundCompletionC NetFlimDirectSoundCompletionC_68k
#define PlayerTickHot                 PlayerTick_68k
#define VodPumpHot                    NetVodPump_68k
#define VodIrqPeekSlotHot             NetVodIrqPeekSlot_68k
#define VodPeekNextSlotHot            NetVodPeekNextSlot_68k
#define VodConsumeNextFrameHot        NetVodConsumeNextFrame_68k
#define DecodeVideoHot                NetFlimDecodeVideo_68k
#define DecodeVideoClippedHot         NetFlimDecodeVideoClipped_68k
#define FastCopyHot                   NetFlimFastCopy_68k
#define ResetFbHot                    NetFlimResetFramebuffer_68k
#endif

#ifdef __m68k__
#ifdef NETFLIM_HOT_020
__asm__(
    ".text\n"
    ".even\n"
    ".globl NetFlimDirectSoundCompletionAsm_020\n"
    "NetFlimDirectSoundCompletionAsm_020:\n\t"
    "movem.l %d0-%d2/%a0-%a1/%a5,%sp@-\n\t"
    "move.l %a0,%d0\n\t"
    "beq.s 1f\n\t"
    "cmp.l #0x4E464135,%a0@(-8)\n\t"
    "bne.s 1f\n\t"
    "move.l %a0@(-4),%a5\n\t"
    "bra.s 2f\n"
    "1:\n\t"
    "move.l 0x0904:w,%a5\n"
    "2:\n\t"
    "jsr NetFlimDirectSoundCompletionC_020\n\t"
    "movem.l %sp@+,%d0-%d2/%a0-%a1/%a5\n\t"
    "rts\n"
);
#else
__asm__(
    ".text\n"
    ".even\n"
    ".globl NetFlimDirectSoundCompletionAsm_68k\n"
    "NetFlimDirectSoundCompletionAsm_68k:\n\t"
    "movem.l %d0-%d2/%a0-%a1/%a5,%sp@-\n\t"
    "move.l %a0,%d0\n\t"
    "beq.s 1f\n\t"
    "cmp.l #0x4E464135,%a0@(-8)\n\t"
    "bne.s 1f\n\t"
    "move.l %a0@(-4),%a5\n\t"
    "bra.s 2f\n"
    "1:\n\t"
    "move.l 0x0904:w,%a5\n"
    "2:\n\t"
    "jsr NetFlimDirectSoundCompletionC_68k\n\t"
    "movem.l %sp@+,%d0-%d2/%a0-%a1/%a5\n\t"
    "rts\n"
);
#endif
#else
#ifdef NETFLIM_HOT_020
void NetFlimDirectSoundCompletionAsm_020(void) { NetFlimDirectSoundCompletionC_020(); }
#else
void NetFlimDirectSoundCompletionAsm_68k(void) { NetFlimDirectSoundCompletionC_68k(); }
#endif
#endif

void NetFlimDirectSoundCompletionC(void) {
    OSErr err;
    unsigned long curIdx;
    const NetVodFrameDesc *slot;

    if (gDirectStopReq || gState != kPlayerPlaying) {
        gDirectActive = false;
        return;
    }

    if (gDirectSndPB.pb.ioParam.ioResult != noErr) {
        if (gDirectSndPB.pb.ioParam.ioResult != abortErr) {
            gDirectFailed = true;
        }
        gDirectActive = false;
        return;
    }

    gDirectCallbacks++;
    gDirectLastTick = FAST_TICKS();

    if (gDirectCurFrame != 0xFFFFFFFFUL) {
        NetVodIrqSetCompletedUpTo(gDirectCurFrame + 1UL);
    }

    curIdx = gDirectNextFrame;
    if (gTotalFrames > 0 && curIdx >= gTotalFrames) {
        gDirectEosReached = true;
        gDirectActive     = false;
        return;
    }

    slot = VodIrqPeekSlotHot(curIdx);
    if (slot != NULL) {
        Ptr bufPtr;
        long reqCount;
        unsigned short ticks = slot->ticks;

        if (!gMuted && !gVodStreamSilent && slot->ffReqBytes > 0 && slot->ffSynthPtr != NULL) {
            bufPtr   = (Ptr)slot->ffSynthPtr;
            reqCount = (long)slot->ffReqBytes;
        } else {
            bufPtr   = (Ptr)gSilenceFFBuf;
            reqCount = (ticks <= 8U) ? gSilenceReqByTicks[ticks] : PlayerInternalCalcSilenceFFBytes(ticks);
        }

        /* ioCompletion, ioRefNum, ioPosMode stay pre-initialized from StartDirectAudioAtFrame */
        gDirectSndPB.pb.ioParam.ioResult    = 0;
        gDirectSndPB.pb.ioParam.ioBuffer    = bufPtr;
        gDirectSndPB.pb.ioParam.ioReqCount  = reqCount;
        gDirectSndPB.pb.ioParam.ioPosOffset = 0;

        gDirectCurFrame  = curIdx;
        gDirectNextFrame = curIdx + 1UL;

        err = PBWriteAsync((ParmBlkPtr)&gDirectSndPB.pb);
        if (err != noErr) {
            gDirectCurFrame = 0xFFFFFFFFUL;
            gDirectFailed   = true;
            gDirectActive   = false;
            return;
        }

        if (slot->videoBytes >= 4 && slot->codecId != NETFLIM_CODEC_NULL) {
            if (gUsingDirectVRAM && gScreenBaseAddr) {
                DecodeVideoHot(gScreenBaseAddr, slot->videoPtr, slot->videoBytes);
            } else if (DecodeVideoHot(gOffscreenFB, slot->videoPtr, slot->videoBytes) > 0) {
                gDirectNeedsBlit = true;
            }
        }

        gCurrentFrame  = curIdx + 1UL;
        gElapsedTicks += (unsigned long)ticks;
    } else {
        /* Brief network underrun: hold stream clock with 2 ticks silence and retry curIdx */
        gDirectCurFrame                     = 0xFFFFFFFFUL;
        gDirectSndPB.pb.ioParam.ioResult    = 0;
        gDirectSndPB.pb.ioParam.ioBuffer    = (Ptr)gSilenceFFBuf;
        gDirectSndPB.pb.ioParam.ioReqCount  = gSilenceReqByTicks[2];
        gDirectSndPB.pb.ioParam.ioPosOffset = 0;

        err = PBWriteAsync((ParmBlkPtr)&gDirectSndPB.pb);
        if (err != noErr) {
            gDirectFailed = true;
            gDirectActive = false;
        }
    }
}

Boolean PlayerTickHot(WindowPtr mainWin, const Rect *viewportRect, Boolean *outUiChanged) {
    NetVodInfo info;
    unsigned long now;
    const NetVodFrameDesc *slot;

    if (outUiChanged) *outUiChanged = false;
    if (gState == kPlayerStopped || gState == kPlayerError) {
        return false;
    }

    /*
     * Always service completed TCPRcv / OT chunks first (<1 us check when idle)
     * so networking stays continuously armed and IRQ-finished frames are freed.
     */
    VodPumpHot();

    /* Fast-path active Direct .Sound steady state without copying 44B NetVodInfo */
    if (gState == kPlayerPlaying && gUseDirectSndDrv && gDirectActive &&
        !gDirectEosReached && !gDirectFailed) {
        if (gDirectCallbacks != 0 || (long)(FAST_TICKS() - gDirectLastTick) <= 36L) {
            if (gDirectNeedsBlit) {
                gDirectNeedsBlit = false;
                PlayerInternalBlitCurrentFrame(mainWin, viewportRect);
            }
            if (!gFullscreen && gElapsedTicks >= gNextHudTick) {
                do {
                    gNextHudTick += 300UL;
                } while (gElapsedTicks >= gNextHudTick);
                gLastHudBuf = NetVodGetBufferedReady();
                if (outUiChanged) *outUiChanged = true;
            }
            return true;
        }
    }

    now = FAST_TICKS();
    if (gState == kPlayerPlaying && !gUseDirectSndDrv && (long)(now - gNextFrameDueTick) < 0L) {
        return true;
    }

    NetVodGetInfo(&info);

    if (gState == kPlayerPaused) {
        return false;
    }

    if (info.openStatus != NF_OPEN_OK) {
        NetVodClose();
        PlayerInternalCloseSoundChannel();
        if (gFullscreen) {
            gFullscreen = false;
            PlayerInternalExitFullscreenVisual(mainWin);
        }
        gUsingWindowVRAM = false;
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
        short minPrebuf = (info.numSlots > 64) ? 20 : ((info.numSlots > 32) ? 14 : 6);
        short halfRing = (info.numSlots > 2) ? (short)(info.numSlots / 2) : info.numSlots;
        if (!info.ackReceived) {
            if ((now - gStartReqTick) > OPEN_TIMEOUT_TICKS) {
                NetVodClose();
                if (gFullscreen) {
                    gFullscreen = false;
                    PlayerInternalExitFullscreenVisual(mainWin);
                }
                gUsingWindowVRAM = false;
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
            if (gUseDirectSndDrv) {
                if (!PlayerInternalStartDirectAudioAtFrame(gCurrentFrame, mainWin, viewportRect)) {
                    gUseDirectSndDrv = false;
                    if (!gMuted && !gVodStreamSilent) {
                        PlayerInternalEnsureSoundChannelOpen();
                    }
                } else {
                    return true;
                }
            }
        } else if (info.eosReached && info.bufferedReady == 0) {
            PlayerStop(mainWin);
            if (outUiChanged) *outUiChanged = true;
            return false;
        } else {
            return true;
        }
    }

    /* Direct .Sound (-4) fallback control & EOS/loop/watchdog handling */
    if (gUseDirectSndDrv) {
        if (gDirectEosReached) {
            if (gLooping) {
                PlayerInternalFlushAudioChannel();
                ResetFbHot(gOffscreenFB);
                if (gUsingDirectVRAM && gScreenBaseAddr) {
                    FastCopyHot(gScreenBaseAddr, gOffscreenFB, NETFLIM_FB_BYTES);
                }
                gCurrentFrame     = 0;
                gElapsedTicks     = 0;
                gNextHudTick      = 0UL;
                gNextFrameDueTick = now;
                gState            = kPlayerBuffering;
                gStartReqTick     = now;
                sprintf(gStatusMsg, "Buffering %s (%s)...", gVideoTitle, gActiveProfile);
                NetVodSeek(0);
                if (outUiChanged) *outUiChanged = true;
                return true;
            } else {
                PlayerStop(mainWin);
                if (outUiChanged) *outUiChanged = true;
                return false;
            }
        }

        if (!gDirectActive && !gDirectFailed) {
            if (!PlayerInternalStartDirectAudioAtFrame(gCurrentFrame, mainWin, viewportRect)) {
                gDirectFailed = true;
            }
        }

        /* Watchdog or driver error: fall back transparently to Sound Manager */
        if (gDirectFailed ||
            (gDirectActive && gDirectCallbacks == 0 && (long)(now - gDirectLastTick) > 36L)) {
            unsigned long resumeAt = info.nextNeededFrame;
            PlayerInternalFlushAudioChannel();
            gUseDirectSndDrv = false;
            if (!gMuted && !gVodStreamSilent) {
                PlayerInternalEnsureSoundChannelOpen();
            }
            gCurrentFrame     = resumeAt;
            gNextFrameDueTick = now;
        } else {
            if (gDirectNeedsBlit) {
                gDirectNeedsBlit = false;
                PlayerInternalBlitCurrentFrame(mainWin, viewportRect);
            }
            if (!gFullscreen && gElapsedTicks >= gNextHudTick) {
                do {
                    gNextHudTick += 300UL;
                } while (gElapsedTicks >= gNextHudTick);
                gLastHudBuf = info.bufferedReady;
                if (outUiChanged) *outUiChanged = true;
            }
            return true;
        }
    }

    /* Check end-of-stream completion (Sound Manager fallback path) */
    if (gTotalFrames > 0 && info.nextNeededFrame >= gTotalFrames) {
        if (gLooping) {
            ResetFbHot(gOffscreenFB);
            if (gUsingDirectVRAM) {
                FastCopyHot(gScreenBaseAddr, gOffscreenFB, NETFLIM_FB_BYTES);
            } else if (gUsingWindowVRAM) {
                FastCopyHot(gScreenBaseAddr + (64UL * NETFLIM_ROW_BYTES),
                            gOffscreenFB + (43UL * NETFLIM_ROW_BYTES),
                            256UL * NETFLIM_ROW_BYTES);
            }
            PlayerInternalFlushAudioChannel();
            gCurrentFrame     = 0;
            gElapsedTicks     = 0;
            gNextHudTick      = 0UL;
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

    /* Single-frame VBL pacer (Sound Manager fallback path) */
    if (now >= gNextFrameDueTick) {
        Boolean needBlit = false;

        slot = VodPeekNextSlotHot();
        if (slot != NULL) {
            Boolean canWinVRAM = false;
            if (!gFullscreen && gCanDirectVRAM && mainWin && viewportRect &&
                (viewportRect->right - viewportRect->left) == NETFLIM_WIDTH &&
                (viewportRect->bottom - viewportRect->top) == 256 &&
                mainWin->portBits.bounds.left == 0 &&
                (viewportRect->top - mainWin->portBits.bounds.top) == 64) {
                canWinVRAM = true;
            }
            gUsingWindowVRAM = canWinVRAM;

            if (slot->videoBytes >= 4 && slot->codecId != NETFLIM_CODEC_NULL) {
                int decRes;
                if (gUsingDirectVRAM) {
                    decRes = DecodeVideoHot(gScreenBaseAddr, slot->videoPtr, slot->videoBytes);
                } else if (canWinVRAM) {
                    decRes = DecodeVideoClippedHot(gScreenBaseAddr + (21UL * NETFLIM_ROW_BYTES),
                                                   43,
                                                   299,
                                                   slot->videoPtr,
                                                   slot->videoBytes);
                } else {
                    decRes = DecodeVideoHot(gOffscreenFB, slot->videoPtr, slot->videoBytes);
                }
                if (decRes == 2 || (decRes > 0 && (gUsingDirectVRAM || canWinVRAM))) {
                    needBlit = true;
                }
            }

            if (slot->pcmBytes > 0 && !gMuted && !gVodStreamSilent && slot->ffSynthPtr != NULL) {
                PlayerInternalFeedFrameAudio(slot->ffSynthPtr + SND_FF_HDR_BYTES, slot->pcmBytes);
            }

            gCurrentFrame      = slot->frameIdx + 1UL;
            gElapsedTicks     += (unsigned long)slot->ticks;
            gNextFrameDueTick += (unsigned long)slot->ticks;

            if (!gFullscreen && gElapsedTicks >= gNextHudTick) {
                do {
                    gNextHudTick += 300UL;
                } while (gElapsedTicks >= gNextHudTick);
                gLastHudBuf = info.bufferedReady;
                if (outUiChanged) *outUiChanged = true;
            }
            VodConsumeNextFrameHot();
        } else {
            if (gSndWriteSlot >= 0 && gSndWritePos > 0 && PlayerInternalCountBusySoundBuffers() == 0) {
                PlayerInternalDispatchCurrentAudioSlot();
            }
            /* Prevent schedule clock from accumulating catch-up debt during an underrun */
            gNextFrameDueTick = now;
        }

        now = FAST_TICKS();
        if ((long)(now - gNextFrameDueTick) > 3L) {
            /* Clamp maximum schedule lag to 2 ticks (1 frame) so we never burst-decode */
            gNextFrameDueTick = now - 2UL;
        }

        if (needBlit) {
            PlayerInternalBlitCurrentFrame(mainWin, viewportRect);
        }

        VodPumpHot();
    }

    return true;
}
