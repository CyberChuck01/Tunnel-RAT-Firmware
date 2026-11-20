// Device: Flipper Zero (Tunnel R.A.T. firmware) - Tunnel R.A.T. Link v2
//
// One app that lets the Flipper and the Tunnel R.A.T. NanoPi feed off each other.
//
//   USB to the NanoPi : the Flipper switches to dual USB serial. Channel 0 stays the normal
//                       CLI (the dashboard drives Sub-GHz / NFC through it), channel 1
//                       (/dev/ttyACM1 on the NanoPi) carries the "TR" line protocol below.
//   WiFi dev board    : the same TR protocol over the GPIO UART (pins 13/14) to an ESP32
//                       running TunnelRAT_FlipperBridge.ino (WiFi scans + spotter on the
//                       ESP32 itself), or the old remote-CLI bridge.
//
// TR protocol (one line each, '\n'):
//   Flipper -> peer : TR HELLO | TR WIFI SCAN | TR BLE SCAN | TR AUDIT HS <bssid> <ch> <s>
//                     TR WARDRIVE START|STOP | TR SUBGHZ START|STOP | TR NFC START|STOP
//                     TR RECON <s>
//   peer -> Flipper : TR HELLO <name> <caps,..> | TR AP bssid|ch|rssi|enc|flag|ssid
//                     TR BT mac|rssi|flag|name | TR SUB freq|proto|data | TR NFC text
//                     TR STAT text | TR MSG text | TR ERR text | TR END what count
//   flag = - | FLOCK | AXON | MOTO | MESH  (worked out on the peer from MAC prefixes)

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_usb_cdc.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/widget.h>
#include <input/input.h>
#include <expansion/expansion.h>
#include <loader/loader.h>
#include <notification/notification_messages.h>
#include <toolbox/pipe.h>
#include <toolbox/cli/cli_registry.h>
#include <toolbox/cli/shell/cli_shell.h>

#define TAG          "TunnelRatLink"
#define UART_BAUD    115200
#define RX_BUF_SIZE  4096
#define LINE_MAX     160
#define MAX_APS      48
#define MAX_BTS      48
#define LOG_MAX      2500
#define TR_CDC_IF    1
#define RECORD_CLI_  "cli"
#define WORKER_STACK 2048

typedef enum {
    TrViewMenu,
    TrViewList,
    TrViewLog,
    TrViewDetail,
} TrView;

typedef enum {
    TrTransportNone,
    TrTransportUsb, // NanoPi, CDC channel 1
    TrTransportUart, // ESP32 WiFi board
} TrTransport;

typedef enum {
    TrScreenMenu,
    TrScreenWifiList,
    TrScreenBtList,
    TrScreenApDetail,
    TrScreenLive, // log-style screens: spotter, wardrive, field mouse, nfc, recon, audit
    TrScreenUsbTools,
    TrScreenCli,
} TrScreen;

typedef enum {
    TrLiveNone,
    TrLiveSpotter,
    TrLiveWardrive,
    TrLiveSubghz,
    TrLiveNfc,
    TrLiveRecon,
    TrLiveAudit,
    TrLiveScan,
} TrLive;

enum {
    TrEventLine = 100,
    TrEventTick,
};

enum {
    MenuWifiScan,
    MenuBtScan,
    MenuSpotter,
    MenuWardrive,
    MenuSubghz,
    MenuNfc,
    MenuRecon,
    MenuUsbTools,
    MenuCli,
    MenuTransport,
};

typedef struct {
    char bssid[18];
    uint8_t ch;
    int8_t rssi;
    char enc[10];
    char flag[8];
    char ssid[33];
} TrAp;

typedef struct {
    char mac[18];
    int8_t rssi;
    char flag[8];
    char name[24];
} TrBt;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* menu;
    Submenu* list;
    TextBox* log_box;
    Widget* detail;
    NotificationApp* notifications;
    Expansion* expansion;
    FuriTimer* tick;

    TrScreen screen;
    TrLive live;
    TrTransport transport;
    bool transport_auto;

    // peer info
    bool peer_ok;
    char peer_name[24];
    char peer_caps[48];

    // RX
    FuriStreamBuffer* rx_stream;
    FuriMessageQueue* lines;
    FuriThread* worker;
    volatile bool worker_stop;
    FuriHalSerialHandle* serial;
    FuriHalUsbInterface* usb_prev;

    // data
    TrAp aps[MAX_APS];
    size_t ap_count;
    TrBt bts[MAX_BTS];
    size_t bt_count;
    size_t sel_ap;
    uint32_t flagged_hits;
    FuriString* log;
    FuriString* header;

    // remote CLI (old bridge mode)
    bool cli_running;
    PipeSide* own_pipe;
    PipeSide* shell_pipe;
    CliShell* shell;
    CliRegistry* registry;
} TrApp;

