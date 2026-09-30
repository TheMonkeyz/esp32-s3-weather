idf_component_register(
    SRCS "main.c" "display.c" "net.c" "weather.c" "ui.c" "touch.c" "radar.c" "config.c" "web.c" "presence.c" "diag.c" "lvgl_mem.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl dns_server esp_codec_dev esp_driver_i2s esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_wifi esp_netif esp_event
             nvs_flash esp_http_client esp_http_server esp_https_server json mbedtls esp_timer esp_partition esp_psram spi_flash
    EMBED_FILES "montserrat.ttf"
    EMBED_TXTFILES "web/index.html" "certs/servercert.pem" "certs/prvtkey.pem")

# lvgl_mem.c provides LVGL's allocator (CONFIG_LV_USE_CUSTOM_MALLOC). Nothing in main references it,
# so force the linker to keep it (liblvgl is scanned before libmain resolves these).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init" "-u lv_malloc_core" "-u lv_realloc_core" "-u lv_free_core")
