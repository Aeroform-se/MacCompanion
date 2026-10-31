#include "file_io.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Folders.h>
#include <Errors.h>
#include <Gestalt.h>
#include <OSUtils.h>
#include <StandardFile.h>

short           gDestVRefNum       = 0;
long            gDestDirID         = 0;
char            gDestFolderName[MAX_FOLDER_NAME] = "Downloads";
RecentFileInfo  gRecentFiles[RECENT_FILES_COUNT];
short           gRecentFileCount   = 0;

Boolean         gSoundEnabled      = true;
Boolean         gNotifyEnabled     = true;

char            gClientID[64]      = "";
char            gClientName[64]    = "";

const char *GetMacMachineName(void)
{
    long mach = 0;
    if (Gestalt(gestaltMachineType, &mach) == noErr) {
        switch (mach) {
            case gestaltClassic:          return "Macintosh Classic (128k)";
            case gestaltMacXL:            return "Macintosh XL";
            case gestaltMac512KE:         return "Macintosh 512Ke";
            case gestaltMacPlus:          return "Macintosh Plus";
            case gestaltMacSE:            return "Macintosh SE";
            case gestaltMacII:            return "Macintosh II";
            case gestaltMacIIx:           return "Macintosh IIx";
            case gestaltMacIIcx:          return "Macintosh IIcx";
            case gestaltMacSE030:         return "Macintosh SE/30";
            case gestaltPortable:         return "Macintosh Portable";
            case gestaltMacIIci:          return "Macintosh IIci";
            case gestaltMacIIfx:          return "Macintosh IIfx";
            case gestaltMacClassic:       return "Macintosh Classic";
            case gestaltMacIIsi:          return "Macintosh IIsi";
            case gestaltMacLC:            return "Macintosh LC";
            case gestaltMacQuadra900:     return "Macintosh Quadra 900";
            case gestaltPowerBook170:     return "PowerBook 170";
            case gestaltMacQuadra700:     return "Macintosh Quadra 700";
            case gestaltClassicII:        return "Macintosh Classic II";
            case gestaltPowerBook100:     return "PowerBook 100";
            case gestaltPowerBook140:     return "PowerBook 140";
            case gestaltMacQuadra950:     return "Macintosh Quadra 950";
            case gestaltMacLCIII:         return "Macintosh LC III";
            case gestaltPowerBookDuo210:  return "PowerBook Duo 210";
            case gestaltMacCentris650:    return "Macintosh Centris 650";
            case gestaltPowerBookDuo230:  return "PowerBook Duo 230";
            case gestaltPowerBook180:     return "PowerBook 180";
            case gestaltPowerBook160:     return "PowerBook 160";
            case gestaltMacQuadra800:     return "Macintosh Quadra 800";
            case gestaltMacQuadra650:     return "Macintosh Quadra 650";
            case gestaltMacLCII:          return "Macintosh LC II";
            default:                      return "Macintosh 68k";
        }
    }
    return "Macintosh 68k";
}

unsigned long   gSavedServerIP   = 0;
unsigned short  gSavedServerPort = 0;

void SetClientName(const char *newName)
{
    if (newName && newName[0] != '\0') {
        strncpy(gClientName, newName, sizeof(gClientName) - 1);
        gClientName[sizeof(gClientName) - 1] = '\0';
    } else {
        strncpy(gClientName, GetMacMachineName(), sizeof(gClientName) - 1);
        gClientName[sizeof(gClientName) - 1] = '\0';
    }
    SavePreferences();
}

static void CToPString(const char *cStr, Str255 pStr)
{
    short len = (short)strlen(cStr);
    if (len > 255) len = 255;
    pStr[0] = (unsigned char)len;
    if (len > 0) {
        memcpy(&pStr[1], cStr, len);
    }
}


static OSErr GetDirectoryID(short vRefNum, long parentDirID, ConstStr255Param name, long *outDirID)
{
    CInfoPBRec cinfo;
    OSErr err;

    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.dirInfo.ioVRefNum   = vRefNum;
    cinfo.dirInfo.ioDrDirID   = parentDirID;
    cinfo.dirInfo.ioNamePtr   = (StringPtr)name;
    cinfo.dirInfo.ioFDirIndex = 0; /* use name and parent dir */

    err = PBGetCatInfoSync(&cinfo);
    if (err == noErr) {
        if (cinfo.dirInfo.ioFlAttrib & 0x10) { /* is directory */
            *outDirID = cinfo.dirInfo.ioDrDirID;
            return noErr;
        }
        return dirNFErr;
    }
    return err;
}

