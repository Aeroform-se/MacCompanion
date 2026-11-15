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

#define NETFLIM_FRAME_FLAG_MIDPAD1 0x01

typedef struct {
    unsigned short        ticks;
    const unsigned char  *ffSynthPtr;  /* Points to 6B FFSynthRec header + PCM bytes */
    unsigned short        ffSynthBytes;
    unsigned long         ffSynthRate; /* 16.16 Fixed step: 0x00010000=22.25k, 0x00008000=11.13k */
    const unsigned char  *pcmPtr;
    unsigned short        pcmBytes;
    const unsigned char  *videoPtr;    /* Points to 4-byte codec header + payload */
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
 * Parses a single serialized .flim frame slice into audio FFSynth/PCM and video codec pointers.
 * Returns 1 on success, 0 if truncated or malformed.
 */
int NetFlimParseFrameSliceEx(const unsigned char *frameBuf,
                             unsigned short frameBytes,
                             unsigned char frameFlags,
                             NetFlimParsedFrame *out);

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

/**
 * Decodes a single MacFlim video payload directly into a shifted framebuffer
 * (such as screenBits.baseAddr + 1344 for a 512x256 windowed viewport on a
 * 512x342 Compact Mac), clipping all writes strictly to scanlines
 * [clipMinRow, clipMaxRow). Eliminates both offscreen RAM writes and the
 * 16,384-byte per-frame viewport blit in windowed mode.
 */
int NetFlimDecodeVideoClipped(unsigned char *fbBase,
                              unsigned short clipMinRow,
                              unsigned short clipMaxRow,
                              const unsigned char *videoPtr,
                              unsigned short videoBytes);

#ifdef __cplusplus
}
#endif

typedef int (*NetFlimDecodeVideoFn)(unsigned char *fb,
                                     const unsigned char *videoPtr,
                                     unsigned short videoBytes);

/*
 * Initialize runtime CPU dispatch (Gestalt / SysEnvirons) so 68020/030/040
 * Macs use the -m68020 compiled decoder & blitter automatically.
 */
void NetFlimCodecInit(void);
int NetFlimCodecIs020(void);
const char *NetFlimCodecCpuName(void);
NetFlimDecodeVideoFn NetFlimGetDecodeVideoFn(void);

/* Direct CPU-specific symbols exported for dual-compiled _020/_68k hot paths */
void NetFlimFastCopy_68k(void *dst, const void *src, unsigned long len);
void NetFlimFastCopy_020(void *dst, const void *src, unsigned long len);
void NetFlimResetFramebuffer_68k(unsigned char *fb);
void NetFlimResetFramebuffer_020(unsigned char *fb);
int  NetFlimParseFrameSliceEx_68k(const unsigned char *frameBuf,
                                  unsigned short frameBytes,
                                  unsigned char frameFlags,
                                  NetFlimParsedFrame *out);
int  NetFlimParseFrameSliceEx_020(const unsigned char *frameBuf,
                                  unsigned short frameBytes,
                                  unsigned char frameFlags,
                                  NetFlimParsedFrame *out);
int  NetFlimDecodeVideo_68k(unsigned char *fb,
                            const unsigned char *videoPtr,
                            unsigned short videoBytes);
int  NetFlimDecodeVideo_020(unsigned char *fb,
                            const unsigned char *videoPtr,
                            unsigned short videoBytes);
int  NetFlimDecodeVideoClipped_68k(unsigned char *fbBase,
                                   unsigned short clipMinRow,
                                   unsigned short clipMaxRow,
                                   const unsigned char *videoPtr,
                                   unsigned short videoBytes);
int  NetFlimDecodeVideoClipped_020(unsigned char *fbBase,
                                   unsigned short clipMinRow,
                                   unsigned short clipMaxRow,
                                   const unsigned char *videoPtr,
                                   unsigned short videoBytes);

#endif /* NETFLIM_CODEC_H */
