/*
 * 68020+ compiled instantiation of the NetFlim codec & blitter.
 * Compiled separately with -m68020 -msoft-float and dispatched at runtime
 * via Gestalt() inside codec.c so one single NetFlim binary serves 68000
 * through 68040 Macintoshes cleanly.
 */
#define NETFLIM_CODEC_020 1
#include "codec.c"
