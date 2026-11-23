/* =====================================================================
 * Tunnel R.A.T. - Flipper Zero controller FAP
 * ---------------------------------------------------------------------
 * Talks to the NanoPi flipper_bridge.py over USB CDC (newline JSON).
 * Renders the folder/tool tree the Pi sends, pops the keyboard on
 * needinput, shows a confirm dialog on needconfirm, and long-pressing OK
 * on a tool sends the delete gesture.
 *
 * Verified against Momentum/official firmware headers (furi_hal_usb_cdc,
 * cli_vcp, gui modules). Build:  ufbt   then   ufbt launch
 *
 * USB note: launch switches USB to usb_cdc_dual and takes the 2nd CDC
 * channel (interface 02 -> /dev/flipper-link on the NanoPi). The CLI keeps
 * interface 00 (/dev/flipper), so the dashboard's Sub-GHz / NFC / SD
 * features still work while this app runs. Same channel as Tunnel R.A.T. Link.
 * ===================================================================== */
#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/text_box.h>

#define TAG           "TunnelRat"
#define CDC_CH        1  // interface 02 (/dev/flipper-link); CLI keeps channel 0
#define LINE_MAX      1024
#define ARGS_MAX      128
#define ITEMS_MAX     16
#define NAME_MAX      48
#define RESULT_MAX    2048
#define DLG_TEXT_MAX  160

typedef enum { ViewSubmenu, ViewKeyboard, ViewConfirm, ViewResult } TrView;
typedef enum { WorkerEvtStop = (1 << 0), WorkerEvtRx = (1 << 1) } WorkerEvt;
typedef enum { TrEventRx = 0 } TrCustomEvent;

typedef struct {
    Gui* gui;
    ViewDispatcher* vd;
    Submenu* submenu;
    TextInput* keyboard;
    DialogEx* confirm;
    TextBox* result;

    FuriThread* worker;
    FuriThreadId worker_id;
    FuriMessageQueue* rx_queue; // holds char* (heap lines)
    FuriSemaphore* tx_sem;      // serialises CDC tx
    FuriHalUsbInterface* usb_prev;
    volatile bool running;

    TrView view;

    // rx line assembly (worker thread only)
    char line[LINE_MAX];
    size_t line_len;

    // menu state (GUI thread only)
    char path0[NAME_MAX];
    char items[ITEMS_MAX][NAME_MAX];
    uint32_t item_count;

    // pending / buffers (GUI thread only; dialog text must persist)
    char pending_tool[NAME_MAX];
    char args[ARGS_MAX];
    char dlg_text[DLG_TEXT_MAX];
    char result_text[RESULT_MAX];
} TunnelRat;

static void tr_submenu_cb(void* ctx, InputType type, uint32_t index); // fwd
static void tr_keyboard_done(void* ctx);                              // fwd

/* -------- tiny JSON scan (the Pi controls the format) --------------- */
static bool json_str(const char* json, const char* key, char* out, size_t out_sz) {
    char pat[40];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if(!p) return false;
    p = strchr(p + strlen(pat), ':');
    if(!p) return false;
    p++;
    while(*p == ' ' || *p == '"') p++;
    size_t i = 0;
    while(*p && *p != '"' && *p != ',' && *p != '}' && i < out_sz - 1) out[i++] = *p++;
    out[i] = 0;
    return true;
}

/* -------- CDC tx (GUI thread only), chunked to 64B, tx-flow gated ---- */
static void tr_send(TunnelRat* app, const char* json) {
    char line[320];
    int n = snprintf(line, sizeof(line), "%s\n", json);
    if(n <= 0) return;
    size_t len = (size_t)n;
    if(len >= sizeof(line)) len = sizeof(line) - 1;
    size_t off = 0;
    while(off < len) {
        size_t chunk = len - off;
        if(chunk > CDC_DATA_SZ) chunk = CDC_DATA_SZ;
        furi_semaphore_acquire(app->tx_sem, 200); // wait for prev packet (best effort)
        furi_hal_cdc_send(CDC_CH, (uint8_t*)line + off, (uint16_t)chunk);
        off += chunk;
    }
}

