#include <gui/modules/menu.h>
#include <gui/modules/submenu.h>
#include <assets_icons.h>
#include <applications.h>
#include <archive/helpers/archive_favorites.h>
#include <toolbox/run_parallel.h>
#include <archive/helpers/archive_helpers_ext.h>

#include "loader.h"
#include "loader_i.h"
#include "loader_menu.h"
#include "loader_menu_storage_i.h"

#include <flipper_application/flipper_application.h>
#include <toolbox/stream/file_stream.h>
#include <gui/modules/file_browser.h>
#include <core/dangerous_defines.h>
#include <momentum/momentum.h>
#include <gui/icon_i.h>
#include <m-list.h>

#define TAG "LoaderMenu"

typedef enum {
    LoaderMenuViewPrimary,
    LoaderMenuViewSettings,
    LoaderMenuViewCategory,
} LoaderMenuView;

#define TRAT_SUBDIR_MAX 128

struct LoaderMenu {
    FuriThread* thread;
    void (*closed_cb)(void*);
    void* context;

    Loader* loader;
    FuriPubSubSubscription* subscription;

    uint32_t selected_primary;
    uint32_t selected_setting;
    LoaderMenuView current_view;
    bool settings_only;

    // Tunnel R.A.T. category navigation (persists while apps run)
    int32_t category;
    uint32_t selected_category_item;
    char subdir[TRAT_SUBDIR_MAX];
};

static int32_t loader_menu_thread(void* p);

static void loader_pubsub_callback(const void* message, void* context) {
    const LoaderEvent* event = message;
    LoaderMenu* loader_menu = context;

    if(event->type == LoaderEventTypeApplicationBeforeLoad) {
        if(loader_menu->thread) {
            furi_thread_flags_set(furi_thread_get_id(loader_menu->thread), 0);
            furi_thread_join(loader_menu->thread);
            furi_thread_free(loader_menu->thread);
            loader_menu->thread = NULL;
        }
    } else if(event->type == LoaderEventTypeNoMoreAppsInQueue) {
        if(!loader_menu->thread) {
            loader_menu->thread = furi_thread_alloc_ex(TAG, 2048, loader_menu_thread, loader_menu);
            furi_thread_start(loader_menu->thread);
        }
    }
}

LoaderMenu* loader_menu_alloc(void (*closed_cb)(void*), void* context, bool settings_only) {
    LoaderMenu* loader_menu = malloc(sizeof(LoaderMenu));
    loader_menu->closed_cb = closed_cb;
    loader_menu->context = context;
    loader_menu->selected_primary = 0;
    loader_menu->selected_setting = 0;
    loader_menu->settings_only = settings_only;
    loader_menu->category = -1;
    loader_menu->selected_category_item = 0;
    loader_menu->subdir[0] = '\0';
    loader_menu->current_view = settings_only ? LoaderMenuViewSettings : LoaderMenuViewPrimary;
    loader_menu->loader = furi_record_open(RECORD_LOADER);

    view_holder_set_back_callback(loader_menu->loader->view_holder, NULL, NULL);
    view_holder_set_view(
        loader_menu->loader->view_holder, loading_get_view(loader_menu->loader->loading));

    loader_menu->subscription = furi_pubsub_subscribe(
        loader_get_pubsub(loader_menu->loader), loader_pubsub_callback, loader_menu);

    loader_menu->thread = furi_thread_alloc_ex(TAG, 2048, loader_menu_thread, loader_menu);
    furi_thread_start(loader_menu->thread);
    return loader_menu;
}

void loader_menu_free(LoaderMenu* loader_menu) {
    furi_assert(loader_menu);

    furi_pubsub_unsubscribe(loader_get_pubsub(loader_menu->loader), loader_menu->subscription);
    furi_record_close(RECORD_LOADER);

    if(loader_menu->thread) {
        furi_thread_join(loader_menu->thread);
        furi_thread_free(loader_menu->thread);
    }

    view_holder_set_view(loader_menu->loader->view_holder, NULL);

    free(loader_menu);
}

/* ============================================================================
 * Tunnel R.A.T. category menu
 *
 * Main menu = one icon per category (Bruce-style carousel).
 * OK on a category opens a single flat list that merges:
 *   - built-in firmware apps for that category
 *   - every .fap / .js found in its /ext/apps/<folder>
 *   - subfolders (e.g. GPIO/ESP), which open as nested lists
 * No more "Apps -> category folder -> app" double layer.
 * ==========================================================================*/

