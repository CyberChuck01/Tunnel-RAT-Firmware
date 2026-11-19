// Device: Flipper Zero (Tunnel R.A.T. firmware) - Tunnel R.A.T. Link app
//
// Runs a full Flipper CLI session on the GPIO USART (pin 13 TX, pin 14 RX,
// 115200 8N1). An ESP32 running TunnelRAT_FlipperBridge.ino sits on those
// pins, joins the tunnel-rat AP and exposes the session as a TCP socket, so
// the Tunnel R.A.T. dashboard drives the Flipper over WiFi with the exact same
// CLI commands it uses over USB.
//
//   OK    start / stop the bridge
//   LEFT  / RIGHT  toggle 5V on GPIO pin 1 (for boards powered from 5V)
//   BACK  stop and exit

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <expansion/expansion.h>
#include <notification/notification_messages.h>
#include <toolbox/pipe.h>
#include <toolbox/cli/cli_registry.h>
#include <toolbox/cli/shell/cli_shell.h>

#define TAG          "TunnelRatLink"
#define LINK_BAUD    115200
#define RECORD_CLI_  "cli"
#define RX_BUF_SIZE  2048
#define PIPE_SIZE    1024
#define WORKER_STACK 2048

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* input_queue;
    FuriMutex* mutex;
    NotificationApp* notifications;
    Expansion* expansion;

    bool running;
    bool otg;
    bool otg_was_on;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    char status[32];

    FuriHalSerialHandle* serial;
    FuriStreamBuffer* rx_stream;
    FuriThread* worker;
    volatile bool stop;

    PipeSide* own_pipe;
    PipeSide* shell_pipe;
    CliShell* shell;
    CliRegistry* registry;
} TunnelRatLink;

static const CliCommandExternalConfig link_ext_config = {
    .search_directory = "/ext/apps_data/cli/plugins",
    .fal_prefix = "cli_",
    .appid = "cli",
};

static void link_motd(void* context) {
    UNUSED(context);
    printf("\r\n"
           "  _____                        _   ___    _  _____ \r\n"
           " |_   _|  _ _ _  _ _  ___ | |  | _ \\  /_\\|_   _|\r\n"
           "   | || || | ' \\| ' \\/ -_)| |  |   / / _ \\ | |  \r\n"
           "   |_| \\_,_|_||_|_||_\\___||_|  |_|_\\/_/ \\_\\|_|  \r\n"
           "\r\n"
           " Tunnel R.A.T. Link - Flipper CLI over WiFi bridge\r\n"
           " Type `help` for commands.\r\n"
           "TR-LINK-READY\r\n");
}

// ---------------------------------------------------------------- serial I/O

static void link_rx_callback(FuriHalSerialHandle* handle, FuriHalSerialRxEvent event, void* ctx) {
    TunnelRatLink* app = ctx;
    if(event & FuriHalSerialRxEventData) {
        uint8_t byte = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &byte, 1, 0);
    }
}

static int32_t link_worker(void* ctx) {
    TunnelRatLink* app = ctx;
    uint8_t buf[64];

    while(!app->stop) {
        // UART -> shell
        size_t n = furi_stream_buffer_receive(app->rx_stream, buf, sizeof(buf), 5);
        if(n) {
            pipe_send(app->own_pipe, buf, n);
            app->rx_bytes += n;
        }
        // shell -> UART
        size_t avail = pipe_bytes_available(app->own_pipe);
        while(avail && !app->stop) {
            size_t chunk = MIN(avail, sizeof(buf));
            chunk = pipe_receive(app->own_pipe, buf, chunk);
            if(!chunk) break;
            furi_hal_serial_tx(app->serial, buf, chunk);
            app->tx_bytes += chunk;
            avail = pipe_bytes_available(app->own_pipe);
        }
        if(pipe_state(app->own_pipe) == PipeStateBroken) {
            strlcpy(app->status, "Shell closed", sizeof(app->status));
            break;
        }
    }
    return 0;
}

static bool link_start(TunnelRatLink* app) {
    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(!app->serial) {
        strlcpy(app->status, "UART busy!", sizeof(app->status));
        return false;
    }

    expansion_disable(app->expansion);
    furi_hal_serial_init(app->serial, LINK_BAUD);
    app->rx_stream = furi_stream_buffer_alloc(RX_BUF_SIZE, 1);
    furi_hal_serial_async_rx_start(app->serial, link_rx_callback, app, false);

    PipeSideBundle bundle = pipe_alloc(PIPE_SIZE, 1);
    app->own_pipe = bundle.alices_side;
    app->shell_pipe = bundle.bobs_side;

    app->registry = furi_record_open(RECORD_CLI_);
    app->shell = cli_shell_alloc(link_motd, app, app->shell_pipe, app->registry, &link_ext_config);
    cli_shell_start(app->shell);

    app->stop = false;
    app->rx_bytes = 0;
    app->tx_bytes = 0;
    app->worker = furi_thread_alloc_ex("TRLinkWorker", WORKER_STACK, link_worker, app);
    furi_thread_start(app->worker);

    app->running = true;
    strlcpy(app->status, "Bridge running", sizeof(app->status));
    notification_message(app->notifications, &sequence_blink_green_100);
    return true;
}

