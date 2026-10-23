#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <MacTypes.h>

#define VIZ_W               200
#define VIZ_H               200
#define VIZ_ROWBYTES        26   /* ((200 + 15) / 16) * 2 = 26 bytes/row */
#define VIZ_BITMAP_BYTES    5200 /* 26 * 200 */

#define VIZ_MODE_EQ         0    /* 1: Winamp Classic 16-Band Spectrum + Scope + Reflection */
#define VIZ_MODE_PLASMA     1    /* 2: Geiss / Amiga Demoscene 64-Shade Dithered Plasma & Metaballs */
#define VIZ_MODE_3D         2    /* 3: 3D Shaded Audio-Morphing Torus + Warp Starfield + Cyber Grid */
#define VIZ_MODE_TUNNEL     3    /* 4: MilkDrop 3D Wormhole Vortex Tunnel + Radial Spectrum Burst */
#define VIZ_MODE_COUNT      4

/* Initialize lookup tables (Sine/Cosine, 8x8 Bayer Dither, Polar Tunnel, 3D Mesh) */
void VisualizerInit(void);

/* Advance simulation, analyze live audio / beat state, and render 200x200 1-bit frame */
void VisualizerRenderFrame(
    short mode,
    Boolean isPlaying,
    long progressMs,
    const char *trackId,
    unsigned char *outBits /* 5200-byte buffer (200x200, rowBytes=26) */
);

#endif /* VISUALIZER_H */
