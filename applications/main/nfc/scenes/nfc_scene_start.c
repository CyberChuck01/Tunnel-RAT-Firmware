#include <string.h>
#include "../nfc_app_i.h"
#include <dolphin/dolphin.h>

enum SubmenuIndex {
    SubmenuIndexRead,
    SubmenuIndexDetectReader,
    SubmenuIndexSaved,
    SubmenuIndexExtraAction,
    SubmenuIndexAddManually,
    SubmenuIndexDebug,
};

void nfc_scene_start_submenu_callback(void* context, uint32_t index) {
    NfcApp* nfc = context;

    view_dispatcher_send_custom_event(nfc->view_dispatcher, index);
}

// Tunnel R.A.T.: the category menu launches straight into one action with "TR:<Action>".
// The action is "pressed" for the user; backing out of it closes the app again.
static int32_t tr_pending = -1;
static bool tr_return_exits = false;

void nfc_scene_start_tr_request(const char* arg) {
    tr_pending = -1;
    tr_return_exits = false;
    if(!arg || strncmp(arg, "TR:", 3) != 0) return;
    const char* a = arg + 3;
    if(!strcmp(a, "Read"))
        tr_pending = SubmenuIndexRead;
    else if(!strcmp(a, "ExtractMfcKeys"))
        tr_pending = SubmenuIndexDetectReader;
    else if(!strcmp(a, "Saved"))
        tr_pending = SubmenuIndexSaved;
    else if(!strcmp(a, "ExtraActions"))
        tr_pending = SubmenuIndexExtraAction;
    else if(!strcmp(a, "AddManually"))
        tr_pending = SubmenuIndexAddManually;
}

static bool
    nfc_scene_start_tr_handle(SceneManager* scene_manager, ViewDispatcher* view_dispatcher) {
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

void nfc_scene_start_on_enter(void* context) {
    NfcApp* nfc = context;
    Submenu* submenu = nfc->submenu;

    // Clear file name and device contents
    furi_string_reset(nfc->file_name);
    nfc_device_clear(nfc->nfc_device);
    iso14443_3a_reset(nfc->iso14443_3a_edit_data);
    // Reset detected protocols list
    nfc_detected_protocols_reset(nfc->detected_protocols);

    submenu_add_item(submenu, "Read", SubmenuIndexRead, nfc_scene_start_submenu_callback, nfc);
    submenu_add_item(
        submenu,
        "Extract MFC Keys",
        SubmenuIndexDetectReader,
        nfc_scene_start_submenu_callback,
        nfc);
    submenu_add_item(submenu, "Saved", SubmenuIndexSaved, nfc_scene_start_submenu_callback, nfc);
    submenu_add_item(
        submenu, "Extra Actions", SubmenuIndexExtraAction, nfc_scene_start_submenu_callback, nfc);
    submenu_add_item(
        submenu, "Add Manually", SubmenuIndexAddManually, nfc_scene_start_submenu_callback, nfc);

    submenu_add_lockable_item(
        submenu,
        "Debug",
        SubmenuIndexDebug,
        nfc_scene_start_submenu_callback,
        nfc,
        !furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug),
        "Enable\n"
        "Settings >\n"
        "System >\n"
        "Debug");

    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(nfc->scene_manager, NfcSceneStart));

    view_dispatcher_switch_to_view(nfc->view_dispatcher, NfcViewMenu);
    nfc_scene_start_tr_handle(nfc->scene_manager, nfc->view_dispatcher);
}

bool nfc_scene_start_on_event(void* context, SceneManagerEvent event) {
    NfcApp* nfc = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        consumed = true;
        if(event.event == SubmenuIndexRead) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneDetect);
            dolphin_deed(DolphinDeedNfcRead);
        } else if(event.event == SubmenuIndexDetectReader) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneMfClassicDetectReader);
        } else if(event.event == SubmenuIndexSaved) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneFileSelect);
        } else if(event.event == SubmenuIndexExtraAction) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneExtraActions);
        } else if(event.event == SubmenuIndexAddManually) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneSetType);
        } else if(event.event == SubmenuIndexDebug) {
            scene_manager_next_scene(nfc->scene_manager, NfcSceneDebug);
        } else {
            consumed = false;
        }
        if(consumed) {
            scene_manager_set_scene_state(nfc->scene_manager, NfcSceneStart, event.event);
        }
    }
    return consumed;
}

void nfc_scene_start_on_exit(void* context) {
    NfcApp* nfc = context;

    submenu_reset(nfc->submenu);
}