OSErr SetDestinationFolderName(const char *folderName)
{
    Str255 pFolderName;
    long dirID = 0;
    OSErr err;

    if (!folderName || folderName[0] == '\0') {
        folderName = "Downloads";
    }

    CToPString(folderName, pFolderName);
    err = DirCreate(gDestVRefNum, fsRtDirID, pFolderName, &dirID);
    if (err == dupFNErr) {
        err = GetDirectoryID(gDestVRefNum, fsRtDirID, pFolderName, &dirID);
    }

    if (err == noErr) {
        gDestDirID = dirID;
        strncpy(gDestFolderName, folderName, MAX_FOLDER_NAME - 1);
        gDestFolderName[MAX_FOLDER_NAME - 1] = '\0';
        SavePreferences();
    }
    return err;
}

typedef struct {
    char    folderName[MAX_FOLDER_NAME];
    Boolean soundEnabled;
    Boolean notifyEnabled;
    short   prefVersion;
    char    clientID[64];
    char    clientName[64];
    short   destVRefNum;
    long    destDirID;
    unsigned long  serverIP;
    unsigned short serverPort;
} MacDropPrefsRecord;

void LoadPreferences(void)
{
    short vRefNum = 0;
    long dirID = 0;
    short refNum;
    MacDropPrefsRecord prefs;
    long count = sizeof(prefs);
    OSErr err;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &vRefNum, &dirID);
    if (err == noErr) {
        err = HOpen(vRefNum, dirID, "\pMacDrop Prefs", fsRdPerm, &refNum);
        if (err == noErr) {
            long bytesRead = count;
            if (FSRead(refNum, &bytesRead, &prefs) == noErr && bytesRead >= 68) {
                if (prefs.folderName[0] != '\0') {
                    strncpy(gDestFolderName, prefs.folderName, MAX_FOLDER_NAME - 1);
                    gDestFolderName[MAX_FOLDER_NAME - 1] = '\0';
                }
                gSoundEnabled  = prefs.soundEnabled;
                gNotifyEnabled = prefs.notifyEnabled;
                if (bytesRead >= 68 + 64 + 64) {
                    if (prefs.clientID[0] != '\0') {
                        strncpy(gClientID, prefs.clientID, sizeof(gClientID) - 1);
                        gClientID[sizeof(gClientID) - 1] = '\0';
                    }
                    if (prefs.clientName[0] != '\0') {
                        strncpy(gClientName, prefs.clientName, sizeof(gClientName) - 1);
                        gClientName[sizeof(gClientName) - 1] = '\0';
                    }
                }
                if (bytesRead >= 68 + 64 + 64 + 6) {
                    if (prefs.destVRefNum != 0 && prefs.destDirID != 0) {
                        gDestVRefNum = prefs.destVRefNum;
                        gDestDirID   = prefs.destDirID;
                    }
                }
                if (bytesRead >= (long)sizeof(prefs)) {
                    if (prefs.serverIP != 0) {
                        gSavedServerIP   = prefs.serverIP;
                        gSavedServerPort = prefs.serverPort ? prefs.serverPort : 6800;
                    }
                }
            }
            FSClose(refNum);
        }
    }

    if (gClientName[0] == '\0') {
        strncpy(gClientName, GetMacMachineName(), sizeof(gClientName) - 1);
        gClientName[sizeof(gClientName) - 1] = '\0';
    }

    if (gClientID[0] == '\0') {
        unsigned long now = 0;
        GetDateTime(&now);
        snprintf(gClientID, sizeof(gClientID), "mac_%08lX", now);
        SavePreferences();
    }
}

