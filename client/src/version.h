#ifndef VERSION_H
#define VERSION_H

/*
 * Single source of truth for SpotiMac 68k Client versioning.
 * Included by both C source (`main.c`) and Rez resources (`resources.r`).
 */
#define APP_NAME_STR            "SpotiMac"
#define APP_VERSION_MAJOR_BCD   0x00
#define APP_VERSION_MINOR_BCD   0x42
#define APP_VERSION_STAGE       final
#define APP_VERSION_NONFINAL    0x00

#define APP_VERSION_SHORT       "0.4.2"
#define APP_VERSION_CODENAME    "Whip the Llama's Macs"
#define APP_VERSION_LONG        "0.4.2 \"Whip the Llama's Macs\" (SpotiMac 68k)"
#define APP_VERSION_SLUG        "v0.4.2"

#endif /* VERSION_H */
