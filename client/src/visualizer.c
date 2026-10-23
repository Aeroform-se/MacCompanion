/*
 * SpotiMac "Partylizer" — 68000 Assembly & Demoscene Visualizer Engine
 * Renders 200x200 1-bit dithered Winamp & Amiga/Mac demoscene visualizations
 * at 25-30+ FPS on a stock 16 MHz 68030 Macintosh SE/30.
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

static Boolean       gVizInited = false;

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
 * For row y_mod_8 (0..7) and intensity v (0..63):
 *   gDitherP0[y][v] holds bits 7..6 (1 = black, 0 = white)
 *   gDitherP1[y][v] holds bits 5..4
 *   gDitherP2[y][v] holds bits 3..2
 *   gDitherP3[y][v] holds bits 1..0
 * Combining `p0[v0] | p1[v1] | p2[v2] | p3[v3]` produces 8 individually
 * 1x1 Bayer-dithered pixels in a single byte with zero branches!
 */
static unsigned char gDitherP0[8][64];
static unsigned char gDitherP1[8][64];
static unsigned char gDitherP2[8][64];
static unsigned char gDitherP3[8][64];

/*
 * 17-level (0..16) 4x4 Ordered Dither Full-Byte Table for fast 3D polygon scanline fills:
 * `gShadeByte[shade][y & 3]` gives the 8-pixel byte (1 = black, 0 = white).
 */
static unsigned char gShadeByte[17][4];

/* Cosine-like smooth plasma wave palette mapping 0..255 -> 0..63 brightness */
static unsigned char gPlasmaLut[256];

/* Radial distance table for 100x100 delta (dx=0..99, dy=0..99) */
static unsigned char *gRadialDist = NULL; /* 100 * 100 = 10,000 bytes */

/* Polar coordinate tables for 100x100 Tunnel (centered at 50,50, padded to 128x128 for offset sway) */
#define TUNNEL_LUT_W 128
#define TUNNEL_LUT_H 128
static unsigned char *gTunnelAngle = NULL; /* 128 * 128 = 16,384 bytes */
static unsigned char *gTunnelDepth = NULL; /* 128 * 128 = 16,384 bytes */
static unsigned char *gTunnelFog   = NULL; /* 128 * 128 = 16,384 bytes */

/* ============================================================================
 * Audio Spectrum, Waveform & Beat State
 * ============================================================================ */

static unsigned char gRawPcm[256];
static signed char   gWave200[VIZ_W];     /* -20..+20 vertical scope offset */
static short         gEqBands[16];        /* 0..96 smoothed bar heights */
static short         gEqPeaks[16];        /* 0..96 peak hold positions */
static short         gEqPeakVel[16];      /* falling velocity of peak caps */
static short         gAudioEnergy = 0;    /* 0..100 overall energy */
static short         gBassPulse   = 0;    /* 0..64 bass kick envelope */
static short         gTreblePulse = 0;    /* 0..64 treble envelope */
static short         gAvgBass     = 24;
static unsigned long gFrameCount  = 0;

/* ============================================================================
 * 3D Scene State (Mode 3: 3D Torus + Starfield)
 * ============================================================================ */

#define NUM_STARS    48
typedef struct {
    short x; /* -120..+120 */
    short y; /* -120..+120 */
    short z; /* 16..255 */
} Star3D;

