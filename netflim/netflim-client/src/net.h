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

/* HTTP Catalog & Posters */
Boolean NetHttpGet(const char *path, char *outBuf, long outMax, long *outLen, short *outHttpStatus);
short   NetFetchCatalog(NetFlimVideoItem *items, short maxItems, char *outErr, int errMax);
Boolean NetFetchPosterRaw(const char *videoId, unsigned char *out1376);

/* Framed TCP VOD Streaming Session (port 6800) */
Boolean NetVodOpen(const char *videoId, const char *profile, Boolean stripAudio, unsigned long startFrame);
void    NetVodClose(void);
void    NetVodPump(void);
void    NetVodSeek(unsigned long targetFrame);
void    NetVodSetPaused(Boolean paused);
Boolean NetVodNeedsFastPump(void);

/* Returns pointer to complete frame data for gNextNeededFrame if ready, else NULL */
const unsigned char *NetVodPeekNextFrame(unsigned short *outBytes, unsigned long *outFrameIdx);
void                 NetVodConsumeNextFrame(void);
void                 NetVodGetInfo(NetVodInfo *outInfo);

#endif /* NETFLIM_NET_H */
