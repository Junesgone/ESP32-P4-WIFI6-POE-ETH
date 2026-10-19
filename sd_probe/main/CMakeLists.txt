idf_component_register(SRCS "sd_probe_main.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES sdmmc esp_driver_gpio esp_driver_sdmmc)
