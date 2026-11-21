#include "BoltFastCompat.h"

#include <Devices.h>
#include <Events.h>
#include <Gestalt.h>
#include <string.h>
#ifdef __m68k__
#include <LowMem.h>
#define FastTicksNow() ((uint32_t)LMGetTicks())
#else
#define FastTicksNow() ((uint32_t)TickCount())
#endif

#define kIdleStatusGateTicks 15UL /* 250 ms minimum gap between fallback TCPStatus traps */

static Boolean IsStateFinDrained(uint8_t st) {
    return (st != BOLT_TCP_STATE_ESTABLISHED &&
            st != BOLT_TCP_STATE_FIN_WAIT1 &&
            st != BOLT_TCP_STATE_FIN_WAIT2);
}

static Boolean IsRemoteFinDrainedSync(short ippRef, StreamPtr stream) {
    TCPiopb pb;
    memset(&pb, 0, sizeof(pb));
    pb.ioCRefNum = ippRef;
    pb.tcpStream = stream;
    pb.csCode = TCPStatus;
    if (PBControlSync((ParmBlkPtr)&pb) != noErr) {
        return true;
    }
    if (pb.csParam.status.amtUnreadData > 0) {
        return false;
    }
    return IsStateFinDrained((uint8_t)pb.csParam.status.connectionState);
}

void BoltDrop_FastCopy(void *dst, const void *src, uint32_t len, Boolean has68020Plus) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (len == 0) return;

    /* On 68000/68010, odd relative parity cannot use word/long reads */
    if (len < 16 || (!has68020Plus && ((((uintptr_t)d ^ (uintptr_t)s) & 1U) != 0))) {
        memcpy(d, s, len);
        return;
    }

    /* Align destination to 4 bytes (on 68020+, unaligned src reads are native) */
    while (((uintptr_t)d & 3U) != 0 && len > 0) {
        *d++ = *s++;
        len--;
    }

    {
#ifdef __m68k__
        uint32_t chunks32 = len >> 5; /* 32 bytes per iteration */
        if (chunks32 > 0 && chunks32 <= 65536UL) {
            uint16_t dbraCnt = (uint16_t)(chunks32 - 1UL);
            __asm__ volatile (
                "1:\n\t"
                "move.l (%0)+,(%1)+\n\t"
                "move.l (%0)+,(%1)+\n\t"
                "move.l (%0)+,(%1)+\n\t"
                "move.l (%0)+,(%1)+\n\t"
                "move.l (%0)+,(%1)+\n\t"
                "move.l (%0)+,(%1)+\n\t"
                "move.l (%0)+,(%1)+\n\t"
                "move.l (%0)+,(%1)+\n\t"
                "dbra   %2,1b\n"
                : "+a"(s), "+a"(d), "+d"(dbraCnt)
                :
                : "memory", "cc"
            );
        } else {
            while (chunks32--) {
                __asm__ volatile (
                    "move.l (%0)+,(%1)+\n\t"
                    "move.l (%0)+,(%1)+\n\t"
                    "move.l (%0)+,(%1)+\n\t"
                    "move.l (%0)+,(%1)+\n\t"
                    "move.l (%0)+,(%1)+\n\t"
                    "move.l (%0)+,(%1)+\n\t"
                    "move.l (%0)+,(%1)+\n\t"
                    "move.l (%0)+,(%1)+\n\t"
                    : "+a"(s), "+a"(d)
                    :
                    : "memory"
                );
            }
        }
        len &= 31U;
        while (len >= 4U) {
            __asm__ volatile ("move.l (%0)+,(%1)+\n\t" : "+a"(s), "+a"(d) : : "memory");
            len -= 4U;
        }
#else
        uint32_t *dl = (uint32_t *)d;
        const uint32_t *sl = (const uint32_t *)s;
        uint32_t blocks = len >> 4; /* 16 bytes per iteration */
        while (blocks-- > 0) {
            dl[0] = sl[0];
            dl[1] = sl[1];
            dl[2] = sl[2];
            dl[3] = sl[3];
            dl += 4;
            sl += 4;
        }
        len &= 15U;
        while (len >= 4U) {
            *dl++ = *sl++;
            len -= 4U;
        }
        d = (uint8_t *)dl;
        s = (const uint8_t *)sl;
#endif
    }

    while (len-- > 0) {
        *d++ = *s++;
    }
}

