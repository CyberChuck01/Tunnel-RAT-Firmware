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
    char letter; // A-Z sections: chosen letter ('A'..'Z', '#'), 0 = letter list
    uint32_t selected_letter_item;
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
            loader_menu->thread = furi_thread_alloc_ex(TAG, 4096, loader_menu_thread, loader_menu);
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
    loader_menu->letter = 0;
    loader_menu->selected_letter_item = 0;
    loader_menu->current_view = settings_only ? LoaderMenuViewSettings : LoaderMenuViewPrimary;
    loader_menu->loader = furi_record_open(RECORD_LOADER);

    view_holder_set_back_callback(loader_menu->loader->view_holder, NULL, NULL);
    view_holder_set_view(
        loader_menu->loader->view_holder, loading_get_view(loader_menu->loader->loading));

    loader_menu->subscription = furi_pubsub_subscribe(
        loader_get_pubsub(loader_menu->loader), loader_pubsub_callback, loader_menu);

    loader_menu->thread = furi_thread_alloc_ex(TAG, 4096, loader_menu_thread, loader_menu);
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
 * Big sections (Tools) open an A-Z letter list first; OK on a letter shows the
 * apps starting with it. That also keeps each list under TRAT_MAX_ENTRIES.
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
    bool alpha; // open as an A-Z letter list first
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
     .icon_big = &I_CatBig_SubGhz_32x32,
     TRAT_BUILTINS(
         {"Read", "Sub-GHz", "TR:Read"},
         {"Read RAW", "Sub-GHz", "TR:ReadRAW"},
         {"Frequency Analyzer", "Sub-GHz", "TR:FrequencyAnalyzer"},
         {"Saved", "Sub-GHz", "TR:Saved"},
         {"Add Manually", "Sub-GHz", "TR:AddManually"},
         {"Radio Settings", "Sub-GHz", "TR:RadioSettings"}),
     TRAT_DIRS("Sub-GHz")},
    {.label = "NFC",
     .icon = &A_NFC_14,
     .icon_big = &I_CatBig_Nfc_32x32,
     TRAT_BUILTINS(
         {"Read", "NFC", "TR:Read"},
         {"Extract MFC Keys", "NFC", "TR:ExtractMfcKeys"},
         {"Saved", "NFC", "TR:Saved"},
         {"Extra Actions", "NFC", "TR:ExtraActions"},
         {"Add Manually", "NFC", "TR:AddManually"}),
     TRAT_DIRS("NFC")},
    {.label = "RFID",
     .icon = &A_125khz_14,
     .icon_big = &I_CatBig_Rfid_32x32,
     TRAT_BUILTINS(
         {"Read", "125 kHz RFID", "TR:Read"},
         {"Saved", "125 kHz RFID", "TR:Saved"},
         {"Add Manually", "125 kHz RFID", "TR:AddManually"},
         {"Extra Actions", "125 kHz RFID", "TR:ExtraActions"}),
     TRAT_DIRS("RFID")},
    {.label = "Infrared",
     .icon = &A_Infrared_14,
     .icon_big = &I_CatBig_Infrared_32x32,
     TRAT_BUILTINS(
         {"Universal Remotes", "Infrared", "TR:Universal"},
         {"Learn New Remote", "Infrared", "TR:Learn"},
         {"Saved Remotes", "Infrared", "TR:Saved"},
         {"IR GPIO Settings", "Infrared", "TR:GpioSettings"}),
     TRAT_DIRS("Infrared")},
    {.label = "iButton",
     .icon = &A_iButton_14,
     .icon_big = &I_CatBig_IButton_32x32,
     TRAT_BUILTINS(
         {"Read", "iButton", "TR:Read"},
         {"Saved", "iButton", "TR:Saved"},
         {"Add Manually", "iButton", "TR:AddManually"}),
     TRAT_DIRS("iButton")},
    {.label = "BadUSB",
     .icon = &A_BadUsb_14,
     .icon_big = &I_CatBig_BadUsb_32x32,
     TRAT_BUILTINS({"Run Script", "Bad KB", NULL}, {"U2F Security Key", "U2F", NULL}),
     TRAT_DIRS("USB")},
    {.label = "WiFi",
     .icon = &I_Cat_Wifi_14x14,
     .icon_big = &I_CatBig_Wifi_32x32,
     TRAT_DIRS("GPIO/ESP", "GPIO/FlipperHTTP", "GPIO/ESP32", "GPIO/ESP8266")},
    {.label = "Bluetooth",
     .icon = &I_Cat_Bluetooth_14x14,
     .icon_big = &I_CatBig_Bluetooth_32x32,
     TRAT_DIRS("Bluetooth")},
    {.label = "GPIO",
     .icon = &A_GPIO_14,
     .icon_big = &I_CatBig_Gpio_32x32,
     TRAT_BUILTINS(
         {"USB-UART Bridge", "GPIO", "TR:UsbUart"},
         {"Manual Control", "GPIO", "TR:ManualControl"},
         {"I2C Scanner", "GPIO", "TR:I2CScanner"},
         {"I2C SFP", "GPIO", "TR:I2CSfp"}),
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
     .icon_big = &I_CatBig_Tools_32x32,
     .kind = TratCatMore, // also picks up any folder you add to /ext/apps yourself
     .alpha = true,
     TRAT_DIRS("Tools", "Scripts")},
    {.label = "Themes",
     .icon = &I_Cat_Themes_14x14,
     .icon_big = &I_CatBig_Themes_32x32,
     TRAT_BUILTINS(
         {"Asset Pack & Anims", "R.A.T. Settings", "Graphics"},
         {"Screen & RGB Backlight", "R.A.T. Settings", "MiscScreen"},
         {"Main Menu Style", "R.A.T. Settings", "MenuStyle"},
         {"Lockscreen", "R.A.T. Settings", "Lockscreen"},
         {"Statusbar", "R.A.T. Settings", "Statusbar"},
         {"VGM Colors", "R.A.T. Settings", "Vgm"})},
    {.label = "Config",
     .icon = &A_Settings_14,
     .icon_big = &I_CatBig_Settings_32x32,
     TRAT_BUILTINS(
         {"Bluetooth", "Bluetooth", NULL},
         {"LCD and Notifications", "LCD and Notifications", NULL},
         {"Power", "Power", NULL},
         {"Storage", "Storage", NULL},
         {"Clock & Alarm", "Clock & Alarm", NULL},
         {"Desktop", "Desktop", NULL},
         {"Input", "Input", NULL},
         {"Expansion Modules", "Expansion Modules", NULL},
         {"System", "System", NULL},
         {"General", "R.A.T. Settings", "General"},
         {"File Browser", "R.A.T. Settings", "FileBrowser"},
         {"Sub-GHz Frequencies", "R.A.T. Settings", "Protocols"},
         {"Rat Level", "R.A.T. Settings", "Dolphin"},
         {"Spoofing", "R.A.T. Settings", "Spoof"},
         {"Passport", "Passport", NULL},
         {"About", "About", NULL})},
};
#define TRAT_CATEGORIES_COUNT COUNT_OF(trat_categories)