static void tr_menu_rebuild(TrApp* app);

/* ------------------------------------------------------------------ helpers */

static bool tr_has_cap(TrApp* app, const char* cap) {
    return app->peer_ok && strstr(app->peer_caps, cap) != NULL;
}

static const char* tr_transport_name(TrTransport t) {
    switch(t) {
    case TrTransportUsb:
        return "USB NanoPi";
    case TrTransportUart:
        return "WiFi board";
    default:
        return "none";
    }
}

static void tr_log(TrApp* app, const char* text) {
    furi_string_cat_str(app->log, text);
    furi_string_cat_str(app->log, "\n");
    if(furi_string_size(app->log) > LOG_MAX) {
        furi_string_right(app->log, furi_string_size(app->log) - LOG_MAX / 2);
        size_t nl = furi_string_search_char(app->log, '\n');
        if(nl != FURI_STRING_FAILURE) furi_string_right(app->log, nl + 1);
    }
    if(app->screen == TrScreenLive) {
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
        text_box_set_focus(app->log_box, TextBoxFocusEnd);
    }
}

static void tr_alert(TrApp* app) {
    notification_message(app->notifications, &sequence_double_vibro);
    notification_message(app->notifications, &sequence_blink_red_100);
}

/* --------------------------------------------------------------- transport */

static void tr_uart_rx_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    TrApp* app = ctx;
    if(event & FuriHalSerialRxEventData) {
        uint8_t b = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &b, 1, 0);
    }
}

static void tr_cdc_rx_cb(void* ctx) {
    TrApp* app = ctx;
    uint8_t buf[64];
    int32_t n = furi_hal_cdc_receive(TR_CDC_IF, buf, sizeof(buf));
    if(n > 0) furi_stream_buffer_send(app->rx_stream, buf, n, 0);
}

static CdcCallbacks tr_cdc_callbacks = {
    .tx_ep_callback = NULL,
    .rx_ep_callback = tr_cdc_rx_cb,
    .state_callback = NULL,
    .ctrl_line_callback = NULL,
    .config_callback = NULL,
};

static void tr_send_raw(TrApp* app, const uint8_t* data, size_t len) {
    if(app->transport == TrTransportUart && app->serial) {
        furi_hal_serial_tx(app->serial, data, len);
    } else if(app->transport == TrTransportUsb) {
        while(len) {
            uint16_t chunk = MIN(len, (size_t)CDC_DATA_SZ);
            furi_hal_cdc_send(TR_CDC_IF, (uint8_t*)data, chunk);
            data += chunk;
            len -= chunk;
            furi_delay_ms(2);
        }
    }
}

static void tr_send(TrApp* app, const char* fmt, ...) {
    char line[LINE_MAX];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    if(n < 0) return;
    if(n > (int)sizeof(line) - 3) n = sizeof(line) - 3;
    line[n++] = '\n';
    line[n] = '\0';
    tr_send_raw(app, (uint8_t*)line, n);
}

static void tr_transport_close(TrApp* app) {
    if(app->transport == TrTransportUart && app->serial) {
        furi_hal_serial_async_rx_stop(app->serial);
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
        app->serial = NULL;
        expansion_enable(app->expansion);
    } else if(app->transport == TrTransportUsb) {
        furi_hal_cdc_set_callbacks(TR_CDC_IF, NULL, NULL);
        if(app->usb_prev) furi_hal_usb_set_config(app->usb_prev, NULL);
        app->usb_prev = NULL;
    }
    app->transport = TrTransportNone;
    app->peer_ok = false;
}