Boolean BoltDrop_Probe(BoltDropContext *ctx, short ippRefNum) {
    BoltControlPB bpb;
    long cpuType = 0;

    memset(ctx, 0, sizeof(*ctx));
    ctx->ippRefNum = ippRefNum;

    if (Gestalt(gestaltProcessorType, &cpuType) == noErr && cpuType >= gestalt68020) {
        ctx->has68020Plus = true;
    }

    if (ippRefNum == 0) {
        return false;
    }

    memset(&bpb, 0, sizeof(bpb));
    bpb.ioCRefNum = ippRefNum;
    bpb.csCode = kBoltGetCapabilities;
    bpb.p.caps.apiOut = &ctx->api;

    if (PBControlSync((ParmBlkPtr)&bpb) == noErr &&
        ctx->api.magic == BOLT_MAGIC &&
        ctx->api.Poll != nil &&
        ctx->api.RecvZeroCopy != nil &&
        ctx->api.RecvRelease != nil &&
        ctx->api.SendZeroCopy != nil &&
        ctx->api.SetSocketFlags != nil) {
        ctx->hasBoltFast = true;
        return true;
    }

    memset(&ctx->api, 0, sizeof(ctx->api));
    ctx->hasBoltFast = false;
    return false;
}

uint32_t BoltDrop_RecommendedBufSize(const BoltDropContext *ctx) {
    long cpuType = 0;
    if (!ctx || !ctx->has68020Plus) {
        return 16384UL;
    }
    if (ctx->hasBoltFast && (ctx->api.link_tier == BOLT_TIER_SONIC || ctx->api.version >= 0x0031)) {
        return 32768UL;
    }
    if (Gestalt(gestaltProcessorType, &cpuType) == noErr && cpuType >= gestalt68040) {
        return 32768UL;
    }
    /* Keep individual 53C80 SCSI writes <= 16 KB on legacy MacTCP/OT / pre-0.1.33 DP8390 */
    return 16384UL;
}

void BoltDrop_BeginTransfer(BoltDropStream *ds,
                            BoltDropContext *ctx,
                            StreamPtr stream,
                            Boolean enableTickless,
                            Boolean skipRxChecksum) {
    uint8_t flags = 0;

    memset(ds, 0, sizeof(*ds));
    ds->ctx = ctx;
    ds->stream = stream;
    ds->lastRxTick = FastTicksNow();

    if (ctx && ctx->hasBoltFast && stream) {
        if (enableTickless) flags |= BOLT_MODE_TICKLESS;
        if (skipRxChecksum) flags |= BOLT_FLAG_SKIP_RX_CSUM;
        ctx->api.SetSocketFlags(stream, flags);
        ds->ticklessActive = enableTickless;
    }
}

void BoltDrop_YieldTickless(BoltDropStream *ds, Boolean resumeAfter) {
    if (!ds || !ds->ctx || !ds->ctx->hasBoltFast || !ds->stream) {
        return;
    }
    if (!resumeAfter) {
        if (ds->ticklessActive) {
            ds->ctx->api.SetSocketFlags(ds->stream, BOLT_FLAG_SKIP_RX_CSUM);
            ds->ticklessActive = false;
        }
    } else {
        if (!ds->ticklessActive) {
            ds->ctx->api.SetSocketFlags(ds->stream,
                                        BOLT_MODE_TICKLESS | BOLT_FLAG_SKIP_RX_CSUM);
            ds->ticklessActive = true;
        }
    }
}

void BoltDrop_EndTransfer(BoltDropStream *ds) {
    if (!ds) return;
    if (ds->ctx && ds->ctx->hasBoltFast && ds->stream) {
        ds->ctx->api.SetSocketFlags(ds->stream, 0);
        ds->ticklessActive = false;
    }
}

void BoltDrop_Poll(BoltDropStream *ds) {
    if (ds && ds->ctx && ds->ctx->hasBoltFast) {
        ds->ctx->api.Poll();
    }
}

void BoltDrop_BeginSend(BoltDropSendJob *job,
                        BoltDropStream *ds,
                        const uint8_t *buf,
                        uint32_t len,
                        Boolean pushEnd) {
    memset(job, 0, sizeof(*job));
    job->ds = ds;
    job->buf = buf;
    job->len = len;
    job->pushEnd = pushEnd;
    job->lastStateTick = FastTicksNow();
    job->done = (len == 0);
}

