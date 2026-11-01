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

/* ========================================================================= */
/* MacBinary II Transmitter (68k File -> Server Stream)                     */
/* ========================================================================= */

static unsigned short CalcMacBinaryCRC(const unsigned char *buf, short length)
{
    unsigned short crc = 0;
    short i, j;
    for (i = 0; i < length; i++) {
        crc ^= ((unsigned short)buf[i] << 8);
        for (j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = (crc << 1);
            }
        }
    }
    return crc;
}

static void WriteUInt32(unsigned char *p, unsigned long val)
{
    p[0] = (unsigned char)((val >> 24) & 0xFF);
    p[1] = (unsigned char)((val >> 16) & 0xFF);
    p[2] = (unsigned char)((val >> 8) & 0xFF);
    p[3] = (unsigned char)(val & 0xFF);
}

static void WriteOSType(unsigned char *p, OSType val)
{
    p[0] = (unsigned char)((val >> 24) & 0xFF);
    p[1] = (unsigned char)((val >> 16) & 0xFF);
    p[2] = (unsigned char)((val >> 8) & 0xFF);
    p[3] = (unsigned char)(val & 0xFF);
}

OSErr MacBinaryTx_Init(MacBinaryTx *tx, const FSSpec *spec)
{
    CInfoPBRec cinfo;
    OSErr err;
    short nameLen;
    unsigned short crc;

    if (!tx || !spec) return paramErr;
    memset(tx, 0, sizeof(MacBinaryTx));
    tx->fileSpec = *spec;

    /* Query file catalog info (dates, lengths, Finder info) */
    memset(&cinfo, 0, sizeof(cinfo));
    cinfo.hFileInfo.ioVRefNum   = spec->vRefNum;
    cinfo.hFileInfo.ioDirID     = spec->parID;
    cinfo.hFileInfo.ioNamePtr   = (StringPtr)spec->name;
    cinfo.hFileInfo.ioFDirIndex = 0;

    err = PBGetCatInfoSync(&cinfo);
    if (err != noErr) {
        tx->lastError = err;
        tx->state = TX_ERROR;
        return err;
    }

    nameLen = spec->name[0];
    if (nameLen > 31) nameLen = 31;
    memcpy(tx->fileName, &spec->name[1], nameLen);
    tx->fileName[nameLen] = '\0';

    tx->fileType     = cinfo.hFileInfo.ioFlFndrInfo.fdType;
    tx->fileCreator  = cinfo.hFileInfo.ioFlFndrInfo.fdCreator;
    tx->finderFlags  = cinfo.hFileInfo.ioFlFndrInfo.fdFlags;
    tx->dataLen      = cinfo.hFileInfo.ioFlLgLen;
    tx->rsrcLen      = cinfo.hFileInfo.ioFlRLgLen;
    tx->creationDate = cinfo.hFileInfo.ioFlCrDat;
    tx->modDate      = cinfo.hFileInfo.ioFlMdDat;

    tx->dataBytesTarget = ((tx->dataLen + 127) / 128) * 128;
    tx->rsrcBytesTarget = ((tx->rsrcLen + 127) / 128) * 128;

    /* Build standard 128-byte MacBinary II header */
    memset(tx->headerBuf, 0, 128);
    tx->headerBuf[0] = 0x00;
    tx->headerBuf[1] = (unsigned char)nameLen;
    memcpy(&tx->headerBuf[2], &spec->name[1], nameLen);

    WriteOSType(&tx->headerBuf[65], tx->fileType);
    WriteOSType(&tx->headerBuf[69], tx->fileCreator);

    tx->headerBuf[73] = (unsigned char)((tx->finderFlags >> 8) & 0xFF);
    tx->headerBuf[74] = 0x00;

    WriteUInt32(&tx->headerBuf[83], tx->dataLen);
    WriteUInt32(&tx->headerBuf[87], tx->rsrcLen);
    WriteUInt32(&tx->headerBuf[91], tx->creationDate);
    WriteUInt32(&tx->headerBuf[95], tx->modDate);

    tx->headerBuf[101] = (unsigned char)(tx->finderFlags & 0xFF);

    tx->headerBuf[122] = 0x81; /* MacBinary II */
    tx->headerBuf[123] = 0x81;

    crc = CalcMacBinaryCRC(tx->headerBuf, 124);
    tx->headerBuf[124] = (unsigned char)((crc >> 8) & 0xFF);
    tx->headerBuf[125] = (unsigned char)(crc & 0xFF);

    /* Open Data Fork */
    if (tx->dataLen > 0) {
        err = FSpOpenDF(spec, fsRdPerm, &tx->dataRefNum);
        if (err != noErr) {
            tx->lastError = err;
            tx->state = TX_ERROR;
            return err;
        }
    }

    /* Open Resource Fork */
    if (tx->rsrcLen > 0) {
        err = FSpOpenRF(spec, fsRdPerm, &tx->rsrcRefNum);
        if (err != noErr) {
            if (tx->dataRefNum) {
                FSClose(tx->dataRefNum);
                tx->dataRefNum = 0;
            }
            tx->lastError = err;
            tx->state = TX_ERROR;
            return err;
        }
    }

    tx->state = TX_HEADER;
    return noErr;
}