static void tr_hello(TunnelRat* app) {
    tr_send(app, "{\"cmd\":\"hello\"}");
}
static void tr_menu(TunnelRat* app, const char* folder) {
    char b[96];
    if(folder && folder[0])
        snprintf(b, sizeof(b), "{\"cmd\":\"menu\",\"path\":[\"%s\"]}", folder);
    else
        snprintf(b, sizeof(b), "{\"cmd\":\"menu\",\"path\":[]}");
    tr_send(app, b);
}
static void tr_run(TunnelRat* app, const char* tool, const char* args) {
    char b[256];
    snprintf(b, sizeof(b), "{\"cmd\":\"run\",\"tool\":\"%s\",\"args\":\"%s\"}", tool, args ? args : "");
    tr_send(app, b);
}
static void tr_delete(TunnelRat* app, const char* tool) {
    char b[128];
    snprintf(b, sizeof(b), "{\"cmd\":\"delete\",\"tool\":\"%s\"}", tool);
    tr_send(app, b);
}

/* -------- handle one JSON line (GUI thread, via custom event) ------- */
static void tr_handle_line(TunnelRat* app, const char* json) {
    char type[24];
    if(!json_str(json, "type", type, sizeof(type))) return;

    if(strcmp(type, "menu") == 0) {
        submenu_reset(app->submenu);
        submenu_set_header(app->submenu, app->path0[0] ? app->path0 : "Tunnel R.A.T.");
        app->item_count = 0;
        const char* items = strstr(json, "\"items\"");
        if(items) {
            const char* p = strchr(items, '[');
            if(p) {
                p++;
                while(app->item_count < ITEMS_MAX) {
                    const char* q = strchr(p, '"');
                    const char* close = strchr(p, ']');
                    if(!q || (close && close < q)) break;
                    q++;
                    char name[NAME_MAX];
                    size_t i = 0;
                    while(*q && *q != '"' && i < sizeof(name) - 1) name[i++] = *q++;
                    name[i] = 0;
                    if(name[0]) {
                        strncpy(app->items[app->item_count], name, NAME_MAX - 1);
                        app->items[app->item_count][NAME_MAX - 1] = 0;
                        submenu_add_item_ex(
                            app->submenu, app->items[app->item_count],
                            app->item_count, tr_submenu_cb, app);
                        app->item_count++;
                    }
                    p = (*q == '"') ? q + 1 : q;
                }
            }
        }
        app->view = ViewSubmenu;
        view_dispatcher_switch_to_view(app->vd, ViewSubmenu);

    } else if(strcmp(type, "needinput") == 0) {
        json_str(json, "tool", app->pending_tool, sizeof(app->pending_tool));
        char prompt[64];
        if(!json_str(json, "prompt", prompt, sizeof(prompt))) strcpy(prompt, "Input");
        app->args[0] = 0;
        text_input_reset(app->keyboard);
        text_input_set_header_text(app->keyboard, prompt);
        // reset() nulls the buffer binding, so (re)bind it here or the
        // first keypress dereferences a NULL text_buffer and crashes.
        text_input_set_result_callback(
            app->keyboard, tr_keyboard_done, app, app->args, sizeof(app->args), true);
        app->view = ViewKeyboard;
        view_dispatcher_switch_to_view(app->vd, ViewKeyboard);

    } else if(strcmp(type, "needconfirm") == 0) {
        json_str(json, "tool", app->pending_tool, sizeof(app->pending_tool));
        if(!json_str(json, "prompt", app->dlg_text, sizeof(app->dlg_text)))
            strcpy(app->dlg_text, "Confirm?");
        dialog_ex_reset(app->confirm);
        dialog_ex_set_context(app->confirm, app);
        dialog_ex_set_header(app->confirm, "Confirm", 64, 2, AlignCenter, AlignTop);
        dialog_ex_set_text(app->confirm, app->dlg_text, 64, 30, AlignCenter, AlignCenter);
        dialog_ex_set_left_button_text(app->confirm, "Cancel");
        dialog_ex_set_right_button_text(app->confirm, "OK");
        app->view = ViewConfirm;
        view_dispatcher_switch_to_view(app->vd, ViewConfirm);

    } else if(strcmp(type, "result") == 0) {
        if(json_str(json, "output", app->result_text, sizeof(app->result_text))) {
            text_box_reset(app->result);
            text_box_set_text(app->result, app->result_text);
            app->view = ViewResult;
            view_dispatcher_switch_to_view(app->vd, ViewResult);
        }
    } else if(strcmp(type, "deleted") == 0) {
        if(!json_str(json, "output", app->result_text, sizeof(app->result_text)))
            strcpy(app->result_text, "deleted");
        text_box_reset(app->result);
        text_box_set_text(app->result, app->result_text);
        app->view = ViewResult;
        view_dispatcher_switch_to_view(app->vd, ViewResult);
    }
}