OSErr BoltDrop_PollSend(BoltDropSendJob *job, Boolean *outDone) {
    BoltDropStream *ds = job->ds;

    if (outDone) *outDone = job->done;
    if (job->done) return job->err;
    if (!ds || !ds->ctx || !ds->stream) {
        job->done = true;
        job->err = paramErr;
        if (outDone) *outDone = true;
        return paramErr;
    }

    if (ds->ctx->hasBoltFast) {
        ds->ctx->api.Poll();
        while (job->off < job->len) {
            uint32_t rem = job->len - job->off;
            uint16_t chunk = (rem > 16384UL) ? 16384U : (uint16_t)rem;
            uint8_t flags = (job->pushEnd && (job->off + chunk >= job->len)) ? BOLT_FLAG_NODELAY : 0;
            OSErr err = ds->ctx->api.SendZeroCopy(ds->stream, job->buf + job->off, chunk, flags);
            if (err == noErr) {
                job->off += chunk;
                continue;
            }
            if (err != insufficientResources) {
                job->done = true;
                job->err = err;
                if (outDone) *outDone = true;
                return err;
            }
            /* Check peer close without Device Manager trap when GetStreamState exists */
            if (ds->ctx->api.GetStreamState) {
                if (IsStateFinDrained(ds->ctx->api.GetStreamState(ds->stream))) {
                    job->done = true;
                    job->err = connectionClosing;
                    if (outDone) *outDone = true;
                    return connectionClosing;
                }
            } else if ((uint32_t)(FastTicksNow() - job->lastStateTick) >= kIdleStatusGateTicks) {
                job->lastStateTick = FastTicksNow();
                if (IsRemoteFinDrainedSync(ds->ctx->ippRefNum, ds->stream)) {
                    job->done = true;
                    job->err = connectionClosing;
                    if (outDone) *outDone = true;
                    return connectionClosing;
                }
            }
            return noErr;
        }
        job->done = true;
        job->err = noErr;
        if (outDone) *outDone = true;
        return noErr;
    }

    /* MacTCP fallback: 2 pipelined 16 KB async TCPSend PBs */
    {
        enum { kFallbackPipe = 2, kChunkCap = 16384U };
        int i;

        for (i = 0; i < kFallbackPipe; i++) {
            if (job->pending[i] && job->pbs[i].ioResult <= 0) {
                OSErr r = job->pbs[i].ioResult;
                job->pending[i] = false;
                if (r != noErr) {
                    int other = 1 - i;
                    while (job->pending[other] && job->pbs[other].ioResult > 0) {}
                    job->pending[other] = false;
                    job->done = true;
                    job->err = r;
                    if (outDone) *outDone = true;
                    return r;
                }
            }
            if (!job->pending[i] && job->off < job->len) {
                uint32_t rem = job->len - job->off;
                uint16_t chunk = (rem > kChunkCap) ? (uint16_t)kChunkCap : (uint16_t)rem;

                job->wds[i][0].length = chunk;
                job->wds[i][0].ptr = (Ptr)(job->buf + job->off);
                job->wds[i][1].length = 0;
                job->wds[i][1].ptr = nil;

                memset(&job->pbs[i], 0, sizeof(TCPiopb));
                job->pbs[i].ioCRefNum = ds->ctx->ippRefNum;
                job->pbs[i].tcpStream = ds->stream;
                job->pbs[i].csCode = TCPSend;
                job->pbs[i].csParam.send.pushFlag = (job->pushEnd && (job->off + chunk >= job->len));
                job->pbs[i].csParam.send.wdsPtr = (Ptr)job->wds[i];
                PBControlAsync((ParmBlkPtr)&job->pbs[i]);
                job->pending[i] = true;
                job->off += chunk;
            }
        }

        if (job->off >= job->len && !job->pending[0] && !job->pending[1]) {
            job->done = true;
            job->err = noErr;
            if (outDone) *outDone = true;
        }
    }
    return noErr;
}

void BoltDrop_WaitSend(BoltDropSendJob *job) {
    Boolean done = job->done;
    while (!done) {
        if (BoltDrop_PollSend(job, &done) != noErr) {
            break;
        }
    }
}

