#ifndef NETFLIM_CODEC_H
#define NETFLIM_CODEC_H

#ifdef __cplusplus
extern "C" {
#endif

#define NETFLIM_WIDTH       512
#define NETFLIM_HEIGHT      342
#define NETFLIM_ROW_BYTES   64
#define NETFLIM_FB_BYTES    21888  /* 64 * 342 */

#define NETFLIM_CODEC_NULL   0x00
#define NETFLIM_CODEC_Z16    0x01
#define NETFLIM_CODEC_Z32    0x02
#define NETFLIM_CODEC_INVERT 0x03
#define NETFLIM_CODEC_LINES  0x04

typedef struct {
    unsigned short        ticks;
    const unsigned char  *pcmPtr;
    unsigned short        pcmBytes;
    const unsigned char  *videoPtr;   /* Points to 4-byte codec header + payload */
    unsigned short        videoBytes;
    unsigned char         codecId;
} NetFlimParsedFrame;

/**
 * Fast 68k-optimized block memory copy (uses 32-byte unrolled move.l bursts
 * when even-aligned, avoiding Newlib's byte-loop penalty).
 */
void NetFlimFastCopy(void *dst, const void *src, unsigned long len);

/**
 * Resets a 512x342 (21,888-byte) 1-bit framebuffer to solid black (0xFF),
 * matching MacFlim's initial movie state.
 */
void NetFlimResetFramebuffer(unsigned char *fb);

/**
 * Parses a single serialized .flim frame slice into audio PCM and video codec pointers.
 * Returns 1 on success, 0 if truncated or malformed.
 */
int NetFlimParseFrameSlice(const unsigned char *frameBuf,
                           unsigned short frameBytes,
                           NetFlimParsedFrame *out);

/**
 * Decodes a single MacFlim video payload (starting at the 4-byte codec header)
 * into a 21,888-byte 512x342 1-bit framebuffer (rowBytes = 64, even-aligned).
 * Supports codecs 0x00 (null), 0x01 (z16), 0x02 (z32), 0x03 (invert), 0x04 (lines).
 * Returns 1 on success, 0 if codec unknown or input invalid.
 */
int NetFlimDecodeVideo(unsigned char *fb,
                       const unsigned char *videoPtr,
                       unsigned short videoBytes);

#ifdef __cplusplus
}
#endif

#endif /* NETFLIM_CODEC_H */
