#ifndef MACAI_NETWORK_H
#define MACAI_NETWORK_H

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

/* Request cancellation of an in-flight MacTCP HTTP request (e.g. on Quit or Cmd+.) */
void NetAbortCurrentOperation(void);

/*
 * Broadcasts "MACCOMPANION_DISCOVER" over UDP port 6801 and waits for
 * "MACCOMPANION_SERVER|<ip>|<port>|<modules>".
 */
Boolean NetDiscoverServer(void);

typedef void (*NetStatusCallback)(const char *statusText);

/*
 * Performs an HTTP/1.0 GET request to the MacCompanion server and copies the
 * response body into `outBody` (up to `maxLen - 1` bytes).
 * Returns number of body bytes read, or -1 on network error.
 */
long NetHttpGet(const char *path, char *outBody, long maxLen);

/*
 * Performs an HTTP/1.0 GET request with an optional real-time status callback.
 * If lines beginning with "STATUS|" arrive, onStatus is invoked.
 */
long NetHttpGetStream(const char *path, char *outBody, long maxLen, NetStatusCallback onStatus, short timeoutSec);

/*
 * Performs an HTTP/1.0 GET request and writes the payload directly into an open file.
 * Returns total bytes written, or -1 on network/file error.
 */
long NetHttpGetToFile(const char *path, short fileRefNum);

/*
 * Performs an HTTP/1.0 POST request with a raw binary or text payload
 * (`postBody`, `postLen` bytes) and copies the response body into `outBody`.
 */
long NetHttpPost(const char *path, const char *contentType,
                 const void *postBody, long postLen,
                 char *outBody, long maxLen);

/* URL-encodes a query parameter string */
void UrlEncode(const char *src, char *dst, short maxDst);

/* ============================================================================
 * Native 68k Audio Streaming Engine (TCP Port 6802 -> Sound Manager for TTS)
 * ============================================================================ */

void AudioStreamInit(void);
Boolean AudioStreamStart(unsigned short sampleRateHz);
void AudioStreamStop(void);
Boolean AudioStreamIsRunning(void);
void AudioStreamPump(void);

/* ============================================================================
 * Sound Input Manager Hardware Detection & Voice Recording
 * ============================================================================ */

/*
 * Checks Gestalt + Sound Input Manager (SPBOpenDevice) to determine if this
 * Macintosh has an active microphone / sound input device.
 * Compact Macs without a 3rd-party sound input peripheral return false,
 * allowing MacAI to hide the microphone button cleanly.
 */
Boolean SoundInputHasHardware(void);

/* Start async recording into an internal 8-bit unsigned PCM buffer */
Boolean SoundInputStartRecording(unsigned short *outSampleRateHz);

/* Poll active recording; returns true if still recording */
Boolean SoundInputIsRecording(void);

/* Stop recording and return pointer + byte length + sample rate of recorded 8-bit PCM */
Boolean SoundInputStopRecording(Ptr *outBuf, long *outLen, unsigned short *outSampleRateHz);

#endif /* MACAI_NETWORK_H */
