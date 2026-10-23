/*
 * SpotiMac "Partylizer II" — 68000/68040 Assembly & Demoscene Visualizer Engine
 * Renders 7 Winamp & Amiga/Mac demoscene visualizations in 200x200 1-bit dithered
 * graphics, including two 40 MHz 68040 showcase engines (3D Ray-Cast Chrome Boing
 * Spheres and 3D Voxel Space Audio Canyon Flyover).
 */

#include "visualizer.h"
#include "network.h"
#include <Memory.h>
#include <Events.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================================
 * Precomputed Lookup Tables (Sine, 8x8 Bayer Dither, 4x4 Shade, Polar Tunnel)
 * ============================================================================ */

static Boolean gVizInited = false;

/* 256-entry signed 8.8 fixed-point sine table (-127..+127) */
static const short gSinTbl[256] = {
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

#define FAST_SIN(a) (gSinTbl[(unsigned char)(a)])
#define FAST_COS(a) (gSinTbl[(unsigned char)((a) + 64)])

/*
 * 8x8 Bayer Ordered Dither Matrix (0..63)
 */
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

/*
 * Pre-shifted 2-pixel Bayer dither tables for 68000 assembly byte packing:
 * Combining `p0[v0] | p1[v1] | p2[v2] | p3[v3]` produces 8 individually
 * 1x1 Bayer-dithered pixels in a single byte with zero branches!
 */
static unsigned char gDitherP0[8][64];
static unsigned char gDitherP1[8][64];
static unsigned char gDitherP2[8][64];
static unsigned char gDitherP3[8][64];

/*
 * 17-level (0..16) 4x4 Ordered Dither Full-Byte Table for fast 3D polygon scanline fills
 */
static unsigned char gShadeByte[17][4];

/* Cosine-like smooth plasma wave palette mapping 0..255 -> 0..63 brightness */
static unsigned char gPlasmaLut[256];

/* Radial distance table for 100x100 delta (dx=0..99, dy=0..99) */
static unsigned char *gRadialDist = NULL; /* 100 * 100 = 10,000 bytes */

/* Polar coordinate tables for 128x128 Wormhole Tunnel */
#define TUNNEL_LUT_W 128
#define TUNNEL_LUT_H 128
static unsigned char *gTunnelAngle = NULL; /* 16,384 bytes */
static unsigned char *gTunnelDepth = NULL; /* 16,384 bytes */
static unsigned char *gTunnelFog   = NULL; /* 16,384 bytes */

/* Mode 5: Persistent 100x100 Cellular Fire / Smoke Diffusion Field (0..63) */
static unsigned char *gFireBuf = NULL;     /* 100 * 102 = 10,200 bytes */

/* Mode 6 (68040): Precomputed 64x64 Sphere Normal / Specular / Lat-Lon Map */
#define SPHERE_LUT_DIM 64 /* Covers radius R=32 (-32..+31) */
static signed char   *gSphereNz   = NULL;  /* 64 * 64: >0 inside sphere (0..63), 0 outside */
static unsigned char *gSphereEnvU = NULL;  /* 64 * 64: spherical u coordinate */
static unsigned char *gSphereEnvV = NULL;  /* 64 * 64: spherical v coordinate */
static unsigned char *gSphereSpec = NULL;  /* 64 * 64: Phong specular highlight (0..48) */

/* Mode 7 (68040): 64x64 Rolling 3D Voxel Audio Heightmap & Shade Map */
#define VOXEL_MAP_DIM 64
static unsigned char *gVoxelHeight = NULL; /* 64 * 64 = 4,096 bytes */
static unsigned char *gVoxelShade  = NULL; /* 64 * 64 = 4,096 bytes */
static unsigned char  gVoxelWriteRow = 0;

/* ============================================================================
 * Audio Spectrum, Waveform & Beat State
 * ============================================================================ */

static unsigned char gRawPcm[256];
static signed char   gWave200[VIZ_W];     /* -21..+21 vertical scope offset */
static short         gEqBands[16];        /* 0..96 smoothed bar heights */
static short         gEqPeaks[16];        /* 0..96 peak hold positions */
static short         gEqPeakVel[16];      /* falling velocity of peak caps */
static short         gAudioEnergy = 0;    /* 0..100 overall energy */
static short         gBassPulse   = 0;    /* 0..64 bass kick envelope */
static short         gTreblePulse = 0;    /* 0..64 treble envelope */
static short         gAvgBass     = 20;
static Boolean       gBeatHit     = false;
static unsigned long gFrameCount  = 0;

/* ============================================================================
 * 3D Scene State (Mode 3: Supercharged Audio-Reactive Torus + Starfield)
 * ============================================================================ */

#define NUM_STARS    48
typedef struct {
    short x; /* -120..+120 */
    short y; /* -120..+120 */
    short z; /* 16..255 */
} Star3D;

static Star3D gStars[NUM_STARS];

#if 0 /* Archived Mode 8 Hyperspace 3D Warp Stars */
#define NUM_WARP_STARS 128
typedef struct {
    short x, y; /* -160..+160 */
    short z;    /* 12..255 */
} WarpStar3D;
static WarpStar3D gWarpStars[NUM_WARP_STARS];
static short      gWarpBoostSmooth = 0;
#endif

static unsigned char gAngleX = 20;
static unsigned char gAngleY = 0;
static unsigned char gAngleZ = 10;
static short         gTorusZoomSmooth = 0;
static short         gTorusTwistPhase = 0;

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

static FaceQuad2D gVisibleFaces[TORUS_FACES];

/* ============================================================================
 * 68000 Assembly Primitives
 * ============================================================================ */

/*
 * Ultra-fast 68000 assembly clear of 5,200-byte bitmap to 0xFF (pure black in QuickDraw)
 */
static void AsmClearBlack5200(unsigned char *dst)
{
    unsigned long fill = 0xFFFFFFFFUL;
    short loops = (5200 / 16) - 1; /* 325 iterations of 16 bytes = 5200 bytes */
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
 * 68000 assembly converter: takes 100 brightness values (0..63) in `rowVals[0..99]`
 * and packs them into 25 single-pixel-dithered bytes (200 horizontal pixels) in `dstRow`
 * using the 4 pre-shifted Bayer tables `p0, p1, p2, p3`!
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
    short count = 25 - 1; /* 25 bytes = 200 pixels */
    long d0_tmp, d1_tmp;
    __asm__ volatile (
        "1:\n\t"
        "moveq #0, %2\n\t"
        "move.b (%1)+, %2\n\t"
        "move.b 0(%5, %2.l), %3\n\t"

        "moveq #0, %2\n\t"
        "move.b (%1)+, %2\n\t"
        "or.b 0(%6, %2.l), %3\n\t"

        "moveq #0, %2\n\t"
        "move.b (%1)+, %2\n\t"
        "or.b 0(%7, %2.l), %3\n\t"

        "moveq #0, %2\n\t"
        "move.b (%1)+, %2\n\t"
        "or.b 0(%8, %2.l), %3\n\t"

        "move.b %3, (%0)+\n\t"
        "dbf %4, 1b\n\t"
        : "+a"(dstRow), "+a"(rowVals), "=&d"(d0_tmp), "=&d"(d1_tmp), "+d"(count)
        : "a"(p0), "a"(p1), "a"(p2), "a"(p3)
        : "cc", "memory"
    );
}

/*
 * 68000 assembly cellular fire/smoke upward diffusion pass on a 98-pixel row:
 * dst[x] = max(0, ((srcBelow[x-1] + srcBelow[x]*2 + srcBelow[x+1]) >> 2) - 1)
 */
static void AsmDiffuseFireRow98(unsigned char *dst, const unsigned char *srcBelow)
{
    short count = 98 - 1;
    long d0, d1;
    __asm__ volatile (
        "1:\n\t"
        "moveq #0, %2\n\t"
        "move.b (%1)+, %2\n\t"
        "moveq #0, %3\n\t"
        "move.b (%1), %3\n\t"
        "add.w %3, %3\n\t"
        "add.w %3, %2\n\t"
        "moveq #0, %3\n\t"
        "move.b 1(%1), %3\n\t"
        "add.w %3, %2\n\t"
        "lsr.w #2, %2\n\t"
        "beq.s 2f\n\t"
        "subq.w #1, %2\n\t"
        "2:\n\t"
        "move.b %2, (%0)+\n\t"
        "dbf %4, 1b\n\t"
        : "+a"(dst), "+a"(srcBelow), "=&d"(d0), "=&d"(d1), "+d"(count)
        :
        : "cc", "memory"
    );
}

/* Fast inline pixel operations on the 200x200 (26 bytes/row) buffer */
static inline void SetPixelWhite(unsigned char *bits, short x, short y)
{
    if ((unsigned short)x < VIZ_W && (unsigned short)y < VIZ_H) {
        bits[y * VIZ_ROWBYTES + (x >> 3)] &= (unsigned char)~(0x80 >> (x & 7));
    }
}

static inline void SetPixelBlack(unsigned char *bits, short x, short y)
{
    if ((unsigned short)x < VIZ_W && (unsigned short)y < VIZ_H) {
        bits[y * VIZ_ROWBYTES + (x >> 3)] |= (unsigned char)(0x80 >> (x & 7));
    }
}

/* Fast Bresenham line drawer in white (clears bits to 0) */
static void DrawLineWhite(unsigned char *bits, short x0, short y0, short x1, short y1)
{
    short dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
    short dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
    short err = dx + dy, e2;

    for (;;) {
        if ((unsigned short)x0 < VIZ_W && (unsigned short)y0 < VIZ_H) {
            bits[y0 * VIZ_ROWBYTES + (x0 >> 3)] &= (unsigned char)~(0x80 >> (x0 & 7));
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err << 1;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

#if 0 /* Used by Archived Visualizer Modes 8, 9, 10 */
/* Fast Bresenham line drawer in black (sets bits to 1) */
static void DrawLineBlack(unsigned char *bits, short x0, short y0, short x1, short y1)
{
    short dx = abs(x1 - x0), sx = (x0 < x1) ? 1 : -1;
    short dy = -abs(y1 - y0), sy = (y0 < y1) ? 1 : -1;
    short err = dx + dy, e2;

    for (;;) {
        if ((unsigned short)x0 < VIZ_W && (unsigned short)y0 < VIZ_H) {
            bits[y0 * VIZ_ROWBYTES + (x0 >> 3)] |= (unsigned char)(0x80 >> (x0 & 7));
        }
        if (x0 == x1 && y0 == y1) break;
        e2 = err << 1;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
#endif

/* Fast horizontal span fill using a pre-dithered byte pattern (`patByte`, 0=white, 1=black) */
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
        }
        rowPtr[bRight] = (rowPtr[bRight] & ~rightMask) | (patByte & rightMask);
    }
}

/* ============================================================================
 * Integer Math Helpers for Table Initialization
 * ============================================================================ */

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

/* Octant-based integer atan2 returning 0..255 angle */
static unsigned char IntAtan2(short dy, short dx)
{
    short ax = abs(dx);
    short ay = abs(dy);
    short angle;

    if (ax == 0 && ay == 0) return 0;
    if (ax >= ay) {
        angle = (short)(((long)ay * 32L) / (ax ? ax : 1));
    } else {
        angle = 64 - (short)(((long)ax * 32L) / (ay ? ay : 1));
    }
    if (dx < 0) angle = 128 - angle;
    if (dy < 0) angle = 256 - angle;
    return (unsigned char)(angle & 0xFF);
}

/* ============================================================================
 * Initialization of All Lookup Tables
 * ============================================================================ */

void VisualizerInit(void)
{
    short y, x, v, i;
    static const unsigned char bayer4x4[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 }
    };

    if (gVizInited) return;

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
                unsigned short idx = (unsigned short)(y * TUNNEL_LUT_W + x);
                unsigned short depth;
                short fog;

                if (r < 3) r = 3;
                gTunnelAngle[idx] = IntAtan2(dy, dx);
                depth = (unsigned short)(1600U / r);
                gTunnelDepth[idx] = (unsigned char)(depth & 0xFF);

                fog = (short)r;
                if (fog > 63) fog = 63;
                gTunnelFog[idx] = (unsigned char)fog;
            }
        }
    }

    /* 6. Mode 5: Allocate 100x102 Fire/Smoke Diffusion Field */
    gFireBuf = (unsigned char *)NewPtrClear(100 * 102);

    /* 7. Mode 6 (68040): Precompute 64x64 3D Sphere Normal, Environment & Phong Specular LUTs */
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
                short idx = y * SPHERE_LUT_DIM + x;
                if (r2 < 30 * 30) {
                    unsigned short nz = IntSqrt((unsigned long)(30 * 30 - r2));
                    /* Light direction from top-left-front: (-14, -14, +22) */
                    long dot = (long)(-dx * 14 - dy * 14 + (short)nz * 22);
                    short spec = 0;
                    gSphereNz[idx]   = (signed char)(nz * 2); /* 1..60 */
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

    /* 8. Mode 7 (68040): Allocate & seed 64x64 Voxel Canyon Heightmap */
    gVoxelHeight = (unsigned char *)NewPtrClear(VOXEL_MAP_DIM * VOXEL_MAP_DIM);
    gVoxelShade  = (unsigned char *)NewPtrClear(VOXEL_MAP_DIM * VOXEL_MAP_DIM);
    if (gVoxelHeight && gVoxelShade) {
        for (y = 0; y < VOXEL_MAP_DIM; y++) {
            for (x = 0; x < VOXEL_MAP_DIM; x++) {
                short canyon = abs(x - 32);
                short h = (canyon * 2) + ((FAST_SIN((unsigned char)(x * 8 + y * 6)) + 128) >> 4);
                if (h > 63) h = 63;
                gVoxelHeight[y * VOXEL_MAP_DIM + x] = (unsigned char)h;
                gVoxelShade[y * VOXEL_MAP_DIM + x]  = (unsigned char)(16 + (h >> 1));
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

/* ============================================================================
 * Real-Time Audio Spectrum & Waveform Analyzer
 * ============================================================================ */

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
        unsigned long t = TickCount();
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
        gTreblePulse = 20 + ((FAST_SIN(t * 13 + seed) + 128) >> 3);

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
            gWave200[i] = (signed char)(FAST_SIN((unsigned char)(i * 2 + gFrameCount * 3)) >> 6);
        }
    }

    /* Smooth attack / exponential decay on the 16 EQ bars + gravity peak-hold caps */
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

        for (y = yMin; y <= yMax; y++) {
            SetPixelWhite(bits, x, y);
        }
        if ((x & 1) == (short)(gFrameCount & 1)) {
            if (yMin > 5)  SetPixelWhite(bits, x, yMin - 2);
            if (yMax < 49) SetPixelWhite(bits, x, yMax + 2);
        }
        prevY = curY;
    }

    for (b = 0; b < 16; b++) {
        short x0   = 5 + b * 12;
        short x1   = x0 + 9;
        short barH = gEqBands[b];
        short peakY = 152 - gEqPeaks[b];
        short dy;

        for (dy = 0; dy < barH; dy++) {
            unsigned char *rowPtr;
            unsigned char pat;
            if ((dy % 3) == 2) continue;

            y = 152 - dy;
            if (y < 54) break;
            rowPtr = bits + y * VIZ_ROWBYTES;

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
            FillSpanPattern(bits + peakY * VIZ_ROWBYTES, x0, x1, 0x00);
            if (peakY > 54) {
                FillSpanPattern(bits + (peakY - 1) * VIZ_ROWBYTES, x0, x1, 0x00);
            }
        }

        {
            short refH = (barH * 2) / 5;
            if (refH > 38) refH = 38;
            for (dy = 1; dy <= refH; dy += 2) {
                short ry = 155 + dy;
                short ripple = FAST_SIN((unsigned char)(dy * 14 + gFrameCount * 12 + b * 19)) >> 6;
                short rx0 = x0 + ripple;
                short rx1 = x1 + ripple;
                unsigned char refPat = (dy > (refH >> 1)) ? 0xEE : ((ry & 1) ? 0xAA : 0xCC);
                if (ry < 196) {
                    FillSpanPattern(bits + ry * VIZ_ROWBYTES, rx0, rx1, refPat);
                }
            }
        }
    }

    FillSpanPattern(bits + 154 * VIZ_ROWBYTES, 2, 197, 0x55);
}

