/*
 * visualizer_020.c — 68020/68030/68040 Native Visualizer Kernels
 * Compiled with -m68020 -O2 and linked into the unified SpotiMac binary.
 * Shares all lookup tables and state with visualizer.c (zero duplicate RAM).
 */
#define VIZ_BUILD_020 1
#include "visualizer.c"