static Star3D gStars[NUM_STARS];
static unsigned char gAngleX = 20;
static unsigned char gAngleY = 0;
static unsigned char gAngleZ = 10;

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
        "move.b 0(%4, %2.l), %3\n\t"

        "moveq #0, %2\n\t"
        "move.b (%1)+, %2\n\t"
        "or.b 0(%5, %2.l), %3\n\t"

        "moveq #0, %2\n\t"
        "move.b (%1)+, %2\n\t"
        "or.b 0(%6, %2.l), %3\n\t"

        "moveq #0, %2\n\t"
        "move.b (%1)+, %2\n\t"
        "or.b 0(%7, %2.l), %3\n\t"

        "move.b %3, (%0)+\n\t"
        "dbf %8, 1b\n\t"
        : "+a"(dstRow), "+a"(rowVals), "=&d"(d0_tmp), "=&d"(d1_tmp)
        : "a"(p0), "a"(p1), "a"(p2), "a"(p3), "d"(count)
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
            /* Pair 0: pixels 0, 1 -> bits 7, 6 */
            b0 = (v > gBayer8x8[y][0]) ? 0 : 1;
            b1 = (v > gBayer8x8[y][1]) ? 0 : 1;
            gDitherP0[y][v] = (unsigned char)((b0 << 7) | (b1 << 6));

            /* Pair 1: pixels 2, 3 -> bits 5, 4 */
            b0 = (v > gBayer8x8[y][2]) ? 0 : 1;
            b1 = (v > gBayer8x8[y][3]) ? 0 : 1;
            gDitherP1[y][v] = (unsigned char)((b0 << 5) | (b1 << 4));

            /* Pair 2: pixels 4, 5 -> bits 3, 2 */
            b0 = (v > gBayer8x8[y][4]) ? 0 : 1;
            b1 = (v > gBayer8x8[y][5]) ? 0 : 1;
            gDitherP2[y][v] = (unsigned char)((b0 << 3) | (b1 << 2));

            /* Pair 3: pixels 6, 7 -> bits 1, 0 */
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
                /* 0 bit = white, 1 bit = black */
                if (v <= bayer4x4[y][x & 3]) {
                    byteVal |= (unsigned char)(0x80 >> x);
                }
            }
            gShadeByte[v][y] = byteVal;
        }
    }

    /* 3. Build smooth cosine palette LUT (0..255 -> 0..63) */
    for (i = 0; i < 256; i++) {
        short s = gSinTbl[i]; /* -127..+127 */
        short norm = (s + 128) >> 2; /* 0..63 */
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

    /* 5. Allocate & populate 128x128 Polar Wormhole Tunnel LUTs (centered at 64, 64) */
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

                /* Distance fog: dark in the center (r < 8), bright toward outer walls */
                fog = (short)r;
                if (fog > 63) fog = 63;
                gTunnelFog[idx] = (unsigned char)fog;
            }
        }
    }

    /* 6. Initialize 3D Starfield */
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

    if (isPlaying && AudioStreamIsRunning()) {
        gotLivePcm = AudioGetLiveSamples(gRawPcm, 256);
    }

    if (gotLivePcm) {
        /*
         * Genuine Real-Time Time-Domain / Filterbank Analysis of 256 Live PCM Samples:
         * - Convert unsigned 8-bit PCM (0..255) to signed (-128..+127)
         * - Compute 200-pixel oscilloscope trace (`gWave200`)
         * - Extract low-pass (bass), band-pass (mids), and high-pass difference (treble)
         *   across 16 sub-windows + zero-crossing frequency buckets!
         */
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
            short base = b << 4; /* 16 samples per window */
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
            /* Blend low, mid, and high energy according to band index b (0=sub-bass .. 15=air) */
            if (b < 4) {
                rawBands[b] = (subLow * (6 - b)) >> 4;
                bassAcc += rawBands[b];
            } else if (b < 10) {
                rawBands[b] = ((subLow >> 1) + subMid * 2) >> 4;
            } else {
                rawBands[b] = (subMid + subHigh * 2) >> 4;
                trebAcc += rawBands[b];
            }
            /* Add musical harmonic variation across neighboring bands */
            rawBands[b] = (rawBands[b] * (12 + ((b * 5) & 7))) >> 3;
            if (rawBands[b] > 94) rawBands[b] = 94;
        }

        gAudioEnergy = (short)(totalAbs >> 7);
        if (gAudioEnergy > 100) gAudioEnergy = 100;

        {
            short curBass = (short)(bassAcc >> 2);
            if (curBass > (gAvgBass + 8) && curBass > 18) {
                gBassPulse = 48;
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
        /*
         * Remote Spotify Connect Speaker Active:
         * Synthesize track-seeded musical beat, bassline, and 16-band spectrum
         * locked to `TickCount()` and `progressMs` so visuals dance vividly!
         */
        unsigned long t = TickCount();
        unsigned char seed = 0x5A;
        unsigned char beatPhase;
        if (trackId && trackId[0]) {
            const char *p = trackId;
            while (*p) { seed = (unsigned char)(seed * 33 + *p++); }
        }
        /* ~122 BPM beat pulse (every ~29 ticks) */
        beatPhase = (unsigned char)((t * 9) & 0xFF);
        gBassPulse = (beatPhase < 64) ? (short)((64 - beatPhase) >> 1) : 0;
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
        /* Paused: decay spectrum and flatten waveform smoothly */
        gAudioEnergy = 0;
        if (gBassPulse > 2) gBassPulse -= 2; else gBassPulse = 0;
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
            /* Fast attack */
            gEqBands[b] = (gEqBands[b] + target * 3) >> 2;
        } else {
            /* Smooth Winamp decay */
            gEqBands[b] -= 4;
            if (gEqBands[b] < target) gEqBands[b] = target;
        }
        if (gEqBands[b] < 2)  gEqBands[b] = 2;
        if (gEqBands[b] > 94) gEqBands[b] = 94;

        /* Peak-hold cap physics */
        if (gEqBands[b] >= gEqPeaks[b]) {
            gEqPeaks[b]   = gEqBands[b] + 2;
            if (gEqPeaks[b] > 96) gEqPeaks[b] = 96;
            gEqPeakVel[b] = -2; /* Brief hang time at apex */
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

    /* 1. Top Oscilloscope Bay (y = 5..49, center axis at y = 27) */
    for (x = 6; x < 194; x += 4) {
        SetPixelWhite(bits, x, 27);
    }

    for (x = 4; x < 196; x++) {
        short curY = 27 + gWave200[x];
        short yMin = (curY < prevY) ? curY : prevY;
        short yMax = (curY > prevY) ? curY : prevY;

        if (yMin < 4)  yMin = 4;
        if (yMax > 50) yMax = 50;

        /* Solid white vertical segment connecting sample x-1 to x */
        for (y = yMin; y <= yMax; y++) {
            SetPixelWhite(bits, x, y);
        }
        /* Dithered phosphor glow trail above & below trace */
        if ((x & 1) == (short)(gFrameCount & 1)) {
            if (yMin > 5)  SetPixelWhite(bits, x, yMin - 2);
            if (yMax < 49) SetPixelWhite(bits, x, yMax + 2);
        }
        prevY = curY;
    }

    /* 2. Middle 16-Band Segmented Spectrum Analyzer (y = 54..152, baseline = 152) */
    for (b = 0; b < 16; b++) {
        short x0   = 5 + b * 12; /* 10px wide bar: x0 .. x0+9, 2px gap */
        short x1   = x0 + 9;
        short barH = gEqBands[b];
        short peakY = 152 - gEqPeaks[b];
        short dy;

        /* Draw segmented 2px LED blocks with 4-zone dithered gradient */
        for (dy = 0; dy < barH; dy++) {
            unsigned char *rowPtr;
            unsigned char pat;
            if ((dy % 3) == 2) continue; /* 1px black LED separator gap every 3px */

            y = 152 - dy;
            if (y < 54) break;
            rowPtr = bits + y * VIZ_ROWBYTES;

            if (dy >= 66) {
                /* Top peak zone: 100% solid white */
                pat = 0x00;
            } else if (dy >= 42) {
                /* Upper-mid zone: 75% bright dither */
                pat = (y & 1) ? 0x22 : 0x88;
            } else if (dy >= 18) {
                /* Mid zone: 50% checkerboard dither */
                pat = (y & 1) ? 0x55 : 0xAA;
            } else {
                /* Base zone: 37.5% warm dither */
                pat = (y & 1) ? 0x77 : 0xDD;
            }

            FillSpanPattern(rowPtr, x0, x1, pat);
            /* Crisp 1px white left & right rim on every LED block so bars pop sharply */
            SetPixelWhite(bits, x0, y);
            SetPixelWhite(bits, x1, y);
        }

        /* Draw 2px-tall solid white floating Peak-Hold Cap */
        if (peakY >= 54 && peakY <= 151) {
            FillSpanPattern(bits + peakY * VIZ_ROWBYTES, x0, x1, 0x00);
            if (peakY > 54) {
                FillSpanPattern(bits + (peakY - 1) * VIZ_ROWBYTES, x0, x1, 0x00);
            }
        }

        /* 3. Bottom Rippling Water Reflection (y = 156..195, 40px tall) */
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

    /* Horizon separator line at y = 154 */
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

    /* Two orbiting metaball attractors in 100x200 space */
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
        short yHalf = y >> 1; /* 0..99 */
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

        /* Pack 100 64-level intensities into 25 1x1 Bayer-dithered bytes via 68000 assembly! */
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

    /* Carve high-contrast 3px live oscilloscope ribbon through the plasma center */
    {
        short prevY = 100 + (gWave200[0] * 2);
        for (x = 0; x < VIZ_W; x++) {
            short curY = 100 + (gWave200[x] * 2);
            short yMin = (curY < prevY) ? curY : prevY;
            short yMax = (curY > prevY) ? curY : prevY;
            if (yMin < 4)   yMin = 4;
            if (yMax > 195) yMax = 195;

            /* Black halo border above & below ribbon for maximum contrast on bright plasma */
            SetPixelBlack(bits, x, yMin - 2);
            SetPixelBlack(bits, x, yMin - 1);
            SetPixelBlack(bits, x, yMax + 1);
            SetPixelBlack(bits, x, yMax + 2);

            /* Bright white 2px core ribbon */
            for (y = yMin; y <= yMax + 1; y++) {
                SetPixelWhite(bits, x, y);
            }
            prevY = curY;
        }
    }
}

/* ============================================================================
 * MODE 3: Demoscene 3D Shaded Torus + Warp Starfield + Cyber Grid
 * ============================================================================ */

/* Scanline-fills a convex 2D quad with a 4x4 ordered dither shade (1..16) */
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

    AsmClearBlack5200(bits);

    /* 1. 3D Warp Starfield */
    starSpeed = 3 + (gAudioEnergy >> 4) + (gBassPulse >> 3);
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
            if (gStars[i].z < 110 && projX + 1 < VIZ_W) {
                SetPixelWhite(bits, projX + 1, projY);
            }
        }
    }

    /* 2. Perspective 3D Scrolling Cyber-Grid Floor (y = 164..198) */
    {
        short scroll = (short)((gFrameCount * 2) & 7);
        FillSpanPattern(bits + 164 * VIZ_ROWBYTES, 0, 199, (gBassPulse > 20) ? 0x00 : 0x55);
        for (i = 1; i <= 5; i++) {
            short gy = 164 + ((i * i * 28 + scroll * i) >> 2);
            if (gy > 164 && gy < 199) {
                FillSpanPattern(bits + gy * VIZ_ROWBYTES, 0, 199, (i >= 4) ? 0x55 : 0xAA);
            }
        }
        for (i = -5; i <= 5; i++) {
            short topX = 100 + i * 10;
            short botX = 100 + i * 34;
            DrawLineWhite(bits, topX, 164, botX, 199);
        }
    }

    /* 3. Advance 3D Torus Euler Angles (boosted by audio energy & bass hits) */
    gAngleX = (unsigned char)(gAngleX + 2 + (gBassPulse >> 4));
    gAngleY = (unsigned char)(gAngleY + 3 + (gAudioEnergy >> 5));
    gAngleZ = (unsigned char)(gAngleZ + 1);

    sxA = FAST_SIN(gAngleX); cxA = FAST_COS(gAngleX);
    syA = FAST_SIN(gAngleY); cyA = FAST_COS(gAngleY);
    szA = FAST_SIN(gAngleZ); czA = FAST_COS(gAngleZ);

    /* 4. Generate & Transform 48 Audio-Morphing Torus Vertices */
    for (r = 0; r < TORUS_RINGS; r++) {
        unsigned char uAngle = (unsigned char)(r * (256 / TORUS_RINGS));
        short cosU = FAST_COS(uAngle);
        short sinU = FAST_SIN(uAngle);
        /* Audio-reactive minor radius per ring driven by EQ spectrum! */
        short minorR = 17 + (gEqBands[r * 2] >> 3);

        for (s = 0; s < TORUS_SEGS; s++) {
            short idx = r * TORUS_SEGS + s;
            unsigned char vAngle = (unsigned char)(s * (256 / TORUS_SEGS));
            short cosV = FAST_COS(vAngle);
            short sinV = FAST_SIN(vAngle);
            short ringR = 44 + ((minorR * cosV) >> 7);

            /* Model-space coordinates (-70..+70) */
            short mx = (ringR * cosU) >> 7;
            short my = (ringR * sinU) >> 7;
            short mz = (minorR * sinV) >> 7;

            /* Rotate around X */
            short y1 = (my * cxA - mz * sxA) >> 7;
            short z1 = (my * sxA + mz * cxA) >> 7;

            /* Rotate around Y */
            short x2 = (mx * cyA + z1 * syA) >> 7;
            short z2 = (-mx * syA + z1 * cyA) >> 7;

            /* Rotate around Z */
            short x3 = (x2 * czA - y1 * szA) >> 7;
            short y3 = (x2 * szA + y1 * czA) >> 7;

            short dist = z2 + 210;
            if (dist < 64) dist = 64;

            vz[idx] = z2;
            px[idx] = 100 + (short)(((long)x3 * 185L) / dist);
            py[idx] = 88  + (short)(((long)y3 * 185L) / dist);
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

            /* 2D screen-space cross product for backface culling */
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

                /* Lighting from normal facing + depth + beat specular boost */
                shade = (short)(crossZ >> 4) + ((40 - f->avgZ) >> 3) + (gBassPulse >> 4);
                if (shade < 2)  shade = 2;
                if (shade > 16) shade = 16;
                f->shade = (unsigned char)shade;
            }
        }
    }

    /* 6. Painter's Algorithm Depth Sort (back-to-front: largest avgZ first) */
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

    /* Swaying camera offset in the 128x128 tunnel LUT (window is 100x100, so offset in 0..27) */
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

            /* Spiral wormhole coordinates warped by live frequency spectrum */
            unsigned char u = (unsigned char)(ang * 2 + (dep >> 1) + twistScroll);
            unsigned char v = (unsigned char)(dep + depthScroll - bandVal);
            unsigned char tex = gPlasmaLut[(unsigned char)(u ^ v)] + (bandVal >> 1);
            unsigned short lit = (unsigned short)(tex * fog) >> 6;

            if (lit > 63) lit = 63;
            rowVals[x] = (unsigned char)lit;
        }

        /* Pack 100 64-level tunnel intensities into 25 1x1 Bayer-dithered bytes via 68000 assembly! */
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

    /* Superimpose 16-Ray Radial Spectrum Starburst from the moving vortex eye */
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

    switch (mode & 3) {
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
        default:
            RenderMode4Tunnel(outBits);
            break;
    }
}
