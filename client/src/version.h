#ifndef VERSION_H
#define VERSION_H

/*
 * Single source of truth for SpotiMac 68k Client versioning.
 * Included by both C source (`main.c`) and Rez resources (`resources.r`).
 */
#define APP_NAME_STR            "SpotiMac"
#define APP_VERSION_MAJOR_BCD   0x00
#define APP_VERSION_MINOR_BCD   0x35
#define APP_VERSION_STAGE       release
#define APP_VERSION_NONFINAL    0x01

#define APP_VERSION_SHORT       "0.3.5 RC"
#define APP_VERSION_CODENAME    "Business"
#define APP_VERSION_LONG        "0.3.5 RC \"Business\" (SpotiMac 68k)"
#define APP_VERSION_SLUG        "v0.3.5-rc"

#endif /* VERSION_H */