/* -------- submenu select: short=enter/run, long=delete -------------- */
static void tr_submenu_cb(void* ctx, InputType type, uint32_t index) {
    TunnelRat* app = ctx;
    if(index >= app->item_count) return;
    const char* name = app->items[index];
    if(!app->path0[0]) {
        if(type == InputTypeShort) {
            strncpy(app->path0, name, sizeof(app->path0) - 1);
            app->path0[sizeof(app->path0) - 1] = 0;
            tr_menu(app, app->path0);
        }
    } else {
        if(type == InputTypeLong) {
            tr_delete(app, name); // Pi replies needconfirm
        } else if(type == InputTypeShort) {
            tr_run(app, name, "");
        }
    }
}

static void tr_keyboard_done(void* ctx) {
    TunnelRat* app = ctx;
    tr_run(app, app->pending_tool, app->args);
    app->view = ViewSubmenu;
    view_dispatcher_switch_to_view(app->vd, ViewSubmenu);
}

static void tr_confirm_cb(DialogExResult result, void* ctx) {
    TunnelRat* app = ctx;
    tr_send(app, (result == DialogExResultRight) ? "{\"cmd\":\"confirm\"}"
                                                 : "{\"cmd\":\"cancel\"}");
    app->view = ViewSubmenu;
    view_dispatcher_switch_to_view(app->vd, ViewSubmenu);
}

/* -------- custom event: drain rx queue on the GUI thread ------------ */
static bool tr_custom_cb(void* ctx, uint32_t event) {
    TunnelRat* app = ctx;
    if(event == TrEventRx) {
        char* line = NULL;
        while(furi_message_queue_get(app->rx_queue, &line, 0) == FuriStatusOk) {
            if(line) {
                tr_handle_line(app, line);
                free(line);
            }
        }
        return true;
    }
    return false;
}

/* -------- back button -------------------------------------------- */
static bool tr_nav_cb(void* ctx) {
    TunnelRat* app = ctx;
    switch(app->view) {
    case ViewKeyboard:
    case ViewResult:
        app->view = ViewSubmenu;
        view_dispatcher_switch_to_view(app->vd, ViewSubmenu);
        return true;
    case ViewConfirm:
        tr_send(app, "{\"cmd\":\"cancel\"}");
        app->view = ViewSubmenu;
        view_dispatcher_switch_to_view(app->vd, ViewSubmenu);
        return true;
    case ViewSubmenu:
        if(app->path0[0]) {
            app->path0[0] = 0;
            tr_menu(app, NULL); // back to root
            return true;
        }
        return false; // exit app
    default:
        return false;
    }
}

/* -------- CDC callbacks (USB context): signal only ------------------ */
static void tr_cdc_tx(void* ctx) {
    TunnelRat* app = ctx;
    furi_semaphore_release(app->tx_sem);
}
static void tr_cdc_rx(void* ctx) {
    TunnelRat* app = ctx;
    if(app->worker_id) furi_thread_flags_set(app->worker_id, WorkerEvtRx);
}
static const CdcCallbacks tr_cdc_cb = {
    .tx_ep_callback = tr_cdc_tx,
    .rx_ep_callback = tr_cdc_rx,
    .state_callback = NULL,
    .ctrl_line_callback = NULL,
    .config_callback = NULL,
};

/* -------- worker: drain CDC into newline-delimited lines ------------ */
static int32_t tr_worker(void* ctx) {
    TunnelRat* app = ctx;
    uint8_t buf[CDC_DATA_SZ];
    while(app->running) {
        uint32_t flags =
            furi_thread_flags_wait(WorkerEvtStop | WorkerEvtRx, FuriFlagWaitAny, FuriWaitForever);
        if(flags & FuriFlagError) continue;
        if(flags & WorkerEvtStop) break;
        if(flags & WorkerEvtRx) {
            int32_t len;
            do {
                len = furi_hal_cdc_receive(CDC_CH, buf, sizeof(buf));
                for(int32_t i = 0; i < len; i++) {
                    char c = (char)buf[i];
                    if(c == '\n') {
                        app->line[app->line_len] = 0;
                        if(app->line_len > 0) {
                            char* copy = malloc(app->line_len + 1);
                            if(copy) {
                                memcpy(copy, app->line, app->line_len + 1);
                                if(furi_message_queue_put(app->rx_queue, &copy, 0) != FuriStatusOk)
                                    free(copy);
                                else
                                    view_dispatcher_send_custom_event(app->vd, TrEventRx);
                            }
                        }
                        app->line_len = 0;
                    } else if(app->line_len < LINE_MAX - 1) {
                        app->line[app->line_len++] = c;
                    } else {
                        app->line_len = 0; // overflow -> drop partial
                    }
                }
            } while(len > 0);
        }
    }
    return 0;
}