static bool tr_transport_open(TrApp* app, TrTransport t) {
    tr_transport_close(app);
    furi_stream_buffer_reset(app->rx_stream);
    if(t == TrTransportUart) {
        expansion_disable(app->expansion);
        app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
        if(!app->serial) {
            expansion_enable(app->expansion);
            return false;
        }
        furi_hal_serial_init(app->serial, UART_BAUD);
        furi_hal_serial_async_rx_start(app->serial, tr_uart_rx_cb, app, false);
    } else if(t == TrTransportUsb) {
        app->usb_prev = furi_hal_usb_get_config();
        if(!furi_hal_usb_set_config(&usb_cdc_dual, NULL)) {
            app->usb_prev = NULL;
            return false;
        }
        furi_delay_ms(200);
        furi_hal_cdc_set_callbacks(TR_CDC_IF, &tr_cdc_callbacks, app);
    }
    app->transport = t;
    tr_send(app, "TR HELLO TunnelRAT-Flipper");
    return true;
}

static int32_t tr_worker(void* ctx) {
    TrApp* app = ctx;
    char line[LINE_MAX];
    size_t len = 0;
    while(!app->worker_stop) {
        uint8_t b;
        if(!furi_stream_buffer_receive(app->rx_stream, &b, 1, 50)) continue;
        if(app->cli_running) {
            // remote-CLI mode: bytes go straight to the shell
            pipe_send(app->own_pipe, &b, 1);
            continue;
        }
        if(b == '\r') continue;
        if(b == '\n') {
            line[len] = '\0';
            if(len >= 3 && !strncmp(line, "TR ", 3)) {
                char* copy = malloc(len + 1);
                memcpy(copy, line, len + 1);
                if(furi_message_queue_put(app->lines, &copy, 0) == FuriStatusOk) {
                    view_dispatcher_send_custom_event(app->view_dispatcher, TrEventLine);
                } else {
                    free(copy);
                }
            }
            len = 0;
        } else if(len < sizeof(line) - 1) {
            line[len++] = (char)b;
        }
    }
    return 0;
}

/* -------------------------------------------------------------- remote CLI */

static const CliCommandExternalConfig tr_cli_ext = {
    .search_directory = "/ext/apps_data/cli/plugins",
    .fal_prefix = "cli_",
    .appid = "cli",
};

static void tr_cli_motd(void* context) {
    UNUSED(context);
    printf("\r\nTunnel R.A.T. Link - Flipper CLI over the WiFi board\r\nTR-LINK-READY\r\n");
}

static int32_t tr_cli_pump(void* ctx) {
    // shell -> UART
    TrApp* app = ctx;
    uint8_t buf[64];
    while(app->cli_running) {
        size_t avail = pipe_bytes_available(app->own_pipe);
        if(!avail) {
            if(pipe_state(app->own_pipe) == PipeStateBroken) break;
            furi_delay_ms(5);
            continue;
        }
        size_t n = pipe_receive(app->own_pipe, buf, MIN(avail, sizeof(buf)));
        if(n) tr_send_raw(app, buf, n);
    }
    return 0;
}

static FuriThread* tr_cli_thread = NULL;

static void tr_cli_start(TrApp* app) {
    if(app->transport != TrTransportUart && !tr_transport_open(app, TrTransportUart)) {
        tr_log(app, "UART busy - unplug other GPIO apps");
        return;
    }
    PipeSideBundle b = pipe_alloc(1024, 1);
    app->own_pipe = b.alices_side;
    app->shell_pipe = b.bobs_side;
    app->registry = furi_record_open(RECORD_CLI_);
    app->shell = cli_shell_alloc(tr_cli_motd, app, app->shell_pipe, app->registry, &tr_cli_ext);
    cli_shell_start(app->shell);
    app->cli_running = true;
    tr_cli_thread = furi_thread_alloc_ex("TRCliPump", 1024, tr_cli_pump, app);
    furi_thread_start(tr_cli_thread);
    tr_log(
        app,
        "Remote CLI running on GPIO 13/14.\nThe ESP32 bridge serves it on TCP 2323.\nBack = stop.");
}

static void tr_cli_stop(TrApp* app) {
    if(!app->cli_running) return;
    app->cli_running = false;
    furi_thread_join(tr_cli_thread);
    furi_thread_free(tr_cli_thread);
    tr_cli_thread = NULL;
    pipe_free(app->own_pipe);
    cli_shell_join(app->shell);
    cli_shell_free(app->shell);
    pipe_free(app->shell_pipe);
    furi_record_close(RECORD_CLI_);
    app->own_pipe = app->shell_pipe = NULL;
    app->shell = NULL;
}

/* ------------------------------------------------------------ line parsing */