/* ============================================================================
 * MODE 2: Geiss / Amiga Demoscene 64-Shade Dithered Plasma & Metaballs
 * ============================================================================ */

static void RenderMode2Plasma(unsigned char *bits)
{
    short x, y;
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

    for (y = 0; y < VIZ_H; y++) {
        unsigned char *dstRow = bits + y * VIZ_ROWBYTES;
        short yHalf = y >> 1;
        short dy0 = abs(yHalf - (by0 >> 1));
        short dy1 = abs(yHalf - (by1 >> 1));
        const unsigned char *rRow0 = gRadialDist ? (gRadialDist + dy0 * 100) : NULL;
        const unsigned char *rRow1 = gRadialDist ? (gRadialDist + dy1 * 100) : NULL;
        unsigned char wy = (unsigned char)(
            (FAST_SIN((unsigned char)(y * 3 - t2)) >> 1) +
            (FAST_COS((unsigned char)(y * 5 + t3)) >> 2) +
            beatShift
        );
        short ry = y & 7;

        if (rRow0 && rRow1) {
            for (x = 0; x < 100; x++) {
                short dx0 = abs(x - bx0);
                short dx1 = abs(x - bx1);
                unsigned char phase = (unsigned char)(waveX[x] + wy + rRow0[dx0] - rRow1[dx1]);
                rowVals[x] = gPlasmaLut[phase];
            }
        } else {
            for (x = 0; x < 100; x++) {
                unsigned char phase = (unsigned char)(waveX[x] + wy + x * 3);
                rowVals[x] = gPlasmaLut[phase];
            }
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

            for (y = yMin; y <= yMax + 1; y++) {
                SetPixelWhite(bits, x, y);
            }
            prevY = curY;
        }
    }
}