void SavePreferences(void)
{
    short vRefNum = 0;
    long dirID = 0;
    short refNum;
    MacDropPrefsRecord prefs;
    long count = sizeof(prefs);
    OSErr err;

    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &vRefNum, &dirID);
    if (err != noErr) return;

    HDelete(vRefNum, dirID, "\pMacDrop Prefs");
    err = HCreate(vRefNum, dirID, "\pMacDrop Prefs", 'MCDP', 'pref');
    if (err == noErr) {
        err = HOpen(vRefNum, dirID, "\pMacDrop Prefs", fsWrPerm, &refNum);
        if (err == noErr) {
            memset(&prefs, 0, sizeof(prefs));
            snprintf(prefs.folderName, sizeof(prefs.folderName), "%s", gDestFolderName);
            prefs.soundEnabled  = gSoundEnabled;
            prefs.notifyEnabled = gNotifyEnabled;
            prefs.prefVersion   = 3;
            snprintf(prefs.clientID, sizeof(prefs.clientID), "%s", gClientID);
            snprintf(prefs.clientName, sizeof(prefs.clientName), "%s", gClientName);
            prefs.destVRefNum   = gDestVRefNum;
            prefs.destDirID     = gDestDirID;
            prefs.serverIP      = gSavedServerIP;
            prefs.serverPort    = gSavedServerPort ? gSavedServerPort : 6800;
            FSWrite(refNum, &count, &prefs);
            FSClose(refNum);
        }
    }
}

static pascal short PutFolderDlgHook(short item, DialogRef theDialog, void *yourDataPtr)
{
    if (item == sfHookFirstCall) {
        short itemType;
        Handle itemH = NULL;
        Rect itemBox;
        /* Item 1 is the primary confirmation button (Save) */
        GetDialogItem(theDialog, 1, &itemType, &itemH, &itemBox);
        if (itemH) {
            SetControlTitle((ControlHandle)itemH, "\pSelect");
        }
    }
    return item;
}

Boolean ChooseDestinationFolder(void)
{
    StandardFileReply reply;
    Point where = {-1, -1};
    DlgHookYDUPP hookUPP = NewDlgHookYDUPP(PutFolderDlgHook);

    CustomPutFile(
        "\pSelect destination folder for received files:",
        "\p(Select Current Folder)",
        &reply,
        0,
        where,
        hookUPP,
        NULL,
        NULL,
        NULL,
        NULL
    );

    DisposeDlgHookYDUPP(hookUPP);

    if (reply.sfGood) {
        CInfoPBRec cinfo;
        Str255 folderPName;
        memset(&cinfo, 0, sizeof(cinfo));
        cinfo.dirInfo.ioVRefNum   = reply.sfFile.vRefNum;
        cinfo.dirInfo.ioDrDirID   = reply.sfFile.parID;
        cinfo.dirInfo.ioNamePtr   = folderPName;
        cinfo.dirInfo.ioFDirIndex = -1; /* get info on directory itself */

        if (PBGetCatInfoSync(&cinfo) == noErr) {
            short len = folderPName[0];
            if (len > MAX_FOLDER_NAME - 1) len = MAX_FOLDER_NAME - 1;
            memcpy(gDestFolderName, &folderPName[1], len);
            gDestFolderName[len] = '\0';
        } else {
            strncpy(gDestFolderName, "Custom Folder", sizeof(gDestFolderName) - 1);
        }

        gDestVRefNum = reply.sfFile.vRefNum;
        gDestDirID   = reply.sfFile.parID;
        SavePreferences();
        return true;
    }
    return false;
}

OSErr InitFileManager(void)
{
    short bootVRef = 0;
    long bootDir = 0;
    OSErr err;

    /* Get default boot volume */
    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &bootVRef, &bootDir);
    if (err != noErr) {
        GetVol(NULL, &bootVRef);
    }
    gDestVRefNum = bootVRef;

    LoadPreferences();

    /* If loaded vRefNum and dirID are valid and directory still exists, use it */
    if (gDestVRefNum != 0 && gDestDirID != 0) {
        CInfoPBRec cinfo;
        Str255 pName;
        memset(&cinfo, 0, sizeof(cinfo));
        cinfo.dirInfo.ioVRefNum   = gDestVRefNum;
        cinfo.dirInfo.ioDrDirID   = gDestDirID;
        cinfo.dirInfo.ioNamePtr   = pName;
        cinfo.dirInfo.ioFDirIndex = -1;
        if (PBGetCatInfoSync(&cinfo) == noErr) {
            return noErr;
        }
    }

    /* Fallback: Ensure target folder exists on boot volume */
    gDestVRefNum = bootVRef;
    return SetDestinationFolderName(gDestFolderName);
}

