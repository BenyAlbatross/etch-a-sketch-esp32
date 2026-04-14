idf_component_register(SRCS "app_api.c"
                            "image_framebuffer.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_common
                       PRIV_REQUIRES freertos heap log mbedtls esp_http_client esp-tls)
