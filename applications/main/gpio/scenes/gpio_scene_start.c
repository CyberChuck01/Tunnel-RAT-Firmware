#include <string.h>
#ifndef __GPIO_SCENE_START_H__
#define __GPIO_SCENE_START_H__

#include "../gpio_app_i.h"
#include <furi_hal_power.h>
#include <furi_hal_usb.h>
#include <dolphin/dolphin.h>

enum GpioItem {
    GpioItemUsbUart,
    GpioItemTest,
    GpioItemOtg,
    GpioItemI2CScanner,
    GpioItemI2CSfp,
};

enum GpioOtg {
    GpioOtgOff,
    GpioOtgOn,
    GpioOtgSettingsNum,
};

const char* const gpio_otg_text[GpioOtgSettingsNum] = {
    "OFF",
    "ON",
};

static void gpio_scene_start_var_list_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    GpioApp* app = context;
    if(index == GpioItemTest) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventManualControl);
    } else if(index == GpioItemUsbUart) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventUsbUart);
    } else if(index == GpioItemI2CScanner) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventI2CScanner);
    } else if(index == GpioItemI2CSfp) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventI2CSfp);
    }
}

static void gpio_scene_start_var_list_change_callback(VariableItem* item) {
    GpioApp* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, gpio_otg_text[index]);
    if(index == GpioOtgOff) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventOtgOff);
    } else if(index == GpioOtgOn) {
        view_dispatcher_send_custom_event(app->view_dispatcher, GpioStartEventOtgOn);
    }
}

// Tunnel R.A.T.: the category menu launches straight into one action with "TR:<Action>".
// The action is "pressed" for the user; backing out of it closes the app again.
static int32_t tr_pending = -1;
static bool tr_return_exits = false;

void gpio_scene_start_tr_request(const char* arg) {
    tr_pending = -1;
    tr_return_exits = false;
    if(!arg || strncmp(arg, "TR:", 3) != 0) return;
    const char* a = arg + 3;
    if(!strcmp(a, "UsbUart"))
        tr_pending = GpioStartEventUsbUart;
    else if(!strcmp(a, "ManualControl"))
        tr_pending = GpioStartEventManualControl;
    else if(!strcmp(a, "I2CScanner"))
        tr_pending = GpioStartEventI2CScanner;
    else if(!strcmp(a, "I2CSfp"))
        tr_pending = GpioStartEventI2CSfp;
}

static bool
    gpio_scene_start_tr_handle(SceneManager* scene_manager, ViewDispatcher* view_dispatcher) {
    if(tr_return_exits) {
        tr_return_exits = false;
        scene_manager_stop(scene_manager);
        view_dispatcher_stop(view_dispatcher);
        return true;
    }
    if(tr_pending >= 0) {
        uint32_t event = (uint32_t)tr_pending;
        tr_pending = -1;
        tr_return_exits = true;
        view_dispatcher_send_custom_event(view_dispatcher, event);
    }
    return false;
}

void gpio_scene_start_on_enter(void* context) {
    GpioApp* app = context;
    VariableItemList* var_item_list = app->var_item_list;

    VariableItem* item;
    variable_item_list_set_enter_callback(
        var_item_list, gpio_scene_start_var_list_enter_callback, app);

    variable_item_list_add(var_item_list, "USB-UART Bridge", 0, NULL, NULL);

    variable_item_list_add(var_item_list, "GPIO Manual Control", 0, NULL, NULL);

    item = variable_item_list_add(
        var_item_list,
        "5V on GPIO",
        GpioOtgSettingsNum,
        gpio_scene_start_var_list_change_callback,
        app);
    if(power_is_otg_enabled(app->power)) {
        variable_item_set_current_value_index(item, GpioOtgOn);
        variable_item_set_current_value_text(item, gpio_otg_text[GpioOtgOn]);
    } else {
        variable_item_set_current_value_index(item, GpioOtgOff);
        variable_item_set_current_value_text(item, gpio_otg_text[GpioOtgOff]);
    }

    variable_item_list_add(var_item_list, "I2C-Scanner", 0, NULL, NULL);
    variable_item_list_add(var_item_list, "I2C-SFP", 0, NULL, NULL);

    variable_item_list_set_selected_item(
        var_item_list, scene_manager_get_scene_state(app->scene_manager, GpioSceneStart));

    view_dispatcher_switch_to_view(app->view_dispatcher, GpioAppViewVarItemList);
    gpio_scene_start_tr_handle(app->scene_manager, app->view_dispatcher);
}

bool gpio_scene_start_on_event(void* context, SceneManagerEvent event) {
    GpioApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GpioStartEventOtgOn) {
            power_enable_otg(app->power, true);
        } else if(event.event == GpioStartEventOtgOff) {
            power_enable_otg(app->power, false);
        } else if(event.event == GpioStartEventManualControl) {
            scene_manager_set_scene_state(app->scene_manager, GpioSceneStart, GpioItemTest);
            scene_manager_next_scene(app->scene_manager, GpioSceneTest);
        } else if(event.event == GpioStartEventI2CScanner) {
            scene_manager_set_scene_state(app->scene_manager, GpioSceneStart, GpioItemI2CScanner);
            scene_manager_next_scene(app->scene_manager, GpioSceneI2CScanner);
        } else if(event.event == GpioStartEventI2CSfp) {
            scene_manager_set_scene_state(app->scene_manager, GpioSceneStart, GpioItemI2CSfp);
            scene_manager_next_scene(app->scene_manager, GpioSceneI2CSfp);
        } else if(event.event == GpioStartEventUsbUart) {
            scene_manager_set_scene_state(app->scene_manager, GpioSceneStart, GpioItemUsbUart);
            if(!furi_hal_usb_is_locked()) {
                dolphin_deed(DolphinDeedGpioUartBridge);
                scene_manager_next_scene(app->scene_manager, GpioSceneUsbUart);
            } else {
                scene_manager_next_scene(app->scene_manager, GpioSceneUsbUartCloseRpc);
            }
        }
        consumed = true;
    }
    return consumed;
}

void gpio_scene_start_on_exit(void* context) {
    GpioApp* app = context;
    variable_item_list_reset(app->var_item_list);
}

#endif // __GPIO_SCENE_START_H__
