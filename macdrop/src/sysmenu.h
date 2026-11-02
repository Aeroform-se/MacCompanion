#ifndef MACDROP_SYSMENU_H
#define MACDROP_SYSMENU_H

#include <MacTypes.h>
#include <Menus.h>
#include <Events.h>
#include <Files.h>
#include "shared_data.h"

#define kPrefMenuID     -19999
#define kPrefIconID     129

typedef pascal void (*InsertMenuProcPtr)(MenuHandle mHdl, short beforeID);
typedef pascal void (*DrawMenuProcPtr)(void);
typedef pascal long (*MenuSelectProcPtr)(Point where);
typedef pascal void (*SystemMenuProcPtr)(long result);
typedef pascal Boolean (*WaitNextEventProcPtr)(EventMask mask, EventRecord *theEvent, unsigned long sleep, RgnHandle mouseRgn);

typedef struct {
    InsertMenuProcPtr    saveInsertMenu;
    DrawMenuProcPtr      saveDrawMenuBar;
    MenuSelectProcPtr    saveMenuSelect;
    SystemMenuProcPtr    saveSystemMenu;
    WaitNextEventProcPtr saveWaitNextEvent;

    short                menuID;
    short              **sysMenus;
    MenuHandle           mHdl;
    Handle               menuIcon;
    FSSpec               homeFile;
} GlobalsRec;

#endif /* MACDROP_SYSMENU_H */
