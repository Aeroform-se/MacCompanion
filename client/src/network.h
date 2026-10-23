#ifndef NETWORK_H
#define NETWORK_H

#include <MacTypes.h>
#include <MacTCP.h>

#define DEFAULT_HTTP_PORT   6800
#define DEFAULT_UDP_PORT    6801
#define DEFAULT_AUDIO_PORT  6802

/* Initialize MacTCP driver (.ipp) and fetch local IP/subnet mask */
Boolean NetInit(void);

/* Format an ip_addr (uint32 big-endian) as "A.B.C.D" */
void FormatIPAddress(ip_addr addr, char *outBuf);

/* Parse "A.B.C.D" string into an ip_addr; returns true if valid */
Boolean ParseIPAddress(const char *str, ip_addr *outAddr);

/* Set or get the active MacCompanion server IP and HTTP port */
void NetSetServer(ip_addr ip, unsigned short httpPort);
ip_addr NetGetServerIP(void);
ip_addr NetGetOurIP(void);
unsigned short NetGetServerPort(void);
Boolean NetHasServer(void);

/* Register an idle callback invoked while waiting on async MacTCP HTTP calls */
void NetSetIdleCallback(void (*callback)(void));

/*
 * Broadcasts "MACCOMPANION_DISCOVER" over UDP port 6801 (to subnet broadcast,
 * global broadcast, and local gateway/host alias 10.0.2.2) and waits for
 * "MACCOMPANION_SERVER|<ip>|<port>|<modules>".
 * Returns true and sets the server IP/port if discovered.
 */
Boolean NetDiscoverServer(void);

/*
 * Performs an HTTP/1.0 GET request to the MacCompanion server and copies the
 * response body (after "\r\n\r\n") into `outBody` (up to `maxLen - 1` bytes),
 * null-terminating `outBody`.
 * Returns number of body bytes read, or -1 on network error.
 */
long NetHttpGet(const char *path, char *outBody, long maxLen);

/*
 * URL-encodes a query parameter string (e.g. search query or Spotify URI).
 */
void UrlEncode(const char *src, char *dst, short maxDst);

/* ============================================================================
 * Native 68k Audio Streaming Engine (TCP Port 6802 -> Sound Manager)
 * ============================================================================ */

void AudioStreamInit(void);
Boolean AudioStreamStart(unsigned short sampleRateHz);
void AudioStreamStop(void);
Boolean AudioStreamIsRunning(void);
void AudioStreamPump(void);
Boolean AudioGetLiveSamples(unsigned char *outSamples, short count);

#endif /* NETWORK_H */
