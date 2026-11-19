#include <string.h>
#include "../subghz_i.h"
#include "subghz_scene_start.h"
#include <dolphin/dolphin.h>
#include <loader/loader.h>

#include <lib/subghz/protocols/raw.h>

void subghz_scene_start_submenu_callback(void* context, uint32_t index) {
    SubGhz* subghz = context;
    view_dispatcher_send_custom_event(subghz->view_dispatcher, index);
}

// Tunnel R.A.T.: the category menu launches straight into one action with "TR:<Action>".
// The action is "pressed" for the user; backing out of it closes the app again.
static int32_t tr_pending = -1;
static bool tr_return_exits = false;

void subghz_scene_start_tr_request(const char* arg) {
    tr_pending = -1;
    tr_return_exits = false;
    if(!arg || strncmp(arg, "TR:", 3) != 0) return;
    const char* a = arg + 3;
    if(!strcmp(a, "Read"))
        tr_pending = SubmenuIndexRead;
    else if(!strcmp(a, "ReadRAW"))
        tr_pending = SubmenuIndexReadRAW;
    else if(!strcmp(a, "Saved"))
        tr_pending = SubmenuIndexSaved;
    else if(!strcmp(a, "AddManually"))
        tr_pending = SubmenuIndexAddManually;
    else if(!strcmp(a, "FrequencyAnalyzer"))
        tr_pending = SubmenuIndexFrequencyAnalyzer;
    else if(!strcmp(a, "RadioSettings"))
        tr_pending = SubmenuIndexExtSettings;
}

static bool
    subghz_scene_start_tr_handle(SceneManager* scene_manager, ViewDispatcher* view_dispatcher) {
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

void subghz_scene_start_on_enter(void* context) {
    SubGhz* subghz = context;
    if(subghz->state_notifications == SubGhzNotificationStateStarting) {
        subghz->state_notifications = SubGhzNotificationStateIDLE;
    }

    submenu_add_item(
        subghz->submenu, "Read", SubmenuIndexRead, subghz_scene_start_submenu_callback, subghz);
    submenu_add_item(
        subghz->submenu,
        "Read RAW",
        SubmenuIndexReadRAW,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu, "Saved", SubmenuIndexSaved, subghz_scene_start_submenu_callback, subghz);
    submenu_add_item(
        subghz->submenu,
        "Add Manually",
        SubmenuIndexAddManually,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Add Manually [Advanced]",
        SubmenuIndexAddManuallyAdvanced,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Frequency Analyzer",
        SubmenuIndexFrequencyAnalyzer,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_add_item(
        subghz->submenu,
        "Radio Settings",
        SubmenuIndexExtSettings,
        subghz_scene_start_submenu_callback,
        subghz);
    submenu_set_selected_item(
        subghz->submenu, scene_manager_get_scene_state(subghz->scene_manager, SubGhzSceneStart));

    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdMenu);
    subghz_scene_start_tr_handle(subghz->scene_manager, subghz->view_dispatcher);
}

bool subghz_scene_start_on_event(void* context, SceneManagerEvent event) {
    SubGhz* subghz = context;
    if(event.type == SceneManagerEventTypeBack) {
        //exit app
        scene_manager_stop(subghz->scene_manager);
        view_dispatcher_stop(subghz->view_dispatcher);
        return true;
    } else if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(subghz->scene_manager, SubGhzSceneStart, event.event);
        if(event.event == SubmenuIndexReadRAW) {
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReadRAW);
            return true;
        } else if(event.event == SubmenuIndexRead) {
            subghz_rx_key_state_set(subghz, SubGhzRxKeyStateIDLE);
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneReceiver);
            return true;
        } else if(event.event == SubmenuIndexSaved) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneSaved);
            return true;
        } else if(
            event.event == SubmenuIndexAddManually ||
            event.event == SubmenuIndexAddManuallyAdvanced) {
            const char* arg = (event.event == SubmenuIndexAddManuallyAdvanced) ?
                                  "AddManuallyAdvanced" :
                                  "AddManually";
            FuriString* self_path = furi_string_alloc();
            Loader* loader = furi_record_open(RECORD_LOADER);
            furi_check(loader_get_application_launch_path(loader, self_path));
            loader_enqueue_launch(
                loader,
                EXT_PATH("apps/assets/subghz_add_manually.fap"),
                arg,
                LoaderDeferredLaunchFlagGui);
            loader_enqueue_launch(
                loader, furi_string_get_cstr(self_path), arg, LoaderDeferredLaunchFlagGui);
            furi_record_close(RECORD_LOADER);
            furi_string_free(self_path);
            while(scene_manager_previous_scene(subghz->scene_manager))
                ;
            scene_manager_stop(subghz->scene_manager);
            view_dispatcher_stop(subghz->view_dispatcher);
            return true;
        } else if(event.event == SubmenuIndexFrequencyAnalyzer) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneFrequencyAnalyzer);
            dolphin_deed(DolphinDeedSubGhzFrequencyAnalyzer);
            return true;
        } else if(event.event == SubmenuIndexExtSettings) {
            scene_manager_next_scene(subghz->scene_manager, SubGhzSceneExtModuleSettings);
            return true;
        }
    }
    return false;
}

void subghz_scene_start_on_exit(void* context) {
    SubGhz* subghz = context;
    submenu_reset(subghz->submenu);
}
