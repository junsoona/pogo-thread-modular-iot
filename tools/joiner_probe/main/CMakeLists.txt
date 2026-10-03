idf_component_register(SRCS "main.c" INCLUDE_DIRS "."
                       REQUIRES openthread esp_event esp_netif nvs_flash vfs console)
