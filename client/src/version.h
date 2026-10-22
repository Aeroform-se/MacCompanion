#ifndef VERSION_H
#define VERSION_H

/*
 * Single source of truth for MacCompanion Spotify 68k Client versioning.
 * Included by both C source (`main.c`) and Rez resources (`resources.r`).
 */
#define APP_VERSION_MAJOR_BCD   0x00
#define APP_VERSION_MINOR_BCD   0x10
#define APP_VERSION_STAGE       beta
#define APP_VERSION_NONFINAL    0x01

#define APP_VERSION_SHORT       "0.1 Beta"
#define APP_VERSION_LONG        "0.1 Beta (MacCompanion Spotify 68k)"
#define APP_VERSION_SLUG        "v0.1-beta"

#endif /* VERSION_H */