typedef enum {
    TratEntryBuiltin, // launch by internal name
    TratEntryDir, // open nested list
    TratEntryFap, // launch by path
    TratEntryFile, // .js etc, run_with_default_app
    TratEntryLetter, // A-Z sections: open that letter
} TratEntryKind;

typedef struct {
    TratEntryKind kind;
    FuriString* label;
    FuriString* target; // app name or absolute path
    FuriString* args; // launch args (empty = none)
    const Icon* icon; // left icon in the list (NULL = none)
    bool icon_owned; // true if loaded from a .fap and must be freed
    uint32_t order; // actions keep their defined order
} TratEntry;

// Reading every .fap's embedded icon means an ELF parse + malloc per app, so
// cap how many we load per category to keep both time and heap in check. Apps
// past the budget still appear in the list, just without their own icon.
#define TRAT_ICON_BUDGET 150

bool loader_menu_load_fap_meta(
    Storage* storage,
    FuriString* path,
    FuriString* name,
    const Icon** icon);

static void trat_free_loaded_icon(const Icon* icon) {
    if(!icon) return;
    if(icon->frames) {
        free((void*)icon->frames[0]);
        free((void*)icon->frames);
    }
    free((void*)icon);
}

#define TRAT_LETTER_BUCKETS 27 // A-Z sections: '#' (digits, symbols) + A-Z

