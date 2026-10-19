# Local copy of espressif/es8311 v1.0.0 (Apache-2.0) with fixed requirements.
# Upstream only declares: REQUIRES "driver".
# On ESP-IDF >= 6.0 the legacy header driver/gpio.h moved to esp_driver_gpio,
# so the upstream component fails to compile with "driver/gpio.h: No such file".
idf_component_register(SRCS "es8311.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "priv_include"
                       REQUIRES driver esp_driver_gpio)
