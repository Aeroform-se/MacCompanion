#include "net_internal.h"
#include <string.h>

typedef char NetVodFrameDescSizeCheck[(sizeof(NetVodFrameDesc) == 32) ? 1 : -1];

#ifdef NETFLIM_HOT_020
#define ReapIrqCompletedFrames  ReapIrqCompletedFrames_020
#define ScanArenaFrames         ScanArenaFrames_020
#define OtCopyChainToArena      OtCopyChainToArena_020
#define OtDrainIncoming         OtDrainIncoming_020
#define OtVodPump               OtVodPump_020
#define NetVodPump              NetVodPump_020
#define NetVodConsumeNextFrame  NetVodConsumeNextFrame_020
#define NetVodIrqPeekSlot       NetVodIrqPeekSlot_020
#define NetVodPeekNextSlot      NetVodPeekNextSlot_020
#define FastCopyHot             NetFlimFastCopy_020
#define ParseSliceExHot         NetFlimParseFrameSliceEx_020
#else
#define ReapIrqCompletedFrames  ReapIrqCompletedFrames_68k
#define ScanArenaFrames         ScanArenaFrames_68k
#define OtCopyChainToArena      OtCopyChainToArena_68k
#define OtDrainIncoming         OtDrainIncoming_68k
#define OtVodPump               OtVodPump_68k
#define NetVodPump              NetVodPump_68k
#define NetVodConsumeNextFrame  NetVodConsumeNextFrame_68k
#define NetVodIrqPeekSlot       NetVodIrqPeekSlot_68k
#define NetVodPeekNextSlot      NetVodPeekNextSlot_68k
#define FastCopyHot             NetFlimFastCopy_68k
#define ParseSliceExHot         NetFlimParseFrameSliceEx_68k
#endif

static void ReapIrqCompletedFrames(void) {
    unsigned long a, b, doneUpTo;
    if (!gVodActive || gRingNumSlots <= 0) return;

    do {
        a = gIrqCompletedUpTo;
        b = gIrqCompletedUpTo;
    } while (a != b);
    doneUpTo = a;

    while (gReclaimCursor < doneUpTo) {
        NetVodFrameDesc *d = &gDescRing[gReclaimCursor & NF_INDEX_MASK];
        if (d->ready && d->frameIdx == gReclaimCursor) {
            if (gArenaUsed >= d->recordBytes) gArenaUsed -= d->recordBytes;
            else gArenaUsed = 0;
            d->ready = 0;
            if (gBufferedCount > 0) gBufferedCount--;
        }
        gReclaimCursor++;
        if (gNextNeededFrame < gReclaimCursor) {
            gNextNeededFrame = gReclaimCursor;
        }
    }
}

