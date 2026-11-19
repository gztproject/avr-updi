#pragma once

#include "updi/updi_session.h"

#include <dialogs/dialogs.h>
#include <expansion/expansion.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/modules/text_box.h>

#include <furi.h>

typedef enum {
    UpdiViewSubmenu,
    UpdiViewVarList,
    UpdiViewWidget,
    UpdiViewProgress,
    UpdiViewText,
} UpdiView;

typedef enum {
    UpdiSceneMenu,
    UpdiScenePins,
    UpdiSceneWiring,
    UpdiSceneWork,
    UpdiSceneCount,
} UpdiScene;

typedef enum {
    UpdiMenuRead,
    UpdiMenuFlash,
    UpdiMenuDump,
    UpdiMenuFuses,
    UpdiMenuPins,
    UpdiMenuWiring,
} UpdiMenuIndex;

typedef enum {
    UpdiOpRead,
    UpdiOpFlash,
    UpdiOpDump,
    UpdiOpFuses,
} UpdiOp;

typedef enum {
    UpdiEventProgress = 1,
    UpdiEventFinished = 2,
} UpdiEvent;

#define UPDI_DUMP_PATH "/ext/apps_data/updi_avr128/flash.hex"
#define UPDI_PINS_PATH "/ext/apps_data/updi_avr128/pins.txt"

typedef struct {
    char status[40];
    uint8_t percent;
} UpdiProgressModel;

typedef struct UpdiApp {
    Gui* gui;
    DialogsApp* dialogs;
    Expansion* expansion;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    Submenu* submenu;
    VariableItemList* var_list;
    Widget* widget;
    View* progress;
    TextBox* text_box;
    FuriThread* thread;
    FuriMutex* mutex;
    FuriString* hex_path;

    UpdiOp op;
    UpdiPins pins;
    char pins_label[24];
    volatile bool cancel;
    bool showing_result;
    char status[40];
    uint8_t percent;
    char result[1024];
} UpdiApp;

int32_t updi_worker(void* context);
void updi_pins_load(UpdiApp* app);
void updi_pins_save(UpdiApp* app);
const char* updi_pins_label(UpdiPins pins);
