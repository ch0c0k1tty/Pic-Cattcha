idf_component_register(SRCS "main.c" "ble.c" "film_filter.c" "selftest.c" "focus.c" "focus_mode.c" INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_driver_rmt esp_timer fatfs wear_levelling nvs_flash vfs bt esp_wifi esp_netif esp_event esp_http_server)
