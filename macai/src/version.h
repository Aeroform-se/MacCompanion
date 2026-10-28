/*
 * version.h - Single source of truth for MacAI 68k Client version
 * Included by both C sources (main.c) and Rez resource compiler (resources.r).
 */
#ifndef MACAI_VERSION_H
#define MACAI_VERSION_H

#define APP_NAME               "MacAI"
#define APP_CODENAME           "Hello World!"

/* Numeric BCD fields for Mac OS 'vers' resource (0.1.9 Beta) */
#define APP_VERSION_MAJOR_BCD  0x00
#define APP_VERSION_MINOR_BCD  0x19
#define APP_VERSION_STAGE      0x60   /* beta */
#define APP_VERSION_NONFINAL   0x01

/* Human-readable strings */
#define APP_VERSION_STR        "0.1.9"
#define APP_VERSION_TAG        "v0.1.9-beta"
#define APP_VERSION_SHORT      "0.1.9 Beta"
#define APP_VERSION_LONG       "0.1.9 Beta (Hello World!)"

#endif /* MACAI_VERSION_H */
