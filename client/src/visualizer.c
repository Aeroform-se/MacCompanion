/*
 * SpotiMac "Partylizer II" — 68000 / 68020+ Dual-Compiled Demoscene Visualizer Engine
 * Renders 7 Winamp & Amiga/Mac demoscene visualizations in 200x200 1-bit dithered
 * graphics (plus 256x161 -> 512x322 fullscreen), with runtime CPU dispatch:
 *   - 68000 path (Mac Plus, SE, Classic)
 *   - 68020/68030/68040 path (SE/30, Mac II, Quadra) selected automatically at launch.
 */

#include "visualizer.h"
#include "network.h"
#include <Memory.h>
#include <Events.h>
#include <OSUtils.h>
#include <Gestalt.h>
#include <string.h>
#include <stdlib.h>

#define FS_W            256
#define FS_H            161
#define FS_ROWBYTES     32   /* 256 / 8 = 32 bytes per row (exact, zero padding) */
#define FS_BITMAP_BYTES 5152 /* 32 * 161 */

#define NUM_STARS    48
typedef struct {
    short x; /* -120..+120 */
    short y; /* -120..+120 */
    short z; /* 16..255 */
} Star3D;

#define TORUS_RINGS  8
#define TORUS_SEGS   6
#define TORUS_VERTS  (TORUS_RINGS * TORUS_SEGS) /* 48 vertices */
#define TORUS_FACES  (TORUS_RINGS * TORUS_SEGS) /* 48 quads */

typedef struct {
    short sx[4];
    short sy[4];
    short avgZ;
    unsigned char shade; /* 1..16 */
} FaceQuad2D;

typedef struct {
    short cx, cy, cz; /* Center in 100x100xZ space */
    short scaleInv;   /* 64 = full R=30, 96 = R=20, 128 = R=15 */
    short radiusPx;   /* Screen radius in 100x100 space */
    unsigned char spin;
} ChromeSphere;

#define TUNNEL_LUT_W   128
#define TUNNEL_LUT_H   128
#define SPHERE_LUT_DIM 64 /* Covers radius R=32 (-32..+31) */
#define VOXEL_MAP_DIM  64

#define FAST_SIN(a) (gSinTbl[(unsigned char)(a)])
#define FAST_COS(a) (gSinTbl[(unsigned char)((a) + 64)])

/* ============================================================================
 * Shared Lookup Tables & State (defined once in 68000 TU, shared by 68020+ TU)
 * ============================================================================ */

#ifndef VIZ_BUILD_020

static Boolean gVizInited        = false;
static Boolean gFsLutInited      = false;
static Boolean gUse020Visualizer = false;
static short   gDetectedCpu      = 0;

/* 256-entry signed 8.8 fixed-point sine table (-127..+127) */
const short gSinTbl[256] = {
       0,    3,    6,    9,   12,   16,   19,   22,   25,   28,   31,   34,   37,   40,   43,   46,
      49,   51,   54,   57,   60,   63,   65,   68,   71,   73,   76,   78,   81,   83,   85,   88,
      90,   92,   94,   96,   98,  100,  102,  104,  106,  107,  109,  111,  112,  113,  115,  116,
     117,  118,  120,  121,  122,  122,  123,  124,  125,  125,  126,  126,  126,  127,  127,  127,
     127,  127,  127,  127,  126,  126,  126,  125,  125,  124,  123,  122,  122,  121,  120,  118,
     117,  116,  115,  113,  112,  111,  109,  107,  106,  104,  102,  100,   98,   96,   94,   92,
      90,   88,   85,   83,   81,   78,   76,   73,   71,   68,   65,   63,   60,   57,   54,   51,
      49,   46,   43,   40,   37,   34,   31,   28,   25,   22,   19,   16,   12,    9,    6,    3,
       0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,  -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
     -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,  -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
     -90,  -92,  -94,  -96,  -98, -100, -102, -104, -106, -107, -109, -111, -112, -113, -115, -116,
    -117, -118, -120, -121, -122, -122, -123, -124, -125, -125, -126, -126, -126, -127, -127, -127,
    -127, -127, -127, -127, -126, -126, -126, -125, -125, -124, -123, -122, -122, -121, -120, -118,
    -117, -116, -115, -113, -112, -111, -109, -107, -106, -104, -102, -100,  -98,  -96,  -94,  -92,
     -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,  -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
     -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,  -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3
};

/* Precomputed `(x * 16) / 100` for x = 0..99 (eliminates 11,200 __divsi3 calls/frame in Mode 6!) */
const unsigned char kBand100[100] = {
     0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  3,
     3,  3,  3,  3,  3,  4,  4,  4,  4,  4,  4,  4,  5,  5,  5,  5,  5,  5,  6,  6,
     6,  6,  6,  6,  7,  7,  7,  7,  7,  7,  8,  8,  8,  8,  8,  8,  8,  9,  9,  9,
     9,  9,  9, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12,
    12, 12, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 15, 15, 15, 15, 15, 15
};

/* Precomputed `((x - 50) * 8) / 5` for x = 0..99 (eliminates per-pixel __divsi3 in FS Mode 7 sun!) */
const signed char kSunDx100[100] = {
    -80, -78, -76, -75, -73, -72, -70, -68, -67, -65, -64, -62, -60, -59, -57, -56, -54, -52, -51, -49,
    -48, -46, -44, -43, -41, -40, -38, -36, -35, -33, -32, -30, -28, -27, -25, -24, -22, -20, -19, -17,
    -16, -14, -12, -11,  -9,  -8,  -6,  -4,  -3,  -1,   0,   1,   3,   4,   6,   8,   9,  11,  12,  14,
     16,  17,  19,  20,  22,  24,  25,  27,  28,  30,  32,  33,  35,  36,  38,  40,  41,  43,  44,  46,
     48,  49,  51,  52,  54,  56,  57,  59,  60,  62,  64,  65,  67,  68,  70,  72,  73,  75,  76,  78
};

/* 8x8 Bayer Ordered Dither Matrix (0..63) */
static const unsigned char gBayer8x8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 }
};

unsigned char gDitherP0[8][64];
unsigned char gDitherP1[8][64];
unsigned char gDitherP2[8][64];
unsigned char gDitherP3[8][64];
unsigned char gShadeByte[17][4];
unsigned char gPlasmaLut[256];
unsigned char gFireCoolLut[256];
unsigned short gRowOff200[VIZ_H];
unsigned char gFsYTo98[FS_H];
unsigned char gFsYTo99[FS_H];

unsigned char *gRadialDist  = NULL; /* 100 * 100 = 10,000 bytes */
unsigned char *gTunnelAngle = NULL; /* 16,384 bytes */
unsigned char *gTunnelDepth = NULL; /* 16,384 bytes */
unsigned char *gTunnelFog   = NULL; /* 16,384 bytes */
unsigned char *gFireBuf     = NULL; /* 100 * 102 = 10,200 bytes */

signed char   *gSphereNz    = NULL; /* 64 * 64 */
unsigned char *gSphereEnvU  = NULL; /* 64 * 64 */
unsigned char *gSphereEnvV  = NULL; /* 64 * 64 */
unsigned char *gSphereSpec  = NULL; /* 64 * 64 */

unsigned char *gVoxelHeight = NULL; /* 64 * 64 = 4,096 bytes */
unsigned char *gVoxelShade  = NULL; /* 64 * 64 = 4,096 bytes */
unsigned char  gVoxelWriteRow = 0;

static unsigned char gRawPcm[256];
signed char   gWave200[VIZ_W];     /* -21..+21 vertical scope offset */
short         gEqBands[16];        /* 0..96 smoothed bar heights */
short         gEqPeaks[16];        /* 0..96 peak hold positions */
static short  gEqPeakVel[16];      /* falling velocity of peak caps */
short         gAudioEnergy = 0;    /* 0..100 overall energy */
short         gBassPulse   = 0;    /* 0..64 bass kick envelope */
short         gTreblePulse = 0;    /* 0..64 treble envelope */
static short  gAvgBass     = 20;
Boolean       gBeatHit     = false;
unsigned long gFrameCount  = 0;

Star3D        gStars[NUM_STARS];
unsigned char gAngleX = 20;
unsigned char gAngleY = 0;
unsigned char gAngleZ = 10;
short         gTorusZoomSmooth = 0;
short         gTorusTwistPhase = 0;
FaceQuad2D    gVisibleFaces[TORUS_FACES];

unsigned char  gFsBits[FS_BITMAP_BYTES];
static unsigned short gByteDoubleLut[256];

#else /* VIZ_BUILD_020: reference shared tables & state from 68000 TU */

extern const short         gSinTbl[256];
extern const unsigned char kBand100[100];
extern const signed char   kSunDx100[100];
extern unsigned char       gDitherP0[8][64];
extern unsigned char       gDitherP1[8][64];
extern unsigned char       gDitherP2[8][64];
extern unsigned char       gDitherP3[8][64];
extern unsigned char       gShadeByte[17][4];
extern unsigned char       gPlasmaLut[256];
extern unsigned char       gFireCoolLut[256];
extern unsigned short      gRowOff200[VIZ_H];
extern unsigned char       gFsYTo98[FS_H];
extern unsigned char       gFsYTo99[FS_H];

extern unsigned char *gRadialDist;
extern unsigned char *gTunnelAngle;
extern unsigned char *gTunnelDepth;
extern unsigned char *gTunnelFog;
extern unsigned char *gFireBuf;
extern signed char   *gSphereNz;
extern unsigned char *gSphereEnvU;
extern unsigned char *gSphereEnvV;
extern unsigned char *gSphereSpec;
extern unsigned char *gVoxelHeight;
extern unsigned char *gVoxelShade;
extern unsigned char  gVoxelWriteRow;

extern signed char    gWave200[VIZ_W];
extern short          gEqBands[16];
extern short          gEqPeaks[16];
extern short          gAudioEnergy;
extern short          gBassPulse;
extern short          gTreblePulse;
extern Boolean        gBeatHit;
extern unsigned long  gFrameCount;

extern Star3D         gStars[NUM_STARS];
extern unsigned char  gAngleX;
extern unsigned char  gAngleY;
extern unsigned char  gAngleZ;
extern short          gTorusZoomSmooth;
extern short          gTorusTwistPhase;
extern FaceQuad2D     gVisibleFaces[TORUS_FACES];
extern unsigned char  gFsBits[FS_BITMAP_BYTES];

#endif /* VIZ_BUILD_020 */

/* Forward declarations for dual-dispatched entry points */
void VizDispatch200_000(short mode, unsigned char *outBits);
void VizDispatchFS_000(short mode, unsigned char *fsBits);
void VizDispatch200_020(short mode, unsigned char *outBits);
void VizDispatchFS_020(short mode, unsigned char *fsBits);

/* ============================================================================
 * Fast Hardware 16-bit Multiply/Divide & Assembly Primitives
 * ============================================================================ */

/* Inline 68000 hardware signed 32/16 -> 16 divide (avoids 220-cycle __divsi3 libcall) */
static inline short DivS16(long num, short den)
{
    __asm__ ("divs.w %1, %0" : "+d"(num) : "dm"(den) : "cc");
    return (short)num;
}

/* Inline 68000 hardware signed 16*16 -> 32 multiply (avoids 120-cycle __mulsi3 libcall) */
static inline long MulS16(short a, short b)
{
    long res = a;
    __asm__ ("muls.w %1, %0" : "+d"(res) : "dm"(b) : "cc");
    return res;
}

static void AsmClearBlack5200(unsigned char *dst)
{
    unsigned long fill = 0xFFFFFFFFUL;
    short loops = (5200 / 16) - 1;
    __asm__ volatile (
        "1:\n\t"
        "move.l %2, (%0)+\n\t"
        "move.l %2, (%0)+\n\t"
        "move.l %2, (%0)+\n\t"
        "move.l %2, (%0)+\n\t"
        "dbf %1, 1b\n\t"
        : "+a"(dst), "+d"(loops)
        : "d"(fill)
        : "cc", "memory"
    );
}

static void AsmClearBlack5152(unsigned char *dst)
{
    unsigned long fill = 0xFFFFFFFFUL;
    short loops = (5152 / 16) - 1;
    __asm__ volatile (
        "1:\n\t"
        "move.l %2, (%0)+\n\t"
        "move.l %2, (%0)+\n\t"
        "move.l %2, (%0)+\n\t"
        "move.l %2, (%0)+\n\t"
        "dbf %1, 1b\n\t"
        : "+a"(dst), "+d"(loops)
        : "d"(fill)
        : "cc", "memory"
    );
}

/*
 * F11 Optimized 68000 assembly converter: clears high bits of d0_tmp ONCE before the loop
 * and uses word-indexed lookups (saves 100 instructions per row = 20,000 instructions/frame).
 */
static void AsmPackDitherRow25(
    unsigned char *dstRow,
    const unsigned char *rowVals,
    const unsigned char *p0,
    const unsigned char *p1,
    const unsigned char *p2,
    const unsigned char *p3
)
{
    short count = 25 - 1;
    long d0_tmp, d1_tmp;
    __asm__ volatile (
        "moveq #0, %2\n\t"
        "1:\n\t"
        "move.b (%1)+, %2\n\t"
        "move.b 0(%5, %2.w), %3\n\t"

        "move.b (%1)+, %2\n\t"
        "or.b 0(%6, %2.w), %3\n\t"

        "move.b (%1)+, %2\n\t"
        "or.b 0(%7, %2.w), %3\n\t"

        "move.b (%1)+, %2\n\t"
        "or.b 0(%8, %2.w), %3\n\t"

        "move.b %3, (%0)+\n\t"
        "dbf %4, 1b\n\t"
        : "+a"(dstRow), "+a"(rowVals), "=&d"(d0_tmp), "=&d"(d1_tmp), "+d"(count)
        : "a"(p0), "a"(p1), "a"(p2), "a"(p3)
        : "cc", "memory"
    );
}

static void AsmPackDitherRow32(
    unsigned char *dstRow,
    const unsigned char *rowVals,
    const unsigned char *p0,
    const unsigned char *p1,
    const unsigned char *p2,
    const unsigned char *p3
)
{
    short count = 32 - 1;
    long d0_tmp, d1_tmp;
    __asm__ volatile (
        "moveq #0, %2\n\t"
        "1:\n\t"
        "move.b (%1)+, %2\n\t"
        "move.b 0(%5, %2.w), %3\n\t"

        "move.b (%1)+, %2\n\t"
        "or.b 0(%6, %2.w), %3\n\t"

        "move.b (%1)+, %2\n\t"
        "or.b 0(%7, %2.w), %3\n\t"

        "move.b (%1)+, %2\n\t"
        "or.b 0(%8, %2.w), %3\n\t"

        "move.b %3, (%0)+\n\t"
        "dbf %4, 1b\n\t"
        : "+a"(dstRow), "+a"(rowVals), "=&d"(d0_tmp), "=&d"(d1_tmp), "+d"(count)
        : "a"(p0), "a"(p1), "a"(p2), "a"(p3)
        : "cc", "memory"
    );
}