void ScanArenaFrames(void) {
    while (gArenaAvailToParse >= 8UL && gBufferedCount < gRingNumSlots) {
        const unsigned char *hdr;
        unsigned long frameIdx;
        unsigned short payloadLen;
        unsigned short padWord;
        unsigned short prePad;
        unsigned short postPad;
        unsigned char  midFlag;
        unsigned long recordBytes;

        if (gArenaParse + 8UL > gArenaCap) {
            unsigned long hWrap = (gArenaParse + 8UL) - gArenaCap;
            FastCopyHot(gArenaMem + gArenaCap, gArenaMem, hWrap);
        }

        hdr        = gArenaMem + gArenaParse;
        frameIdx   = net_read_u32be(hdr);
        payloadLen = net_read_u16be(hdr + 4);
        padWord    = net_read_u16be(hdr + 6);
        prePad     = (unsigned short)((padWord >> 8) & 3U);
        postPad    = (unsigned short)(padWord & 3U);
        midFlag    = (padWord & 0x0400U) ? NETFLIM_FRAME_FLAG_MIDPAD1 : 0;

        if (payloadLen == 0xFFFFU) {
            gVodEosReached     = true;
            gVodFrameCount     = frameIdx;
            gTcpRemoteEof      = true;
            gArenaAvailToParse = 0;
            break;
        }

        recordBytes = 8UL + (unsigned long)prePad + (unsigned long)payloadLen + (unsigned long)postPad;
        if (gArenaAvailToParse < recordBytes) {
            /* Full frame payload has not arrived in the arena yet */
            break;
        }

        if (frameIdx >= gReclaimCursor + (unsigned long)gRingNumSlots) {
            /* Descriptor ring index full or frame audio still pinned; wait for consumer to advance */
            break;
        }

        if (gArenaParse + recordBytes > gArenaCap) {
            unsigned long overflow = (gArenaParse + recordBytes) - gArenaCap;
            if (overflow <= NF_ARENA_TAIL) {
                FastCopyHot(gArenaMem + gArenaCap, gArenaMem, overflow);
            }
        }

        if (frameIdx < gNextNeededFrame || payloadLen > NF_MAX_FRAME_BYTES) {
            if (gArenaUsed >= recordBytes) gArenaUsed -= recordBytes;
            else gArenaUsed = 0;
        } else {
            const unsigned char *frameData = gArenaMem + gArenaParse + 8UL + (unsigned long)prePad;
            NetVodFrameDesc *d = &gDescRing[frameIdx & NF_INDEX_MASK];
            NetFlimParsedFrame parsed;

            d->frameIdx    = frameIdx;
            d->recordBytes = recordBytes;
            d->flags       = midFlag;
            /* Pre-parse audio/video offsets once on the main loop so IRQ never calls ParseFrameSliceEx */
            if (ParseSliceExHot(frameData, payloadLen, midFlag, &parsed)) {
                d->ticks      = parsed.ticks;
                d->ffSynthPtr = parsed.ffSynthPtr;
                d->ffReqBytes = (parsed.ffSynthPtr != NULL && parsed.pcmBytes > 0)
                    ? (unsigned short)(6U + parsed.pcmBytes)
                    : 0U;
                d->pcmBytes   = parsed.pcmBytes;
                d->videoPtr   = parsed.videoPtr;
                d->videoBytes = parsed.videoBytes;
                d->codecId    = parsed.codecId;
            } else {
                d->ticks      = 2;
                d->ffSynthPtr = NULL;
                d->ffReqBytes = 0;
                d->pcmBytes   = 0;
                d->videoPtr   = NULL;
                d->videoBytes = 0;
                d->codecId    = NETFLIM_CODEC_NULL;
            }
            __asm__ volatile ("" ::: "memory");
            d->ready = 1;
            gBufferedCount++;
        }

        gArenaParse += recordBytes;
        if (gArenaParse >= gArenaCap) {
            gArenaParse -= gArenaCap;
        }
        gArenaAvailToParse -= recordBytes;
    }
}

static unsigned long OtCopyChainToArena(void) {
    unsigned long freeBytes = (gArenaCap > gArenaUsed) ? (gArenaCap - gArenaUsed) : 0UL;
    unsigned long totalCopied = 0UL;

    while (gOtPendingCur != NULL && freeBytes > 0UL) {
        unsigned long fragLen = (unsigned long)gOtPendingCur->fLen;
        unsigned long fragRem;
        unsigned long take;

        if (gOtPendingOff >= fragLen) {
            gOtPendingCur = gOtPendingCur->fNext;
            gOtPendingOff = 0UL;
            continue;
        }

        fragRem = fragLen - gOtPendingOff;
        take = (fragRem < freeBytes) ? fragRem : freeBytes;

        if (take > 0UL) {
            const unsigned char *src = (const unsigned char *)gOtPendingCur->fData + gOtPendingOff;
            unsigned long toEnd = gArenaCap - gArenaWrite;

            if (take <= toEnd) {
                FastCopyHot(gArenaMem + gArenaWrite, src, take);
                gArenaWrite += take;
                if (gArenaWrite >= gArenaCap) {
                    gArenaWrite = 0UL;
                }
            } else {
                FastCopyHot(gArenaMem + gArenaWrite, src, toEnd);
                FastCopyHot(gArenaMem, src + toEnd, take - toEnd);
                gArenaWrite = take - toEnd;
            }

            gArenaUsed         += take;
            gArenaAvailToParse += take;
            gOtPendingOff      += take;
            freeBytes          -= take;
            totalCopied        += take;
        }

        if (gOtPendingOff >= fragLen) {
            gOtPendingCur = gOtPendingCur->fNext;
            gOtPendingOff = 0UL;
        }
    }

    if (gOtPendingCur == NULL && gOtPendingHead != NULL) {
        OTReleaseBuffer(gOtPendingHead);
        gOtPendingHead = NULL;
        gOtPendingCur  = NULL;
        gOtPendingOff  = 0UL;
    }

    return totalCopied;
}

