#include <string.h>
#include "../infrared_app_i.h"

enum SubmenuIndex {
    SubmenuIndexUniversalRemotes,
    SubmenuIndexLearnNewRemote,
    SubmenuIndexSavedRemotes,
    SubmenuIndexGpioSettings,
    SubmenuIndexDebug
};

static void infrared_scene_start_submenu_callback(void* context, uint32_t index) {
    InfraredApp* infrared = context;
    view_dispatcher_send_custom_event(infrared->view_dispatcher, index);
}

// Tunnel R.A.T.: the category menu launches straight into one action with "TR:<Action>".
// The action is "pressed" for the user; backing out of it closes the app again.
static int32_t tr_pending = -1;
static bool tr_return_exits = false;

void infrared_scene_start_tr_request(const char* arg) {
    tr_pending = -1;
    tr_return_exits = false;
    if(!arg || strncmp(arg, "TR:", 3) != 0) return;
    const char* a = arg + 3;
    if(!strcmp(a, "Universal"))
        tr_pending = SubmenuIndexUniversalRemotes;
    else if(!strcmp(a, "Learn"))
        tr_pending = SubmenuIndexLearnNewRemote;
    else if(!strcmp(a, "Saved"))
        tr_pending = SubmenuIndexSavedRemotes;
    else if(!strcmp(a, "GpioSettings"))
        tr_pending = SubmenuIndexGpioSettings;
}

static bool
    infrared_scene_start_tr_handle(SceneManager* scene_manager, ViewDispatcher* view_dispatcher) {
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

void infrared_scene_start_on_enter(void* context) {
    InfraredApp* infrared = context;
    Submenu* submenu = infrared->submenu;
    SceneManager* scene_manager = infrared->scene_manager;

    submenu_add_item(
        submenu,
        "Universal Remotes",
        SubmenuIndexUniversalRemotes,
        infrared_scene_start_submenu_callback,
        infrared);
    submenu_add_item(
        submenu,
        "Learn New Remote",
        SubmenuIndexLearnNewRemote,
        infrared_scene_start_submenu_callback,
        infrared);
    submenu_add_item(
        submenu,
        "Saved Remotes",
        SubmenuIndexSavedRemotes,
        infrared_scene_start_submenu_callback,
        infrared);
    submenu_add_item(
        submenu,
        "GPIO Settings",
        SubmenuIndexGpioSettings,
        infrared_scene_start_submenu_callback,
        infrared);

    submenu_add_lockable_item(
        submenu,
        "Debug RX",
        SubmenuIndexDebug,
        infrared_scene_start_submenu_callback,
        infrared,
        !infrared->app_state.is_debug_enabled,
        "Enable\n"
        "Settings >\n"
        "System >\n"
        "Debug");

    const uint32_t submenu_index =
        scene_manager_get_scene_state(scene_manager, InfraredSceneStart);
    submenu_set_selected_item(submenu, submenu_index);
    scene_manager_set_scene_state(scene_manager, InfraredSceneStart, SubmenuIndexUniversalRemotes);

    view_dispatcher_switch_to_view(infrared->view_dispatcher, InfraredViewSubmenu);
    infrared_scene_start_tr_handle(infrared->scene_manager, infrared->view_dispatcher);
}

bool infrared_scene_start_on_event(void* context, SceneManagerEvent event) {
    InfraredApp* infrared = context;
    SceneManager* scene_manager = infrared->scene_manager;

    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        const uint32_t submenu_index = event.event;
        scene_manager_set_scene_state(scene_manager, InfraredSceneStart, submenu_index);
        if(submenu_index == SubmenuIndexUniversalRemotes) {
            // Set file_path only once here so repeated usages of
            // "Load from Library File" have file browser focused on
            // last selected file, feels more intuitive
            furi_string_set(infrared->file_path, INFRARED_APP_FOLDER);
            scene_manager_next_scene(scene_manager, InfraredSceneUniversal);
        } else if(submenu_index == SubmenuIndexLearnNewRemote) {
            infrared->app_state.is_learning_new_remote = true;
            infrared->app_state.current_button_index = 0; // Reset index when starting new remote
            scene_manager_next_scene(scene_manager, InfraredSceneLearn);
        } else if(submenu_index == SubmenuIndexSavedRemotes) {
            furi_string_set(infrared->file_path, INFRARED_APP_FOLDER);
            scene_manager_next_scene(scene_manager, InfraredSceneRemoteList);
        } else if(submenu_index == SubmenuIndexGpioSettings) {
            scene_manager_next_scene(scene_manager, InfraredSceneGpioSettings);
        } else if(submenu_index == SubmenuIndexDebug) {
            scene_manager_next_scene(scene_manager, InfraredSceneDebug);
        }

        consumed = true;
    }

    return consumed;
}

void infrared_scene_start_on_exit(void* context) {
    InfraredApp* infrared = context;
    submenu_reset(infrared->submenu);
}