static size_t tr_split(char* s, char** out, size_t max) {
    size_t n = 0;
    out[n++] = s;
    for(char* p = s; *p && n < max; p++) {
        if(*p == '|') {
            *p = '\0';
            out[n++] = p + 1;
        }
    }
    return n;
}

static void tr_show_live(TrApp* app, const char* title) {
    furi_string_set_str(app->header, title);
    app->screen = TrScreenLive;
    text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
    text_box_set_focus(app->log_box, TextBoxFocusEnd);
    view_dispatcher_switch_to_view(app->view_dispatcher, TrViewLog);
}

static void tr_list_select_ap(void* ctx, uint32_t index);

static void tr_show_ap_list(TrApp* app) {
    submenu_reset(app->list);
    char head[32];
    snprintf(head, sizeof(head), "WiFi: %u APs", (unsigned)app->ap_count);
    submenu_set_header(app->list, head);
    char label[48];
    for(size_t i = 0; i < app->ap_count; i++) {
        TrAp* a = &app->aps[i];
        bool flagged = a->flag[0] && strcmp(a->flag, "-");
        snprintf(
            label,
            sizeof(label),
            "%s%s %d",
            flagged ? "! " : "",
            a->ssid[0] ? a->ssid : a->bssid,
            a->rssi);
        submenu_add_item(app->list, label, i, tr_list_select_ap, app);
    }
    if(!app->ap_count) submenu_add_item(app->list, "(nothing found)", 999, NULL, NULL);
    app->screen = TrScreenWifiList;
    view_dispatcher_switch_to_view(app->view_dispatcher, TrViewList);
}

static void tr_show_bt_list(TrApp* app) {
    submenu_reset(app->list);
    char head[32];
    snprintf(head, sizeof(head), "Bluetooth: %u", (unsigned)app->bt_count);
    submenu_set_header(app->list, head);
    char label[48];
    for(size_t i = 0; i < app->bt_count; i++) {
        TrBt* b = &app->bts[i];
        bool flagged = b->flag[0] && strcmp(b->flag, "-");
        snprintf(
            label,
            sizeof(label),
            "%s%s %d",
            flagged ? "! " : "",
            b->name[0] ? b->name : b->mac,
            b->rssi);
        submenu_add_item(app->list, label, i, NULL, NULL);
    }
    if(!app->bt_count) submenu_add_item(app->list, "(nothing found)", 999, NULL, NULL);
    app->screen = TrScreenBtList;
    view_dispatcher_switch_to_view(app->view_dispatcher, TrViewList);
}

static void tr_detail_button(GuiButtonType button, InputType type, void* ctx) {
    TrApp* app = ctx;
    if(type != InputTypeShort || button != GuiButtonTypeCenter) return;
    TrAp* a = &app->aps[app->sel_ap];
    furi_string_reset(app->log);
    app->live = TrLiveAudit;
    tr_send(app, "TR AUDIT HS %s %u 60", a->bssid, a->ch);
    tr_log(
        app,
        "Asked the NanoPi to capture a handshake (60 s).\nResult lands in FILES on the dashboard.");
    tr_show_live(app, "Handshake capture");
}

static void tr_list_select_ap(void* ctx, uint32_t index) {
    TrApp* app = ctx;
    if(index >= app->ap_count) return;
    app->sel_ap = index;
    TrAp* a = &app->aps[index];
    widget_reset(app->detail);
    char text[200];
    snprintf(
        text,
        sizeof(text),
        "\e#%s\n%s\nCh %u  RSSI %d  %s\n%s%s",
        a->ssid[0] ? a->ssid : "(hidden)",
        a->bssid,
        a->ch,
        a->rssi,
        a->enc,
        (a->flag[0] && strcmp(a->flag, "-")) ? "Flagged: " : "",
        (a->flag[0] && strcmp(a->flag, "-")) ? a->flag : "");
    widget_add_text_scroll_element(app->detail, 0, 0, 128, 50, text);
    if(tr_has_cap(app, "audit")) {
        widget_add_button_element(
            app->detail, GuiButtonTypeCenter, "Handshake", tr_detail_button, app);
    }
    app->screen = TrScreenApDetail;
    view_dispatcher_switch_to_view(app->view_dispatcher, TrViewDetail);
}