void OtDrainIncoming(void) {
    int budget = 16;
    Boolean didParseData = false;

    if (gOtVodEp == kOTInvalidEndpointRef || gTcpConnState != kVodTcpStreaming || !gArenaMem || gArenaCap == 0) {
        return;
    }

    if (gOtPendingHead != NULL) {
        if (gParsePhase != kParseFrames) {
            OTReleaseBuffer(gOtPendingHead);
            gOtPendingHead = NULL;
            gOtPendingCur  = NULL;
            gOtPendingOff  = 0UL;
        } else {
            if (OtCopyChainToArena() > 0UL) {
                didParseData = true;
            }
            if (gOtPendingHead != NULL) {
                if (didParseData && gArenaAvailToParse >= 8UL && gBufferedCount < gRingNumSlots) {
                    ScanArenaFrames();
                }
                gOtVodPausedForRoom = true;
                return;
            }
        }
    }

    while (budget-- > 0 && !gTcpRemoteEof && !gVodEosReached) {
        OTFlags rxFlags = 0;
        OTResult n;

        if (gParsePhase != kParseFrames) {
            unsigned long toEnd = (gArenaCap > gArenaWrite) ? (gArenaCap - gArenaWrite) : 0UL;
            unsigned long chunk = (toEnd > 4096UL) ? 4096UL : toEnd;
            if (chunk == 0UL) {
                gOtVodPausedForRoom = true;
                break;
            }
            gOtVodHasData = false;
            n = OTRcv(gOtVodEp, gArenaMem + gArenaWrite, (OTByteCount)chunk, &rxFlags);
            if (n > 0) {
                gOtVodHasData = true;
                gOtVodPausedForRoom = false;
                gArenaWrite += (unsigned long)n;
                NetInternalParseInitialHandshake();
                continue;
            }
        } else {
            OTBuffer *rxBuf = NULL;
            if (!NetInternalRingCanQueueMore()) {
                gOtVodPausedForRoom = true;
                break;
            }
            gOtVodHasData = false;
            n = OTRcv(gOtVodEp, &rxBuf, kOTNetbufDataIsOTBufferStar, &rxFlags);
            if (n >= 0 && rxBuf != NULL) {
                gOtVodHasData = true;
                gOtVodPausedForRoom = false;
                gOtPendingHead = rxBuf;
                gOtPendingCur  = rxBuf;
                gOtPendingOff  = 0UL;
                if (OtCopyChainToArena() > 0UL) {
                    didParseData = true;
                }
                if (gOtPendingHead != NULL) {
                    gOtVodPausedForRoom = true;
                    break;
                }
                continue;
            } else if (n == 0) {
                gOtVodPausedForRoom = false;
                break;
            }
        }

        if (n == kOTNoDataErr) {
            gOtVodPausedForRoom = false;
            if (gOtVodNeedLook) {
                OTResult look;
                gOtVodNeedLook = false;
                look = OTLook(gOtVodEp);
                if (look == T_ORDREL) {
                    OTRcvOrderlyDisconnect(gOtVodEp);
                    gTcpRemoteEof = true;
                } else if (look == T_DISCONNECT) {
                    OTRcvDisconnect(gOtVodEp, NULL);
                    gTcpRemoteEof = true;
                    if (!gVodAckReceived) {
                        gVodOpenStatus = NF_OPEN_ERROR;
                    }
                }
            }
            break;
        } else if (n == kOTLookErr) {
            OTResult look;
            gOtVodNeedLook = false;
            look = OTLook(gOtVodEp);
            if (look == T_ORDREL) {
                OTRcvOrderlyDisconnect(gOtVodEp);
                gTcpRemoteEof = true;
                break;
            } else if (look == T_DISCONNECT) {
                OTRcvDisconnect(gOtVodEp, NULL);
                gTcpRemoteEof = true;
                if (!gVodAckReceived) {
                    gVodOpenStatus = NF_OPEN_ERROR;
                }
                break;
            } else {
                gOtVodHasData = true;
                continue;
            }
        } else {
            gLastTcpErr = (OSErr)n;
            gTcpRemoteEof = true;
            if (!gVodAckReceived) {
                gVodOpenStatus = NF_OPEN_ERROR;
            }
            break;
        }
    }

    /* Scan all newly arrived frames once per drain batch rather than after every OTRcv packet */
    if (didParseData && gParsePhase == kParseFrames &&
        gArenaAvailToParse >= 8UL && gBufferedCount < gRingNumSlots) {
        ScanArenaFrames();
    }

    if (gOtVodNeedLook && !gTcpRemoteEof) {
        OTResult look;
        gOtVodNeedLook = false;
        look = OTLook(gOtVodEp);
        if (look == T_ORDREL) {
            if (NetInternalRingCanQueueMore()) {
                OTRcvOrderlyDisconnect(gOtVodEp);
                gTcpRemoteEof = true;
            } else {
                /* Wait to consume orderly release until queued T_DATA has drained cleanly */
                gOtVodNeedLook = true;
            }
        } else if (look == T_DISCONNECT) {
            OTRcvDisconnect(gOtVodEp, NULL);
            gTcpRemoteEof = true;
            if (!gVodAckReceived) {
                gVodOpenStatus = NF_OPEN_ERROR;
            }
        }
    }

    if (gTcpRemoteEof && gVodAckReceived && !gVodEosReached) {
        if (gArenaAvailToParse < 8UL && gOtPendingHead == NULL) {
            gVodEosReached = true;
        }
    }
    if (gTcpRemoteEof && !gVodAckReceived && gVodOpenStatus == NF_OPEN_OK) {
        gVodOpenStatus = NF_OPEN_ERROR;
        gVodEosReached = true;
    }
}