static void link_stop(TunnelRatLink* app) {
    if(!app->running) return;

    app->stop = true;
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    app->worker = NULL;

    // Same teardown order as the USB CLI: drop our end, then the shell exits
    pipe_free(app->own_pipe);
    app->own_pipe = NULL;
    cli_shell_join(app->shell);
    cli_shell_free(app->shell);
    app->shell = NULL;
    pipe_free(app->shell_pipe);
    app->shell_pipe = NULL;
    furi_record_close(RECORD_CLI_);

    furi_hal_serial_async_rx_stop(app->serial);
    furi_hal_serial_deinit(app->serial);
    furi_hal_serial_control_release(app->serial);
    app->serial = NULL;
    furi_stream_buffer_free(app->rx_stream);
    app->rx_stream = NULL;

    expansion_enable(app->expansion);
    app->running = false;
    strlcpy(app->status, "Stopped", sizeof(app->status));
}

static void link_set_otg(TunnelRatLink* app, bool on) {
    if(on && !furi_hal_power_is_otg_enabled()) {
        furi_hal_power_enable_otg();
    } else if(!on && furi_hal_power_is_otg_enabled()) {
        furi_hal_power_disable_otg();
    }
    app->otg = furi_hal_power_is_otg_enabled();
}

// ---------------------------------------------------------------------- GUI

static void link_draw(Canvas* canvas, void* ctx) {
    TunnelRatLink* app = ctx;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Tunnel R.A.T. Link");
    canvas_draw_line(canvas, 0, 13, 127, 13);

    canvas_set_font(canvas, FontSecondary);
    char line[48];
    snprintf(line, sizeof(line), "WiFi bridge: %s", app->running ? "ON" : "OFF");
    canvas_draw_str(canvas, 2, 24, line);
    canvas_draw_str(canvas, 2, 33, app->status);
    snprintf(
        line,
        sizeof(line),
        "RX %lu  TX %lu",
        (unsigned long)app->rx_bytes,
        (unsigned long)app->tx_bytes);
    canvas_draw_str(canvas, 2, 42, line);
    snprintf(line, sizeof(line), "5V pin1: %s  UART 13/14", app->otg ? "ON" : "off");
    canvas_draw_str(canvas, 2, 51, line);

    elements_button_center(canvas, app->running ? "Stop" : "Start");
    elements_button_right(canvas, "5V");

    furi_mutex_release(app->mutex);
}

static void link_input(InputEvent* event, void* ctx) {
    TunnelRatLink* app = ctx;
    furi_message_queue_put(app->input_queue, event, FuriWaitForever);
}

int32_t tunnel_rat_link_app(void* p) {
    UNUSED(p);
    TunnelRatLink* app = malloc(sizeof(TunnelRatLink));
    memset(app, 0, sizeof(TunnelRatLink));

    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    app->expansion = furi_record_open(RECORD_EXPANSION);
    app->otg_was_on = furi_hal_power_is_otg_enabled();
    app->otg = app->otg_was_on;
    strlcpy(app->status, "Press OK to start", sizeof(app->status));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, link_draw, app);
    view_port_input_callback_set(app->view_port, link_input, app);
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    InputEvent event;
    bool exit = false;
    while(!exit) {
        if(furi_message_queue_get(app->input_queue, &event, 250) == FuriStatusOk) {
            if(event.type != InputTypeShort) continue;
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            switch(event.key) {
            case InputKeyOk:
                if(app->running) {
                    link_stop(app);
                } else {
                    link_start(app);
                }
                break;
            case InputKeyLeft:
            case InputKeyRight:
                link_set_otg(app, !app->otg);
                break;
            case InputKeyBack:
                exit = true;
                break;
            default:
                break;
            }
            furi_mutex_release(app->mutex);
        } else if(
            app->running && app->worker &&
            furi_thread_get_state(app->worker) == FuriThreadStateStopped) {
            // Shell ended on its own (e.g. `reboot` typed remotely): clean up
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            link_stop(app);
            strlcpy(app->status, "Shell ended - OK restarts", sizeof(app->status));
            furi_mutex_release(app->mutex);
        }
        view_port_update(app->view_port);
    }

    link_stop(app);
    if(!app->otg_was_on) link_set_otg(app, false);

    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_EXPANSION);
    furi_record_close(RECORD_NOTIFICATION);
    furi_message_queue_free(app->input_queue);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