typedef struct {
    const char* label; // shown in the list
    const char* app; // app name to launch (internal or menu-external)
    const char* args; // launch args, e.g. "TR:Read" jumps straight into that action
} TratBuiltin;

typedef enum {
    TratCatNormal,
    TratCatMore,
    TratCatSettings,
} TratCatKind;

typedef struct {
    const char* label;
    const Icon* icon;
    const Icon* icon_big; // 32x32 rat badge for the Tunnel R.A.T. carousel
    TratCatKind kind;
    const TratBuiltin* builtins;
    size_t builtins_count;
    const char* const* dirs;
    size_t dirs_count;
} TratCategory;

#define TRAT_DIRS(...)                          \
    .dirs = (const char* const[]){__VA_ARGS__}, \
    .dirs_count = sizeof((const char* const[]){__VA_ARGS__}) / sizeof(const char*)
#define TRAT_BUILTINS(...)                          \
    .builtins = (const TratBuiltin[]){__VA_ARGS__}, \
    .builtins_count = sizeof((const TratBuiltin[]){__VA_ARGS__}) / sizeof(TratBuiltin)

static const TratCategory trat_categories[] = {
    {.label = "Sub-GHz",
     .icon = &A_Sub1ghz_14,
     TRAT_BUILTINS(
         {"Read", "Sub-GHz", "TR:Read"},
         {"Read RAW", "Sub-GHz", "TR:ReadRAW"},
         {"Frequency Analyzer", "Sub-GHz", "TR:FrequencyAnalyzer"},
         {"Saved", "Sub-GHz", "TR:Saved"},
         {"Add Manually", "Sub-GHz", "TR:AddManually"},
         {"Radio Settings", "Sub-GHz", "TR:RadioSettings"}),
     .icon_big = &I_CatBig_SubGhz_32x32,
     TRAT_DIRS("Sub-GHz")},
    {.label = "NFC",
     .icon = &A_NFC_14,
     TRAT_BUILTINS(
         {"Read", "NFC", "TR:Read"},
         {"Extract MFC Keys", "NFC", "TR:ExtractMfcKeys"},
         {"Saved", "NFC", "TR:Saved"},
         {"Extra Actions", "NFC", "TR:ExtraActions"},
         {"Add Manually", "NFC", "TR:AddManually"}),
     .icon_big = &I_CatBig_Nfc_32x32,
     TRAT_DIRS("NFC")},
    {.label = "RFID",
     .icon = &A_125khz_14,
     TRAT_BUILTINS(
         {"Read", "125 kHz RFID", "TR:Read"},
         {"Saved", "125 kHz RFID", "TR:Saved"},
         {"Add Manually", "125 kHz RFID", "TR:AddManually"},
         {"Extra Actions", "125 kHz RFID", "TR:ExtraActions"}),
     .icon_big = &I_CatBig_Rfid_32x32,
     TRAT_DIRS("RFID")},
    {.label = "Infrared",
     .icon = &A_Infrared_14,
     TRAT_BUILTINS(
         {"Universal Remotes", "Infrared", "TR:Universal"},
         {"Learn New Remote", "Infrared", "TR:Learn"},
         {"Saved Remotes", "Infrared", "TR:Saved"},
         {"IR GPIO Settings", "Infrared", "TR:GpioSettings"}),
     .icon_big = &I_CatBig_Infrared_32x32,
     TRAT_DIRS("Infrared")},
    {.label = "iButton",
     .icon = &A_iButton_14,
     TRAT_BUILTINS(
         {"Read", "iButton", "TR:Read"},
         {"Saved", "iButton", "TR:Saved"},
         {"Add Manually", "iButton", "TR:AddManually"}),
     .icon_big = &I_CatBig_IButton_32x32,
     TRAT_DIRS("iButton")},
    {.label = "BadUSB",
     .icon = &A_BadUsb_14,
     TRAT_BUILTINS({"Run Script", "Bad KB", NULL}, {"U2F Security Key", "U2F", NULL}),
     .icon_big = &I_CatBig_BadUsb_32x32,
     TRAT_DIRS("USB")},
    {.label = "Bluetooth",
     .icon = &I_Cat_Bluetooth_14x14,
     .icon_big = &I_CatBig_Bluetooth_32x32,
     TRAT_DIRS("Bluetooth")},
    {.label = "GPIO / WiFi",
     .icon = &A_GPIO_14,
     TRAT_BUILTINS(
         {"USB-UART Bridge", "GPIO", "TR:UsbUart"},
         {"Manual Control", "GPIO", "TR:ManualControl"},
         {"I2C Scanner", "GPIO", "TR:I2CScanner"},
         {"I2C SFP", "GPIO", "TR:I2CSfp"}),
     .icon_big = &I_CatBig_Gpio_32x32,
     TRAT_DIRS("GPIO")},
    {.label = "Tunnel R.A.T.",
     .icon = &I_Cat_TunnelRat_14x14,
     .icon_big = &I_CatBig_TunnelRat_32x32,
     TRAT_DIRS("Tunnel RAT")},
    {.label = "Games",
     .icon = &I_Cat_Games_14x14,
     .icon_big = &I_CatBig_Games_32x32,
     TRAT_DIRS("Games")},
    {.label = "Media",
     .icon = &I_Cat_Media_14x14,
     .icon_big = &I_CatBig_Media_32x32,
     TRAT_DIRS("Media")},
    {.label = "Tools",
     .icon = &I_Cat_Tools_14x14,
     TRAT_BUILTINS({"Archive", "Archive", NULL}),
     .icon_big = &I_CatBig_Tools_32x32,
     TRAT_DIRS("Tools", "Scripts")},
    {.label = "More",
     .icon = &I_Cat_More_14x14,
     .icon_big = &I_CatBig_More_32x32,
     .kind = TratCatMore},
    {.label = "Settings",
     .icon = &A_Settings_14,
     .icon_big = &I_CatBig_Settings_32x32,
     .kind = TratCatSettings},
};
#define TRAT_CATEGORIES_COUNT COUNT_OF(trat_categories)