static void OtVodPump(void) {
    OSStatus err;

    if (gTcpConnState == kVodTcpOpening) {
        EndpointRef ep;
        if (!gOtVodOpenDone) return;

        ep = (EndpointRef)gOtVodOpenEp;
        gOtVodOpenEp = kOTInvalidEndpointRef;
        if (gOtVodOpenErr != noErr || ep == kOTInvalidEndpointRef) {
            if (ep != kOTInvalidEndpointRef) {
                NetInternalOtCloseEndpointSync(ep);
            }
            NetInternalOtVodFailClosed((OSErr)(gOtVodOpenErr ? gOtVodOpenErr : -23016));
            return;
        }
        gOtVodEp = ep;

        OTSetSynchronous(ep);
        err = OTBind(ep, NULL, NULL);
        if (err != noErr) {
            NetInternalOtCloseVodStream();
            NetInternalOtVodFailClosed((OSErr)err);
            return;
        }
        NetInternalOtSetFourByteOption(ep, INET_TCP, TCP_NODELAY, T_YES);
        NetInternalOtSetFourByteOption(ep, XTI_GENERIC, XTI_RCVBUF, TCP_VOD_CREATE_MAX);

        OTSetAsynchronous(ep);
        OTSetNonBlocking(ep);

        OTInitInetAddress(&gOtVodCallAddr, gServerHttpPort, gServerIP);
        memset(&gOtVodCall, 0, sizeof(gOtVodCall));
        gOtVodCall.addr.buf = (UInt8 *)&gOtVodCallAddr;
        gOtVodCall.addr.len = sizeof(gOtVodCallAddr);

        gOtVodConnectDone = false;
        err = OTConnect(ep, &gOtVodCall, NULL);
        if (err == noErr) {
            gTcpConnState = kVodTcpSending;
        } else if (err == kOTNoDataErr) {
            gTcpConnState = kVodTcpConnecting;
            return;
        } else if (err == kOTLookErr) {
            OTResult look = OTLook(ep);
            if (look == T_CONNECT && OTRcvConnect(ep, NULL) == noErr) {
                gTcpConnState = kVodTcpSending;
            } else {
                if (look == T_DISCONNECT) {
                    OTRcvDisconnect(ep, NULL);
                }
                NetInternalOtCloseVodStream();
                NetInternalOtVodFailClosed(connectionTerminated);
                return;
            }
        } else {
            NetInternalOtCloseVodStream();
            NetInternalOtVodFailClosed((OSErr)err);
            return;
        }
    }

    if (gTcpConnState == kVodTcpConnecting) {
        OTResult look;
        if (gOtVodEp == kOTInvalidEndpointRef) return;
        if (!gOtVodConnectDone) {
            look = OTLook(gOtVodEp);
            if (look != T_CONNECT && look != T_DISCONNECT) {
                return;
            }
            gOtVodConnectDone = true;
        }
        err = OTRcvConnect(gOtVodEp, NULL);
        if (err == noErr) {
            gTcpConnState = kVodTcpSending;
        } else if (err == kOTNoDataErr) {
            gOtVodConnectDone = false;
            return;
        } else {
            if (err == kOTLookErr && OTLook(gOtVodEp) == T_DISCONNECT) {
                OTRcvDisconnect(gOtVodEp, NULL);
            }
            NetInternalOtCloseVodStream();
            NetInternalOtVodFailClosed((OSErr)err);
            return;
        }
    }

    if (gTcpConnState == kVodTcpSending) {
        if (gOtVodEp == kOTInvalidEndpointRef) return;
        while (gOtVodSendOff < gOtVodSendLen) {
            OTResult n = OTSnd(gOtVodEp,
                               gVodHttpReq + gOtVodSendOff,
                               (OTByteCount)(gOtVodSendLen - gOtVodSendOff),
                               0);
            if (n > 0) {
                gOtVodSendOff = (unsigned short)(gOtVodSendOff + (unsigned short)n);
            } else if (n == kOTFlowErr || n == kOTNoDataErr) {
                gOtVodGoData = false;
                return;
            } else if (n == kOTLookErr) {
                OTResult look = OTLook(gOtVodEp);
                if (look == T_DISCONNECT) {
                    OTRcvDisconnect(gOtVodEp, NULL);
                    NetInternalOtCloseVodStream();
                    NetInternalOtVodFailClosed(connectionTerminated);
                    return;
                } else if (look == T_GODATA) {
                    gOtVodGoData = false;
                } else {
                    break;
                }
            } else {
                NetInternalOtCloseVodStream();
                NetInternalOtVodFailClosed((OSErr)n);
                return;
            }
        }
        gTcpConnState = kVodTcpStreaming;
        gOtVodHasData = true;
    }

    if (gTcpConnState != kVodTcpStreaming) return;

    ReapIrqCompletedFrames();

    if (gOtPendingHead != NULL || gOtVodHasData || gOtVodNeedLook ||
        (gOtVodPausedForRoom && NetInternalRingCanQueueMore())) {
        OtDrainIncoming();
    }

    if (gParsePhase == kParseFrames && gArenaAvailToParse >= 8UL && gBufferedCount < gRingNumSlots) {
        ScanArenaFrames();
    }
}

