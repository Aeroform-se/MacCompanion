#ifndef NETFLIM_PLAYER_INTERNAL_H
#define NETFLIM_PLAYER_INTERNAL_H

#include "player.h"
#include "codec.h"

#include <Devices.h>
#include <OSUtils.h>
#include <Quickdraw.h>
#include <Sound.h>

#ifdef __m68k__
static inline unsigned long PlayerFastTicks(void) {
    unsigned long t;
    __asm__ volatile ("move.l 0x016A:w,%0" : "=r"(t));
    return t;
}
#define FAST_TICKS() PlayerFastTicks()
#else
#define FAST_TICKS() TickCount()
#endif

#define SND_DRIVER_REFNUM     (-4)  /* Classic Mac ".Sound" driver */
#define SND_FF_HDR_BYTES      6     /* int16 mode=ffMode(0) + Fixed rate */
#define SND_FF_RATE_22K       0x00010000UL
#define SND_FF_RATE_11K       0x00008000UL
#define SND_NUM_BUFS          8     /* Power-of-2 Sound Manager ring (0..7) */
#define SND_BUF_MASK          (SND_NUM_BUFS - 1)
#define SND_BUF_SIZE          8192
#define SND_FLUSH_THRESHOLD   3700  /* 10 ticks (166ms) @ 22.25 kHz */
#define SND_PRIME_SILENCE     1480  /* 4 ticks (66ms) @ 22.25 kHz */
#define SND_SILENCE_MAX_BYTES (SND_FF_HDR_BYTES + (8 * 370))
#define OPEN_TIMEOUT_TICKS    240   /* 4 seconds */

typedef struct {
    unsigned long magic;
    unsigned long savedA5;
    ParamBlockRec pb;
} DirectSoundPB;

#define DIRECT_SND_PB_MAGIC 0x4E464135UL /* 'NFA5' */

/* Shared player globals owned in player.c */
extern PlayerState             gState;
extern Boolean                 gFullscreen;
extern Boolean                 gMuted;
extern Boolean                 gHalfAudio;
extern Boolean                 gLooping;
extern Boolean                 gCanDirectVRAM;
extern Boolean                 gUsingDirectVRAM;
extern Boolean                 gUsingWindowVRAM;

extern unsigned char          *gScreenBaseAddr;
extern unsigned char          *gOffscreenFB;

extern Boolean                 gUseDirectSndDrv;
extern DirectSoundPB           gDirectSndPB;
extern unsigned char           gSilenceFFBuf[SND_SILENCE_MAX_BYTES];
extern long                    gSilenceReqByTicks[9];
extern volatile Boolean        gDirectActive;
extern volatile Boolean        gDirectStopReq;
extern volatile Boolean        gDirectFailed;
extern volatile Boolean        gDirectEosReached;
extern volatile Boolean        gDirectNeedsBlit;
extern volatile unsigned long  gDirectCurFrame;
extern volatile unsigned long  gDirectNextFrame;
extern volatile unsigned long  gDirectCallbacks;
extern volatile unsigned long  gDirectLastTick;

extern short                   gSndWriteSlot;
extern unsigned short          gSndWritePos;

extern char                    gVideoTitle[48];
extern char                    gActiveProfile[10];
extern char                    gStatusMsg[96];
extern unsigned long           gStartReqTick;
extern volatile unsigned long  gCurrentFrame;
extern unsigned long           gTotalFrames;
extern volatile unsigned long  gElapsedTicks;
extern unsigned long           gTotalTicks;
extern unsigned long           gNextFrameDueTick;
extern unsigned long           gNextHudTick;
extern unsigned short          gByterate;
extern Boolean                 gVodStreamSilent;
extern short                   gLastHudBuf;

/* Cold helpers implemented in player.c */
long    PlayerInternalCalcSilenceFFBytes(unsigned short ticks);
Boolean PlayerInternalStartDirectAudioAtFrame(unsigned long startIdx, WindowPtr mainWin, const Rect *viewportRect);
void    PlayerInternalFlushAudioChannel(void);
void    PlayerInternalCloseSoundChannel(void);
void    PlayerInternalEnsureSoundChannelOpen(void);
short   PlayerInternalCountBusySoundBuffers(void);
void    PlayerInternalDispatchCurrentAudioSlot(void);
void    PlayerInternalFeedFrameAudio(const unsigned char *pcm, unsigned short pcmBytes);
void    PlayerInternalExitFullscreenVisual(WindowPtr mainWin);
void    PlayerInternalBlitCurrentFrame(WindowPtr mainWin, const Rect *viewportRect);

/* Hot functions instantiated cleanly for 68000 (_68k) and 68020+ (_020) */
void    NetFlimDirectSoundCompletionAsm_68k(void);
void    NetFlimDirectSoundCompletionAsm_020(void);
void    NetFlimDirectSoundCompletionC_68k(void);
void    NetFlimDirectSoundCompletionC_020(void);
Boolean PlayerTick_68k(WindowPtr mainWin, const Rect *viewportRect, Boolean *outUiChanged);
Boolean PlayerTick_020(WindowPtr mainWin, const Rect *viewportRect, Boolean *outUiChanged);

#endif /* NETFLIM_PLAYER_INTERNAL_H */
