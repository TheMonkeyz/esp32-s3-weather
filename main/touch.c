// CST9217 capacitive touch (I2C 0x5A) -> LVGL pointer input
#include "touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"
#include "presence.h"

static const char *TAG = "touch";

#define PIN_SDA   15
#define PIN_SCL   14
#define PIN_RST   40
#define ADDR      0x5A
#define RES       466

static i2c_master_dev_handle_t dev;
static i2c_master_bus_handle_t bus;
static bool ok;
static int n_ok, n_err;
// Simulated finger from the test console (testcon.c): replaces the controller's report while active, so wake-up,
// long-press and gestures go through exactly the same code as a real touch.
static volatile bool inj_on, inj_down;
static volatile int inj_x, inj_y;

void touch_inject(bool down, int x, int y)
{
    inj_x = x < 0 ? 0 : x >= RES ? RES - 1 : x;
    inj_y = y < 0 ? 0 : y >= RES ? RES - 1 : y;
    inj_down = down;
    inj_on = true;
}

void touch_inject_end(void) { inj_down = false; inj_on = false; }

void touch_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) { ESP_LOGE(TAG, "I2C bus init failed"); return; }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = ADDR, .scl_speed_hz = 400000 };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &dev));

    gpio_config_t rc = { .pin_bit_mask = 1ULL << PIN_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rc);
    gpio_set_level(PIN_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_RST, 1); vTaskDelay(pdMS_TO_TICKS(60));

    ok = i2c_master_probe(bus, ADDR, 50) == ESP_OK;
    esp_log_level_set("i2c.master", ESP_LOG_NONE);   // controller NACKs while idle; that's normal
    ESP_LOGI(TAG, "CST9217 %s", ok ? "found" : "NOT found");
}

// 1 = pressed, 0 = not pressed, -1 = bus error (keep previous state)
static volatile int64_t last_down_us;   // touch_idle_ms(): when a finger was last seen down

static int touch_read_chip(int *x, int *y);

static int touch_read(int *x, int *y)
{
    int r = touch_read_chip(x, y);
    if (r > 0) last_down_us = esp_timer_get_time();
    return r;
}

uint32_t touch_idle_ms(void) { return (uint32_t)((esp_timer_get_time() - last_down_us) / 1000); }

static int n_nofinger, n_status;        // "up" answers: no finger in the report / a status other than contact

static int touch_read_chip(int *x, int *y)
{
    if (inj_on) { *x = inj_x; *y = inj_y; return inj_down; }
    if (!ok) return 0;
    uint8_t reg[2] = {0xD0, 0x00};
    uint8_t d[10] = {0};
    if (i2c_master_transmit_receive(dev, reg, 2, d, sizeof(d), 20) != ESP_OK) { n_err++; return -1; }
    uint8_t ack[3] = {0xD0, 0x00, 0xAB};      // tell the controller we consumed the report
    i2c_master_transmit(dev, ack, 3, 20);
    n_ok++;
    if (d[6] != 0xAB) return -1;
    if ((d[5] & 0x7F) == 0) { n_nofinger++; return 0; }
    if ((d[0] & 0x0F) != 0x06) { n_status++; return 0; }
    int rx = (d[1] << 4) | (d[3] >> 4);
    int ry = (d[2] << 4) | (d[3] & 0x0F);
    // Panel is mounted rotated 180° relative to the touch sensor (mirror X and Y)
    *x = RES - 1 - rx;
    *y = RES - 1 - ry;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    return 1;
}

static volatile bool forget;            // touch_forget(): the touch LVGL last saw was handled elsewhere

// A drag drawn outside LVGL (slide.c) consumed the touch. Without this, a bus error right after it made read_cb
// "hold" the last point LVGL had seen (the NACK guard below): a fake press there, then a release, so a tap where the
// drag had started (it opened the hourly view after place drags).
static volatile uint32_t forgotten;
void touch_forget(void) { forget = true; forgotten++; }
uint32_t touch_forgotten(void) { return forgotten; }

static void read_core(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int lx, ly;
    static bool was;
    static int errs;
    static uint32_t last_log;
    if (forget) { forget = false; was = false; errs = 0; }
    uint32_t now = lv_tick_get();
    if (now - last_log > 15000) {
        ESP_LOGI(TAG, "reads ok=%d err=%d, up answers: no finger %d, other status %d", n_ok, n_err, n_nofinger,
                 n_status);
        last_log = now;
    }
    int x, y;
    int r = touch_read(&x, &y);
    if (r < 0 && was && ++errs < 6) {       // transient NACK mid-swipe: hold last point
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = lx; data->point.y = ly;
        return;
    }
    errs = 0;
    static bool swallow;                    // the touch that wakes a dark screen does nothing else
    if (r > 0 && !was && presence_touch()) swallow = true;
    if (swallow) {
        if (r == 0) swallow = false;
        was = r > 0;
        data->state = LV_INDEV_STATE_RELEASED;
        data->point.x = lx; data->point.y = ly;
        return;
    }
    if (r > 0) {
        lx = x; ly = y;
        data->state = LV_INDEV_STATE_PRESSED;
        if (!was) ESP_LOGI(TAG, "down %d,%d", x, y);
        was = true;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
        if (was) ESP_LOGI(TAG, "up %d,%d", lx, ly);
        was = false;
    }
    data->point.x = lx;
    data->point.y = ly;
}

// The finger now, read directly (for drags drawn outside LVGL, slide.c): 1 = pressed at x,y, 0 = up, -1 = bus error
// (keep the last point). LVGL isn't reading meanwhile (display lock held by the caller).
// The loops that follow a finger (slide.c) read the chip directly. Read at most every 10 ms, as LVGL does every
// 15 ms: polled every millisecond, the chip answered "up" for long stretches while the finger moved on it (each read is
// acknowledged, and a new report takes it a while), and the hourly list stopped under the finger.
static bool fresh;
bool touch_fresh(void) { return fresh; }

int touch_get(int *x, int *y)
{
    static int64_t last;
    static int lr, lx, ly;
    int64_t now = esp_timer_get_time();
    fresh = inj_on || !last || now - last >= 10000;
    if (!fresh) { *x = lx; *y = ly; return lr; }
    last = now;
    lr = touch_read(x, y);
    lx = *x;
    ly = *y;
    return lr;
}

static touch_read_hook_t read_hook;

void touch_set_read_hook(touch_read_hook_t hook) { read_hook = hook; }

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    read_core(indev, data);
    if (read_hook) read_hook(indev, data);               // before LVGL handles this read
}

void touch_register_lvgl(void)
{
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, read_cb);
}

i2c_master_bus_handle_t touch_i2c_bus(void) { return bus; }
