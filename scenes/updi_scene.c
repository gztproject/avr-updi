#include "updi_app_i.h"

#include <dialogs/dialogs.h>

#include <stdio.h>

static bool updi_confirm(UpdiApp* app, const char* header, const char* text, const char* center) {
    DialogMessage* message = dialog_message_alloc();
    dialog_message_set_header(message, header, 64, 0, AlignCenter, AlignTop);
    dialog_message_set_text(message, text, 64, 32, AlignCenter, AlignCenter);
    dialog_message_set_buttons(message, "Cancel", center, NULL);
    DialogMessageButton button = dialog_message_show(app->dialogs, message);
    dialog_message_free(message);
    return button == DialogMessageButtonCenter;
}

static void updi_menu_callback(void* context, uint32_t index) {
    UpdiApp* app = context;

    if(index == UpdiMenuPins) {
        scene_manager_next_scene(app->scene_manager, UpdiScenePins);
        return;
    }

    if(index == UpdiMenuWiring) {
        scene_manager_next_scene(app->scene_manager, UpdiSceneWiring);
        return;
    }

    if(index == UpdiMenuFlash) {
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options, "hex", NULL);
        options.base_path = "/ext";
        FuriString* start = furi_string_alloc_set("/ext");
        bool picked = dialog_file_browser_show(app->dialogs, app->hex_path, start, &options);
        furi_string_free(start);
        if(!picked) return;
        if(!updi_confirm(
               app,
               "Erase and flash?",
               "Chip erase clears flash.\nEEPROM clears unless\nEESAVE is set.",
               "Flash")) {
            return;
        }
        app->op = UpdiOpFlash;
        scene_manager_next_scene(app->scene_manager, UpdiSceneWork);
        return;
    }

    if(index == UpdiMenuDump) {
        if(!updi_confirm(
               app,
               "Dump flash?",
               "Writes 128KB to\napps_data/avr_updi/\nflash.hex",
               "Dump")) {
            return;
        }
        app->op = UpdiOpDump;
        scene_manager_next_scene(app->scene_manager, UpdiSceneWork);
        return;
    }

    if(index == UpdiMenuRead) app->op = UpdiOpRead;
    if(index == UpdiMenuFuses) app->op = UpdiOpFuses;
    scene_manager_next_scene(app->scene_manager, UpdiSceneWork);
}

void updi_scene_menu_on_enter(void* context) {
    UpdiApp* app = context;
    submenu_reset(app->submenu);
    submenu_add_item(app->submenu, "Read device", UpdiMenuRead, updi_menu_callback, app);
    submenu_add_item(app->submenu, "Flash HEX", UpdiMenuFlash, updi_menu_callback, app);
    submenu_add_item(app->submenu, "Dump flash", UpdiMenuDump, updi_menu_callback, app);
    submenu_add_item(app->submenu, "Read fuses", UpdiMenuFuses, updi_menu_callback, app);
    snprintf(app->pins_label, sizeof(app->pins_label), "Pins %s", updi_pins_label(app->pins));
    submenu_add_item(app->submenu, app->pins_label, UpdiMenuPins, updi_menu_callback, app);
    submenu_add_item(app->submenu, "Wiring", UpdiMenuWiring, updi_menu_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewSubmenu);
}

bool updi_scene_menu_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void updi_scene_menu_on_exit(void* context) {
    UpdiApp* app = context;
    submenu_reset(app->submenu);
}

static void updi_pins_changed(VariableItem* item) {
    UpdiApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    app->pins = index == 1 ? UpdiPins13_14 : UpdiPins15_16;
    variable_item_set_current_value_text(item, updi_pins_label(app->pins));
    updi_pins_save(app);
}

void updi_scene_pins_on_enter(void* context) {
    UpdiApp* app = context;
    variable_item_list_reset(app->var_list);
    VariableItem* item = variable_item_list_add(app->var_list, "TX/RX", 2, updi_pins_changed, app);
    uint8_t index = app->pins == UpdiPins13_14 ? 1 : 0;
    variable_item_set_current_value_index(item, index);
    variable_item_set_current_value_text(item, updi_pins_label(app->pins));
    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewVarList);
}

