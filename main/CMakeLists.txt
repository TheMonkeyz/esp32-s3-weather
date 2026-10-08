idf_component_register(
    SRCS "main.c" "display.c" "weather.c" "ui.c" "touch.c" "radar.c" "config.c" "audio.c" "lvgl_mem.c" "alerts.c" "pager.c" "imu.c" "i18n.c" "sound.c" "slide.c" "routes.c" "console.c" "services.c"
    INCLUDE_DIRS "."
    # forge_core, forge_net, forge_ota, forge_presence: espforge's components at the tag in idf_component.yml (Wi-Fi, web server,
    # updates, diagnostics, test console, languages, textfit, png_rows, screen dimming by presence...)
    REQUIRES lvgl forge_core forge_net forge_ota forge_presence esp_codec_dev esp_driver_i2s esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_wifi esp_netif esp_event
             nvs_flash esp_http_client esp_http_server json esp_timer esp_partition esp_psram spi_flash esp_app_format app_update
    EMBED_FILES "montserrat.ttf" "syllabics.ttf"
    EMBED_TXTFILES "web/index.html")

# lvgl_mem.c provides LVGL's allocator (CONFIG_LV_USE_CUSTOM_MALLOC). Nothing in main references it,
# so force the linker to keep it (liblvgl is scanned before libmain resolves these).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_mem_init" "-u lv_malloc_core" "-u lv_realloc_core" "-u lv_free_core")
