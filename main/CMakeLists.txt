idf_component_register(SRCS "main.c"
                       "image_framebuffer.c"
                       "app_api.c"
                   PRIV_REQUIRES spi_flash mbedtls esp_driver_uart esp_http_server esp_http_client esp-tls nvs_flash esp_wifi esp_event esp_netif
                       INCLUDE_DIRS "")
