idf_component_register(SRCS "main.c"
                       "image_framebuffer.c"
                   PRIV_REQUIRES spi_flash mbedtls esp_driver_uart esp_http_server
                       INCLUDE_DIRS "")