typedef enum {
    TratEntryBuiltin, // launch by internal name
    TratEntryDir, // open nested list
    TratEntryFap, // launch by path
    TratEntryFile, // .js etc, run_with_default_app
} TratEntryKind;

typedef struct {
    TratEntryKind kind;
    FuriString* label;
    FuriString* target; // app name or absolute path
    FuriString* args; // launch args (empty = none)
    uint32_t order; // actions keep their defined order
} TratEntry;

typedef struct {
    LoaderMenu* loader_menu;
    Menu* primary_menu;
    Submenu* settings_menu;
    Submenu* category_menu;
    TratEntry* entries;
    size_t entries_count;
    size_t entries_cap;
} LoaderMenuApp;

static void loader_menu_start_args(const char* name, const char* args) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_detached_with_gui_error(loader, name, (args && args[0]) ? args : NULL);
    furi_record_close(RECORD_LOADER);
}

static void loader_menu_start(const char* name) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_start_detached_with_gui_error(loader, name, NULL);
    furi_record_close(RECORD_LOADER);
}

/* ---------- entries list helpers ---------- */

static void trat_entries_clear(LoaderMenuApp* app) {
    for(size_t i = 0; i < app->entries_count; i++) {
        furi_string_free(app->entries[i].label);
        furi_string_free(app->entries[i].target);
        furi_string_free(app->entries[i].args);
    }
    app->entries_count = 0;
}

static void trat_entries_free(LoaderMenuApp* app) {
    trat_entries_clear(app);
    free(app->entries);
    app->entries = NULL;
    app->entries_cap = 0;
}

static void trat_entries_add_ex(
    LoaderMenuApp* app,
    TratEntryKind kind,
    const char* label,
    const char* target,
    const char* args) {
    // Skip duplicates (same target + args)
    for(size_t i = 0; i < app->entries_count; i++) {
        if(furi_string_equal_str(app->entries[i].target, target) &&
           furi_string_equal_str(app->entries[i].args, args ? args : ""))
            return;
    }
    if(app->entries_count == app->entries_cap) {
        app->entries_cap = app->entries_cap ? app->entries_cap * 2 : 16;
        app->entries = realloc(app->entries, app->entries_cap * sizeof(TratEntry)); //-V701
    }
    TratEntry* e = &app->entries[app->entries_count++];
    e->kind = kind;
    e->label = furi_string_alloc_set_str(label);
    e->target = furi_string_alloc_set_str(target);
    e->args = furi_string_alloc_set_str(args ? args : "");
    e->order = app->entries_count;
}

