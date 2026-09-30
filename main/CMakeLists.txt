idf_component_register(
    SRCS "main.c" "display.c" "net.c" "weather.c" "ui.c" "touch.c" "radar.c" "config.c" "web.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl dns_server esp_lcd esp_driver_spi esp_driver_gpio esp_driver_i2c esp_wifi esp_netif esp_event
             nvs_flash esp_http_client esp_http_server esp_https_server json mbedtls esp_timer esp_partition
    EMBED_FILES "montserrat.ttf"
    EMBED_TXTFILES "web/index.html" "certs/servercert.pem" "certs/prvtkey.pem")