void NetVodPump(void) {
    int budget = 8;
    Boolean didParseFrames = false;

    if (!gVodActive) return;

    if (gUseNativeOT) {
        OtVodPump();
        return;
    }

    if (gVodTcpStream == 0) return;

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
        NetInternalArmTcpRecv();
    }

    if (gTcpConnState != kVodTcpStreaming) return;

    ReapIrqCompletedFrames();

    if (!gTcpRxArmed[0] && !gTcpRxArmed[1] && !gTcpRemoteEof && !gVodEosReached && NetInternalRingCanQueueMore()) {
        NetInternalArmTcpRecvSlot((short)(gTcpRxActiveSlot ^ 1));
    }

    while (budget-- > 0) {
        short cur = gTcpRxActiveSlot;
        if (gTcpRxArmed[cur] && gVodRxPB[cur].ioResult != 1) {
            OSErr res = gVodRxPB[cur].ioResult;
            unsigned short got = (res == noErr || res == connectionClosing)
                ? gVodRxPB[cur].csParam.receive.rcvBuffLen
                : 0;
            short nextSlot = (short)(cur ^ 1);
            gTcpRxArmed[cur] = false;

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
                    NetInternalParseInitialHandshake();
                    if (!gTcpRemoteEof && (res == noErr || res == -23016) && NetInternalRingCanQueueMore()) {
                        NetInternalArmTcpRecvSlot(nextSlot);
                    }
                } else {
                    gArenaWrite += (unsigned long)got;
                    if (gArenaWrite >= gArenaCap) {
                        gArenaWrite -= gArenaCap;
                    }
                    gArenaUsed         += (unsigned long)got;
                    gArenaAvailToParse += (unsigned long)got;
                    didParseFrames      = true;

                    if (!gTcpRemoteEof && (res == noErr || res == -23016) && NetInternalRingCanQueueMore()) {
                        NetInternalArmTcpRecvSlot(nextSlot);
                    }
                }
            } else if (!gTcpRemoteEof && (res == noErr || res == -23016) && NetInternalRingCanQueueMore()) {
                NetInternalArmTcpRecvSlot(nextSlot);
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

    if ((didParseFrames || gArenaAvailToParse >= 8UL) &&
        gParsePhase == kParseFrames && gBufferedCount < gRingNumSlots) {
        ScanArenaFrames();
    }

    if (!gTcpRxArmed[0] && !gTcpRxArmed[1] && !gTcpRemoteEof && !gVodEosReached && NetInternalRingCanQueueMore()) {
        NetInternalArmTcpRecvSlot((short)(gTcpRxActiveSlot ^ 1));
    }
}

