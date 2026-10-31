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
extern char             gActiveTransferName[64];
extern unsigned long    gActiveTransferBytes;
extern unsigned long    gActiveTransferTotal;
extern char             gCurrentTransferId[64];

/* Initializes MacTCP driver and retrieves local IP address */
Boolean NetInit(void);

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

#endif /* MACDROP_NETWORK_H */