/*
 * F10 Optimized Fire Diffusion: uses precomputed `gFireCoolLut[sum]` (no branch, single zero-init)
 */
static void AsmDiffuseFireRow98(unsigned char *dst, const unsigned char *srcBelow)
{
    short count = 98 - 1;
    const unsigned char *coolLut = gFireCoolLut;
    long d0, d1;
    __asm__ volatile (
        "moveq #0, %3\n\t"
        "1:\n\t"
        "moveq #0, %2\n\t"
        "move.b (%1)+, %2\n\t"
        "move.b (%1), %3\n\t"
        "add.w %3, %2\n\t"
        "add.w %3, %2\n\t"
        "move.b 1(%1), %3\n\t"
        "add.w %3, %2\n\t"
        "move.b 0(%5, %2.w), (%0)+\n\t"
        "dbf %4, 1b\n\t"
        : "+a"(dst), "+a"(srcBelow), "=&d"(d0), "=&d"(d1), "+d"(count)
        : "a"(coolLut)
        : "cc", "memory"
    );
}

/* F9 Optimized Pixel & Vertical Span Helpers (use kRowOff200 / pointer stepping instead of muls.w #26) */
static inline void SetPixelWhite(unsigned char *bits, short x, short y)
{
    if ((unsigned short)x < VIZ_W && (unsigned short)y < VIZ_H) {
        bits[gRowOff200[y] + (x >> 3)] &= (unsigned char)~(0x80 >> (x & 7));
    }
}