typedef struct {
    LoaderMenu* loader_menu;
    Menu* primary_menu;
    Submenu* settings_menu;
    Submenu* category_menu;
    TratEntry* entries;
    size_t entries_count;
    size_t entries_cap;
    size_t loaded_icons; // count of .fap icons loaded this build (budget)
    bool counting; // A-Z letter list pass: only count apps per letter
    char letter_filter; // A-Z letter pass: only keep apps starting with this
    uint16_t letter_counts[TRAT_LETTER_BUCKETS];
    FuriThread* build_thread; // worker that scans the SD for a category
    volatile bool building; // true while build_thread is scanning
} LoaderMenuApp;

// Hard cap on items in one category list. A 1-bit list of hundreds of apps is
// unusable anyway, and (more importantly) building an unbounded list exhausts
// the heap -> malloc fail -> crash. Sub-GHz and Tools are the big offenders.
#define TRAT_MAX_ENTRIES 300

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
        if(app->entries[i].icon_owned) trat_free_loaded_icon(app->entries[i].icon);
    }
    app->entries_count = 0;
    app->loaded_icons = 0;
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
    const char* args,
    const Icon* icon,
    bool icon_owned) {
    // Bound the list so a huge folder can't exhaust the heap and crash.
    if(app->entries_count >= TRAT_MAX_ENTRIES) {
        if(icon_owned) trat_free_loaded_icon(icon);
        return;
    }
    // Skip duplicates (same target + args)
    for(size_t i = 0; i < app->entries_count; i++) {
        if(furi_string_equal_str(app->entries[i].target, target) &&
           furi_string_equal_str(app->entries[i].args, args ? args : "")) {
            if(icon_owned) trat_free_loaded_icon(icon);
            return;
        }
    }
    if(app->entries_count == app->entries_cap) {
        size_t new_cap = app->entries_cap ? app->entries_cap * 2 : 16;
        TratEntry* grown = realloc(app->entries, new_cap * sizeof(TratEntry));
        if(!grown) { // out of memory: keep what we have instead of crashing
            if(icon_owned) trat_free_loaded_icon(icon);
            return;
        }
        app->entries = grown;
        app->entries_cap = new_cap;
    }
    TratEntry* e = &app->entries[app->entries_count++];
    e->kind = kind;
    e->label = furi_string_alloc_set_str(label);
    e->target = furi_string_alloc_set_str(target);
    e->args = furi_string_alloc_set_str(args ? args : "");
    e->icon = icon;
    e->icon_owned = icon_owned;
    e->order = app->entries_count;
}

static void trat_entries_add(
    LoaderMenuApp* app,
    TratEntryKind kind,
    const char* label,
    const char* target) {
    trat_entries_add_ex(app, kind, label, target, NULL, NULL, false);
}

static void trat_entries_add_icon(
    LoaderMenuApp* app,
    TratEntryKind kind,
    const char* label,
    const char* target,
    const Icon* icon,
    bool icon_owned) {
    trat_entries_add_ex(app, kind, label, target, NULL, icon, icon_owned);
}