static void trat_entries_add(
    LoaderMenuApp* app,
    TratEntryKind kind,
    const char* label,
    const char* target) {
    trat_entries_add_ex(app, kind, label, target, NULL);
}

static int trat_entry_cmp(const void* a, const void* b) {
    const TratEntry* ea = a;
    const TratEntry* eb = b;
    // builtins first, then folders, then apps/files
    int ra = ea->kind == TratEntryBuiltin ? 0 : (ea->kind == TratEntryDir ? 1 : 2);
    int rb = eb->kind == TratEntryBuiltin ? 0 : (eb->kind == TratEntryDir ? 1 : 2);
    if(ra != rb) return ra - rb;
    if(ra == 0) return (int)ea->order - (int)eb->order; // actions: defined order
    return strcasecmp(furi_string_get_cstr(ea->label), furi_string_get_cstr(eb->label));
}

// "flappy_bird.fap" -> "Flappy Bird"
static void trat_pretty_name(FuriString* out, const char* filename) {
    furi_string_set_str(out, filename);
    size_t dot = furi_string_search_rchar(out, '.');
    if(dot != FURI_STRING_FAILURE && dot > 0) furi_string_left(out, dot);
    bool word_start = true;
    for(size_t i = 0; i < furi_string_size(out); i++) {
        char c = furi_string_get_char(out, i);
        if(c == '_' || c == '-') {
            furi_string_set_char(out, i, ' ');
            word_start = true;
        } else {
            if(word_start && c >= 'a' && c <= 'z') furi_string_set_char(out, i, c - 32);
            word_start = (c == ' ');
        }
    }
}

static bool trat_is_external_app(const char* name) {
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(!strcmp(FLIPPER_EXTERNAL_APPS[i].name, name)) return true;
    }
    return false;
}

static bool trat_is_internal_app(const char* name) {
    for(size_t i = 0; i < FLIPPER_APPS_COUNT; i++) {
        if(!strcmp(FLIPPER_APPS[i].name, name)) return true;
    }
    for(size_t i = 0; i < FLIPPER_SYSTEM_APPS_COUNT; i++) {
        if(!strcmp(FLIPPER_SYSTEM_APPS[i].name, name)) return true;
    }
    return false;
}

static bool trat_dir_claimed(const char* name) {
    for(size_t c = 0; c < TRAT_CATEGORIES_COUNT; c++) {
        for(size_t d = 0; d < trat_categories[c].dirs_count; d++) {
            if(!strcasecmp(trat_categories[c].dirs[d], name)) return true;
        }
    }
    // Asset/resource folders are not app categories
    return !strcasecmp(name, "assets") || !strcasecmp(name, "Debug") ||
           !strcasecmp(name, "Examples");
}

/* Scan one directory. If only_unclaimed, top-level /ext/apps folders that
 * belong to a category are skipped (used by "More"). */
static void
    trat_scan_dir(LoaderMenuApp* app, Storage* storage, const char* dir_path, bool only_unclaimed) {
    File* dir = storage_file_alloc(storage);
    FuriString* full = furi_string_alloc();
    FuriString* label = furi_string_alloc();
    char name[96];
    FileInfo info;

    if(storage_dir_open(dir, dir_path)) {
        while(storage_dir_read(dir, &info, name, sizeof(name))) {
            if(name[0] == '.') continue;
            furi_string_printf(full, "%s/%s", dir_path, name);
            const char* full_c = furi_string_get_cstr(full);

            if(file_info_is_dir(&info)) {
                if(only_unclaimed && trat_dir_claimed(name)) continue;
                furi_string_printf(label, "%s >", name);
                trat_entries_add(app, TratEntryDir, furi_string_get_cstr(label), full_c);
                continue;
            }

            const char* ext = strrchr(name, '.');
            if(!ext) continue;
            if(!strcasecmp(ext, ".fap")) {
                // Firmware's own menu apps (Sub-GHz, Infrared, Bad KB...) keep real names
                bool official = false;
                for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
                    if(!strcmp(FLIPPER_EXTERNAL_APPS[i].path, full_c)) {
                        official = true;
                        // Section already lists this app's actions (Bruce-style): don't add
                        // the whole app again
                        bool has_actions = false;
                        for(size_t e = 0; e < app->entries_count; e++) {
                            if(furi_string_equal_str(
                                   app->entries[e].target, FLIPPER_EXTERNAL_APPS[i].name))
                                has_actions = true;
                        }
                        if(!has_actions) {
                            trat_entries_add(
                                app, TratEntryBuiltin, FLIPPER_EXTERNAL_APPS[i].name, full_c);
                        }
                        break;
                    }
                }
                if(!official) {
                    trat_pretty_name(label, name);
                    trat_entries_add(app, TratEntryFap, furi_string_get_cstr(label), full_c);
                }
            } else if(!strcasecmp(ext, ".js")) {
                trat_pretty_name(label, name);
                furi_string_cat_str(label, " (js)");
                trat_entries_add(app, TratEntryFile, furi_string_get_cstr(label), full_c);
            }
        }
    }
    storage_dir_close(dir);
    storage_file_free(dir);
    furi_string_free(full);
    furi_string_free(label);
}

