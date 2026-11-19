#include <string.h>
#include "../ibutton_i.h"
#include "ibutton/scenes/ibutton_scene.h"
#include <dolphin/dolphin.h>

enum SubmenuIndex {
    SubmenuIndexRead,
    SubmenuIndexSaved,
    SubmenuIndexAdd,
};

// Tunnel R.A.T.: the category menu launches straight into one action with "TR:<Action>".
// The action is "pressed" for the user; backing out of it closes the app again.
static int32_t tr_pending = -1;
static bool tr_return_exits = false;

void ibutton_scene_start_tr_request(const char* arg) {
    tr_pending = -1;
    tr_return_exits = false;
    if(!arg || strncmp(arg, "TR:", 3) != 0) return;
    const char* a = arg + 3;
    if(!strcmp(a, "Read"))
        tr_pending = SubmenuIndexRead;
    else if(!strcmp(a, "Saved"))
        tr_pending = SubmenuIndexSaved;
    else if(!strcmp(a, "AddManually"))
        tr_pending = SubmenuIndexAdd;
}

static bool
    ibutton_scene_start_tr_handle(SceneManager* scene_manager, ViewDispatcher* view_dispatcher) {
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

void ibutton_scene_start_on_enter(void* context) {
    iButton* ibutton = context;
    Submenu* submenu = ibutton->submenu;

    ibutton_reset_key(ibutton);

    submenu_add_item(submenu, "Read", SubmenuIndexRead, ibutton_submenu_callback, ibutton);
    submenu_add_item(submenu, "Saved", SubmenuIndexSaved, ibutton_submenu_callback, ibutton);
    submenu_add_item(submenu, "Add Manually", SubmenuIndexAdd, ibutton_submenu_callback, ibutton);

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(ibutton->scene_manager, iButtonSceneStart));

    view_dispatcher_switch_to_view(ibutton->view_dispatcher, iButtonViewSubmenu);
    ibutton_scene_start_tr_handle(ibutton->scene_manager, ibutton->view_dispatcher);
}

bool ibutton_scene_start_on_event(void* context, SceneManagerEvent event) {
    iButton* ibutton = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        scene_manager_set_scene_state(ibutton->scene_manager, iButtonSceneStart, event.event);
        consumed = true;
        if(event.event == SubmenuIndexRead) {
            scene_manager_next_scene(ibutton->scene_manager, iButtonSceneRead);
            dolphin_deed(DolphinDeedIbuttonRead);
        } else if(event.event == SubmenuIndexSaved) {
            scene_manager_next_scene(ibutton->scene_manager, iButtonSceneSelectKey);
        } else if(event.event == SubmenuIndexAdd) {
            scene_manager_next_scene(ibutton->scene_manager, iButtonSceneAddType);
        }
    }

    return consumed;
}

void ibutton_scene_start_on_exit(void* context) {
    iButton* ibutton = context;
    submenu_reset(ibutton->submenu);
}