static void tr_handle_line(TrApp* app, char* line) {
    char* rest = line + 3; // after "TR "
    if(!strncmp(rest, "HELLO ", 6)) {
        char* p = rest + 6;
        char* sp = strchr(p, ' ');
        if(sp) {
            *sp = '\0';
            strlcpy(app->peer_caps, sp + 1, sizeof(app->peer_caps));
        } else {
            app->peer_caps[0] = '\0';
        }
        strlcpy(app->peer_name, p, sizeof(app->peer_name));
        bool was = app->peer_ok;
        app->peer_ok = true;
        if(!was) {
            notification_message(app->notifications, &sequence_blink_green_100);
            if(app->screen == TrScreenMenu) tr_menu_rebuild(app);
        }
    } else if(!strncmp(rest, "AP ", 3)) {
        char* f[6];
        if(tr_split(rest + 3, f, 6) < 6) return;
        // update existing or append
        size_t i;
        for(i = 0; i < app->ap_count; i++)
            if(!strcmp(app->aps[i].bssid, f[0])) break;
        if(i == app->ap_count) {
            if(app->ap_count >= MAX_APS) return;
            app->ap_count++;
        }
        TrAp* a = &app->aps[i];
        strlcpy(a->bssid, f[0], sizeof(a->bssid));
        a->ch = atoi(f[1]);
        a->rssi = atoi(f[2]);
        strlcpy(a->enc, f[3], sizeof(a->enc));
        strlcpy(a->flag, f[4], sizeof(a->flag));
        strlcpy(a->ssid, f[5], sizeof(a->ssid));
        if(strcmp(a->flag, "-") && a->flag[0] &&
           (app->live == TrLiveSpotter || app->live == TrLiveWardrive ||
            app->live == TrLiveRecon)) {
            char msg[80];
            snprintf(msg, sizeof(msg), "!! %s WiFi %s %ddBm", a->flag, a->bssid, a->rssi);
            tr_log(app, msg);
            app->flagged_hits++;
            tr_alert(app);
        }
    } else if(!strncmp(rest, "BT ", 3)) {
        char* f[4];
        if(tr_split(rest + 3, f, 4) < 4) return;
        size_t i;
        for(i = 0; i < app->bt_count; i++)
            if(!strcmp(app->bts[i].mac, f[0])) break;
        if(i == app->bt_count) {
            if(app->bt_count >= MAX_BTS) return;
            app->bt_count++;
        }
        TrBt* b = &app->bts[i];
        strlcpy(b->mac, f[0], sizeof(b->mac));
        b->rssi = atoi(f[1]);
        strlcpy(b->flag, f[2], sizeof(b->flag));
        strlcpy(b->name, f[3], sizeof(b->name));
        if(strcmp(b->flag, "-") && b->flag[0] &&
           (app->live == TrLiveSpotter || app->live == TrLiveWardrive ||
            app->live == TrLiveRecon)) {
            char msg[80];
            snprintf(msg, sizeof(msg), "!! %s BLE %s %ddBm", b->flag, b->mac, b->rssi);
            tr_log(app, msg);
            app->flagged_hits++;
            tr_alert(app);
        }
    } else if(!strncmp(rest, "SUB ", 4)) {
        char* f[3];
        if(tr_split(rest + 4, f, 3) < 3) return;
        char msg[96];
        snprintf(msg, sizeof(msg), "%s %s %s", f[0], f[1], f[2]);
        tr_log(app, msg);
        notification_message(app->notifications, &sequence_blink_blue_10);
    } else if(!strncmp(rest, "NFC ", 4)) {
        tr_log(app, rest + 4);
        notification_message(app->notifications, &sequence_success);
    } else if(!strncmp(rest, "STAT ", 5) || !strncmp(rest, "MSG ", 4)) {
        tr_log(app, strchr(rest, ' ') + 1);
    } else if(!strncmp(rest, "ERR ", 4)) {
        char msg[LINE_MAX];
        snprintf(msg, sizeof(msg), "ERROR: %s", rest + 4);
        tr_log(app, msg);
        notification_message(app->notifications, &sequence_error);
    } else if(!strncmp(rest, "END ", 4)) {
        char* what = rest + 4;
        if(!strncmp(what, "WIFI", 4) && app->screen == TrScreenLive && app->live == TrLiveScan) {
            tr_show_ap_list(app);
        } else if(!strncmp(what, "BLE", 3) && app->screen == TrScreenLive && app->live == TrLiveScan) {
            tr_show_bt_list(app);
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), "-- done: %s", what);
            tr_log(app, msg);
        }
    }
}

/* ------------------------------------------------------------------- menus */