static void trat_category_menu_callback(void* context, uint32_t index);

static void trat_build_category_list(LoaderMenuApp* app) {
    LoaderMenu* lm = app->loader_menu;
    const TratCategory* cat = &trat_categories[lm->category];

    trat_entries_clear(app);
    submenu_reset(app->category_menu);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc();

    if(lm->subdir[0]) {
        // Inside a nested folder
        trat_scan_dir(app, storage, lm->subdir, false);
        const char* leaf = strrchr(lm->subdir, '/');
        submenu_set_header(app->category_menu, leaf ? leaf + 1 : lm->subdir);
    } else {
        submenu_set_header(app->category_menu, cat->label);
        for(size_t i = 0; i < cat->builtins_count; i++) {
            if(trat_is_internal_app(cat->builtins[i].app) ||
               trat_is_external_app(cat->builtins[i].app)) {
                trat_entries_add_ex(
                    app,
                    TratEntryBuiltin,
                    cat->builtins[i].label,
                    cat->builtins[i].app,
                    cat->builtins[i].args);
            }
        }
        for(size_t d = 0; d < cat->dirs_count; d++) {
            furi_string_printf(path, EXT_PATH("apps/%s"), cat->dirs[d]);
            trat_scan_dir(app, storage, furi_string_get_cstr(path), false);
        }
        if(cat->kind == TratCatMore) {
            trat_scan_dir(app, storage, EXT_PATH("apps"), true);
        }
    }

    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);

    if(app->entries_count > 1) {
        qsort(app->entries, app->entries_count, sizeof(TratEntry), trat_entry_cmp);
    }

    if(app->entries_count == 0) {
        submenu_add_item(app->category_menu, "(no apps - add to SD)", (uint32_t)-1, NULL, NULL);
    }
    for(size_t i = 0; i < app->entries_count; i++) {
        submenu_add_item(
            app->category_menu,
            furi_string_get_cstr(app->entries[i].label),
            i,
            trat_category_menu_callback,
            app);
    }
    if(lm->selected_category_item < app->entries_count) {
        submenu_set_selected_item(app->category_menu, lm->selected_category_item);
    }
}

// Can't do this in GUI callbacks because now ViewHolder waits for ongoing
// input, and inputs are not processed because GUI is processing callbacks
static void loader_menu_set_view_pending(void* context, uint32_t arg) {
    LoaderMenuApp* app = context;
    view_holder_set_view(app->loader_menu->loader->view_holder, (View*)arg);
}

static void trat_rebuild_pending(void* context, uint32_t arg) {
    UNUSED(arg);
    LoaderMenuApp* app = context;
    trat_build_category_list(app);
    view_holder_set_view(
        app->loader_menu->loader->view_holder, submenu_get_view(app->category_menu));
}

static void trat_category_menu_callback(void* context, uint32_t index) {
    LoaderMenuApp* app = context;
    LoaderMenu* lm = app->loader_menu;
    if(index >= app->entries_count) return;
    TratEntry* e = &app->entries[index];
    const char* target = furi_string_get_cstr(e->target);
    lm->selected_category_item = index;

    switch(e->kind) {
    case TratEntryDir:
        strlcpy(lm->subdir, target, sizeof(lm->subdir));
        lm->selected_category_item = 0;
        furi_timer_pending_callback(trat_rebuild_pending, app, 0);
        break;
    case TratEntryFile:
        run_with_default_app(target);
        break;
    case TratEntryBuiltin:
        loader_menu_start_args(target, furi_string_get_cstr(e->args));
        break;
    case TratEntryFap:
    default:
        loader_menu_start(target);
        break;
    }
}