bool updi_scene_pins_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void updi_scene_pins_on_exit(void* context) {
    UpdiApp* app = context;
    variable_item_list_reset(app->var_list);
}

void updi_scene_wiring_on_enter(void* context) {
    UpdiApp* app = context;
    widget_reset(app->widget);
    const char* text = app->pins == UpdiPins13_14 ?
                           "13 TX - 1k - UPDI\n"
                           "14 RX ------ UPDI\n"
                           "GND pin --- GND\n"
                           "9 3V3 optional\n"
                           "3.3V only. No 5V.\n"
                           "No 12V HV-UPDI." :
                           "15 TX - 1k - UPDI\n"
                           "16 RX ------ UPDI\n"
                           "GND pin --- GND\n"
                           "9 3V3 optional\n"
                           "3.3V only. No 5V.\n"
                           "No 12V HV-UPDI.";
    widget_add_string_multiline_element(
        app->widget, 0, 0, AlignLeft, AlignTop, FontSecondary, text);
    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewWidget);
}

bool updi_scene_wiring_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void updi_scene_wiring_on_exit(void* context) {
    UpdiApp* app = context;
    widget_reset(app->widget);
}

void updi_scene_work_on_enter(void* context) {
    UpdiApp* app = context;
    app->cancel = false;
    app->showing_result = false;
    app->percent = 0;
    snprintf(app->status, sizeof(app->status), "Connecting");
    app->result[0] = '\0';

    with_view_model(
        app->progress,
        UpdiProgressModel * model,
        {
            model->percent = 0;
            snprintf(model->status, sizeof(model->status), "Connecting");
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewProgress);
    app->thread = furi_thread_alloc_ex("UpdiWorker", 8 * 1024, updi_worker, app);
    furi_thread_start(app->thread);
}

bool updi_scene_work_on_event(void* context, SceneManagerEvent event) {
    UpdiApp* app = context;

    if(event.type == SceneManagerEventTypeBack) {
        if(app->showing_result) return false;
        app->cancel = true;
        return true;
    }

    if(event.type != SceneManagerEventTypeCustom) return false;

    if(event.event == UpdiEventProgress) {
        char status[40];
        uint8_t percent = 0;
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        snprintf(status, sizeof(status), "%s", app->status);
        percent = app->percent;
        furi_mutex_release(app->mutex);
        with_view_model(
            app->progress,
            UpdiProgressModel * model,
            {
                model->percent = percent;
                snprintf(model->status, sizeof(model->status), "%s", status);
            },
            true);
        return true;
    }

    if(event.event == UpdiEventFinished) {
        text_box_set_text(app->text_box, app->result);
        text_box_set_focus(app->text_box, TextBoxFocusStart);
        view_dispatcher_switch_to_view(app->view_dispatcher, UpdiViewText);
        app->showing_result = true;
        return true;
    }

    return false;
}

void updi_scene_work_on_exit(void* context) {
    UpdiApp* app = context;
    app->cancel = true;
    if(app->thread != NULL) {
        furi_thread_join(app->thread);
        furi_thread_free(app->thread);
        app->thread = NULL;
    }
    text_box_reset(app->text_box);
}

void (*const updi_on_enter_handlers[])(void*) = {
    updi_scene_menu_on_enter,
    updi_scene_pins_on_enter,
    updi_scene_wiring_on_enter,
    updi_scene_work_on_enter,
};

bool (*const updi_on_event_handlers[])(void*, SceneManagerEvent) = {
    updi_scene_menu_on_event,
    updi_scene_pins_on_event,
    updi_scene_wiring_on_event,
    updi_scene_work_on_event,
};

void (*const updi_on_exit_handlers[])(void*) = {
    updi_scene_menu_on_exit,
    updi_scene_pins_on_exit,
    updi_scene_wiring_on_exit,
    updi_scene_work_on_exit,
};

const SceneManagerHandlers updi_scene_handlers = {
    .on_enter_handlers = updi_on_enter_handlers,
    .on_event_handlers = updi_on_event_handlers,
    .on_exit_handlers = updi_on_exit_handlers,
    .scene_num = UpdiSceneCount,
};