OSErr BoltDrop_SendAll(BoltDropStream *ds,
                       const uint8_t *buf,
                       uint32_t len,
                       Boolean pushEnd,
                       BoltDropIdleProc idleHook,
                       void *userCtx) {
    BoltDropSendJob job;
    Boolean done = false;
    OSErr err;

    BoltDrop_BeginSend(&job, ds, buf, len, pushEnd);
    while (!done) {
        err = BoltDrop_PollSend(&job, &done);
        if (err != noErr) {
            return err;
        }
        if (!done && idleHook && !idleHook(userCtx)) {
            int i;
            for (i = 0; i < 2; i++) {
                while (job.pending[i] && job.pbs[i].ioResult > 0) {}
            }
            return userCanceledErr;
        }
    }
    return job.err;
}

OSErr BoltDrop_RecvChunk(BoltDropStream *ds,
                         uint8_t *outBuf,
                         uint32_t bufCap,
                         uint32_t *outGotBytes,
                         Boolean *outEof) {
    if (outGotBytes) *outGotBytes = 0;
    if (outEof) *outEof = false;
    if (!ds || !ds->ctx || !ds->stream || !outBuf || bufCap == 0) return paramErr;

    if (ds->ctx->hasBoltFast) {
        uint32_t total = 0;
        Boolean has68020 = ds->ctx->has68020Plus;

        ds->ctx->api.Poll();
        while (total < bufCap) {
            const uint8_t *dmaPtr = nil;
            uint16_t avail = ds->ctx->api.RecvZeroCopy(ds->stream, &dmaPtr);
            uint32_t room;
            uint16_t take;

            if (avail == 0 || !dmaPtr) {
                break;
            }
            room = bufCap - total;
            take = (avail > room) ? (uint16_t)room : avail;
            BoltDrop_FastCopy(outBuf + total, dmaPtr, take, has68020);

            /*
             * Release IMMEDIATELY so BoltTcp's ring advances and opens the TCP
             * receive window right away (before any disk write).
             */
            ds->ctx->api.RecvRelease(ds->stream, take);
            total += take;
        }

        if (outGotBytes) *outGotBytes = total;
        if (total > 0) {
            ds->lastRxTick = FastTicksNow();
            return noErr;
        }

        /* Zero-trap state/EOF check when available, otherwise gated at 15 ticks (250 ms) */
        if (ds->ctx->api.GetStreamState) {
            uint8_t st = ds->ctx->api.GetStreamState(ds->stream);
            if (IsStateFinDrained(st)) {
                const uint8_t *recheck = nil;
                if (ds->ctx->api.RecvZeroCopy(ds->stream, &recheck) == 0) {
                    ds->eofReached = true;
                    if (outEof) *outEof = true;
                }
            }
        } else if ((uint32_t)(FastTicksNow() - ds->lastRxTick) >= kIdleStatusGateTicks) {
            ds->lastRxTick = FastTicksNow();
            if (IsRemoteFinDrainedSync(ds->ctx->ippRefNum, ds->stream)) {
                ds->eofReached = true;
                if (outEof) *outEof = true;
            }
        }
        return noErr;
    }

    /* MacTCP fallback path (32 KB chunks via async TCPRcv) */
    if (!ds->rxPending) {
        uint16_t reqLen = (bufCap > 32768UL) ? 32768U : (uint16_t)bufCap;
        memset(&ds->rxPB, 0, sizeof(TCPiopb));
        ds->rxPB.ioCRefNum = ds->ctx->ippRefNum;
        ds->rxPB.tcpStream = ds->stream;
        ds->rxPB.csCode = TCPRcv;
        ds->rxPB.csParam.receive.rcvBuff = (Ptr)outBuf;
        ds->rxPB.csParam.receive.rcvBuffLen = reqLen;
        ds->rxPB.csParam.receive.commandTimeoutValue = 5;
        PBControlAsync((ParmBlkPtr)&ds->rxPB);
        ds->rxPending = true;
    }

    if (ds->rxPB.ioResult <= 0) {
        OSErr r = ds->rxPB.ioResult;
        uint16_t got = ds->rxPB.csParam.receive.rcvBuffLen;
        ds->rxPending = false;
        if (r == noErr) {
            ds->lastRxTick = FastTicksNow();
            if (outGotBytes) *outGotBytes = got;
            return noErr;
        }
        if (r == connectionClosing) {
            ds->eofReached = true;
            if (outEof) *outEof = true;
            return noErr;
        }
        return r;
    }

    return noErr;
}
