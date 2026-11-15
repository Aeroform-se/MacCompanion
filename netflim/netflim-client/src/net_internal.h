#ifndef NETFLIM_NET_INTERNAL_H
#define NETFLIM_NET_INTERNAL_H

#include "net.h"
#include "codec.h"

#include <Devices.h>
#include <Errors.h>
#include <OpenTransport.h>
#include <OpenTransportProviders.h>
/* Universal Interfaces defines Mentat uid_t/gid_t/dev_t as UInt32, conflicting with newlib <sys/types.h> */
#define uid_t _ot_uid_t
#define gid_t _ot_gid_t
#define dev_t _ot_dev_t
#include <OpenTransportProtocol.h>
#undef dev_t
#undef gid_t
#undef uid_t

#define NF_ARENA_TAIL       (NF_MAX_FRAME_BYTES + 16UL)
#define NF_INDEX_SLOTS      128
#define NF_INDEX_MASK       (NF_INDEX_SLOTS - 1)
#define TCP_VOD_CREATE_MAX  24576L
#define TCP_VOD_CHUNK_MAX   12288UL

typedef enum {
    kVodTcpClosed = 0,
    kVodTcpOpening,
    kVodTcpConnecting,
    kVodTcpSending,
    kVodTcpStreaming
} VodTcpConnState;

typedef enum {
    kParseHttpHdr = 0,
    kParseStreamHdr,
    kParseFrames
} VodParsePhase;

static inline unsigned short net_read_u16be(const unsigned char *p) {
    return (unsigned short)(((unsigned short)p[0] << 8) | (unsigned short)p[1]);
}

static inline unsigned long net_read_u32be(const unsigned char *p) {
    return ((unsigned long)p[0] << 24) |
           ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |
           (unsigned long)p[3];
}

/* Shared globals owned in net.c */
extern short                  gMacTCPRef;
extern Boolean                gUseNativeOT;
extern OSErr                  gLastTcpErr;
extern ip_addr                gServerIP;
extern unsigned short         gServerHttpPort;

extern unsigned char         *gArenaMem;
extern unsigned long          gArenaCap;
extern unsigned long          gArenaWrite;
extern unsigned long          gArenaParse;
extern unsigned long          gArenaUsed;
extern unsigned long          gArenaAvailToParse;
extern short                  gRingNumSlots;
extern short                  gBufferedCount;
extern unsigned long          gReclaimCursor;
extern volatile unsigned long gIrqCompletedUpTo;
extern NetVodFrameDesc        gDescRing[NF_INDEX_SLOTS];

extern unsigned long          gTcpCreateBufSize;
extern StreamPtr              gVodTcpStream;
extern TCPiopb                gVodOpenPB;
extern TCPiopb                gVodSendPB;
extern TCPiopb                gVodRxPB[2];
extern Boolean                gTcpRxArmed[2];
extern short                  gTcpRxActiveSlot;
extern VodTcpConnState        gTcpConnState;
extern char                   gVodHttpReq[384];
extern wdsEntry               gVodWds[2];

extern Boolean                gTcpRemoteEof;
extern VodParsePhase          gParsePhase;

extern Boolean                gVodActive;
extern Boolean                gVodAckReceived;
extern unsigned char          gVodOpenStatus;
extern Boolean                gVodEosReached;
extern Boolean                gVodPaused;
extern unsigned long          gVodFrameCount;
extern unsigned long          gNextNeededFrame;

extern EndpointRef            gOtVodEp;
extern InetAddress            gOtVodCallAddr;
extern TCall                  gOtVodCall;
extern unsigned short         gOtVodSendOff;
extern unsigned short         gOtVodSendLen;
extern volatile Boolean       gOtVodOpenDone;
extern volatile OSStatus      gOtVodOpenErr;
extern volatile EndpointRef   gOtVodOpenEp;
extern volatile Boolean       gOtVodConnectDone;
extern volatile Boolean       gOtVodHasData;
extern volatile Boolean       gOtVodNeedLook;
extern volatile Boolean       gOtVodGoData;
extern Boolean                gOtVodPausedForRoom;
extern OTBuffer              *gOtPendingHead;
extern OTBuffer              *gOtPendingCur;
extern unsigned long          gOtPendingOff;

/* Cold helpers implemented in net.c */
Boolean  NetInternalRingCanQueueMore(void);
void     NetInternalArmTcpRecvSlot(short slot);
void     NetInternalArmTcpRecv(void);
void     NetInternalParseInitialHandshake(void);
void     NetInternalOtCloseVodStream(void);
void     NetInternalOtVodFailClosed(OSErr err);
void     NetInternalOtCloseEndpointSync(EndpointRef ep);
OSStatus NetInternalOtSetFourByteOption(EndpointRef ep, OTXTILevel level, OTXTIName name, UInt32 value);

/* Hot functions compiled in net_hot.c (_68k) and net_hot_020.c (_020) */
void ScanArenaFrames_68k(void);
void ScanArenaFrames_020(void);
void OtDrainIncoming_68k(void);
void OtDrainIncoming_020(void);

#endif /* NETFLIM_NET_INTERNAL_H */
