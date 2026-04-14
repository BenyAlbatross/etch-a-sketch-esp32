idf_component_register(SRCS "main.c"
                   PRIV_REQUIRES etch_sketch_core spi_flash esp_driver_uart esp_http_server esp_http_client esp-tls nvs_flash esp_wifi esp_event esp_netif
                       INCLUDE_DIRS "")
