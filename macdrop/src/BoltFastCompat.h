#ifndef BOLT_FAST_COMPAT_H
#define BOLT_FAST_COMPAT_H

#include <MacTypes.h>
#include <MacTCP.h>
#include <stdint.h>

#include "BoltTcp.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BoltFastCompat - Drop-in hybrid MacTCP + BoltTcp FastAPI client helper.
 *
 * Usage model:
 *   1. Open ".IPP" and connect your StreamPtr as usual using standard MacTCP
 *      (TCPCreate + TCPActiveOpen / TCPPassiveOpen) so ARP, DNS/DNR, and
 *      connection setup work identically on BoltTcp, Apple MacTCP, and OT.
 *   2. Call BoltDrop_Probe(ctx, ippRefNum) once at app startup.
 *   3. Before entering a bulk transfer loop, call BoltDrop_BeginTransfer().
 *   4. Transfer data via BoltDrop_RecvChunk() / BoltDrop_SendAll() (or
 *      non-blocking BoltDrop_BeginSend() + BoltDrop_PollSend()).
 *   5. Before WaitNextEvent, dialogs, or TCPClose/TCPAbort/TCPRelease,
 *      call BoltDrop_EndTransfer() (or BoltDrop_YieldTickless() around yields).
 */

typedef struct BoltDropContext {
    short       ippRefNum;
    Boolean     hasBoltFast;
    Boolean     has68020Plus;  /* Hardware unaligned longword reads safe */
    BoltFastAPI api;
} BoltDropContext;

typedef struct BoltDropStream {
    BoltDropContext *ctx;
    StreamPtr        stream;
    Boolean          ticklessActive;
    Boolean          eofReached;
    uint32_t         lastRxTick;  /* Used to gate fallback TCPStatus checks */
    TCPiopb          rxPB;        /* Used for async MacTCP fallback */
    Boolean          rxPending;
} BoltDropStream;

typedef struct BoltDropSendJob {
    BoltDropStream *ds;
    const uint8_t  *buf;
    uint32_t        len;
    uint32_t        off;
    Boolean         pushEnd;
    Boolean         done;
    OSErr           err;
    uint32_t        lastStateTick;
    TCPiopb         pbs[2];
    wdsEntry        wds[2][2];
    Boolean         pending[2];
} BoltDropSendJob;

/* Probe ".IPP" for BoltTcp FastAPI support and CPU capabilities. */
Boolean BoltDrop_Probe(BoltDropContext *ctx, short ippRefNum);

/* Suggest safe ping-pong chunk size (16 KB on 68000 / DP8390 / .ENET, 32 KB on 040/SONIC). */
uint32_t BoltDrop_RecommendedBufSize(const BoltDropContext *ctx);

/* Attach an already-open StreamPtr and enable FastAPI flags if available. */
void BoltDrop_BeginTransfer(BoltDropStream *ds,
                            BoltDropContext *ctx,
                            StreamPtr stream,
                            Boolean enableTickless,
                            Boolean skipRxChecksum);

/* Suspend tickless mode safely before UI dialogs, WaitNextEvent, or close. */
void BoltDrop_YieldTickless(BoltDropStream *ds, Boolean resumeAfter);
void BoltDrop_EndTransfer(BoltDropStream *ds);

/* Pump FastAPI link/timer state (no-op when FastAPI is unavailable). */
void BoltDrop_Poll(BoltDropStream *ds);

/*
 * Fast 68k block copy: uses 4x unrolled longword moves on 68020+ (even when
 * `src` is an odd HTTP/ring offset) to avoid Retro68 newlib's move.b fallback.
 */
void BoltDrop_FastCopy(void *dst, const void *src, uint32_t len, Boolean has68020Plus);

/*
 * Non-blocking send state machine (supports cross-buffer overlap with async
 * disk reads even in MacTCP fallback mode).
 * Caller must keep `buf` valid until `BoltDrop_PollSend` sets `*outDone = true`.
 */
void BoltDrop_BeginSend(BoltDropSendJob *job,
                        BoltDropStream *ds,
                        const uint8_t *buf,
                        uint32_t len,
                        Boolean pushEnd);
OSErr BoltDrop_PollSend(BoltDropSendJob *job, Boolean *outDone);
void BoltDrop_WaitSend(BoltDropSendJob *job);

/* Synchronous convenience wrapper around BeginSend/PollSend with idle hook. */
typedef Boolean (*BoltDropIdleProc)(void *userCtx);

OSErr BoltDrop_SendAll(BoltDropStream *ds,
                       const uint8_t *buf,
                       uint32_t len,
                       Boolean pushEnd,
                       BoltDropIdleProc idleHook,
                       void *userCtx);

/*
 * Copy up to `bufCap` available bytes into `outBuf` (non-blocking).
 * - FastAPI path: reads via RecvZeroCopy(), copies with BoltDrop_FastCopy(),
 *   and IMMEDIATELY calls RecvRelease() before returning. Uses zero-trap
 *   GetStreamState() when idle to detect peer FIN (or 15-tick gated TCPStatus).
 * - MacTCP fallback path: drives a 32 KB async TCPRcv PB cleanly.
 */
OSErr BoltDrop_RecvChunk(BoltDropStream *ds,
                         uint8_t *outBuf,
                         uint32_t bufCap,
                         uint32_t *outGotBytes,
                         Boolean *outEof);

#ifdef __cplusplus
}
#endif

#endif /* BOLT_FAST_COMPAT_H */