/* -------- lifecycle ------------------------------------------------- */
static TunnelRat* tr_alloc(void) {
    TunnelRat* app = malloc(sizeof(TunnelRat));
    memset(app, 0, sizeof(TunnelRat));

    app->gui = furi_record_open(RECORD_GUI);
    app->vd = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->vd, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->vd, app);
    view_dispatcher_set_custom_event_callback(app->vd, tr_custom_cb);
    view_dispatcher_set_navigation_event_callback(app->vd, tr_nav_cb);

    app->submenu = submenu_alloc();
    view_dispatcher_add_view(app->vd, ViewSubmenu, submenu_get_view(app->submenu));
    app->keyboard = text_input_alloc();
    text_input_set_result_callback(
        app->keyboard, tr_keyboard_done, app, app->args, sizeof(app->args), true);
    view_dispatcher_add_view(app->vd, ViewKeyboard, text_input_get_view(app->keyboard));
    app->confirm = dialog_ex_alloc();
    dialog_ex_set_context(app->confirm, app);
    dialog_ex_set_result_callback(app->confirm, tr_confirm_cb);
    view_dispatcher_add_view(app->vd, ViewConfirm, dialog_ex_get_view(app->confirm));
    app->result = text_box_alloc();
    view_dispatcher_add_view(app->vd, ViewResult, text_box_get_view(app->result));

    app->rx_queue = furi_message_queue_alloc(16, sizeof(char*));
    app->tx_sem = furi_semaphore_alloc(1, 1);
    app->running = true;
    app->worker = furi_thread_alloc_ex("TunnelRatWorker", 2048, tr_worker, app);
    return app;
}

static void tr_free(TunnelRat* app) {
    // drain any unprocessed rx lines
    char* line = NULL;
    while(furi_message_queue_get(app->rx_queue, &line, 0) == FuriStatusOk)
        if(line) free(line);

    view_dispatcher_remove_view(app->vd, ViewSubmenu);
    view_dispatcher_remove_view(app->vd, ViewKeyboard);
    view_dispatcher_remove_view(app->vd, ViewConfirm);
    view_dispatcher_remove_view(app->vd, ViewResult);
    submenu_free(app->submenu);
    text_input_free(app->keyboard);
    dialog_ex_free(app->confirm);
    text_box_free(app->result);
    view_dispatcher_free(app->vd);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(app->rx_queue);
    furi_semaphore_free(app->tx_sem);
    free(app);
}

int32_t tunnel_rat_app(void* p) {
    UNUSED(p);
    TunnelRat* app = tr_alloc();

    // --- add a 2nd USB serial channel (interface 02) and keep the CLI on 00 ---
    // Same approach as Tunnel R.A.T. Link: the NanoPi sees /dev/flipper-link for
    // this app's JSON, while /dev/flipper stays the normal CLI the dashboard uses.
    app->usb_prev = furi_hal_usb_get_config();
    if(!furi_hal_usb_set_config(&usb_cdc_dual, NULL)) app->usb_prev = NULL;
    furi_delay_ms(200);

    furi_thread_start(app->worker);
    app->worker_id = furi_thread_get_id(app->worker);
    furi_hal_cdc_set_callbacks(CDC_CH, (CdcCallbacks*)&tr_cdc_cb, app);

    app->view = ViewSubmenu;
    view_dispatcher_switch_to_view(app->vd, ViewSubmenu);
    tr_hello(app);

    view_dispatcher_run(app->vd);

    // --- stop worker ---
    app->running = false;
    furi_thread_flags_set(app->worker_id, WorkerEvtStop);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    // --- restore USB ---
    furi_hal_cdc_set_callbacks(CDC_CH, NULL, NULL);
    if(app->usb_prev) furi_hal_usb_set_config(app->usb_prev, NULL);

    tr_free(app);
    return 0;
}
