// Test console on the USB serial port: lets tools/harness drive the display without a person.
// One command per line; every answer is a log line starting with "test: " (see docs/TESTING.md, "Harness").
// USB only (never on the network): whoever can type here can already reflash the board, so it is in every build
// and the harness tests exactly what ships.
#include "testcon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "net.h"
#include "presence.h"
#include "diag.h"
#include "web.h"

static const char *TAG = "test";

#define CX 233
#define CY 233

// Finger moves in steps of ~16 ms (LVGL reads the touch every 30 ms, so every read sees a new point)
static void finger_path(int x0, int y0, int x1, int y1, int ms)
{
    int steps = ms / 16 < 2 ? 2 : ms / 16;
    for (int i = 0; i <= steps; i++) {
        touch_inject(true, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
        vTaskDelay(pdMS_TO_TICKS(16));
    }
}

static void finger_up(int x, int y)
{
    touch_inject(false, x, y);
    vTaskDelay(pdMS_TO_TICKS(100));          // LVGL must read the release before the real controller takes over
    touch_inject_end();
}

static void press(int x, int y, int ms)
{
    touch_inject(true, x, y);
    vTaskDelay(pdMS_TO_TICKS(ms));
    finger_up(x, y);
}

static void swipe(const char *dir)
{
    int d = 150, x0 = CX, y0 = CY, x1 = CX, y1 = CY;
    if (!strcmp(dir, "left"))       { x0 = CX + d; x1 = CX - d; }
    else if (!strcmp(dir, "right")) { x0 = CX - d; x1 = CX + d; }
    else if (!strcmp(dir, "up"))    { y0 = CY + d; y1 = CY - d; }
    else if (!strcmp(dir, "down"))  { y0 = CY - d; y1 = CY + d; }
    else { ESP_LOGW(TAG, "error swipe: left, right, up or down"); return; }
    finger_path(x0, y0, x1, y1, 200);
    finger_up(x1, y1);
}

static void cmd_heap(void)
{
    ESP_LOGI(TAG, "heap internal=%u min=%u largest=%u psram=%u psram_min=%u uptime_s=%lld",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024,
             esp_timer_get_time() / 1000000);
}

static void run(char *line)
{
    char *argv[8];
    int argc = 0;
    for (char *t = strtok(line, " \t"); t && argc < 8; t = strtok(NULL, " \t")) argv[argc++] = t;
    if (!argc) return;
    const char *c = argv[0];
    if (!strcmp(c, "ping")) {
        ESP_LOGI(TAG, "pong %s", esp_app_get_description()->version);
    } else if (!strcmp(c, "screen")) {
        display_lock(-1);
        const char *n = ui_screen_name();
        display_unlock();
        ESP_LOGI(TAG, "screen %s", n);
    } else if (!strcmp(c, "tap") && argc == 3) {
        press(atoi(argv[1]), atoi(argv[2]), 60);
        ESP_LOGI(TAG, "ok tap");
    } else if (!strcmp(c, "press") && argc >= 3) {           // long-press: press X Y [ms, default 1200]
        press(atoi(argv[1]), atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 1200);
        ESP_LOGI(TAG, "ok press");
    } else if (!strcmp(c, "swipe") && argc == 2) {
        swipe(argv[1]);
        ESP_LOGI(TAG, "ok swipe %s", argv[1]);
    } else if (!strcmp(c, "drag") && argc >= 5) {            // drag X1 Y1 X2 Y2 [ms]
        int x1 = atoi(argv[3]), y1 = atoi(argv[4]);
        finger_path(atoi(argv[1]), atoi(argv[2]), x1, y1, argc > 5 ? atoi(argv[5]) : 400);
        finger_up(x1, y1);
        ESP_LOGI(TAG, "ok drag");
    } else if (!strcmp(c, "wake")) {
        presence_wake();
        ESP_LOGI(TAG, "ok wake");
    } else if (!strcmp(c, "presence")) {
        presence_status_t st;
        presence_get_status(&st);
        ESP_LOGI(TAG, "presence state=%d brightness=%d quiet_s=%.0f mic=%d imu=%d", st.state, st.brightness,
                 st.quiet_s, st.mic_ok, st.imu_ok);
    } else if (!strcmp(c, "wifi") && argc == 2) {
        char info[200];
        if (!strcmp(argv[1], "offline")) net_test_offline();
        else if (!strcmp(argv[1], "online")) net_test_online();
        else if (!strcmp(argv[1], "offline-boot")) {
            net_test_offline_next_boot();
            ESP_LOGI(TAG, "ok restarting; the next boot can't reach the saved network");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else if (strcmp(argv[1], "status")) { ESP_LOGW(TAG, "error wifi: status, offline, offline-boot, online"); return; }
        net_test_info(info, sizeof(info));
        ESP_LOGI(TAG, "wifi %s", info);
    } else if (!strcmp(c, "portal") && argc == 2 && !strcmp(argv[1], "windows-quiet")) {
        web_test_windows_quiet(true);                        // until restart
        ESP_LOGI(TAG, "ok portal windows-quiet");
    } else if (!strcmp(c, "heap")) {
        cmd_heap();
    } else if (!strcmp(c, "bench")) {
        diag_bench_request();                                // renders on its own 10 KB stack
        ESP_LOGI(TAG, "ok bench started (result: \"diag: bench\" lines)");
    } else if (!strcmp(c, "reboot")) {
        ESP_LOGI(TAG, "ok restarting");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (!strcmp(c, "help")) {
        ESP_LOGI(TAG, "commands: ping, screen, tap X Y, press X Y [ms], swipe left|right|up|down, "
                      "drag X1 Y1 X2 Y2 [ms], wake, presence, wifi status|offline|offline-boot|online, portal windows-quiet, "
                      "heap, bench, reboot");
    } else {
        ESP_LOGW(TAG, "error unknown command '%s' (help lists them)", c);
        return;
    }
}

static void testcon_task(void *arg)
{
    char line[96];
    int n = 0;
    uint8_t ch;
    while (1) {
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) continue;
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[n] = 0;
            n = 0;
            run(line);
        } else if (n < (int)sizeof(line) - 1) {
            line[n++] = ch;
        }
    }
}

void testcon_start(void)
{
    usb_serial_jtag_driver_config_t cfg = { .rx_buffer_size = 256, .tx_buffer_size = 256 };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) { ESP_LOGW(TAG, "USB serial driver not installed"); return; }
    // Stack in internal RAM: commands read and write flash (saved network, Wi-Fi config), and a task with a PSRAM
    // stack can't run while flash is busy (tried: "wifi online" reset the board). 3 KB: check "testcon" in diag tasks.
    xTaskCreatePinnedToCore(testcon_task, "testcon", 3072, NULL, 3, NULL, 0);
    ESP_LOGI(TAG, "console ready on USB (send 'help')");
}
