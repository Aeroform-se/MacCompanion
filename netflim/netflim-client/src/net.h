#ifndef NETFLIM_NET_H
#define NETFLIM_NET_H

#include <Types.h>
#include <MacTCP.h>

#define DEFAULT_HTTP_PORT      6800
#define DEFAULT_DISCOVERY_PORT 6801
#define DEFAULT_VOD_PORT       6804

#define NF_MAX_VIDEOS          48
#define NF_POSTER_W            128
#define NF_POSTER_H            86
#define NF_POSTER_ROWBYTES     16
#define NF_POSTER_BYTES        1376

#define NF_MAX_FRAME_BYTES     28672
#define NF_DEFAULT_SLOTS       20
#define NF_LOWMEM_SLOTS        6

/* NFVM / NFVS UDP VOD protocol constants */
#define NF_MAGIC_CLIENT        0x4E46564DUL  /* 'NFVM' */
#define NF_MAGIC_SERVER        0x4E465653UL  /* 'NFVS' */
#define NF_PKT_OPEN            0x01
#define NF_PKT_WIN_ACK         0x02
#define NF_PKT_SEEK            0x03
#define NF_PKT_CLOSE           0x04
#define NF_PKT_OPEN_ACK        0x81
#define NF_PKT_CHUNK           0x82
#define NF_PKT_EOS             0x83

#define NF_OPEN_OK             0
#define NF_OPEN_NOT_FOUND      1
#define NF_OPEN_NO_PROFILE     2
#define NF_OPEN_ERROR          3

typedef enum {
    kProfileAuto = 0,
    kProfileSE30 = 1,
    kProfileSE   = 2,
    kProfilePlus = 3
} NetFlimProfileMode;

typedef struct {
    char          id[18];
    char          title[48];
    unsigned long durationSec;
    char          profilesCsv[24];
    unsigned long sizeBytes;
    Boolean       hasPoster;
    unsigned long frameCount;
    Boolean       silent;
} NetFlimVideoItem;

typedef struct {
    Boolean        active;
    Boolean        ackReceived;
    unsigned char  openStatus;
    Boolean        eosReached;
    Boolean        silent;
    Boolean        paused;
    unsigned short width;
    unsigned short height;
    unsigned short byterate;
    unsigned long  frameCount;
    unsigned long  totalTicks;
    unsigned long  nextNeededFrame;
    short          numSlots;
    short          bufferedReady;
} NetVodInfo;

/* Core networking & preferences */
Boolean NetInit(void);
void    NetShutdown(void);
void    FormatIPAddress(ip_addr addr, char *outBuf);
Boolean ParseIPAddress(const char *str, ip_addr *outAddr);

ip_addr        NetGetServerIP(void);
unsigned short NetGetServerPort(void);
ip_addr        NetGetOurIP(void);
Boolean        NetHasServer(void);
void           NetSetServer(ip_addr ip, unsigned short httpPort);

NetFlimProfileMode NetGetProfileMode(void);
void               NetSetProfileMode(NetFlimProfileMode mode);
const char        *NetResolveProfileName(void);
Boolean            NetIsHighSpeedCPU(void);

void        NetSavePrefs(void);
Boolean     NetProbeServer(ip_addr ip, unsigned short port, char *outStatus, int maxLen);
Boolean     NetDiscoverServer(char *outModules, int maxLen);
Boolean     NetAutoConnect(char *outStatus, int statusMax);
Boolean     NetReadSiblingServerIP(ip_addr *outIp, unsigned short *outPort, char *outSource, int srcMax);
const char *NetGetLastErrorNote(void);
const char *NetGetPrefsSourceName(void);
Boolean     NetHasOpenTransport(void);
Boolean     NetIsUsingNativeOT(void);

/* HTTP Catalog & Posters */
Boolean NetHttpGet(const char *path, char *outBuf, long outMax, long *outLen, short *outHttpStatus);
short   NetFetchCatalog(NetFlimVideoItem *items, short maxItems, char *outErr, int errMax);
Boolean NetFetchPosterRaw(const char *videoId, unsigned char *out1376);

/* Framed TCP VOD Streaming Session (port 6800) */
Boolean NetVodOpen(const char *videoId, const char *profile, Boolean stripAudio, Boolean halfAudio, unsigned long startFrame);
void    NetVodClose(void);
void    NetVodPump(void);
void    NetVodSeek(unsigned long targetFrame);
void    NetVodSetPaused(Boolean paused);
Boolean NetVodNeedsFastPump(void);

/* Fixed 32-byte power-of-2 pre-parsed frame descriptor shared with .Sound ISR */
typedef struct {
    unsigned long          frameIdx;     /*  0..3  */
    const unsigned char   *ffSynthPtr;   /*  4..7  */
    const unsigned char   *videoPtr;     /*  8..11 */
    unsigned long          recordBytes;  /* 12..15 */
    unsigned short         ticks;        /* 16..17 */
    unsigned short         ffReqBytes;   /* 18..19 */
    unsigned short         videoBytes;   /* 20..21 */
    unsigned short         pcmBytes;     /* 22..23 */
    unsigned char          codecId;      /* 24 */
    volatile unsigned char ready;        /* 25 */
    unsigned char          flags;        /* 26 */
    unsigned char          pad[5];       /* 27..31 */
} NetVodFrameDesc;

/* Pre-parsed descriptor lookup & reclaim watermark for Sound Manager and interrupt-driven .Sound */
const NetVodFrameDesc *NetVodPeekNextSlot(void);
void                   NetVodConsumeNextFrame(void);
const NetVodFrameDesc *NetVodIrqPeekSlot(unsigned long frameIdx);
void                   NetVodIrqSetCompletedUpTo(unsigned long completedUpTo);
void                   NetVodGetInfo(NetVodInfo *outInfo);
short                  NetVodGetBufferedReady(void);

/* Direct CPU-specific hot entry points selected via Gestalt */
void                   NetVodPump_68k(void);
void                   NetVodPump_020(void);
void                   NetVodConsumeNextFrame_68k(void);
void                   NetVodConsumeNextFrame_020(void);
const NetVodFrameDesc *NetVodIrqPeekSlot_68k(unsigned long frameIdx);
const NetVodFrameDesc *NetVodIrqPeekSlot_020(unsigned long frameIdx);
const NetVodFrameDesc *NetVodPeekNextSlot_68k(void);
const NetVodFrameDesc *NetVodPeekNextSlot_020(void);

#endif /* NETFLIM_NET_H */
