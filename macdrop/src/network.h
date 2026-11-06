#ifndef MACDROP_NETWORK_H
#define MACDROP_NETWORK_H

#include <MacTypes.h>
#include <MacTCP.h>
#include "file_io.h"

#define MACDROP_TCP_PORT        6803
#define DEFAULT_HTTP_PORT       6800
#define DEFAULT_UDP_PORT        6801

extern Boolean          gNetReady;
extern ip_addr          gOurIP;
extern ip_addr          gServerIP;
extern unsigned short   gServerPort;
extern Boolean          gServerConnected;
extern Boolean          gChannelConnected;

extern Boolean          gTransferActive;
extern Boolean          gTransferIsUpload;
extern char             gActiveTransferName[64];
extern unsigned long    gActiveTransferBytes;
extern unsigned long    gActiveTransferTotal;
extern unsigned long    gTransferSpeedBps;
extern char             gCurrentTransferId[64];
extern Rect             gBtnCancelTransferRect;

#define MAX_MACDROP_PEERS 5

typedef struct {
    char id[48];
    char name[48];
    char machine[48];
} MacDropPeer;

extern MacDropPeer      gOnlinePeers[MAX_MACDROP_PEERS];
extern short            gOnlinePeerCount;
extern char             gActiveUploadTargetName[48];

/* Cancels active inbound or outbound file transfer */
void NetCancelActiveTransfer(void);

/* Initializes MacTCP driver and retrieves local IP address */
Boolean NetInit(void);

/* Sends a file to MacCompanion server / modern Mac via HTTP POST MacBinary */
Boolean NetSendFile(const FSSpec *spec);

/* Sends a file to a specific target ("SERVER" or a peer 68k Mac ID) */
Boolean NetSendFileToTarget(const FSSpec *spec, const char *targetId, const char *targetName);

/* Queries /api/macdrop/peers from server to update gOnlinePeers immediately */
Boolean NetRefreshPeers(void);

/* Attempts UDP discovery of MacCompanion middleware */
Boolean NetDiscoverServer(void);

/* Formats IP address as dotted string */
void FormatIP(ip_addr addr, char *outBuf);

/* Parses dotted string into ip_addr */
Boolean ParseIPAddress(const char *str, ip_addr *outAddr);

/* Checks if server at IP:port responds with MACCOMPANION_SERVER */
Boolean NetVerifyServer(ip_addr ip, unsigned short port);

/* Sets current active server */
void NetSetServer(ip_addr ip, unsigned short port);

/* Performs HTTP GET request to MacCompanion server */
long NetHttpGet(const char *path, char *outBody, long maxLen);

/* Sends transfer confirmation ACK to server */
void NetSendTransferAck(const char *transferId, const char *status, unsigned long bytesReceived);

/* Starts the passive TCP listener on port 6803 */
Boolean NetStartListener(void);

/* Stops TCP listener */
void NetStopListener(void);

/* Establishes reverse push channel to Node.js server */
Boolean NetConnectChannel(void);

/* Closes reverse push channel */
void NetStopChannel(void);

/* Main event loop network polling */
void NetIdle(void);

/* Called when a new file transfer starts so any unacknowledged alert is automatically cleared */
void OnTransferStarted(Boolean isUpload);

/* Displays or updates the non-blocking transfer completion alert, baking in any unacknowledged files */
void ShowTransferNotifyAlert(const char *fileName, Boolean isUpload);

#endif /* MACDROP_NETWORK_H */