static void tr_stop_live(TrApp* app) {
    switch(app->live) {
    case TrLiveWardrive:
        tr_send(app, "TR WARDRIVE STOP");
        break;
    case TrLiveSubghz:
        tr_send(app, "TR SUBGHZ STOP");
        break;
    case TrLiveNfc:
        tr_send(app, "TR NFC STOP");
        break;
    default:
        break;
    }
    app->live = TrLiveNone;
}

static bool tr_need_peer(TrApp* app, const char* cap) {
    if(tr_has_cap(app, cap)) return true;
    furi_string_reset(app->log);
    if(!app->peer_ok) {
        tr_log(
            app,
            "No Tunnel R.A.T. peer yet.\n\nUSB: plug into the NanoPi (dashboard v17+).\n"
            "WiFi board: flash TunnelRAT_FlipperBridge and pick 'Connection: WiFi board'.");
    } else {
        char msg[96];
        snprintf(
            msg, sizeof(msg), "%s can't do '%s'.\nTry the other connection.", app->peer_name, cap);
        tr_log(app, msg);
    }
    tr_show_live(app, "Tunnel R.A.T.");
    app->live = TrLiveNone;
    return false;
}

static const char* tr_usb_apps[][2] = {
    {"Bad KB (scripts)", "Bad KB"},
    {"U2F Security Key", "U2F"},
    {"Mass Storage", "Mass Storage"},
    {"USB Remote / Mouse", "USB Remote"},
    {"USB Consumer Control", "USB Consumer Control"},
    {"USB Game Controller", "USB Game Controller"},
};

static void tr_usb_tools_cb(void* ctx, uint32_t index) {
    TrApp* app = ctx;
    if(index >= COUNT_OF(tr_usb_apps)) return;
    Loader* loader = furi_record_open(RECORD_LOADER);
    loader_enqueue_launch(loader, tr_usb_apps[index][1], NULL, LoaderDeferredLaunchFlagGui);
    furi_record_close(RECORD_LOADER);
    view_dispatcher_stop(app->view_dispatcher); // hand USB over to the tool
}

static void tr_show_usb_tools(TrApp* app) {
    submenu_reset(app->list);
    submenu_set_header(app->list, "USB tools (PC)");
    for(size_t i = 0; i < COUNT_OF(tr_usb_apps); i++) {
        submenu_add_item(app->list, tr_usb_apps[i][0], i, tr_usb_tools_cb, app);
    }
    app->screen = TrScreenUsbTools;
    view_dispatcher_switch_to_view(app->view_dispatcher, TrViewList);
}