/* ============================================================================
 * MODE 3: Supercharged Audio-Reactive 3D Shaded Torus + Starfield + Cyber Grid
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
                    short xInt = x0 + (short)(((long)(y - y0) * (x1 - x0)) / (y1 - y0));
                    if (xInt < xLeft)  xLeft  = xInt;
                    if (xInt > xRight) xRight = xInt;
                }
            }
        }
        if (xLeft <= xRight) {
            unsigned char pat = gShadeByte[q->shade][y & 3];
            FillSpanPattern(bits + y * VIZ_ROWBYTES, xLeft, xRight, pat);
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

    /* 1. 3D Warp Starfield — accelerates dramatically with bass & energy */
    starSpeed = 2 + (gAudioEnergy >> 3) + (gBassPulse >> 2);
    for (i = 0; i < NUM_STARS; i++) {
        short projX, projY;
        gStars[i].z -= starSpeed;
        if (gStars[i].z <= 16) {
            gStars[i].z += 220;
        }
        projX = 100 + (short)(((long)gStars[i].x * 128L) / gStars[i].z);
        projY = 92  + (short)(((long)gStars[i].y * 128L) / gStars[i].z);
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
        FillSpanPattern(bits + 164 * VIZ_ROWBYTES, 0, 199, (gBassPulse > 24) ? 0x00 : 0x55);
        for (i = 1; i <= 5; i++) {
            short gy = 164 + ((i * i * 28 + scroll * i) >> 2);
            if (gy > 164 && gy < 199) {
                FillSpanPattern(bits + gy * VIZ_ROWBYTES, 0, 199, (i >= 4) ? 0x55 : 0xAA);
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

    fovScale   = 165 + (gTorusZoomSmooth >> 1); /* Gentle, pleasing ~165..185 zoom on bass hits */
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
        /* Balanced tube radius: swells smoothly from 12 up to 28 */
        short minorR = 12 + (bandLow >> 2);
        /* Subtle per-ring radial pulse & Z-wave */
        short ringMajorR = baseMajorR + ((bandHigh - 24) >> 3);
        short ringWaveZ  = ((bandLow - 20) * FAST_SIN((unsigned char)(uAngle * 2 + gTorusTwistPhase))) >> 8;

        for (s = 0; s < TORUS_SEGS; s++) {
            short idx = r * TORUS_SEGS + s;
            /* Audio-driven torsional twist around the tube cross-section */
            unsigned char vAngle = (unsigned char)(s * (256 / TORUS_SEGS) + (bandHigh >> 2));
            short cosV = FAST_COS(vAngle);
            short sinV = FAST_SIN(vAngle);
            short segBump = (s & 1) ? (gBassPulse >> 4) : -(gTreblePulse >> 5);
            short curMinor = minorR + segBump;
            short ringR = ringMajorR + ((curMinor * cosV) >> 7);

            short mx = (ringR * cosU) >> 7;
            short my = (ringR * sinU) >> 7;
            short mz = ((curMinor * sinV) >> 7) + ringWaveZ;

            short y1 = (my * cxA - mz * sxA) >> 7;
            short z1 = (my * sxA + mz * cxA) >> 7;

            short x2 = (mx * cyA + z1 * syA) >> 7;
            short z2 = (-mx * syA + z1 * cyA) >> 7;

            short x3 = (x2 * czA - y1 * szA) >> 7;
            short y3 = (x2 * szA + y1 * czA) >> 7;

            short dist = z2 + 220;
            if (dist < 70) dist = 70;

            vz[idx] = z2;
            px[idx] = 100 + (short)(((long)x3 * (long)fovScale) / dist);
            py[idx] = 86  + (short)(((long)y3 * (long)fovScale) / dist);
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

            long dx1 = px[i1] - px[i0];
            long dy1 = py[i1] - py[i0];
            long dx2 = px[i3] - px[i0];
            long dy2 = py[i3] - py[i0];
            long crossZ = dx1 * dy2 - dy1 * dx2;

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
 * MODE 4: MilkDrop 3D Wormhole Vortex Tunnel + Radial Spectrum Burst
 * ============================================================================ */

static void RenderMode4Tunnel(unsigned char *bits)
{
    short x, y, b;
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

    for (y = 0; y < VIZ_H; y++) {
        unsigned char *dstRow = bits + y * VIZ_ROWBYTES;
        short lutY = (y >> 1) + camOy;
        unsigned short rowBase = (unsigned short)(lutY * TUNNEL_LUT_W + camOx);
        const unsigned char *angPtr = gTunnelAngle + rowBase;
        const unsigned char *depPtr = gTunnelDepth + rowBase;
        const unsigned char *fogPtr = gTunnelFog   + rowBase;
        short ry = y & 7;

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
 * MODE 5: Demoscene Cellular Fire & Smoke Diffusion + 3D Harmonograph Ribbon
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
        short b = (x * 16) / 100;
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
        unsigned char *dstRow = bits + y * VIZ_ROWBYTES;
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
 * MODE 6 (40 MHz 68040 Showcase #1):
 * Real-Time Ray-Cast 3D Reflective Chrome Boing Spheres + Phong Specular +
 * Infinite Perspective Checkered Floor with Real-Time Soft Drop Shadows
 * ============================================================================ */

typedef struct {
    short cx, cy, cz; /* Center in 100x100xZ space */
    short scaleInv;   /* 64 = full R=30, 96 = R=20, 128 = R=15 */
    short radiusPx;   /* Screen radius in 100x100 space */
    unsigned char spin;
} ChromeSphere;

static void RenderMode6ChromeSpheres(unsigned char *bits)
{
    short x, y, i;
    unsigned char rowVals[100];
    ChromeSphere spheres[3];
    short bounce = abs(FAST_SIN((unsigned char)(gFrameCount * 5))) >> 2; /* 0..31 */
    short floorScrollZ = (short)(gFrameCount * 4 + (gAudioEnergy >> 3));
    short floorScrollX = (FAST_SIN((unsigned char)(gFrameCount * 2)) * 24) >> 7;
    unsigned char orbitAng = (unsigned char)(gFrameCount * 4);

    if (!gSphereNz || !gSphereEnvU || !gSphereEnvV || !gSphereSpec) {
        RenderMode3Demoscene3D(bits);
        return;
    }

    /*
     * Configure 3 3D Chrome Spheres:
     * - Sphere 0: Giant Central Boing/Chrome Sphere that bounces & swells with bass
     * - Sphere 1 & 2: Two orbiting satellite spheres that circle in 3D depth (in front & behind!)
     */
    spheres[0].cx       = 50 + ((FAST_SIN((unsigned char)(gFrameCount * 2)) * 10) >> 7);
    spheres[0].cy       = 48 - (bounce >> 1) - (gBassPulse >> 2);
    spheres[0].cz       = 100;
    spheres[0].radiusPx = 22 + (gBassPulse >> 3) + (gAudioEnergy >> 4);
    if (spheres[0].radiusPx > 30) spheres[0].radiusPx = 30;
    spheres[0].scaleInv = (short)((30 * 64) / spheres[0].radiusPx);
    spheres[0].spin     = (unsigned char)(gFrameCount * 3);

    spheres[1].cx       = 50 + ((FAST_COS(orbitAng) * 34) >> 7);
    spheres[1].cz       = 100 + ((FAST_SIN(orbitAng) * 40) >> 7);
    spheres[1].cy       = 46 + ((FAST_SIN((unsigned char)(orbitAng * 2)) * 12) >> 7) - (gEqBands[2] >> 3);
    spheres[1].radiusPx = (spheres[1].cz < 100) ? 16 : 11;
    spheres[1].scaleInv = (short)((30 * 64) / spheres[1].radiusPx);
    spheres[1].spin     = (unsigned char)(-gFrameCount * 5);

    spheres[2].cx       = 50 - ((FAST_COS(orbitAng) * 34) >> 7);
    spheres[2].cz       = 100 - ((FAST_SIN(orbitAng) * 40) >> 7);
    spheres[2].cy       = 46 - ((FAST_SIN((unsigned char)(orbitAng * 2)) * 12) >> 7) - (gEqBands[11] >> 3);
    spheres[2].radiusPx = (spheres[2].cz < 100) ? 16 : 11;
    spheres[2].scaleInv = (short)((30 * 64) / spheres[2].radiusPx);
    spheres[2].spin     = (unsigned char)(gFrameCount * 6);

    /* Depth-sort the 3 spheres back-to-front (largest cz drawn first, smallest cz on top) */
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

    for (y = 0; y < VIZ_H; y++) {
        unsigned char *dstRow = bits + y * VIZ_ROWBYTES;
        short yh = y >> 1; /* 0..99 */
        short ry = y & 7;

        /* 1. Render Background: Audio Horizon Sky (yh < 56) or 3D Ray-Cast Checkered Floor (yh >= 56) */
        if (yh < 56) {
            /* Starry / Nebula gradient sky with EQ horizon aurora at yh = 36..55 */
            short skyBase = (yh > 36) ? ((yh - 36) * 2) : (yh >> 2);
            for (x = 0; x < 100; x++) {
                short b = (x * 16) / 100;
                short eqGlow = (yh > (54 - (gEqBands[b] >> 2))) ? 26 : 0;
                short val = skyBase + eqGlow;
                if (val > 52) val = 52;
                rowVals[x] = (unsigned char)val;
            }
        } else {
            /* Ray-cast 3D perspective floor plane: z = 512 / (yh - 54) */
            short dyFloor = yh - 54;
            short zDist   = (short)(640 / dyFloor);
            short zTile   = (zDist + floorScrollZ) >> 4;
            short fogAtten = (dyFloor > 32) ? 64 : (dyFloor * 2);

            for (x = 0; x < 100; x++) {
                short xWorld = (((x - 50) * zDist) >> 5) + floorScrollX;
                short xTile  = xWorld >> 4;
                Boolean isWhiteTile = ((xTile ^ zTile) & 1) != 0;
                short shade = isWhiteTile ? ((50 * fogAtten) >> 6) : ((12 * fogAtten) >> 6);

                /* Cast soft 3D drop shadows under each of the 3 spheres onto the floor! */
                for (i = 0; i < 3; i++) {
                    short sdx = abs(x - spheres[i].cx);
                    short sdy = abs(yh - (72 + ((120 - spheres[i].cz) >> 2)));
                    if (sdx < spheres[i].radiusPx && sdy < (spheres[i].radiusPx >> 1)) {
                        shade >>= 2; /* Deep shadow umbra */
                    }
                }
                rowVals[x] = (unsigned char)shade;
            }
        }

        /* 2. Ray-Cast & Environment-Map the 3 3D Chrome Boing Spheres (Back-to-Front) */
        for (i = 0; i < 3; i++) {
            short dy = yh - spheres[i].cy;
            short rPx = spheres[i].radiusPx;
            if (abs(dy) < rPx) {
                short lutY = 32 + ((dy * spheres[i].scaleInv) >> 6);
                if ((unsigned short)lutY < SPHERE_LUT_DIM) {
                    short rowOff = lutY * SPHERE_LUT_DIM;
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
                                /* Environment-mapped reflection + Amiga Boing facets + Phong Specular! */
                                unsigned char u = (unsigned char)(gSphereEnvU[sIdx] + spheres[i].spin);
                                unsigned char v = (unsigned char)(gSphereEnvV[sIdx] + (gBassPulse >> 1));
                                Boolean boingFacet = (((u >> 5) ^ (v >> 5)) & 1) != 0;
                                short chromeRefl = gPlasmaLut[(unsigned char)(u * 2 + v + floorScrollZ)] >> 1;
                                short shade = (boingFacet ? 26 : 6) + chromeRefl + gSphereSpec[sIdx];
                                /* Crisp Fresnel rim highlight around the sphere silhouette */
                                if (nz < 18) shade += 18;
                                if (shade > 63) shade = 63;
                                rowVals[x] = (unsigned char)shade;
                            }
                        }
                    }
                }
            }
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

/* ============================================================================
 * MODE 7 (40 MHz 68040 Showcase #2):
 * Comanche / Demoscene 3D Voxel Space Audio Canyon Flyover +
 * Banking Camera + Directional Sunlight Shading + Pulsing Synthwave Sun
 * ============================================================================ */

static void RenderMode7VoxelCanyon(unsigned char *bits)
{
    short x, y, b, step;
    unsigned char yBuf[100];      /* Front-to-back vertical horizon occlusion buffer (0..99) */
    unsigned char *colBuf = gFireBuf; /* Share 10,200-byte heap buffer (never on 68k stack!) */
    short bankTilt;
    short camX, camY, camHeight;
    short sunRadius;

    if (!gVoxelHeight || !gVoxelShade || !colBuf) {
        RenderMode4Tunnel(bits);
        return;
    }

    /*
     * 1. Stream the latest 16-band EQ spectrum into the advancing Voxel Horizon Row!
     *    Creates a living 3D mountain canyon where every beat and melody note becomes
     *    physical 3D terrain you fly over!
     */
    gVoxelWriteRow = (unsigned char)((gVoxelWriteRow + 1) & (VOXEL_MAP_DIM - 1));
    {
        unsigned char *hRow = gVoxelHeight + gVoxelWriteRow * VOXEL_MAP_DIM;
        unsigned char *sRow = gVoxelShade  + gVoxelWriteRow * VOXEL_MAP_DIM;
        unsigned char prevRowIdx = (unsigned char)((gVoxelWriteRow - 1) & (VOXEL_MAP_DIM - 1));
        const unsigned char *prevH = gVoxelHeight + prevRowIdx * VOXEL_MAP_DIM;

        for (x = 0; x < VOXEL_MAP_DIM; x++) {
            /* Canyon profile: lower valley in center (x=22..42), high EQ peaks on ridges & walls */
            short distCenter = abs(x - 32);
            short bandIdx = (x >> 2) & 15;
            short eqH = (gEqBands[bandIdx] * 3) >> 3;
            short ridge = (distCenter > 8) ? ((distCenter - 8) * 2) : 0;
            short wave  = (FAST_SIN((unsigned char)(x * 11 + gFrameCount * 7)) + 128) >> 4;
            short h = (prevH[x] + ridge + eqH + wave) >> 1;
            short slope;
            short shade;

            if (h > 62) h = 62;
            hRow[x] = (unsigned char)h;

            /* Directional sunlight shading from left-to-right terrain slope + altitude */
            slope = (x > 0) ? (h - hRow[x - 1]) : 0;
            shade = 18 + (h >> 1) + (slope * 3);
            /* Add glowing grid lines every 4 voxel cells */
            if ((x & 3) == 0 || (gVoxelWriteRow & 3) == 0) {
                shade += 12;
            }
            if (shade < 4)  shade = 4;
            if (shade > 63) shade = 63;
            sRow[x] = (unsigned char)shade;
        }
    }

    /* 2. Fill Sky & Pulsing Synthwave Sun into `colBuf` (100x100) */
    sunRadius = 16 + (gBassPulse >> 3);
    for (y = 0; y < 100; y++) {
        unsigned char *row = colBuf + y * 100;
        short dySun = y - 24;
        short skyShade = (y < 48) ? (y >> 2) : 12;
        memset(row, skyShade, 100);

        /* Draw sliced Synthwave Sun centered at (50, 24) */
        if (abs(dySun) < sunRadius && (dySun < 2 || (y & 1) == 0)) {
            for (x = 50 - sunRadius; x <= 50 + sunRadius; x++) {
                short dxSun = x - 50;
                if (dxSun * dxSun + dySun * dySun < sunRadius * sunRadius) {
                    row[x] = (unsigned char)(58 - (dySun > 0 ? dySun : 0));
                }
            }
        }
    }

    /* 3. Front-to-Back Voxel Ray-Casting with Camera Banking & Occlusion Culling */
    for (x = 0; x < 100; x++) {
        yBuf[x] = 100;
    }

    camX      = (32 << 8) + (FAST_SIN((unsigned char)(gFrameCount * 2)) * 18);
    camY      = ((short)gVoxelWriteRow - 34) & (VOXEL_MAP_DIM - 1);
    camHeight = 52 + (gBassPulse >> 2);
    bankTilt  = (FAST_COS((unsigned char)(gFrameCount * 3)) * 14) >> 7; /* Camera roll */

    for (step = 3; step < 34; step++) {
        short mapY = (camY + step) & (VOXEL_MAP_DIM - 1);
        const unsigned char *hRow = gVoxelHeight + mapY * VOXEL_MAP_DIM;
        const unsigned char *sRow = gVoxelShade  + mapY * VOXEL_MAP_DIM;
        short invZ = (short)(280 / step);
        short fog  = (step > 20) ? (36 - step) : 16; /* Distance fog attenuation */

        /* Perspective horizontal step across the 100 screen columns */
        long worldX = ((long)camX - ((long)step * 38L)) & 0xFFFF;
        long stepX  = ((long)step * 76L) / 100L;

        for (x = 0; x < 100; x++) {
            short mx = (short)((worldX >> 8) & (VOXEL_MAP_DIM - 1));
            short terrainH = hRow[mx];
            short tiltOffset = ((x - 50) * bankTilt) >> 5;
            short projY = 44 + tiltOffset + (short)(((long)(camHeight - terrainH) * invZ) >> 5);

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

    /* 4. Pack 100x100 Voxel Frame into 200x200 64-Shade 8x8 Bayer Dithered Output */
    for (y = 0; y < VIZ_H; y++) {
        unsigned char *dstRow = bits + y * VIZ_ROWBYTES;
        const unsigned char *srcRow = colBuf + (y >> 1) * 100;
        short ry = y & 7;
        AsmPackDitherRow25(
            dstRow,
            srcRow,
            gDitherP0[ry],
            gDitherP1[ry],
            gDitherP2[ry],
            gDitherP3[ry]
        );
        dstRow[25] = 0xFF;
    }

    /* 5. Cockpit HUD Waveform Overlay in Top Sky */
    for (b = 0; b < 16; b++) {
        short hx = 22 + b * 10;
        short hh = gEqBands[b] >> 3;
        DrawLineWhite(bits, hx, 12, hx, 12 - hh);
    }
}

#if 0 /* Archived Visualizer Modes 8, 9, 10 */
/* ============================================================================
 * MODE 8: 3D Millennium Falcon Hyperdrive Jump & Space Trench Run
 * - 64-shade dithered volumetric slipstream nebula tunnel
 * - 3D banking/rolling cockpit camera with swaying vanishing point
 * - 5 3D octagonal space-gate frames flying out of deep space past the camera
 * - 128 3D stars stretching into multi-width hyperdrive streaks + comet flares
 * ============================================================================ */

static void RenderMode8Hyperspace(unsigned char *bits)
{
    short i, x, y, g;
    unsigned char rowVals[100];
    short targetBoost, warpSpeed, tailStretch;
    unsigned char rollAng;
    short sinR, cosR;
    short vpX, vpY, lutOx, lutOy;
    unsigned char slipScroll;

    /* Smoothly ramp hyperdrive boost on bass kicks & audio energy */
    targetBoost = (gBassPulse >> 1) + (gAudioEnergy >> 2) + (gBeatHit ? 22 : 0);
    if (targetBoost > gWarpBoostSmooth) {
        gWarpBoostSmooth = (gWarpBoostSmooth + targetBoost * 3) >> 2;
    } else {
        gWarpBoostSmooth = (gWarpBoostSmooth * 7 + targetBoost) >> 3;
    }

    warpSpeed   = 4 + (gWarpBoostSmooth >> 1);
    tailStretch = 8 + (gWarpBoostSmooth * 2);

    /* 3D Cockpit Roll & Pitch/Yaw Vanishing Point Sway */
    rollAng = (unsigned char)((FAST_SIN((unsigned char)(gFrameCount * 2)) >> 2) + (gFrameCount >> 1));
    sinR    = FAST_SIN(rollAng);
    cosR    = FAST_COS(rollAng);
    vpX     = 100 + ((FAST_SIN((unsigned char)(gFrameCount * 2)) * 22) >> 7);
    vpY     = 100 + ((FAST_COS((unsigned char)(gFrameCount * 3)) * 18) >> 7);

    /* 1. Render 64-Shade Dithered Volumetric Hyperspace Slipstream Background */
    if (gTunnelAngle && gTunnelDepth && gTunnelFog) {
        lutOx = 64 - (vpX >> 1);
        lutOy = 64 - (vpY >> 1);
        if (lutOx < 0) lutOx = 0;
        if (lutOx > 27) lutOx = 27;
        if (lutOy < 0) lutOy = 0;
        if (lutOy > 27) lutOy = 27;
        slipScroll = (unsigned char)(gFrameCount * 7 + gWarpBoostSmooth);

        for (y = 0; y < VIZ_H; y++) {
            unsigned char *dstRow = bits + y * VIZ_ROWBYTES;
            unsigned short rowBase = (unsigned short)(((y >> 1) + lutOy) * TUNNEL_LUT_W + lutOx);
            const unsigned char *angPtr = gTunnelAngle + rowBase;
            const unsigned char *depPtr = gTunnelDepth + rowBase;
            const unsigned char *fogPtr = gTunnelFog   + rowBase;
            short ry = y & 7;

            for (x = 0; x < 100; x++) {
                unsigned char ang = (unsigned char)(angPtr[x] + rollAng);
                unsigned char dep = depPtr[x];
                unsigned char fog = fogPtr[x];
                /* Radial slipstream streaks along `ang * 8` rushing outward with `dep + slipScroll` */
                short ray = FAST_SIN((unsigned char)(ang * 8)) + FAST_COS((unsigned char)(ang * 13 - (dep >> 1)));
                short wave = gPlasmaLut[(unsigned char)(dep + slipScroll)];
                short val = ((ray + 256) * wave) >> 10;
                /* Brighten slipstream walls when hyperdrive boost kicks in */
                val = (val * (10 + (gWarpBoostSmooth >> 1)) * fog) >> 11;
                if (val > 36) val = 36;
                rowVals[x] = (unsigned char)val;
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
    } else {
        AsmClearBlack5200(bits);
    }

    /* 2. Render 5 3D Octagonal Space-Gate / Hyper-Corridor Frames Flying Past Camera */
    for (g = 0; g < 5; g++) {
        short zGate = 220 - (((short)(gFrameCount * 5 + (gWarpBoostSmooth >> 1) + g * 44)) % 200);
        if (zGate > 18 && zGate < 210) {
            short gateCx = vpX + (((100 - vpX) * (220 - zGate)) >> 7);
            short gateCy = vpY + (((100 - vpY) * (220 - zGate)) >> 7);
            short eqBand = gEqBands[(g * 3) & 15] >> 2;
            short rad    = (short)(((long)(58 + eqBand) * 128L) / zGate);
            if (rad > 6 && rad < 135) {
                short prevX = 0, prevY = 0, firstX = 0, firstY = 0;
                short p;
                unsigned char gateSpin = (unsigned char)(rollAng + g * 12);
                for (p = 0; p < 8; p++) {
                    unsigned char a = (unsigned char)(p * 32 + gateSpin);
                    short gx = gateCx + ((FAST_COS(a) * rad) >> 7);
                    short gy = gateCy + ((FAST_SIN(a) * rad) >> 7);
                    if (p > 0) {
                        DrawLineWhite(bits, prevX, prevY, gx, gy);
                        if (zGate < 85) {
                            DrawLineBlack(bits, prevX + 1, prevY + 1, gx + 1, gy + 1);
                        }
                    } else {
                        firstX = gx;
                        firstY = gy;
                    }
                    prevX = gx;
                    prevY = gy;
                }
                DrawLineWhite(bits, prevX, prevY, firstX, firstY);
            }
        }
    }

    /* 3. Render 128 3D Roll-Rotated Stars with Multi-Width Hyperdrive Streaks & Flares */
    for (i = 0; i < NUM_WARP_STARS; i++) {
        short curZ = gWarpStars[i].z - warpSpeed;
        short tailZ;
        short rx, ry;
        short x0, y0, x1, y1;

        if (curZ <= 12) {
            curZ = 220 + ((short)(gFrameCount + i * 19) & 31);
            gWarpStars[i].x = ((short)((i * 97 + gFrameCount * 13) % 280)) - 140;
            gWarpStars[i].y = ((short)((i * 151 + gFrameCount * 17) % 280)) - 140;
            /* Keep a clear central flight tunnel so stars wrap around the cockpit */
            if (abs(gWarpStars[i].x) < 14 && abs(gWarpStars[i].y) < 14) {
                gWarpStars[i].x += (i & 1) ? 28 : -28;
            }
        }
        gWarpStars[i].z = curZ;
        tailZ = curZ + tailStretch;

        /* Apply 3D cockpit roll rotation to star coordinates */
        rx = (gWarpStars[i].x * cosR - gWarpStars[i].y * sinR) >> 7;
        ry = (gWarpStars[i].x * sinR + gWarpStars[i].y * cosR) >> 7;

        x0 = vpX + (short)(((long)rx * 136L) / tailZ);
        y0 = vpY + (short)(((long)ry * 136L) / tailZ);
        x1 = vpX + (short)(((long)rx * 136L) / curZ);
        y1 = vpY + (short)(((long)ry * 136L) / curZ);

        DrawLineWhite(bits, x0, y0, x1, y1);

        /* Close stars become thick 2px-3px blazing hyper-streaks with diamond flares */
        if (curZ < 95) {
            DrawLineWhite(bits, x0 + 1, y0, x1 + 1, y1);
            if (curZ < 55 && (unsigned short)x1 < (VIZ_W - 2) && (unsigned short)y1 < (VIZ_H - 2) && x1 >= 2 && y1 >= 2) {
                DrawLineWhite(bits, x0, y0 + 1, x1, y1 + 1);
                /* 5x5 Diamond Star-Flare Head */
                SetPixelWhite(bits, x1 - 2, y1);
                SetPixelWhite(bits, x1 + 2, y1);
                SetPixelWhite(bits, x1, y1 - 2);
                SetPixelWhite(bits, x1, y1 + 2);
                SetPixelWhite(bits, x1 - 1, y1 - 1);
                SetPixelWhite(bits, x1 + 1, y1 + 1);
            }
        }
    }
}

/* ============================================================================
 * MODE 9: 3D Snaking Organic Vortex Wormhole & Braided Spiral Web
 * (Inspired by the top reference image: a deep curving 3D fibrous spiral tunnel
 * plunging into a pitch-black abyss with a pinpoint core and 3D braided ribs)
 * ============================================================================ */

static void RenderMode9SpiralVortex(unsigned char *bits)
{
    short x, y, r, p;
    unsigned char rowVals[100];
    unsigned char rot1 = (unsigned char)(gFrameCount * 4 + (gAudioEnergy >> 3));
    unsigned char rot2 = (unsigned char)(-gFrameCount * 5 - (gBassPulse >> 2));
    unsigned char depthScroll = (unsigned char)(gFrameCount * 6 + (gBassPulse >> 1));
    short coreScreenX, coreScreenY;
    short lutOx, lutOy;
    short prevRingX[12], prevRingY[12];
    Boolean hasPrevRing = false;

    if (!gTunnelAngle || !gTunnelDepth || !gTunnelFog) {
        RenderMode4Tunnel(bits);
        return;
    }

    /* 3D Snaking Wormhole Singularity Center */
    coreScreenX = 100 + ((FAST_SIN((unsigned char)(gFrameCount * 2)) * 22) >> 7);
    coreScreenY = 100 + ((FAST_COS((unsigned char)(gFrameCount * 3)) * 18) >> 7);
    lutOx = 64 - (coreScreenX >> 1);
    lutOy = 64 - (coreScreenY >> 1);
    if (lutOx < 0) lutOx = 0;
    if (lutOx > 27) lutOx = 27;
    if (lutOy < 0) lutOy = 0;
    if (lutOy > 27) lutOy = 27;

    /* 1. Raycast 64-Shade Organic Fibrous Vortex Tunnel with Pitch-Black Central Abyss */
    for (y = 0; y < VIZ_H; y++) {
        unsigned char *dstRow = bits + y * VIZ_ROWBYTES;
        short lutY = (y >> 1) + lutOy;
        unsigned short rowBase = (unsigned short)(lutY * TUNNEL_LUT_W + lutOx);
        const unsigned char *angPtr = gTunnelAngle + rowBase;
        const unsigned char *depPtr = gTunnelDepth + rowBase;
        const unsigned char *fogPtr = gTunnelFog   + rowBase;
        short ry = y & 7;

        /* Per-scanline 3D S-curve warp so the vortex visibly bends in 3D space */
        short scanCurve = (FAST_SIN((unsigned char)((y >> 1) * 3 + gFrameCount * 3)) * 6) >> 7;

        for (x = 0; x < 100; x++) {
            unsigned char dep = depPtr[x];
            unsigned char fog = fogPtr[x];
            /* Depth-dependent 3D helical torsion */
            unsigned char twist = (unsigned char)((dep * 3 >> 1) + scanCurve);
            unsigned char ang   = (unsigned char)(angPtr[x] + twist);
            short bandVal       = gEqBands[(ang >> 4) & 15] >> 2;

            /* Interlocking fibrous spiral tendrils (matching the veined green/dark vortex!) */
            short arm1    = FAST_SIN((unsigned char)(ang * 6 + dep * 2 - rot1));
            short arm2    = FAST_SIN((unsigned char)(ang * 9 - dep * 3 + rot2));
            short fiber   = abs(arm1 - arm2); /* Sharp organic fibrous ridges (0..254) */
            short pulse   = FAST_COS((unsigned char)(ang * 2 + dep * 4 - depthScroll));

            short val = (fiber >> 2) + ((pulse + 128) >> 4) + bandVal;

            /* Deep pitch-black funnel in the center (dep > 96 fades smoothly into the abyss) */
            if (dep > 96) {
                short abyssAtten = 230 - dep;
                if (abyssAtten < 0) abyssAtten = 0;
                val = (val * abyssAtten) >> 7;
            }
            val = (val * fog) >> 5;
            if (val > 63) val = 63;
            rowVals[x] = (unsigned char)val;
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

    /* 2. Project 11 3D Braided Spiral Polygon Web Rings Flying Out of the Abyss */
    {
        short zSub = (short)((gFrameCount * 4 + (gBassPulse >> 1)) & 15);
        for (r = 10; r >= 0; r--) {
            short z = 24 + r * 16 - zSub;
            if (z >= 14 && z <= 190) {
                /* Each ring follows a curving 3D helical spine from (coreScreenX, coreScreenY) to camera */
                unsigned char spinePhase = (unsigned char)((z >> 1) + gFrameCount * 3);
                short curveWeight = 190 - z;
                short ringCx = coreScreenX + (((100 - coreScreenX) * curveWeight) >> 7) +
                               (((FAST_SIN(spinePhase) * curveWeight) >> 7) >> 2);
                short ringCy = coreScreenY + (((100 - coreScreenY) * curveWeight) >> 7) +
                               (((FAST_COS(spinePhase) * curveWeight) >> 7) >> 2);
                short curRingX[12], curRingY[12];
                unsigned char ringTwist = (unsigned char)(z * 2 - gFrameCount * 4);

                for (p = 0; p < 12; p++) {
                    unsigned char vAng = (unsigned char)(p * 21 + ringTwist);
                    short eqBump = gEqBands[(p + r) & 15] >> 2;
                    short rad3D  = 52 + eqBump + ((p & 1) ? (gBassPulse >> 2) : -(gBassPulse >> 3));
                    short rad2D  = (short)(((long)rad3D * 116L) / z);
                    curRingX[p] = ringCx + ((FAST_COS(vAng) * rad2D) >> 7);
                    curRingY[p] = ringCy + ((FAST_SIN(vAng) * rad2D) >> 7);
                }

                /* Draw 3D polygon ring & diagonal braided spiral web struts to previous ring */
                for (p = 0; p < 12; p++) {
                    short nextP = (p + 1 == 12) ? 0 : (p + 1);
                    if (z < 125) {
                        /* Shadow + Highlight double-contour rib for strong 3D relief */
                        DrawLineBlack(bits, curRingX[p] + 1, curRingY[p] + 1, curRingX[nextP] + 1, curRingY[nextP] + 1);
                        DrawLineWhite(bits, curRingX[p], curRingY[p], curRingX[nextP], curRingY[nextP]);
                    }
                    if (hasPrevRing) {
                        /* Braided spiral tendon connecting ring `r+1` to ring `r` */
                        DrawLineBlack(bits, prevRingX[p] + 1, prevRingY[p], curRingX[nextP] + 1, curRingY[nextP]);
                        DrawLineWhite(bits, prevRingX[p], prevRingY[p], curRingX[nextP], curRingY[nextP]);
                    }
                    prevRingX[p] = curRingX[p];
                    prevRingY[p] = curRingY[p];
                }
                hasPrevRing = true;
            }
        }
    }

    /* 3. Pinpoint Glowing Singularity Core Deep Inside the Black Abyss (like the red dot in image 1!) */
    for (y = -1; y <= 1; y++) {
        for (x = -1; x <= 1; x++) {
            SetPixelWhite(bits, coreScreenX + x, coreScreenY + y);
        }
    }
}

/* ============================================================================
 * MODE 10: 3D Psychedelic Ribbon Canyon & Contour Portal
 * (Inspired by the bottom reference image: flying through a curving 3D canyon
 * made of 30+ densely stacked undulating liquid contour slices opening into a
 * warped psychedelic portal in the distance)
 * ============================================================================ */

static void RenderMode10LiquidRibbons(unsigned char *bits)
{
    short x, y, step;
    unsigned char yTopBuf[100]; /* Front-to-back top ceiling occlusion horizon (0..100) */
    unsigned char yBotBuf[100]; /* Front-to-back bottom floor occlusion horizon (0..100) */
    unsigned char *colBuf = gVoxelScreen; /* 100x100 64-shade heap buffer */
    short scrollZ, bankTilt;
    short portalCx, portalCy;

    if (!colBuf) {
        RenderMode2Plasma(bits);
        return;
    }

    for (x = 0; x < 100; x++) {
        yTopBuf[x] = 0;
        yBotBuf[x] = 100;
    }

    scrollZ  = (short)(gFrameCount * 2 + (gBassPulse >> 2));
    bankTilt = (FAST_SIN((unsigned char)(gFrameCount * 2)) * 14) >> 7;
    portalCx = 50 + ((FAST_SIN((unsigned char)(gFrameCount * 3)) * 16) >> 7);
    portalCy = 50 + ((FAST_COS((unsigned char)(gFrameCount * 2)) * 10) >> 7);

    /*
     * 1. Front-to-Back 3D Curving Ribbon Slice Raycaster (30 Depth Slices):
     *    Each slice `step = 1..30` is a 3D cross-section of a warping liquid tube/canyon.
     *    Because we march front-to-back with `yTopBuf[x]` and `yBotBuf[x]` occlusion,
     *    near slices naturally overlap far slices with crisp 3D topological contour lines!
     */
    for (step = 1; step <= 30; step++) {
        short zWorld   = step + scrollZ;
        short invZ     = (short)(240 / (step + 3)); /* Perspective scale */
        /* 3D S-curve spine of the ribbon canyon */
        short sliceCx  = 50 + (((portalCx - 50) * step) >> 5) +
                         ((FAST_SIN((unsigned char)(zWorld * 9)) * step) >> 6);
        short sliceCy  = 50 + (((portalCy - 50) * step) >> 5) +
                         ((FAST_COS((unsigned char)(zWorld * 7)) * step) >> 6);

        unsigned char wavePhase1 = (unsigned char)(zWorld * 13 - gFrameCount * 4);
        unsigned char wavePhase2 = (unsigned char)(zWorld * 7  + gFrameCount * 3);
        short eqBand   = gEqBands[step & 15];
        short waveAmp  = 10 + (eqBand >> 2) + (gBassPulse >> 2);
        /* Base psychedelic shade of this depth slice (cycles smoothly with depth) */
        short baseShade = gPlasmaLut[(unsigned char)(zWorld * 18 - gFrameCount * 5)];
        /* Alternating high-contrast contour rim shade for the topological slice edge */
        unsigned char rimShade = ((zWorld & 1) != 0) ? 63 : 4;

        for (x = 0; x < 100; x++) {
            if (yTopBuf[x] < yBotBuf[x]) {
                short dx = x - sliceCx;
                short tilt = (dx * bankTilt) >> 5;
                /* Parabolic side-wall wrap so ribbons curve up the left & right canyon walls! */
                short wallWrap = (dx * dx) >> 6;
                short undulTop = ((FAST_SIN((unsigned char)(x * 5 + wavePhase1)) +
                                   FAST_COS((unsigned char)(x * 3 - wavePhase2))) * waveAmp) >> 8;
                short undulBot = ((FAST_COS((unsigned char)(x * 4 - wavePhase1)) +
                                   FAST_SIN((unsigned char)(x * 6 + wavePhase2))) * waveAmp) >> 8;

                /* Project 3D ceiling and floor ribbon heights at this depth slice */
                short openTop = 34 - wallWrap + undulTop;
                short openBot = 34 - wallWrap + undulBot;
                short ceilY, floorY;
                short sliceShade;

                if (openTop < 4) openTop = 4;
                if (openBot < 4) openBot = 4;

                ceilY  = sliceCy + tilt - ((openTop * invZ) >> 5);
                floorY = sliceCy + tilt + ((openBot * invZ) >> 5);

                if (ceilY < 0)   ceilY = 0;
                if (ceilY > 100) ceilY = 100;
                if (floorY < 0)   floorY = 0;
                if (floorY > 100) floorY = 100;

                sliceShade = baseShade + ((dx + 50) >> 2);
                if (sliceShade > 60) sliceShade = 60;
                if (sliceShade < 8)  sliceShade = 8;

                /* Fill visible Upper / Left-Right Ceiling Ribbon Terrace */
                if (ceilY > yTopBuf[x]) {
                    short yEnd = (ceilY > yBotBuf[x]) ? yBotBuf[x] : ceilY;
                    unsigned char *pCol = colBuf + yTopBuf[x] * 100 + x;
                    for (y = yTopBuf[x]; y < yEnd; y++) {
                        *pCol = (y == ceilY - 1) ? rimShade : (unsigned char)sliceShade;
                        pCol += 100;
                    }
                    yTopBuf[x] = (unsigned char)yEnd;
                }

                /* Fill visible Lower / Left-Right Floor Ribbon Terrace */
                if (floorY < yBotBuf[x]) {
                    short yStart = (floorY < yTopBuf[x]) ? yTopBuf[x] : floorY;
                    unsigned char *pCol = colBuf + yStart * 100 + x;
                    unsigned char botShade = (unsigned char)(63 - sliceShade + 6);
                    if (botShade > 60) botShade = 60;
                    for (y = yStart; y < yBotBuf[x]; y++) {
                        *pCol = (y == floorY) ? rimShade : botShade;
                        pCol += 100;
                    }
                    yBotBuf[x] = (unsigned char)yStart;
                }
            }
        }
    }

    /*
     * 2. Fill the Distant Portal Opening at the End of the Canyon with a Warped Chrome Plasma
     */
    for (x = 0; x < 100; x++) {
        if (yTopBuf[x] < yBotBuf[x]) {
            unsigned char *pCol = colBuf + yTopBuf[x] * 100 + x;
            for (y = yTopBuf[x]; y < yBotBuf[x]; y++) {
                unsigned char u = (unsigned char)((x - portalCx) * 6 + gFrameCount * 5);
                unsigned char v = (unsigned char)((y - portalCy) * 6 - gFrameCount * 7);
                *pCol = gPlasmaLut[(unsigned char)(u ^ v)];
                pCol += 100;
            }
        }
    }

    /* 3. Pack 100x100 64-Shade Buffer into 200x200 8x8 Bayer Dithered Output */
    for (y = 0; y < VIZ_H; y++) {
        unsigned char *dstRow = bits + y * VIZ_ROWBYTES;
        const unsigned char *srcRow = colBuf + (y >> 1) * 100;
        short ry = y & 7;
        AsmPackDitherRow25(
            dstRow,
            srcRow,
            gDitherP0[ry],
            gDitherP1[ry],
            gDitherP2[ry],
            gDitherP3[ry]
        );
        dstRow[25] = 0xFF;
    }

    /* 4. Render a Rotating 3D Crystal Octahedron Floating Inside the Distant Canyon Portal */
    {
        static const signed char kOctVerts[6][3] = {
            {  0, -28,   0 },
            {  0,  28,   0 },
            { -24,  0, -24 },
            {  24,  0, -24 },
            {  24,  0,  24 },
            { -24,  0,  24 }
        };
        static const unsigned char kOctEdges[12][2] = {
            {0,2}, {0,3}, {0,4}, {0,5},
            {1,2}, {1,3}, {1,4}, {1,5},
            {2,3}, {3,4}, {4,5}, {5,2}
        };
        short vx[6], vy[6];
        unsigned char ay = (unsigned char)(gFrameCount * 5);
        unsigned char ax = (unsigned char)(gFrameCount * 3);
        short syA = FAST_SIN(ay), cyA = FAST_COS(ay);
        short sxA = FAST_SIN(ax), cxA = FAST_COS(ax);
        short scale = 18 + (gBassPulse >> 2);
        short i;

        for (i = 0; i < 6; i++) {
            short x0 = (kOctVerts[i][0] * scale) >> 4;
            short y0 = (kOctVerts[i][1] * scale) >> 4;
            short z0 = (kOctVerts[i][2] * scale) >> 4;
            short x1 = (x0 * cyA + z0 * syA) >> 7;
            short z1 = (-x0 * syA + z0 * cyA) >> 7;
            short y2 = (y0 * cxA - z1 * sxA) >> 7;
            vx[i] = (portalCx * 2) + x1;
            vy[i] = (portalCy * 2) + y2;
        }
        for (i = 0; i < 12; i++) {
            short a = kOctEdges[i][0];
            short b = kOctEdges[i][1];
            DrawLineBlack(bits, vx[a] + 1, vy[a] + 1, vx[b] + 1, vy[b] + 1);
            DrawLineWhite(bits, vx[a], vy[a], vx[b], vy[b]);
        }
    }
}
#endif /* Archived Visualizer Modes 8, 9, 10 */

/* ============================================================================
 * Public Entry Point Called by Main UI / Cooperative Loop
 * ============================================================================ */

void VisualizerRenderFrame(
    short mode,
    Boolean isPlaying,
    long progressMs,
    const char *trackId,
    unsigned char *outBits
)
{
    if (!outBits) return;
    if (!gVizInited) {
        VisualizerInit();
    }

    AnalyzeAudioFrame(isPlaying, progressMs, trackId);

    switch (mode % VIZ_MODE_COUNT) {
        case VIZ_MODE_EQ:
            RenderMode1WinampEQ(outBits);
            break;
        case VIZ_MODE_PLASMA:
            RenderMode2Plasma(outBits);
            break;
        case VIZ_MODE_3D:
            RenderMode3Demoscene3D(outBits);
            break;
        case VIZ_MODE_TUNNEL:
            RenderMode4Tunnel(outBits);
            break;
        case VIZ_MODE_FIRE_SCOPE:
            RenderMode5FireScope(outBits);
            break;
        case VIZ_MODE_CHROME_SPHERES:
            RenderMode6ChromeSpheres(outBits);
            break;
        case VIZ_MODE_VOXEL_CANYON:
        default:
            RenderMode7VoxelCanyon(outBits);
            break;
    }
}