static inline void SetPixelBlack(unsigned char *bits, short x, short y)
{
    if ((unsigned short)x < VIZ_W && (unsigned short)y < VIZ_H) {
        bits[gRowOff200[y] + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
    }
}

static inline void DrawVSpanWhite(unsigned char *bits, short x, short yMin, short yMax)
{
    unsigned char *p = bits + gRowOff200[yMin] + (x >> 3);
    unsigned char mask = (unsigned char)~(0x80 >> (x & 7));
    short n = yMax - yMin;
    do {
        *p &= mask;
        p += VIZ_ROWBYTES;
    } while (n-- > 0);
}

static inline void SetPixelWhiteFS(unsigned char *bits, short x, short y)
{
    if ((unsigned short)x < FS_W && (unsigned short)y < FS_H) {
        bits[(y << 5) + (x >> 3)] &= (unsigned char)~(0x80 >> (x & 7));
    }
}

static inline void SetPixelBlackFS(unsigned char *bits, short x, short y)
{
    if ((unsigned short)x < FS_W && (unsigned short)y < FS_H) {
        bits[(y << 5) + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
    }
}

static inline void DrawVSpanWhiteFS(unsigned char *bits, short x, short yMin, short yMax)
{
    unsigned char *p = bits + (yMin << 5) + (x >> 3);
    unsigned char mask = (unsigned char)~(0x80 >> (x & 7));
    short n = yMax - yMin;
    do {
        *p &= mask;
        p += FS_ROWBYTES;
    } while (n-- > 0);
}

static void DrawLineWhite(unsigned char *bits, short x0, short y0, short x1, short y1)
{
    short dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
    short dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
    short err = dx + dy, e2;

    for (;;) {
        if ((unsigned short)x0 < VIZ_W && (unsigned short)y0 < VIZ_H) {
            bits[gRowOff200[y0] + (x0 >> 3)] &= (unsigned char)~(0x80 >> (x0 & 7));
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err << 1;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void DrawLineWhiteFS(unsigned char *bits, short x0, short y0, short x1, short y1)
{
    short dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
    short dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
    short err = dx + dy, e2;

    for (;;) {
        if ((unsigned short)x0 < FS_W && (unsigned short)y0 < FS_H) {
            bits[(y0 << 5) + (x0 >> 3)] &= (unsigned char)~(0x80 >> (x0 & 7));
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err << 1;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* F12 Optimized FillSpanPattern: inline byte loop with asm barrier prevents GCC emitting jsr memset */
static void FillSpanPattern(unsigned char *rowPtr, short xLeft, short xRight, unsigned char patByte)
{
    short bLeft, bRight, b;
    unsigned char leftMask, rightMask;

    if (xLeft < 0) xLeft = 0;
    if (xRight >= VIZ_W) xRight = VIZ_W - 1;
    if (xLeft > xRight) return;

    bLeft  = xLeft >> 3;
    bRight = xRight >> 3;
    leftMask  = (unsigned char)(0xFF >> (xLeft & 7));
    rightMask = (unsigned char)(0xFF << (7 - (xRight & 7)));

    if (bLeft == bRight) {
        unsigned char mask = leftMask & rightMask;
        rowPtr[bLeft] = (rowPtr[bLeft] & ~mask) | (patByte & mask);
    } else {
        rowPtr[bLeft] = (rowPtr[bLeft] & ~leftMask) | (patByte & leftMask);
        for (b = bLeft + 1; b < bRight; b++) {
            rowPtr[b] = patByte;
            __asm__ volatile ("" : : : "memory");
        }
        rowPtr[bRight] = (rowPtr[bRight] & ~rightMask) | (patByte & rightMask);
    }
}

static void FillSpanPatternFS(unsigned char *rowPtr, short xLeft, short xRight, unsigned char patByte)
{
    short bLeft, bRight, b;
    unsigned char leftMask, rightMask;

    if (xLeft < 0) xLeft = 0;
    if (xRight >= FS_W) xRight = FS_W - 1;
    if (xLeft > xRight) return;

    bLeft  = xLeft >> 3;
    bRight = xRight >> 3;
    leftMask  = (unsigned char)(0xFF >> (xLeft & 7));
    rightMask = (unsigned char)(0xFF << (7 - (xRight & 7)));

    if (bLeft == bRight) {
        unsigned char mask = leftMask & rightMask;
        rowPtr[bLeft] = (rowPtr[bLeft] & ~mask) | (patByte & mask);
    } else {
        rowPtr[bLeft] = (rowPtr[bLeft] & ~leftMask) | (patByte & leftMask);
        for (b = bLeft + 1; b < bRight; b++) {
            rowPtr[b] = patByte;
            __asm__ volatile ("" : : : "memory");
        }
        rowPtr[bRight] = (rowPtr[bRight] & ~rightMask) | (patByte & rightMask);
    }
}

/* ============================================================================
 * MODE 1: Winamp Classic 16-Band Spectrum + Dual Scope + Water Reflection
 * ============================================================================ */

static void RenderMode1WinampEQ(unsigned char *bits)
{
    short x, y, b;
    short prevY = 27 + gWave200[0];

    AsmClearBlack5200(bits);

    for (x = 6; x < 194; x += 4) {
        SetPixelWhite(bits, x, 27);
    }

    for (x = 4; x < 196; x++) {
        short curY = 27 + gWave200[x];
        short yMin = (curY < prevY) ? curY : prevY;
        short yMax = (curY > prevY) ? curY : prevY;

        if (yMin < 4)  yMin = 4;
        if (yMax > 50) yMax = 50;

        DrawVSpanWhite(bits, x, yMin, yMax);
        if ((x & 1) == (short)(gFrameCount & 1)) {
            if (yMin > 5)  SetPixelWhite(bits, x, yMin - 2);
            if (yMax < 49) SetPixelWhite(bits, x, yMax + 2);
        }
        prevY = curY;
    }

    for (b = 0; b < 16; b++) {
        short x0    = 5 + b * 12;
        short x1    = x0 + 9;
        short barH  = gEqBands[b];
        short peakY = 152 - gEqPeaks[b];
        short dy;
        short mod3  = 0;

        for (dy = 0; dy < barH; dy++) {
            unsigned char *rowPtr;
            unsigned char pat;
            if (mod3 == 2) {
                mod3 = 0;
                continue;
            }
            mod3++;

            y = 152 - dy;
            if (y < 54) break;
            rowPtr = bits + gRowOff200[y];

            if (dy >= 66) {
                pat = 0x00;
            } else if (dy >= 42) {
                pat = (y & 1) ? 0x22 : 0x88;
            } else if (dy >= 18) {
                pat = (y & 1) ? 0x55 : 0xAA;
            } else {
                pat = (y & 1) ? 0x77 : 0xDD;
            }

            FillSpanPattern(rowPtr, x0, x1, pat);
            SetPixelWhite(bits, x0, y);
            SetPixelWhite(bits, x1, y);
        }

        if (peakY >= 54 && peakY <= 151) {
            FillSpanPattern(bits + gRowOff200[peakY], x0, x1, 0x00);
            if (peakY > 54) {
                FillSpanPattern(bits + gRowOff200[peakY - 1], x0, x1, 0x00);
            }
        }

        {
            short refH = (barH * 26) >> 6; /* fast approx of (barH * 2) / 5 */
            if (refH > 38) refH = 38;
            for (dy = 1; dy <= refH; dy += 2) {
                short ry = 155 + dy;
                short ripple = FAST_SIN((unsigned char)(dy * 14 + gFrameCount * 12 + b * 19)) >> 6;
                short rx0 = x0 + ripple;
                short rx1 = x1 + ripple;
                unsigned char refPat = (dy > (refH >> 1)) ? 0xEE : ((ry & 1) ? 0xAA : 0xCC);
                if (ry < 196) {
                    FillSpanPattern(bits + gRowOff200[ry], rx0, rx1, refPat);
                }
            }
        }
    }

    FillSpanPattern(bits + gRowOff200[154], 2, 197, 0x55);
}

/* ============================================================================
 * MODE 2: Geiss / Amiga Demoscene 64-Shade Dithered Plasma & Metaballs (F2 Optimized)
 * ============================================================================ */

static void RenderMode2Plasma(unsigned char *bits)
{
    short x, yh;
    unsigned char baseRow[100];
    unsigned char rowVals[100];
    unsigned char waveX[100];
    unsigned char t1 = (unsigned char)(gFrameCount * 3);
    unsigned char t2 = (unsigned char)(gFrameCount * 5);
    unsigned char t3 = (unsigned char)(gFrameCount * 2);
    unsigned char beatShift = (unsigned char)(gBassPulse << 1);

    short bx0 = 50 + ((FAST_SIN(t1) * 32) >> 7);
    short by0 = 100 + ((FAST_COS(t2) * 64) >> 7);
    short bx1 = 50 + ((FAST_COS((unsigned char)(t3 + 80)) * 34) >> 7);
    short by1 = 100 + ((FAST_SIN((unsigned char)(t1 + 140)) * 68) >> 7);

    for (x = 0; x < 100; x++) {
        waveX[x] = (unsigned char)(
            (FAST_SIN((unsigned char)(x * 5 + t1)) >> 1) +
            (FAST_COS((unsigned char)(x * 9 - t2)) >> 2)
        );
    }

    /* F2: Compute radial metaball distance sum once per yHalf (0..99), then apply wy for 2*yh and 2*yh+1 */
    for (yh = 0; yh < 100; yh++) {
        short dy0 = abs(yh - (by0 >> 1));
        short dy1 = abs(yh - (by1 >> 1));
        const unsigned char *rRow0 = gRadialDist ? (gRadialDist + dy0 * 100) : NULL;
        const unsigned char *rRow1 = gRadialDist ? (gRadialDist + dy1 * 100) : NULL;
        short sub;

        if (rRow0 && rRow1) {
            for (x = 0; x < 100; x++) {
                short dx0 = abs(x - bx0);
                short dx1 = abs(x - bx1);
                baseRow[x] = (unsigned char)(waveX[x] + rRow0[dx0] - rRow1[dx1]);
            }
        } else {
            for (x = 0; x < 100; x++) {
                baseRow[x] = (unsigned char)(waveX[x] + x * 3);
            }
        }

        for (sub = 0; sub < 2; sub++) {
            short y = (yh << 1) + sub;
            unsigned char *dstRow = bits + gRowOff200[y];
            unsigned char wy = (unsigned char)(
                (FAST_SIN((unsigned char)(y * 3 - t2)) >> 1) +
                (FAST_COS((unsigned char)(y * 5 + t3)) >> 2) +
                beatShift
            );
            short ry = y & 7;

            for (x = 0; x < 100; x++) {
                rowVals[x] = gPlasmaLut[(unsigned char)(baseRow[x] + wy)];
            }

            AsmPackDitherRow25(
                dstRow,
                rowVals,
                gDitherP0[ry],
                gDitherP1[ry],
                gDitherP2[ry],
                gDitherP3[ry]
            );
            dstRow[25] = 0xFF;
        }
    }

    {
        short prevY = 100 + (gWave200[0] * 2);
        for (x = 0; x < VIZ_W; x++) {
            short curY = 100 + (gWave200[x] * 2);
            short yMin = (curY < prevY) ? curY : prevY;
            short yMax = (curY > prevY) ? curY : prevY;
            if (yMin < 4)   yMin = 4;
            if (yMax > 195) yMax = 195;

            SetPixelBlack(bits, x, yMin - 2);
            SetPixelBlack(bits, x, yMin - 1);
            SetPixelBlack(bits, x, yMax + 1);
            SetPixelBlack(bits, x, yMax + 2);

            DrawVSpanWhite(bits, x, yMin, yMax + 1);
            prevY = curY;
        }
    }
}

/* ============================================================================
 * MODE 3: Supercharged Audio-Reactive 3D Shaded Torus + Starfield + Cyber Grid (F8 Optimized)
 * ============================================================================ */

static void FillQuadDithered(unsigned char *bits, const FaceQuad2D *q)
{
    short yMin = 199, yMax = 0;
    short i, y;

    for (i = 0; i < 4; i++) {
        if (q->sy[i] < yMin) yMin = q->sy[i];
        if (q->sy[i] > yMax) yMax = q->sy[i];
    }
    if (yMin < 1)   yMin = 1;
    if (yMax > 198) yMax = 198;
    if (yMin > yMax) return;

    for (y = yMin; y <= yMax; y++) {
        short xLeft = 200, xRight = -1;
        for (i = 0; i < 4; i++) {
            short j = (i + 1) & 3;
            short y0 = q->sy[i], y1 = q->sy[j];
            short x0 = q->sx[i], x1 = q->sx[j];
            if ((y0 <= y && y1 >= y) || (y1 <= y && y0 >= y)) {
                if (y0 == y1) {
                    if (x0 < xLeft)  xLeft  = x0;
                    if (x0 > xRight) xRight = x0;
                    if (x1 < xLeft)  xLeft  = x1;
                    if (x1 > xRight) xRight = x1;
                } else {
                    short xInt = x0 + DivS16(MulS16(y - y0, x1 - x0), y1 - y0);
                    if (xInt < xLeft)  xLeft  = xInt;
                    if (xInt > xRight) xRight = xInt;
                }
            }
        }
        if (xLeft <= xRight) {
            unsigned char pat = gShadeByte[q->shade][y & 3];
            FillSpanPattern(bits + gRowOff200[y], xLeft, xRight, pat);
        }
    }
}

static void RenderMode3Demoscene3D(unsigned char *bits)
{
    short i, r, s;
    short vz[TORUS_VERTS];
    short px[TORUS_VERTS], py[TORUS_VERTS];
    short visCount = 0;
    short sxA, cxA, syA, cyA, szA, czA;
    short starSpeed;
    short targetZoom, fovScale, baseMajorR;

    AsmClearBlack5200(bits);

    /* 1. 3D Warp Starfield */
    starSpeed = 2 + (gAudioEnergy >> 3) + (gBassPulse >> 2);
    for (i = 0; i < NUM_STARS; i++) {
        short projX, projY;
        gStars[i].z -= starSpeed;
        if (gStars[i].z <= 16) {
            gStars[i].z += 220;
        }
        projX = 100 + DivS16((long)gStars[i].x << 7, gStars[i].z);
        projY = 92  + DivS16((long)gStars[i].y << 7, gStars[i].z);
        if ((unsigned short)projX < VIZ_W && (unsigned short)projY < 162) {
            SetPixelWhite(bits, projX, projY);
            if (gStars[i].z < 120 && projX + 1 < VIZ_W) {
                SetPixelWhite(bits, projX + 1, projY);
            }
        }
    }

    /* 2. Perspective 3D Scrolling Cyber-Grid Floor (y = 164..198) */
    {
        short scroll = (short)((gFrameCount * (1 + (gAudioEnergy >> 4))) & 7);
        FillSpanPattern(bits + gRowOff200[164], 0, 199, (gBassPulse > 24) ? 0x00 : 0x55);
        for (i = 1; i <= 5; i++) {
            short gy = 164 + ((i * i * 28 + scroll * i) >> 2);
            if (gy > 164 && gy < 199) {
                FillSpanPattern(bits + gRowOff200[gy], 0, 199, (i >= 4) ? 0x55 : 0xAA);
            }
        }
        for (i = -5; i <= 5; i++) {
            short sway = (FAST_SIN((unsigned char)(gFrameCount * 3)) * gBassPulse) >> 9;
            short topX = 100 + i * 10 + sway;
            short botX = 100 + i * 34 + (sway * 2);
            DrawLineWhite(bits, topX, 164, botX, 199);
        }
    }

    /* 3. Smooth Audio-Driven Rotation & Balanced Bass Zoom */
    if (gBeatHit) {
        gAngleX = (unsigned char)(gAngleX + 2);
        gAngleY = (unsigned char)(gAngleY + 3);
    }
    gAngleX = (unsigned char)(gAngleX + 1 + (gBassPulse >> 4));
    gAngleY = (unsigned char)(gAngleY + 2 + (gAudioEnergy >> 5));
    gAngleZ = (unsigned char)(gAngleZ + 1 + (gTreblePulse >> 5));
    gTorusTwistPhase += 3 + (gAudioEnergy >> 4);

    targetZoom = (gBassPulse >> 1) + (gAudioEnergy >> 2);
    gTorusZoomSmooth = (gTorusZoomSmooth * 3 + targetZoom) >> 2;

    fovScale   = 165 + (gTorusZoomSmooth >> 1);
    baseMajorR = 38 + (gTorusZoomSmooth >> 3);

    sxA = FAST_SIN(gAngleX); cxA = FAST_COS(gAngleX);
    syA = FAST_SIN(gAngleY); cyA = FAST_COS(gAngleY);
    szA = FAST_SIN(gAngleZ); czA = FAST_COS(gAngleZ);

    /* 4. Generate & Transform 48 Audio-Sculpted Torus Vertices */
    for (r = 0; r < TORUS_RINGS; r++) {
        unsigned char uAngle = (unsigned char)(r * (256 / TORUS_RINGS));
        short cosU = FAST_COS(uAngle);
        short sinU = FAST_SIN(uAngle);
        short bandLow  = gEqBands[r * 2];
        short bandHigh = gEqBands[r * 2 + 1];
        short minorR = 12 + (bandLow >> 2);
        short ringMajorR = baseMajorR + ((bandHigh - 24) >> 3);
        short ringWaveZ  = (short)(MulS16(bandLow - 20, FAST_SIN((unsigned char)(uAngle * 2 + gTorusTwistPhase))) >> 8);

        for (s = 0; s < TORUS_SEGS; s++) {
            short idx = r * TORUS_SEGS + s;
            unsigned char vAngle = (unsigned char)(s * (256 / TORUS_SEGS) + (bandHigh >> 2));
            short cosV = FAST_COS(vAngle);
            short sinV = FAST_SIN(vAngle);
            short segBump = (s & 1) ? (gBassPulse >> 4) : -(gTreblePulse >> 5);
            short curMinor = minorR + segBump;
            short ringR = ringMajorR + (short)(MulS16(curMinor, cosV) >> 7);

            short mx = (short)(MulS16(ringR, cosU) >> 7);
            short my = (short)(MulS16(ringR, sinU) >> 7);
            short mz = (short)(MulS16(curMinor, sinV) >> 7) + ringWaveZ;

            short y1 = (short)((MulS16(my, cxA) - MulS16(mz, sxA)) >> 7);
            short z1 = (short)((MulS16(my, sxA) + MulS16(mz, cxA)) >> 7);

            short x2 = (short)((MulS16(mx, cyA) + MulS16(z1, syA)) >> 7);
            short z2 = (short)((-MulS16(mx, syA) + MulS16(z1, cyA)) >> 7);

            short x3 = (short)((MulS16(x2, czA) - MulS16(y1, szA)) >> 7);
            short y3 = (short)((MulS16(x2, szA) + MulS16(y1, czA)) >> 7);

            short dist = z2 + 220;
            if (dist < 70) dist = 70;

            vz[idx] = z2;
            px[idx] = 100 + DivS16(MulS16(x3, fovScale), dist);
            py[idx] = 86  + DivS16(MulS16(y3, fovScale), dist);
        }
    }

    /* 5. Assemble Quads, Backface Cull, and Compute Lambertian Lighting */
    for (r = 0; r < TORUS_RINGS; r++) {
        short nextR = (r + 1) & (TORUS_RINGS - 1);
        for (s = 0; s < TORUS_SEGS; s++) {
            short nextS = (s + 1) % TORUS_SEGS;
            short i0 = r * TORUS_SEGS + s;
            short i1 = nextR * TORUS_SEGS + s;
            short i2 = nextR * TORUS_SEGS + nextS;
            short i3 = r * TORUS_SEGS + nextS;

            short dx1 = px[i1] - px[i0];
            short dy1 = py[i1] - py[i0];
            short dx2 = px[i3] - px[i0];
            short dy2 = py[i3] - py[i0];
            long crossZ = MulS16(dx1, dy2) - MulS16(dy1, dx2);

            if (crossZ > 0) {
                FaceQuad2D *f = &gVisibleFaces[visCount++];
                short shade;
                f->sx[0] = px[i0]; f->sy[0] = py[i0];
                f->sx[1] = px[i1]; f->sy[1] = py[i1];
                f->sx[2] = px[i2]; f->sy[2] = py[i2];
                f->sx[3] = px[i3]; f->sy[3] = py[i3];
                f->avgZ  = (vz[i0] + vz[i1] + vz[i2] + vz[i3]) >> 2;

                shade = (short)(crossZ >> 4) + ((44 - f->avgZ) >> 3) + (gBassPulse >> 3);
                if (shade < 2)  shade = 2;
                if (shade > 16) shade = 16;
                f->shade = (unsigned char)shade;
            }
        }
    }

    /* 6. Painter's Algorithm Depth Sort */
    for (i = 1; i < visCount; i++) {
        FaceQuad2D key = gVisibleFaces[i];
        short j = i - 1;
        while (j >= 0 && gVisibleFaces[j].avgZ < key.avgZ) {
            gVisibleFaces[j + 1] = gVisibleFaces[j];
            j--;
        }
        gVisibleFaces[j + 1] = key;
    }

    /* 7. Rasterize Dither-Shaded Quads + Crisp White Wireframe Specular Edges */
    for (i = 0; i < visCount; i++) {
        FaceQuad2D *f = &gVisibleFaces[i];
        FillQuadDithered(bits, f);
        DrawLineWhite(bits, f->sx[0], f->sy[0], f->sx[1], f->sy[1]);
        DrawLineWhite(bits, f->sx[1], f->sy[1], f->sx[2], f->sy[2]);
        DrawLineWhite(bits, f->sx[2], f->sy[2], f->sx[3], f->sy[3]);
        DrawLineWhite(bits, f->sx[3], f->sy[3], f->sx[0], f->sy[0]);
    }
}

/* ============================================================================
 * MODE 4: MilkDrop 3D Wormhole Vortex Tunnel + Radial Spectrum Burst (F2 Optimized)
 * ============================================================================ */

static void RenderMode4Tunnel(unsigned char *bits)
{
    short x, yh, b;
    unsigned char rowVals[100];
    unsigned char eqMod[16];
    unsigned char depthScroll = (unsigned char)(gFrameCount * 5 + (gBassPulse >> 1));
    unsigned char twistScroll = (unsigned char)(gFrameCount * 2);

    short camOx = 14 + ((FAST_SIN((unsigned char)(gFrameCount * 2)) * 12) >> 7);
    short camOy = 14 + ((FAST_COS((unsigned char)(gFrameCount * 3)) * 12) >> 7);

    if (!gTunnelAngle || !gTunnelDepth || !gTunnelFog) {
        RenderMode2Plasma(bits);
        return;
    }

    for (b = 0; b < 16; b++) {
        eqMod[b] = (unsigned char)(gEqBands[b] >> 1);
    }

    /* F2: Compute rowVals once per yh (0..99) and pack into both y0=2*yh and y1=2*yh+1 */
    for (yh = 0; yh < 100; yh++) {
        short lutY = yh + camOy;
        unsigned short rowBase = (unsigned short)((lutY << 7) + camOx);
        const unsigned char *angPtr = gTunnelAngle + rowBase;
        const unsigned char *depPtr = gTunnelDepth + rowBase;
        const unsigned char *fogPtr = gTunnelFog   + rowBase;
        short y0 = yh << 1;
        short ry0 = y0 & 7;
        short ry1 = ry0 + 1;
        unsigned char *dstRow0 = bits + gRowOff200[y0];
        unsigned char *dstRow1 = dstRow0 + VIZ_ROWBYTES;

        for (x = 0; x < 100; x++) {
            unsigned char ang = angPtr[x];
            unsigned char dep = depPtr[x];
            unsigned char fog = fogPtr[x];
            unsigned char bandVal = eqMod[(ang >> 4) & 15];

            unsigned char u = (unsigned char)(ang * 2 + (dep >> 1) + twistScroll);
            unsigned char v = (unsigned char)(dep + depthScroll - bandVal);
            unsigned char tex = gPlasmaLut[(unsigned char)(u ^ v)] + (bandVal >> 1);
            unsigned short lit = (unsigned short)(tex * fog) >> 6;

            if (lit > 63) lit = 63;
            rowVals[x] = (unsigned char)lit;
        }

        AsmPackDitherRow25(dstRow0, rowVals, gDitherP0[ry0], gDitherP1[ry0], gDitherP2[ry0], gDitherP3[ry0]);
        dstRow0[25] = 0xFF;
        AsmPackDitherRow25(dstRow1, rowVals, gDitherP0[ry1], gDitherP1[ry1], gDitherP2[ry1], gDitherP3[ry1]);
        dstRow1[25] = 0xFF;
    }

    {
        short eyeX = (64 - camOx) * 2;
        short eyeY = (64 - camOy) * 2;
        for (b = 0; b < 16; b++) {
            unsigned char rayAng = (unsigned char)(b * 16 + gFrameCount * 2);
            short cs = FAST_COS(rayAng);
            short sn = FAST_SIN(rayAng);
            short rInner = 10;
            short rOuter = 16 + (gEqBands[b] >> 1) + (gBassPulse >> 2);
            short x0 = eyeX + ((rInner * cs) >> 7);
            short y0 = eyeY + ((rInner * sn) >> 7);
            short x1 = eyeX + ((rOuter * cs) >> 7);
            short y1 = eyeY + ((rOuter * sn) >> 7);
            DrawLineWhite(bits, x0, y0, x1, y1);
        }
    }
}

/* ============================================================================
 * MODE 5: Demoscene Cellular Fire & Smoke Diffusion + 3D Harmonograph Ribbon (F1/F10 Optimized)
 * ============================================================================ */

static void RenderMode5FireScope(unsigned char *bits)
{
    short x, y, i;

    if (!gFireBuf) {
        RenderMode2Plasma(bits);
        return;
    }

    /* 1. Inject 16-Band EQ Fire Coal Intensity along the bottom rows (y = 98..100) */
    for (x = 1; x < 99; x++) {
        short b = kBand100[x];
        short coal = (gEqBands[b] * 5) >> 3;
        short noise = (FAST_SIN((unsigned char)(x * 29 + gFrameCount * 17)) + 128) >> 4;
        short val = coal + noise + (gBassPulse >> 1);
        if (val > 63) val = 63;
        gFireBuf[99 * 100 + x]  = (unsigned char)val;
        gFireBuf[100 * 100 + x] = (unsigned char)val;
    }

    /* 2. Inject Glowing 3D Rotating Lissajous Harmonograph Figure into the Fire Field */
    {
        unsigned char pA = (unsigned char)(gFrameCount * 4);
        unsigned char pB = (unsigned char)(gFrameCount * 7 + 40);
        short radX = 24 + (gBassPulse >> 2);
        short radY = 22 + (gAudioEnergy >> 3);

        for (i = 0; i < 64; i++) {
            unsigned char t = (unsigned char)(i * 4);
            short waveMod = gWave200[(i * 3) % VIZ_W];
            short lx = 50 + ((FAST_SIN((unsigned char)(t * 2 + pA)) * radX) >> 7) + (waveMod >> 2);
            short ly = 54 + ((FAST_COS((unsigned char)(t * 3 + pB)) * radY) >> 7) + (waveMod >> 2);
            if (lx >= 2 && lx < 98 && ly >= 4 && ly < 96) {
                gFireBuf[ly * 100 + lx]       = 63;
                gFireBuf[ly * 100 + lx + 1]   = 58;
                gFireBuf[(ly + 1) * 100 + lx] = 58;
            }
        }
    }

    /* 3. Run 68000 Assembly Cellular Upward Diffusion & Cooling Pass */
    for (y = 0; y < 99; y++) {
        AsmDiffuseFireRow98(gFireBuf + y * 100 + 1, gFireBuf + (y + 1) * 100);
    }

    /* 4. Pack 100x100 Fire Field into 200x200 64-Shade 8x8 Bayer Dithered Output */
    for (y = 0; y < VIZ_H; y++) {
        unsigned char *dstRow = bits + gRowOff200[y];
        const unsigned char *fireRow = gFireBuf + (y >> 1) * 100;
        short ry = y & 7;
        AsmPackDitherRow25(
            dstRow,
            fireRow,
            gDitherP0[ry],
            gDitherP1[ry],
            gDitherP2[ry],
            gDitherP3[ry]
        );
        dstRow[25] = 0xFF;
    }
}

/* ============================================================================
 * MODE 6: Real-Time Ray-Cast 3D Reflective Chrome Boing Spheres (F1, F2, F3 Optimized!)
 * ============================================================================ */

static void RenderMode6ChromeSpheres(unsigned char *bits)
{
    short x, yh, i;
    unsigned char rowVals[100];
    unsigned char skyThr[100];
    ChromeSphere spheres[3];
    short shadowCenterY[3];
    short bounce = abs(FAST_SIN((unsigned char)(gFrameCount * 5))) >> 2;
    short floorScrollZ = (short)(gFrameCount * 4 + (gAudioEnergy >> 3));
    short floorScrollX = (FAST_SIN((unsigned char)(gFrameCount * 2)) * 24) >> 7;
    unsigned char orbitAng = (unsigned char)(gFrameCount * 4);

    if (!gSphereNz || !gSphereEnvU || !gSphereEnvV || !gSphereSpec) {
        RenderMode3Demoscene3D(bits);
        return;
    }

    spheres[0].cx       = 50 + ((FAST_SIN((unsigned char)(gFrameCount * 2)) * 10) >> 7);
    spheres[0].cy       = 48 - (bounce >> 1) - (gBassPulse >> 2);
    spheres[0].cz       = 100;
    spheres[0].radiusPx = 22 + (gBassPulse >> 3) + (gAudioEnergy >> 4);
    if (spheres[0].radiusPx > 30) spheres[0].radiusPx = 30;
    spheres[0].scaleInv = DivS16(30 * 64, spheres[0].radiusPx);
    spheres[0].spin     = (unsigned char)(gFrameCount * 3);

    spheres[1].cx       = 50 + ((FAST_COS(orbitAng) * 34) >> 7);
    spheres[1].cz       = 100 + ((FAST_SIN(orbitAng) * 40) >> 7);
    spheres[1].cy       = 46 + ((FAST_SIN((unsigned char)(orbitAng * 2)) * 12) >> 7) - (gEqBands[2] >> 3);
    spheres[1].radiusPx = (spheres[1].cz < 100) ? 16 : 11;
    spheres[1].scaleInv = (spheres[1].cz < 100) ? (30 * 64 / 16) : (30 * 64 / 11);
    spheres[1].spin     = (unsigned char)(-gFrameCount * 5);

    spheres[2].cx       = 50 - ((FAST_COS(orbitAng) * 34) >> 7);
    spheres[2].cz       = 100 - ((FAST_SIN(orbitAng) * 40) >> 7);
    spheres[2].cy       = 46 - ((FAST_SIN((unsigned char)(orbitAng * 2)) * 12) >> 7) - (gEqBands[11] >> 3);
    spheres[2].radiusPx = (spheres[2].cz < 100) ? 16 : 11;
    spheres[2].scaleInv = (spheres[2].cz < 100) ? (30 * 64 / 16) : (30 * 64 / 11);
    spheres[2].spin     = (unsigned char)(gFrameCount * 6);

    for (i = 0; i < 2; i++) {
        short j;
        for (j = i + 1; j < 3; j++) {
            if (spheres[i].cz < spheres[j].cz) {
                ChromeSphere tmp = spheres[i];
                spheres[i] = spheres[j];
                spheres[j] = tmp;
            }
        }
    }

    /* F1: Precompute sky EQ aurora thresholds once per frame (0 divides per pixel!) */
    for (x = 0; x < 100; x++) {
        skyThr[x] = (unsigned char)(54 - (gEqBands[kBand100[x]] >> 2));
    }
    for (i = 0; i < 3; i++) {
        shadowCenterY[i] = 72 + ((120 - spheres[i].cz) >> 2);
    }

    /* F2: Compute each yh (0..99) ONCE and pack into both y0 = 2*yh and y1 = 2*yh+1! */
    for (yh = 0; yh < 100; yh++) {
        short y0  = yh << 1;
        short ry0 = y0 & 7;
        short ry1 = ry0 + 1;
        unsigned char *dstRow0 = bits + gRowOff200[y0];
        unsigned char *dstRow1 = dstRow0 + VIZ_ROWBYTES;

        if (yh < 56) {
            short skyBase = (yh > 36) ? ((yh - 36) * 2) : (yh >> 2);
            short skyGlow = skyBase + 26;
            if (skyBase > 52) skyBase = 52;
            if (skyGlow > 52) skyGlow = 52;
            for (x = 0; x < 100; x++) {
                rowVals[x] = (unsigned char)((yh > skyThr[x]) ? skyGlow : skyBase);
            }
        } else {
            /* F3: Incremental floor accumulator + per-row sphere shadow spans */
            short dyFloor    = yh - 54;
            short zDist      = DivS16(640, dyFloor);
            short zTile      = (zDist + floorScrollZ) >> 4;
            short fogAtten   = (dyFloor > 32) ? 64 : (dyFloor * 2);
            unsigned char whiteShade = (unsigned char)((50 * fogAtten) >> 6);
            unsigned char darkShade  = (unsigned char)((12 * fogAtten) >> 6);
            short acc = -50 * zDist;

            for (x = 0; x < 100; x++) {
                short xWorld = (acc >> 5) + floorScrollX;
                short xTile  = xWorld >> 4;
                acc += zDist;
                rowVals[x] = ((xTile ^ zTile) & 1) ? whiteShade : darkShade;
            }

            for (i = 0; i < 3; i++) {
                short rPx = spheres[i].radiusPx;
                if (abs(yh - shadowCenterY[i]) < (rPx >> 1)) {
                    short sx0 = spheres[i].cx - rPx + 1;
                    short sx1 = spheres[i].cx + rPx - 1;
                    if (sx0 < 0)  sx0 = 0;
                    if (sx1 > 99) sx1 = 99;
                    for (x = sx0; x <= sx1; x++) {
                        rowVals[x] >>= 2;
                    }
                }
            }
        }

        /* 2. Ray-Cast & Environment-Map the 3 3D Chrome Boing Spheres */
        for (i = 0; i < 3; i++) {
            short dy  = yh - spheres[i].cy;
            short rPx = spheres[i].radiusPx;
            if (abs(dy) < rPx) {
                short lutY = 32 + ((dy * spheres[i].scaleInv) >> 6);
                if ((unsigned short)lutY < SPHERE_LUT_DIM) {
                    short rowOff = lutY << 6;
                    short xStart = spheres[i].cx - rPx;
                    short xEnd   = spheres[i].cx + rPx;
                    if (xStart < 0) xStart = 0;
                    if (xEnd > 99)  xEnd = 99;

                    for (x = xStart; x <= xEnd; x++) {
                        short dx = x - spheres[i].cx;
                        short lutX = 32 + ((dx * spheres[i].scaleInv) >> 6);
                        if ((unsigned short)lutX < SPHERE_LUT_DIM) {
                            short sIdx = rowOff + lutX;
                            signed char nz = gSphereNz[sIdx];
                            if (nz > 0) {
                                unsigned char u = (unsigned char)(gSphereEnvU[sIdx] + spheres[i].spin);
                                unsigned char v = (unsigned char)(gSphereEnvV[sIdx] + (gBassPulse >> 1));
                                Boolean boingFacet = (((u >> 5) ^ (v >> 5)) & 1) != 0;
                                short chromeRefl = gPlasmaLut[(unsigned char)(u * 2 + v + floorScrollZ)] >> 1;
                                short shade = (boingFacet ? 26 : 6) + chromeRefl + gSphereSpec[sIdx];
                                if (nz < 18) shade += 18;
                                if (shade > 63) shade = 63;
                                rowVals[x] = (unsigned char)shade;
                            }
                        }
                    }
                }
            }
        }

        AsmPackDitherRow25(dstRow0, rowVals, gDitherP0[ry0], gDitherP1[ry0], gDitherP2[ry0], gDitherP3[ry0]);
        dstRow0[25] = 0xFF;
        AsmPackDitherRow25(dstRow1, rowVals, gDitherP0[ry1], gDitherP1[ry1], gDitherP2[ry1], gDitherP3[ry1]);
        dstRow1[25] = 0xFF;
    }
}

/* ============================================================================
 * MODE 7: 3D Voxel Space Audio Canyon Flyover (F4 & F5 Optimized!)
 * ============================================================================ */

static void RenderMode7VoxelCanyon(unsigned char *bits)
{
    short x, y, b, step, h;
    unsigned char yBuf[100];
    short tilt[100];
    short projH[64];
    unsigned char *colBuf = gFireBuf;
    short bankTilt;
    short camX, camY, camHeight;
    short sunRadius, sunRadSq;

    if (!gVoxelHeight || !gVoxelShade || !colBuf) {
        RenderMode4Tunnel(bits);
        return;
    }

    gVoxelWriteRow = (unsigned char)((gVoxelWriteRow + 1) & (VOXEL_MAP_DIM - 1));
    {
        unsigned char *hRow = gVoxelHeight + (gVoxelWriteRow << 6);
        unsigned char *sRow = gVoxelShade  + (gVoxelWriteRow << 6);
        unsigned char prevRowIdx = (unsigned char)((gVoxelWriteRow - 1) & (VOXEL_MAP_DIM - 1));
        const unsigned char *prevH = gVoxelHeight + (prevRowIdx << 6);

        for (x = 0; x < VOXEL_MAP_DIM; x++) {
            short distCenter = abs(x - 32);
            short bandIdx = (x >> 2) & 15;
            short eqH = (gEqBands[bandIdx] * 3) >> 3;
            short ridge = (distCenter > 8) ? ((distCenter - 8) * 2) : 0;
            short wave  = (FAST_SIN((unsigned char)(x * 11 + gFrameCount * 7)) + 128) >> 4;
            short hVal = (prevH[x] + ridge + eqH + wave) >> 1;
            short slope;
            short shade;

            if (hVal > 62) hVal = 62;
            hRow[x] = (unsigned char)hVal;

            slope = (x > 0) ? (hVal - hRow[x - 1]) : 0;
            shade = 18 + (hVal >> 1) + (slope * 3);
            if ((x & 3) == 0 || (gVoxelWriteRow & 3) == 0) {
                shade += 12;
            }
            if (shade < 4)  shade = 4;
            if (shade > 63) shade = 63;
            sRow[x] = (unsigned char)shade;
        }
    }

    sunRadius = 16 + (gBassPulse >> 3);
    sunRadSq  = sunRadius * sunRadius;
    for (y = 0; y < 100; y++) {
        unsigned char *row = colBuf + y * 100;
        short dySun = y - 24;
        short skyShade = (y < 48) ? (y >> 2) : 12;
        memset(row, skyShade, 100);

        if (abs(dySun) < sunRadius && (dySun < 2 || (y & 1) == 0)) {
            short dySunSq = dySun * dySun;
            unsigned char sunCol = (unsigned char)(58 - (dySun > 0 ? dySun : 0));
            for (x = 50 - sunRadius; x <= 50 + sunRadius; x++) {
                short dxSun = x - 50;
                if (dxSun * dxSun + dySunSq < sunRadSq) {
                    row[x] = sunCol;
                }
            }
        }
    }

    camX      = (32 << 8) + (FAST_SIN((unsigned char)(gFrameCount * 2)) * 18);
    camY      = ((short)gVoxelWriteRow - 34) & (VOXEL_MAP_DIM - 1);
    camHeight = 52 + (gBassPulse >> 2);
    bankTilt  = (FAST_COS((unsigned char)(gFrameCount * 3)) * 14) >> 7;

    /* F4: Precompute per-column tilt once per frame and per-height projection once per step! */
    for (x = 0; x < 100; x++) {
        yBuf[x] = 100;
        tilt[x] = ((x - 50) * bankTilt) >> 5;
    }

    for (step = 3; step < 34; step++) {
        short mapY = (camY + step) & (VOXEL_MAP_DIM - 1);
        const unsigned char *hRow = gVoxelHeight + (mapY << 6);
        const unsigned char *sRow = gVoxelShade  + (mapY << 6);
        short invZ = DivS16(280, step);
        short fog  = (step > 20) ? (36 - step) : 16;

        unsigned short worldX = (unsigned short)(camX - step * 38);
        unsigned short stepX  = (unsigned short)DivS16(step * 76, 100);

        for (h = 0; h < 64; h++) {
            projH[h] = 44 + (short)((MulS16(camHeight - h, invZ)) >> 5);
        }

        for (x = 0; x < 100; x++) {
            short mx = (short)((worldX >> 8) & (VOXEL_MAP_DIM - 1));
            short projY = tilt[x] + projH[hRow[mx]];

            if (projY < 0) projY = 0;
            if (projY < yBuf[x]) {
                unsigned char shade = (unsigned char)(((unsigned short)sRow[mx] * (unsigned short)fog) >> 4);
                unsigned char *pCol = colBuf + projY * 100 + x;
                short span = yBuf[x] - projY;
                while (span-- > 0) {
                    *pCol = shade;
                    pCol += 100;
                }
                yBuf[x] = (unsigned char)projY;
            }
            worldX += stepX;
        }
    }

    for (y = 0; y < 100; y++) {
        short y0  = y << 1;
        short ry0 = y0 & 7;
        short ry1 = ry0 + 1;
        unsigned char *dstRow0 = bits + gRowOff200[y0];
        unsigned char *dstRow1 = dstRow0 + VIZ_ROWBYTES;
        const unsigned char *srcRow = colBuf + y * 100;
        AsmPackDitherRow25(dstRow0, srcRow, gDitherP0[ry0], gDitherP1[ry0], gDitherP2[ry0], gDitherP3[ry0]);
        dstRow0[25] = 0xFF;
        AsmPackDitherRow25(dstRow1, srcRow, gDitherP0[ry1], gDitherP1[ry1], gDitherP2[ry1], gDitherP3[ry1]);
        dstRow1[25] = 0xFF;
    }

    for (b = 0; b < 16; b++) {
        short hx = 22 + b * 10;
        short hh = gEqBands[b] >> 3;
        DrawLineWhite(bits, hx, 12, hx, 12 - hh);
    }
}

/* ============================================================================
 * FULLSCREEN VISUALIZER MODES (256x161 Internal -> 2x LUT Doubled to 512x322)
 * ============================================================================ */

static void RenderMode1WinampEQ_FS(unsigned char *bits)
{
    short x, y, b;
    short prevY = 22 + gWave200[0];

    AsmClearBlack5152(bits);

    for (x = 6; x < 250; x += 4) {
        SetPixelWhiteFS(bits, x, 22);
    }

    for (x = 4; x < 252; x++) {
        short wIdx = (x * 199) >> 8;
        short curY = 22 + gWave200[wIdx];
        short yMin = (curY < prevY) ? curY : prevY;
        short yMax = (curY > prevY) ? curY : prevY;

        if (yMin < 3)  yMin = 3;
        if (yMax > 42) yMax = 42;

        DrawVSpanWhiteFS(bits, x, yMin, yMax);
        if ((x & 1) == (short)(gFrameCount & 1)) {
            if (yMin > 4)  SetPixelWhiteFS(bits, x, yMin - 2);
            if (yMax < 41) SetPixelWhiteFS(bits, x, yMax + 2);
        }
        prevY = curY;
    }

    for (b = 0; b < 16; b++) {
        short x0    = 9 + b * 15;
        short x1    = x0 + 11;
        short barH  = (gEqBands[b] * 3) >> 2;
        short peakH = (gEqPeaks[b] * 3) >> 2;
        short peakY = 122 - peakH;
        short dy;
        short mod3  = 0;

        for (dy = 0; dy < barH; dy++) {
            unsigned char *rowPtr;
            unsigned char pat;
            if (mod3 == 2) {
                mod3 = 0;
                continue;
            }
            mod3++;

            y = 122 - dy;
            if (y < 44) break;
            rowPtr = bits + (y << 5);

            if (dy >= 50) {
                pat = 0x00;
            } else if (dy >= 32) {
                pat = (y & 1) ? 0x22 : 0x88;
            } else if (dy >= 14) {
                pat = (y & 1) ? 0x55 : 0xAA;
            } else {
                pat = (y & 1) ? 0x77 : 0xDD;
            }

            FillSpanPatternFS(rowPtr, x0, x1, pat);
            SetPixelWhiteFS(bits, x0, y);
            SetPixelWhiteFS(bits, x1, y);
        }

        if (peakY >= 44 && peakY <= 121) {
            FillSpanPatternFS(bits + (peakY << 5), x0, x1, 0x00);
            if (peakY > 44) {
                FillSpanPatternFS(bits + ((peakY - 1) << 5), x0, x1, 0x00);
            }
        }

        {
            short refH = (barH * 26) >> 6;
            if (refH > 30) refH = 30;
            for (dy = 1; dy <= refH; dy += 2) {
                short ry = 125 + dy;
                short ripple = FAST_SIN((unsigned char)(dy * 14 + gFrameCount * 12 + b * 19)) >> 6;
                short rx0 = x0 + ripple;
                short rx1 = x1 + ripple;
                unsigned char refPat = (dy > (refH >> 1)) ? 0xEE : ((ry & 1) ? 0xAA : 0xCC);
                if (ry < 158) {
                    FillSpanPatternFS(bits + (ry << 5), rx0, rx1, refPat);
                }
            }
        }
    }

    FillSpanPatternFS(bits + (124 << 5), 2, 253, 0x55);
}

static void RenderMode2Plasma_FS(unsigned char *bits)
{
    short x, yh;
    unsigned char baseRow[128];
    unsigned char rowVals[128];
    unsigned char waveX[128];
    unsigned char t1 = (unsigned char)(gFrameCount * 3);
    unsigned char t2 = (unsigned char)(gFrameCount * 5);
    unsigned char t3 = (unsigned char)(gFrameCount * 2);
    unsigned char beatShift = (unsigned char)(gBassPulse << 1);

    short bx0 = 64 + ((FAST_SIN(t1) * 40) >> 7);
    short by0 = 80 + ((FAST_COS(t2) * 52) >> 7);
    short bx1 = 64 + ((FAST_COS((unsigned char)(t3 + 80)) * 42) >> 7);
    short by1 = 80 + ((FAST_SIN((unsigned char)(t1 + 140)) * 54) >> 7);

    for (x = 0; x < 128; x++) {
        waveX[x] = (unsigned char)(
            (FAST_SIN((unsigned char)(x * 4 + t1)) >> 1) +
            (FAST_COS((unsigned char)(x * 7 - t2)) >> 2)
        );
    }

    for (yh = 0; yh <= 80; yh++) {
        short dy0 = abs(yh - (by0 >> 1));
        short dy1 = abs(yh - (by1 >> 1));
        const unsigned char *rRow0;
        const unsigned char *rRow1;
        short sub;

        if (dy0 > 99) dy0 = 99;
        if (dy1 > 99) dy1 = 99;
        rRow0 = gRadialDist ? (gRadialDist + dy0 * 100) : NULL;
        rRow1 = gRadialDist ? (gRadialDist + dy1 * 100) : NULL;

        if (rRow0 && rRow1) {
            for (x = 0; x < 128; x++) {
                short dx0 = abs(x - bx0);
                short dx1 = abs(x - bx1);
                if (dx0 > 99) dx0 = 99;
                if (dx1 > 99) dx1 = 99;
                baseRow[x] = (unsigned char)(waveX[x] + rRow0[dx0] - rRow1[dx1]);
            }
        } else {
            for (x = 0; x < 128; x++) {
                baseRow[x] = (unsigned char)(waveX[x] + x * 3);
            }
        }

        for (sub = 0; sub < 2; sub++) {
            short y = (yh << 1) + sub;
            unsigned char *dstRow;
            unsigned char wy;
            short ry;
            if (y >= FS_H) break;

            dstRow = bits + (y << 5);
            wy = (unsigned char)(
                (FAST_SIN((unsigned char)(y * 3 - t2)) >> 1) +
                (FAST_COS((unsigned char)(y * 5 + t3)) >> 2) +
                beatShift
            );
            ry = y & 7;

            for (x = 0; x < 128; x++) {
                rowVals[x] = gPlasmaLut[(unsigned char)(baseRow[x] + wy)];
            }

            AsmPackDitherRow32(dstRow, rowVals, gDitherP0[ry], gDitherP1[ry], gDitherP2[ry], gDitherP3[ry]);
        }
    }

    {
        short prevY = 80 + (gWave200[0] * 2);
        for (x = 0; x < FS_W; x++) {
            short wIdx = (x * 199) >> 8;
            short curY = 80 + (gWave200[wIdx] * 2);
            short yMin = (curY < prevY) ? curY : prevY;
            short yMax = (curY > prevY) ? curY : prevY;
            if (yMin < 4)   yMin = 4;
            if (yMax > 156) yMax = 156;

            SetPixelBlackFS(bits, x, yMin - 2);
            SetPixelBlackFS(bits, x, yMin - 1);
            SetPixelBlackFS(bits, x, yMax + 1);
            SetPixelBlackFS(bits, x, yMax + 2);

            DrawVSpanWhiteFS(bits, x, yMin, yMax + 1);
            prevY = curY;
        }
    }
}

static void FillQuadDitheredFS(unsigned char *bits, const FaceQuad2D *q)
{
    short yMin = FS_H - 1, yMax = 0;
    short i, y;

    for (i = 0; i < 4; i++) {
        if (q->sy[i] < yMin) yMin = q->sy[i];
        if (q->sy[i] > yMax) yMax = q->sy[i];
    }
    if (yMin < 1)        yMin = 1;
    if (yMax > FS_H - 2) yMax = FS_H - 2;
    if (yMin > yMax) return;

    for (y = yMin; y <= yMax; y++) {
        short xLeft = FS_W, xRight = -1;
        for (i = 0; i < 4; i++) {
            short j = (i + 1) & 3;
            short y0 = q->sy[i], y1 = q->sy[j];
            short x0 = q->sx[i], x1 = q->sx[j];
            if ((y0 <= y && y1 >= y) || (y1 <= y && y0 >= y)) {
                if (y0 == y1) {
                    if (x0 < xLeft)  xLeft  = x0;
                    if (x0 > xRight) xRight = x0;
                    if (x1 < xLeft)  xLeft  = x1;
                    if (x1 > xRight) xRight = x1;
                } else {
                    short xInt = x0 + DivS16(MulS16(y - y0, x1 - x0), y1 - y0);
                    if (xInt < xLeft)  xLeft  = xInt;
                    if (xInt > xRight) xRight = xInt;
                }
            }
        }
        if (xLeft <= xRight) {
            unsigned char pat = gShadeByte[q->shade][y & 3];
            FillSpanPatternFS(bits + (y << 5), xLeft, xRight, pat);
        }
    }
}

static void RenderMode3Demoscene3D_FS(unsigned char *bits)
{
    short i, r, s;
    short vz[TORUS_VERTS];
    short px[TORUS_VERTS], py[TORUS_VERTS];
    short visCount = 0;
    short sxA, cxA, syA, cyA, szA, czA;
    short starSpeed;
    short targetZoom, fovScale, baseMajorR;

    AsmClearBlack5152(bits);

    starSpeed = 2 + (gAudioEnergy >> 3) + (gBassPulse >> 2);
    for (i = 0; i < NUM_STARS; i++) {
        short projX, projY;
        gStars[i].z -= starSpeed;
        if (gStars[i].z <= 16) {
            gStars[i].z += 220;
        }
        projX = 128 + DivS16(MulS16(gStars[i].x, 150), gStars[i].z);
        projY = 72  + DivS16(MulS16(gStars[i].y, 120), gStars[i].z);
        if ((unsigned short)projX < FS_W && (unsigned short)projY < 130) {
            SetPixelWhiteFS(bits, projX, projY);
            if (gStars[i].z < 120 && projX + 1 < FS_W) {
                SetPixelWhiteFS(bits, projX + 1, projY);
            }
        }
    }

    {
        short scroll = (short)((gFrameCount * (1 + (gAudioEnergy >> 4))) & 7);
        FillSpanPatternFS(bits + (132 << 5), 0, 255, (gBassPulse > 24) ? 0x00 : 0x55);
        for (i = 1; i <= 5; i++) {
            short gy = 132 + ((i * i * 22 + scroll * i) >> 2);
            if (gy > 132 && gy < 160) {
                FillSpanPatternFS(bits + (gy << 5), 0, 255, (i >= 4) ? 0x55 : 0xAA);
            }
        }
        for (i = -6; i <= 6; i++) {
            short sway = (FAST_SIN((unsigned char)(gFrameCount * 3)) * gBassPulse) >> 9;
            short topX = 128 + i * 12 + sway;
            short botX = 128 + i * 38 + (sway * 2);
            DrawLineWhiteFS(bits, topX, 132, botX, 160);
        }
    }

    if (gBeatHit) {
        gAngleX = (unsigned char)(gAngleX + 2);
        gAngleY = (unsigned char)(gAngleY + 3);
    }
    gAngleX = (unsigned char)(gAngleX + 1 + (gBassPulse >> 4));
    gAngleY = (unsigned char)(gAngleY + 2 + (gAudioEnergy >> 5));
    gAngleZ = (unsigned char)(gAngleZ + 1 + (gTreblePulse >> 5));
    gTorusTwistPhase += 3 + (gAudioEnergy >> 4);

    targetZoom = (gBassPulse >> 1) + (gAudioEnergy >> 2);
    gTorusZoomSmooth = (gTorusZoomSmooth * 3 + targetZoom) >> 2;

    fovScale   = 155 + (gTorusZoomSmooth >> 1);
    baseMajorR = 38 + (gTorusZoomSmooth >> 3);

    sxA = FAST_SIN(gAngleX); cxA = FAST_COS(gAngleX);
    syA = FAST_SIN(gAngleY); cyA = FAST_COS(gAngleY);
    szA = FAST_SIN(gAngleZ); czA = FAST_COS(gAngleZ);

    for (r = 0; r < TORUS_RINGS; r++) {
        unsigned char uAngle = (unsigned char)(r * (256 / TORUS_RINGS));
        short cosU = FAST_COS(uAngle);
        short sinU = FAST_SIN(uAngle);
        short bandLow  = gEqBands[r * 2];
        short bandHigh = gEqBands[r * 2 + 1];
        short minorR = 12 + (bandLow >> 2);
        short ringMajorR = baseMajorR + ((bandHigh - 24) >> 3);
        short ringWaveZ  = (short)(MulS16(bandLow - 20, FAST_SIN((unsigned char)(uAngle * 2 + gTorusTwistPhase))) >> 8);

        for (s = 0; s < TORUS_SEGS; s++) {
            short idx = r * TORUS_SEGS + s;
            unsigned char vAngle = (unsigned char)(s * (256 / TORUS_SEGS) + (bandHigh >> 2));
            short cosV = FAST_COS(vAngle);
            short sinV = FAST_SIN(vAngle);
            short segBump = (s & 1) ? (gBassPulse >> 4) : -(gTreblePulse >> 5);
            short curMinor = minorR + segBump;
            short ringR = ringMajorR + (short)(MulS16(curMinor, cosV) >> 7);

            short mx = (short)(MulS16(ringR, cosU) >> 7);
            short my = (short)(MulS16(ringR, sinU) >> 7);
            short mz = (short)(MulS16(curMinor, sinV) >> 7) + ringWaveZ;

            short y1 = (short)((MulS16(my, cxA) - MulS16(mz, sxA)) >> 7);
            short z1 = (short)((MulS16(my, sxA) + MulS16(mz, cxA)) >> 7);

            short x2 = (short)((MulS16(mx, cyA) + MulS16(z1, syA)) >> 7);
            short z2 = (short)((-MulS16(mx, syA) + MulS16(z1, cyA)) >> 7);

            short x3 = (short)((MulS16(x2, czA) - MulS16(y1, szA)) >> 7);
            short y3 = (short)((MulS16(x2, szA) + MulS16(y1, czA)) >> 7);

            short dist = z2 + 220;
            if (dist < 70) dist = 70;

            vz[idx] = z2;
            px[idx] = 128 + DivS16(MulS16(x3, fovScale), dist);
            py[idx] = 66  + DivS16(MulS16(y3, fovScale), dist);
        }
    }

    for (r = 0; r < TORUS_RINGS; r++) {
        short nextR = (r + 1) & (TORUS_RINGS - 1);
        for (s = 0; s < TORUS_SEGS; s++) {
            short nextS = (s + 1) % TORUS_SEGS;
            short i0 = r * TORUS_SEGS + s;
            short i1 = nextR * TORUS_SEGS + s;
            short i2 = nextR * TORUS_SEGS + nextS;
            short i3 = r * TORUS_SEGS + nextS;

            short dx1 = px[i1] - px[i0];
            short dy1 = py[i1] - py[i0];
            short dx2 = px[i3] - px[i0];
            short dy2 = py[i3] - py[i0];
            long crossZ = MulS16(dx1, dy2) - MulS16(dy1, dx2);

            if (crossZ > 0) {
                FaceQuad2D *f = &gVisibleFaces[visCount++];
                short shade;
                f->sx[0] = px[i0]; f->sy[0] = py[i0];
                f->sx[1] = px[i1]; f->sy[1] = py[i1];
                f->sx[2] = px[i2]; f->sy[2] = py[i2];
                f->sx[3] = px[i3]; f->sy[3] = py[i3];
                f->avgZ  = (vz[i0] + vz[i1] + vz[i2] + vz[i3]) >> 2;

                shade = (short)(crossZ >> 4) + ((44 - f->avgZ) >> 3) + (gBassPulse >> 3);
                if (shade < 2)  shade = 2;
                if (shade > 16) shade = 16;
                f->shade = (unsigned char)shade;
            }
        }
    }

    for (i = 1; i < visCount; i++) {
        FaceQuad2D key = gVisibleFaces[i];
        short j = i - 1;
        while (j >= 0 && gVisibleFaces[j].avgZ < key.avgZ) {
            gVisibleFaces[j + 1] = gVisibleFaces[j];
            j--;
        }
        gVisibleFaces[j + 1] = key;
    }

    for (i = 0; i < visCount; i++) {
        FaceQuad2D *f = &gVisibleFaces[i];
        FillQuadDitheredFS(bits, f);
        DrawLineWhiteFS(bits, f->sx[0], f->sy[0], f->sx[1], f->sy[1]);
        DrawLineWhiteFS(bits, f->sx[1], f->sy[1], f->sx[2], f->sy[2]);
        DrawLineWhiteFS(bits, f->sx[2], f->sy[2], f->sx[3], f->sy[3]);
        DrawLineWhiteFS(bits, f->sx[3], f->sy[3], f->sx[0], f->sy[0]);
    }
}

static void RenderMode4Tunnel_FS(unsigned char *bits)
{
    short x, yh, b;
    unsigned char rowVals[128];
    unsigned char eqMod[16];
    unsigned char depthScroll = (unsigned char)(gFrameCount * 5 + (gBassPulse >> 1));
    unsigned char twistScroll = (unsigned char)(gFrameCount * 2);

    short camOx = (FAST_SIN((unsigned char)(gFrameCount * 2)) * 10) >> 7;
    short camOy = 24 + ((FAST_COS((unsigned char)(gFrameCount * 3)) * 10) >> 7);

    if (!gTunnelAngle || !gTunnelDepth || !gTunnelFog) {
        RenderMode2Plasma_FS(bits);
        return;
    }

    for (b = 0; b < 16; b++) {
        eqMod[b] = (unsigned char)(gEqBands[b] >> 1);
    }

    for (yh = 0; yh <= 80; yh++) {
        short lutY = yh + camOy;
        unsigned short rowBase = (unsigned short)(lutY << 7);
        const unsigned char *angPtr = gTunnelAngle + rowBase;
        const unsigned char *depPtr = gTunnelDepth + rowBase;
        const unsigned char *fogPtr = gTunnelFog   + rowBase;
        short y0 = yh << 1;
        short ry0 = y0 & 7;
        unsigned char *dstRow0 = bits + (y0 << 5);

        for (x = 0; x < 128; x++) {
            short lutX = x + camOx;
            unsigned char ang, dep, fog, bandVal, u, v, tex;
            unsigned short lit;
            if (lutX < 0) lutX = 0;
            if (lutX > 127) lutX = 127;
            ang = angPtr[lutX];
            dep = depPtr[lutX];
            fog = fogPtr[lutX];
            bandVal = eqMod[(ang >> 4) & 15];

            u = (unsigned char)(ang * 2 + (dep >> 1) + twistScroll);
            v = (unsigned char)(dep + depthScroll - bandVal);
            tex = gPlasmaLut[(unsigned char)(u ^ v)] + (bandVal >> 1);
            lit = (unsigned short)(tex * fog) >> 6;

            if (lit > 63) lit = 63;
            rowVals[x] = (unsigned char)lit;
        }

        AsmPackDitherRow32(dstRow0, rowVals, gDitherP0[ry0], gDitherP1[ry0], gDitherP2[ry0], gDitherP3[ry0]);
        if (y0 + 1 < FS_H) {
            short ry1 = ry0 + 1;
            AsmPackDitherRow32(dstRow0 + FS_ROWBYTES, rowVals, gDitherP0[ry1], gDitherP1[ry1], gDitherP2[ry1], gDitherP3[ry1]);
        }
    }

    {
        short eyeX = (64 - camOx) * 2;
        short eyeY = (64 - camOy) * 2;
        for (b = 0; b < 16; b++) {
            unsigned char rayAng = (unsigned char)(b * 16 + gFrameCount * 2);
            short cs = FAST_COS(rayAng);
            short sn = FAST_SIN(rayAng);
            short rInner = 10;
            short rOuter = 16 + (gEqBands[b] >> 1) + (gBassPulse >> 2);
            short x0 = eyeX + ((rInner * cs) >> 7);
            short y0 = eyeY + ((rInner * sn) >> 7);
            short x1 = eyeX + ((rOuter * cs) >> 7);
            short y1 = eyeY + ((rOuter * sn) >> 7);
            DrawLineWhiteFS(bits, x0, y0, x1, y1);
        }
    }
}

static void RenderMode5FireScope_FS(unsigned char *bits)
{
    short x, y, i;
    unsigned char rowVals[128];

    if (!gFireBuf) {
        RenderMode2Plasma_FS(bits);
        return;
    }

    for (x = 1; x < 99; x++) {
        short b = kBand100[x];
        short coal = (gEqBands[b] * 5) >> 3;
        short noise = (FAST_SIN((unsigned char)(x * 29 + gFrameCount * 17)) + 128) >> 4;
        short val = coal + noise + (gBassPulse >> 1);
        if (val > 63) val = 63;
        gFireBuf[99 * 100 + x]  = (unsigned char)val;
        gFireBuf[100 * 100 + x] = (unsigned char)val;
    }

    {
        unsigned char pA = (unsigned char)(gFrameCount * 4);
        unsigned char pB = (unsigned char)(gFrameCount * 7 + 40);
        short radX = 18 + (gBassPulse >> 3);
        short radY = 28 + ((gBassPulse * 3) >> 3);

        for (i = 0; i < 64; i++) {
            unsigned char t = (unsigned char)(i * 4);
            short waveMod = gWave200[(i * 3) % VIZ_W];
            short lx = 50 + ((FAST_SIN((unsigned char)(t * 2 + pA)) * radX) >> 7) + (waveMod >> 2);
            short ly = 54 + ((FAST_COS((unsigned char)(t * 3 + pB)) * radY) >> 7) + (waveMod >> 2);
            if (lx >= 2 && lx < 98 && ly >= 4 && ly < 96) {
                gFireBuf[ly * 100 + lx]       = 63;
                gFireBuf[ly * 100 + lx + 1]   = 58;
                gFireBuf[(ly + 1) * 100 + lx] = 58;
            }
        }
    }

    for (y = 0; y < 99; y++) {
        AsmDiffuseFireRow98(gFireBuf + y * 100 + 1, gFireBuf + (y + 1) * 100);
    }

    for (y = 0; y < FS_H; y++) {
        unsigned char *dstRow = bits + (y << 5);
        short fireY = gFsYTo98[y];
        const unsigned char *fireRow = gFireBuf + fireY * 100;
        short ry = y & 7;

        for (x = 0; x < 128; x++) {
            rowVals[x] = fireRow[(x * 98) >> 7];
        }

        AsmPackDitherRow32(dstRow, rowVals, gDitherP0[ry], gDitherP1[ry], gDitherP2[ry], gDitherP3[ry]);
    }
}

static void RenderMode6ChromeSpheres_FS(unsigned char *bits)
{
    short x, yh, i;
    unsigned char rowVals[128];
    unsigned char skyThr[128];
    ChromeSphere spheres[3];
    short shadowCenterY[3];
    short bounce = abs(FAST_SIN((unsigned char)(gFrameCount * 5))) >> 2;
    short floorScrollZ = (short)(gFrameCount * 4 + (gAudioEnergy >> 3));
    short floorScrollX = (FAST_SIN((unsigned char)(gFrameCount * 2)) * 28) >> 7;
    unsigned char orbitAng = (unsigned char)(gFrameCount * 4);

    if (!gSphereNz || !gSphereEnvU || !gSphereEnvV || !gSphereSpec) {
        RenderMode3Demoscene3D_FS(bits);
        return;
    }

    spheres[0].cx       = 64 + ((FAST_SIN((unsigned char)(gFrameCount * 2)) * 12) >> 7);
    spheres[0].cy       = 44 - (bounce >> 1) - (gBassPulse >> 2);
    spheres[0].cz       = 100;
    spheres[0].radiusPx = 22 + (gBassPulse >> 3) + (gAudioEnergy >> 4);
    if (spheres[0].radiusPx > 30) spheres[0].radiusPx = 30;
    spheres[0].scaleInv = DivS16(30 * 64, spheres[0].radiusPx);
    spheres[0].spin     = (unsigned char)(gFrameCount * 3);

    spheres[1].cx       = 64 + ((FAST_COS(orbitAng) * 44) >> 7);
    spheres[1].cz       = 100 + ((FAST_SIN(orbitAng) * 40) >> 7);
    spheres[1].cy       = 42 + ((FAST_SIN((unsigned char)(orbitAng * 2)) * 12) >> 7) - (gEqBands[2] >> 3);
    spheres[1].radiusPx = (spheres[1].cz < 100) ? 16 : 11;
    spheres[1].scaleInv = (spheres[1].cz < 100) ? (30 * 64 / 16) : (30 * 64 / 11);
    spheres[1].spin     = (unsigned char)(-gFrameCount * 5);

    spheres[2].cx       = 64 - ((FAST_COS(orbitAng) * 44) >> 7);
    spheres[2].cz       = 100 - ((FAST_SIN(orbitAng) * 40) >> 7);
    spheres[2].cy       = 42 - ((FAST_SIN((unsigned char)(orbitAng * 2)) * 12) >> 7) - (gEqBands[11] >> 3);
    spheres[2].radiusPx = (spheres[2].cz < 100) ? 16 : 11;
    spheres[2].scaleInv = (spheres[2].cz < 100) ? (30 * 64 / 16) : (30 * 64 / 11);
    spheres[2].spin     = (unsigned char)(gFrameCount * 6);

    for (i = 0; i < 2; i++) {
        short j;
        for (j = i + 1; j < 3; j++) {
            if (spheres[i].cz < spheres[j].cz) {
                ChromeSphere tmp = spheres[i];
                spheres[i] = spheres[j];
                spheres[j] = tmp;
            }
        }
    }

    for (x = 0; x < 128; x++) {
        skyThr[x] = (unsigned char)(44 - (gEqBands[x >> 3] >> 2));
    }
    for (i = 0; i < 3; i++) {
        shadowCenterY[i] = 60 + ((120 - spheres[i].cz) >> 2);
    }

    for (yh = 0; yh <= 80; yh++) {
        short y0  = yh << 1;
        short ry0 = y0 & 7;
        unsigned char *dstRow0 = bits + (y0 << 5);

        if (yh < 46) {
            short skyBase = (yh > 28) ? ((yh - 28) * 2) : (yh >> 2);
            short skyGlow = skyBase + 26;
            if (skyBase > 52) skyBase = 52;
            if (skyGlow > 52) skyGlow = 52;
            for (x = 0; x < 128; x++) {
                rowVals[x] = (unsigned char)((yh > skyThr[x]) ? skyGlow : skyBase);
            }
        } else {
            short dyFloor  = yh - 44;
            short zDist    = DivS16(560, dyFloor);
            short zTile    = (zDist + floorScrollZ) >> 4;
            short fogAtten = (dyFloor > 32) ? 64 : (dyFloor * 2);
            unsigned char whiteShade = (unsigned char)((50 * fogAtten) >> 6);
            unsigned char darkShade  = (unsigned char)((12 * fogAtten) >> 6);
            short acc = -64 * zDist;

            for (x = 0; x < 128; x++) {
                short xWorld = (acc >> 5) + floorScrollX;
                short xTile  = xWorld >> 4;
                acc += zDist;
                rowVals[x] = ((xTile ^ zTile) & 1) ? whiteShade : darkShade;
            }

            for (i = 0; i < 3; i++) {
                short rPx = spheres[i].radiusPx;
                if (abs(yh - shadowCenterY[i]) < (rPx >> 1)) {
                    short sx0 = spheres[i].cx - rPx + 1;
                    short sx1 = spheres[i].cx + rPx - 1;
                    if (sx0 < 0)   sx0 = 0;
                    if (sx1 > 127) sx1 = 127;
                    for (x = sx0; x <= sx1; x++) {
                        rowVals[x] >>= 2;
                    }
                }
            }
        }

        for (i = 0; i < 3; i++) {
            short dy  = yh - spheres[i].cy;
            short rPx = spheres[i].radiusPx;
            if (abs(dy) < rPx) {
                short lutY = 32 + ((dy * spheres[i].scaleInv) >> 6);
                if ((unsigned short)lutY < SPHERE_LUT_DIM) {
                    short rowOff = lutY << 6;
                    short xStart = spheres[i].cx - rPx;
                    short xEnd   = spheres[i].cx + rPx;
                    if (xStart < 0)   xStart = 0;
                    if (xEnd > 127)   xEnd = 127;

                    for (x = xStart; x <= xEnd; x++) {
                        short dx = x - spheres[i].cx;
                        short lutX = 32 + ((dx * spheres[i].scaleInv) >> 6);
                        if ((unsigned short)lutX < SPHERE_LUT_DIM) {
                            short sIdx = rowOff + lutX;
                            signed char nz = gSphereNz[sIdx];
                            if (nz > 0) {
                                unsigned char u = (unsigned char)(gSphereEnvU[sIdx] + spheres[i].spin);
                                unsigned char v = (unsigned char)(gSphereEnvV[sIdx] + (gBassPulse >> 1));
                                Boolean boingFacet = (((u >> 5) ^ (v >> 5)) & 1) != 0;
                                short chromeRefl = gPlasmaLut[(unsigned char)(u * 2 + v + floorScrollZ)] >> 1;
                                short shade = (boingFacet ? 26 : 6) + chromeRefl + gSphereSpec[sIdx];
                                if (nz < 18) shade += 18;
                                if (shade > 63) shade = 63;
                                rowVals[x] = (unsigned char)shade;
                            }
                        }
                    }
                }
            }
        }

        AsmPackDitherRow32(dstRow0, rowVals, gDitherP0[ry0], gDitherP1[ry0], gDitherP2[ry0], gDitherP3[ry0]);
        if (y0 + 1 < FS_H) {
            short ry1 = ry0 + 1;
            AsmPackDitherRow32(dstRow0 + FS_ROWBYTES, rowVals, gDitherP0[ry1], gDitherP1[ry1], gDitherP2[ry1], gDitherP3[ry1]);
        }
    }
}

static void RenderMode7VoxelCanyon_FS(unsigned char *bits)
{
    short x, y, b, step, h;
    unsigned char yBuf[100];
    short tilt[100];
    short projH[64];
    unsigned char rowVals[128];
    unsigned char *colBuf = gFireBuf;
    short bankTilt;
    short camX, camY, camHeight;
    short sunRadius, sunRadSq;

    if (!gVoxelHeight || !gVoxelShade || !colBuf) {
        RenderMode4Tunnel_FS(bits);
        return;
    }

    gVoxelWriteRow = (unsigned char)((gVoxelWriteRow + 1) & (VOXEL_MAP_DIM - 1));
    {
        unsigned char *hRow = gVoxelHeight + (gVoxelWriteRow << 6);
        unsigned char *sRow = gVoxelShade  + (gVoxelWriteRow << 6);
        unsigned char prevRowIdx = (unsigned char)((gVoxelWriteRow - 1) & (VOXEL_MAP_DIM - 1));
        const unsigned char *prevH = gVoxelHeight + (prevRowIdx << 6);

        for (x = 0; x < VOXEL_MAP_DIM; x++) {
            short distCenter = abs(x - 32);
            short bandIdx = (x >> 2) & 15;
            short eqH = (gEqBands[bandIdx] * 3) >> 3;
            short ridge = (distCenter > 8) ? ((distCenter - 8) * 2) : 0;
            short wave  = (FAST_SIN((unsigned char)(x * 11 + gFrameCount * 7)) + 128) >> 4;
            short hVal = (prevH[x] + ridge + eqH + wave) >> 1;
            short slope;
            short shade;

            if (hVal > 62) hVal = 62;
            hRow[x] = (unsigned char)hVal;

            slope = (x > 0) ? (hVal - hRow[x - 1]) : 0;
            shade = 18 + (hVal >> 1) + (slope * 3);
            if ((x & 3) == 0 || (gVoxelWriteRow & 3) == 0) {
                shade += 12;
            }
            if (shade < 4)  shade = 4;
            if (shade > 63) shade = 63;
            sRow[x] = (unsigned char)shade;
        }
    }

    sunRadius = 20 + (gBassPulse >> 3);
    sunRadSq  = sunRadius * sunRadius;
    for (y = 0; y < 100; y++) {
        unsigned char *row = colBuf + y * 100;
        short dySun = y - 26;
        short skyShade = (y < 48) ? (y >> 2) : 12;
        memset(row, skyShade, 100);

        if (abs(dySun) < sunRadius && (dySun < 2 || (y & 1) == 0)) {
            short dySunSq = dySun * dySun;
            unsigned char sunCol = (unsigned char)(58 - (dySun > 0 ? dySun : 0));
            for (x = 50 - sunRadius; x <= 50 + sunRadius; x++) {
                short dxSun = kSunDx100[x];
                if (dxSun * dxSun + dySunSq < sunRadSq) {
                    row[x] = sunCol;
                }
            }
        }
    }

    camX      = (32 << 8) + (FAST_SIN((unsigned char)(gFrameCount * 2)) * 18);
    camY      = ((short)gVoxelWriteRow - 34) & (VOXEL_MAP_DIM - 1);
    camHeight = 52 + (gBassPulse >> 2);
    bankTilt  = (FAST_COS((unsigned char)(gFrameCount * 3)) * 14) >> 7;

    for (x = 0; x < 100; x++) {
        yBuf[x] = 100;
        tilt[x] = ((x - 50) * bankTilt) >> 5;
    }

    for (step = 3; step < 34; step++) {
        short mapY = (camY + step) & (VOXEL_MAP_DIM - 1);
        const unsigned char *hRow = gVoxelHeight + (mapY << 6);
        const unsigned char *sRow = gVoxelShade  + (mapY << 6);
        short invZ = DivS16(280, step);
        short fog  = (step > 20) ? (36 - step) : 16;

        unsigned short worldX = (unsigned short)(camX - step * 38);
        unsigned short stepX  = (unsigned short)DivS16(step * 76, 100);

        for (h = 0; h < 64; h++) {
            projH[h] = 44 + (short)((MulS16(camHeight - h, invZ)) >> 5);
        }

        for (x = 0; x < 100; x++) {
            short mx = (short)((worldX >> 8) & (VOXEL_MAP_DIM - 1));
            short projY = tilt[x] + projH[hRow[mx]];

            if (projY < 0) projY = 0;
            if (projY < yBuf[x]) {
                unsigned char shade = (unsigned char)(((unsigned short)sRow[mx] * (unsigned short)fog) >> 4);
                unsigned char *pCol = colBuf + projY * 100 + x;
                short span = yBuf[x] - projY;
                while (span-- > 0) {
                    *pCol = shade;
                    pCol += 100;
                }
                yBuf[x] = (unsigned char)projY;
            }
            worldX += stepX;
        }
    }

    for (y = 0; y < FS_H; y++) {
        unsigned char *dstRow = bits + (y << 5);
        short srcY = gFsYTo99[y];
        const unsigned char *srcRow = colBuf + srcY * 100;
        short ry = y & 7;

        for (x = 0; x < 128; x++) {
            rowVals[x] = srcRow[(x * 99) >> 7];
        }

        AsmPackDitherRow32(dstRow, rowVals, gDitherP0[ry], gDitherP1[ry], gDitherP2[ry], gDitherP3[ry]);
    }

    for (b = 0; b < 16; b++) {
        short hx = 36 + b * 12;
        short hh = gEqBands[b] >> 3;
        DrawLineWhiteFS(bits, hx, 12, hx, 12 - hh);
    }
}

/* ============================================================================
 * CPU-Specific Dispatch Functions (68000 vs 68020+)
 * ============================================================================ */

#ifdef VIZ_BUILD_020
#define VIZ_DISPATCH_200 VizDispatch200_020
#define VIZ_DISPATCH_FS  VizDispatchFS_020
#else
#define VIZ_DISPATCH_200 VizDispatch200_000
#define VIZ_DISPATCH_FS  VizDispatchFS_000
#endif

void VIZ_DISPATCH_200(short mode, unsigned char *outBits)
{
    switch (mode) {
        case VIZ_MODE_EQ:             RenderMode1WinampEQ(outBits); break;
        case VIZ_MODE_PLASMA:         RenderMode2Plasma(outBits); break;
        case VIZ_MODE_3D:             RenderMode3Demoscene3D(outBits); break;
        case VIZ_MODE_TUNNEL:         RenderMode4Tunnel(outBits); break;
        case VIZ_MODE_FIRE_SCOPE:     RenderMode5FireScope(outBits); break;
        case VIZ_MODE_CHROME_SPHERES: RenderMode6ChromeSpheres(outBits); break;
        case VIZ_MODE_VOXEL_CANYON:
        default:                      RenderMode7VoxelCanyon(outBits); break;
    }
}

void VIZ_DISPATCH_FS(short mode, unsigned char *fsBits)
{
    switch (mode) {
        case VIZ_MODE_EQ:             RenderMode1WinampEQ_FS(fsBits); break;
        case VIZ_MODE_PLASMA:         RenderMode2Plasma_FS(fsBits); break;
        case VIZ_MODE_3D:             RenderMode3Demoscene3D_FS(fsBits); break;
        case VIZ_MODE_TUNNEL:         RenderMode4Tunnel_FS(fsBits); break;
        case VIZ_MODE_FIRE_SCOPE:     RenderMode5FireScope_FS(fsBits); break;
        case VIZ_MODE_CHROME_SPHERES: RenderMode6ChromeSpheres_FS(fsBits); break;
        case VIZ_MODE_VOXEL_CANYON:
        default:                      RenderMode7VoxelCanyon_FS(fsBits); break;
    }
}

/* ============================================================================
 * Initialization, Audio Analysis & Public Entry Points (68000 Base TU Only)
 * ============================================================================ */

#ifndef VIZ_BUILD_020

static unsigned short IntSqrt(unsigned long val)
{
    unsigned long res = 0;
    unsigned long bit = 1UL << 14;
    while (bit > val) bit >>= 2;
    while (bit != 0) {
        if (val >= res + bit) {
            val -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return (unsigned short)res;
}

static unsigned char IntAtan2(short dy, short dx)
{
    short ax = abs(dx);
    short ay = abs(dy);
    short angle;

    if (ax == 0 && ay == 0) return 0;
    if (ax >= ay) {
        angle = DivS16((long)ay << 5, ax ? ax : 1);
    } else {
        angle = 64 - DivS16((long)ax << 5, ay ? ay : 1);
    }
    if (dx < 0) angle = 128 - angle;
    if (dy < 0) angle = 256 - angle;
    return (unsigned char)(angle & 0xFF);
}

void VisualizerInit(void)
{
    short y, x, v, i;
    SysEnvRec env;
    long procType = 0;
    static const unsigned char bayer4x4[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 }
    };

    if (gVizInited) return;

    /* Detect CPU type and 68020+ capabilities via Gestalt (with SysEnvirons fallback) */
    if (Gestalt(gestaltProcessorType, &procType) == noErr) {
        gUse020Visualizer = (procType >= gestalt68020);
        if (procType == gestalt68000)      gDetectedCpu = 0;
        else if (procType == gestalt68010) gDetectedCpu = 10;
        else if (procType == gestalt68020) gDetectedCpu = 20;
        else if (procType == gestalt68030) gDetectedCpu = 30;
        else if (procType >= gestalt68040) gDetectedCpu = 40;
    } else if (SysEnvirons(1, &env) == noErr) {
        gUse020Visualizer = (env.processor >= env68020);
        if (env.processor == env68000)      gDetectedCpu = 0;
        else if (env.processor == env68010) gDetectedCpu = 10;
        else if (env.processor == env68020) gDetectedCpu = 20;
        else if (env.processor == env68030) gDetectedCpu = 30;
        else if (env.processor >= env68040) gDetectedCpu = 40;
    } else {
        gUse020Visualizer = false;
        gDetectedCpu = 0;
    }

    for (y = 0; y < VIZ_H; y++) {
        gRowOff200[y] = (unsigned short)(y * VIZ_ROWBYTES);
    }
    for (y = 0; y < FS_H; y++) {
        gFsYTo98[y] = (unsigned char)DivS16((long)y * 98L, FS_H);
        gFsYTo99[y] = (unsigned char)DivS16((long)y * 99L, FS_H);
    }
    for (i = 0; i < 256; i++) {
        short cooled = (i >> 2) - 1;
        gFireCoolLut[i] = (unsigned char)((cooled > 0) ? cooled : 0);
    }

    /* 1. Build 8x8 Bayer 2-pixel pair tables (gDitherP0..P3) */
    for (y = 0; y < 8; y++) {
        for (v = 0; v < 64; v++) {
            unsigned char b0, b1;
            b0 = (v > gBayer8x8[y][0]) ? 0 : 1;
            b1 = (v > gBayer8x8[y][1]) ? 0 : 1;
            gDitherP0[y][v] = (unsigned char)((b0 << 7) | (b1 << 6));

            b0 = (v > gBayer8x8[y][2]) ? 0 : 1;
            b1 = (v > gBayer8x8[y][3]) ? 0 : 1;
            gDitherP1[y][v] = (unsigned char)((b0 << 5) | (b1 << 4));

            b0 = (v > gBayer8x8[y][4]) ? 0 : 1;
            b1 = (v > gBayer8x8[y][5]) ? 0 : 1;
            gDitherP2[y][v] = (unsigned char)((b0 << 3) | (b1 << 2));

            b0 = (v > gBayer8x8[y][6]) ? 0 : 1;
            b1 = (v > gBayer8x8[y][7]) ? 0 : 1;
            gDitherP3[y][v] = (unsigned char)((b0 << 1) | b1);
        }
    }

    /* 2. Build 17-shade 4x4 Bayer byte table for 3D polygon scanline fills */
    for (v = 0; v <= 16; v++) {
        for (y = 0; y < 4; y++) {
            unsigned char byteVal = 0;
            for (x = 0; x < 8; x++) {
                if (v <= bayer4x4[y][x & 3]) {
                    byteVal |= (unsigned char)(0x80 >> x);
                }
            }
            gShadeByte[v][y] = byteVal;
        }
    }

    /* 3. Build smooth cosine palette LUT (0..255 -> 0..63) */
    for (i = 0; i < 256; i++) {
        short s = gSinTbl[i];
        short norm = (s + 128) >> 2;
        if (norm < 0) norm = 0;
        if (norm > 63) norm = 63;
        gPlasmaLut[i] = (unsigned char)norm;
    }

    /* 4. Allocate & populate 100x100 Radial Distance LUT */
    gRadialDist = (unsigned char *)NewPtr(100 * 100);
    if (gRadialDist) {
        for (y = 0; y < 100; y++) {
            for (x = 0; x < 100; x++) {
                unsigned short d = IntSqrt((unsigned long)(x * x + y * y));
                gRadialDist[y * 100 + x] = (unsigned char)((d * 5) & 0xFF);
            }
        }
    }

    /* 5. Allocate & populate 128x128 Polar Wormhole Tunnel LUTs */
    gTunnelAngle = (unsigned char *)NewPtr(TUNNEL_LUT_W * TUNNEL_LUT_H);
    gTunnelDepth = (unsigned char *)NewPtr(TUNNEL_LUT_W * TUNNEL_LUT_H);
    gTunnelFog   = (unsigned char *)NewPtr(TUNNEL_LUT_W * TUNNEL_LUT_H);
    if (gTunnelAngle && gTunnelDepth && gTunnelFog) {
        for (y = 0; y < TUNNEL_LUT_H; y++) {
            for (x = 0; x < TUNNEL_LUT_W; x++) {
                short dx = x - 64;
                short dy = y - 64;
                unsigned short r = IntSqrt((unsigned long)(dx * dx + dy * dy));
                unsigned short idx = (unsigned short)((y << 7) + x);
                unsigned short depth;
                short fog;

                if (r < 3) r = 3;
                gTunnelAngle[idx] = IntAtan2(dy, dx);
                depth = (unsigned short)DivS16(1600L, (short)r);
                gTunnelDepth[idx] = (unsigned char)(depth & 0xFF);

                fog = (short)r;
                if (fog > 63) fog = 63;
                gTunnelFog[idx] = (unsigned char)fog;
            }
        }
    }

    /* 6. Mode 5: Allocate 100x102 Fire/Smoke Diffusion Field */
    gFireBuf = (unsigned char *)NewPtrClear(100 * 102);

    /* 7. Mode 6: Precompute 64x64 3D Sphere Normal, Environment & Phong Specular LUTs */
    gSphereNz   = (signed char *)NewPtrClear(SPHERE_LUT_DIM * SPHERE_LUT_DIM);
    gSphereEnvU = (unsigned char *)NewPtrClear(SPHERE_LUT_DIM * SPHERE_LUT_DIM);
    gSphereEnvV = (unsigned char *)NewPtrClear(SPHERE_LUT_DIM * SPHERE_LUT_DIM);
    gSphereSpec = (unsigned char *)NewPtrClear(SPHERE_LUT_DIM * SPHERE_LUT_DIM);
    if (gSphereNz && gSphereEnvU && gSphereEnvV && gSphereSpec) {
        for (y = 0; y < SPHERE_LUT_DIM; y++) {
            for (x = 0; x < SPHERE_LUT_DIM; x++) {
                short dx = x - 32;
                short dy = y - 32;
                short r2 = dx * dx + dy * dy;
                short idx = (y << 6) + x;
                if (r2 < 30 * 30) {
                    unsigned short nz = IntSqrt((unsigned long)(30 * 30 - r2));
                    long dot = (long)(-dx * 14 - dy * 14 + (short)nz * 22);
                    short spec = 0;
                    gSphereNz[idx]   = (signed char)(nz * 2);
                    gSphereEnvU[idx] = (unsigned char)(128 + dx * 4 + ((dx * (32 - (short)nz)) >> 3));
                    gSphereEnvV[idx] = (unsigned char)(128 + dy * 4 + ((dy * (32 - (short)nz)) >> 3));
                    if (dot > 480) {
                        spec = (short)((dot - 480) >> 3);
                        if (spec > 52) spec = 52;
                    }
                    gSphereSpec[idx] = (unsigned char)spec;
                }
            }
        }
    }

    /* 8. Mode 7: Allocate & seed 64x64 Voxel Canyon Heightmap */
    gVoxelHeight = (unsigned char *)NewPtrClear(VOXEL_MAP_DIM * VOXEL_MAP_DIM);
    gVoxelShade  = (unsigned char *)NewPtrClear(VOXEL_MAP_DIM * VOXEL_MAP_DIM);
    if (gVoxelHeight && gVoxelShade) {
        for (y = 0; y < VOXEL_MAP_DIM; y++) {
            for (x = 0; x < VOXEL_MAP_DIM; x++) {
                short canyon = abs(x - 32);
                short h = (canyon * 2) + ((FAST_SIN((unsigned char)(x * 8 + y * 6)) + 128) >> 4);
                if (h > 63) h = 63;
                gVoxelHeight[(y << 6) + x] = (unsigned char)h;
                gVoxelShade[(y << 6) + x]  = (unsigned char)(16 + (h >> 1));
            }
        }
    }

    /* 9. Initialize 3D Starfield */
    for (i = 0; i < NUM_STARS; i++) {
        gStars[i].x = (short)((i * 73) % 220) - 110;
        gStars[i].y = (short)((i * 137) % 220) - 110;
        gStars[i].z = (short)((i * 29) % 220) + 24;
    }

    gVizInited = true;
}

static void AnalyzeAudioFrame(Boolean isPlaying, long progressMs, const char *trackId)
{
    short i, b;
    short rawBands[16];
    Boolean gotLivePcm = false;

    gFrameCount++;
    gBeatHit = false;
    (void)progressMs;

    if (isPlaying && AudioStreamIsRunning()) {
        gotLivePcm = AudioGetLiveSamples(gRawPcm, 256);
    }

    if (gotLivePcm) {
        long totalAbs = 0;
        long bassAcc  = 0;
        long trebAcc  = 0;
        short lp = 0;
        short prev = (short)gRawPcm[0] - 128;

        for (i = 0; i < VIZ_W; i++) {
            short s = (short)gRawPcm[i] - 128;
            short w = (s * 22) >> 7;
            if (w < -21) w = -21;
            if (w > 21)  w = 21;
            gWave200[i] = (signed char)w;
        }

        for (b = 0; b < 16; b++) {
            short base = b << 4;
            short subLow = 0, subMid = 0, subHigh = 0;
            for (i = 0; i < 16; i++) {
                short s = (short)gRawPcm[base + i] - 128;
                short diff = s - prev;
                prev = s;
                lp = (lp * 3 + s) >> 2;
                subLow  += abs(lp);
                subMid  += abs(s - lp);
                subHigh += abs(diff);
                totalAbs += abs(s);
            }
            if (b < 4) {
                rawBands[b] = (subLow * (6 - b)) >> 4;
                bassAcc += rawBands[b];
            } else if (b < 10) {
                rawBands[b] = ((subLow >> 1) + subMid * 2) >> 4;
            } else {
                rawBands[b] = (subMid + subHigh * 2) >> 4;
                trebAcc += rawBands[b];
            }
            rawBands[b] = (rawBands[b] * (12 + ((b * 5) & 7))) >> 3;
            if (rawBands[b] > 94) rawBands[b] = 94;
        }

        gAudioEnergy = (short)(totalAbs >> 7);
        if (gAudioEnergy > 100) gAudioEnergy = 100;

        {
            short curBass = (short)(bassAcc >> 2);
            if (curBass > (gAvgBass + 6) && curBass > 15) {
                if (gBassPulse < 28) gBeatHit = true;
                gBassPulse = 56;
            } else if (gBassPulse > 4) {
                gBassPulse -= 4;
            } else {
                gBassPulse = 0;
            }
            gAvgBass = (gAvgBass * 7 + curBass) >> 3;
        }
        gTreblePulse = (short)(trebAcc >> 2);
        if (gTreblePulse > 64) gTreblePulse = 64;

    } else if (isPlaying) {
        unsigned short t = (unsigned short)TickCount();
        unsigned char seed = 0x5A;
        unsigned char beatPhase;
        if (trackId && trackId[0]) {
            const char *p = trackId;
            while (*p) { seed = (unsigned char)(seed * 33 + *p++); }
        }
        beatPhase = (unsigned char)((t * 9) & 0xFF);
        if (beatPhase < 18 && gBassPulse < 30) {
            gBeatHit = true;
        }
        gBassPulse = (beatPhase < 80) ? (short)((80 - beatPhase) * 3 >> 2) : 0;
        gAudioEnergy = 45 + (gBassPulse >> 1);
        gTreblePulse = 20 + ((FAST_SIN((unsigned char)(t * 13 + seed)) + 128) >> 3);

        for (b = 0; b < 16; b++) {
            short s1 = FAST_SIN((unsigned char)(t * (5 + (b & 3)) + b * 23 + seed));
            short s2 = FAST_COS((unsigned char)(t * (9 - (b >> 2)) - b * 37));
            short val = ((s1 + 128) * 22 + (s2 + 128) * 16) >> 8;
            if (b < 4) {
                val += gBassPulse + 10;
            } else if (b > 11) {
                val += (gTreblePulse >> 1);
            }
            if (val < 6)  val = 6;
            if (val > 92) val = 92;
            rawBands[b] = val;
        }

        for (i = 0; i < VIZ_W; i++) {
            short w1 = FAST_SIN((unsigned char)(i * 4 + t * 11));
            short w2 = FAST_COS((unsigned char)(i * 9 - t * 7 + seed));
            short amp = 8 + (gBassPulse >> 2);
            gWave200[i] = (signed char)(((w1 + (w2 >> 1)) * amp) >> 8);
        }
    } else {
        gAudioEnergy = 0;
        if (gBassPulse > 3) gBassPulse -= 3; else gBassPulse = 0;
        gTreblePulse = 0;
        for (b = 0; b < 16; b++) {
            rawBands[b] = 2;
        }
        for (i = 0; i < VIZ_W; i++) {
            gWave200[i] = (signed char)(FAST_SIN((unsigned char)(i * 2 + (short)gFrameCount * 3)) >> 6);
        }
    }

    for (b = 0; b < 16; b++) {
        short target = rawBands[b];
        if (target >= gEqBands[b]) {
            gEqBands[b] = (gEqBands[b] + target * 3) >> 2;
        } else {
            gEqBands[b] -= 4;
            if (gEqBands[b] < target) gEqBands[b] = target;
        }
        if (gEqBands[b] < 2)  gEqBands[b] = 2;
        if (gEqBands[b] > 94) gEqBands[b] = 94;

        if (gEqBands[b] >= gEqPeaks[b]) {
            gEqPeaks[b]   = gEqBands[b] + 2;
            if (gEqPeaks[b] > 96) gEqPeaks[b] = 96;
            gEqPeakVel[b] = -2;
        } else {
            gEqPeakVel[b]++;
            if (gEqPeakVel[b] > 0) {
                gEqPeaks[b] -= (gEqPeakVel[b] >> 1);
                if (gEqPeaks[b] < gEqBands[b]) {
                    gEqPeaks[b] = gEqBands[b];
                }
            }
        }
    }
}

static void InitFullscreenLut(void)
{
    short b, i;
    if (gFsLutInited) return;
    for (b = 0; b < 256; b++) {
        unsigned short w = 0;
        for (i = 0; i < 8; i++) {
            if (b & (1 << i)) {
                w |= (unsigned short)(3U << (i * 2));
            }
        }
        gByteDoubleLut[b] = w;
    }
    gFsLutInited = true;
}

static void AsmExpandRow32To64x2(
    const unsigned char *srcRow,
    unsigned char *dstRow0,
    unsigned char *dstRow1,
    const unsigned short *lut
)
{
    short count = 8 - 1;
    long d0_tmp, d1_tmp;
    __asm__ volatile (
        "moveq #0, %3\n\t"
        "1:\n\t"
        "move.b (%0)+, %3\n\t"
        "add.w %3, %3\n\t"
        "move.w 0(%6, %3.w), %4\n\t"
        "move.w %4, (%1)+\n\t"
        "move.w %4, (%2)+\n\t"
        "moveq #0, %3\n\t"

        "move.b (%0)+, %3\n\t"
        "add.w %3, %3\n\t"
        "move.w 0(%6, %3.w), %4\n\t"
        "move.w %4, (%1)+\n\t"
        "move.w %4, (%2)+\n\t"
        "moveq #0, %3\n\t"

        "move.b (%0)+, %3\n\t"
        "add.w %3, %3\n\t"
        "move.w 0(%6, %3.w), %4\n\t"
        "move.w %4, (%1)+\n\t"
        "move.w %4, (%2)+\n\t"
        "moveq #0, %3\n\t"

        "move.b (%0)+, %3\n\t"
        "add.w %3, %3\n\t"
        "move.w 0(%6, %3.w), %4\n\t"
        "move.w %4, (%1)+\n\t"
        "move.w %4, (%2)+\n\t"
        "moveq #0, %3\n\t"

        "dbf %5, 1b\n\t"
        : "+a"(srcRow), "+a"(dstRow0), "+a"(dstRow1), "=&d"(d0_tmp), "=&d"(d1_tmp), "+d"(count)
        : "a"(lut)
        : "cc", "memory"
    );
}

void VisualizerBlitFullscreenCached(unsigned char *outCanvasBits)
{
    short y;
    if (!outCanvasBits) return;
    if (!gFsLutInited) {
        InitFullscreenLut();
    }
    for (y = 0; y < FS_H; y++) {
        const unsigned char *srcRow = gFsBits + (y << 5);
        unsigned char *dstRow0 = outCanvasBits + (y << 7);
        unsigned char *dstRow1 = dstRow0 + 64;
        AsmExpandRow32To64x2(srcRow, dstRow0, dstRow1, gByteDoubleLut);
    }
}

void VisualizerRenderFrame(
    short mode,
    Boolean isPlaying,
    long progressMs,
    const char *trackId,
    unsigned char *outBits
)
{
    short normMode;
    if (!outBits) return;
    if (!gVizInited) {
        VisualizerInit();
    }

    AnalyzeAudioFrame(isPlaying, progressMs, trackId);

    normMode = mode % VIZ_MODE_COUNT;
    if (normMode < 0) normMode += VIZ_MODE_COUNT;

    if (gUse020Visualizer) {
        VizDispatch200_020(normMode, outBits);
    } else {
        VizDispatch200_000(normMode, outBits);
    }
}

void VisualizerRenderFrameFullscreen(
    short mode,
    Boolean isPlaying,
    long progressMs,
    const char *trackId,
    unsigned char *outCanvasBits
)
{
    short normMode;
    if (!gVizInited) {
        VisualizerInit();
    }
    if (!gFsLutInited) {
        InitFullscreenLut();
    }

    AnalyzeAudioFrame(isPlaying, progressMs, trackId);

    normMode = mode % VIZ_MODE_COUNT;
    if (normMode < 0) normMode += VIZ_MODE_COUNT;

    if (gUse020Visualizer) {
        VizDispatchFS_020(normMode, gFsBits);
    } else {
        VizDispatchFS_000(normMode, gFsBits);
    }

    if (outCanvasBits) {
        VisualizerBlitFullscreenCached(outCanvasBits);
    }
}

Boolean VisualizerIs020Active(void)
{
    if (!gVizInited) {
        VisualizerInit();
    }
    return gUse020Visualizer;
}

short VisualizerGetCpuType(void)
{
    if (!gVizInited) {
        VisualizerInit();
    }
    return gDetectedCpu;
}

#endif /* !VIZ_BUILD_020 */