static void loader_menu_open_category(void* context, uint32_t index) {
    LoaderMenuApp* app = context;
    LoaderMenu* lm = app->loader_menu;
    if(index >= TRAT_CATEGORIES_COUNT) return;

    if(trat_categories[index].kind == TratCatSettings) {
        furi_timer_pending_callback(
            loader_menu_set_view_pending, app, (uint32_t)submenu_get_view(app->settings_menu));
        lm->current_view = LoaderMenuViewSettings;
        return;
    }
    lm->category = index;
    lm->subdir[0] = '\0';
    lm->selected_category_item = 0;
    lm->current_view = LoaderMenuViewCategory;
    furi_timer_pending_callback(trat_rebuild_pending, app, 0);
}

// Can't do this in GUI callbacks because now ViewHolder waits for ongoing
// input, and inputs are not processed because GUI is processing callbacks
static int32_t loader_menu_setting_pin_unpin_parallel(void* context) {
    const char* name = context;
    archive_favorites_handle_setting_pin_unpin(name, NULL);
    return 0;
}

static void
    loader_menu_settings_menu_callback(void* context, InputType input_type, uint32_t index) {
    UNUSED(context);
    const char* name = FLIPPER_SETTINGS_APPS[index].name;

    if(input_type == InputTypeShort) {
        // Workaround for SD format when app can't be opened
        if(!strcmp(name, "Storage")) {
            Storage* storage = furi_record_open(RECORD_STORAGE);
            FS_Error status = storage_sd_status(storage);
            furi_record_close(RECORD_STORAGE);
            // If SD card not ready, cannot be formatted, so we want loader to give
            // normal error message, with function below
            if(status != FSE_NOT_READY) {
                // Attempt to launch the app, and if failed offer to format SD card
                run_parallel(loader_menu_storage_settings, storage, 512);
                return;
            }
        }
        loader_menu_start(name);
    } else if(input_type == InputTypeLong) {
        run_parallel(loader_menu_setting_pin_unpin_parallel, (void*)name, 512);
    }
}

static void loader_menu_back(void* context) {
    LoaderMenuApp* app = context;
    LoaderMenu* lm = app->loader_menu;

    if(lm->current_view == LoaderMenuViewCategory) {
        if(lm->subdir[0]) {
            // Go up one folder, or back to category root
            char* slash = strrchr(lm->subdir, '/');
            if(slash) *slash = '\0';
            bool is_root = false;
            const TratCategory* cat = &trat_categories[lm->category];
            for(size_t d = 0; d < cat->dirs_count; d++) {
                char root[TRAT_SUBDIR_MAX];
                snprintf(root, sizeof(root), EXT_PATH("apps/%s"), cat->dirs[d]);
                if(!strcmp(root, lm->subdir)) is_root = true;
            }
            if(is_root || !strcmp(lm->subdir, EXT_PATH("apps")) || !slash) lm->subdir[0] = '\0';
            lm->selected_category_item = 0;
            furi_timer_pending_callback(trat_rebuild_pending, app, 0);
        } else {
            lm->category = -1;
            lm->current_view = LoaderMenuViewPrimary;
            furi_timer_pending_callback(
                loader_menu_set_view_pending, app, (uint32_t)menu_get_view(app->primary_menu));
        }
        return;
    }

    if(lm->current_view == LoaderMenuViewSettings && !lm->settings_only) {
        furi_timer_pending_callback(
            loader_menu_set_view_pending, app, (uint32_t)menu_get_view(app->primary_menu));
        lm->current_view = LoaderMenuViewPrimary;
    } else {
        furi_thread_flags_set(furi_thread_get_id(lm->thread), 0);
        if(lm->closed_cb) {
            lm->closed_cb(lm->context);
        }
    }
}