long MacBinaryTx_ReadBytes(MacBinaryTx *tx, unsigned char *outBuf, long maxCount)
{
    long totalRead = 0;
    OSErr err;

    if (!tx || !outBuf || maxCount <= 0) return 0;

    while (totalRead < maxCount && tx->state != TX_COMPLETE && tx->state != TX_ERROR) {
        if (tx->state == TX_HEADER) {
            short needed = 128 - tx->headerBytesSent;
            short take = (short)(maxCount - totalRead);
            if (take > needed) take = needed;

            memcpy(outBuf + totalRead, &tx->headerBuf[tx->headerBytesSent], take);
            tx->headerBytesSent += take;
            totalRead += take;

            if (tx->headerBytesSent >= 128) {
                if (tx->dataBytesTarget > 0) {
                    tx->state = TX_DATA;
                } else if (tx->rsrcBytesTarget > 0) {
                    tx->state = TX_RSRC;
                } else {
                    tx->state = TX_COMPLETE;
                }
            }
        } else if (tx->state == TX_DATA) {
            long remainingTarget = (long)(tx->dataBytesTarget - tx->dataBytesPadded);
            long space = maxCount - totalRead;
            long take = (space < remainingTarget) ? space : remainingTarget;

            long dataLeft = (long)(tx->dataLen - tx->dataBytesSent);
            long readAmount = (take < dataLeft) ? take : dataLeft;

            if (readAmount > 0) {
                long actRead = readAmount;
                err = FSRead(tx->dataRefNum, &actRead, outBuf + totalRead);
                if (err != noErr && err != eofErr) {
                    tx->lastError = err;
                    tx->state = TX_ERROR;
                    return totalRead > 0 ? totalRead : -1;
                }
                tx->dataBytesSent += actRead;
                tx->dataBytesPadded += actRead;
                totalRead += actRead;
            } else {
                long padAmount = take;
                memset(outBuf + totalRead, 0, padAmount);
                tx->dataBytesPadded += padAmount;
                totalRead += padAmount;
            }

            if (tx->dataBytesPadded >= tx->dataBytesTarget) {
                if (tx->dataRefNum) {
                    FSClose(tx->dataRefNum);
                    tx->dataRefNum = 0;
                }
                if (tx->rsrcBytesTarget > 0) {
                    tx->state = TX_RSRC;
                } else {
                    tx->state = TX_COMPLETE;
                }
            }
        } else if (tx->state == TX_RSRC) {
            long remainingTarget = (long)(tx->rsrcBytesTarget - tx->rsrcBytesPadded);
            long space = maxCount - totalRead;
            long take = (space < remainingTarget) ? space : remainingTarget;

            long rsrcLeft = (long)(tx->rsrcLen - tx->rsrcBytesSent);
            long readAmount = (take < rsrcLeft) ? take : rsrcLeft;

            if (readAmount > 0) {
                long actRead = readAmount;
                err = FSRead(tx->rsrcRefNum, &actRead, outBuf + totalRead);
                if (err != noErr && err != eofErr) {
                    tx->lastError = err;
                    tx->state = TX_ERROR;
                    return totalRead > 0 ? totalRead : -1;
                }
                tx->rsrcBytesSent += actRead;
                tx->rsrcBytesPadded += actRead;
                totalRead += actRead;
            } else {
                long padAmount = take;
                memset(outBuf + totalRead, 0, padAmount);
                tx->rsrcBytesPadded += padAmount;
                totalRead += padAmount;
            }

            if (tx->rsrcBytesPadded >= tx->rsrcBytesTarget) {
                if (tx->rsrcRefNum) {
                    FSClose(tx->rsrcRefNum);
                    tx->rsrcRefNum = 0;
                }
                tx->state = TX_COMPLETE;
            }
        }
    }

    return totalRead;
}

void MacBinaryTx_Finish(MacBinaryTx *tx)
{
    if (!tx) return;
    if (tx->dataRefNum) {
        FSClose(tx->dataRefNum);
        tx->dataRefNum = 0;
    }
    if (tx->rsrcRefNum) {
        FSClose(tx->rsrcRefNum);
        tx->rsrcRefNum = 0;
    }
}

void MacBinaryTx_Abort(MacBinaryTx *tx)
{
    if (!tx) return;
    MacBinaryTx_Finish(tx);
    tx->state = TX_ERROR;
}