void MacBinaryRx_Init(MacBinaryRx *rx)
{
    if (!rx) return;
    memset(rx, 0, sizeof(MacBinaryRx));
    rx->state = RX_HEADER;
}

void MacBinaryRx_Abort(MacBinaryRx *rx)
{
    if (!rx) return;
    if (rx->dataRefNum != 0) {
        FSClose(rx->dataRefNum);
        rx->dataRefNum = 0;
    }
    if (rx->rsrcRefNum != 0) {
        FSClose(rx->rsrcRefNum);
        rx->rsrcRefNum = 0;
    }
    if (rx->fileName[0] != '\0') {
        Str255 pName;
        CToPString(rx->fileName, pName);
        HDelete(gDestVRefNum, gDestDirID, pName);
    }
    rx->state = RX_ERROR;
}

static unsigned long ReadUInt32(const unsigned char *p)
{
    return ((unsigned long)p[0] << 24) |
           ((unsigned long)p[1] << 16) |
           ((unsigned long)p[2] << 8)  |
           (unsigned long)p[3];
}

static OSType ReadOSType(const unsigned char *p)
{
    return ((OSType)p[0] << 24) |
           ((OSType)p[1] << 16) |
           ((OSType)p[2] << 8)  |
           (OSType)p[3];
}

OSErr MacBinaryRx_ProcessBytes(MacBinaryRx *rx, const unsigned char *buf, long count)
{
    long offset = 0;
    Str255 pName;
    OSErr err;

    if (!rx || !buf || count <= 0) return noErr;

    while (offset < count && rx->state != RX_COMPLETE && rx->state != RX_ERROR) {
        if (rx->state == RX_HEADER) {
            short needed = 128 - rx->headerBytesRead;
            short take = (short)(count - offset);
            if (take > needed) take = needed;

            memcpy(&rx->headerBuf[rx->headerBytesRead], &buf[offset], take);
            rx->headerBytesRead += take;
            offset += take;

            if (rx->headerBytesRead == 128) {
                /* Parse 128-byte MacBinary II header */
                short nameLen = rx->headerBuf[1];
                if (nameLen > 31) nameLen = 31;
                if (nameLen < 1) nameLen = 1;

                memcpy(rx->fileName, &rx->headerBuf[2], nameLen);
                rx->fileName[nameLen] = '\0';

                rx->fileType    = ReadOSType(&rx->headerBuf[65]);
                rx->fileCreator = ReadOSType(&rx->headerBuf[69]);
                rx->finderFlags = ((unsigned short)rx->headerBuf[73] << 8) | rx->headerBuf[101];
                rx->dataLen     = ReadUInt32(&rx->headerBuf[83]);
                rx->rsrcLen     = ReadUInt32(&rx->headerBuf[87]);
                rx->creationDate= ReadUInt32(&rx->headerBuf[91]);
                rx->modDate     = ReadUInt32(&rx->headerBuf[95]);

                rx->dataBytesTarget = ((rx->dataLen + 127) / 128) * 128;
                rx->rsrcBytesTarget = ((rx->rsrcLen + 127) / 128) * 128;

                CToPString(rx->fileName, pName);

                /* If file exists, delete it first to ensure clean overwrite */
                HDelete(gDestVRefNum, gDestDirID, pName);

                err = HCreate(gDestVRefNum, gDestDirID, pName, rx->fileCreator, rx->fileType);
                if (err != noErr) {
                    rx->lastError = err;
                    rx->state = RX_ERROR;
                    return err;
                }

                err = HOpen(gDestVRefNum, gDestDirID, pName, fsWrPerm, &rx->dataRefNum);
                if (err != noErr) {
                    rx->lastError = err;
                    rx->state = RX_ERROR;
                    return err;
                }

                if (rx->rsrcLen > 0) {
                    err = HOpenRF(gDestVRefNum, gDestDirID, pName, fsWrPerm, &rx->rsrcRefNum);
                    if (err != noErr) {
                        rx->lastError = err;
                        rx->state = RX_ERROR;
                        return err;
                    }
                }

                if (rx->dataBytesTarget > 0) {
                    rx->state = RX_DATA;
                } else if (rx->rsrcBytesTarget > 0) {
                    rx->state = RX_RSRC;
                } else {
                    rx->state = RX_COMPLETE;
                }
            }
        } else if (rx->state == RX_DATA) {
            unsigned long neededTarget = rx->dataBytesTarget - rx->dataBytesPadded;
            long take = count - offset;
            long writeAmount;

            if ((unsigned long)take > neededTarget) {
                take = (long)neededTarget;
            }

            /* Only write actual non-padding bytes to the data fork */
            writeAmount = take;
            if (rx->dataBytesWritten + writeAmount > rx->dataLen) {
                writeAmount = rx->dataLen - rx->dataBytesWritten;
                if (writeAmount < 0) writeAmount = 0;
            }

            if (writeAmount > 0) {
                long actWrite = writeAmount;
                err = FSWrite(rx->dataRefNum, &actWrite, &buf[offset]);
                if (err != noErr) {
                    rx->lastError = err;
                    rx->state = RX_ERROR;
                    return err;
                }
                rx->dataBytesWritten += actWrite;
            }

            rx->dataBytesPadded += take;
            offset += take;

            if (rx->dataBytesPadded >= rx->dataBytesTarget) {
                FSClose(rx->dataRefNum);
                rx->dataRefNum = 0;

                if (rx->rsrcBytesTarget > 0) {
                    rx->state = RX_RSRC;
                } else {
                    rx->state = RX_COMPLETE;
                }
            }
        } else if (rx->state == RX_RSRC) {
            unsigned long neededTarget = rx->rsrcBytesTarget - rx->rsrcBytesPadded;
            long take = count - offset;
            long writeAmount;

            if ((unsigned long)take > neededTarget) {
                take = (long)neededTarget;
            }

            writeAmount = take;
            if (rx->rsrcBytesWritten + writeAmount > rx->rsrcLen) {
                writeAmount = rx->rsrcLen - rx->rsrcBytesWritten;
                if (writeAmount < 0) writeAmount = 0;
            }

            if (writeAmount > 0) {
                long actWrite = writeAmount;
                err = FSWrite(rx->rsrcRefNum, &actWrite, &buf[offset]);
                if (err != noErr) {
                    rx->lastError = err;
                    rx->state = RX_ERROR;
                    return err;
                }
                rx->rsrcBytesWritten += actWrite;
            }

            rx->rsrcBytesPadded += take;
            offset += take;

            if (rx->rsrcBytesPadded >= rx->rsrcBytesTarget) {
                FSClose(rx->rsrcRefNum);
                rx->rsrcRefNum = 0;
                rx->state = RX_COMPLETE;
            }
        }
    }

    return noErr;
}

