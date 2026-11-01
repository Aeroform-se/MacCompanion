#ifndef MACDROP_SHARED_DATA_H
#define MACDROP_SHARED_DATA_H

#include <MacTypes.h>
#include <Processes.h>

#define MACDROP_GESTALT_TAG 'MDRO'

typedef enum {
    CMD_NONE = 0,
    CMD_OPEN_WINDOW,
    CMD_HIDE_WINDOW,
    CMD_SEND_FILE,
    CMD_FIND_SERVER,
    CMD_CHOOSE_FOLDER,
    CMD_SET_NAME,
    CMD_TOGGLE_SOUND,
    CMD_TOGGLE_NOTIFY,
    CMD_ABOUT,
    CMD_QUIT
} MacDropCommand;

typedef struct {
    Boolean             DaemonRunning;      /* true when background daemon is active */
    Boolean             ChannelConnected;   /* true if connected to server */
    Boolean             TransferActive;     /* true during file transfer */
    Boolean             WindowVisible;      /* true if main window is open */
    Boolean             SoundEnabled;
    Boolean             NotifyEnabled;
    Boolean             UpdateUI;           /* set to true to trigger menu redraw */
    char                StatusText[64];     /* e.g. "Connected: MacCompanion" */
    char                ClientName[64];     /* local Macintosh client name */
    ProcessSerialNumber DaemonPSN;          /* background process serial number */
    MacDropCommand      PendingCommand;     /* command sent from menu to daemon */
} MacDropSharedData;

typedef MacDropSharedData **MacDropSharedDataHandle;

#endif /* MACDROP_SHARED_DATA_H */
