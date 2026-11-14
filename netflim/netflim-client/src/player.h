#ifndef NETFLIM_PLAYER_H
#define NETFLIM_PLAYER_H

#include <Types.h>
#include <Windows.h>
#include "net.h"

typedef enum {
    kPlayerStopped = 0,
    kPlayerBuffering,
    kPlayerPlaying,
    kPlayerPaused,
    kPlayerError
} PlayerState;

typedef struct {
    PlayerState   state;
    Boolean       fullscreen;
    Boolean       muted;
    Boolean       looping;
    char          videoId[18];
    char          videoTitle[48];
    char          activeProfile[10];
    char          statusMsg[96];
    unsigned long currentFrame;
    unsigned long totalFrames;
    unsigned long elapsedTicks;
    unsigned long totalTicks;
    unsigned short byterate;
    short         bufferedReady;
    short         numSlots;
} PlayerSnapshot;

Boolean PlayerInit(void);
void    PlayerShutdown(void);

Boolean PlayerStart(const NetFlimVideoItem *item, WindowPtr mainWin, Boolean startFullscreen);
void    PlayerStop(WindowPtr mainWin);
void    PlayerTogglePause(void);
void    PlayerToggleLoop(void);
void    PlayerToggleMute(void);
void    PlayerSetFullscreen(Boolean fullscreen, WindowPtr mainWin);
void    PlayerToggleFullscreen(WindowPtr mainWin);

/* Called frequently in event loop; returns true if playback active (use fast polling) */
Boolean PlayerTick(WindowPtr mainWin, const Rect *viewportRect, Boolean *outUiChanged);
Boolean PlayerWantsZeroSleep(void);

/* Redraws player frame inside windowed viewport on updateEvt */
void    PlayerDrawViewport(WindowPtr mainWin, const Rect *viewportRect);

/* Direct inspection helpers */
PlayerState PlayerGetState(void);
Boolean     PlayerIsFullscreen(void);
Boolean     PlayerIsMuted(void);
Boolean     PlayerIsLooping(void);
void        PlayerGetSnapshot(PlayerSnapshot *out);

#endif /* NETFLIM_PLAYER_H */