const NetVodFrameDesc *NetVodPeekNextSlot(void) {
    const NetVodFrameDesc *d;
    if (!gVodActive || !gVodAckReceived || gRingNumSlots <= 0) return NULL;
    if (gVodFrameCount > 0 && gNextNeededFrame >= gVodFrameCount) return NULL;

    d = &gDescRing[gNextNeededFrame & NF_INDEX_MASK];
    if (!d->ready || d->frameIdx != gNextNeededFrame) {
        return NULL;
    }
    return d;
}

void NetVodConsumeNextFrame(void) {
    NetVodFrameDesc *d;
    if (!gVodActive || gRingNumSlots <= 0) return;

    d = &gDescRing[gNextNeededFrame & NF_INDEX_MASK];
    if (d->ready && d->frameIdx == gNextNeededFrame) {
        if (gArenaUsed >= d->recordBytes) gArenaUsed -= d->recordBytes;
        else gArenaUsed = 0;
        d->ready = 0;
        if (gBufferedCount > 0) gBufferedCount--;
    }
    gNextNeededFrame++;
    if (gReclaimCursor < gNextNeededFrame) {
        gReclaimCursor = gNextNeededFrame;
    }
    if (gIrqCompletedUpTo < gNextNeededFrame) {
        gIrqCompletedUpTo = gNextNeededFrame;
    }

    if (gParsePhase == kParseFrames && gArenaAvailToParse >= 8UL && gBufferedCount < gRingNumSlots) {
        ScanArenaFrames();
    }
    if (gUseNativeOT) {
        if ((gOtPendingHead != NULL || gOtVodHasData || gOtVodNeedLook || gOtVodPausedForRoom) &&
            !gTcpRemoteEof && !gVodEosReached && NetInternalRingCanQueueMore()) {
            OtDrainIncoming();
        }
    } else if (!gTcpRxArmed[0] && !gTcpRxArmed[1] && !gTcpRemoteEof && !gVodEosReached && NetInternalRingCanQueueMore()) {
        NetInternalArmTcpRecv();
    }
}

const NetVodFrameDesc *NetVodIrqPeekSlot(unsigned long frameIdx) {
    const NetVodFrameDesc *d;
    if (!gVodActive || !gVodAckReceived || gRingNumSlots <= 0) return NULL;
    if (gVodFrameCount > 0 && frameIdx >= gVodFrameCount) return NULL;

    d = &gDescRing[frameIdx & NF_INDEX_MASK];
    if (!d->ready || d->frameIdx != frameIdx) {
        return NULL;
    }
    return d;
}