void MacBinaryRx_Finish(MacBinaryRx *rx)
{
    FInfo finfo;
    Str255 pName;
    short i;

    if (!rx) return;

    if (rx->dataRefNum != 0) {
        FSClose(rx->dataRefNum);
        rx->dataRefNum = 0;
    }
    if (rx->rsrcRefNum != 0) {
        FSClose(rx->rsrcRefNum);
        rx->rsrcRefNum = 0;
    }

    CToPString(rx->fileName, pName);

    /* Set Finder Info */
    if (HGetFInfo(gDestVRefNum, gDestDirID, pName, &finfo) == noErr) {
        finfo.fdType    = rx->fileType;
        finfo.fdCreator = rx->fileCreator;
        finfo.fdFlags   = rx->finderFlags;
        HSetFInfo(gDestVRefNum, gDestDirID, pName, &finfo);
    }

    FlushVol(NULL, gDestVRefNum);

    /* Add to recent files queue */
    for (i = RECENT_FILES_COUNT - 1; i > 0; i--) {
        gRecentFiles[i] = gRecentFiles[i - 1];
    }
    strncpy(gRecentFiles[0].name, rx->fileName, sizeof(gRecentFiles[0].name) - 1);
    gRecentFiles[0].name[sizeof(gRecentFiles[0].name) - 1] = '\0';
    gRecentFiles[0].size      = rx->dataLen + rx->rsrcLen;
    gRecentFiles[0].type      = rx->fileType;
    gRecentFiles[0].creator   = rx->fileCreator;
    gRecentFiles[0].timeTicks = TickCount();

    if (gRecentFileCount < RECENT_FILES_COUNT) {
        gRecentFileCount++;
    }
}
