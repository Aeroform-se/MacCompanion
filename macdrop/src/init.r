#include "Retro68.r"

/* Standalone System Extension (INIT) Code Resource */
type 'INIT' {
    RETRO68_CODE_TYPE
};

resource 'INIT' (128, sysheap, locked, nonpurgeable) {
    dontBreakAtEntry, $$read("MacDropINIT");
};

/* Shared Memory Location Pointer ('memr' 128) */
type 'memr' {
    longint;
};

resource 'memr' (128, purgeable) {
    0
};