static int trat_entry_cmp(const void* a, const void* b) {
    const TratEntry* ea = a;
    const TratEntry* eb = b;
    // builtins first, then folders, then apps/files
    int ra = (ea->kind == TratEntryBuiltin || ea->kind == TratEntryLetter) ?
                 0 :
                 (ea->kind == TratEntryDir ? 1 : 2);
    int rb = (eb->kind == TratEntryBuiltin || eb->kind == TratEntryLetter) ?
                 0 :
                 (eb->kind == TratEntryDir ? 1 : 2);
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
    for(size_t i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; i++) {
        if(!strcmp(FLIPPER_SETTINGS_APPS[i].name, name)) return true;
    }
    return false;
}

// Icon for a built-in app, looked up by its menu name (NULL if not found).
static const Icon* trat_app_icon(const char* name) {
    for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
        if(!strcmp(FLIPPER_EXTERNAL_APPS[i].name, name)) return FLIPPER_EXTERNAL_APPS[i].icon;
    }
    for(size_t i = 0; i < FLIPPER_APPS_COUNT; i++) {
        if(!strcmp(FLIPPER_APPS[i].name, name)) return FLIPPER_APPS[i].icon;
    }
    for(size_t i = 0; i < FLIPPER_SYSTEM_APPS_COUNT; i++) {
        if(!strcmp(FLIPPER_SYSTEM_APPS[i].name, name)) return FLIPPER_SYSTEM_APPS[i].icon;
    }
    for(size_t i = 0; i < FLIPPER_SETTINGS_APPS_COUNT; i++) {
        if(!strcmp(FLIPPER_SETTINGS_APPS[i].name, name)) return FLIPPER_SETTINGS_APPS[i].icon;
    }
    return NULL;
}

// rel = path under /ext/apps, e.g. "GPIO/ESP"
static bool trat_dir_claimed(const char* rel) {
    for(size_t c = 0; c < TRAT_CATEGORIES_COUNT; c++) {
        for(size_t d = 0; d < trat_categories[c].dirs_count; d++) {
            if(!strcasecmp(trat_categories[c].dirs[d], rel)) return true;
        }
    }
    return false;
}

static bool trat_dir_ignored(const char* name, const char* parent_rel) {
    // Resource folders are never app sections. Only the top-level Debug/Examples
    // folders are skipped: GPIO/Debug holds real tools (SPI Mem Manager, SWD
    // Probe, DAP Link...) that belong in the GPIO list.
    if(!strcasecmp(name, "assets")) return true;
    return !parent_rel[0] && (!strcasecmp(name, "Debug") || !strcasecmp(name, "Examples"));
}

/* ---------- A-Z sections ---------- */

static size_t trat_letter_bucket(const char* label) {
    char c = label[0];
    if(c >= 'a' && c <= 'z') c -= 32;
    return (c >= 'A' && c <= 'Z') ? (size_t)(c - 'A' + 1) : 0;
}

static char trat_bucket_char(size_t bucket) {
    return bucket ? (char)('A' + bucket - 1) : '#';
}

// While counting, tally the label and drop it. While showing one letter, drop
// labels under other letters. Otherwise keep everything.
static bool trat_letter_keep(LoaderMenuApp* app, const char* label) {
    if(app->counting) {
        app->letter_counts[trat_letter_bucket(label)]++;
        return false;
    }
    return !app->letter_filter ||
           trat_bucket_char(trat_letter_bucket(label)) == app->letter_filter;
}

// Does this section already list actions for that app (Bruce-style)? A-Z
// passes don't hold every entry, so the section's own action list is checked
// too.
static bool trat_section_has_actions(LoaderMenuApp* app, const char* name) {
    for(size_t e = 0; e < app->entries_count; e++) {
        if(furi_string_equal_str(app->entries[e].target, name)) return true;
    }
    LoaderMenu* lm = app->loader_menu;
    if(!lm->subdir[0] && lm->category >= 0) {
        const TratCategory* cat = &trat_categories[lm->category];
        for(size_t i = 0; i < cat->builtins_count; i++) {
            if(!strcmp(cat->builtins[i].app, name)) return true;
        }
    }
    return false;
}

