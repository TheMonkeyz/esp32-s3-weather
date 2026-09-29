// CST9217 capacitive touch (I2C 0x5A) -> LVGL pointer input
#include "touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "rom/ets_sys.h"

static const char *TAG = "touch";

#define PIN_SDA   15
#define PIN_SCL   14
#define PIN_RST   40
#define ADDR      0x5A
#define RES       466

static i2c_master_dev_handle_t dev;
static bool ok;
static int n_ok, n_err;

void touch_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0, .sda_io_num = PIN_SDA, .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
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
static int touch_read(int *x, int *y)
{
    if (!ok) return 0;
    uint8_t reg[2] = {0xD0, 0x00};
    uint8_t d[10] = {0};
    if (i2c_master_transmit_receive(dev, reg, 2, d, sizeof(d), 20) != ESP_OK) { n_err++; return -1; }
    uint8_t ack[3] = {0xD0, 0x00, 0xAB};      // tell the controller we consumed the report
    i2c_master_transmit(dev, ack, 3, 20);
    n_ok++;
    if (d[6] != 0xAB) return -1;
    if ((d[5] & 0x7F) == 0 || (d[0] & 0x0F) != 0x06) return 0;
    int rx = (d[1] << 4) | (d[3] >> 4);
    int ry = (d[2] << 4) | (d[3] & 0x0F);
    // Panel is mounted rotated 180° relative to the touch sensor (mirror X and Y)
    *x = RES - 1 - rx;
    *y = RES - 1 - ry;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    return 1;
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static int lx, ly;
    static bool was;
    static int errs;
    static uint32_t last_log;
    uint32_t now = lv_tick_get();
    if (now - last_log > 15000) {
        ESP_LOGI(TAG, "reads ok=%d err=%d", n_ok, n_err);
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

void touch_register_lvgl(void)
{
    lv_indev_t *in = lv_indev_create();
    lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(in, read_cb);
}