static void tr_menu_cb(void* ctx, uint32_t index) {
    TrApp* app = ctx;
    switch(index) {
    case MenuWifiScan:
        if(!tr_need_peer(app, "wifi")) return;
        app->ap_count = 0;
        furi_string_reset(app->log);
        app->live = TrLiveScan;
        tr_send(app, "TR WIFI SCAN");
        tr_log(app, "Scanning WiFi...");
        tr_show_live(app, "WiFi scan");
        break;
    case MenuBtScan:
        if(!tr_need_peer(app, "ble")) return;
        app->bt_count = 0;
        furi_string_reset(app->log);
        app->live = TrLiveScan;
        tr_send(app, "TR BLE SCAN");
        tr_log(app, "Scanning Bluetooth (on the NanoPi)...");
        tr_show_live(app, "Bluetooth scan");
        break;
    case MenuSpotter:
        if(!tr_need_peer(app, "wifi")) return;
        furi_string_reset(app->log);
        app->flagged_hits = 0;
        app->live = TrLiveSpotter;
        tr_log(app, "Flock / Axon / Motorola spotter\nScans every ~10 s. Vibrates on a hit.");
        tr_send(app, "TR WIFI SCAN");
        if(tr_has_cap(app, "ble")) tr_send(app, "TR BLE SCAN");
        tr_show_live(app, "Spotter");
        break;
    case MenuWardrive:
        if(!tr_need_peer(app, "wardrive")) return;
        furi_string_reset(app->log);
        app->live = TrLiveWardrive;
        tr_send(app, "TR WARDRIVE START");
        tr_log(app, "Wardrive mode: NanoPi WiFi/BLE/GPS + Flipper Sub-GHz.\nBack = stop.");
        tr_show_live(app, "Wardrive");
        break;
    case MenuSubghz:
        if(!tr_need_peer(app, "subghz")) return;
        furi_string_reset(app->log);
        app->live = TrLiveSubghz;
        tr_send(app, "TR SUBGHZ START");
        tr_log(
            app,
            "Field mouse: the NanoPi hops the Flipper's Sub-GHz radio\nand logs every decoded signal (GPS-tagged).\nBack = stop.");
        tr_show_live(app, "Sub-GHz field mouse");
        break;
    case MenuNfc:
        if(!tr_need_peer(app, "nfc")) return;
        furi_string_reset(app->log);
        app->live = TrLiveNfc;
        tr_send(app, "TR NFC START");
        tr_log(
            app,
            "NFC scanner: hold tags to the back.\nEach one is logged on the NanoPi.\nBack = stop.");
        tr_show_live(app, "NFC scanner");
        break;
    case MenuRecon:
        if(!tr_need_peer(app, "recon")) return;
        furi_string_reset(app->log);
        app->flagged_hits = 0;
        app->live = TrLiveRecon;
        tr_send(app, "TR RECON 30");
        tr_log(app, "Rat Recon: 30 s WiFi + BLE + Sub-GHz sweep.\nReport saved on the NanoPi.");
        tr_show_live(app, "Rat Recon");
        break;
    case MenuUsbTools:
        tr_show_usb_tools(app);
        break;
    case MenuCli:
        furi_string_reset(app->log);
        app->screen = TrScreenCli;
        tr_cli_start(app);
        furi_string_set_str(app->header, "Remote CLI");
        text_box_set_text(app->log_box, furi_string_get_cstr(app->log));
        view_dispatcher_switch_to_view(app->view_dispatcher, TrViewLog);
        break;
    case MenuTransport: {
        TrTransport next = app->transport == TrTransportUsb ? TrTransportUart : TrTransportUsb;
        app->transport_auto = false;
        if(!tr_transport_open(app, next)) {
            furi_string_reset(app->log);
            tr_log(app, "Couldn't open that connection.");
            tr_show_live(app, "Connection");
            return;
        }
        tr_menu_rebuild(app);
        break;
    }
    default:
        break;
    }
}

static void tr_menu_rebuild(TrApp* app) {
    uint32_t sel = submenu_get_selected_item(app->menu);
    submenu_reset(app->menu);
    char head[40];
    snprintf(
        head,
        sizeof(head),
        "%s %s",
        app->peer_ok ? "*" : "-",
        app->peer_ok ? app->peer_name : tr_transport_name(app->transport));
    submenu_set_header(app->menu, head);
    submenu_add_item(app->menu, "WiFi Scan", MenuWifiScan, tr_menu_cb, app);
    submenu_add_item(app->menu, "Bluetooth Scan", MenuBtScan, tr_menu_cb, app);
    submenu_add_item(app->menu, "Flock / Axon Spotter", MenuSpotter, tr_menu_cb, app);
    submenu_add_item(app->menu, "Wardrive Mode", MenuWardrive, tr_menu_cb, app);
    submenu_add_item(app->menu, "Sub-GHz Field Mouse", MenuSubghz, tr_menu_cb, app);
    submenu_add_item(app->menu, "NFC Scanner", MenuNfc, tr_menu_cb, app);
    submenu_add_item(app->menu, "Rat Recon", MenuRecon, tr_menu_cb, app);
    submenu_add_item(app->menu, "USB Tools (PC)", MenuUsbTools, tr_menu_cb, app);
    submenu_add_item(app->menu, "Remote CLI (WiFi board)", MenuCli, tr_menu_cb, app);
    char conn[40];
    snprintf(conn, sizeof(conn), "Connection: %s", tr_transport_name(app->transport));
    submenu_add_item(app->menu, conn, MenuTransport, tr_menu_cb, app);
    submenu_set_selected_item(app->menu, sel);
}

/* ----------------------------------------------------------------- events */