/* Flat, Bruce-style scan: every app under dir_path (and its subfolders) goes into one
 * list. Subfolders that are their own section (e.g. GPIO/ESP -> WiFi) are skipped.
 * rel is dir_path relative to /ext/apps. */
#define TRAT_SCAN_DEPTH 3
static void trat_scan_dir(
    LoaderMenuApp* app,
    Storage* storage,
    const char* dir_path,
    const char* rel,
    uint8_t depth) {
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
                if(trat_dir_ignored(name, rel) || depth >= TRAT_SCAN_DEPTH) continue;
                FuriString* sub_rel = furi_string_alloc();
                if(rel[0])
                    furi_string_printf(sub_rel, "%s/%s", rel, name);
                else
                    furi_string_set_str(sub_rel, name);
                if(!trat_dir_claimed(furi_string_get_cstr(sub_rel))) {
                    trat_scan_dir(app, storage, full_c, furi_string_get_cstr(sub_rel), depth + 1);
                }
                furi_string_free(sub_rel);
                continue;
            }

            const char* ext = strrchr(name, '.');
            if(!ext) continue;
            const bool is_fap = !strcasecmp(ext, ".fap");
            const bool is_js = !strcasecmp(ext, ".js");
            if(!is_fap && !is_js) continue;

            // Firmware's own menu apps (Sub-GHz, Infrared, Bad KB...) keep real names
            const FlipperExternalApplication* official = NULL;
            if(is_fap) {
                for(size_t i = 0; i < FLIPPER_EXTERNAL_APPS_COUNT; i++) {
                    if(!strcmp(FLIPPER_EXTERNAL_APPS[i].path, full_c)) {
                        official = &FLIPPER_EXTERNAL_APPS[i];
                        break;
                    }
                }
            }
            if(official) {
                // Section already lists this app's actions (Bruce-style): don't add
                // the whole app again
                if(trat_section_has_actions(app, official->name)) continue;
                furi_string_set_str(label, official->name);
            } else {
                trat_pretty_name(label, name);
                if(is_js) furi_string_cat_str(label, " (js)");
            }

            // Decide on the label before loading anything, so the A-Z counting
            // pass stays cheap (no icon loads).
            if(!trat_letter_keep(app, furi_string_get_cstr(label))) continue;

            if(official) {
                trat_entries_add_icon(
                    app, TratEntryBuiltin, official->name, full_c, official->icon, false);
            } else if(is_fap) {
                // Load the app's own icon, up to the per-build budget.
                const Icon* icon = NULL;
                bool owned = false;
                if(app->loaded_icons < TRAT_ICON_BUDGET) {
                    FuriString* tmp = furi_string_alloc();
                    if(loader_menu_load_fap_meta(storage, full, tmp, &icon)) {
                        owned = true;
                        app->loaded_icons++;
                    }
                    furi_string_free(tmp);
                }
                trat_entries_add_icon(
                    app, TratEntryFap, furi_string_get_cstr(label), full_c, icon, owned);
            } else {
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

// Builtins + the section's folders (+ unclaimed folders for "More" sections),
// all passed through the A-Z count/filter.
static void trat_scan_category(LoaderMenuApp* app, Storage* storage, const TratCategory* cat) {
    FuriString* path = furi_string_alloc();
    for(size_t i = 0; i < cat->builtins_count; i++) {
        const TratBuiltin* b = &cat->builtins[i];
        if(!trat_is_internal_app(b->app) && !trat_is_external_app(b->app)) continue;
        if(!trat_letter_keep(app, b->label)) continue;
        trat_entries_add_ex(
            app, TratEntryBuiltin, b->label, b->app, b->args, trat_app_icon(b->app), false);
    }
    for(size_t d = 0; d < cat->dirs_count; d++) {
        furi_string_printf(path, EXT_PATH("apps/%s"), cat->dirs[d]);
        trat_scan_dir(app, storage, furi_string_get_cstr(path), cat->dirs[d], 0);
    }
    if(cat->kind == TratCatMore) {
        // anything you add to /ext/apps that no section claims
        trat_scan_dir(app, storage, EXT_PATH("apps"), "", 0);
    }
    furi_string_free(path);
}

static void trat_build_category_list(LoaderMenuApp* app) {
    LoaderMenu* lm = app->loader_menu;
    const TratCategory* cat = &trat_categories[lm->category];

    trat_entries_clear(app);
    submenu_reset(app->category_menu);

    Storage* storage = furi_record_open(RECORD_STORAGE);

    if(lm->subdir[0]) {
        // Inside a nested folder
        trat_scan_dir(app, storage, lm->subdir, "", 0);
        const char* leaf = strrchr(lm->subdir, '/');
        submenu_set_header(app->category_menu, leaf ? leaf + 1 : lm->subdir);
    } else if(cat->alpha && !lm->letter) {
        // A-Z letter list: count apps per first letter, show letters that have any
        submenu_set_header(app->category_menu, cat->label);
        memset(app->letter_counts, 0, sizeof(app->letter_counts));
        app->counting = true;
        trat_scan_category(app, storage, cat);
        app->counting = false;
        for(size_t b = 0; b < TRAT_LETTER_BUCKETS; b++) {
            if(!app->letter_counts[b]) continue;
            char key[2] = {trat_bucket_char(b), '\0'};
            char text[16];
            snprintf(text, sizeof(text), "%s  (%u)", key, (unsigned)app->letter_counts[b]);
            trat_entries_add_ex(app, TratEntryLetter, text, key, NULL, NULL, false);
        }
    } else {
        if(cat->alpha) {
            // One letter of an A-Z section
            char header[32];
            snprintf(header, sizeof(header), "%s: %c", cat->label, lm->letter);
            submenu_set_header(app->category_menu, header);
            app->letter_filter = lm->letter;
        } else {
            submenu_set_header(app->category_menu, cat->label);
        }
        trat_scan_category(app, storage, cat);
        app->letter_filter = 0;
    }

    furi_record_close(RECORD_STORAGE);

    if(app->entries_count > 1) {
        qsort(app->entries, app->entries_count, sizeof(TratEntry), trat_entry_cmp);
    }

    if(app->entries_count == 0) {
        submenu_add_item(app->category_menu, "(no apps - add to SD)", (uint32_t)-1, NULL, NULL);
    }
    for(size_t i = 0; i < app->entries_count; i++) {
        submenu_add_item_icon(
            app->category_menu,
            furi_string_get_cstr(app->entries[i].label),
            app->entries[i].icon,
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

// The directory scan + qsort is far too heavy for the 1 KB timer-daemon stack
// that furi_timer_pending_callback() runs on (configTIMER_TASK_STACK_DEPTH=256
// words). Scanning Sub-GHz / Tools there overflowed the stack and crashed. So
// the scan runs on our own worker thread with a real stack instead.
static void trat_rebuild_in_place(LoaderMenuApp* app);

static bool trat_state_is(LoaderMenu* lm, int32_t category, char letter, const char* subdir) {
    return lm->category == category && lm->letter == letter && !strcmp(lm->subdir, subdir);
}

static int32_t trat_build_worker(void* context) {
    LoaderMenuApp* app = context;
    LoaderMenu* lm = app->loader_menu;
    int32_t category = -1;
    char letter = 0;
    char subdir[TRAT_SUBDIR_MAX] = {0};
    // Retriggers are ignored while we scan, so if the user moved on meanwhile
    // (Back, another letter or section) scan again for where they are now.
    for(uint8_t pass = 0; pass < 4; pass++) {
        // Back can land before a queued rebuild runs: never index with -1
        if(lm->category < 0 || (size_t)lm->category >= TRAT_CATEGORIES_COUNT) break;
        category = lm->category;
        letter = lm->letter;
        strlcpy(subdir, lm->subdir, sizeof(subdir));

        trat_build_category_list(app);

        if(lm->current_view != LoaderMenuViewCategory) break;
        if(trat_state_is(lm, category, letter, subdir)) break;
    }
    // Only take over the screen if the user is still waiting on this list
    // (they may have pressed Back or picked something else while we scanned).
    if(lm->current_view == LoaderMenuViewCategory && trat_state_is(lm, category, letter, subdir)) {
        view_holder_set_view(lm->loader->view_holder, submenu_get_view(app->category_menu));
    }
    app->building = false;
    // A change that landed after the last check had its retrigger dropped
    // while we were still building: go again.
    if(lm->current_view == LoaderMenuViewCategory &&
       !trat_state_is(lm, category, letter, subdir)) {
        trat_rebuild_in_place(app);
    }
    return 0;
}

static void trat_start_build(LoaderMenuApp* app) {
    if(app->building) return; // a scan is already in flight; ignore the retrigger
    // Reap the previous (finished) worker before reusing the handle.
    if(app->build_thread) {
        furi_thread_join(app->build_thread);
        furi_thread_free(app->build_thread);
        app->build_thread = NULL;
    }
    app->building = true;
    app->build_thread = furi_thread_alloc_ex("TratBuild", 4096, trat_build_worker, app);
    furi_thread_start(app->build_thread);
}

static void trat_rebuild_pending(void* context, uint32_t arg) {
    UNUSED(arg);
    trat_start_build((LoaderMenuApp*)context);
}

// Rebuild the list that is on screen (letter, folder, Back). Show the loading
// view meanwhile, so the old items can't be picked while their entries are
// being freed and refilled by the worker.
static void trat_rebuild_in_place(LoaderMenuApp* app) {
    furi_timer_pending_callback(
        loader_menu_set_view_pending,
        app,
        (uint32_t)loading_get_view(app->loader_menu->loader->loading));
    furi_timer_pending_callback(trat_rebuild_pending, app, 0);
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
        trat_rebuild_in_place(app);
        break;
    case TratEntryLetter:
        lm->letter = target[0];
        lm->selected_letter_item = index;
        lm->selected_category_item = 0;
        trat_rebuild_in_place(app);
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
    lm->letter = 0;
    lm->selected_letter_item = 0;
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

#define TRAT_SETTINGS_APP        "R.A.T. Settings"
#define TRAT_SETTINGS_IDX_SCREEN ((uint32_t) - 10)
#define TRAT_SETTINGS_IDX_APP    ((uint32_t) - 11)

static void
    loader_menu_settings_menu_callback(void* context, InputType input_type, uint32_t index) {
    UNUSED(context);
    // Tunnel R.A.T. entries at the top of the Settings section
    if(index == TRAT_SETTINGS_IDX_SCREEN || index == TRAT_SETTINGS_IDX_APP) {
        if(input_type == InputTypeShort) {
            loader_menu_start_args(
                TRAT_SETTINGS_APP, index == TRAT_SETTINGS_IDX_SCREEN ? "MiscScreen" : NULL);
        }
        return;
    }
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
            trat_rebuild_in_place(app);
        } else if(lm->letter) {
            // A-Z section: from a letter back to the letter list
            lm->letter = 0;
            lm->selected_category_item = lm->selected_letter_item;
            trat_rebuild_in_place(app);
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
    submenu_add_item_ex(
        app->settings_menu,
        "Screen & RGB Backlight",
        TRAT_SETTINGS_IDX_SCREEN,
        loader_menu_settings_menu_callback,
        NULL);
    submenu_add_item_ex(
        app->settings_menu,
        TRAT_SETTINGS_APP,
        TRAT_SETTINGS_IDX_APP,
        loader_menu_settings_menu_callback,
        NULL);
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
    // Make sure no background scan is still touching our menus/entries.
    if(app->build_thread) {
        furi_thread_join(app->build_thread);
        furi_thread_free(app->build_thread);
        app->build_thread = NULL;
    }
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
