#include <string.h>
#include "../lfrfid_i.h"
#include <dolphin/dolphin.h>

static void lfrfid_scene_start_submenu_callback(void* context, uint32_t index) {
    LfRfid* app = context;

    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

// Tunnel R.A.T.: the category menu launches straight into one action with "TR:<Action>".
// The action is "pressed" for the user; backing out of it closes the app again.
static int32_t tr_pending = -1;
static bool tr_return_exits = false;

void lfrfid_scene_start_tr_request(const char* arg) {
    tr_pending = -1;
    tr_return_exits = false;
    if(!arg || strncmp(arg, "TR:", 3) != 0) return;
    const char* a = arg + 3;
    if(!strcmp(a, "Read"))
        tr_pending = LfRfidMenuIndexRead;
    else if(!strcmp(a, "Saved"))
        tr_pending = LfRfidMenuIndexSaved;
    else if(!strcmp(a, "AddManually"))
        tr_pending = LfRfidMenuIndexAddManually;
    else if(!strcmp(a, "ExtraActions"))
        tr_pending = LfRfidMenuIndexExtraActions;
}

static bool
    lfrfid_scene_start_tr_handle(SceneManager* scene_manager, ViewDispatcher* view_dispatcher) {
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

void lfrfid_scene_start_on_enter(void* context) {
    LfRfid* app = context;
    Submenu* submenu = app->submenu;

    submenu_add_item(
        submenu, "Read", LfRfidMenuIndexRead, lfrfid_scene_start_submenu_callback, app);
    submenu_add_item(
        submenu, "Saved", LfRfidMenuIndexSaved, lfrfid_scene_start_submenu_callback, app);
    submenu_add_item(
        submenu,
        "Add Manually",
        LfRfidMenuIndexAddManually,
        lfrfid_scene_start_submenu_callback,
        app);
    submenu_add_item(
        submenu,
        "Extra Actions",
        LfRfidMenuIndexExtraActions,
        lfrfid_scene_start_submenu_callback,
        app);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(app->scene_manager, LfRfidSceneStart));

    // clear key
    furi_string_reset(app->file_name);
    app->protocol_id = PROTOCOL_NO;
    app->read_type = LFRFIDWorkerReadTypeAuto;

    view_dispatcher_switch_to_view(app->view_dispatcher, LfRfidViewSubmenu);
    lfrfid_scene_start_tr_handle(app->scene_manager, app->view_dispatcher);
}

bool lfrfid_scene_start_on_event(void* context, SceneManagerEvent event) {
    LfRfid* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == LfRfidMenuIndexRead) {
            scene_manager_set_scene_state(
                app->scene_manager, LfRfidSceneStart, LfRfidMenuIndexRead);
            scene_manager_next_scene(app->scene_manager, LfRfidSceneRead);
            dolphin_deed(DolphinDeedRfidRead);
            consumed = true;
        } else if(event.event == LfRfidMenuIndexSaved) {
            // Like in the other apps, explicitly save the scene state
            // in each branch in case the user cancels loading a file.
            scene_manager_set_scene_state(
                app->scene_manager, LfRfidSceneStart, LfRfidMenuIndexSaved);
            furi_string_set(app->file_path, LFRFID_APP_FOLDER);
            scene_manager_next_scene(app->scene_manager, LfRfidSceneSelectKey);
            consumed = true;
        } else if(event.event == LfRfidMenuIndexAddManually) {
            scene_manager_set_scene_state(
                app->scene_manager, LfRfidSceneStart, LfRfidMenuIndexAddManually);
            scene_manager_next_scene(app->scene_manager, LfRfidSceneSaveType);
            consumed = true;
        } else if(event.event == LfRfidMenuIndexExtraActions) {
            scene_manager_set_scene_state(
                app->scene_manager, LfRfidSceneStart, LfRfidMenuIndexExtraActions);
            scene_manager_next_scene(app->scene_manager, LfRfidSceneExtraActions);
            consumed = true;
        }
    }

    return consumed;
}

void lfrfid_scene_start_on_exit(void* context) {
    LfRfid* app = context;

    submenu_reset(app->submenu);
}