static bool tr_custom_event(void* ctx, uint32_t event) {
    TrApp* app = ctx;
    if(event == TrEventLine) {
        char* line;
        while(furi_message_queue_get(app->lines, &line, 0) == FuriStatusOk) {
            tr_handle_line(app, line);
            free(line);
        }
        return true;
    } else if(event == TrEventTick) {
        // no peer yet on USB? fall back to the WiFi board automatically, and keep saying hi
        static uint8_t ticks = 0;
        ticks++;
        if(!app->peer_ok && app->transport_auto && ticks % 5 == 0 && !app->cli_running) {
            TrTransport next = app->transport == TrTransportUsb ? TrTransportUart : TrTransportUsb;
            tr_transport_open(app, next);
            if(app->screen == TrScreenMenu) tr_menu_rebuild(app);
        } else if(!app->peer_ok && !app->cli_running) {
            tr_send(app, "TR HELLO TunnelRAT-Flipper");
        }
        if(app->live == TrLiveSpotter && ticks % 5 == 0) {
            tr_send(app, "TR WIFI SCAN");
            if(tr_has_cap(app, "ble")) tr_send(app, "TR BLE SCAN");
        }
        return true;
    }
    return false;
}

static void tr_tick_cb(void* ctx) {
    TrApp* app = ctx;
    view_dispatcher_send_custom_event(app->view_dispatcher, TrEventTick);
}

static bool tr_back_event(void* ctx) {
    TrApp* app = ctx;
    switch(app->screen) {
    case TrScreenMenu:
        return false; // exit app
    case TrScreenCli:
        tr_cli_stop(app);
        break;
    case TrScreenApDetail:
        app->screen = TrScreenWifiList;
        view_dispatcher_switch_to_view(app->view_dispatcher, TrViewList);
        return true;
    case TrScreenLive:
        tr_stop_live(app);
        break;
    default:
        break;
    }
    app->screen = TrScreenMenu;
    tr_menu_rebuild(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, TrViewMenu);
    return true;
}

/* -------------------------------------------------------------------- app */

int32_t tunnel_rat_link_app(void* p) {
    UNUSED(p);
    TrApp* app = malloc(sizeof(TrApp));
    memset(app, 0, sizeof(TrApp));
    app->log = furi_string_alloc();
    app->header = furi_string_alloc();
    app->rx_stream = furi_stream_buffer_alloc(RX_BUF_SIZE, 1);
    app->lines = furi_message_queue_alloc(32, sizeof(char*));
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->expansion = furi_record_open(RECORD_EXPANSION);
    app->gui = furi_record_open(RECORD_GUI);
    app->transport_auto = true;

    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, tr_custom_event);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, tr_back_event);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->menu = submenu_alloc();
    app->list = submenu_alloc();
    app->log_box = text_box_alloc();
    text_box_set_font(app->log_box, TextBoxFontText);
    app->detail = widget_alloc();
    view_dispatcher_add_view(app->view_dispatcher, TrViewMenu, submenu_get_view(app->menu));
    view_dispatcher_add_view(app->view_dispatcher, TrViewList, submenu_get_view(app->list));
    view_dispatcher_add_view(app->view_dispatcher, TrViewLog, text_box_get_view(app->log_box));
    view_dispatcher_add_view(app->view_dispatcher, TrViewDetail, widget_get_view(app->detail));

    app->worker = furi_thread_alloc_ex("TRLinkRx", WORKER_STACK, tr_worker, app);
    furi_thread_start(app->worker);

    // USB first (NanoPi); the tick falls back to the WiFi board if nobody answers
    tr_transport_open(app, TrTransportUsb);
    tr_menu_rebuild(app);
    app->screen = TrScreenMenu;
    view_dispatcher_switch_to_view(app->view_dispatcher, TrViewMenu);

    app->tick = furi_timer_alloc(tr_tick_cb, FuriTimerTypePeriodic, app);
    furi_timer_start(app->tick, 2000);

    view_dispatcher_run(app->view_dispatcher);

    furi_timer_stop(app->tick);
    furi_timer_free(app->tick);
    tr_stop_live(app);
    tr_cli_stop(app);
    tr_transport_close(app);
    app->worker_stop = true;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);

    char* line;
    while(furi_message_queue_get(app->lines, &line, 0) == FuriStatusOk)
        free(line);

    view_dispatcher_remove_view(app->view_dispatcher, TrViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, TrViewList);
    view_dispatcher_remove_view(app->view_dispatcher, TrViewLog);
    view_dispatcher_remove_view(app->view_dispatcher, TrViewDetail);
    submenu_free(app->menu);
    submenu_free(app->list);
    text_box_free(app->log_box);
    widget_free(app->detail);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_EXPANSION);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(app->lines);
    furi_stream_buffer_free(app->rx_stream);
    furi_string_free(app->log);
    furi_string_free(app->header);
    free(app);
    return 0;
}
