#ifndef MACDROP_FILE_IO_H
#define MACDROP_FILE_IO_H

#include <MacTypes.h>
#include <Files.h>

#define MAX_FOLDER_NAME     64
#define RECENT_FILES_COUNT  4

typedef struct {
    char            name[64];
    long            size;
    OSType          type;
    OSType          creator;
    unsigned long   timeTicks;
} RecentFileInfo;

extern short            gDestVRefNum;
extern long             gDestDirID;
extern char             gDestFolderName[MAX_FOLDER_NAME];
extern RecentFileInfo   gRecentFiles[RECENT_FILES_COUNT];
extern short            gRecentFileCount;

/* Preferences */
extern Boolean          gSoundEnabled;
extern Boolean          gNotifyEnabled;
extern unsigned long    gSavedServerIP;
extern unsigned short   gSavedServerPort;

/* Machine & Client Identity */
extern char             gClientID[64];
extern char             gClientName[64];
const char             *GetMacMachineName(void);
void                    SetClientName(const char *newName);

/* Initialize file manager, default Downloads folder, and load prefs */
OSErr InitFileManager(void);

/* Change destination folder name on boot disk */
OSErr SetDestinationFolderName(const char *folderName);

/* Standard Mac OS dialog to choose target folder */
Boolean ChooseDestinationFolder(void);

/* Save / load settings */
void SavePreferences(void);
void LoadPreferences(void);
Boolean TryLoadSiblingServerIP(unsigned long *outIP);

/* Stream Receiver State Machine */
typedef enum {
    RX_IDLE = 0,
    RX_HEADER,
    RX_DATA,
    RX_RSRC,
    RX_COMPLETE,
    RX_ERROR
} RxState;

typedef struct {
    RxState         state;
    char            fileName[64];
    OSType          fileType;
    OSType          fileCreator;
    unsigned short  finderFlags;
    unsigned long   dataLen;
    unsigned long   rsrcLen;
    unsigned long   creationDate;
    unsigned long   modDate;

    unsigned long   dataBytesWritten;
    unsigned long   dataBytesPadded;
    unsigned long   dataBytesTarget;
    unsigned long   rsrcBytesWritten;
    unsigned long   rsrcBytesPadded;
    unsigned long   rsrcBytesTarget;

    short           headerBytesRead;
    unsigned char   headerBuf[128];

    short           dataRefNum;
    short           rsrcRefNum;
    OSErr           lastError;

    ParamBlockRec   writePB;
    Boolean         writePending;
} MacBinaryRx;

void MacBinaryRx_Init(MacBinaryRx *rx);
OSErr MacBinaryRx_WaitWrite(MacBinaryRx *rx);
OSErr MacBinaryRx_ProcessBytes(MacBinaryRx *rx, const unsigned char *buf, long count);
void MacBinaryRx_Finish(MacBinaryRx *rx);
void MacBinaryRx_FlushDestVolume(void);
void MacBinaryRx_Abort(MacBinaryRx *rx);

/* Stream Transmitter State Machine (68k -> Server / Modern Mac) */
typedef enum {
    TX_IDLE = 0,
    TX_HEADER,
    TX_DATA,
    TX_RSRC,
    TX_COMPLETE,
    TX_ERROR
} TxState;

typedef struct {
    TxState         state;
    FSSpec          fileSpec;
    char            fileName[64];
    OSType          fileType;
    OSType          fileCreator;
    unsigned short  finderFlags;
    unsigned long   dataLen;
    unsigned long   rsrcLen;
    unsigned long   creationDate;
    unsigned long   modDate;

    unsigned long   dataBytesSent;
    unsigned long   dataBytesPadded;
    unsigned long   dataBytesTarget;
    unsigned long   rsrcBytesSent;
    unsigned long   rsrcBytesPadded;
    unsigned long   rsrcBytesTarget;

    short           headerBytesSent;
    unsigned char   headerBuf[128];

    short           dataRefNum;
    short           rsrcRefNum;
    OSErr           lastError;
} MacBinaryTx;

OSErr MacBinaryTx_Init(MacBinaryTx *tx, const FSSpec *spec);
long  MacBinaryTx_ReadBytes(MacBinaryTx *tx, unsigned char *outBuf, long maxCount);
void  MacBinaryTx_Finish(MacBinaryTx *tx);
void  MacBinaryTx_Abort(MacBinaryTx *tx);

#endif /* MACDROP_FILE_IO_H */