bool loader_menu_load_fap_meta(
    Storage* storage,
    FuriString* path,
    FuriString* name,
    const Icon** icon) {
    *icon = NULL;
    uint8_t* icon_buf = malloc(CUSTOM_ICON_MAX_SIZE);
    if(!flipper_application_load_name_and_icon(path, storage, &icon_buf, name)) {
        free(icon_buf);
        icon_buf = NULL;
        return false;
    }
    *icon = malloc(sizeof(Icon));
    FURI_CONST_ASSIGN((*icon)->frame_count, 1);
    FURI_CONST_ASSIGN((*icon)->frame_rate, 1);
    FURI_CONST_ASSIGN((*icon)->width, 10);
    FURI_CONST_ASSIGN((*icon)->height, 10);
    FURI_CONST_ASSIGN_PTR((*icon)->frames, malloc(sizeof(const uint8_t*)));
    FURI_CONST_ASSIGN_PTR((*icon)->frames[0], icon_buf);
    return true;
}

static void loader_menu_build_menu(LoaderMenuApp* app, LoaderMenu* menu) {
    const bool big = momentum_settings.menu_style == MenuStyleRat;
    for(size_t i = 0; i < TRAT_CATEGORIES_COUNT; i++) {
        menu_add_item(
            app->primary_menu,
            trat_categories[i].label,
            big ? trat_categories[i].icon_big : trat_categories[i].icon,
            i,
            loader_menu_open_category,
            app);
    }
    menu_set_selected_item(app->primary_menu, menu->selected_primary);
}

static void loader_menu_build_submenu(LoaderMenuApp* app, LoaderMenu* loader_menu) {
    for(size_t i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; i++) {
        submenu_add_item_ex(
            app->settings_menu,
            FLIPPER_SETTINGS_APPS[i].name,
            i,
            loader_menu_settings_menu_callback,
            NULL);
    }
    submenu_set_selected_item(app->settings_menu, loader_menu->selected_setting);
}

static LoaderMenuApp* loader_menu_app_alloc(LoaderMenu* loader_menu) {
    LoaderMenuApp* app = malloc(sizeof(LoaderMenuApp));
    memset(app, 0, sizeof(LoaderMenuApp));
    app->loader_menu = loader_menu;

    // Primary menu (categories) + category list
    if(!app->loader_menu->settings_only) {
        app->primary_menu = menu_alloc();
        loader_menu_build_menu(app, loader_menu);
        app->category_menu = submenu_alloc();
        if(loader_menu->current_view == LoaderMenuViewCategory && loader_menu->category >= 0 &&
           (size_t)loader_menu->category < TRAT_CATEGORIES_COUNT) {
            trat_build_category_list(app);
        } else if(loader_menu->current_view == LoaderMenuViewCategory) {
            loader_menu->current_view = LoaderMenuViewPrimary;
        }
    }

    // Settings menu
    app->settings_menu = submenu_alloc();
    loader_menu_build_submenu(app, loader_menu);

    View* view;
    switch(app->loader_menu->current_view) {
    case LoaderMenuViewSettings:
        view = submenu_get_view(app->settings_menu);
        break;
    case LoaderMenuViewCategory:
        view = submenu_get_view(app->category_menu);
        break;
    default:
        view = menu_get_view(app->primary_menu);
        break;
    }
    view_holder_set_view(app->loader_menu->loader->view_holder, view);
    view_holder_set_back_callback(app->loader_menu->loader->view_holder, loader_menu_back, app);

    return app;
}

static void loader_menu_app_free(LoaderMenuApp* app) {
    view_holder_set_back_callback(app->loader_menu->loader->view_holder, NULL, NULL);
    view_holder_set_view(
        app->loader_menu->loader->view_holder,
        loading_get_view(app->loader_menu->loader->loading));

    if(!app->loader_menu->settings_only) {
        app->loader_menu->selected_primary = menu_get_selected_item(app->primary_menu);
        if(app->loader_menu->current_view == LoaderMenuViewCategory && app->entries_count) {
            app->loader_menu->selected_category_item =
                submenu_get_selected_item(app->category_menu);
        }
        menu_free(app->primary_menu);
        submenu_free(app->category_menu);
        trat_entries_free(app);
    }
    app->loader_menu->selected_setting = app->loader_menu->current_view == LoaderMenuViewSettings ?
                                             submenu_get_selected_item(app->settings_menu) :
                                             0;
    submenu_free(app->settings_menu);

    free(app);
}

static int32_t loader_menu_thread(void* p) {
    LoaderMenu* loader_menu = p;
    furi_assert(loader_menu);

    LoaderMenuApp* app = loader_menu_app_alloc(loader_menu);

    furi_thread_flags_wait(0, FuriFlagWaitAll, FuriWaitForever);

    loader_menu_app_free(app);

    return 0;
}
